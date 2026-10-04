/** @file guest_pump.h @brief Borrowed worker contexts and pipe pumps. */
#ifndef WaddleGuestPumpH
#define WaddleGuestPumpH
#include "guest_wire.h"
#include "guest_process.h"
/** @brief Borrowed worker context, retained by main until all workers join.
 * @note Input exclusively mutates process->input; output workers own one reader
 * each; termination is atomic and read by main only after input joins. */
typedef struct guest_pump_t {
    guest_wire_t *wire; /**< Nonnull borrowed initialized wire state. */
    guest_process_t *process; /**< Nonnull borrowed launched child resources. */
    unsigned stream; /**< One stdout, two stderr; output workers only. */
    volatile LONG termination; /**< Input updates for accepted termination signals. */
} guest_pump_t;
/** @brief Receive and validate controls, forward stdin/resize/signals, close stdin.
 * @param[in,out] context Nonnull borrowed guest_pump_t, retained until join.
 * @return Win32 thread exit code zero; failure reported through wire event.
 * @note One input worker per session; fixed stack buffer, no allocations. */
DWORD WINAPI guest_input_thread(void *context);
/** @brief Drain one output pipe into bounded frames, then emit exactly one EOF.
 * @param[in] context Nonnull borrowed guest_pump_t, retained until join.
 * @return Win32 thread exit code zero; failure reported through wire event.
 * @note One worker per pipe, fixed stack buffer, no allocations. */
DWORD WINAPI guest_output_thread(void *context);
#endif
