/**
 * @file session_exit_codes.h
 * @brief Process exit statuses of waddle-session, shared with its clients.
 *
 * The open client treats WaddleSessionExitLockHeld as a handover: another session already owns
 * the device, so the client waits for its session.sock instead of reporting a failure.
 */
#ifndef WaddleInteropSessionExitCodesH
#define WaddleInteropSessionExitCodesH

#ifdef __cplusplus
extern "C" {
#endif

/** @brief waddle-session process exit statuses. */
enum {
    WaddleSessionExitSuccess = 0, /**< Bounded run completed, or a graceful local stop ended the run. */
    WaddleSessionExitFailure = 1, /**< Runtime or OS failure, or a supervised run that could not continue. */
    WaddleSessionExitUsage = 2, /**< Invalid arguments, device profile, or environment. */
    /** Another session holds the device's session.lock or command.lock (EX_TEMPFAIL). Not a failure. */
    WaddleSessionExitLockHeld = 75
};

#ifdef __cplusplus
}
#endif
#endif
