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
 * @brief One ordered VirtIO-FS backend and its future guest drive binding.
 * @note Fixed arrays are borrowed for the call only. `socket_path` must hold
 *       an absolute NUL-terminated path shorter than WaddleMaxPathLen;
 *       `guest_drive` must be `D:\\` or `D:/` plus NUL. New array APIs reject
 *       commas and control bytes in socket paths because QEMU keyval paths
 *       cannot safely carry them unescaped.
 */
typedef struct qemu_virtiofs_mount_t {
    /** @brief Absolute host AF_UNIX socket path owned by the caller. */
    char socket_path[WaddleMaxPathLen];
    /** @brief One ASCII guest drive letter, colon, slash/backslash and NUL. */
    char guest_drive[4];
} qemu_virtiofs_mount_t;

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
 * @brief Build QEMU argv with zero or more drive-tagged VirtIO-FS mounts.
 *
 * Each mount adds exactly one chardev/device pair. Its drive letter is
 * normalized to lowercase for stable IDs: `Z:\\` produces chardev
 * `waddle_fs_char_z`, device `waddle_fs_dev_z`, and tag `waddle_fs_z`.
 * Mount order is preserved. No guest drive binding or mount lifecycle is
 * performed here. Unlike the legacy single-socket API, count zero requires a
 * NULL array and counts 1..WaddleMaxMounts require a non-NULL array.
 *
 * @param[in] config Valid initialized config, borrowed for this call.
 * @param[in] device_name Valid 1..63 byte ASCII registry identity.
 * @param[in] qmp_sock_path Absolute bounded QMP socket path; commas/control
 *                          bytes are rejected as unsafe QEMU keyval data.
 * @param[in] mounts Ordered borrowed mount array, NULL iff mount_count is zero.
 *                    Paths must be absolute, bounded, comma/control-free and
 *                    unique; drive letters must be unique case-insensitively.
 * @param[in] mount_count Exact number of mounts; never truncated.
 * @param[in] pid_file_path Nullable bounded PID path; NULL/empty omits it.
 * @param[out] argv Caller-owned disjoint pointer array; success ends in NULL.
 * @param[in] max_args Pointer capacity including trailing NULL, at least 32.
 * @return Argument count, or -1 with errno EINVAL, E2BIG, or ENOMEM. Failure
 *         frees all partial strings and leaves argv[0] NULL.
 * @thread_safety Concurrent calls are safe with independent argv arrays.
 */
int qemu_build_args_for_device_mounts(const daemon_config_t *config,
                                      const char *device_name,
                                      const char *qmp_sock_path,
                                      const qemu_virtiofs_mount_t *mounts,
                                      size_t mount_count,
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
 * @brief Spawn QEMU using the drive-tagged multi-mount argv builder.
 *
 * Opens log_path append/create 0644, forks and execs QEMU using the same
 * binary lookup as qemu_spawn_for_device(). Mount records and their strings
 * are borrowed only while building argv; no mount process/socket is created,
 * retained, stopped or reaped here. The legacy single-socket field in proc is
 * left empty because the multi-mount process has no single socket identity.
 *
 * @param[out] proc Non-NULL caller-owned process record initialized on entry.
 * @param[in] config Non-NULL initialized valid config, borrowed only.
 * @param[in] device_name Valid named-device identity (1..63 ASCII bytes).
 * @param[in] qmp_sock_path Absolute bounded QMP socket path without commas or
 *                          control bytes.
 * @param[in] mounts Ordered borrowed array, NULL iff mount_count is zero.
 * @param[in] mount_count Exact count in 0..WaddleMaxMounts; never truncated.
 * @param[in] log_path Non-NULL path opened append/create 0644.
 * @param[in] qemu_binary Nullable explicit executable; NULL/empty uses lookup.
 * @return 0 after successful fork, or -1 with errno; no mount resources are
 *         changed. Failure closes opened descriptors and frees argv strings.
 * @thread_safety Requires an independent process record and output paths.
 */
int qemu_spawn_for_device_mounts(qemu_process_t *proc,
                                 const daemon_config_t *config,
                                 const char *device_name,
                                 const char *qmp_sock_path,
                                 const qemu_virtiofs_mount_t *mounts,
                                 size_t mount_count,
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
 * @brief Kill and reap one owned QEMU child without losing failed ownership.
 * @param[in,out] proc Non-NULL caller-owned initialized record, externally serialized.
 * @return 0 after confirmed reap/ECHILD or no child; -1 with errno on failure.
 * @note Retries EINTR. Kill/wait errors preserve PID, running flag, QMP and log
 *       ownership for retry. No borrowed input is retained or heap allocated.
 */
int qemu_kill(qemu_process_t *proc);

/**
 * @brief Close resources only after an owned child is confirmed dead.
 * @param[in,out] proc Nullable caller-owned initialized record; NULL is a no-op.
 * @return No result. A live or unprobeable child preserves all resources and
 *         sets errno EBUSY or the polling error. Use qemu_kill before retrying.
 * @note Independent records are thread-safe; shared records require external
 *       serialization. Closes the owned log/QMP descriptors only after reap.
 */
void qemu_cleanup(qemu_process_t *proc);

#ifdef __cplusplus
}
#endif

#endif /* WaddleDaemonQemuH */
