/** @file venus_service.h @brief Isolated Linux receiver service lifecycle. */
#ifndef WaddleVenusServiceH
/** @brief Include guard, no storage/ownership. */
#define WaddleVenusServiceH
#include "venus_dispatch.h"
/** @brief Trusted host policy, copied/read only by sole service thread.
 * @note Caller-owned, no pointers/resources; never serialized from guest input.
 */
typedef struct venus_service_config_t {
    uint32_t command_bytes;        /**< Power-of-two receiver/transfer bytes, 64..16MiB. */
    uint64_t reply_bytes;          /**< Power-of-two reply resource bytes, 4096..16MiB. */
    uint32_t resource_count;       /**< Additional registry slots, 1..64. */
    uint64_t resource_bytes;       /**< Additional declared storage quota, 4096..one GiB. */
    uint32_t cpu_timeout_ms;       /**< CPU budget, 1..60000ms. */
    uint32_t gpu_timeout_ms;       /**< Per-GPU-queue budget, 1..60000ms. */
    uint32_t operation_timeout_ms; /**< Handshake/exchange budget, 1..60000ms. */
} venus_service_config_t;
/** @brief Fill documented default trusted policy.
 * @param[out] config Nonnull private caller record, fully initialized.
 * @return RingOk or RingInvalid for null; no allocation, thread-safe/disjoint.
 */
venus_ring_status_t venus_service_config_init(venus_service_config_t *config);
/** @brief Own one Linux service run over borrowed mapping/control descriptors.
 * @param[in] config Nonnull immutable private trusted policy, retained for run.
 * @param[in] mapping_fd Borrowed CLOEXEC regular fixed-size initialized region fd.
 * @param[in] stream_fd Borrowed CLOEXEC nonblocking connected stream socket.
 * @param[in] cancel Nullable borrowed atomic cancellation, retained for run.
 * @return RingClosed orderly/disconnect, terminal channel/health status otherwise;
 * RingInvalid local config/descriptor, RingCorrupt acquisition/ABI/entropy failure.
 * @note Sole service thread, owns mmap/buffer/receiver/channel until return.
 * Caller closes descriptors afterwards. All attached rings close before release;
 * receiver callbacks join before private storage/mmap release. Parent must bound
 * process shutdown because SDK teardown can block. Never reinitialize old cursors.
 */
venus_ring_status_t venus_service_run(const venus_service_config_t *config, int mapping_fd,
                                      int stream_fd, const _Atomic uint32_t *cancel);
/** @brief Run a receiver service with optional trusted native presentation binding.
 * @param[in] config Nonnull immutable trusted policy retained for run.
 * @param[in] mapping_fd Borrowed CLOEXEC fixed-size regular initialized region.
 * @param[in] stream_fd Borrowed CLOEXEC/nonblocking connected guest stream.
 * @param[in] frame_fd Borrowed prepared native seqpacket endpoint distinct from
 * mapping/stream, or -1 for unbound operation.
 * @param[in] controller_pid Positive retained controller PID; zero only if unbound.
 * @param[in] context Nonzero exact controller identity; zero only if unbound.
 * @param[in] cancel Nullable borrowed atomic flag retained for run.
 * @return Same service status policy as venus_service_run; Invalid malformed
 * presentation binding; acquired state unwinds on every failure.
 * @note Sole service thread. Owns call-scoped export leases/receiver, borrows all
 * descriptors/identities. Closes rings before abandoning leases/receiver, then
 * unmaps/frees private storage. Caller stops worker before socket/window teardown
 * and discards old-context allocations on loss; no pixel mapping/copy.
 */
venus_ring_status_t venus_service_run_presented(const venus_service_config_t *config,
                                                int mapping_fd, int stream_fd, int frame_fd,
                                                int32_t controller_pid, uint64_t context,
                                                const _Atomic uint32_t *cancel);
#endif
