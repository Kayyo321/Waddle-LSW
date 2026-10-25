/**
 * @file session_main.c
 * @brief `waddle-session` control-only diagnostic command entry.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session_control_host.h"

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

typedef struct session_cli_options_t {
    const char *device_name;
    interop_mapping_linux_backing_t backing_kind;
    uint32_t run_for_ms;
    bool help;
} session_cli_options_t;

static void session_print_help(FILE *stream)
{
    fputs("Usage: waddle-session --control-only --device NAME [--backing-kind file|kvmfr] [--run-for-ms N]\n"
          "Runs bounded protocol control diagnostics only; advertises no feature capabilities.\n"
          "The default backing kind is file. N is 1..120000 ms after session admission.\n",
          stream);
}

static bool session_parse_decimal(const char *text, uint32_t *value)
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
    if (parsed < 1 || parsed > SessionMaximumRunMs) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool session_parse_args(int argc, char **argv, session_cli_options_t *options)
{
    bool control_only = false;
    bool backing_seen = false;
    bool run_time_seen = false;
    if (options == NULL || argc < 1 || argv == NULL) return false;
    *options = (session_cli_options_t){
        .device_name = NULL,
        .backing_kind = InteropMappingLinuxBackingRegularPrivateFile,
        .run_for_ms = SessionDefaultRunMs,
        .help = false,
    };
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            options->help = true;
            continue;
        }
        if (strcmp(argv[index], "--control-only") == 0) {
            if (control_only) return false;
            control_only = true;
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
    return control_only && options->device_name != NULL &&
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
    interop_host_control_result_t result = interop_host_control_run(&control, &owners);
    if (result == HostControlSuccess) {
        exit_code = 0;
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
    if (signal_fd >= 0) close(signal_fd);
    if (signal_mask_changed) (void)pthread_sigmask(SIG_SETMASK, &prior_mask, NULL);
    return exit_code;
}

#ifndef WaddleSessionNoMain
int main(int argc, char **argv)
{
    return waddle_session_main(argc, argv);
}
#endif
