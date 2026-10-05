/**
 * @file daemon_client.h
 * @brief Host CLI client library for interacting with the Waddle background supervisor daemon.
 *
 * This header defines C client primitives and high-level commands for querying daemon status,
 * starting, stopping, restarting, killing, viewing logs, and inspecting VirtIO-FS filesystem
 * mounts over the local UNIX domain control socket.
 */

#ifndef WADDLE_DAEMON_CLIENT_H
#define WADDLE_DAEMON_CLIENT_H

#include "waddle/daemon_protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default client timeout for synchronous requests in milliseconds (10 seconds). */
#define WaddleClientDefaultTimeoutMs UINT32_C(10000)

/** @brief Default timeout when waiting for daemon socket to become ready after spawn (5 seconds). */
#define WaddleDaemonSpawnTimeoutMs UINT32_C(5000)

/**
 * @brief Resolves the default UNIX domain socket path for the Waddle daemon.
 *
 * Follows XDG Base Directory specification: checks $XDG_RUNTIME_DIR/waddle/daemon.sock,
 * falling back to $HOME/.local/state/waddle/run/daemon.sock or /tmp/waddle-<uid>/daemon.sock.
 *
 * @param[out] buf     Destination character buffer for NUL-terminated path.
 * @param[in]  buf_len Size of destination buffer in bytes.
 * @return 0 on success, or -1 on buffer overflow or resolution failure (errno set).
 */
int waddle_client_default_socket_path(char *buf, size_t buf_len);

/**
 * @brief Resolves the default runtime directory for Waddle daemon artifacts.
 *
 * @param[out] buf     Destination character buffer for NUL-terminated path.
 * @param[in]  buf_len Size of destination buffer in bytes.
 * @return 0 on success, or -1 on buffer overflow or resolution failure (errno set).
 */
int waddle_client_default_runtime_dir(char *buf, size_t buf_len);

/**
 * @brief Resolves the default log directory for Waddle daemon and hypervisor logs.
 *
 * @param[out] buf     Destination character buffer for NUL-terminated path.
 * @param[in]  buf_len Size of destination buffer in bytes.
 * @return 0 on success, or -1 on buffer overflow or resolution failure (errno set).
 */
int waddle_client_default_log_dir(char *buf, size_t buf_len);

/**
 * @brief Connects to the Waddle supervisor daemon UNIX domain socket.
 *
 * @param[in] socket_path Path to daemon UNIX domain socket, or NULL to use default.
 * @return Connected non-blocking socket file descriptor on success, or -1 on failure (errno set).
 */
int waddle_client_connect(const char *socket_path);

/**
 * @brief Checks whether the Waddle supervisor daemon is active and responsive.
 *
 * @param[in] socket_path Path to daemon UNIX domain socket, or NULL to use default.
 * @return 1 if daemon is running and responsive, 0 otherwise.
 */
int waddle_client_is_alive(const char *socket_path);

/**
 * @brief Spawns the waddled supervisor daemon in the background if not running.
 *
 * Searches for waddled in WADDLE_DAEMON_BIN, relative to the calling binary directory,
 * or in PATH.
 *
 * @param[in] runtime_dir Custom runtime directory, or NULL to use default.
 * @return PID of the spawned daemon process on success, or -1 on failure (errno set).
 */
pid_t waddle_client_spawn_daemon(const char *runtime_dir);

/**
 * @brief Ensures the Waddle daemon is running, spawning it if absent or unresponsive.
 *
 * @param[in] socket_path Path to daemon UNIX domain socket, or NULL to use default.
 * @param[in] timeout_ms  Maximum wait time in milliseconds for socket to become ready.
 * @return Connected socket file descriptor on success, or -1 on failure (errno set).
 */
int waddle_client_ensure_daemon(const char *socket_path, uint32_t timeout_ms);

/**
 * @brief Sends a Start request to the daemon and receives the result.
 *
 * @param[in]  fd          Connected daemon client socket descriptor.
 * @param[in]  flags       Start flags bitmask (waddle_daemon_start_flags_t).
 * @param[in]  timeout_sec Timeout in seconds for VM/guest readiness (0 for default).
 * @param[out] resp        Destination pointer for result response.
 * @return 0 on communication success, or -1 on socket or protocol failure (errno set).
 */
int waddle_client_start(int fd,
                        uint32_t flags,
                        uint32_t timeout_sec,
                        waddle_daemon_result_resp_t *resp);

/**
 * @brief Sends a Stop request to the daemon and receives the result.
 *
 * @param[in]  fd          Connected daemon client socket descriptor.
 * @param[in]  force       1 immediate SIGKILL, 0 ACPI with fallback, WaddleStopGracefulOnly ACPI without fallback.
 * @param[in]  timeout_sec Timeout in seconds before force kill fallback (0 for default).
 * @param[out] resp        Destination pointer for result response.
 * @return 0 on communication success, or -1 on socket or protocol failure (errno set).
 */
int waddle_client_stop(int fd,
                       uint32_t force,
                       uint32_t timeout_sec,
                       waddle_daemon_result_resp_t *resp);

/**
 * @brief Queries comprehensive subsystem status from the daemon.
 *
 * @param[in]  fd   Connected daemon client socket descriptor.
 * @param[out] resp Destination pointer for status response.
 * @return 0 on communication success, or -1 on socket or protocol failure (errno set).
 */
int waddle_client_status(int fd, waddle_daemon_status_resp_t *resp);

/**
 * @brief Request supervisor exit after all subsystem children are stopped and reaped.
 * @param[in] fd Borrowed connected socket, nonnegative; caller closes it.
 * @param[out] resp Non-null borrowed result destination; status_code may be EBUSY.
 * @return 0 for a complete validated response, -1 with errno for transport/protocol error.
 * @note No allocation; single caller per socket. Reply precedes disconnect and lease release.
 * Older supervisors may return ENOTSUP; callers must leave their lease intact.
 */
int waddle_client_shutdown(int fd, waddle_daemon_result_resp_t *resp);

/** @brief Wait until the supervisor socket is removed and its stable lease is free.
 * @param[in] socket_path Borrowed explicit path or NULL for legacy transport.
 * @param[in] timeout_sec Deadline seconds, 0 selects 15 seconds.
 * @return 0 fully exited, -1 errno ETIMEDOUT/EINTR/EACCES or I/O.
 * @note No heap allocation or retained fds; single caller, never unlinks lock files.
 */
int waddle_client_wait_supervisor_exit(const char *socket_path, uint32_t timeout_sec);


/**
 * @brief Sends a Kill request to the daemon to force-terminate all processes.
 *
 * @param[in]  fd   Connected daemon client socket descriptor.
 * @param[out] resp Destination pointer for result response.
 * @return 0 on communication success, or -1 on socket or protocol failure (errno set).
 */
int waddle_client_kill(int fd, waddle_daemon_result_resp_t *resp);

/**
 * @brief Queries active VirtIO-FS shared filesystem mounts from the daemon.
 *
 * @param[in]  fd   Connected daemon client socket descriptor.
 * @param[out] resp Destination pointer for filesystem mounts list response.
 * @return 0 on communication success, or -1 on socket or protocol failure (errno set).
 */
int waddle_client_fs_list(int fd, waddle_daemon_fs_list_resp_t *resp);

/**
 * @brief Queries recent log lines from the daemon.
 *
 * @param[in]  fd      Connected daemon client socket descriptor.
 * @param[in]  target  Log target: 0 = all, 1 = daemon, 2 = QEMU, 3 = virtiofsd.
 * @param[in]  lines   Number of lines to query (0 for default).
 * @param[out] buf     Destination character buffer for log contents.
 * @param[in]  buf_len Capacity of destination buffer in bytes.
 * @param[out] out_len Pointer receiving number of bytes copied into buf.
 * @return 0 on communication success, or -1 on socket or protocol failure (errno set).
 */
int waddle_client_logs(int fd,
                       uint32_t target,
                       uint32_t lines,
                       char *buf,
                       size_t buf_len,
                       size_t *out_len);

/**
 * @brief High-level CLI command implementation for `waddle start` / `waddle --start`.
 *
 * @param[in] socket_path Custom socket path, or NULL to use default.
 * @param[in] wait_guest  1 to wait for guest agent readiness, 0 to return immediately.
 * @param[in] timeout_sec Timeout in seconds for startup.
 * @return 0 on successful start, or non-zero POSIX exit code on error.
 */
int waddle_client_cmd_start(const char *socket_path, int wait_guest, uint32_t timeout_sec);

/**
 * @brief High-level CLI command implementation for `waddle stop` / `waddle --stop`.
 *
 * @param[in] socket_path Custom socket path, or NULL to use default.
 * @param[in] force       1 force kill, 0 ACPI with fallback, WaddleStopGracefulOnly preserves VM on timeout.
 * @param[in] timeout_sec Timeout in seconds before force kill fallback.
 * @return 0 stopped, nonzero on communication/stop/supervisor failure; graceful-only
 * timeout leaves the VM and its resources active and never proceeds to restart.
 * @note Borrowed path, no retained data; caller thread owns/reaps any spawned
 * supervisor through the existing lifecycle. Calls are serialized by the caller.
 */
int waddle_client_cmd_stop(const char *socket_path, int force, uint32_t timeout_sec);

/**
 * @brief High-level CLI command implementation for `waddle restart` / `waddle --restart`.
 *
 * @param[in] socket_path Custom socket path, or NULL to use default.
 * @param[in] force       1 force kill, 0 ACPI with fallback, WaddleStopGracefulOnly preserves VM on timeout.
 * @param[in] timeout_sec Timeout in seconds.
 * @return 0 restarted and guest ready, nonzero on stop/start/readiness failure.
 * @note Borrowed path, no retained data. Caller thread serializes lifecycle calls;
 * graceful-only stop failure does not start another VM or free live resources.
 */
int waddle_client_cmd_restart(const char *socket_path, int force, uint32_t timeout_sec);

/**
 * @brief High-level CLI command implementation for `waddle status` / `waddle --status`.
 *
 * @param[in] socket_path Custom socket path, or NULL to use default.
 * @param[in] json_output 1 for machine-readable JSON, 0 for formatted human text.
 * @return 0 on success, or non-zero POSIX exit code on error.
 */
int waddle_client_cmd_status(const char *socket_path, int json_output);

/**
 * @brief High-level CLI command implementation for `waddle kill` / `waddle --kill`.
 *
 * Forcefully terminates all daemon and hypervisor processes and cleans stale socket files.
 *
 * @param[in] socket_path Custom socket path, or NULL to use default.
 * @return 0 on success, or non-zero POSIX exit code on error.
 */
int waddle_client_cmd_kill(const char *socket_path);

/**
 * @brief High-level CLI command implementation for `waddle fs` / `waddle --mount`.
 *
 * Displays active VirtIO-FS shared directory mappings.
 *
 * @param[in] socket_path Custom socket path, or NULL to use default.
 * @return 0 on success, or non-zero POSIX exit code on error.
 */
int waddle_client_cmd_fs(const char *socket_path);

/**
 * @brief High-level CLI command implementation for `waddle logs` / `waddle --logs`.
 *
 * Displays supervisor, QEMU, or virtiofsd logs with optional tailing.
 *
 * @param[in] socket_path Custom socket path, or NULL to use default.
 * @param[in] follow      1 to follow/tail log output continuously, 0 to print and exit.
 * @param[in] lines       Number of lines to display (0 for default 50).
 * @return 0 on success, or non-zero POSIX exit code on error.
 */
int waddle_client_cmd_logs(const char *socket_path, int follow, uint32_t lines);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_CLIENT_H */
