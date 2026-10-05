/**
 * @file daemon_state.h
 * @brief Subsystem operational state machine and runtime context management.
 *
 * Manages runtime directories, single-instance lockfile enforcement, supervisor state
 * transitions, process coordination (virtiofsd + QEMU), and health reporting.
 */

#ifndef WADDLE_DAEMON_STATE_H
#define WADDLE_DAEMON_STATE_H

#include "daemon_config.h"
#include "../av/av_environment.h"
#include "daemon_fs.h"
#include "daemon_qemu.h"
#include "waddle/daemon_protocol.h"
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Complete runtime state and supervisor context for the Waddle daemon.
 */
typedef struct daemon_state_t {
    /** @brief Current operational state from waddle_subsystem_state_t. */
    waddle_subsystem_state_t state;
    /** @brief Parsed configuration parameters. */
    daemon_config_t config;
    /** @brief QEMU hypervisor process manager handle. */
    qemu_process_t qemu;
    /** @brief VirtIO-FS daemon process manager handle. */
    virtiofs_process_t virtiofs;
    /** @brief Owned AV mapping, prepared before QEMU and released after stop. */
    av_environment_t av_environment;

    /** @brief Path to isolated runtime directory ($XDG_RUNTIME_DIR/waddle). */
    char runtime_dir[WaddleMaxPathLen];
    /** @brief Path to lockfile ($XDG_RUNTIME_DIR/waddle/waddle.lock). */
    char lock_path[WaddleMaxPathLen];
    /** @brief Path to daemon IPC UNIX socket ($XDG_RUNTIME_DIR/waddle/daemon.sock). */
    char daemon_sock_path[WaddleMaxPathLen];
    /** @brief Path to QEMU QMP socket ($XDG_RUNTIME_DIR/waddle/qmp.sock). */
    char qmp_sock_path[WaddleMaxPathLen];
    /** @brief Path to VirtIO-FS vhost-user socket ($XDG_RUNTIME_DIR/waddle/virtiofsd.sock). */
    char virtiofsd_sock_path[WaddleMaxPathLen];
    /** @brief Optional override path for mock guest socket in testing environments. */
    char mock_guest_sock_path[WaddleMaxPathLen];

    /** @brief Path to persistent state directory (~/.local/state/waddle). */
    char state_dir[WaddleMaxPathLen];
    /** @brief Path to log output directory (~/.local/state/waddle/logs). */
    char log_dir[WaddleMaxPathLen];
    /** @brief Log file path for daemon supervisor logs. */
    char daemon_log_path[WaddleMaxPathLen];
    /** @brief Log file path for QEMU output. */
    char qemu_log_path[WaddleMaxPathLen];
    /** @brief Log file path for VirtIO-FS output. */
    char virtiofsd_log_path[WaddleMaxPathLen];

    /** @brief Open file descriptor for waddle.lock (-1 if not held). */
    int lock_fd;
    /** @brief Timestamp (seconds since Unix epoch) when subsystem entered Running state. */
    uint64_t running_since_sec;
    /** @brief Last error diagnostic message if state is SubsystemStateFailed. */
    char last_error[256];
} daemon_state_t;

/**
 * @brief Initializes the daemon supervisor state and determines runtime directories.
 *
 * @param[out] s                  Non-null pointer to daemon_state_t to initialize.
 * @param[in]  custom_runtime_dir Optional directory override for testing (NULL for standard XDG).
 * @return 0 on success, or -1 on failure (e.g. cannot create directories).
 */
int daemon_state_init(daemon_state_t *s, const char *custom_runtime_dir);

/**
 * @brief Acquires exclusive single-instance lock via fcntl F_WRLCK on waddle.lock.
 *
 * @param[in,out] s Non-null pointer to initialized daemon_state_t.
 * @return 0 on success, -1 if another instance is already running (EADDRINUSE/EAGAIN), or -1 on I/O error.
 */
int daemon_state_acquire_lock(daemon_state_t *s);

/**
 * @brief Releases lock and closes lockfile descriptor.
 *
 * @param[in,out] s Non-null pointer to daemon_state_t.
 */
void daemon_state_release_lock(daemon_state_t *s);

/**
 * @brief Orchestrates subsystem startup (virtiofsd -> QEMU -> guest probe).
 *
 * @param[in,out] s           Daemon supervisor state.
 * @param[in]     flags       Startup flags (waddle_daemon_start_flags_t).
 * @param[in]     timeout_sec Maximum total wait time in seconds (0 = default).
 * @return 0 on success, or -1 on error (s->last_error contains description).
 */
int daemon_state_start_subsystem(daemon_state_t *s, uint32_t flags, uint32_t timeout_sec);

/**
 * @brief Orchestrates graceful or forceful subsystem shutdown.
 *
 * @param[in,out] s           Daemon supervisor state.
 * @param[in]     force       1 immediate SIGKILL, 0 ACPI with fallback, WaddleStopGracefulOnly ACPI without fallback.
 * @param[in]     timeout_sec Timeout in seconds before force kill fallback.
 * @return 0 stopped, -1 with errno on failure; ETIMEDOUT in graceful-only mode
 * leaves prior state, children, sockets and mappings intact.
 * @note Nonnull borrowed supervisor; its startup thread serializes calls. No
 * ownership transfer. Resources are freed only after successful child teardown.
 */
int daemon_state_stop_subsystem(daemon_state_t *s, uint32_t force, uint32_t timeout_sec);

/**
 * @brief Immediately force-terminates all subsystem child processes and cleans sockets.
 *
 * @param[in,out] s Daemon supervisor state.
 * @return 0 on success, or -1 on error.
 */
int daemon_state_kill_subsystem(daemon_state_t *s);

/**
 * @brief Populates a status response frame from current daemon state.
 *
 * @param[in]  s    Daemon supervisor state.
 * @param[out] resp Destination status response struct.
 */
void daemon_state_get_status(const daemon_state_t *s, waddle_daemon_status_resp_t *resp);

/**
 * @brief Non-blockingly reaps child processes and transitions state if child exited.
 *
 * @param[in,out] s Daemon supervisor state.
 */
void daemon_state_reap_children(daemon_state_t *s);

/**
 * @brief Shuts down all processes, releases lock, and unlinks sockets.
 *
 * @param[in,out] s Daemon supervisor state.
 */
void daemon_state_cleanup(daemon_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_STATE_H */
