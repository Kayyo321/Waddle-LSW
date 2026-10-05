#ifndef WaddleAvEnvironmentH
#define WaddleAvEnvironmentH
#include <stddef.h>
#include "daemon_config.h"
/** @brief Daemon-owned AV memory resource, acquired before starting QEMU.
 * @note Set fd=-1 before prepare. free unmaps/closes and removes only a newly
 * created private regular file. Workers and VM must stop before free.
 */
typedef struct av_environment_t {
    int fd; /**< Owned shared-memory or KVMFR descriptor, -1 when empty. */
    void *mapping; /**< Owned mapping until free; borrowed by AV workers. */
    size_t length; /**< Exact two-GiB mapping length. */
    int created_file; /**< Whether this instance exclusively created the file. */
    char path[WaddleMaxPathLen]; /**< Retained pathname for owned-file cleanup. */
} av_environment_t;
/** @brief Prepare configured IVSHMEM memory and validate explicit GPU ownership.
 * @param[out] environment Nonnull empty caller-owned record with fd=-1.
 * @param[in] config Nonnull validated daemon settings; AV must be enabled.
 * @param[out] error Nonnull caller-owned diagnostic buffer of capacity bytes.
 * @param[in] capacity Accessible diagnostic bytes, at least one.
 * @return 0 on prepared resource, -1 with errno and diagnostic on failure.
 * @note Daemon startup thread only, before VM/AV workers start. Never detaches
 * a host GPU, overwrites a preexisting file, or resets compositor-owned slots.
 * File creation/driver mapping happens here, without manually prepared memory.
 */
int av_environment_prepare(av_environment_t *environment, const daemon_config_t *config,
                            char *error, size_t capacity);
/** @brief Release owned mapping/FD and remove only this instance's created file.
 * @param[in,out] environment Nonnull partially initialized/active/empty record.
 * @note Startup thread only after workers and VM stop; idempotent; no result.
 */
void av_environment_free(av_environment_t *environment);
#endif
