#ifndef WaddleAvCommandsH
#define WaddleAvCommandsH
#include <stddef.h>
#include <stdint.h>
/** @brief AV command kinds, local ABI only; immutable enum values. */
typedef enum av_command_kind_t { AvSetup = 1, AvProbe = 2, AvRun = 3 } av_command_kind_t;
/** @brief Validated private argv copy; caller owns storage, no retained pointers. */
typedef struct av_command_t {
    uint32_t kind; /**< One av_command_kind_t value. */
    uint32_t process_id; /**< Run target, positive Windows PID; zero for setup/probe. */
    uint32_t uefi; /**< Setup selects managed persistent OVMF firmware. */
    uint32_t latency; /**< Run opts into diagnostic fixture compositor-commit RTT. */
    uint32_t kvmfr; /**< Setup requests privileged pinned module provisioning. */
    char device[64]; /**< ASCII registry name, empty selects saved default. */
    char firmware_vars[1024]; /**< Optional existing image firmware metadata to import. */
    char gpu_bdf[16]; /**< Optional registered mdev parent, never detached. */
    char gpu_mdev_uuid[37]; /**< Owned canonical UUID of an existing administrator slice. */
} av_command_t;
/** @brief Parse bounded AV command arguments before any side effects.
 * @param[in] count Number of argv strings, 0..12.
 * @param[in] arguments Nonnull array of count nonnull NUL-terminated strings.
 * @param[out] command Nonnull caller-owned result; unchanged on invalid input.
 * @return 0 validated, 1 help, -1 invalid UTF-8/grammar/value/count.
 * @note Thread-safe, allocation-free; C buffers have fixed bounds, no ownership transfer.
 */
int waddle_av_parse(size_t count, const char *const *arguments, av_command_t *command);
#endif
