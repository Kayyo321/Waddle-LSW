/**
 * @file daemon_qemu.h
 * @brief QEMU process lifecycle manager and argument builder.
 *
 * Provides argument construction, process spawning with pipe/file redirection,
 * QMP integration, graceful shutdown, and force termination for QEMU.
 */

#ifndef WADDLE_DAEMON_QEMU_H
#define WADDLE_DAEMON_QEMU_H

#include "daemon_config.h"
#include "daemon_qmp.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of command-line arguments supported for QEMU invocation. */
#define QemuMaxArgs 80U

/**
 * @brief State structure for a managed QEMU process instance.
 */
typedef struct qemu_process_t {
    /** @brief Process ID of spawned QEMU process (0 if not running). */
    pid_t pid;
    /** @brief UNIX socket path for QMP monitor. */
    char qmp_sock_path[WaddleMaxPathLen];
    /** @brief UNIX socket path for VirtIO-FS vhost-user backend. */
    char virtiofsd_sock_path[WaddleMaxPathLen];
    /** @brief File path for QEMU stdout/stderr logging. */
    char log_path[WaddleMaxPathLen];
    /** @brief Open log file descriptor (-1 if closed). */
    int log_fd;
    /** @brief QMP control client session. */
    qmp_client_t qmp;
    /** @brief 1 if process is actively executing, 0 if terminated. */
    int is_running;
} qemu_process_t;

/**
 * @brief Constructs the argument vector for spawning QEMU.
 *
 * Dynamically allocates argument strings into argv. Caller must free populated
 * strings via free() or use qemu_free_args().
 *
 * @param[in]  config               Subsystem configuration.
 * @param[in]  qmp_sock_path        Path to QMP UNIX socket.
 * @param[in]  virtiofsd_sock_path  Path to virtiofsd UNIX socket (nullable if disabled).
 * @param[in]  pid_file_path        Path to QEMU PID file (nullable).
 * @param[out] argv                 Array of char* pointers of capacity at least QemuMaxArgs.
 * @param[in]  max_args             Capacity of argv array.
 * @return Number of arguments populated in argv (excluding trailing NULL), or -1 on error.
 */
int qemu_build_args(const daemon_config_t *config,
                    const char *qmp_sock_path,
                    const char *virtiofsd_sock_path,
                    const char *pid_file_path,
                    char **argv,
                    size_t max_args);

/**
 * @brief Releases dynamic argument strings created by qemu_build_args.
 *
 * @param[in,out] argv Array of argument strings to free.
 * @param[in]     argc Number of valid argument strings in argv.
 */
void qemu_free_args(char **argv, size_t argc);

/**
 * @brief Locates the QEMU emulator binary, checking relative vendor paths first.
 *
 * Resolves the directory of the currently running executable via /proc/self/exe
 * and inspects the bundled vendor directory (<exe_dir>/vendor/qemu-system-x86_64)
 * before falling back to system directories and PATH.
 *
 * @param[out] out_path Buffer of capacity at least WaddleMaxPathLen.
 * @param[in]  path_cap Capacity of out_path buffer in bytes.
 * @return 0 on success, or -1 if QEMU cannot be found.
 */
int qemu_find_binary(char *out_path, size_t path_cap);

/**
 * @brief Spawns the QEMU hypervisor process.
 *
 * Redirects stdout and stderr to the specified log file.
 *
 * @param[out] proc                 Non-null pointer to qemu_process_t to initialize.
 * @param[in]  config               Subsystem configuration.
 * @param[in]  qmp_sock_path        Path to QMP socket.
 * @param[in]  virtiofsd_sock_path  Path to virtiofsd socket (nullable).
 * @param[in]  log_path             Path to log file for stdout/stderr.
 * @param[in]  qemu_binary          Path or name of QEMU binary (NULL for "qemu-system-x86_64").
 * @return 0 on success, or -1 on spawn failure (errno set).
 */
int qemu_spawn(qemu_process_t *proc,
               const daemon_config_t *config,
               const char *qmp_sock_path,
               const char *virtiofsd_sock_path,
               const char *log_path,
               const char *qemu_binary);

/**
 * @brief Checks if the QEMU process is currently running.
 *
 * Non-blockingly calls waitpid to reap and update proc->is_running.
 *
 * @param[in,out] proc Non-null pointer to qemu_process_t.
 * @return 1 if running, 0 if terminated, or -1 on error.
 */
int qemu_poll_status(qemu_process_t *proc);

/**
 * @brief Issues graceful shutdown via QMP system_powerdown.
 *
 * @param[in,out] proc Non-null pointer to qemu_process_t.
 * @return 0 on success, or -1 on error.
 */
int qemu_shutdown_graceful(qemu_process_t *proc);

/**
 * @brief Forcefully terminates the QEMU process via SIGKILL.
 *
 * @param[in,out] proc Non-null pointer to qemu_process_t.
 * @return 0 on success, or -1 on error.
 */
int qemu_kill(qemu_process_t *proc);

/**
 * @brief Cleans up resources, reaps child process if needed, and closes descriptors.
 *
 * @param[in,out] proc Pointer to qemu_process_t.
 */
void qemu_cleanup(qemu_process_t *proc);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_QEMU_H */
