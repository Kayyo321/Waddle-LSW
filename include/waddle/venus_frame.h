/** @file venus_frame.h @brief Private bounded image metadata and trusted FD handoff. */
#ifndef WaddleVenusFrameH
/** @brief Include guard; no ownership or storage. */
#define WaddleVenusFrameH
#include "venus_dmabuf.h"
/** @brief Exact little-endian presentation packet extent, including zero padding. */
#define VenusFrameBytes 1216u
/** @brief Maximum validated damage rectangles per frame. */
#define VenusFrameMaxDamage 64u
/** @brief Decoded caller-owned image metadata, no FD/pixel/pointer ownership.
 * @note Natural eight-byte C/Zig alignment; never cast onto transport bytes.
 * Immutable query/encode inputs are thread-safe; outputs must be disjoint.
 */
typedef struct venus_frame_t {
    uint64_t context;             /**< Nonzero controller context binding, never a host address. */
    uint64_t frame;               /**< Nonzero monotonically increasing per-context frame ID. */
    venus_dmabuf_layout_t layout; /**< Private validated image layout. */
    uint32_t resource_ids[4];     /**< Registered IDs 2..65 per active plane; inactive zero. */
    uint32_t damage_count;        /**< Active rectangle count, 1..64. */
    venus_dmabuf_damage_t damage[VenusFrameMaxDamage]; /**< Active then zero rectangles. */
} venus_frame_t;
/** @brief Encode one validated immutable private frame into exact wire storage.
 * @param[in] frame Nonnull private input, disjoint from bytes.
 * @param[out] bytes Nonnull private bytes[length], unchanged on failure.
 * @param[in] length Exactly VenusFrameBytes accessible bytes.
 * @return RingOk or RingInvalid NULL/length/metadata error.
 * @note Allocation-free/thread-safe, no retained pointers or native padding on wire.
 */
venus_ring_status_t venus_frame_encode(const venus_frame_t *frame, void *bytes, size_t length);
/** @brief Decode and validate all fields of one immutable private frame packet.
 * @param[out] frame Nonnull disjoint private output, zeroed on every failure.
 * @param[in] bytes Nonnull immutable private bytes[length], borrowed for call.
 * @param[in] length Exactly VenusFrameBytes accessible bytes.
 * @return RingOk, RingInvalid NULL, RingCorrupt shape/identity/padding/layout errors.
 * @note Allocation-free/thread-safe. Native credentials, FD count, expected context
 * and frame ordering remain separate channel/owner checks.
 */
venus_ring_status_t venus_frame_decode(venus_frame_t *frame, const void *bytes, size_t length);
/** @brief Exact little-endian release packet size; independent of frame packets. */
#define VenusReleaseBytes 32u
/** @brief Caller-owned private completion record, naturally eight-byte aligned.
 * @note No resource/pointer ownership; immutable codec inputs are thread-safe.
 * Never cast onto wire bytes. Padding is native-only and not serialized.
 */
typedef struct venus_release_t {
    uint64_t context;           /**< Nonzero retained controller context identity. */
    uint64_t frame;             /**< Nonzero frame identity, matched against worker leases. */
    venus_ring_status_t status; /**< Ok release, Invalid rejection, Cancelled or Closed. */
} venus_release_t;
/** @brief Encode a private completion record into exact release wire storage.
 * @param[in] release Nonnull immutable input disjoint from bytes, borrowed for call.
 * @param[out] bytes Nonnull private bytes[length]; unchanged on failure.
 * @param[in] length Exactly VenusReleaseBytes accessible bytes.
 * @return RingOk or RingInvalid NULL/length/identity/status error.
 * @note Allocation-free/thread-safe; retains no pointers and owns no resources.
 */
venus_ring_status_t venus_release_encode(const venus_release_t *release, void *bytes,
                                         size_t length);
/** @brief Decode and validate one private release packet.
 * @param[out] release Nonnull disjoint private output, zeroed on every failure.
 * @param[in] bytes Nonnull immutable private bytes[length], borrowed for call.
 * @param[in] length Exactly VenusReleaseBytes accessible bytes.
 * @return RingOk, RingInvalid NULL, RingCorrupt shape/identity/status/reserved errors.
 * @note Allocation-free/thread-safe; credentials and lease matching are separate.
 */
venus_ring_status_t venus_release_decode(venus_release_t *release, const void *bytes,
                                         size_t length);
#ifndef _WIN32
/** @brief Verify borrowed trusted socket and enable kernel credential delivery.
 * @param[in] socket_fd Borrowed CLOEXEC/nonblocking AF_UNIX SOCK_SEQPACKET endpoint.
 * @return RingOk or RingInvalid local/socket/option error, no descriptor acquisition.
 * @note Call on both endpoints before worker launch/traffic. Intentionally sets
 * SO_PASSCRED on the socket; caller owns close and unreaped worker identity lifetime.
 */
venus_ring_status_t venus_frame_prepare(int socket_fd);
/** @brief Queue exact frame and borrowed plane FDs with kernel-checkable credentials.
 * @param[in] socket_fd Prepared borrowed socket, native send owner thread.
 * @param[in] frame Nonnull immutable private metadata, borrowed for call.
 * @param[in] fds Nonnull borrowed array[count], valid DMA-BUF FDs retained for call.
 * @param[in] count Exactly validated active plane count 1..4.
 * @return RingOk queued kernel duplicates; Again would-block, Invalid local/FD/schema,
 * Closed native send error, Corrupt impossible partial packet. Originals stay caller-owned.
 * @note Nonblocking/allocation-free; never maps pixels or transmits FDs to the guest.
 */
venus_ring_status_t venus_frame_send(int socket_fd, const venus_frame_t *frame, const int *fds,
                                     size_t count);
/** @brief Acquire a frame and plane descriptors only from the retained worker identity.
 * @param[in] socket_fd Prepared borrowed socket, native receive owner thread.
 * @param[in] expected_pid Positive live/unreaped worker PID; caller prevents reuse.
 * @param[in] expected_context Nonzero exact controller binding.
 * @param[out] frame Nonnull private output, zeroed on failure.
 * @param[out] fds Nonnull disjoint four-slot private output, initially -1; success
 * transfers owned CLOEXEC FDs; release with venus_frame_fds_free after handoff.
 * @return RingOk, Again no packet, Invalid local args, Closed native disconnect/error,
 * Corrupt payload/ancillary/sender/context/count mismatch. All acquired FDs close on failure.
 * @note Nonblocking/allocation-free; verifies sender real UID equals caller's getuid().
 * Outputs must not overlap. Kernel closes ancillary-truncated excess descriptors.
 */
venus_ring_status_t venus_frame_receive(int socket_fd, int32_t expected_pid,
                                        uint64_t expected_context, venus_frame_t *frame,
                                        int fds[4]);
/** @brief Close/reset an owned four-slot descriptor result after import/cancellation.
 * @param[in,out] fds Nullable private four-slot array, each slot -1 or owned FD.
 * @note Sole ownership thread, idempotent after first call; sets all entries -1.
 * No allocation or status; never use on borrowed send descriptors.
 */
void venus_frame_fds_free(int fds[4]);
#endif
#endif
