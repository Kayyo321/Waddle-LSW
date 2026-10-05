#ifndef WaddleVenusMappingH
#define WaddleVenusMappingH
#include "venus_region.h"

/** @brief Owned native dedicated region mapping; create/open pairs with free.
 * @note Caller-owned record; never copy an active owner. Session thread alone
 * manages lifecycle and joins all workers before free. Linux seals a memfd
 * against resize; Windows pins the signed IVSHMEM driver mapping with a handle.
 * Zero-initialized/failed/freed records are safe to free. No AV layout is reused.
 */
typedef struct venus_mapping_t {
    void *mapping;              /**< Owned mapping pages while owns_mapping is set. */
    size_t mapping_bytes;       /**< Accessible mapped bytes, zero when inactive. */
    intptr_t native_handle;     /**< Owned Linux fd or Windows driver handle. */
    uint32_t owns_handle;       /**< Set immediately after native handle acquisition. */
    uint32_t owns_mapping;      /**< Set after successful mmap/driver mapping request. */
    venus_region_view_t view;   /**< Borrowed rings/resources pinned by this owner. */
} venus_mapping_t;

#ifndef _WIN32
/** @brief Create and initialize a dedicated sealed Linux memfd for QEMU IVSHMEM.
 * @param[out] memory Nonnull inactive caller-owned record; reset on failure.
 * @param[in] mapping_bytes Power-of-two BAR bytes, 4096..one GiB, rings must fit.
 * @param[in] capacity Valid bounded power-of-two capacity of each ring.
 * @return Zero on success; -1 on failure with errno. No partially owned resource.
 * @note Session-thread only. Caller keeps native_handle open until QEMU and all
 * workers stop, then calls free. QEMU may open /proc/<host-pid>/fd/<native_handle>
 * while this owner lives. FD is close-on-exec; no child ownership is implied.
 */
int venus_mapping_create(venus_mapping_t *memory, size_t mapping_bytes, uint32_t capacity);
#else
/** @brief Open a selected signed-driver IVSHMEM device and validate Venus identity.
 * @param[out] memory Nonnull inactive caller-owned record; reset on failure.
 * @param[in] device_index SetupAPI enumeration index of the dedicated Venus BAR.
 * @return Zero on success; -1 with preserved GetLastError on driver/ABI failure.
 * @note Session-thread only. Uses cached coherent driver mapping, never initializes
 * guest memory. Host must hand off a ready region before this call. Free after
 * workers stop. A selected AV or unrelated device fails ABI validation safely.
 */
int venus_mapping_open(venus_mapping_t *memory, unsigned device_index);
#endif
/** @brief Release an owned mapping and handle after all workers/VM users stop.
 * @param[in,out] memory Nullable active/partial/zero/freed caller-owned record.
 * @note Session-thread only, idempotent, clears every pointer/ownership flag and
 * borrowed view. Does not close rings; lifecycle owner coordinates peer shutdown.
 */
void venus_mapping_free(venus_mapping_t *memory);
#endif
