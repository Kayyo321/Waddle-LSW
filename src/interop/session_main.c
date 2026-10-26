/**
 * @file session_main.c
 * @brief `waddle-session` control-only diagnostic command entry.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session_app.h"
#include "session_control_host.h"
#include "session_handlers_builtin.h"
#include "session_supervisor.h"
#include "session_exit_codes.h"

#include "daemon_config.h"
#include "daemon_device.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef WaddleSessionAgentVersion
#define WaddleSessionAgentVersion "0.0.0"
#endif

#define SessionAdmissionTimeoutMs UINT64_C(120000)
#define SessionGuestReadyTimeoutMs 30000u
#define SessionDrainTimeoutMs 5000u
#define SessionDefaultRunMs 30000u
#define SessionMaximumRunMs 120000u
#define SessionMaximumRetries 1000u

typedef struct session_cli_options_t {
    const char *device_name;
    interop_mapping_linux_backing_t backing_kind;
    uint32_t run_for_ms;
    uint32_t max_retries;
    bool control_only;
    bool help;
} session_cli_options_t;

static void session_print_help(FILE *stream)
{
    fputs("Usage: waddle-session --device NAME [--backing-kind file|kvmfr] [--run-for-ms N]\n"
          "                     [--supervised | --control-only] [--max-retries N]\n"
          "Default (no mode flag) and --supervised: each attempt is a fresh admission with the built-in\n"
          "handlers; retryable refusals and dropped guests are retried with 250 ms to 4 s backoff.\n"
          "--max-retries 0 retries without limit (supervised only).\n"
          "--control-only: one bounded control session; advertises no feature capabilities.\n"
          "Exit 75: another session holds this device's lock (a handover, not a failure).\n"
          "The default backing kind is file. N is 1..120000 ms after session admission.\n",
          stream);
}

/* Parse an unsigned decimal in [minimum, maximum]; rejects signs, spaces, and overflow. */
static bool session_parse_bounded(const char *text, uint64_t minimum, uint64_t maximum, uint32_t *value)
{
    uint64_t parsed = 0;
    if (text == NULL || text[0] == '\0' || value == NULL) return false;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)text[index];
        if (character < '0' || character > '9') return false;
        uint32_t digit = (uint32_t)(character - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    if (parsed < minimum || parsed > maximum) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool session_parse_decimal(const char *text, uint32_t *value)
{
    return session_parse_bounded(text, 1, SessionMaximumRunMs, value);
}

static bool session_parse_retries(const char *text, uint32_t *value)
{
    return session_parse_bounded(text, 0, SessionMaximumRetries, value);
}

static bool session_parse_args(int argc, char **argv, session_cli_options_t *options)
{
    bool supervised_seen = false;
    bool retries_seen = false;
    bool backing_seen = false;
    bool run_time_seen = false;
    if (options == NULL || argc < 1 || argv == NULL) return false;
    *options = (session_cli_options_t){
        .device_name = NULL,
        .backing_kind = InteropMappingLinuxBackingRegularPrivateFile,
        .run_for_ms = SessionDefaultRunMs,
        .max_retries = 0,
        .control_only = false,
        .help = false,
    };
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            options->help = true;
            continue;
        }
        if (strcmp(argv[index], "--control-only") == 0) {
            if (options->control_only || supervised_seen) return false;
            options->control_only = true;
            continue;
        }
        if (strcmp(argv[index], "--supervised") == 0) {
            if (supervised_seen || options->control_only) return false;
            supervised_seen = true;
            continue;
        }
        if (strcmp(argv[index], "--max-retries") == 0 && index + 1 < argc) {
            if (retries_seen) return false;
            retries_seen = true;
            if (!session_parse_retries(argv[++index], &options->max_retries))
                return false;
            continue;
        }
        if (strcmp(argv[index], "--device") == 0 && index + 1 < argc) {
            if (options->device_name != NULL) return false;
            options->device_name = argv[++index];
            continue;
        }
        if (strcmp(argv[index], "--backing-kind") == 0 && index + 1 < argc) {
            if (backing_seen) return false;
            backing_seen = true;
            const char *kind = argv[++index];
            if (strcmp(kind, "file") == 0)
                options->backing_kind = InteropMappingLinuxBackingRegularPrivateFile;
            else if (strcmp(kind, "kvmfr") == 0)
                options->backing_kind = InteropMappingLinuxBackingKvmfrCharacter;
            else return false;
            continue;
        }
        if (strcmp(argv[index], "--run-for-ms") == 0 && index + 1 < argc) {
            if (run_time_seen) return false;
            run_time_seen = true;
            if (!session_parse_decimal(argv[++index], &options->run_for_ms))
                return false;
            continue;
        }
        return false;
    }
    if (options->help) return true;
    if (retries_seen && options->control_only) return false;
    return options->device_name != NULL &&
        daemon_device_validate_name(options->device_name) == 0;
}

static uint64_t session_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return UINT64_MAX;
    uint64_t seconds = (uint64_t)now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)now.tv_nsec / UINT64_C(1000000)) /
            UINT64_C(1000)) return UINT64_MAX;
    return seconds * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static int session_wait_connect(int fd, int stop_fd, uint64_t deadline_ms)
{
    while (true) {
        uint64_t now_ms = session_monotonic_ms();
        if (now_ms == UINT64_MAX) { errno = EIO; return -1; }
        if (now_ms >= deadline_ms) { errno = ETIMEDOUT; return -1; }
        uint64_t remaining = deadline_ms - now_ms;
        int timeout = remaining > 1000 ? 1000 : (int)remaining;
        struct pollfd descriptors[2] = {
            {.fd = fd, .events = POLLOUT, .revents = 0},
            {.fd = stop_fd, .events = POLLIN, .revents = 0},
        };
        int result = poll(descriptors, 2, timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (result == 0) continue;
        if (descriptors[1].revents != 0) { errno = ECANCELED; return -1; }
        now_ms = session_monotonic_ms();
        if (now_ms == UINT64_MAX) { errno = EIO; return -1; }
        if (now_ms >= deadline_ms) { errno = ETIMEDOUT; return -1; }
        int socket_error = 0;
        socklen_t error_bytes = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_bytes) != 0)
            return -1;
        if (error_bytes != sizeof(socket_error)) { errno = EPROTO; return -1; }
        if (socket_error != 0) { errno = socket_error; return -1; }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            errno = ECONNRESET;
            return -1;
        }
        if ((descriptors[0].revents & POLLOUT) != 0) return 0;
    }
}

static int session_connect_daemon(const char *socket_path, int stop_fd,
                                  uint64_t deadline_ms)
{
    struct sockaddr_un address;
    if (socket_path == NULL || socket_path[0] == '\0' ||
        strlen(socket_path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int result = connect(fd, (const struct sockaddr *)&address, sizeof(address));
    if (result != 0 && errno != EINPROGRESS && errno != EAGAIN) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (result != 0 && session_wait_connect(fd, stop_fd, deadline_ms) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    uint64_t now_ms = session_monotonic_ms();
    if (now_ms == UINT64_MAX || now_ms >= deadline_ms) {
        int saved = now_ms == UINT64_MAX ? EIO : ETIMEDOUT;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int session_read_random_token(uint64_t *token, int stop_fd,
                                     uint64_t deadline_ms)
{
    uint32_t attempt = 0;
    while (attempt < 64) {
        uint64_t now_ms = session_monotonic_ms();
        if (now_ms == UINT64_MAX) { errno = EIO; return -1; }
        if (now_ms >= deadline_ms) { errno = ETIMEDOUT; return -1; }
        struct pollfd stop = {.fd = stop_fd, .events = POLLIN, .revents = 0};
        int ready = poll(&stop, 1, 0);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (ready > 0 && stop.revents != 0) { errno = ECANCELED; return -1; }
        uint64_t candidate = 0;
        size_t offset = 0;
        while (offset < sizeof(candidate)) {
            ssize_t result = getrandom((uint8_t *)&candidate + offset,
                sizeof(candidate) - offset, GRND_NONBLOCK);
            if (result < 0 && errno == EINTR) {
                if (++attempt >= 64) { errno = EAGAIN; return -1; }
                continue;
            }
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (++attempt >= 64) return -1;
                continue;
            }
            if (result <= 0) return -1;
            offset += (size_t)result;
        }
        if (candidate != 0) {
            now_ms = session_monotonic_ms();
            if (now_ms == UINT64_MAX) { errno = EIO; return -1; }
            if (now_ms >= deadline_ms) { errno = ETIMEDOUT; return -1; }
            struct pollfd final_stop = {
                .fd = stop_fd, .events = POLLIN, .revents = 0,
            };
            int final_ready = poll(&final_stop, 1, 0);
            if (final_ready < 0 && errno == EINTR) { ++attempt; continue; }
            if (final_ready < 0) return -1;
            if (final_ready > 0 && final_stop.revents != 0) {
                errno = ECANCELED;
                return -1;
            }
            *token = candidate;
            return 0;
        }
        ++attempt;
    }
    errno = EIO;
    return -1;
}

/* Read every queued signal record from the signal descriptor; EAGAIN ends the drain. */
static void session_signals_drain(int signal_fd)
{
    struct signalfd_siginfo record;
    for (;;) {
        ssize_t received = read(signal_fd, &record, sizeof(record));
        if (received < 0 && errno == EINTR) continue;
        if (received != (ssize_t)sizeof(record)) return;
    }
}

static int session_signals_begin(sigset_t *prior_mask)
{
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    int result = pthread_sigmask(SIG_BLOCK, &blocked, prior_mask);
    if (result != 0) { errno = result; return -1; }
    int signal_fd = signalfd(-1, &blocked, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) {
        int saved = errno;
        (void)pthread_sigmask(SIG_SETMASK, prior_mask, NULL);
        errno = saved;
        return -1;
    }
    return signal_fd;
}

static int session_resolve_device(const char *name, device_info_t *device)
{
    device_list_t devices;
    if (daemon_device_list(&devices) < 0) return -1;
    for (size_t index = 0; index < devices.count; ++index) {
        if (strcmp(devices.devices[index].name, name) != 0) continue;
        if (!devices.devices[index].config_valid) { errno = EINVAL; return -1; }
        *device = devices.devices[index];
        return 0;
    }
    errno = ENOENT;
    return -1;
}

/** @brief Execute the bounded control-only waddle-session diagnostic command.
 * @param[in] argc Argument count supplied by the process entry point.
 * @param[in] argv Borrowed NUL-terminated argument vector, valid for this call.
 * @return 0 after a clean bounded run, 2 for CLI/profile errors, or 1 for runtime failure.
 * @note Performs read-only device/profile discovery, opens existing configured descriptors,
 *       blocks SIGINT/SIGTERM in the calling thread, and may acquire then release local
 *       daemon/session/AV resources. If authentic owner cleanup still fails after retry,
 *       the standalone process exits at the process boundary so ownership cannot be lost.
 * @thread_safety Intended only for the process main thread; not reentrant.
 */
/** @brief Inputs shared by every supervised attempt; borrowed for the whole supervised run. */
typedef struct session_cli_attempts_t {
    const device_info_t *device; /**< Resolved device; read only. */
    const char *mapping_path; /**< Configured AV mapping path; read only. */
    interop_mapping_linux_backing_t backing_kind; /**< Mapping backing for every attempt. */
    int stop_fd; /**< Signal descriptor that cancels connection waits and admission. */
    uint32_t last_admission; /**< Session-layer admission result of the latest attempt; 0 when none. */
} session_cli_attempts_t;

/** @brief True when a session-layer admission result means another session holds a device lock. */
static bool session_admission_is_lock_held(uint32_t admission_result)
{
    return admission_result == HostSessionBusy || admission_result == HostSessionAvBusy;
}

/**
 * @brief Run one supervised attempt: connect, open the mapping, read a fresh token, admit and run.
 * @param[in] context session_cli_attempts_t, read only.
 * @param[in] app Application config of the run; its control fields are used and its admission
 *            is replaced for this attempt.
 * @param[out] owners Zero on entry; holds this attempt's owners.
 * @param[out] detail Non-NULL; the supervisor always passes one. Records the session-layer result when admission
 *             could not start: DaemonError for a failed daemon connection, OsError for a mapping or
 *             token failure, ClockError for an unusable monotonic clock.
 * @return The control outcome; AdmissionFailed when admission could not start.
 * @note The attempt's descriptors are closed before return. CleanupRequired is retried once here,
 *       while the mapping descriptor is still open.
 * @thread_safety Runs on the supervisor's calling thread only.
 */
static interop_host_control_result_t session_cli_attempt_body(session_cli_attempts_t *attempts,
    const session_app_config_t *app, interop_host_control_owners_t *owners,
    interop_host_control_detail_t *detail);

static interop_host_control_result_t session_cli_attempt(void *context,
    const session_app_config_t *app, interop_host_control_owners_t *owners,
    interop_host_control_detail_t *detail)
{
    session_cli_attempts_t *attempts = context;
    interop_host_control_result_t outcome = session_cli_attempt_body(attempts, app, owners, detail);
    attempts->last_admission = detail->admission_result;
    return outcome;
}

static interop_host_control_result_t session_cli_attempt_body(session_cli_attempts_t *attempts,
    const session_app_config_t *app, interop_host_control_owners_t *owners,
    interop_host_control_detail_t *detail)
{
    session_app_config_t attempt = *app;
    interop_host_control_result_t outcome;
    uint64_t started_ms = session_monotonic_ms();
    uint64_t deadline_ms;
    uint64_t token = 0;
    int daemon_fd;
    int mapping_fd;
    static const uint8_t agent_version[] = WaddleSessionAgentVersion;

    if (started_ms == UINT64_MAX || started_ms > UINT64_MAX - SessionAdmissionTimeoutMs) {
        detail->admission_result = HostSessionClockError;
        return HostControlAdmissionFailed;
    }
    deadline_ms = started_ms + SessionAdmissionTimeoutMs;
    daemon_fd = session_connect_daemon(attempts->device->socket_path, attempts->stop_fd, deadline_ms);
    if (daemon_fd < 0) {
        detail->admission_result = HostSessionDaemonError;
        return HostControlAdmissionFailed;
    }
    mapping_fd = open(attempts->mapping_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (mapping_fd < 0) {
        close(daemon_fd);
        detail->admission_result = HostSessionOsError;
        return HostControlAdmissionFailed;
    }
    if (session_read_random_token(&token, attempts->stop_fd, deadline_ms) != 0) {
        close(mapping_fd);
        close(daemon_fd);
        detail->admission_result = HostSessionOsError;
        return HostControlAdmissionFailed;
    }
    interop_host_session_config_t admission = {
        .device = attempts->device,
        .daemon_fd = daemon_fd,
        .agent_version = agent_version,
        .agent_version_bytes = sizeof(agent_version) - 1,
        .connection_token = token,
        .local_capabilities = 0,
        .deadline_ms = deadline_ms,
        .cancel_fd = attempts->stop_fd,
        .guest_ready_timeout_ms = SessionGuestReadyTimeoutMs,
        .drain_timeout_ms = SessionDrainTimeoutMs,
        .reserved_zero = 0,
    };
    attempt.control.admission = &admission;
    attempt.control.mapping_fd = mapping_fd;
    attempt.control.backing_kind = attempts->backing_kind;
    outcome = session_app_run_detailed(&attempt, owners, detail);
    if (outcome == HostControlCleanupRequired)
        (void)interop_host_control_retry_cleanup(owners);
    close(mapping_fd);
    close(daemon_fd);
    return outcome;
}

/**
 * @brief Run the supervised command: retry fresh admissions until a terminal outcome or stop.
 * @param[in] device Resolved device; read only.
 * @param[in] daemon_config Loaded configuration whose mapping path is used.
 * @param[in] backing_kind Mapping backing for every attempt.
 * @param[in] run_for_ms Run bound of each attempt, as --run-for-ms.
 * @param[in] max_retries Retry limit; 0 is unlimited.
 * @param[in] stop_fd Signal descriptor that ends the run.
 * @return 0 when the final outcome is Success, otherwise 1. Exits the process only when cleanup
 *         of a failed attempt remains impossible, as the one-shot command does.
 */
static int session_cli_run_supervised(const device_info_t *device,
    const daemon_config_t *daemon_config, interop_mapping_linux_backing_t backing_kind,
    uint32_t run_for_ms, uint32_t max_retries, int stop_fd)
{
    session_cli_attempts_t attempts = {
        .device = device,
        .mapping_path = daemon_config->av_shm_path,
        .backing_kind = backing_kind,
        .stop_fd = stop_fd,
        .last_admission = 0,
    };
    session_builtin_storage_t builtin_storage;
    session_builtin_config_t builtin = {
        .runtime_dir = NULL,
        .device_name = device->name,
        .storage = &builtin_storage,
    };
    session_handler_t handlers[SessionAppMaxHandlers];
    size_t handler_count = 0;
    session_supervisor_config_t supervisor;
    session_supervisor_report_t report;
    interop_host_control_owners_t owners = {0};
    interop_host_control_result_t outcome;
    char expected_socket[WaddleMaxPathLen];
    const char *runtime_root = getenv("XDG_RUNTIME_DIR");

    /* The launch handler serves session.sock where the client looks for it: the directory of the
     * daemon socket. The handler's layout is <runtime>/waddle/<device>, so the daemon socket must
     * follow that same layout; otherwise the client and the handler would disagree. */
    /* A relative XDG_RUNTIME_DIR was already refused by device resolution (daemon_device_get_socket_path),
     * so only a missing one can reach this check with a path that is not the handler's layout. */
    if (runtime_root == NULL ||
            snprintf(expected_socket, sizeof(expected_socket), "%s/waddle/%s/daemon.sock",
                runtime_root, device->name) < 0 ||
            strcmp(expected_socket, device->socket_path) != 0) {
        fprintf(stderr, "waddle-session: supervised mode needs an absolute XDG_RUNTIME_DIR that holds the daemon socket\n");
        return 2;
    }
    builtin.runtime_dir = runtime_root;
    memset(&builtin_storage, 0, sizeof(builtin_storage));
    if (session_handlers_builtin_make(&builtin, handlers, &handler_count) != 0) {
        fprintf(stderr, "waddle-session: handler registry rejected the device\n");
        return 1;
    }

    memset(&supervisor, 0, sizeof(supervisor));
    supervisor.app.control.stop_fd = stop_fd;
    supervisor.app.control.run_for_ms = run_for_ms;
    supervisor.app.control.backing_kind = backing_kind;
    supervisor.app.control.reserved_zero = 0;
    supervisor.app.handlers = handlers;
    supervisor.app.handler_count = handler_count;
    /* Attempts that end before the run bound stop the handlers and remove their socket, so an
     * ended run never leaves a stale launch endpoint behind. */
    supervisor.seams = session_supervisor_system_seams();
    supervisor.seams.context = &attempts;
    supervisor.seams.run = session_cli_attempt;
    supervisor.max_retries = max_retries;
    outcome = session_supervisor_run(&supervisor, &owners, &report);
    session_handlers_builtin_destroy(&builtin_storage);
    fprintf(stderr,
        "waddle-session: supervised attempts=%u retries=%u ready_sessions=%u backoff_ms=%llu\n",
        (unsigned)report.attempts, (unsigned)report.retries, (unsigned)report.ready_sessions,
        (unsigned long long)report.backoff_total_ms);
    /* A graceful local stop (LocalStop or a stop signal) is the clean end of a supervised run. */
    if (outcome == HostControlSuccess || outcome == HostControlCancelled) return WaddleSessionExitSuccess;
    if (outcome == HostControlAdmissionFailed && session_admission_is_lock_held(attempts.last_admission)) {
        fprintf(stderr, "waddle-session: another session holds this device's lock\n");
        return WaddleSessionExitLockHeld;
    }
    if (outcome == HostControlCleanupRequired &&
            interop_host_control_retry_cleanup(&owners) != HostControlSuccess) {
        fprintf(stderr, "waddle-session: local cleanup remains required\n");
        _exit(1);
    }
    fprintf(stderr, "waddle-session: supervised run failed (%u)\n", (unsigned)outcome);
    return 1;
}

int waddle_session_main(int argc, char **argv)
{
    session_cli_options_t options;
    device_info_t device;
    daemon_config_t daemon_config;
    sigset_t prior_mask;
    interop_host_control_owners_t owners = {0};
    int daemon_fd = -1;
    int mapping_fd = -1;
    int signal_fd = -1;
    bool signal_mask_changed = false;
    int exit_code = 1;

    if (!session_parse_args(argc, argv, &options)) {
        session_print_help(stderr);
        return 2;
    }
    if (options.help) {
        session_print_help(stdout);
        return 0;
    }
    if (session_resolve_device(options.device_name, &device) != 0) {
        fprintf(stderr, "waddle-session: device lookup failed: %s\n", strerror(errno));
        return 2;
    }
    daemon_config_init_defaults(&daemon_config);
    if (daemon_config_load_file(&daemon_config, device.config_path) != 0 ||
        daemon_config.av_enabled != 1 || daemon_config.av_shm_path[0] == '\0') {
        fprintf(stderr, "waddle-session: device has no valid configured AV mapping\n");
        return 2;
    }
    uint64_t started_ms = session_monotonic_ms();
    if (started_ms == UINT64_MAX || started_ms > UINT64_MAX - SessionAdmissionTimeoutMs) {
        fprintf(stderr, "waddle-session: monotonic clock unavailable\n");
        return 1;
    }
    uint64_t admission_deadline_ms = started_ms + SessionAdmissionTimeoutMs;
    signal_fd = session_signals_begin(&prior_mask);
    if (signal_fd < 0) {
        fprintf(stderr, "waddle-session: cannot install signal wait: %s\n", strerror(errno));
        return 1;
    }
    signal_mask_changed = true;
    if (!options.control_only) {
        exit_code = session_cli_run_supervised(&device, &daemon_config, options.backing_kind,
            options.run_for_ms, options.max_retries, signal_fd);
        goto cleanup;
    }
    daemon_fd = session_connect_daemon(device.socket_path, signal_fd,
                                       admission_deadline_ms);
    if (daemon_fd < 0) {
        fprintf(stderr, "waddle-session: daemon connection failed: %s\n", strerror(errno));
        goto cleanup;
    }
    mapping_fd = open(daemon_config.av_shm_path,
                      O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (mapping_fd < 0) {
        fprintf(stderr, "waddle-session: cannot open configured mapping: %s\n", strerror(errno));
        goto cleanup;
    }
    uint64_t token = 0;
    if (session_read_random_token(&token, signal_fd, admission_deadline_ms) != 0) {
        fprintf(stderr, "waddle-session: random connection token failed: %s\n", strerror(errno));
        goto cleanup;
    }
    static const uint8_t agent_version[] = WaddleSessionAgentVersion;
    interop_host_session_config_t admission = {
        .device = &device,
        .daemon_fd = daemon_fd,
        .agent_version = agent_version,
        .agent_version_bytes = sizeof(agent_version) - 1,
        .connection_token = token,
        .local_capabilities = 0,
        .deadline_ms = admission_deadline_ms,
        .cancel_fd = signal_fd,
        .guest_ready_timeout_ms = SessionGuestReadyTimeoutMs,
        .drain_timeout_ms = SessionDrainTimeoutMs,
        .reserved_zero = 0,
    };
    interop_host_control_config_t control = {
        .admission = &admission,
        .mapping_fd = mapping_fd,
        .backing_kind = options.backing_kind,
        .stop_fd = signal_fd,
        .run_for_ms = options.run_for_ms,
        .reserved_zero = 0,
        .feature_ops = NULL,
    };
    fprintf(stderr, "waddle-session: RuntimeControlOnly device=%s capabilities=0\n",
            device.name);
    interop_host_control_detail_t run_detail;
    memset(&run_detail, 0, sizeof(run_detail));
    interop_host_control_result_t result = interop_host_control_run_detailed(&control, &owners, &run_detail);
    if (result == HostControlSuccess) {
        exit_code = WaddleSessionExitSuccess;
    } else if (result == HostControlAdmissionFailed && session_admission_is_lock_held(run_detail.admission_result)) {
        fprintf(stderr, "waddle-session: another session holds this device's lock\n");
        exit_code = WaddleSessionExitLockHeld;
    } else {
        fprintf(stderr, "waddle-session: control-only run failed (%u)\n",
                (unsigned)result);
        if (result == HostControlCleanupRequired) {
            interop_host_control_result_t retried =
                interop_host_control_retry_cleanup(&owners);
            if (retried != HostControlSuccess) {
                fprintf(stderr, "waddle-session: local cleanup remains required\n");
                _exit(1);
            }
        }
    }

cleanup:
    if (daemon_fd >= 0) close(daemon_fd);
    if (mapping_fd >= 0) close(mapping_fd);
    if (signal_fd >= 0) {
        /* A stop signal the wait only polled stays pending while the descriptor is blocked. Consume it
         * here; restoring the prior mask would otherwise deliver it with its default action. */
        session_signals_drain(signal_fd);
        close(signal_fd);
    }
    if (signal_mask_changed) (void)pthread_sigmask(SIG_SETMASK, &prior_mask, NULL);
    return exit_code;
}

#ifndef WaddleSessionNoMain
int main(int argc, char **argv)
{
    return waddle_session_main(argc, argv);
}
#endif
