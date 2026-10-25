/**
 * @file session_local_handler.h
 * @brief Session feature handler serving the local session.sock command socket.
 *
 * Accepts LocalLaunch (0x7F00) and LocalStop (0x7F01) from up to four same-uid clients,
 * forwards launches to the guest as LaunchRequest, correlates the guest LaunchResult with
 * the waiting client, stores the host-owned activation token, and answers LocalStop before
 * requesting a graceful drain. Normative behaviour: impl/seamless-interop/claude_45_percent/
 * session_local_handler_contract.md. The handler performs no allocation; the caller owns the
 * whole @ref session_local_handler_t, which may be heap or static storage.
 */
#ifndef WaddleInteropSessionLocalHandlerH
#define WaddleInteropSessionLocalHandlerH

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "activation_store.h"
#include "session_handler.h"
#include "waddle/interop_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fixed bounds and timeouts of the local command handler. */
enum {
    SessionLocalMaxClients = 4, /**< Simultaneous local clients; a fifth is closed at once. */
    SessionLocalMaxPending = 32, /**< Correlation entries; equals the activation store's pending capacity. */
    SessionLocalMaxOutstanding = 4, /**< Launches one client may have awaiting results. */
    SessionLocalRxBytes = 4096, /**< Receive buffer per client; also the largest local frame. */
    SessionLocalTxBytes = 8192, /**< Transmit buffer per client for queued reply frames. */
    SessionLocalScratchBytes = 4096, /**< Frame scratch for LaunchRequest construction. */
    SessionLocalSocketPathBytes = 108, /**< sockaddr_un sun_path capacity including NUL. */
    SessionLocalDeviceNameMax = 63, /**< Longest accepted device name. */
    SessionLocalListenBacklog = 8, /**< listen(2) backlog. */
    SessionLocalLaunchTimeoutMs = 30000, /**< Guest result deadline, from forwarding. */
    SessionLocalIdleTimeoutMs = 10000, /**< Idle limit before a client's first complete frame. */
    SessionLocalStopFlushMs = 1000 /**< Bounded wait for the LocalStopResult flush. */
};

/** @brief Result of the local frame scan (session_local_frame_scan in the implementation). */
enum {
    InteropLocalScanNeedMore = 0, /**< Fewer bytes than the frame needs; nothing consumed. */
    InteropLocalScanReady = 1, /**< A complete frame of frame_length bytes starts the buffer. */
    InteropLocalScanMalformed = 2, /**< Header rejected by the codec (magic, version, length, alignment). */
    InteropLocalScanTooLarge = 3 /**< Frame larger than SessionLocalRxBytes. */
};

/** @brief Result of session_local_handler_init, and of decisions that refuse an action. */
typedef enum session_local_result_t {
    SessionLocalSuccess = 0, /**< Operation completed. */
    SessionLocalInvalidArgument = 1, /**< NULL or malformed input, or a path that does not fit. */
    SessionLocalRefused = 2 /**< A safety rule refused the action; nothing was changed. */
} session_local_result_t;

/** @brief Outcome of the runtime-directory decision. */
typedef enum session_local_directory_decision_t {
    SessionLocalDirectoryUsable = 0, /**< Directory, not a link, owned by the euid, mode 0700. */
    SessionLocalDirectoryRefused = 1 /**< Any other entry: refuse, do not repair. */
} session_local_directory_decision_t;

/** @brief Outcome of the stale-socket decision for a path that exists. */
typedef enum session_local_stale_decision_t {
    SessionLocalStaleUnlink = 0, /**< Connection refused and the singleton lock is proven held. */
    SessionLocalStaleRefuse = 1 /**< Live socket, unknown error, or no proof: never unlink. */
} session_local_stale_decision_t;

/** @brief Outcome of the peer credential decision. */
typedef enum session_local_peer_decision_t {
    SessionLocalPeerAllowed = 0, /**< Peer uid equals the effective uid. */
    SessionLocalPeerRefused = 1 /**< Any other uid: close at once. */
} session_local_peer_decision_t;

/** @brief Action a feature send result maps to for a launch that was not admitted. */
typedef enum session_local_send_outcome_t {
    SessionLocalOutcomeAccepted = 0, /**< HostFeatureSuccess: correlation entry created. */
    SessionLocalOutcomeBusy = 1, /**< QueueFull or RequestIdsFull: LaunchResult Busy. */
    SessionLocalOutcomeUnsupported = 2, /**< NotNegotiated: LaunchResult Unsupported. */
    SessionLocalOutcomeCancelled = 3, /**< Draining: LaunchResult Cancelled. */
    SessionLocalOutcomeError = 4 /**< Any other result: Error InteropErrorUnexpected. */
} session_local_send_outcome_t;

/** @brief Caller inputs for one handler; all strings are borrowed until session_local_handler_destroy. */
typedef struct session_local_config_t {
    const char *runtime_dir; /**< In: absolute XDG_RUNTIME_DIR, NUL-terminated. */
    const char *device_name; /**< In: 1..SessionLocalDeviceNameMax bytes of [A-Za-z0-9_-]. */
    /**
     * In: nonzero only while this process holds the device's session.lock (and command.lock).
     * Exact session_main obligations:
     *  - set 1 only after interop_linux_session_acquire returned success for this device; set 0
     *    when acquire failed, was not attempted, or the owner has been released;
     *  - set it before session_local_handler_init (init copies it once; later edits to the config
     *    have no effect), and before the handler's on_ready, which makes the stale-socket decision
     *    and so unlinks an old session.sock only when the copied proof is nonzero;
     *  - keep the owner alive until the handler's on_closing has run; releasing the owner first
     *    lets a second session acquire the lock while this handler still serves clients, so two
     *    sessions would serve one device.
     * The handler never acquires, releases, or inspects session.lock.
     */
    uint32_t lock_held_proof;
    uint32_t reserved_zero; /**< In: must be zero. */
} session_local_config_t;

/** @brief Counters reported by session_local_handler_stats. All unsigned 64-bit, never reset. */
typedef struct session_local_stats_t {
    uint64_t accepted; /**< Connections accepted into a slot. */
    uint64_t refused_capacity; /**< Connections closed because no slot was free or the handler was stopping. */
    uint64_t refused_peer; /**< Connections closed by the uid check or a failed SO_PEERCRED read. */
    uint64_t closed_malformed; /**< Clients closed for a malformed or invalid frame. */
    uint64_t closed_unknown_type; /**< Clients closed for a type other than LocalLaunch or LocalStop. */
    uint64_t closed_oversize; /**< Clients closed for a frame above SessionLocalRxBytes. */
    uint64_t closed_zero_request; /**< Clients closed for a zero request identifier. */
    uint64_t closed_io; /**< Clients closed by end of stream, receive or send error, or overflow. */
    uint64_t idle_closed; /**< Clients closed by the first-frame idle timeout. */
    uint64_t launches_forwarded; /**< LaunchRequest frames admitted by the runtime. */
    uint64_t launches_busy; /**< LocalLaunch answered Busy (outstanding limit, pending table, or send). */
    uint64_t launches_unsupported; /**< LocalLaunch answered Unsupported. */
    uint64_t launches_cancelled; /**< LocalLaunch answered Cancelled (stopping or draining). */
    uint64_t launch_errors; /**< LocalLaunch answered with an Error frame. */
    uint64_t results_delivered; /**< Guest results queued to an attached client. */
    uint64_t ignored_results; /**< Guest results with an unknown or released identifier. */
    uint64_t timeouts; /**< Launch deadlines that expired. */
    uint64_t replies_dropped; /**< Replies for a client that was gone or whose buffer was full. */
    uint64_t token_stored; /**< Activation tokens accepted by the store. */
    uint64_t token_refused; /**< Activation tokens refused by the store. */
    uint64_t stop_requests; /**< Accepted LocalStop frames. */
    uint64_t stop_deadline_expired; /**< Stops that ended by the flush bound. */
    uint64_t accept_errors; /**< accept4 failures other than EAGAIN, EINTR and ECONNABORTED. */
} session_local_stats_t;

/** @brief One accepted local client. Owned by the handler; the descriptor is closed by it. */
typedef struct session_local_client_t {
    int fd; /**< Connected nonblocking descriptor, or -1 when the slot is free. */
    uint64_t serial; /**< Nonzero while occupied; never reused for a different client. */
    uint64_t accepted_ms; /**< Monotonic accept time for the first-frame idle timeout. */
    int frame_seen; /**< Nonzero after the first complete frame. */
    int epoll_writable; /**< Nonzero while EPOLLOUT is registered for pending output. */
    size_t rx_length; /**< Bytes buffered in rx. */
    size_t tx_length; /**< Bytes queued in tx, including sent bytes before tx_offset. */
    size_t tx_offset; /**< First unsent byte of tx. */
    uint8_t rx[SessionLocalRxBytes]; /**< Receive accumulator. */
    uint8_t tx[SessionLocalTxBytes]; /**< Reply frames awaiting send. */
} session_local_client_t;

/** @brief One correlation entry: a forwarded launch awaiting its guest LaunchResult. */
typedef struct session_local_pending_t {
    int in_use; /**< Nonzero while the entry is live. */
    uint32_t guest_request_id; /**< Key: identifier reserved from the runtime. */
    uint32_t local_request_id; /**< Client request identifier echoed in the reply. */
    uint32_t client_index; /**< Client slot that receives the reply. */
    uint64_t client_serial; /**< Serial of that client; a mismatch means the client is gone. */
    uint64_t deadline_ms; /**< Monotonic deadline: send time plus SessionLocalLaunchTimeoutMs. */
    uint32_t token_length; /**< Client activation token length, 0 when absent. */
    uint8_t token[InteropActivationTokenBufferBytes]; /**< NUL-terminated copy of the client token. */
} session_local_pending_t;

/** @brief Complete handler state. Caller-owned; every member is initialized by init. */
typedef struct session_local_handler_t {
    uint32_t lock_held_proof; /**< Copy of the config proof; the borrowed strings are never kept. */
    uint32_t expected_uid; /**< geteuid() at init; the only peer and directory owner accepted. */
    char parent_path[SessionLocalSocketPathBytes]; /**< <runtime>/waddle, created with mode 0700 when missing. */
    char directory_path[SessionLocalSocketPathBytes]; /**< <runtime>/waddle/<device>. */
    char socket_path[SessionLocalSocketPathBytes]; /**< <directory>/session.sock. */
    int listen_fd; /**< Listening socket, or -1. */
    int epoll_fd; /**< Epoll set multiplexing the listener and clients, or -1. */
    int socket_created; /**< Nonzero when this handler bound the socket file. */
    int bound; /**< Nonzero after a successful on_ready. */
    int stopping; /**< Nonzero after an accepted LocalStop. */
    int closed; /**< Nonzero after on_closing has run. */
    uint32_t stop_client_index; /**< Client that receives the LocalStopResult. */
    uint64_t stop_client_serial; /**< Serial of that client. */
    uint64_t stop_deadline_ms; /**< Flush bound for the stop reply. */
    uint64_t last_now_ms; /**< Last monotonic time from any callback, used by on_frame. */
    uint64_t next_serial; /**< Next client serial; starts at one. */
    session_local_client_t clients[SessionLocalMaxClients]; /**< Client slots. */
    session_local_pending_t pending[SessionLocalMaxPending]; /**< Correlation table. */
    uint8_t scratch[SessionLocalScratchBytes]; /**< LaunchRequest construction buffer. */
    interop_activation_store_t tokens; /**< Host-owned activation tokens. */
    session_local_stats_t stats; /**< Counters. */
} session_local_handler_t;

/**
 * @brief Initialize caller storage for one handler. Performs no I/O and no allocation.
 * @param[out] handler Non-NULL writable storage; fully overwritten on success.
 * @param[in] config Non-NULL config. The device name must match the documented grammar, the
 *            runtime directory must be absolute, and the resulting socket path must fit in
 *            SessionLocalSocketPathBytes including its NUL.
 * @return SessionLocalSuccess, SessionLocalInvalidArgument for NULL, malformed or too-long input,
 *         or SessionLocalRefused when reserved_zero is nonzero. On failure @p handler is unchanged.
 * @note Copies the effective uid. Thread-safety: not thread-safe; one owner thread.
 */
int session_local_handler_init(session_local_handler_t *handler, const session_local_config_t *config);

/**
 * @brief Fill a callback table that routes every runtime callback to this handler.
 * @param[in,out] handler Initialized handler; retained by the table as its context.
 * @param[out] out_table Non-NULL writable table; fully overwritten.
 * @note Capabilities are exactly InteropCapLaunch. The table is valid while the handler lives.
 * @thread_safety Pump thread only.
 */
void session_local_handler_table(session_local_handler_t *handler, session_handler_t *out_table);

/**
 * @brief Release every resource still held and make the handler inert.
 * @param[in,out] handler Initialized handler or NULL (no-op). Safe after on_closing and twice.
 * @note Closes any open descriptor, unlinks the socket file only when this handler created it and
 *       it still exists, and never sends. Does not free the caller storage.
 * @thread_safety Pump thread only, after the run has returned.
 */
void session_local_handler_destroy(session_local_handler_t *handler);

/**
 * @brief Copy the current counters.
 * @param[in] handler Initialized handler.
 * @param[out] out_stats Non-NULL writable structure.
 * @thread_safety Pump thread only.
 */
void session_local_handler_stats(const session_local_handler_t *handler, session_local_stats_t *out_stats);

/**
 * @brief Decide whether the runtime directory entry is usable.
 * @param[in] is_directory Nonzero when lstat reports a directory.
 * @param[in] is_symlink Nonzero when lstat reports a symbolic link.
 * @param[in] owner_uid st_uid of the entry.
 * @param[in] mode st_mode permission bits (mode & 07777).
 * @param[in] expected_uid Effective uid of the handler.
 * @return SessionLocalDirectoryUsable only when directory, not a link, owned by expected_uid and mode 0700.
 * @note Pure; no I/O. Thread-safe.
 */
session_local_directory_decision_t session_local_directory_decision(int is_directory, int is_symlink,
    uint32_t owner_uid, uint32_t mode, uint32_t expected_uid);

/**
 * @brief Decide what to do with an existing session.sock that is not known to be live.
 * @param[in] probe_errno errno from a nonblocking AF_UNIX connect probe; 0 when connect succeeded.
 * @param[in] lock_held_proof Nonzero only when the caller holds the singleton session lock.
 * @return SessionLocalStaleUnlink only for ECONNREFUSED with proof; every other case refuses.
 * @note Pure; EAGAIN and EINPROGRESS count as live. Thread-safe.
 */
session_local_stale_decision_t session_local_stale_decision(int probe_errno, uint32_t lock_held_proof);

/**
 * @brief Decide whether a connected peer may use the socket.
 * @param[in] peer_uid SO_PEERCRED uid of the peer.
 * @param[in] expected_uid Effective uid of the handler.
 * @return SessionLocalPeerAllowed only when the uids are equal.
 * @note Pure. Thread-safe.
 */
session_local_peer_decision_t session_local_peer_decision(uint32_t peer_uid, uint32_t expected_uid);

/**
 * @brief Map a feature sender result for a forwarded launch to the reply action.
 * @param[in] send_result interop_host_feature_result_t value returned by the sender.
 * @return The action of section 8.1 of the contract. Unknown values map to Error.
 * @note Pure. Thread-safe.
 */
session_local_send_outcome_t session_local_send_outcome(int send_result);


#ifdef __cplusplus
}
#endif
#endif
