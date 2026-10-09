#ifndef WaddleAvIdentityH
#define WaddleAvIdentityH
#include "waddle/av_protocol.h"
/** @brief Connection capability selected once by the first Create. */
typedef enum av_identity_mode_t {
    AvIdentityUnknown = 0, /**< No window has been announced. */
    AvIdentityLegacy = 1, /**< Old Create, always display-only. */
    AvIdentityModern = 2 /**< CreateV2, immutable lifecycle incarnations. */
} av_identity_mode_t;
/** @brief Caller-owned connection admission state; zero-initialize on reconnect.
 * @note Event-thread-only; contains no owned resources or heap allocations.
 */
typedef struct av_identity_session_t {
    av_identity_mode_t mode; /**< Immutable after first admitted Create. */
    uint64_t high_water; /**< Last admitted modern token, never decremented. */
} av_identity_session_t;
/** @brief Validate capability and strictly increasing CreateV2 admission.
 * @param[in,out] session Nonnull caller-owned state, unchanged on failure.
 * @param[in] message Nonnull borrowed Create or CreateV2, retained during call.
 * @return 0 admitted, -1 malformed, mixed capability or regressed/duplicate token.
 * @note Event-thread only, allocation-free. Caller rejects live ID/pool duplicates
 * before allocating native resources. UINT64_MAX admits once, never wraps.
 */
int av_identity_admit(av_identity_session_t *session, const av_message_t *message);
/** @brief Reserve the next guest connection-local incarnation.
 * @param[in,out] counter Nonnull last issued token, zero at session start.
 * @param[out] token Nonnull caller output, unchanged on error.
 * @return 0 issued, -1 exhausted. Event-thread-only; no allocation or wrap.
 */
int av_identity_next(uint64_t *counter, uint64_t *token);
/** @brief Match a modern lifecycle/control against current admitted metadata.
 * @param[in] current Optional borrowed current metadata, NULL means absent ID.
 * @param[in] message Nonnull borrowed Geometry, Destroy or Close.
 * @return 1 exact ID/incarnation/process match, 0 stale/absent, -1 malformed or
 * zero incarnation. A zero request process is unspecified; nonzero must match.
 * @note Pure, allocation-free, thread-safe; validates before identity lookup.
 */
int av_identity_match(const av_message_t *current, const av_message_t *message);
/** @brief Borrowed synchronous effect callback, called only for a matched identity.
 * @param[in] message Nonnull validated matched message, transient.
 * @param[in,out] context Optional borrowed caller context.
 * @return 0 success, -1 native failure. Must not retain message; event thread only.
 */
typedef int (*av_identity_effect_t)(const av_message_t *message, void *context);
/** @brief Apply a modern control only after portable identity admission.
 * @param[in] current Optional borrowed current metadata, NULL means absent.
 * @param[in] message Nonnull borrowed Geometry or Close.
 * @param[in] effect Nonnull callback, invoked once only for a current match.
 * @param[in,out] context Optional borrowed effect context.
 * @return 0 accepted or stale no-op, -1 malformed/type/native failure.
 * @note Event-thread-only; no allocations, stale requests invoke no effect.
 */
int av_identity_apply(const av_message_t *current, const av_message_t *message,
                      av_identity_effect_t effect, void *context);
#endif
