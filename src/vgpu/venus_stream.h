/** @file venus_stream.h @brief Internal native readiness/completion boundary. */
#ifndef WaddleVenusStreamH
/** @brief Compile-time guard; no storage or ownership. */
#define WaddleVenusStreamH
#include "waddle/venus_channel.h"
/** @brief Validate stream and acquire event.
 * @param[in,out] channel Nonnull pending record with borrowed stream.
 * @return RingOk/RingInvalid. Failure retains no owned event.
 * @note Session-thread-only; event released by venus_stream_free.
 */
venus_ring_status_t venus_stream_open(venus_channel_t *channel);
/** @brief Release only the owned completion event.
 * @param[in,out] channel Nonnull quiescent record; event zeroed.
 * @note Session-thread-only; never closes the borrowed stream. No active I/O.
 */
void venus_stream_free(venus_channel_t *channel);
/** @brief Read monotonic milliseconds with a nonzero origin.
 * @return Milliseconds, zero on clock failure/overflow; no allocation/ownership.
 * @note Thread-safe, no wall-clock adjustments.
 */
uint64_t venus_stream_time_ms(void);
/** @brief Perform one bounded native I/O attempt with no pending request on return.
 * @param[in,out] channel Nonnull initialized record, exclusively owned by caller.
 * @param[in,out] buffer Nonnull private length-byte range; write borrows input,
 * read modifies only transferred bytes, disjoint from channel/stream structures.
 * @param[in] length Nonzero bytes, at most VenusControlBytes.
 * @param[in] writing One for write, zero for read.
 * @param[out] transferred Nonnull private byte count, zero on no progress.
 * @return RingOk for positive bytes, RingAgain for one-ms slice/EINTR/EAGAIN,
 * RingClosed for EOF/OS error. No buffer ownership transfer or allocation.
 * @note Sole session thread. Windows cancels/joins pending stack OVERLAPPED
 * before return; cancellation-racing completed bytes remain positive progress.
 */
venus_ring_status_t venus_stream_io(venus_channel_t *channel, void *buffer, size_t length,
                                    int writing, size_t *transferred);
#endif
