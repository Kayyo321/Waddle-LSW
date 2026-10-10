/**
 * @file terminal.h
 * @brief Terminal raw mode management, dimensions querying, and asynchronous signal traps.
 */

#ifndef WaddleTerminalH
#define WaddleTerminalH

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct winsize;

/**
 * @brief Queries current terminal window dimensions via TIOCGWINSZ ioctl.
 *
 * Defaults rows to 24 and columns to 80 if ioctl reports zero dimensions.
 *
 * @param[out] size Non-null pointer to struct winsize to populate.
 * @return 0 on success, or -1 on ioctl failure.
 */
int waddle_terminal_size(struct winsize *size);

/**
 * @brief Configures terminal attributes and registers signal handlers.
 *
 * Sets up a self-pipe trick for SIGINT, SIGQUIT, SIGTERM, and SIGWINCH. If interactive,
 * captures original termios state and transitions STDIN into raw mode.
 *
 * @param[in] interactive Non-zero to enable raw termios mode; 0 for standard pipe mode.
 * @return 0 on success, or -1 on initialization failure.
 * @note Installs an atexit hook to ensure waddle_restore() is invoked on termination.
 */
int waddle_terminal_init(int interactive);

/**
 * @brief Captures original flags for stdin, stdout, stderr and sets them to non-blocking.
 *
 * @return 0 on success, or -1 on fcntl failure.
 */
int waddle_standard_nonblock(void);

/**
 * @brief Restores original termios attributes and file descriptor flags.
 *
 * Idempotent and safe to invoke from signal handlers or atexit callbacks.
 */
void waddle_restore(void);

/**
 * @brief Closes signal pipes and restores terminal attributes.
 */
void waddle_terminal_close(void);

/**
 * @brief Returns the read-end file descriptor of the internal signal wake-up pipe.
 *
 * @return File descriptor suitable for inclusion in poll() waitsets.
 */
int waddle_signal_fd(void);

/**
 * @brief Flushes any accumulated wake-up bytes from the signal pipe without acting.
 */
void waddle_drain_signals(void);

/**
 * @brief Serializes and transmits queued signals and resize events to the guest.
 *
 * Drains signal pipe and enqueues WaddleMsgTerminalResize or WaddleMsgSignalEvent.
 *
 * @param[in,out] tx          Non-null pointer to outbound transmission queue.
 * @param[in,out] seq         Non-null pointer to message sequence counter.
 * @param[in]     interactive Non-zero if session is interactive (forwards SIGWINCH).
 * @return 0 on success, or -1 on transmission failure.
 */
int waddle_send_pending(queue_t *tx, uint32_t *seq, int interactive);

#ifdef __cplusplus
}
#endif

#endif /* WaddleTerminalH */
