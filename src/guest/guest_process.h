/** @file guest_process.h @brief Win32 session process and handle ownership. */
#ifndef WaddleGuestProcessH
/** @brief Include guard; compile-time marker with no storage or ownership. */
#define WaddleGuestProcessH
#include "guest_environment.h"
/** @brief One session's owned Win32 resources; zero-initialize before launch.
 * @note Main thread owns lifecycle; input/output workers borrow their respective
 * pipe handles until joined. No handle may be closed while its worker is active. */
typedef struct guest_process_t {
    HANDLE process; /**< Owned child process, closed after wait. */
    HANDLE job; /**< Owned kill-on-close job containing all descendants. */
    HANDLE input; /**< Owned parent stdin writer, input thread closes on EOF. */
    HANDLE output[2]; /**< Owned stdout/stderr readers, closed after readers join. */
    HPCON console; /**< Owned ConPTY, closed while output reader drains. */
    DWORD pid; /**< Child PID and raw console process group. */
    DWORD interactive; /**< ConPTY mode flag. */
    ULONGLONG started; /**< Monotonic child start time in milliseconds. */
} guest_process_t;
/** @brief Launch a suspended child into a kill-on-close job, then resume it.
 * @param[out] process Nonnull caller-owned zeroed record; owns handles on success.
 * @param[in] spawn Nonnull immutable validated views retained until this returns.
 * @param[in] executable Nonnull validated UTF-8 argv[0], borrowed.
 * @return 0 success, -1 failure with GetLastError preserved; failures release all
 * allocations/handles and kill any child. Caller must guest_process_close on success.
 * @note Listener main only, before workers start. Temporarily clears/restores the
 * process standard-handle table for ConPTY creation; no concurrent launch or
 * standard-handle mutation is allowed. Output drains concurrently after success. */
int guest_process_launch(guest_process_t *process, const guest_spawn_t *spawn,
                         const uint8_t *executable);
/** @brief Kill child tree, wait and release all remaining owned resources.
 * @param[in,out] process Nonnull session record, handles reset to NULL.
 * @note Main thread only, after workers join. Close output before ConPTY if no
 * reader exists; normal teardown closes ConPTY while reader still drains. Idempotent. */
void guest_process_close(guest_process_t *process);
#endif
