#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session_control_windows_api_double.h"
#include "windows_socket_owner.h"
#include "interop_mapping_windows.h"
#include "interop_mapping_init.h"
#include "interop_layout.h"
#include "waddle/interop_protocol.h"
#include "waddle/interop_memory.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>

struct interop_mapping_windows_t { void *base; };
struct interop_windows_socket_t { uint32_t drives; };
static uint64_t test_now_ms = 1000;
static uint32_t test_wsa_calls;
static uint32_t test_last_wsa_error_calls;
static uint32_t test_map_close_calls;
static uint32_t test_poll_calls;
static uint32_t test_socket_drive_calls;
static uint32_t test_close_socket_calls;
static uint32_t test_init_random;
static uint32_t test_last_error = 5;
static DWORD test_thread_id = 77;
static interop_guest_test_failure_t test_failure;
static uint32_t test_fast_timeout = 1;
static uint32_t test_socket_close_tries;
static uint32_t test_clock_reads;
static uint32_t test_clock_fail_read;
static uint32_t test_clock_override_read;
static uint64_t test_clock_override_value;
static uint32_t test_accepted;
static uint8_t *test_mapping_base;
static uint64_t test_final_guest_heartbeat;
static uint64_t test_ready_event_ms;
static uint32_t test_guest_heartbeat_stores;
static uint32_t test_guest_heartbeat_before_ready;

void interop_guest_test_reset(void) {
    test_now_ms = 1000; test_wsa_calls = 0; test_last_wsa_error_calls = 0; test_map_close_calls = 0;
    test_poll_calls = 0; test_socket_drive_calls = 0; test_close_socket_calls = 0; test_init_random = 0;
    test_thread_id = 77;
    test_failure = InteropGuestTestNoFailure; test_fast_timeout = 1;
    test_socket_close_tries = 0; test_mapping_base = NULL;
    test_clock_reads = 0;
    test_clock_fail_read = 0; test_clock_override_read = 0; test_clock_override_value = 0;
    test_accepted = 0;
    test_final_guest_heartbeat = 0;
    test_ready_event_ms = 0; test_guest_heartbeat_stores = 0; test_guest_heartbeat_before_ready = 0;
}
void interop_guest_test_set_failure(interop_guest_test_failure_t failure) {
    test_failure = failure;
    test_last_error = failure == InteropGuestTestFailSocketClosePending ||
        failure == InteropGuestTestPeerQueryAndCloseFailure ? WSAEWOULDBLOCK : 5;
    if (failure == InteropGuestTestNoFailure || failure == InteropGuestTestMapClose ||
        failure == InteropGuestTestSocketClosePending || failure == InteropGuestTestSocketCloseIndeterminate ||
        failure == InteropGuestTestFailSocketPoll || failure == InteropGuestTestFailSessionPoll ||
        failure == InteropGuestTestFailSocketDrive || failure == InteropGuestTestReadyCapabilities ||
        failure == InteropGuestTestHeaderInvalid || failure == InteropGuestTestFeatureEvent ||
        failure == InteropGuestTestDrainingEvent || failure == InteropGuestTestHeartbeatMismatch ||
        failure == InteropGuestTestFailGuestHeartbeatLoad || failure == InteropGuestTestFailHostHeartbeatLoad ||
        failure == InteropGuestTestFailGuestHeartbeatStore || failure == InteropGuestTestNonreadyPhase ||
        failure == InteropGuestTestImmediateWork || failure == InteropGuestTestEofBeforeReady ||
        failure == InteropGuestTestNoReadyTimeout || failure == InteropGuestTestRunTimeoutAfterReady ||
        failure == InteropGuestTestMaxHeartbeat || failure == InteropGuestTestSocketWorkClosePending ||
        failure == InteropGuestTestSocketWorkCloseIndeterminate ||
        failure == InteropGuestTestSocketWorkPeerFailure ||
        failure == InteropGuestTestFailHeartbeatLoadDuringRun ||
        failure == InteropGuestTestStopDuringRun || failure == InteropGuestTestFailStopWaitDuringRun ||
        failure == InteropGuestTestStopAfterSessionPoll || failure == InteropGuestTestFailStopWaitAfterSessionPoll) {
        test_fast_timeout = 0;
    }
    if (failure == InteropGuestTestNoIncomingPeer || failure == InteropGuestTestClockBackwards) { test_fast_timeout = 0; }
}
void interop_guest_test_set_fast_timeout(uint32_t enabled) { test_fast_timeout = enabled != 0; }
void interop_guest_test_fail_clock_read(uint32_t read_number) { test_clock_fail_read = read_number; }
void interop_guest_test_override_clock_read(uint32_t read_number, uint64_t value) {
    test_clock_override_read = read_number; test_clock_override_value = value;
}
void interop_guest_test_set_thread_id(DWORD value) { test_thread_id = value; }
uint32_t interop_guest_test_wsa_calls(void) { return test_wsa_calls; }
uint32_t interop_guest_test_map_close_calls(void) { return test_map_close_calls; }
uint32_t interop_guest_test_socket_drive_calls(void) { return test_socket_drive_calls; }
uint32_t interop_guest_test_close_socket_calls(void) { return test_close_socket_calls; }
uint64_t interop_guest_test_final_guest_heartbeat(void) { return test_final_guest_heartbeat; }
uint32_t interop_guest_test_guest_heartbeat_stores(void) { return test_guest_heartbeat_stores; }
uint32_t interop_guest_test_guest_heartbeat_before_ready(void) { return test_guest_heartbeat_before_ready; }
int interop_guest_test_wsa_startup(uint16_t version, WSADATA *data) {
    test_wsa_calls++;
    if (test_failure == InteropGuestTestFailStartup) { return 10091; }
    if (version != MAKEWORD(2, 2) || data == NULL) { return -1; }
    data->version = version; return 0;
}
uint32_t interop_guest_test_last_wsa_error_calls(void) { return test_last_wsa_error_calls; }
int interop_guest_test_wsa_cleanup(void) { return test_failure == InteropGuestTestFailWsaCleanup ? -1 : 0; }
void *interop_guest_test_calloc(size_t count, size_t size) {
    if (test_failure == InteropGuestTestFailSessionAllocation) { return NULL; }
    return calloc(count, size);
}
HANDLE interop_guest_test_open_family(void) { return test_failure == InteropGuestTestFailOpenFamily ? (HANDLE)-1 : (HANDLE)3; }
int interop_guest_test_query_family(HANDLE handle, DWORD *family, DWORD *returned) {
    if (handle != (HANDLE)3 || family == NULL || returned == NULL) { return 0; }
    if (test_failure == InteropGuestTestFailQueryFamily) { return 0; }
    *family = test_failure == InteropGuestTestBadFamily ||
        test_failure == InteropGuestTestBadFamilyCloseFailure ? UINT16_MAX + 1u :
        test_failure == InteropGuestTestZeroFamily ? 0 : 0x4242;
    *returned = test_failure == InteropGuestTestBadFamilySize ? 0 : sizeof(*family); return 1;
}
int interop_guest_test_close_handle(HANDLE handle) {
    return handle == (HANDLE)3 && test_failure != InteropGuestTestFailCloseHandle &&
        test_failure != InteropGuestTestBadFamilyCloseFailure;
}
SOCKET interop_guest_test_create_socket(int family) { return family == 0x4242 && test_failure != InteropGuestTestFailCreateListener ? 10 : INVALID_SOCKET; }
int interop_guest_test_set_noninherit(SOCKET socket_value) {
    return (socket_value == 10 && test_failure != InteropGuestTestFailListenerInherit) ||
           (socket_value == 11 && test_failure != InteropGuestTestFailAcceptedInherit);
}
int interop_guest_test_set_nonblocking(SOCKET socket_value) {
    return (socket_value == 10 && test_failure != InteropGuestTestFailListenerNonblock) ||
           (socket_value == 11 && test_failure != InteropGuestTestFailAcceptedNonblock);
}
int interop_guest_test_bind(SOCKET socket_value, const void *address, int length) {
    return socket_value == 10 && address != NULL && length == 12 && test_failure != InteropGuestTestFailBind ? 0 : -1;
}
int interop_guest_test_listen(SOCKET socket_value) { return socket_value == 10 && test_failure != InteropGuestTestFailListen ? 0 : -1; }
int interop_guest_test_poll(WSAPOLLFD *descriptor, unsigned long count, int timeout_ms) {
    test_poll_calls++;
    if (count != 1 || descriptor == NULL || timeout_ms < 0) { return -1; }
    if (descriptor->fd == 10 && test_failure == InteropGuestTestFailListenerPoll) { return -1; }
    if (descriptor->fd != 10 && test_failure == InteropGuestTestFailSessionPoll) { return -1; }
    if (descriptor->fd == 10 && test_failure != InteropGuestTestFailListenerPoll) {
        if (test_failure == InteropGuestTestNoIncomingPeer) {
            uint64_t advance = timeout_ms > 1 ? (uint64_t)timeout_ms / 2 : (uint64_t)timeout_ms;
            test_now_ms += advance; descriptor->revents = 0; return 0;
        }
        if (test_failure == InteropGuestTestListenerNoEvent) {
            descriptor->revents = 0; test_now_ms += 30001; test_failure = InteropGuestTestNoFailure;
        }
        else { descriptor->revents = test_failure == InteropGuestTestBadPollEvent ? POLLERR : POLLRDNORM; }
        return 1;
    }
    if (descriptor->fd != 10 && test_failure == InteropGuestTestNoReadyTimeout) {
        test_now_ms += 120001;
        descriptor->revents = 0;
        return 0;
    }
    if (timeout_ms == 0) { test_now_ms += test_failure == InteropGuestTestImmediateWork ? 100u : 1u; }
    else if (test_poll_calls == 2 && test_fast_timeout != 0) { test_now_ms = 120999; }
    else { test_now_ms += (uint64_t)timeout_ms; }
    if (test_failure == InteropGuestTestHeartbeatMismatch && test_mapping_base != NULL && test_now_ms >= 1500) {
        interop_shm_header_t *header = (interop_shm_header_t *)test_mapping_base;
        atomic_store_explicit(&header->guest_heartbeat, UINT64_C(77), memory_order_relaxed);
        test_failure = InteropGuestTestNoFailure;
    }
    descriptor->revents = 0; return 0;
}
SOCKET interop_guest_test_accept(SOCKET socket_value) {
    if (socket_value == 10 && test_failure != InteropGuestTestFailAccept) { test_accepted = 1; return 11; }
    return INVALID_SOCKET;
}
int interop_guest_test_peer_address(SOCKET socket_value, void *out, int *length) {
    struct { uint16_t family; uint16_t reserved; uint32_t port; uint32_t cid; } address = {0x4242, 0, 5002, 2};
    if (socket_value != 11 || out == NULL || length == NULL || *length < (int)sizeof(address) ||
        test_failure == InteropGuestTestFailPeerQuery || test_failure == InteropGuestTestPeerQueryAndCloseFailure) { return -1; }
    if (test_failure == InteropGuestTestBadPeer) { address.cid = 4; }
    if (test_failure == InteropGuestTestBadPeerFamily) { address.family++; }
    if (test_failure == InteropGuestTestBadPeerReserved) { address.reserved = 1; }
    memcpy(out, &address, sizeof(address));
    *length = test_failure == InteropGuestTestBadPeerLength ? (int)sizeof(address) - 1 : (int)sizeof(address);
    return 0;
}
int interop_guest_test_close_socket(SOCKET socket_value) {
    test_close_socket_calls++;
    return (socket_value == 10 || socket_value == 11) &&
        test_failure != InteropGuestTestFailSocketClose && test_failure != InteropGuestTestFailSocketClosePending &&
        test_failure != InteropGuestTestPeerQueryAndCloseFailure ? 0 : -1;
}
DWORD interop_guest_test_wait_stop(HANDLE event, DWORD timeout_ms) {
    (void)timeout_ms;
    if (test_failure == InteropGuestTestFailStopWait ||
        (test_failure == InteropGuestTestFailStopWaitDuringRun && test_accepted) ||
        (test_failure == InteropGuestTestFailStopWaitAfterSessionPoll && test_poll_calls > 1)) { return UINT32_MAX; }
    if (event != (HANDLE)0 && (test_failure == InteropGuestTestStop ||
        (test_failure == InteropGuestTestStopDuringRun && test_accepted) ||
        (test_failure == InteropGuestTestStopAfterSessionPoll && test_poll_calls > 1))) { return WAIT_OBJECT_0; }
    return WAIT_TIMEOUT;
}
uint64_t interop_guest_test_now(void) {
    test_clock_reads++;
    if (test_clock_override_read != 0 && test_clock_reads == test_clock_override_read) { return test_clock_override_value; }
    if (test_clock_fail_read != 0 && test_clock_reads == test_clock_fail_read) { return UINT64_MAX; }
    if (test_failure == InteropGuestTestClockError) { return UINT64_MAX; }
    if (test_failure == InteropGuestTestClockNearWrap) { return UINT64_MAX - 100; }
    if (test_failure == InteropGuestTestClockBackwards && test_clock_reads == 2) { return test_now_ms - 1; }
    return test_now_ms;
}
DWORD interop_guest_test_thread_id(void) { return test_thread_id; }
int interop_guest_test_random(void *output, size_t length) {
    if (test_failure == InteropGuestTestFailRandom) { return (int32_t)UINT32_C(0xc000000d); }
    if (output == NULL || length != sizeof(uint64_t)) { return -1; }
    uint64_t epoch = test_failure == InteropGuestTestZeroEpoch ? 0 : ++test_init_random + 8;
    memcpy(output, &epoch, sizeof(epoch)); return 0;
}
int interop_guest_test_last_wsa_error(void) { test_last_wsa_error_calls++; return test_last_error; }
DWORD interop_guest_test_last_win32_error(void) { return test_last_error; }
void interop_guest_test_sleep(uint32_t milliseconds) { test_now_ms += milliseconds; }
int interop_guest_test_mapping_create(struct interop_mapping_windows_t **out) {
    if (test_failure == InteropGuestTestFailMappingCreate) { return InteropMappingWindowsOutOfMemory; }
    if (out == NULL || *out != NULL) { return InteropMappingWindowsInvalidArgument; }
    *out = calloc(1, sizeof(**out)); return *out == NULL ? InteropMappingWindowsOutOfMemory : InteropMappingWindowsOk;
}
int interop_guest_test_mapping_open(struct interop_mapping_windows_t *owner, uint32_t index,
                                    struct interop_mapping_windows_view_t *view_out) {
    if (test_failure == InteropGuestTestFailMappingOpen) { return InteropMappingWindowsOsError; }
    if (owner == NULL || index != 4 || view_out == NULL) { return InteropMappingWindowsInvalidArgument; }
    owner->base = mmap(NULL, (size_t)UINT64_C(0x80000000), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (owner->base == MAP_FAILED) { owner->base = NULL; return InteropMappingWindowsOsError; }
    test_mapping_base = owner->base;
    memset(view_out, 0, sizeof(*view_out));
    view_out->mapping_base = (uintptr_t)owner->base; view_out->mapping_length = UINT64_C(0x80000000);
    view_out->peer_id = 1; return InteropMappingWindowsOk;
}
int interop_guest_test_mapping_close(struct interop_mapping_windows_t *owner, uint32_t proof) {
    test_map_close_calls++;
    if (owner == NULL) { return InteropMappingWindowsInvalidArgument; }
    if (owner->base != NULL) {
        interop_shm_header_t *header = (interop_shm_header_t *)owner->base;
        test_final_guest_heartbeat = atomic_load_explicit(&header->guest_heartbeat, memory_order_relaxed);
    }
    if (proof != 1 && owner->base != NULL) { return InteropMappingWindowsMissingProof; }
    if (test_failure == InteropGuestTestMapClose) { return InteropMappingWindowsCleanupPending; }
    if (owner->base != NULL && munmap(owner->base, (size_t)UINT64_C(0x80000000)) != 0) { return InteropMappingWindowsCleanupPending; }
    owner->base = NULL; return InteropMappingWindowsOk;
}
int interop_guest_test_mapping_initialize(uintptr_t base, uint64_t length, uint64_t epoch, uint32_t proof) {
    if (test_failure == InteropGuestTestFailMappingInitialize) { return InteropMappingInitStatusInvalidArgument; }
    return interop_mapping_initialize(base, length, epoch, proof);
}
int interop_guest_test_mapping_destroy(struct interop_mapping_windows_t **slot) {
    if (test_failure == InteropGuestTestFailMappingDestroy) { return InteropMappingWindowsBusy; }
    if (slot == NULL || *slot == NULL || (*slot)->base != NULL) { return InteropMappingWindowsBusy; }
    free(*slot); *slot = NULL; return InteropMappingWindowsOk;
}
int interop_guest_test_socket_create(SOCKET socket_value, const struct interop_windows_socket_config_t *config,
                                     struct interop_windows_socket_t **out) {
    if (socket_value != 11 || config == NULL || config->peer_config.local_capabilities != 0 || out == NULL || *out != NULL) { return InteropWindowsSocketInvalidArgument; }
    if (test_failure == InteropGuestTestFailSocketCreate) { return InteropWindowsSocketOsError; }
    *out = calloc(1, sizeof(**out)); return *out == NULL ? InteropWindowsSocketOutOfMemory : InteropWindowsSocketSuccess;
}
int interop_guest_test_socket_destroy(struct interop_windows_socket_t **slot) {
    if (slot == NULL || *slot == NULL) { return InteropWindowsSocketSuccess; }
    if ((test_failure == InteropGuestTestSocketClosePending ||
         test_failure == InteropGuestTestRetrySocketClosePending) && test_socket_close_tries++ == 0) { return InteropWindowsSocketClosePending; }
    if (test_failure == InteropGuestTestSocketCloseIndeterminate) { return InteropWindowsSocketCloseIndeterminate; }
    if (test_failure == InteropGuestTestSocketDestroyPeerFailure) { return InteropWindowsSocketPeerFailure; }
    free(*slot); *slot = NULL; return InteropWindowsSocketSuccess;
}
int interop_guest_test_socket_retry_close(struct interop_windows_socket_t *owner, uint64_t token) {
    (void)owner; (void)token;
    return test_failure == InteropGuestTestRetrySocketClosePending ? InteropWindowsSocketClosePending : InteropWindowsSocketSuccess;
}
int interop_guest_test_socket_poll_events(struct interop_windows_socket_t *owner, uint64_t token,
    uint64_t now_ms, struct interop_windows_socket_poll_t *out) {
    if (owner == NULL || token == 0 || out == NULL) { return InteropWindowsSocketInvalidArgument; }
    if (test_failure == InteropGuestTestFailSocketPoll) { return InteropWindowsSocketOsError; }
    *out = (interop_windows_socket_poll_t){99, POLLRDNORM,
        test_failure == InteropGuestTestShortPollTimeout ? 50u : 100u,
        test_failure == InteropGuestTestImmediateWork ? 1u : 0u, 0, now_ms + 1};
    return InteropWindowsSocketSuccess;
}
int interop_guest_test_socket_drive(struct interop_windows_socket_t *owner, uint64_t token,
    uint64_t cookie, uint64_t now_ms, uint32_t revents, size_t rx, size_t tx, uint32_t calls,
    uint8_t *frame, size_t frame_size, struct interop_windows_socket_work_t *out) {
    (void)cookie; (void)now_ms; (void)revents; (void)rx; (void)tx; (void)calls; (void)frame; (void)frame_size;
    if (owner == NULL || token == 0 || out == NULL) { return InteropWindowsSocketInvalidArgument; }
    if (test_failure == InteropGuestTestFailSocketDrive) { return InteropWindowsSocketOsError; }
    memset(out, 0, sizeof(*out)); out->state = InteropWindowsSocketActive; out->status = InteropWindowsSocketWorkProgress;
    if (test_failure == InteropGuestTestEofBeforeReady) {
        out->state = InteropWindowsSocketTransportClosed;
        out->status = InteropWindowsSocketWorkTransportClosed;
        out->terminal_reason = InteropWindowsSocketTerminalEof;
        out->event.phase = InteropPeerAwaitHelloAck;
        test_socket_drive_calls++;
        return InteropWindowsSocketSuccess;
    }
    if (test_failure == InteropGuestTestRunTimeoutAfterReady && owner->drives == 0) { test_now_ms = 121001; }
    if (test_failure == InteropGuestTestSocketWorkClosePending ||
        test_failure == InteropGuestTestSocketWorkCloseIndeterminate ||
        test_failure == InteropGuestTestSocketWorkPeerFailure) {
        out->status = test_failure == InteropGuestTestSocketWorkClosePending ?
            InteropWindowsSocketWorkClosePending : InteropWindowsSocketWorkCloseIndeterminate;
        if (test_failure == InteropGuestTestSocketWorkPeerFailure) { out->status = InteropWindowsSocketWorkPeerFailure; }
        out->state = test_failure == InteropGuestTestSocketWorkClosePending ?
            InteropWindowsSocketClosePendingState : InteropWindowsSocketCloseIndeterminateState;
        test_socket_drive_calls++;
        return InteropWindowsSocketSuccess;
    }
    int first_drive = owner->drives++ == 0;
    if (first_drive) { test_ready_event_ms = test_now_ms; }
    out->event.kind = first_drive ? InteropPeerEventReady : InteropPeerEventNone;
    if (test_failure == InteropGuestTestFeatureEvent) { out->event.kind = InteropPeerEventFeatureFrame; }
    if (test_failure == InteropGuestTestDrainingEvent) { out->event.kind = InteropPeerEventDraining; }
    if (test_failure == InteropGuestTestErrorEvent) { out->event.kind = InteropPeerEventError; }
    if (test_failure == InteropGuestTestReadyCapabilities) { out->event.capabilities = 1; }
    out->event.phase = test_failure == InteropGuestTestNonreadyPhase ? InteropPeerAwaitHelloAck :
        first_drive ? InteropPeerReady : 0;
    test_socket_drive_calls++; return InteropWindowsSocketSuccess;
}
int interop_guest_test_socket_mapping_result(struct interop_windows_socket_t *owner, uint64_t token,
    uint64_t now_ms, uint64_t epoch, uint32_t version, uint32_t succeeded, struct interop_peer_event_t *out) {
    (void)now_ms; if (owner == NULL || token == 0 || epoch == 0 || version != InteropLayoutVersion || succeeded != 1 || out == NULL) { return InteropWindowsSocketInvalidArgument; }
    if (test_failure == InteropGuestTestFailMappingResult) { return InteropWindowsSocketMappingFailed; }
    memset(out, 0, sizeof(*out)); return InteropWindowsSocketSuccess;
}
int interop_guest_test_socket_begin_drain(struct interop_windows_socket_t *owner, uint64_t token,
    uint64_t now_ms, struct interop_peer_event_t *out) {
    (void)now_ms; if (owner == NULL || token == 0 || out == NULL) { return InteropWindowsSocketInvalidArgument; }
    memset(out, 0, sizeof(*out)); return InteropWindowsSocketSuccess;
}
interop_atomic_status_t interop_guest_test_heartbeat_load(const interop_shm_header_t *header,
    uintptr_t base, uint64_t length, interop_heartbeat_kind_t kind, uint64_t *value) {
    if (kind == InteropGuestHeartbeat && test_failure == InteropGuestTestFailHeartbeatLoadDuringRun && test_now_ms >= 1500) {
        return InteropAtomicStatusNotLockFree;
    }
    if (kind == InteropGuestHeartbeat && test_failure == InteropGuestTestFailGuestHeartbeatLoad) { return InteropAtomicStatusNotLockFree; }
    if (kind == InteropHostHeartbeat && test_failure == InteropGuestTestFailHostHeartbeatLoad) { return InteropAtomicStatusNotLockFree; }
    return interop_heartbeat_load(header, base, length, kind, value);
}
interop_atomic_status_t interop_guest_test_heartbeat_store_next(interop_shm_header_t *header,
    uintptr_t base, uint64_t length, interop_heartbeat_kind_t kind, uint64_t next) {
    if (kind == InteropGuestHeartbeat && test_failure == InteropGuestTestFailGuestHeartbeatStore) { return InteropAtomicStatusNotLockFree; }
    if (kind == InteropGuestHeartbeat) {
        test_guest_heartbeat_stores++;
        if (test_ready_event_ms == 0 || test_now_ms < test_ready_event_ms + 500u) { test_guest_heartbeat_before_ready++; }
    }
    return interop_heartbeat_store_next(header, base, length, kind, next);
}
int interop_guest_test_socket_quarantine_handoff(struct interop_windows_socket_t **slot,
    uint64_t token, struct interop_windows_socket_quarantine_t *out) {
    interop_windows_socket_quarantine_t *record = (interop_windows_socket_quarantine_t *)out;
    if (slot == NULL || *slot == NULL || token == 0 || out == NULL) { return InteropWindowsSocketInvalidArgument; }
    memset(record, 0, sizeof(*record)); record->socket_value = 11; record->wsa_error = 10022;
    record->close_classification = InteropWindowsSocketCloseIndeterminateState;
    record->owner_thread_id = test_thread_id; record->connection_token = token;
    free(*slot); *slot = NULL; return InteropWindowsSocketSuccess;
}
interop_layout_status_t interop_layout_validate_shm_header(const uint8_t *bytes, size_t length,
    uintptr_t base, uint64_t mapping_length, uint64_t epoch, interop_shm_header_snapshot_t *out) {
    if (test_failure == InteropGuestTestHeaderInvalid) { return InteropLayoutStatusInvalidLayout; }
    if (bytes == NULL || length != 4096 || base == 0 || mapping_length != UINT64_C(0x80000000) || epoch == 0 || out == NULL) { return InteropLayoutStatusInvalidArgument; }
    memset(out, 0, sizeof(*out)); out->session_epoch = epoch; out->total_bytes = mapping_length; return InteropLayoutStatusOk;
}
