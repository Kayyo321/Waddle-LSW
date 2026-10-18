/**
 * @file clvk_runtime_windows.c
 * @brief Private exact-handle-count harness for the C++ runtime DLL probe.
 *
 * This program loads one caller-specified DLL path, invokes its version-1 C
 * export, unloads it, verifies module/export absence, and waits at most 100
 * ten-millisecond intervals for the process handle count to return exactly to
 * its initial value. It does not create graphics, network, or Vulkan objects.
 */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** Compile-time test lifetime count, 1..4096 inclusive. Defaults to eight;
 * stress builds override it without changing the exact handle oracle/budget.
 * No runtime input, allocation or synchronization state is owned. */
#ifndef WaddleRuntimeLifetimeCount
#define WaddleRuntimeLifetimeCount 8
#endif

/** Bounded fixture constants; compile-time immutable values own no resource.
 * No runtime inputs/outputs or synchronization state are associated with them. */
typedef enum {
   ApiVersion = 1,
   LifetimeCount = WaddleRuntimeLifetimeCount,
   HandleWaitPollCount = 100,
   HandleWaitIntervalMs = 10,
   PathCapacity = 32768,
   TlsProbeCapacity = 2048,
} runtime_probe_constants_t;

/** Borrowed optional C ABI export pointer, invoked only after a nonnull lookup.
 * @param[in] api_version Must equal ApiVersion; value is never nullable.
 * @return Zero only for completed runtime assertions, nonzero on failure.
 * @ownership No pointers or allocations cross the ABI. Clear before unload.
 * @thread_safety Call sequentially; the export joins its own worker thread. */
typedef uint32_t(__cdecl *runtime_lifetime_probe_t)(uint32_t api_version);

_Static_assert(LifetimeCount >= 1 && LifetimeCount <= 4096,
               "Runtime lifetime count must be within 1..4096");

_Static_assert(sizeof(FARPROC) == sizeof(runtime_lifetime_probe_t),
               "Windows function pointer ABI mismatch");

/**
 * @brief Convert the one UTF-8 DLL path argument to a bounded UTF-16 path.
 * @param[in] text Non-null UTF-8 command-line argument.
 * @param[out] path Caller-owned array with PathCapacity elements.
 * @return TRUE on complete conversion; FALSE on invalid or oversized input.
 * @ownership No heap allocation; output storage belongs to the caller.
 * @thread_safety Function is reentrant and has no shared state.
 */
static BOOL
convert_dll_path(const char *text, WCHAR path[PathCapacity])
{
   int required;
   if (!text || !path)
      return FALSE;

   required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                  NULL, 0);
   if (required <= 0 || required > PathCapacity)
      return FALSE;

   return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, path,
                              PathCapacity) == required;
}

/**
 * @brief Wait within the fixed budget for an exact process handle count.
 * @param[in] expected_count Baseline count to observe.
 * @param[out] observed_count Receives the last observed count.
 * @param[out] polls_used Receives the number of 10 ms waits performed.
 * @return TRUE only when the exact baseline is observed within the budget.
 * @ownership No handles are acquired; output values are caller-owned.
 * @thread_safety Function is reentrant and has no shared state.
 */
static BOOL
wait_for_handle_baseline(DWORD expected_count,
                         DWORD *observed_count,
                         unsigned *polls_used)
{
   if (!observed_count || !polls_used)
      return FALSE;

   *observed_count = 0;
   *polls_used = 0;
   for (unsigned poll_index = 0; poll_index < HandleWaitPollCount;
        poll_index++) {
      Sleep(HandleWaitIntervalMs);
      if (!GetProcessHandleCount(GetCurrentProcess(), observed_count))
         return FALSE;
      *polls_used = poll_index + 1;
      if (*observed_count == expected_count)
         return TRUE;
   }

   return FALSE;
}

/**
 * @brief Count free process TLS indices without retaining any probe index.
 * @param[out] available Receives the number of allocatable slots.
 * @return TRUE if exhaustion was observed within the fixed 2048-slot bound and
 *         all probe-owned indices were freed; FALSE on bound or free failure.
 * @ownership Each successful TlsAlloc belongs only to this call and is released
 *            with TlsFree on every path. No borrowed TLS index is inspected/freed.
 * @thread_safety Call only while the test's DLL callers/threads are quiescent.
 */
static BOOL
count_free_tls_slots(unsigned *available)
{
   DWORD indices[TlsProbeCapacity];
   unsigned count = 0;
   BOOL exhausted = FALSE;
   BOOL freed = TRUE;
   if (!available)
      return FALSE;
   while (count < sizeof(indices) / sizeof(indices[0])) {
      DWORD index = TlsAlloc();
      if (index == TLS_OUT_OF_INDEXES) {
         exhausted = TRUE;
         break;
      }
      indices[count++] = index;
   }
   *available = count;
   while (count != 0) {
      count--;
      if (!TlsFree(indices[count]))
         freed = FALSE;
   }
   return exhausted && freed;
}

/**
 * @brief Run bounded sequential DLL lifetime cycles and enforce exact balance.
 * @param[in] argc Must equal two.
 * @param[in] argv Contains one UTF-8 DLL path.
 * @return Zero only when each DLL call returns zero, the export/module are
 *         absent after unload, and process handle count equals baseline.
 * @ownership Closes only module references returned by LoadLibraryW. The
 *            current-process pseudo-handle is borrowed and never closed.
 * @thread_safety Single-threaded test process entry point.
 */
int
main(int argc, char **argv)
{
   WCHAR dll_path[PathCapacity] = { 0 };
   WCHAR loaded_path[PathCapacity] = { 0 };
   DWORD baseline_count = 0;
   unsigned baseline_tls = 0;

   (void)setvbuf(stdout, NULL, _IONBF, 0);
   (void)setvbuf(stderr, NULL, _IONBF, 0);

   if (argc != 2 || !convert_dll_path(argc == 2 ? argv[1] : NULL, dll_path)) {
      (void)fprintf(stderr,
                    "usage: runtime_lifetime_probe.exe <exact-utf8-dll-path>\n");
      return 2;
   }
   if (!count_free_tls_slots(&baseline_tls)) {
      (void)fprintf(stderr, "BASELINE_TLS_PROBE_ERROR\n");
      return 1;
   }
   if (!GetProcessHandleCount(GetCurrentProcess(), &baseline_count)) {
      (void)fprintf(stderr, "BASELINE_HANDLE_COUNT_ERROR win32=%lu\n",
                    (unsigned long)GetLastError());
      return 1;
   }
   (void)printf("BASELINE handles=%lu wait_budget_ms=%u free_tls=%u\n",
                (unsigned long)baseline_count,
                (unsigned)(HandleWaitPollCount * HandleWaitIntervalMs), baseline_tls);

   for (unsigned cycle = 1; cycle <= LifetimeCount; cycle++) {
      DWORD before_load = 0;
      DWORD after_unload = 0;
      DWORD path_length;
      unsigned polls_used = 0;
      HMODULE module = NULL;
      FARPROC export_address;
      runtime_lifetime_probe_t probe = NULL;
      uint32_t probe_result;
      BOOL count_returned;
      BOOL module_absent;
      BOOL symbol_absent;
      unsigned after_tls = 0;
      BOOL tls_measured;

      if (!GetProcessHandleCount(GetCurrentProcess(), &before_load)) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=before_count win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         return 1;
      }
      if (before_load != baseline_count) {
         (void)fprintf(stderr,
                       "CYCLE_FAIL cycle=%u stage=before_count baseline=%lu "
                       "observed=%lu\n",
                       cycle, (unsigned long)baseline_count,
                       (unsigned long)before_load);
         return 1;
      }

      module = LoadLibraryW(dll_path);
      if (!module) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=load win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         return 1;
      }

      path_length = GetModuleFileNameW(module, loaded_path, PathCapacity);
      if (path_length == 0 || path_length >= PathCapacity) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=module_path win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         (void)FreeLibrary(module);
         return 1;
      }

      export_address = GetProcAddress(module, "runtime_lifetime_probe");
      if (!export_address) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=export win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         (void)FreeLibrary(module);
         return 1;
      }
      memcpy(&probe, &export_address, sizeof(probe));
      probe_result = probe(ApiVersion);
      if (!FreeLibrary(module)) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=unload win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         return 1;
      }
      module = NULL;
      probe = NULL;
      export_address = NULL;

      module = GetModuleHandleW(loaded_path);
      module_absent = module == NULL;
      if (module_absent) {
         symbol_absent = TRUE;
      } else {
         symbol_absent = GetProcAddress(module, "runtime_lifetime_probe") == NULL;
      }

      count_returned =
         wait_for_handle_baseline(baseline_count, &after_unload, &polls_used);

      tls_measured = count_free_tls_slots(&after_tls);

      (void)printf("CYCLE cycle=%u probe_result=%lu before=%lu after=%lu "
                   "polls=%u exact_handles=%u module_absent=%u "
                   "symbol_absent=%u free_tls=%u tls_balanced=%u\n",
                   cycle, (unsigned long)probe_result,
                   (unsigned long)before_load, (unsigned long)after_unload,
                   polls_used, count_returned ? 1u : 0u,
                   module_absent ? 1u : 0u, symbol_absent ? 1u : 0u, after_tls,
                   tls_measured && after_tls == baseline_tls ? 1u : 0u);

      if (probe_result != 0 || !count_returned || !module_absent ||
          !symbol_absent || !tls_measured || after_tls != baseline_tls) {
         (void)fprintf(stderr, "CYCLE_FAIL cycle=%u\n", cycle);
         return 1;
      }
   }

   (void)printf("RUNTIME_LIFETIME_PROBE PASS cycles=%u baseline_handles=%lu\n",
                (unsigned)LifetimeCount, (unsigned long)baseline_count);
   return 0;
}
