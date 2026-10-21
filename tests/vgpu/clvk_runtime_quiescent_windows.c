/**
 * @file clvk_runtime_quiescent_windows.c
 * @brief Strict runtime probe with externally observed startup-pool retirement.
 *
 * This separate qualification executable preserves every original cycle oracle.
 * Before the original main's first handle baseline (after its setvbuf, idle-thread
 * setup and TLS/FLS baseline probes), it pauses at one closed-handle UMDH gate.
 * The supervisor must observe both exact OS startup allocations then their
 * retirement before acknowledging within the existing60-second stage budget.
 * No candidate runtime DLL has loaded at that point. This is not a warm-up DLL
 * lifetime, a heap/count exclusion, a tolerance, or acceptance on a short run.
 */
#include <windows.h>

/** @brief Intercept only the sole-main-thread process-count sampling boundary.
 * @param[in] process Borrowed process handle; never closed here.
 * @param[out] count Nonnull original caller-owned count destination.
 * @return Original API result after one exact startup handshake, FALSE on gate
 * failure. No heap or handle ownership escapes; original main cleans up on FALSE.
 * @note Sole main thread only. The static phase flag is never shared with workers.
 */
static BOOL quiescent_handle_count(HANDLE process, PDWORD count);

/* Windows declarations are already included; map only calls inside the unchanged
 * included fixture. All assertion constants, waits and cleanup remain identical. */
#define GetProcessHandleCount quiescent_handle_count
#define main strict_runtime_probe_main
#include "clvk_runtime_windows.c"
#undef main
#undef GetProcessHandleCount

/* Included after undoing the API alias so its marker uses the real count API.
 * Original main has set stream buffering before the first intercepted call. */
#include "dxvk_heap_audit_windows.inc"

static BOOL quiescent_handle_count(HANDLE process, PDWORD count)
{
    static int startup_ready = 0;
    if (!startup_ready) {
        wchar_t directory[1024];
        if (audit_directory(directory, 1024) != 1 ||
            audit_stage(directory, "startup_quiescence")) {
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        startup_ready = 1;
    }
    return GetProcessHandleCount(process, count);
}

/** @brief Run the complete unchanged strict runtime fixture after startup proof.
 * @param[in] argc Argument count of original fixture.
 * @param[in] argv Borrowed original UTF-8 argument vector.
 * @return Original fixture status; success requires all configured cycles and
 * complete natural teardown. The supervisor separately checks startup evidence.
 * @note Process main thread; no owner or reference transfers across the wrapper.
 */
int main(int argc, char **argv)
{
    return strict_runtime_probe_main(argc, argv);
}
