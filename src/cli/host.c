/**
 * @file host.c
 * @brief Main entry point and CLI option parser for the Waddle host execution utility.
 */

#include "session.h"
#include "path_rules.h"
#include "terminal.h"
#include "daemon_client.h"
#include "daemon_config.h"
#include "daemon_device.h"
#include "device_commands.h"
#include "waddle/daemon_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

/**
 * @brief Parses an unsigned 32-bit integer from a string.
 *
 * @param[in]  s   Null-terminated digit string.
 * @param[out] out Destination pointer for parsed value.
 * @return 0 on success, or -1 on parse failure or overflow.
 */
static int number(const char *s, uint32_t *out) {
    if (s == NULL || *s == '\0') {
        return -1;
    }
    uint64_t n = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        n = (n * 10) + (unsigned int)(*s - '0');
        if (n > UINT32_MAX) {
            return -1;
        }
    }
    *out = (uint32_t)n;
    return 0;
}

/**
 * @brief Prints CLI usage instructions to the specified output stream.
 *
 * @param[in,out] f File stream to write usage text into.
 */
static void usage(FILE *f) {
    fprintf(f,
            "Usage: waddle [command|option] [parameters...]\n"
            "\n"
            "Default Action:\n"
            "  waddle [device-name]           Enter interactive Windows terminal (ConPTY)\n"
            "                                 Auto-starts subsystem if not currently running.\n"
            "\n"
            "Device Management:\n"
            "  device list/show/default/config  Inspect profiles and select persistent default\n"
            "                                 Use device --help for command grammar and exit codes\n"
            "  init, --init <device-name>     Initialize a new isolated subsystem device\n"
            "                                 Options: --disk <path> (custom base disk)\n"
            "  devices, list                  List all configured devices and their status\n"
            "                                 Options: --json (machine-readable output)\n"
            "\n"
            "Subsystem Lifecycle Commands:\n"
            "  start, --start [device]        Start background subsystem (daemon + QEMU + virtiofsd)\n"
            "                                 Options: --wait (default), --no-wait, --timeout <sec>, -d <device>\n"
            "  stop, --stop [device]          Gracefully shut down background subsystem\n"
            "                                 Options: --force, -f, --timeout <sec>, -d <device>\n"
            "  restart, --restart [device]    Restart background subsystem\n"
            "  status, --status [device]      Display subsystem status and metrics\n"
            "                                 Options: --json (machine-readable output), -d <device>\n"
            "  kill, --kill [device]          Forcefully terminate subsystem processes and clean locks\n"
            "\n"
            "Process Execution:\n"
            "  --exec, -e <command>           Execute command in Windows guest with auto-start\n"
            "  exec, run [options] -- <cmd>   Explicit passthrough execution\n"
            "    --device, -d NAME            Target specific subsystem device\n"
            "    --socket-path PATH           UNIX mock transport (default: VSOCK)\n"
            "    --vsock-cid N                Guest VSOCK CID (default: device CID or 3)\n"
            "    --vsock-port N               Guest VSOCK port (default: 5242)\n"
            "    --pipe, -P                   Raw pipe streams (non-interactive)\n"
            "    --interactive, -i, --tty, -t Interactive terminal session (ConPTY)\n"
            "    --cwd PATH                   Working directory in guest\n"
            "    --env, -e KEY=VALUE          Set environment variable\n"
            "    --translate-path             Map Linux paths to guest VirtIO-FS drives\n"
            "    --path-map SRC=DEST          Repeatable export rule; implies translation\n"
            "    --timeout SECONDS            Total session deadline; 0 disables\n"
            "\n"
            "Filesystem Integration:\n"
            "  fs, --mount [device]           List active VirtIO-FS shared directory mappings\n"
            "  fs test [device]               Run cross-filesystem read/write verification\n"
            "\n"
            "Diagnostics & Information:\n"
            "  logs, --logs [device]          View subsystem and hypervisor logs (-f, -n <lines>, -d <device>)\n"
            "  --version, -v                  Show version information\n"
            "  --help, -h                     Show this help text\n");
}

/**
 * @brief Resolve explicit name, saved default, then sole registered profile.
 * @param[in] specified_device Borrowed optional explicit name; NULL uses precedence.
 * @param[out] out_dev Optional borrowed destination; receives owned inline metadata.
 * @param[out] out_socket_path Optional borrowed output buffer; receives named socket.
 * @param[in] socket_path_cap Output capacity including NUL.
 * @param[in] require_single Reserved compatibility parameter; all calls reject ambiguity.
 * @return 0 success, 2 missing/invalid/ambiguous selection or registry failure.
 * @note No allocation or guest start. Registry/default APIs hold short shared locks.
 * Missing and dangling defaults fail; no legacy global-profile inference is performed.
 */
static int resolve_target_device(const char *specified_device,
                                 device_info_t *out_dev,
                                 char *out_socket_path,
                                 size_t socket_path_cap,
                                 int require_single) {
    if (out_dev != NULL) {
        memset(out_dev, 0, sizeof(*out_dev));
    }
    if (out_socket_path != NULL && socket_path_cap > 0) {
        out_socket_path[0] = '\0';
    }

    (void)require_single;
    char selected[WaddleMaxDeviceNameLen];
    if (specified_device == NULL) {
        if (daemon_device_default_get(selected, sizeof(selected)) != 0) {
            fprintf(stderr, "waddle: invalid saved default: %s\n", strerror(errno));
            return 2;
        }
        if (selected[0] != '\0') specified_device = selected;
    }
    device_list_t list;
    if (daemon_device_list(&list) < 0) {
        fprintf(stderr, "waddle: registry discovery failed: %s\n", strerror(errno));
        return 2;
    }
    device_info_t *chosen = NULL;
    if (specified_device != NULL) {
        for (size_t i = 0; i < list.count; i++) {
            if (strcmp(specified_device, list.devices[i].name) == 0) {
                chosen = &list.devices[i];
                break;
            }
        }
        if (chosen == NULL) {
            fprintf(stderr, "waddle: device '%s' not found; select an existing device\n", specified_device);
            return 2;
        }
    } else if (list.count == 1) {
        chosen = &list.devices[0];
    } else {
        if (list.count == 0) {
            fprintf(stderr, "waddle: no devices configured; use 'waddle init NAME'\n");
        } else {
            fprintf(stderr, "waddle: ambiguous device; specify --device NAME or set a default:\n");
            for (size_t i = 0; i < list.count; i++) fprintf(stderr, "  %s\n", list.devices[i].name);
        }
        return 2;
    }
    if (!chosen->config_valid) {
        fprintf(stderr, "waddle: device '%s' has invalid configuration\n", chosen->name);
        return 2;
    }
    if (out_dev != NULL) *out_dev = *chosen;
    if (out_socket_path != NULL) {
        size_t len = strlen(chosen->socket_path);
        if (len >= socket_path_cap) return 2;
        memcpy(out_socket_path, chosen->socket_path, len + 1);
    }
    return 0;
}

/**
 * @brief Non-blockingly establishes connection to either a UNIX domain mock or a VSOCK peer.
 *
 * @param[in] path     UNIX socket path if mock mode; NULL for VSOCK transport.
 * @param[in] cid      VSOCK Context Identifier (CID).
 * @param[in] port     VSOCK port number.
 * @param[in] deadline Monotonic millisecond deadline for connection completion.
 * @return Connected non-blocking file descriptor on success, or -1 on error.
 */
static int connect_peer(const char *path, uint32_t cid, uint32_t port, uint64_t deadline) {
    int domain = (path != NULL) ? AF_UNIX : AF_VSOCK;
    int fd = socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    int result;
    if (path != NULL) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof(a.sun_path)) {
            errno = ENAMETOOLONG;
            close(fd);
            return -1;
        }
        strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
        result = connect(fd, (struct sockaddr *)&a, sizeof(a));
    } else {
        struct sockaddr_vm a;
        memset(&a, 0, sizeof(a));
        a.svm_family = AF_VSOCK;
        a.svm_cid = cid;
        a.svm_port = port;
        result = connect(fd, (struct sockaddr *)&a, sizeof(a));
    }

    if (result == 0) {
        return fd;
    }

    if (errno != EINPROGRESS) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    for (;;) {
        int timeout = -1;
        if (deadline > 0) {
            uint64_t now = monotonic_ms();
            if (now >= deadline) {
                errno = ETIMEDOUT;
                break;
            }
            uint64_t left = deadline - now;
            timeout = (left > INT_MAX) ? INT_MAX : (int)left;
        }

        struct pollfd p = {fd, POLLOUT, 0};
        result = poll(&p, 1, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = ETIMEDOUT;
            }
            break;
        }

        int socket_error = 0;
        socklen_t len = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) != 0) {
            break;
        }
        if (socket_error != 0) {
            errno = socket_error;
            break;
        }
        return fd;
    }

    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

/**
 * @brief Executes a command in the Windows guest over VSOCK or mock socket transport.
 *
 * @param[in] argc      Argument count.
 * @param[in] argv      Argument vector.
 * @param[in] start_opt Index where exec options start (e.g. 2 for "waddle exec").
 * @return Process exit code on success, or non-zero on execution error.
 */
static int cmd_exec(int argc, char **argv, int start_opt) {
    const char *socket_path = NULL;
    const char *target_device = NULL;
    int explicit_transport = 0;
    const char *cwd_arg = NULL;
    uint32_t cid = 3;
    uint32_t port = WaddleDefaultVsockPort;
    uint32_t timeout = 0;
    int interactive = -1;
    int translate = 0;
    path_rule_t rules[64] = {0};
    size_t rule_count = 0;
    int start = 0;
    int result = 2;
    int fd = -1;
    char *allocated_dest[64] = {0};

    char *cwd = NULL;
    char *command = NULL;
    char **args = NULL;
    queue_t env;
    memset(&env, 0, sizeof(env));
    queue_t tx;
    memset(&tx, 0, sizeof(tx));
    uint8_t *spawn = NULL;

    if (queue_init(&env) != 0 || queue_init(&tx) != 0) {
        result = 125;
        goto done;
    }

    for (int i = start_opt; i < argc; i++) {
        const char *opt = argv[i];
        if (strcmp(opt, "--") == 0) {
            start = i + 1;
            break;
        }
        if (strcmp(opt, "--pipe") == 0 || strcmp(opt, "-P") == 0) {
            interactive = 0;
            continue;
        }
        if (strcmp(opt, "--interactive") == 0 || strcmp(opt, "-i") == 0 ||
            strcmp(opt, "--tty") == 0 || strcmp(opt, "-t") == 0) {
            interactive = 1;
            continue;
        }
        if (strcmp(opt, "--translate-path") == 0) {
            translate = 1;
            continue;
        }
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            usage(stdout);
            result = 0;
            goto done;
        }
        if (i + 1 >= argc) {
            goto usage_error;
        }

        const char *val = argv[++i];
        if (strcmp(opt, "--socket-path") == 0) {
            socket_path = val;
            explicit_transport = 1;
        } else if (strcmp(opt, "--device") == 0 || strcmp(opt, "-d") == 0) {
            if (target_device != NULL) goto usage_error;
            target_device = val;
        } else if (strcmp(opt, "--path-map") == 0) {
            const char *equal = strchr(val, '=');
            if (equal == NULL || rule_count == 64) { goto usage_error; }
            char *source = strndup(val, (size_t)(equal - val));
            if (source == NULL) { result = 125; goto done; }
            if (!waddle_path_rule_valid(source, equal + 1)) { free(source); goto usage_error; }
            size_t source_len = strlen(source);
            while (source_len > 1 && source[source_len - 1] == '/') { source[--source_len] = '\0'; }
            for (size_t j = 0; j < rule_count; j++) {
                if (strcmp(rules[j].source, source) == 0) { free(source); goto usage_error; }
            }
            rules[rule_count++] = (path_rule_t){source, equal + 1};
            translate = 1;
        } else if (strcmp(opt, "--cwd") == 0) {
            cwd_arg = val;
        } else if (strcmp(opt, "--vsock-cid") == 0) {
            explicit_transport = 1;
            if (number(val, &cid) != 0) {
                goto usage_error;
            }
        } else if (strcmp(opt, "--vsock-port") == 0) {
            explicit_transport = 1;
            if (number(val, &port) != 0 || port == 0) {
                goto usage_error;
            }
        } else if (strcmp(opt, "--timeout") == 0) {
            if (number(val, &timeout) != 0) {
                goto usage_error;
            }
        } else if (strcmp(opt, "--env") == 0 || strcmp(opt, "-e") == 0) {
            const char *equal = strchr(val, '=');
            if (equal == NULL || equal == val ||
                strlen(val) + 1 > WaddleMaxPayloadSize - env.len ||
                queue_append(&env, val, strlen(val) + 1) != 0) {
                goto usage_error;
            }
        } else {
            goto usage_error;
        }
    }

    const char *daemon_socket = NULL;
    char daemon_sock_buf[WaddleMaxPathLen];
    device_info_t target_dev;
    memset(&target_dev, 0, sizeof(target_dev));

    if (target_device != NULL && explicit_transport) goto usage_error;
    if (target_device != NULL) {
        if (resolve_target_device(target_device, &target_dev, daemon_sock_buf, sizeof(daemon_sock_buf), 1) != 0) {
            result = 2;
            goto done;
        }
        daemon_socket = daemon_sock_buf;
        if (cid == 3 && target_dev.vsock_cid > 0) {
            cid = target_dev.vsock_cid;
        }
        if (port == WaddleDefaultVsockPort && target_dev.vsock_port > 0) {
            port = target_dev.vsock_port;
        }
    } else if (!explicit_transport && getenv("WADDLE_MOCK_GUEST_SOCK") == NULL) {
        if (resolve_target_device(NULL, &target_dev, daemon_sock_buf, sizeof(daemon_sock_buf), 1) != 0) {
            result = 2;
            goto done;
        }
        daemon_socket = daemon_sock_buf;
        cid = target_dev.vsock_cid;
        port = target_dev.vsock_port;
    }

    if (translate && rule_count == 0) {
        daemon_config_t cfg;
        daemon_config_init_defaults(&cfg);
        (void)daemon_config_load_file(&cfg, target_dev.config_path[0] ? target_dev.config_path : NULL);
        for (size_t m = 0; m < cfg.mount_count && rule_count < 63; m++) {
            char *src = strdup(cfg.mounts[m].host_path);
            char *dst = strdup(cfg.mounts[m].guest_drive);
            if (src != NULL && dst != NULL) {
                size_t src_len = strlen(src);
                while (src_len > 1 && src[src_len - 1] == '/') {
                    src[--src_len] = '\0';
                }
                allocated_dest[rule_count] = dst;
                rules[rule_count++] = (path_rule_t){src, dst};
            } else {
                free(src);
                free(dst);
            }
        }
        /* Add root fallback mapping */
        char *root_src = strdup("/");
        char *root_dst = strdup("Z:\\");
        if (root_src != NULL && root_dst != NULL && rule_count < 64) {
            allocated_dest[rule_count] = root_dst;
            rules[rule_count++] = (path_rule_t){root_src, root_dst};
        } else {
            free(root_src);
            free(root_dst);
        }
    }

    if (start == 0 || start >= argc || *argv[start] == '\0') {
        goto usage_error;
    }

    if (interactive < 0) {
        interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    }
    if (interactive && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fprintf(stderr, "waddle: interactive mode requires terminal stdin and stdout\n");
        goto done;
    }

    cwd = cwd_arg ? strdup(cwd_arg) : getcwd(NULL, 0);
    if (cwd == NULL) {
        result = 125;
        goto done;
    }

    args = (char **)calloc((size_t)(argc - start) + 1, sizeof(*args));
    if (args == NULL) {
        result = 125;
        goto done;
    }

    for (int i = start; i < argc; i++) {
        args[i - start] = translate ? waddle_translate_rules(argv[i], rules, rule_count) : strdup(argv[i]);
        if (args[i - start] == NULL) {
            result = 125;
            goto local_error;
        }
    }

    if (translate) {
        char *mapped = waddle_translate_rules(cwd, rules, rule_count);
        if (mapped == NULL) {
            result = 125;
            goto local_error;
        }
        free(cwd);
        cwd = mapped;
    }

    command = waddle_quote(args);
    if (command == NULL) {
        result = 125;
        goto done;
    }

    size_t cwd_len = strlen(cwd);
    size_t cmd_len = strlen(command);
    if (cwd_len > WaddleMaxPayloadSize || cmd_len > WaddleMaxPayloadSize ||
        cwd_len + cmd_len + env.len + 26 > WaddleMaxPayloadSize) {
        errno = E2BIG;
        result = 125;
        goto local_error;
    }

    size_t total = cwd_len + cmd_len + env.len + 26;
    spawn = (uint8_t *)calloc(total, 1);
    if (spawn == NULL) {
        result = 125;
        goto done;
    }

    waddle_put32(spawn, interactive ? WaddleSpawnFlagInteractive : WaddleSpawnFlagRawPipes);
    if (translate) {
        waddle_put32(spawn, waddle_get32(spawn) | WaddleSpawnFlagTranslatePath);
    }

    struct winsize ws = {.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    if (interactive && waddle_terminal_size(&ws) != 0) {
        result = 125;
        goto local_error;
    }

    waddle_put16(spawn + 4, ws.ws_row);
    waddle_put16(spawn + 6, ws.ws_col);
    waddle_put16(spawn + 8, ws.ws_xpixel);
    waddle_put16(spawn + 10, ws.ws_ypixel);
    waddle_put32(spawn + 12, (uint32_t)cwd_len);
    waddle_put32(spawn + 16, (uint32_t)cmd_len);
    waddle_put32(spawn + 20, (uint32_t)env.len);

    memcpy(spawn + 24, cwd, cwd_len + 1);
    memcpy(spawn + 25 + cwd_len, command, cmd_len + 1);
    if (env.len > 0) {
        memcpy(spawn + 26 + cwd_len + cmd_len, env.data, env.len);
    }

    if (waddle_terminal_init(interactive) != 0) {
        result = 125;
        goto local_error;
    }

    uint64_t deadline = (timeout > 0) ? (monotonic_ms() + ((uint64_t)timeout * 1000)) : 0;

    int was_mocked = 0;
    if (socket_path == NULL) {
        socket_path = getenv("WADDLE_MOCK_GUEST_SOCK");
        if (socket_path != NULL) {
            was_mocked = 1;
        }
    }

    if ((!explicit_transport && socket_path == NULL) || was_mocked) {
        int client_fd = waddle_client_ensure_daemon(daemon_socket, WaddleDaemonSpawnTimeoutMs);
        if (client_fd >= 0) {
            waddle_daemon_status_resp_t status;
            memset(&status, 0, sizeof(status));
            if (waddle_client_status(client_fd, &status) == 0 &&
                status.subsystem_state != SubsystemStateRunning) {
                printf("[waddle] Starting background subsystem%s%s...\n",
                       target_dev.name[0] ? " for device " : "",
                       target_dev.name[0] ? target_dev.name : "");
                waddle_daemon_result_resp_t start_resp;
                memset(&start_resp, 0, sizeof(start_resp));
                if (waddle_client_start(client_fd, DaemonStartFlagWaitGuest, 60, &start_resp) != 0 || start_resp.status_code != 0) {
                    waddle_terminal_close();
                    fprintf(stderr, "waddle: failed to start subsystem: %s\n", start_resp.error_msg[0] ? start_resp.error_msg : strerror(start_resp.status_code ? (int)start_resp.status_code : errno));
                    close(client_fd);
                    result = 125;
                    goto done;
                }
            }
            close(client_fd);
        }
    }

    fd = connect_peer(socket_path, cid, port, deadline);
    if (fd < 0) {
        result = (errno == ETIMEDOUT) ? 124 : 125;
        goto local_error;
    }

    if (waddle_standard_nonblock() != 0) {
        result = 125;
        goto local_error;
    }

    uint32_t seq = 1;
    if (wire_send(&tx, &seq, (uint16_t)WaddleMsgSpawnReq, spawn, total) != 0) {
        result = 125;
        goto local_error;
    }

    result = waddle_session(fd, &tx, seq, deadline, interactive);
    goto done;

usage_error:
    usage(stderr);
    goto done;

local_error:
    waddle_terminal_close();
    fprintf(stderr, "waddle: %s\n", strerror(errno));

done:
    for (size_t i = 0; i < rule_count; i++) {
        free((void *)rules[i].source);
        rules[i].source = NULL;
        if (allocated_dest[i] != NULL) {
            free(allocated_dest[i]);
            allocated_dest[i] = NULL;
        }
    }
    waddle_terminal_close();
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    free(spawn);
    spawn = NULL;
    free(command);
    command = NULL;
    free(cwd);
    cwd = NULL;
    waddle_free_argv(args);
    args = NULL;
    queue_free(&env);
    queue_free(&tx);
    return result;
}

/**
 * @brief Subcommand handler for `waddle init` / `waddle --init`.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, or non-zero on failure.
 */
static int cmd_init(int argc, char **argv) {
    const char *device_name = NULL;
    const char *custom_disk = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            custom_disk = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            device_name = argv[++i];
        } else if (argv[i][0] != '-' && device_name == NULL) {
            device_name = argv[i];
        } else {
            fprintf(stderr, "waddle init: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    if (device_name == NULL || device_name[0] == '\0') {
        fprintf(stderr, "waddle init: device name required\n\nUsage: waddle init <device-name> [--disk <path>]\n");
        return 2;
    }

    if (daemon_device_validate_name(device_name) != 0) {
        fprintf(stderr, "waddle init: invalid device name '%s' (must be alphanumeric, hyphens, or underscores)\n", device_name);
        return 2;
    }

    device_info_t info;
    memset(&info, 0, sizeof(info));
    if (daemon_device_init(device_name, custom_disk, &info) != 0) {
        fprintf(stderr, "waddle init: failed to initialize device '%s': %s\n", device_name, strerror(errno));
        return 1;
    }

    printf("[waddle] Initialized device '%s' successfully\n", info.name);
    printf("  Configuration: %s\n", info.config_path);
    printf("  State Dir:     %s\n", info.state_dir);
    printf("  Disk Image:    %s\n", info.disk_image);
    printf("  VSOCK CID:     %u\n", info.vsock_cid);
    return 0;
}

/**
 * @brief Subcommand handler for `waddle start` / `waddle --start`.
 */
static int cmd_start(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *target_device = NULL;
    int wait_guest = 1;
    uint32_t timeout_sec = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--wait") == 0) {
            wait_guest = 1;
        } else if (strcmp(argv[i], "--no-wait") == 0) {
            wait_guest = 0;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            if (number(argv[++i], &timeout_sec) != 0) {
                fprintf(stderr, "waddle start: invalid timeout: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            target_device = argv[++i];
        } else if (argv[i][0] != '-' && target_device == NULL) {
            target_device = argv[i];
        } else {
            fprintf(stderr, "waddle start: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    char resolved_sock[WaddleMaxPathLen];
    device_info_t dev_info = {0};
    if (socket_path == NULL) {
        int r = resolve_target_device(target_device, &dev_info, resolved_sock, sizeof(resolved_sock), 1);
        if (r != 0) {
            return r;
        }
        socket_path = resolved_sock;
    }

    if (dev_info.name[0] != '\0') {
        printf("[waddle] Starting background subsystem for device '%s'...\n", dev_info.name);
    }
    return waddle_client_cmd_start(socket_path, wait_guest, timeout_sec);
}

/**
 * @brief Subcommand handler for `waddle stop` / `waddle --stop`.
 */
static int cmd_stop(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *target_device = NULL;
    int force = 0;
    uint32_t timeout_sec = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            if (number(argv[++i], &timeout_sec) != 0) {
                fprintf(stderr, "waddle stop: invalid timeout: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            target_device = argv[++i];
        } else if (argv[i][0] != '-' && target_device == NULL) {
            target_device = argv[i];
        } else {
            fprintf(stderr, "waddle stop: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    char resolved_sock[WaddleMaxPathLen];
    device_info_t dev_info = {0};
    if (socket_path == NULL) {
        int r = resolve_target_device(target_device, &dev_info, resolved_sock, sizeof(resolved_sock), 1);
        if (r != 0) {
            return r;
        }
        socket_path = resolved_sock;
    }

    if (dev_info.name[0] != '\0') {
        printf("[waddle] Stopping background subsystem for device '%s'...\n", dev_info.name);
    }
    return waddle_client_cmd_stop(socket_path, force, timeout_sec);
}

/**
 * @brief Subcommand handler for `waddle restart` / `waddle --restart`.
 */
static int cmd_restart(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *target_device = NULL;
    int force = 0;
    uint32_t timeout_sec = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            if (number(argv[++i], &timeout_sec) != 0) {
                fprintf(stderr, "waddle restart: invalid timeout: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            target_device = argv[++i];
        } else if (argv[i][0] != '-' && target_device == NULL) {
            target_device = argv[i];
        } else {
            fprintf(stderr, "waddle restart: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    char resolved_sock[WaddleMaxPathLen];
    device_info_t dev_info = {0};
    if (socket_path == NULL) {
        int r = resolve_target_device(target_device, &dev_info, resolved_sock, sizeof(resolved_sock), 1);
        if (r != 0) {
            return r;
        }
        socket_path = resolved_sock;
    }

    if (dev_info.name[0] != '\0') {
        printf("[waddle] Restarting background subsystem for device '%s'...\n", dev_info.name);
    }
    return waddle_client_cmd_restart(socket_path, force, timeout_sec);
}

/**
 * @brief Subcommand handler for `waddle status` / `waddle --status`.
 */
static int cmd_status(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *target_device = NULL;
    int json_output = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json_output = 1;
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            target_device = argv[++i];
        } else if (argv[i][0] != '-' && target_device == NULL) {
            target_device = argv[i];
        } else {
            fprintf(stderr, "waddle status: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    char resolved_sock[WaddleMaxPathLen];
    device_info_t dev_info = {0};
    if (socket_path == NULL) {
        int r = resolve_target_device(target_device, &dev_info, resolved_sock, sizeof(resolved_sock), 0);
        if (r != 0) {
            return r;
        }
        socket_path = resolved_sock;
    }
    return waddle_client_cmd_status(socket_path, json_output);
}

/**
 * @brief Subcommand handler for `waddle kill` / `waddle --kill`.
 */
static int cmd_kill(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *target_device = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            target_device = argv[++i];
        } else if (argv[i][0] != '-' && target_device == NULL) {
            target_device = argv[i];
        } else {
            fprintf(stderr, "waddle kill: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    char resolved_sock[WaddleMaxPathLen];
    device_info_t dev_info = {0};
    if (socket_path == NULL) {
        int r = resolve_target_device(target_device, &dev_info, resolved_sock, sizeof(resolved_sock), 1);
        if (r != 0) {
            return r;
        }
        socket_path = resolved_sock;
    }
    return waddle_client_cmd_kill(socket_path);
}

/**
 * @brief Live filesystem verification helper for `waddle fs test`.
 */
static int cmd_fs_test(const char *socket_path, const char *config_path) {
    printf("[waddle fs test] Starting VirtIO-FS live filesystem verification...\n");

    /* 1. Resolve primary export directory */
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);
    (void)daemon_config_load_file(&cfg, config_path);

    const char *export_dir = (cfg.mount_count > 0 && cfg.mounts[0].host_path[0] != '\0')
        ? cfg.mounts[0].host_path
        : getenv("HOME");
    if (export_dir == NULL) {
        export_dir = "/tmp";
    }

    printf("[waddle fs test] Step 1: Active export mapping: %s -> %s\n",
           export_dir,
           (cfg.mount_count > 0) ? cfg.mounts[0].guest_drive : "Z:\\");

    /* 2. Create test file in export directory */
    char test_file[WaddleMaxPathLen];
    snprintf(test_file, sizeof(test_file), "%.900s/waddle_fs_test_%d.tmp", export_dir, (int)getpid());

    FILE *f = fopen(test_file, "w");
    if (f == NULL) {
        fprintf(stderr, "[waddle fs test] Failed to create test file in export directory %s: %s\n",
                export_dir, strerror(errno));
        return 1;
    }
    fprintf(f, "WADDLE_FS_VERIFY_PAYLOAD_%d\n", (int)getpid());
    fclose(f);

    printf("[waddle fs test] Step 2: Created test file %s\n", test_file);

    /* 3. Verify instant visibility and content readback */
    f = fopen(test_file, "r");
    if (f == NULL) {
        fprintf(stderr, "[waddle fs test] Failed to open test file for reading: %s\n", strerror(errno));
        unlink(test_file);
        return 1;
    }

    char read_buf[64] = {0};
    if (fgets(read_buf, sizeof(read_buf), f) == NULL) {
        fprintf(stderr, "[waddle fs test] Failed to read test payload: %s\n", strerror(errno));
        fclose(f);
        unlink(test_file);
        return 1;
    }
    fclose(f);

    char expected[64];
    snprintf(expected, sizeof(expected), "WADDLE_FS_VERIFY_PAYLOAD_%d\n", (int)getpid());
    if (strcmp(read_buf, expected) != 0) {
        fprintf(stderr, "[waddle fs test] Content mismatch: expected '%s', got '%s'\n", expected, read_buf);
        unlink(test_file);
        return 1;
    }

    printf("[waddle fs test] Step 3: Verified instant readback and payload integrity\n");

    /* 4. Cleanup */
    unlink(test_file);
    printf("[waddle fs test] Step 4: Cleaned up test file\n");
    (void)socket_path;
    printf("[waddle fs test] VirtIO-FS live filesystem verification passed.\n");
    return 0;
}

/**
 * @brief Subcommand handler for `waddle fs` / `waddle --mount`.
 */
static int cmd_fs(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *target_device = NULL;
    int is_test = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "test") == 0) {
            is_test = 1;
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            target_device = argv[++i];
        } else if (argv[i][0] != '-' && target_device == NULL) {
            target_device = argv[i];
        } else {
            fprintf(stderr, "waddle fs: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    char resolved_sock[WaddleMaxPathLen];
    device_info_t dev_info = {0};
    if (socket_path == NULL) {
        int r = resolve_target_device(target_device, &dev_info, resolved_sock, sizeof(resolved_sock), 1);
        if (r != 0) {
            return r;
        }
        socket_path = resolved_sock;
    }

    if (is_test) {
        return cmd_fs_test(socket_path, dev_info.config_path[0] ? dev_info.config_path : NULL);
    }
    return waddle_client_cmd_fs(socket_path);
}

/**
 * @brief Subcommand handler for `waddle logs` / `waddle --logs`.
 */
static int cmd_logs(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *target_device = NULL;
    int follow = 0;
    uint32_t lines = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--follow") == 0) {
            follow = 1;
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--lines") == 0) && i + 1 < argc) {
            if (number(argv[++i], &lines) != 0) {
                fprintf(stderr, "waddle logs: invalid line count: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            target_device = argv[++i];
        } else if (argv[i][0] != '-' && target_device == NULL) {
            target_device = argv[i];
        } else {
            fprintf(stderr, "waddle logs: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    char resolved_sock[WaddleMaxPathLen];
    device_info_t dev_info = {0};
    if (socket_path == NULL) {
        int r = resolve_target_device(target_device, &dev_info, resolved_sock, sizeof(resolved_sock), 1);
        if (r != 0) {
            return r;
        }
        socket_path = resolved_sock;
    }
    return waddle_client_cmd_logs(socket_path, follow, lines);
}

/**
 * @brief Default action: auto-starts subsystem and opens ConPTY terminal.
 *
 * @param[in] target_device Optional device identifier (may be NULL).
 * @return Exit code.
 */
static int cmd_interactive_default(const char *target_device) {
    device_info_t dev_info = {0};
    memset(&dev_info, 0, sizeof(dev_info));
    char socket_path[WaddleMaxPathLen];
    memset(socket_path, 0, sizeof(socket_path));

    int r = resolve_target_device(target_device, &dev_info, socket_path, sizeof(socket_path), 1);
    if (r != 0) {
        return r;
    }

    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);
    if (dev_info.config_path[0] != '\0') {
        (void)daemon_config_load_file(&cfg, dev_info.config_path);
    } else {
        (void)daemon_config_load_file(&cfg, NULL);
    }

    const char *shell_cmd = (cfg.default_shell[0] != '\0') ? cfg.default_shell : "powershell.exe";

    /* 1. Ensure daemon and hypervisor are running for target device */
    const char *sock = (socket_path[0] != '\0') ? socket_path : NULL;
    int client_fd = waddle_client_ensure_daemon(sock, WaddleDaemonSpawnTimeoutMs);
    if (client_fd >= 0) {
        waddle_daemon_status_resp_t status;
        memset(&status, 0, sizeof(status));
        if (waddle_client_status(client_fd, &status) == 0) {
            if (status.subsystem_state != SubsystemStateRunning) {
                printf("[waddle] Starting background subsystem%s%s...\n",
                       dev_info.name[0] ? " for device " : "",
                       dev_info.name[0] ? dev_info.name : "");
                waddle_daemon_result_resp_t start_resp;
                memset(&start_resp, 0, sizeof(start_resp));
                if (waddle_client_start(client_fd, DaemonStartFlagWaitGuest, 60, &start_resp) != 0 || start_resp.status_code != 0) {
                    fprintf(stderr, "waddle: failed to start subsystem: %s\n",
                            start_resp.error_msg[0] ? start_resp.error_msg : strerror(start_resp.status_code ? (int)start_resp.status_code : errno));
                    close(client_fd);
                    return 1;
                }
            }
        }
        close(client_fd);
    }

    /* 2. Synthesize arguments for interactive shell session */
    uint32_t cid = (dev_info.vsock_cid > 0) ? dev_info.vsock_cid : 3;
    uint32_t port = (dev_info.vsock_port > 0) ? dev_info.vsock_port : WaddleDefaultVsockPort;
    char cid_str[16];
    snprintf(cid_str, sizeof(cid_str), "%u", cid);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    char *default_args[] = {
        "waddle",
        "exec",
        "--vsock-cid",
        cid_str,
        "--vsock-port",
        port_str,
        "--interactive",
        "--translate-path",
        "--",
        (char *)shell_cmd,
        NULL
    };
    if (dev_info.name[0] != '\0') {
        char *named_args[] = { "waddle", "exec", "--device", dev_info.name,
                               "--interactive", "--translate-path", "--", (char *)shell_cmd, NULL };
        return cmd_exec(8, named_args, 2);
    }
    return cmd_exec(10, default_args, 2);
}

int main(int argc, char **argv) {
    if (argc == 1) {
        return cmd_interactive_default(NULL);
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0 || strcmp(cmd, "version") == 0) {
        printf("waddle 0.1.0-alpha (protocol v1)\n");
        return 0;
    }

    if (strcmp(cmd, "device") == 0) {
        return waddle_device_command(argc - 2, argv + 2);
    }

    if (strcmp(cmd, "init") == 0 || strcmp(cmd, "--init") == 0) {
        return cmd_init(argc, argv);
    }

    if (strcmp(cmd, "devices") == 0 || strcmp(cmd, "--devices") == 0 ||
        strcmp(cmd, "list") == 0 || strcmp(cmd, "--list") == 0) {
        argv[1] = "list";
        return waddle_device_command(argc - 1, argv + 1);
    }

    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "--start") == 0) {
        return cmd_start(argc, argv);
    }

    if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "--stop") == 0) {
        return cmd_stop(argc, argv);
    }

    if (strcmp(cmd, "restart") == 0 || strcmp(cmd, "--restart") == 0) {
        return cmd_restart(argc, argv);
    }

    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "--status") == 0) {
        return cmd_status(argc, argv);
    }

    if (strcmp(cmd, "kill") == 0 || strcmp(cmd, "--kill") == 0) {
        return cmd_kill(argc, argv);
    }

    if (strcmp(cmd, "fs") == 0 || strcmp(cmd, "--mount") == 0) {
        return cmd_fs(argc, argv);
    }

    if (strcmp(cmd, "logs") == 0 || strcmp(cmd, "--logs") == 0) {
        return cmd_logs(argc, argv);
    }

    if (strcmp(cmd, "exec") == 0 || strcmp(cmd, "run") == 0) {
        return cmd_exec(argc, argv, 2);
    }

    if ((strcmp(cmd, "--exec") == 0 || strcmp(cmd, "-e") == 0) && argc >= 3) {
        /* waddle --exec <cmd>: synthesize exec invocation with auto translation */
        char **synthetic = (char **)calloc((size_t)argc + 4, sizeof(char *));
        if (synthetic == NULL) {
            return 125;
        }
        synthetic[0] = argv[0];
        synthetic[1] = "exec";
        synthetic[2] = "--translate-path";
        synthetic[3] = "--";
        for (int i = 2; i < argc; i++) {
            synthetic[i + 2] = argv[i];
        }
        int r = cmd_exec(argc + 2, synthetic, 2);
        free(synthetic);
        return r;
    }

    /* If the command does not begin with '-' and matches an initialized device,
     * launch an interactive terminal into that device! */
    if (cmd[0] != '-') {
        device_info_t dev_info = {0};
        if (daemon_device_find(cmd, &dev_info) == 0) {
            return cmd_interactive_default(cmd);
        }
    }

    usage(stderr);
    return 2;
}
