/**
 * @file daemon_fs.h
 * @brief VirtIO-FS (virtiofsd) lifecycle manager and filesystem path translation bridge.
 *
 * Provides virtiofsd binary discovery, argument vector building, process lifecycle
 * supervision, socket readiness polling, and integration with path translation rules.
 */

#ifndef WADDLE_DAEMON_FS_H
#define WADDLE_DAEMON_FS_H

#include "daemon_config.h"
#include "path_rules.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of command-line arguments for virtiofsd. */
#define VirtiofsMaxArgs UINT32_C(32)

/** @brief Default timeout in milliseconds waiting for virtiofsd socket creation. */
#define VirtiofsDefaultWaitMs UINT32_C(3000)

/**
 * @brief State structure for managed virtiofsd process instance.
 */
typedef struct virtiofs_process_t {
    /** @brief Process ID of spawned virtiofsd process (0 if not running). */
    pid_t pid;
    /** @brief UNIX socket path for vhost-user IPC. */
    char socket_path[WaddleMaxPathLen];
    /** @brief Log file path for stdout/stderr redirection. */
    char log_path[WaddleMaxPathLen];
    /** @brief Host directory path exported by this instance. */
    char shared_dir[WaddleMaxPathLen];
    /** @brief Open log file descriptor (-1 if closed). */
    int log_fd;
    /** @brief 1 if process is actively executing, 0 if terminated. */
    int is_running;
} virtiofs_process_t;

/**
 * @brief Locates the virtiofsd binary on the host system.
 *
 * Resolves the directory of the currently running executable via /proc/self/exe
 * and checks the bundled vendor directory (<exe_dir>/vendor/virtiofsd) before
 * falling back to standard system directories (/usr/libexec/virtiofsd,
 * /usr/lib/qemu/virtiofsd, etc.) and searching the PATH environment variable.
 *
 * @param[out] out_path Buffer of capacity at least WaddleMaxPathLen.
 * @param[in]  path_cap Capacity of out_path buffer in bytes.
 * @return 0 on success, or -1 if virtiofsd cannot be found.
 */
int daemon_fs_find_binary(char *out_path, size_t path_cap);

/**
 * @brief Constructs argument vector for virtiofsd.
 *
 * @param[in]  socket_path  Path to vhost-user UNIX socket.
 * @param[in]  shared_dir   Host directory to export.
 * @param[in]  sandbox_mode Sandbox mode (e.g. "none" or "auto").
 * @param[in]  cache_mode   Cache policy (e.g. "auto", "always", "none").
 * @param[out] argv         Array of char* pointers of capacity at least VirtiofsMaxArgs.
 * @param[in]  max_args     Capacity of argv array.
 * @return Number of arguments populated, or -1 on error.
 */
int daemon_fs_build_args(const char *socket_path,
                         const char *shared_dir,
                         const char *sandbox_mode,
                         const char *cache_mode,
                         char **argv,
                         size_t max_args);

/**
 * @brief Releases argument vector allocated by daemon_fs_build_args.
 *
 * @param[in,out] argv Array of argument strings.
 * @param[in]     argc Number of valid argument strings.
 */
void daemon_fs_free_args(char **argv, size_t argc);

/**
 * @brief Spawns virtiofsd and waits for its vhost-user UNIX socket to become ready.
 *
 * @param[out] proc         Non-null pointer to virtiofs_process_t to initialize.
 * @param[in]  socket_path  Destination UNIX socket path.
 * @param[in]  shared_dir   Host directory to export.
 * @param[in]  log_path     Log file path for stdout/stderr.
 * @param[in]  binary_path  Path to virtiofsd binary (NULL to auto-detect).
 * @param[in]  timeout_ms   Maximum time to wait for socket availability.
 * @return 0 on success, or -1 on failure.
 */
int daemon_fs_spawn(virtiofs_process_t *proc,
                    const char *socket_path,
                    const char *shared_dir,
                    const char *log_path,
                    const char *binary_path,
                    uint32_t timeout_ms);

/**
 * @brief Checks if virtiofsd is running and reaps child if terminated.
 *
 * @param[in,out] proc virtiofs process handle.
 * @return 1 if running, 0 if terminated, or -1 on error.
 */
int daemon_fs_poll_status(virtiofs_process_t *proc);

/**
 * @brief Gracefully terminates virtiofsd via SIGTERM and reaps it.
 *
 * @param[in,out] proc       virtiofs process handle.
 * @param[in]     timeout_ms Timeout before SIGKILL fallback.
 * @return 0 on success, or -1 on error.
 */
int daemon_fs_stop(virtiofs_process_t *proc, uint32_t timeout_ms);

/**
 * @brief Converts daemon mount descriptors to path_rule_t rules for path translation.
 *
 * @param[in]  mounts      Array of waddle_daemon_fs_mount_t descriptors.
 * @param[in]  mount_count Number of mounts in array.
 * @param[out] rules       Destination array of path_rule_t.
 * @param[in]  max_rules   Capacity of rules array.
 * @return Number of rules populated, or -1 on error.
 */
int daemon_fs_mounts_to_rules(const waddle_daemon_fs_mount_t *mounts,
                              size_t mount_count,
                              path_rule_t *rules,
                              size_t max_rules);

/**
 * @brief Translates a host POSIX path to a guest Windows path using active daemon config.
 *
 * @param[in] path   POSIX path to translate.
 * @param[in] config Active daemon configuration.
 * @return Malloc-allocated translated string (caller frees via free()), or NULL on error.
 */
char *daemon_fs_translate_path(const char *path, const daemon_config_t *config);

/**
 * @brief Cleans up virtiofsd process, socket file, and descriptors.
 *
 * @param[in,out] proc virtiofs process handle.
 */
void daemon_fs_cleanup(virtiofs_process_t *proc);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_FS_H */
