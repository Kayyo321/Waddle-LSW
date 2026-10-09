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
     * @return 0 accepted, 1 retired identity (only identity probe), -1 denied/OS failure. No ownership transfer.
     */
    int (*target)(void *context, uint64_t window_id, uint64_t incarnation, int activate);
    /** Inject one event, or release a previously injected key/button.
     * @param[in,out] context Optional borrowed caller context.
     * @param[in] event Nonnull transient validated event. Synthetic releases have
     * serial zero and remain mandatory even after the original target vanishes.
     * @return 0 successful injection, -1 failure; caller keeps held bit on failure.
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
/** @brief Release every held key/button, clear focus only when all succeed.
 * @param[in,out] state Nonnull owned state; serial retained until session ends.
 * @param[in] ops Nonnull complete callback table.
 * @param[in,out] context Optional borrowed callback context.
 * @return 0 empty, -1 at least one release failed and remains pending for retry.
 * @note Owner thread; no allocation. Does not activate or validate old foreground.
 */
int av_input_reset(av_input_state_t *state, const av_input_ops_t *ops, void *context);
/** @brief Translate supported Linux physical key to Windows Set-1 scan code.
 * @param[in] key Evdev key code. @return Low byte scan code with bit 8 meaning
 * extended E0; zero means unsupported. Pure/thread-safe, allocation-free.
 */
uint16_t av_input_scan_code(uint32_t key);
#endif
