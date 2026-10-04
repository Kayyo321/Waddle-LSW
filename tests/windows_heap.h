/** @file windows_heap.h @brief Test-only MSVC debug-heap instrumentation.
 * Forced into guest C units; no production ABI or allocation policy change.
 * Every guest allocation is a CRT client block, owned by its original caller.
 * Listener checkpoints occur after session workers join and peer cleanup.
 */
#ifndef WindowsHeapIncluded
#define WindowsHeapIncluded
#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <crtdbg.h>
#ifndef _DEBUG
#error Windows heap auditing requires the debug CRT
#endif
#ifdef GuestHeapListener
static int __cdecl heap_report(int kind, char *message, int *result) {
    (void)result;
    fputs(message, stderr);
    FILE *report = fopen("build/windows_heap_errors.log", "a");
    if (report != NULL) { fputs(message, report); fclose(report); }
    fflush(stderr);
    if (kind != _CRT_WARN) { ExitProcess(86); }
    return TRUE;
}
static void heap_start(void) {
    _CrtSetReportHook(heap_report);
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF |
                  _CRTDBG_DELAY_FREE_MEM_DF);
}
static void heap_check(void) {
    _CrtMemState state;
    _CrtMemCheckpoint(&state);
    if (!_CrtCheckMemory() || state.lCounts[_CLIENT_BLOCK] != 0 ||
        state.lSizes[_CLIENT_BLOCK] != 0) {
        FILE *report = fopen("build/windows_heap_errors.log", "a");
        if (report != NULL) {
            fprintf(report, "client blocks=%zu bytes=%zu\n",
                    state.lCounts[_CLIENT_BLOCK], state.lSizes[_CLIENT_BLOCK]);
            fclose(report);
        }
        _CrtMemDumpAllObjectsSince(NULL);
        fputs("FAIL: guest heap integrity or retained client allocation\n", stderr);
        ExitProcess(86);
    }
}
static SOCKET WSAAPI heap_accept(SOCKET socket, struct sockaddr *address, int *length) {
    static unsigned completed;
    if (completed == 0) {
        heap_start();
    } else {
        heap_check();
        FILE *report = fopen("build/windows_heap.log", "a");
        if (report == NULL) { ExitProcess(86); }
        fprintf(report, "session %u: 0 allocations, 0 bytes, intact heap\n", completed);
        if (fclose(report) != 0) { ExitProcess(86); }
    }
    completed++;
    return accept(socket, address, length);
}
#define accept heap_accept
#endif
/* Preserve caller file/line metadata; macro names match the CRT API boundary. */
#define malloc(length) _malloc_dbg((length), _CLIENT_BLOCK, __FILE__, __LINE__)
#define calloc(count, length) _calloc_dbg((count), (length), _CLIENT_BLOCK, __FILE__, __LINE__)
#define realloc(pointer, length) _realloc_dbg((pointer), (length), _CLIENT_BLOCK, __FILE__, __LINE__)
#define free(pointer) _free_dbg((pointer), _CLIENT_BLOCK)
#endif
