/**
 * @file session_control_host.c
 * @brief Sole-thread control-only host admission, mapping validation, and socket pump.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session_control_host.h"

#include "interop_native_atomic.h"
#include "waddle/interop_linux_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define HostControlMaxPollMs 100u
#define HostControlHeartbeatMs 500u
#define HostControlFrameBytes InteropLinuxSocketFrameCapacity
#define HostControlRxBudget InteropLinuxSocketMaxRxBudgetBytes
#define HostControlTxBudget InteropLinuxSocketMaxTxBudgetBytes
#define HostControlSyscallBudget InteropLinuxSocketMaxSyscallBudget

static uint64_t host_control_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return UINT64_MAX;
    uint64_t seconds = (uint64_t)now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)now.tv_nsec / UINT64_C(1000000)) /
            UINT64_C(1000)) return UINT64_MAX;
    return seconds * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static bool host_control_config_valid(const interop_host_control_config_t *config,
                                      const interop_host_control_owners_t *owners)
{
    return config != NULL && owners != NULL && config->admission != NULL &&
        config->mapping_fd >= 0 &&
        (config->backing_kind == InteropMappingLinuxBackingRegularPrivateFile ||
         config->backing_kind == InteropMappingLinuxBackingKvmfrCharacter) &&
        config->stop_fd >= 0 && config->run_for_ms >= 1 &&
        config->run_for_ms <= 120000 && config->reserved_zero == 0 &&
        config->admission->local_capabilities == 0 &&
        config->admission->cancel_fd == config->stop_fd &&
        config->stop_fd != config->mapping_fd &&
        config->stop_fd != config->admission->daemon_fd &&
        owners->session_owner == NULL && owners->mapping_owner == NULL;
}

static interop_host_control_result_t host_control_cleanup_owners(
    interop_host_control_owners_t *owners)
{
    bool cleanup_required = false;
    if (owners->mapping_owner != NULL) {
        interop_mapping_linux_status_t closed =
            interop_mapping_linux_close(owners->mapping_owner, 1);
        if (closed == InteropMappingLinuxOk ||
            closed == InteropMappingLinuxCloseDiagnostic) {
            interop_mapping_linux_status_t destroyed =
                interop_mapping_linux_destroy(&owners->mapping_owner);
            if (destroyed != InteropMappingLinuxOk) cleanup_required = true;
        } else {
            cleanup_required = true;
        }
    }
    /* Keep the device leases while any local mapping resource remains owned. */
    if (owners->session_owner != NULL && owners->mapping_owner == NULL) {
        interop_linux_session_result_t destroyed =
            interop_linux_session_destroy_peer(owners->session_owner);
        if (destroyed == InteropLinuxSessionSuccess ||
            destroyed == InteropLinuxSessionCloseDiagnostic) {
            interop_linux_session_result_t released =
                interop_linux_session_release(&owners->session_owner);
            if (released != InteropLinuxSessionSuccess &&
                released != InteropLinuxSessionCloseDiagnostic)
                cleanup_required = true;
        } else {
            cleanup_required = true;
        }
    }
    return cleanup_required ? HostControlCleanupRequired : HostControlSuccess;
}

interop_host_control_result_t interop_host_control_retry_cleanup(
    interop_host_control_owners_t *owners)
{
    if (owners == NULL) return HostControlInvalidArgument;
    return host_control_cleanup_owners(owners);
}

static interop_host_control_result_t host_control_report_mapping(
    interop_mapping_linux_t *mapping_owner,
    interop_linux_socket_t *socket_owner,
    uint64_t token,
    const interop_peer_event_t *request,
    uint64_t now_ms,
    interop_mapping_linux_view_t *view,
    interop_shm_header_snapshot_t *snapshot,
    bool *view_published)
{
    interop_mapping_linux_status_t validated =
        interop_mapping_linux_validate_header(mapping_owner,
            request->session_epoch, snapshot);
    if (validated != InteropMappingLinuxOk) {
        interop_peer_event_t failed_event;
        int failed = interop_linux_socket_mapping_result(socket_owner, token,
            now_ms, request->session_epoch, request->layout_version, 0,
            &failed_event);
        return failed == InteropLinuxSocketSuccess
            ? HostControlMappingFailed : HostControlPeerFailed;
    }
    if (request->layout_version != InteropLayoutVersion ||
        interop_mapping_linux_view(mapping_owner, view) != InteropMappingLinuxOk) {
        interop_peer_event_t failed_event;
        int failed = interop_linux_socket_mapping_result(socket_owner, token,
            now_ms, request->session_epoch, request->layout_version, 0,
            &failed_event);
        return failed == InteropLinuxSocketSuccess
            ? HostControlMappingFailed : HostControlPeerFailed;
    }
    *view_published = true;
    interop_peer_event_t mapped_event;
    int reported = interop_linux_socket_mapping_result(socket_owner, token,
        now_ms, request->session_epoch, InteropLayoutVersion, 1,
        &mapped_event);
    return reported == InteropLinuxSocketSuccess
        ? HostControlSuccess : HostControlPeerFailed;
}

static interop_host_control_result_t host_control_heartbeat(
    const interop_mapping_linux_view_t *view,
    uint64_t *last_heartbeat,
    uint64_t *next_heartbeat_ms,
    uint64_t now_ms)
{
    if (now_ms < *next_heartbeat_ms) return HostControlSuccess;
    interop_shm_header_t *header = (interop_shm_header_t *)view->mapping_base;
    uint64_t observed = 0;
    if (interop_heartbeat_load(header, view->mapping_base, view->mapping_length,
            InteropHostHeartbeat, &observed) != InteropAtomicStatusOk)
        return HostControlMappingFailed;
    if (observed != *last_heartbeat || observed == UINT64_MAX)
        return HostControlMappingFailed;
    if (interop_heartbeat_store_next(header, view->mapping_base,
            view->mapping_length, InteropHostHeartbeat, observed + 1) !=
        InteropAtomicStatusOk) return HostControlMappingFailed;
    *last_heartbeat = observed + 1;
    if (now_ms > UINT64_MAX - HostControlHeartbeatMs)
        *next_heartbeat_ms = UINT64_MAX;
    else
        *next_heartbeat_ms = now_ms + HostControlHeartbeatMs;
    return HostControlSuccess;
}

static interop_host_control_result_t host_control_begin_drain(
    interop_linux_socket_t *socket_owner, uint64_t token, uint64_t now_ms)
{
    interop_peer_event_t event;
    return interop_linux_socket_begin_drain(socket_owner, token, now_ms, &event) ==
        InteropLinuxSocketSuccess ? HostControlSuccess : HostControlPeerFailed;
}

static void host_control_record_outcome(
    interop_host_control_result_t *outcome,
    interop_host_control_result_t next)
{
    if (*outcome == HostControlSuccess) *outcome = next;
}

interop_host_control_result_t interop_host_control_run(
    const interop_host_control_config_t *config,
    interop_host_control_owners_t *out_owners)
{
    interop_host_control_result_t outcome = HostControlSuccess;
    interop_mapping_linux_view_t view;
    interop_shm_header_snapshot_t snapshot;
    interop_linux_socket_poll_t poll_request;
    interop_linux_socket_work_t work;
    interop_linux_socket_t *socket_owner;
    uint8_t frame[HostControlFrameBytes];
    uint64_t token;
    uint64_t run_deadline;
    uint64_t now_ms;
    uint64_t last_heartbeat = 0;
    uint64_t next_heartbeat_ms = UINT64_MAX;
    bool view_published = false;
    bool ready = false;
    bool draining = false;

    memset(&view, 0, sizeof(view));
    memset(&snapshot, 0, sizeof(snapshot));
    if (config == NULL || out_owners == NULL ||
        (uintptr_t)config % _Alignof(interop_host_control_config_t) != 0 ||
        (uintptr_t)out_owners % _Alignof(interop_host_control_owners_t) != 0 ||
        (uintptr_t)config > UINTPTR_MAX - sizeof(*config) ||
        (uintptr_t)out_owners > UINTPTR_MAX - sizeof(*out_owners) ||
        ((uintptr_t)config < (uintptr_t)out_owners + sizeof(*out_owners) &&
         (uintptr_t)out_owners < (uintptr_t)config + sizeof(*config)))
        return HostControlInvalidArgument;
    if (config->admission == NULL) return HostControlInvalidArgument;
    uintptr_t admission_start = (uintptr_t)config->admission;
    if (admission_start % _Alignof(interop_host_session_config_t) != 0 ||
        admission_start > UINTPTR_MAX - sizeof(*config->admission) ||
        (admission_start < (uintptr_t)config + sizeof(*config) &&
         (uintptr_t)config < admission_start + sizeof(*config->admission)) ||
        (admission_start < (uintptr_t)out_owners + sizeof(*out_owners) &&
         (uintptr_t)out_owners < admission_start + sizeof(*config->admission)))
        return HostControlInvalidArgument;
    if (!host_control_config_valid(config, out_owners))
        return HostControlInvalidArgument;
    uintptr_t device_start = (uintptr_t)config->admission->device;
    uintptr_t version_start = (uintptr_t)config->admission->agent_version;
    if (device_start == 0 || version_start == 0 ||
        device_start % _Alignof(device_info_t) != 0 ||
        version_start > UINTPTR_MAX - config->admission->agent_version_bytes ||
        device_start > UINTPTR_MAX - sizeof(device_info_t) ||
        (device_start < (uintptr_t)config + sizeof(*config) &&
         (uintptr_t)config < device_start + sizeof(device_info_t)) ||
        (device_start < (uintptr_t)out_owners + sizeof(*out_owners) &&
         (uintptr_t)out_owners < device_start + sizeof(device_info_t)) ||
        (version_start < (uintptr_t)config + sizeof(*config) &&
         (uintptr_t)config < version_start + config->admission->agent_version_bytes) ||
        (version_start < (uintptr_t)out_owners + sizeof(*out_owners) &&
         (uintptr_t)out_owners < version_start + config->admission->agent_version_bytes) ||
        (device_start < admission_start + sizeof(*config->admission) &&
         admission_start < device_start + sizeof(device_info_t)) ||
        (version_start < admission_start + sizeof(*config->admission) &&
         admission_start < version_start + config->admission->agent_version_bytes) ||
        (device_start < version_start + config->admission->agent_version_bytes &&
         version_start < device_start + sizeof(device_info_t)))
        return HostControlInvalidArgument;
    int stop_status = fcntl(config->stop_fd, F_GETFL);
    int stop_descriptor_flags = fcntl(config->stop_fd, F_GETFD);
    if (stop_status < 0 || stop_descriptor_flags < 0 ||
        (stop_status & O_NONBLOCK) == 0 ||
        (stop_descriptor_flags & FD_CLOEXEC) == 0)
        return HostControlInvalidArgument;
    token = config->admission->connection_token;
    if (token == 0) return HostControlInvalidArgument;

    interop_host_session_config_t admission = *config->admission;
    admission.local_capabilities = 0;
    interop_host_session_result_t admitted = interop_host_session_admit(
        &admission, &out_owners->session_owner,
        &(uint32_t){0});
    if (admitted != HostSessionSuccess) {
        outcome = HostControlAdmissionFailed;
        goto cleanup;
    }
    now_ms = host_control_monotonic_ms();
    if (now_ms == UINT64_MAX || now_ms > UINT64_MAX - config->run_for_ms) {
        outcome = HostControlOsError;
        goto cleanup;
    }
    run_deadline = now_ms + config->run_for_ms;
    if (interop_mapping_linux_create(&out_owners->mapping_owner) !=
        InteropMappingLinuxOk) {
        outcome = HostControlMappingFailed;
        goto cleanup;
    }
    if (interop_mapping_linux_open(out_owners->mapping_owner, config->mapping_fd,
            config->backing_kind) != InteropMappingLinuxOk) {
        outcome = HostControlMappingFailed;
        goto cleanup;
    }
    socket_owner = interop_linux_session_socket_owner(out_owners->session_owner);
    if (socket_owner == NULL) {
        outcome = HostControlPeerFailed;
        goto cleanup;
    }
    while (true) {
        struct pollfd descriptors[3];
        nfds_t descriptor_count = 0;
        int socket_index = -1;
        int daemon_index = -1;
        int stop_index = -1;
        int timeout_ms;
        int poll_result;
        short socket_events = 0;
        bool snapshot_invalidated = false;
        uint64_t wait_deadline;
        now_ms = host_control_monotonic_ms();
        if (now_ms == UINT64_MAX) {
            host_control_record_outcome(&outcome, HostControlOsError);
            break;
        }
        if (now_ms >= run_deadline) {
            if (!ready) host_control_record_outcome(&outcome, HostControlTimedOut);
            if (!draining && host_control_begin_drain(socket_owner, token, now_ms) ==
                    HostControlSuccess) draining = true;
            else if (!draining) {
                host_control_record_outcome(&outcome, HostControlPeerFailed);
                break;
            }
        }
        if (outcome != HostControlSuccess && !draining) {
            if (host_control_begin_drain(socket_owner, token, now_ms) ==
                HostControlSuccess) draining = true;
            else break;
        }
        if (ready && !draining) {
            outcome = host_control_heartbeat(&view, &last_heartbeat,
                &next_heartbeat_ms, now_ms);
            if (outcome != HostControlSuccess) {
                if (host_control_begin_drain(socket_owner, token, now_ms) ==
                    HostControlSuccess) draining = true;
                else break;
            }
        }
        if (interop_linux_socket_poll_events(socket_owner, token, now_ms,
                &poll_request) != InteropLinuxSocketSuccess) {
            host_control_record_outcome(&outcome, HostControlPeerFailed);
            break;
        }
        if (poll_request.fd >= 0) {
            socket_index = (int)descriptor_count;
            descriptors[descriptor_count++] = (struct pollfd){
                .fd = poll_request.fd,
                .events = (short)poll_request.events,
                .revents = 0,
            };
        }
        daemon_index = (int)descriptor_count;
        descriptors[descriptor_count++] = (struct pollfd){
            .fd = admission.daemon_fd,
            .events = POLLIN,
            .revents = 0,
        };
        stop_index = (int)descriptor_count;
        descriptors[descriptor_count++] = (struct pollfd){
            .fd = config->stop_fd,
            .events = POLLIN,
            .revents = 0,
        };
        uint32_t suggested_ms = poll_request.timeout_ms;
        if (suggested_ms > HostControlMaxPollMs) suggested_ms = HostControlMaxPollMs;
        wait_deadline = draining ? UINT64_MAX : run_deadline;
        if (ready && !draining && next_heartbeat_ms < wait_deadline)
            wait_deadline = next_heartbeat_ms;
        if (now_ms >= wait_deadline) timeout_ms = 0;
        else {
            uint64_t remaining = wait_deadline - now_ms;
            timeout_ms = (int)(remaining < suggested_ms ? remaining : suggested_ms);
        }
        while (true) {
            now_ms = host_control_monotonic_ms();
            if (now_ms == UINT64_MAX) {
                host_control_record_outcome(&outcome, HostControlOsError);
                poll_result = -1;
                break;
            }
            if (now_ms >= wait_deadline) timeout_ms = 0;
            else {
                uint64_t remaining = wait_deadline - now_ms;
                timeout_ms = (int)(remaining < suggested_ms ? remaining : suggested_ms);
            }
            poll_result = poll(descriptors, descriptor_count, timeout_ms);
            if (poll_result >= 0 || errno != EINTR) break;
        }
        if (poll_result < 0) {
            host_control_record_outcome(&outcome, HostControlOsError);
            break;
        }
        now_ms = host_control_monotonic_ms();
        if (now_ms == UINT64_MAX) {
            host_control_record_outcome(&outcome, HostControlOsError);
            break;
        }
        if (!ready && !draining && now_ms >= run_deadline) {
            host_control_record_outcome(&outcome, HostControlTimedOut);
            if (host_control_begin_drain(socket_owner, token, now_ms) !=
                HostControlSuccess) {
                host_control_record_outcome(&outcome, HostControlPeerFailed);
                break;
            }
            draining = true;
            snapshot_invalidated = true;
        }
        if (descriptors[stop_index].revents != 0) {
            struct signalfd_siginfo signal_info;
            bool cancelled = false;
            if ((descriptors[stop_index].revents &
                    (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                host_control_record_outcome(&outcome, HostControlOsError);
            } else if ((descriptors[stop_index].revents & POLLIN) != 0) {
                bool retry_exhausted = true;
                for (uint32_t attempt = 0; attempt < 8; ++attempt) {
                    ssize_t signal_bytes = read(config->stop_fd, &signal_info,
                                                sizeof(signal_info));
                    if (signal_bytes < 0 && errno == EINTR) continue;
                    retry_exhausted = false;
                    if (signal_bytes < 0 && errno == EAGAIN) break;
                    if (signal_bytes != (ssize_t)sizeof(signal_info) ||
                        (signal_info.ssi_signo != SIGINT &&
                         signal_info.ssi_signo != SIGTERM)) {
                        host_control_record_outcome(&outcome, HostControlOsError);
                    } else {
                        cancelled = true;
                    }
                    break;
                }
                if (retry_exhausted)
                    host_control_record_outcome(&outcome, HostControlOsError);
            }
            if (cancelled) host_control_record_outcome(&outcome, HostControlCancelled);
            if (outcome != HostControlSuccess && !draining) {
                if (host_control_begin_drain(socket_owner, token, now_ms) ==
                    HostControlSuccess) {
                    draining = true;
                    snapshot_invalidated = true;
                } else break;
            }
        }
        short daemon_events = descriptors[daemon_index].revents;
        bool daemon_lost = false;
        if ((daemon_events & POLLIN) != 0) {
            uint8_t byte;
            ssize_t peeked = recv(admission.daemon_fd, &byte, 1,
                                  MSG_PEEK | MSG_DONTWAIT);
#if EAGAIN == EWOULDBLOCK
            daemon_lost = peeked >= 0 || errno != EAGAIN;
#else
            daemon_lost = peeked >= 0 ||
                (errno != EAGAIN && errno != EWOULDBLOCK);
#endif
        } else if ((daemon_events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            daemon_lost = true;
        }
        if (daemon_lost) {
            host_control_record_outcome(&outcome, HostControlDaemonLost);
            if (!draining && host_control_begin_drain(socket_owner, token, now_ms) ==
                    HostControlSuccess) { draining = true; snapshot_invalidated = true; }
            else if (!draining) break;
        }
        if (snapshot_invalidated) {
            if (interop_linux_socket_poll_events(socket_owner, token, now_ms,
                    &poll_request) != InteropLinuxSocketSuccess) {
                host_control_record_outcome(&outcome, HostControlPeerFailed);
                break;
            }
            socket_events = 0;
            if (poll_request.fd >= 0) {
                struct pollfd fresh_socket = {
                    .fd = poll_request.fd,
                    .events = (short)poll_request.events,
                    .revents = 0,
                };
                int refreshed = poll(&fresh_socket, 1, 0);
                if (refreshed > 0) socket_events = fresh_socket.revents;
                else if (refreshed < 0 && errno != EINTR) {
                    host_control_record_outcome(&outcome, HostControlOsError);
                    break;
                }
            }
        } else {
            socket_events = socket_index < 0 ? 0 : descriptors[socket_index].revents;
        }
        if (interop_linux_socket_drive(socket_owner, token, poll_request.poll_cookie,
                now_ms, (uint32_t)(unsigned short)socket_events,
                HostControlRxBudget, HostControlTxBudget, HostControlSyscallBudget,
                frame, sizeof(frame), &work) != InteropLinuxSocketSuccess) {
            host_control_record_outcome(&outcome, HostControlPeerFailed);
            break;
        }
        if (work.event.kind == InteropPeerEventValidateMapping) {
            interop_host_control_result_t mapping_result = host_control_report_mapping(out_owners->mapping_owner,
                socket_owner, token, &work.event, now_ms, &view, &snapshot,
                &view_published);
            if (mapping_result == HostControlMappingFailed) {
                /* Failed host mapping_result has already put the peer into Draining. */
                host_control_record_outcome(&outcome, mapping_result);
                draining = true;
            } else if (mapping_result != HostControlSuccess && !draining) {
                host_control_record_outcome(&outcome, mapping_result);
                if (host_control_begin_drain(socket_owner, token, now_ms) ==
                    HostControlSuccess) draining = true;
                else break;
            }
        } else if (work.event.kind == InteropPeerEventReady) {
            if (work.event.capabilities != 0 || !view_published) {
                host_control_record_outcome(&outcome, HostControlPeerFailed);
                if (host_control_begin_drain(socket_owner, token, now_ms) ==
                    HostControlSuccess) draining = true;
                else break;
            } else if (outcome == HostControlSuccess) {
                ready = true;
                last_heartbeat = snapshot.host_heartbeat;
                next_heartbeat_ms = now_ms > UINT64_MAX - HostControlHeartbeatMs
                    ? UINT64_MAX : now_ms + HostControlHeartbeatMs;
            }
        } else if (work.event.kind == InteropPeerEventFeatureFrame) {
            if (outcome == HostControlSuccess)
                host_control_record_outcome(&outcome, HostControlUnexpectedFeature);
            if (!draining && host_control_begin_drain(socket_owner, token, now_ms) ==
                    HostControlSuccess) draining = true;
            else if (!draining) break;
        } else if (work.event.kind == InteropPeerEventError ||
                   work.event.kind == InteropPeerEventDraining) {
            if (!draining) draining = true;
            host_control_record_outcome(&outcome, HostControlPeerFailed);
        }
        if (work.state == InteropLinuxSocketTransportClosed) {
            if (!draining) host_control_record_outcome(&outcome, HostControlPeerFailed);
            break;
        }
    }

cleanup:
    if (view_published) {
        /* The pump is synchronous and no other thread ever receives this view. */
        view_published = false;
    }
    if (host_control_cleanup_owners(out_owners) == HostControlCleanupRequired)
        return HostControlCleanupRequired;
    return outcome;
}
