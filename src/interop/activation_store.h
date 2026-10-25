#ifndef WaddleInteropActivationStoreH
#define WaddleInteropActivationStoreH
/** @file activation_store.h @brief Activation token selection and the bounded 30 s (token, process_id) store. */
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum activation token length in bytes. */
#define InteropActivationMaxTokenBytes 255u
/** @brief Maximum stored (token, process_id) pairs. */
#define InteropActivationMaxEntries 32u
/** @brief Entry lifetime in milliseconds; live while now < inserted + this value. */
#define InteropActivationTtlMs 30000u
/** @brief Bytes of the take output buffer: the token plus a NUL terminator. */
#define InteropActivationTokenBufferBytes 256u
/** @brief Byte size of the store; must equal sizeof(interop_activation_store_t). */
#define InteropActivationStoreBytes 8992u

/**
 * @brief Opaque-to-C store storage: caller-owned, 8-byte aligned, no pointers.
 * @note All-zero bytes are a valid empty store. Not thread-safe; one owner thread.
 */
typedef struct interop_activation_store_t {
    uint64_t words[InteropActivationStoreBytes / 8u]; /**< Private layout, see activation_store_contract.md. */
} interop_activation_store_t;

_Static_assert(sizeof(interop_activation_store_t) == InteropActivationStoreBytes, "store size is part of the ABI");

/** @brief Status of interop_activation_put. */
typedef enum interop_activation_put_status_t {
    InteropActivationPutOk = 0, /**< Pair inserted or replaced. */
    InteropActivationPutInvalidArgument = 1, /**< NULL store, or NULL token with length above 0. */
    InteropActivationPutInvalidToken = 2, /**< Token is empty, longer than 255 bytes, or outside 0x21..0x7E. */
    InteropActivationPutInvalidProcess = 3 /**< process_id is zero. */
} interop_activation_put_status_t;

/** @brief Status of interop_activation_take. */
typedef enum interop_activation_take_status_t {
    InteropActivationTakeFound = 0, /**< Token copied and removed from the store. */
    InteropActivationTakeInvalidArgument = 1, /**< NULL store, out or length_out. */
    InteropActivationTakeNotFound = 2, /**< No live token for this process_id. */
    InteropActivationTakeInvalidProcess = 3 /**< process_id is zero. */
} interop_activation_take_status_t;

/** @brief Status of interop_activation_pick_token. */
typedef enum interop_activation_pick_status_t {
    InteropActivationPickFound = 0, /**< A candidate was valid; outputs are set. */
    InteropActivationPickNone = 1, /**< Neither candidate was valid; outputs unchanged. */
    InteropActivationPickInvalidArgument = 2 /**< NULL token_out or length_out; outputs unchanged. */
} interop_activation_pick_status_t;

/**
 * @brief Initialize caller storage to an empty store.
 * @param[out] store Nonnull; InteropActivationStoreBytes writable bytes, 8-byte aligned.
 * @return 0 on success, 1 when store is NULL.
 */
int interop_activation_store_init(interop_activation_store_t *store);

/**
 * @brief Insert or replace a (token, process_id) pair at monotonic time now_ms.
 * @param[in,out] store Nonnull store.
 * @param[in] token Readable bytes; NULL allowed only when token_length is 0.
 * @param[in] token_length Byte count, 0..255 valid.
 * @param[in] process_id Nonzero process identifier.
 * @param[in] now_ms Monotonic milliseconds; a value below the previous one clears the store.
 * @return An interop_activation_put_status_t value.
 * @note Stale pairs are expired before any eviction; the oldest pair is evicted only when full.
 */
int interop_activation_put(interop_activation_store_t *store, const uint8_t *token, size_t token_length, uint32_t process_id, uint64_t now_ms);

/**
 * @brief Remove and copy the newest live token for a process identifier.
 * @param[in,out] store Nonnull store.
 * @param[in] process_id Nonzero process identifier.
 * @param[in] now_ms Monotonic milliseconds.
 * @param[out] out Nonnull buffer of InteropActivationTokenBufferBytes (256) bytes. On success it
 *            holds the token followed by a NUL at index length; bytes after the NUL are not written.
 * @param[out] length_out Nonnull; receives the token length when found.
 * @return An interop_activation_take_status_t value. A token is returned at most once.
 */
int interop_activation_take(interop_activation_store_t *store, uint32_t process_id, uint64_t now_ms, uint8_t out[InteropActivationTokenBufferBytes], size_t *length_out);

/**
 * @brief Remove every expired pair.
 * @param[in,out] store Store; NULL returns 0.
 * @param[in] now_ms Monotonic milliseconds.
 * @return Number of pairs removed by this call.
 */
size_t interop_activation_expire(interop_activation_store_t *store, uint64_t now_ms);

/**
 * @brief Remove every pair. Clock state and the rollback counter are kept.
 * @param[in,out] store Store; NULL is a no-op.
 */
void interop_activation_clear(interop_activation_store_t *store);

/**
 * @brief Choose the activation token: the first valid of XDG_ACTIVATION_TOKEN, then DESKTOP_STARTUP_ID.
 * @param[in] xdg NUL-terminated XDG_ACTIVATION_TOKEN, or NULL. At most 256 bytes are examined.
 * @param[in] startup NUL-terminated DESKTOP_STARTUP_ID, or NULL. At most 256 bytes are examined.
 * @param[out] token_out Nonnull; receives a pointer into the chosen input when found.
 * @param[out] length_out Nonnull; receives the token length when found.
 * @return An interop_activation_pick_status_t value.
 * @note The result borrows the chosen input; it is valid while that environment string lives.
 */
int interop_activation_pick_token(const char *xdg, const char *startup, const char **token_out, size_t *length_out);

#endif
