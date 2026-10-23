/**
 * @file daemon_qemu.h
 * @brief QEMU process lifecycle manager and argument builder.
 *
 * Provides argument construction, process spawning with pipe/file redirection,
 * QMP integration, graceful shutdown, and force termination for QEMU.
 */

#ifndef WaddleDaemonQemuH
#define WaddleDaemonQemuH

#include "daemon_config.h"
#include "daemon_qmp.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief QEMU argv pointer capacity, including the trailing NULL pointer.
 * @note The current maximum is 64 populated arguments; replacing one current
 *       VirtIO-FS group with eight groups adds 28, yielding 92 arguments plus
 *       the NULL terminator. Three slots remain for growth. Callers may pass a
 *       smaller test/runtime cap.
 */
#define QemuMaxArgs 96U

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
 * @brief Constructs QEMU argv using the compatibility identity `default`.
 *
 * Dynamically allocates every populated argument string. On failure, frees all
 * strings already appended and leaves argv[0] NULL when argv has capacity.
 * Successful arguments are independent heap strings owned by the caller until
 * qemu_free_args() is called. The function does not retain config or path data.
 * Safe to call concurrently with independent argument vectors.
 *
 * @param[in] config Non-null initialized configuration; the complete structure
 *                   is validated before any argument is retained.
 * @param[in] qmp_sock_path Non-null NUL-terminated absolute socket path shorter
 *                          than WaddleMaxPathLen.
 * @param[in] virtiofsd_sock_path Nullable NUL-terminated socket path shorter
 *                                than WaddleMaxPathLen; NULL/empty disables it.
 * @param[in] pid_file_path Nullable NUL-terminated path shorter than
 *                          WaddleMaxPathLen; NULL/empty disables it.
 * @param[out] argv Non-null caller-owned pointer array of max_args entries; on
 *                  success argv[count] is NULL. Must not alias input strings.
 * @param[in] max_args Pointer capacity including the trailing NULL slot; at
 *                     least 32 and no greater than the actual argv array size.
 * @return Nonnegative argument count on success, or -1 with errno set to EINVAL,
 *         E2BIG, or ENOMEM. On every error no argument strings remain owned by
 *         the caller and argv[0] is NULL.
 * @thread_safety Concurrent calls are safe when each call has independent argv.
 */
int qemu_build_args(const daemon_config_t *config,
                    const char *qmp_sock_path,
                    const char *virtiofsd_sock_path,
                    const char *pid_file_path,
                    char **argv,
                    size_t max_args);

/**
 * @brief Build QEMU arguments with a device-specific PipeWire discard target.
 * @param[in] config Non-null initialized, validated configuration; borrowed for
 *                   this call and not retained.
 * @param[in] device_name Borrowed NUL-terminated identifier using ASCII letters,
 *                        digits, hyphen or underscore; length is 1..63 bytes.
 * @param[in] qmp_sock_path Non-null NUL-terminated socket path shorter than
 *                          WaddleMaxPathLen.
 * @param[in] virtiofsd_sock_path Nullable path shorter than WaddleMaxPathLen;
 *                                NULL/empty omits the VirtIO-FS backend.
 * @param[in] pid_file_path Nullable path shorter than WaddleMaxPathLen;
 *                          NULL/empty omits the PID file argument.
 * @param[out] argv Caller-owned pointer array of max_args entries, disjoint
 *                  from every input; success includes a trailing NULL.
 * @param[in] max_args Pointer capacity including the trailing NULL, at least 32.
 * @return Argument count on success, or -1 with errno set; failure frees every
 *         partially built argument and leaves argv[0] NULL.
 * @thread_safety Safe for concurrent calls using independent argv arrays.
 */
int qemu_build_args_for_device(const daemon_config_t *config,
                               const char *device_name,
                               const char *qmp_sock_path,
                               const char *virtiofsd_sock_path,
                               const char *pid_file_path,
                               char **argv,
                               size_t max_args);

/**
 * @brief Releases dynamic argument strings created by qemu_build_args.
 *
 * @param[in,out] argv Nullable caller-owned argument vector; each freed entry
 *                     becomes NULL. No-op for NULL.
 * @param[in] argc Number of valid heap strings to release; must be the exact
 *                 nonnegative count returned by qemu_build_args().
 * @return No error result. Does not access or free strings beyond argc.
 * @thread_safety Safe for independent vectors; callers synchronize shared ones.
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
 * @brief Spawns QEMU using the compatibility identity `default`.
 *
 * Redirects stdout and stderr to the specified log file.
 *
 * @param[out] proc Non-null caller-owned process record initialized on entry.
 * @param[in] config Non-null initialized configuration, borrowed only.
 * @param[in] qmp_sock_path Non-null NUL-terminated socket path shorter than
 *                          WaddleMaxPathLen.
 * @param[in] virtiofsd_sock_path Nullable socket path shorter than
 *                                WaddleMaxPathLen.
 * @param[in] log_path Non-null NUL-terminated path opened append/create 0644.
 * @param[in] qemu_binary Nullable executable path/name; NULL/empty uses lookup.
 * @return 0 after successful fork, or -1 with errno set. No input is retained.
 * @thread_safety Calls require independent process records and output paths.
 */
int qemu_spawn(qemu_process_t *proc,
               const daemon_config_t *config,
               const char *qmp_sock_path,
               const char *virtiofsd_sock_path,
               const char *log_path,
               const char *qemu_binary);

/**
 * @brief Spawn QEMU with the target identity used by the matching session sink.
 * @param[out] proc Non-null caller-owned process record initialized on entry.
 * @param[in] config Non-null initialized validated configuration, borrowed only.
 * @param[in] device_name Borrowed valid ASCII device identifier, 1..63 bytes.
 * @param[in] qmp_sock_path Non-null path shorter than WaddleMaxPathLen.
 * @param[in] virtiofsd_sock_path Nullable path shorter than WaddleMaxPathLen.
 * @param[in] log_path Non-null NUL-terminated path; opened append/create 0644.
 * @param[in] qemu_binary Nullable executable path/name; NULL/empty triggers
 *                        bundled/system lookup.
 * @return 0 after successful fork, or -1 with errno set. Child and parent own
 *         only their respective process/descriptor state; no input is retained.
 * @thread_safety Each call requires an independent proc and output paths.
 */
int qemu_spawn_for_device(qemu_process_t *proc,
                          const daemon_config_t *config,
                          const char *device_name,
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

#endif /* WaddleDaemonQemuH */
