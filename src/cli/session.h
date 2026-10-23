/**
 * @file session.h
 * @brief Multiplexed stream event loop bridging host I/O and guest transport.
 */

#ifndef WaddleSessionH
#define WaddleSessionH

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Runs the multiplexed bidirectional session event loop until process termination.
 *
 * Multiplexes host stdin into outbound wire frames, demultiplexes incoming stdout and
 * stderr stream frames to host descriptors, forwards terminal signals, and monitors
 * overall session deadlines. If the transport stops accepting writes, queued stdin
 * and signals are discarded while incoming frames are validated for at most two
 * seconds (or the earlier session deadline). Only both stream EOFs followed by a
 * complete ProcessExit authorize a guest exit result; a disconnect alone never does.
 *
 * @param[in]     fd          Connected transport socket descriptor.
 * @param[in,out] tx          Non-null pointer to outbound transmission queue.
 * @param[in]     seq         Initial message sequence number.
 * @param[in]     deadline    Monotonic millisecond deadline, or 0 for unlimited.
 * @param[in]     interactive Non-zero if session is interactive with raw terminal.
 * @return Guest process exit code (0-255), 124 on timeout, 125 on local error,
 *         126 on cannot-execute, or 127 on command-not-found.
 * @note Caller retains ownership of fd and tx. This function is their sole I/O owner
 *       during the call; it is not safe to call concurrently with terminal/session APIs.
 *       Defensively frees all intermediate queues and decoder buffers before returning.
 */
int waddle_session(int fd, queue_t *tx, uint32_t seq, uint64_t deadline, int interactive);

#ifdef __cplusplus
}
#endif

#endif /* WaddleSessionH */
