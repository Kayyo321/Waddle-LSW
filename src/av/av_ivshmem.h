#ifndef WaddleAvIvshmemH
#define WaddleAvIvshmemH
#include <stddef.h>
#include <windows.h>
/** @brief Caller-owned signed-driver mapping; init/free are symmetric.
 * @note Kernel owns mapped pages; device handle pins them until unmap/free.
 * Main session owns lifetime; all capture/audio workers join before free.
 */
typedef struct av_ivshmem_t {
    HANDLE device; /**< Owned open driver handle, NULL when inactive. */
    void *mapping; /**< Borrowed driver mapping while device remains open. */
    size_t length; /**< Accessible bytes reported by driver. */
} av_ivshmem_t;
/** @brief Map a selected signed Red Hat IVSHMEM device using cached coherent memory.
 * @param[out] memory Nonnull zero-initialized caller-owned record.
 * @param[in] device_index Zero-based SetupAPI device enumeration index.
 * @return 0 on validated fixed-layout mapping, -1 on driver/map/ABI failure.
 * @note Session thread only; frees partial resources; never initializes host memory.
 */
int av_ivshmem_init(av_ivshmem_t *memory, unsigned device_index);
/** @brief Unmap and close driver after capture/audio workers have joined.
 * @param[in,out] memory Nonnull partially initialized/active/empty record.
 * @note Session thread only, idempotent; NULLs references and zeros length.
 */
void av_ivshmem_free(av_ivshmem_t *memory);
#endif
