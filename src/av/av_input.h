#ifndef WaddleAvInputH
#define WaddleAvInputH
#include "waddle/av_protocol.h"
/** @brief Fixed physical key registry capacity; evdev codes 1..127 accepted. */
#define AvInputKeys 128u
/** @brief Release-mask bit for all held keyboard keys. */
#define AvInputReleaseKeys 1u
/** @brief Release-mask bit for all held pointer buttons. */
#define AvInputReleaseButtons 2u
/** @brief Borrowed synchronous platform callbacks, same owner thread as state.
 * No callbacks may retain event pointers or recursively mutate the state.
 */
typedef struct av_input_ops_t {
    /** Validate ID/incarnation/process/foreground; activate when nonzero.
     * @param[in,out] context Optional caller-owned context.
     * @param[in] window_id Live exported ID. @param[in] incarnation Create.sequence.
     * @param[in] activate 1 request activation, 0 validate existing foreground,
     * -1 validate identity only before releasing old focus.
     * @return 0 accepted, 1 retired identity for activate=-1, -1 denied/OS failure.
     * The V3 already-admitted boundary additionally permits positive suspension
     * for activate=0/1, with no native insertion. The V2 public entry normalizes
     * any such suspension to terminal failure. No ownership transfer.
     */
    int (*target)(void *context, uint64_t window_id, uint64_t incarnation, int activate);
    /** Inject one event, or release a previously injected key/button.
     * @param[in,out] context Optional borrowed caller context.
     * @param[in] event Nonnull transient validated event. Synthetic releases have
     * serial zero and remain mandatory even after the original target vanishes.
     * @return 0 inserted, 1 not inserted due to observed transition, -1 failure; caller keeps held bit on failure.
     */
    int (*emit)(void *context, const av_message_t *event);
} av_input_ops_t;
/** @brief Caller-owned bounded guest input state, zero initialize per connection.
 * All fields are private to the owner thread; no heap or retained event pointers.
 */
typedef struct av_input_state_t {
    uint64_t window_id; /**< Current focused exported ID, zero if none. */
    uint64_t incarnation; /**< Current Create.sequence, zero if no focus. */
    uint32_t serial; /**< Last accepted wire serial; never wraps. */
    uint8_t keys[AvInputKeys]; /**< Successfully injected keys awaiting release. */
    uint8_t buttons[4]; /**< Successfully injected button IDs 1..3. */
} av_input_state_t;
/** @brief Immutable translation category returned by av_input_translate_key().
 * Values have static table semantics, require no ownership or deallocation,
 * and may be consumed concurrently. Unknown evdev values always return
 * AvKeyTranslationUnsupported.
 */
typedef enum av_key_translation_kind_t {
    /** No supported mapping exists. */ AvKeyTranslationUnsupported,
    /** Set-1 scan byte, with E0 indicated by `extended`. */ AvKeyTranslationScanCode,
    /** Windows virtual key in `virtual_key`; no scan code is implied. */ AvKeyTranslationVirtualKey,
    /** Pause has explicit E1 make/break byte sequences in `pause_make` and `pause_break`. */ AvKeyTranslationPause,
    /** Power/sleep policy suppresses the key; no injection is authorized. */ AvKeyTranslationSuppressed,
} av_key_translation_kind_t;
/** @brief Allocation-free typed translation value for one Linux evdev code.
 * @details Fields not selected by `kind` are zero. Scan-code fields are a
 * single Set-1 byte plus E0 extension flag; Pause fields preserve exact E1
 * sequences. The returned value owns no resources and is safe to copy.
 * @note Pure and thread-safe; does not itself authorize or perform injection.
 */
typedef struct av_key_translation_t {
    /** Result category. */ av_key_translation_kind_t kind;
    /** Set-1 scan byte for AvKeyTranslationScanCode. */ uint8_t scan_code;
    /** Nonzero only for an E0-prefixed scan code. */ uint8_t extended;
    /** Windows VK value for AvKeyTranslationVirtualKey. */ uint16_t virtual_key;
    /** Pause make bytes E1 1D 45 for AvKeyTranslationPause. */ uint8_t pause_make[3];
    /** Pause break bytes E1 9D C5 for AvKeyTranslationPause. */ uint8_t pause_break[3];
} av_key_translation_t;
/** @brief Translate evdev code 1..767 using the frozen host keyboard table.
 * @param[in] key Linux evdev key code; no upper-wire admission is implied.
 * @return By-value typed translation. Unsupported inputs return the explicit
 * unsupported kind and all unused fields are zero.
 * @note Allocation-free, pure, thread-safe. The table is immutable. This
 * function does not inject keys and does not change AV protocol bounds.
 */
av_key_translation_t av_input_translate_key(uint32_t key);
/** @brief Apply one input event after defensive canonical validation.
 * @param[in,out] state Nonnull caller-owned zero-initialized state.
 * @param[in] event Nonnull borrowed input message; never retained.
 * @param[in] ops Nonnull borrowed complete callback table.
 * @param[in,out] context Optional callback context, never retained.
 * @return 0 accepted or safely ignored stale focus, -1 malformed/replayed/OS failure; failures are fatal
 * to the connection and must be followed by reset. Partial releases are tracked.
 * @note Owner thread only; allocation-free. No state may cross connections.
 */
int av_input_apply(av_input_state_t *state, const av_message_t *event,
                   const av_input_ops_t *ops, void *context);
/** @brief Apply an already canonical, serial-admitted legacy-shaped event.
 * @param[in,out] state Nonnull owned state, serial remains unchanged.
 * @param[in] event Nonnull borrowed canonical opcode 7..12; epoch removed by lease core.
 * @param[in] ops Nonnull complete borrowed callbacks.
 * @param[in,out] context Optional borrowed context.
 * @return 0 applied/no-op, 1 suspended without insertion, -1 native failure.
 * @note Owner thread, internal authorization boundary: caller has consumed serial
 * exactly once. Successful insertion is committed before caller reconciles observations.
 */
int av_input_apply_admitted(av_input_state_t *state, const av_message_t *event,
                           const av_input_ops_t *ops, void *context);
/** @brief Release every held key/button, clear focus only when all succeed.
 * @param[in,out] state Nonnull owned state; serial retained until session ends.
 * @param[in] ops Nonnull complete callback table.
 * @param[in,out] context Optional borrowed callback context.
 * @return 0 empty, -1 at least one release failed and remains pending for retry.
 * @note Owner thread; no allocation. Does not activate or validate old foreground.
 */
int av_input_reset(av_input_state_t *state, const av_input_ops_t *ops, void *context);
/** @brief Translate supported Linux physical key to Windows Set-1 scan code.
 * @param[in] key Legacy AV evdev code 1..127. @return Low byte scan code with bit 8 meaning
 * extended E0; zero means the typed result is not an ordinary scan code.
 * Pause and virtual-key results are never coerced into scan codes.
 * Pure/thread-safe, allocation-free. Preserves the existing ABI and does not
 * widen the AV wire codec's 1..127 admission bound.
 */
uint16_t av_input_scan_code(uint32_t key);
/** @brief Whether a duplicate down of this evdev key is an admitted host typematic repeat.
 * @param[in] key Linux evdev code. @return 1 for repeatable keys; 0 for modifiers
 * (Shift, Ctrl, Alt, Meta), lock keys (CapsLock, NumLock, ScrollLock), Pause,
 * suppressed power/sleep, and any code outside 1..767.
 * @note Pure, allocation-free, thread-safe. Mirrors the host policy of
 * keyboard_repeat.zig so the guest admits exactly the repeats the host generates.
 * A duplicate down for a repeatable key is injected as a plain down; a duplicate
 * for any other key stays suppressed.
 */
int av_input_key_repeatable(uint32_t key);
#endif
