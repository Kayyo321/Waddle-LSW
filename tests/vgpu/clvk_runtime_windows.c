/**
 * @file clvk_runtime_windows.c
 * @brief Native exact-lifetime, heap, TLS/FLS and thread-boundary harness.
 *
 * The executable owns its synchronization objects, raw threads, DLL references,
 * and temporary TLS/FLS probe slots. DLL-owned resources remain inside the DLL.
 */
#include <windows.h>
#include <heapapi.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef WaddleRuntimeLifetimeCount
#define WaddleRuntimeLifetimeCount 8
#endif
#ifndef WaddleRuntimeIdleThreadCount
#define WaddleRuntimeIdleThreadCount 4
#endif

#ifdef WaddleRuntimeHeapAudit
#include "dxvk_heap_audit_windows.inc"
#endif

enum {
   ApiVersion = 2,
   ForeignThreadCount = 4,
   IdleThreadCapacity = 16,
   HeapHandleCapacity = 128,
   TlsProbeCapacity = 2048,
   FlsProbeCapacity = 4097,
   HandleWaitPollCount = 100,
   HandleWaitIntervalMs = 10,
   HeapWaitPollCount = 100,
   HeapWaitIntervalMs = 10,
   ThreadWaitTimeoutMs = 10000,
   PathCapacity = 32768,
   StatusOk = 0,
};

typedef struct heap_snapshot_t {
   uint64_t busy_bytes;
   uint64_t busy_entries;
   DWORD win32_error;
} heap_snapshot_t;

typedef struct idle_thread_context_t {
   HANDLE ready_event;
   HANDLE release_event;
   volatile LONG *ready_count;
   volatile LONG *thread_error;
} idle_thread_context_t;

typedef struct foreign_thread_context_t {
   uint32_t (__cdecl *probe)(uint32_t, uint32_t, uintptr_t, uintptr_t);
   uint32_t slot;
   uintptr_t ready_event;
   uintptr_t release_event;
   uint32_t result;
} foreign_thread_context_t;

typedef struct primary_thread_context_t {
   uint32_t (__cdecl *probe)(uint32_t);
   uint32_t result;
} primary_thread_context_t;

typedef struct module_operation_context_t {
   const WCHAR *path;
   HMODULE module;
   DWORD error;
   BOOL succeeded;
} module_operation_context_t;

typedef uint32_t(__cdecl *runtime_lifetime_probe_t)(uint32_t);
typedef uint32_t(__cdecl *runtime_foreign_thread_probe_t)(uint32_t, uint32_t,
                                                           uintptr_t,
                                                           uintptr_t);

/**
 * @brief Fail closed without running process CRT teardown after a live-thread timeout.
 * @param[in] message Static diagnostic string.
 * @param[in] exit_code Nonzero test-process exit code.
 * @return Never returns.
 * @ownership Uses borrowed stderr/current-process handles only; OS tears down.
 * @thread_safety Fatal-path helper uses no shared C runtime state.
 */
static _Noreturn void
fail_closed_process(const char *message, DWORD exit_code)
{
   HANDLE error_handle = GetStdHandle(STD_ERROR_HANDLE);
   DWORD written = 0;
   if (error_handle && error_handle != INVALID_HANDLE_VALUE && message) {
      DWORD length = 0;
      while (message[length] != '\0')
         length++;
      (void)WriteFile(error_handle, message, length, &written, NULL);
      (void)FlushFileBuffers(error_handle);
   }
   (void)TerminateProcess(GetCurrentProcess(), exit_code);
   for (;;)
      Sleep(1000);
}

_Static_assert(WaddleRuntimeLifetimeCount >= 3 && WaddleRuntimeLifetimeCount <= 4096,
               "Runtime lifetime count must be within 3..4096");
_Static_assert(WaddleRuntimeIdleThreadCount >= 0 &&
                  WaddleRuntimeIdleThreadCount <= IdleThreadCapacity,
               "Runtime idle thread count must be within 0..16");
#ifdef WaddleRuntimeHeapAudit
_Static_assert(WaddleRuntimeLifetimeCount <= 8,
               "UMDH capture runs must be bounded to at most eight cycles");
#endif
_Static_assert(sizeof(FARPROC) == sizeof(runtime_lifetime_probe_t),
               "Windows function pointer ABI mismatch");
_Static_assert(sizeof(FARPROC) == sizeof(runtime_foreign_thread_probe_t),
               "Windows foreign function pointer ABI mismatch");

/**
 * @brief Convert one bounded UTF-8 path to UTF-16 without heap allocation.
 * @param[in] text Non-null UTF-8 command-line string.
 * @param[out] path Caller-owned array with PathCapacity elements.
 * @return TRUE for complete conversion; FALSE for invalid or oversized input.
 * @ownership Uses only caller-owned stack storage.
 * @thread_safety Reentrant; no shared state.
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
 * @brief Count currently free process TLS indices and free every probe index.
 * @param[out] available Receives the number of successful temporary indices.
 * @return TRUE only when exhaustion and all TlsFree calls succeed.
 * @ownership Every successful TlsAlloc belongs to this call and is freed.
 * @thread_safety Call with all test-owned DLL worker calls quiescent.
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
   while (count < TlsProbeCapacity) {
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
 * @brief Count free process FLS indices and free every probe index.
 * @param[out] available Receives the number of successful temporary indices.
 * @return TRUE only when exhaustion and all FlsFree calls succeed.
 * @ownership Every successful FlsAlloc belongs to this call and is freed.
 * @thread_safety Call with all test-owned DLL worker calls quiescent.
 */
static BOOL
count_free_fls_slots(unsigned *available)
{
   DWORD indices[FlsProbeCapacity];
   unsigned count = 0;
   BOOL exhausted = FALSE;
   BOOL freed = TRUE;
   if (!available)
      return FALSE;
   while (count < FlsProbeCapacity) {
      DWORD index = FlsAlloc(NULL);
      if (index == FLS_OUT_OF_INDEXES) {
         exhausted = TRUE;
         break;
      }
      indices[count++] = index;
   }
   *available = count;
   while (count != 0) {
      count--;
      if (!FlsFree(indices[count]))
         freed = FALSE;
   }
   return exhausted && freed;
}

/**
 * @brief Sum busy bytes and entries in a quiescent process-heap enumeration.
 * @param[out] snapshot Receives aggregate busy bytes/entries or Win32 error.
 * @return TRUE only if enumeration, every lock/walk/unlock, and sums succeed.
 * @ownership Heap handles are borrowed; fixed handle array and counters are local.
 * @thread_safety Call after test-owned DLL calls retire. Heap enumeration and
 * walks are not an atomic process-wide snapshot: unexpected external heap
 * mutation can invalidate membership/handles. This is a diagnostic tuple;
 * attributed native heap evidence remains a separate acceptance requirement.
 */
static BOOL
capture_heap_snapshot(heap_snapshot_t *snapshot)
{
   HANDLE heaps[HeapHandleCapacity];
   ULONG heap_count;
   ULONG copied_heap_count;
   if (!snapshot)
      return FALSE;
   snapshot->busy_bytes = 0;
   snapshot->busy_entries = 0;
   snapshot->win32_error = ERROR_SUCCESS;
   heap_count = GetProcessHeaps(0, NULL);
   if (heap_count == 0) {
      snapshot->win32_error = GetLastError();
      return FALSE;
   }
   if (heap_count > HeapHandleCapacity) {
      snapshot->win32_error = ERROR_INSUFFICIENT_BUFFER;
      return FALSE;
   }
   copied_heap_count = GetProcessHeaps(heap_count, heaps);
   if (copied_heap_count != heap_count) {
      snapshot->win32_error = GetLastError();
      if (copied_heap_count > HeapHandleCapacity)
         snapshot->win32_error = ERROR_INSUFFICIENT_BUFFER;
      return FALSE;
   }

   for (ULONG heap_index = 0; heap_index < heap_count; heap_index++) {
      PROCESS_HEAP_ENTRY entry;
      BOOL walk_ok = TRUE;
      BOOL walk_finished = FALSE;
      BOOL unlock_ok;
      DWORD walk_error = ERROR_SUCCESS;
      DWORD unlock_error = ERROR_SUCCESS;
      memset(&entry, 0, sizeof(entry));
      if (!HeapLock(heaps[heap_index])) {
         snapshot->win32_error = GetLastError();
         return FALSE;
      }
      SetLastError(ERROR_SUCCESS);
      while (HeapWalk(heaps[heap_index], &entry)) {
         if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) != 0) {
            if (UINT64_MAX - snapshot->busy_bytes < entry.cbData ||
                snapshot->busy_entries == UINT64_MAX) {
               walk_ok = FALSE;
               walk_error = ERROR_ARITHMETIC_OVERFLOW;
               break;
            }
            snapshot->busy_bytes += (uint64_t)entry.cbData;
            snapshot->busy_entries++;
         }
      }
      if (walk_ok) {
         walk_error = GetLastError();
         walk_finished = walk_error == ERROR_NO_MORE_ITEMS;
         if (!walk_finished)
            walk_ok = FALSE;
      }
      unlock_ok = HeapUnlock(heaps[heap_index]);
      if (!unlock_ok)
         unlock_error = GetLastError();
      if (!walk_ok || !unlock_ok) {
         snapshot->win32_error = !walk_ok ? walk_error : unlock_error;
         return FALSE;
      }
   }
   return TRUE;
}

/**
 * @brief Compare the busy heap tuple exactly.
 * @param[in] left First fully captured snapshot.
 * @param[in] right Second fully captured snapshot.
 * @return TRUE when both bytes and busy-entry count match.
 * @ownership No resources.
 * @thread_safety Pure function.
 */
static BOOL
heap_snapshots_equal(const heap_snapshot_t *left, const heap_snapshot_t *right)
{
   return left && right && left->busy_bytes == right->busy_bytes &&
          left->busy_entries == right->busy_entries;
}

/** @brief Prove native heap enumeration detects and then retires an owned block.
 * @return Zero only if baseline, deliberate retained block, exact restoration
 * and private-heap destruction all succeed; no DLL is loaded.
 * @ownership Owns one private heap and one 12345-byte block, freed on every path.
 * @thread_safety Run before any harness-owned worker exists. Diagnostic only. */
static int
run_heap_controls(void)
{
   heap_snapshot_t before = { 0 }, retained = { 0 }, after = { 0 };
   HANDLE heap = HeapCreate(0, 0, 0);
   void *block = NULL;
   BOOL passed = FALSE;
   if (!heap)
      goto cleanup;
   if (!capture_heap_snapshot(&before))
      goto cleanup;
   block = HeapAlloc(heap, 0, 12345);
   if (!block || !capture_heap_snapshot(&retained))
      goto cleanup;
   if (retained.busy_bytes < before.busy_bytes ||
       retained.busy_bytes - before.busy_bytes < 12345 ||
       retained.busy_entries <= before.busy_entries ||
       heap_snapshots_equal(&before, &retained))
      goto cleanup;
   if (!HeapFree(heap, 0, block))
      goto cleanup;
   block = NULL;
   if (!capture_heap_snapshot(&after) || !heap_snapshots_equal(&before, &after))
      goto cleanup;
   passed = TRUE;
cleanup:
   if (block && !HeapFree(heap, 0, block))
      passed = FALSE;
   if (heap && !HeapDestroy(heap))
      passed = FALSE;
   if (!passed) {
      (void)fprintf(stderr, "HEAP_CONTROLS FAIL before_error=%lu retained_error=%lu after_error=%lu win32=%lu\n",
                    (unsigned long)before.win32_error, (unsigned long)retained.win32_error,
                    (unsigned long)after.win32_error, (unsigned long)GetLastError());
      return 1;
   }
   (void)printf("HEAP_CONTROLS PASS owned_bytes=12345 retained_detected=1 exact_restoration=1\n");
   return 0;
}

/**
 * @brief Wait up to 100 x 10 ms for the heap tuple to equal its reference.
 * @param[in] expected Reference tuple, or NULL to require two consecutive equal
 *                      snapshots and establish a stable post-cycle-2 tuple.
 * @param[out] observed Last captured tuple.
 * @param[out] polls_used Number of waits used.
 * @return TRUE on exact/stable tuple; FALSE on API error or exhausted budget.
 * @ownership No handles are acquired; snapshots are caller-owned stack values.
 * @thread_safety Requires all harness heap-mutating activity to be quiescent.
 */
static BOOL
wait_for_heap_snapshot(const heap_snapshot_t *expected,
                       heap_snapshot_t *observed,
                       unsigned *polls_used)
{
   heap_snapshot_t previous = { 0, 0, ERROR_SUCCESS };
   BOOL have_previous = FALSE;
   if (!observed || !polls_used)
      return FALSE;
   *observed = previous;
   *polls_used = 0;
   for (unsigned poll = 0; poll < HeapWaitPollCount; poll++) {
      Sleep(HeapWaitIntervalMs);
      if (!capture_heap_snapshot(observed))
         return FALSE;
      *polls_used = poll + 1;
      if (expected && heap_snapshots_equal(expected, observed))
         return TRUE;
      if (!expected && have_previous &&
          heap_snapshots_equal(&previous, observed))
         return TRUE;
      previous = *observed;
      have_previous = TRUE;
   }
   return FALSE;
}

/**
 * @brief Wait for exact process handle count using the original fixed budget.
 * @param[in] expected_count Baseline to observe.
 * @param[out] observed_count Last process count observed.
 * @param[out] polls_used Number of 10 ms sleeps.
 * @return TRUE only when exact equality is observed within the budget.
 * @ownership Uses the borrowed current-process pseudo-handle only.
 * @thread_safety No shared state.
 */
static BOOL
wait_for_handle_baseline(DWORD expected_count, DWORD *observed_count,
                         unsigned *polls_used)
{
   if (!observed_count || !polls_used)
      return FALSE;
   *observed_count = 0;
   *polls_used = 0;
   for (unsigned poll = 0; poll < HandleWaitPollCount; poll++) {
      Sleep(HandleWaitIntervalMs);
      if (!GetProcessHandleCount(GetCurrentProcess(), observed_count))
         return FALSE;
      *polls_used = poll + 1;
      if (*observed_count == expected_count)
         return TRUE;
   }
   return FALSE;
}

/**
 * @brief Keep one harness-owned pre-existing thread idle until release.
 * @param[in,out] opaque Pointer to caller-owned idle_thread_context_t.
 * @return Zero on release; nonzero when signaling/readiness fails.
 * @ownership Borrows events and counters from main; owns no handle.
 * @thread_safety Each thread gets a distinct immutable context.
 */
static DWORD WINAPI
idle_thread_main(LPVOID opaque)
{
   idle_thread_context_t *context = (idle_thread_context_t *)opaque;
   if (!context || !context->ready_event || !context->release_event ||
       !context->ready_count || !context->thread_error)
      return 1;
   if (InterlockedIncrement(context->ready_count) == WaddleRuntimeIdleThreadCount &&
       !SetEvent(context->ready_event)) {
      InterlockedExchange(context->thread_error, 1);
      return 2;
   }
   return WaitForSingleObject(context->release_event, INFINITE) == WAIT_OBJECT_0
             ? 0
             : 3;
}

/**
 * @brief Invoke the borrowed v2 foreign-thread export from raw CreateThread.
 * @param[in,out] opaque Pointer to caller-owned foreign_thread_context_t.
 * @return Zero if the DLL export returns StatusOk; otherwise nonzero.
 * @ownership Borrows the export and events, valid until the caller joins us.
 * @thread_safety Each thread receives a unique slot in the fixed context array.
 */
static DWORD WINAPI
foreign_thread_main(LPVOID opaque)
{
   foreign_thread_context_t *context = (foreign_thread_context_t *)opaque;
   if (!context || !context->probe)
      return 1;
   context->result = context->probe(ApiVersion, context->slot,
                                    context->ready_event,
                                    context->release_event);
   return context->result == StatusOk ? 0 : 2;
}

/**
 * @brief Execute the primary DLL export on an owned watchdog-observed thread.
 * @param[in,out] opaque Caller-owned primary_thread_context_t.
 * @return The export result as the Win32 thread exit code.
 * @ownership Borrows the export pointer until main joins this thread.
 * @thread_safety One context and thread per DLL load.
 */
static DWORD WINAPI
primary_thread_main(LPVOID opaque)
{
   primary_thread_context_t *context = (primary_thread_context_t *)opaque;
   if (!context || !context->probe)
      return UINT32_MAX;
   context->result = context->probe(ApiVersion);
   return context->result;
}

/**
 * @brief Perform candidate LoadLibraryW under a bounded native-thread watchdog.
 * @param[in,out] opaque Caller-owned module_operation_context_t.
 * @return Zero after the operation completes; nonzero on load failure.
 * @ownership Stores the module reference in caller-owned context.
 * @thread_safety One operation context per owned thread.
 */
static DWORD WINAPI
load_module_main(LPVOID opaque)
{
   module_operation_context_t *context = (module_operation_context_t *)opaque;
   if (!context || !context->path)
      return 1;
   context->module = LoadLibraryW(context->path);
   context->succeeded = context->module != NULL;
   context->error = context->succeeded ? ERROR_SUCCESS : GetLastError();
   return context->succeeded ? 0 : 1;
}

/**
 * @brief Perform candidate FreeLibrary under a bounded native-thread watchdog.
 * @param[in,out] opaque Caller-owned module_operation_context_t.
 * @return Zero after successful release; nonzero on release failure.
 * @ownership Releases exactly the module reference stored in context.
 * @thread_safety One operation context per owned thread.
 */
static DWORD WINAPI
free_module_main(LPVOID opaque)
{
   module_operation_context_t *context = (module_operation_context_t *)opaque;
   if (!context || !context->module)
      return 1;
   context->succeeded = FreeLibrary(context->module);
   context->error = context->succeeded ? ERROR_SUCCESS : GetLastError();
   if (context->succeeded)
      context->module = NULL;
   return context->succeeded ? 0 : 1;
}

/**
 * @brief Run an owned thread procedure with a strict completion watchdog.
 * @param[in] procedure Win32 procedure with caller-owned input context.
 * @param[in,out] context Caller-owned state that remains live until join.
 * @param[in] timeout_message Static fatal diagnostic if the bounded wait fails.
 * @param[in] exit_code Nonzero test process exit code on watchdog failure.
 * @return TRUE after the thread ended and its owned handle closed.
 * @ownership Closes only the thread handle created by this function.
 * @thread_safety Context must not be used concurrently outside the operation.
 */
static BOOL
run_bounded_thread(LPTHREAD_START_ROUTINE procedure, void *context,
                   const char *timeout_message, DWORD exit_code)
{
   HANDLE thread = CreateThread(NULL, 0, procedure, context, 0, NULL);
   if (!thread) {
      (void)fprintf(stderr, "WATCHDOG_ERROR stage=create_thread win32=%lu\n",
                    (unsigned long)GetLastError());
      (void)fflush(stderr);
      return FALSE;
   }
   if (WaitForSingleObject(thread, ThreadWaitTimeoutMs) != WAIT_OBJECT_0)
      fail_closed_process(timeout_message, exit_code);
   if (!CloseHandle(thread))
      fail_closed_process("WATCHDOG_FATAL stage=thread_handle_close\n",
                          exit_code + 1);
   return TRUE;
}

/**
 * @brief Join all non-null thread handles, then close each owned handle.
 * @param[in,out] threads Fixed caller-owned handle array.
 * @param[in] thread_count Number of entries to inspect.
 * @return TRUE only when every thread terminated and every handle closed.
 * @ownership Closes only non-null handles in the provided owned array.
 * @thread_safety Caller must not concurrently wait/close the same handles.
 */
static BOOL
join_and_close_threads(HANDLE *threads, unsigned thread_count)
{
   BOOL all_ok = TRUE;
   if (!threads)
      return FALSE;
   for (unsigned index = 0; index < thread_count; index++) {
      if (!threads[index])
         continue;
      DWORD wait_status = WaitForSingleObject(threads[index],
                                               ThreadWaitTimeoutMs);
      if (wait_status != WAIT_OBJECT_0) {
         all_ok = FALSE;
         /* Keep the handle and fail-close before unloading or CRT teardown. */
         return FALSE;
      }
      if (!CloseHandle(threads[index]))
         all_ok = FALSE;
      threads[index] = NULL;
   }
   return all_ok;
}

/**
 * @brief Run the v2 DLL runtime probe and exact per-cycle ownership oracles.
 * @param[in] argc Exactly two arguments: executable and UTF-8 DLL path, or
 *                  --heap-controls for the DLL-free native heap regression.
 * @param[in] argv Borrowed process argument vector.
 * @return Zero only when all per-cycle handle/TLS/FLS/module/heap gates pass.
 * @ownership Closes only handles/modules allocated by this harness; process
 *            pseudo-handles and DLL-owned objects are never closed by the harness.
 * @thread_safety Main-thread entry point; raw workers are joined before unload.
 */
int
main(int argc, char **argv)
{
   WCHAR dll_path[PathCapacity] = { 0 };
   WCHAR loaded_path[PathCapacity] = { 0 };
   HANDLE idle_ready_event = NULL;
   HANDLE idle_release_event = NULL;
   HANDLE idle_threads[IdleThreadCapacity] = { NULL };
   idle_thread_context_t idle_contexts[IdleThreadCapacity];
   const unsigned idle_thread_count = WaddleRuntimeIdleThreadCount;
#ifdef WaddleRuntimeHeapAudit
   wchar_t audit_path[1024] = { 0 };
   int audit_enabled = 0;
#endif
   HANDLE foreign_ready_event = NULL;
   HANDLE foreign_release_event = NULL;
   HANDLE foreign_threads[ForeignThreadCount] = { NULL, NULL, NULL, NULL };
   foreign_thread_context_t foreign_contexts[ForeignThreadCount];
   HMODULE msvcrt_module = NULL;
   HMODULE module = NULL;
   runtime_lifetime_probe_t lifetime_probe = NULL;
   runtime_foreign_thread_probe_t foreign_probe = NULL;
   primary_thread_context_t primary_context;
   module_operation_context_t module_context;
   volatile LONG idle_ready_count = 0;
   volatile LONG idle_thread_error = 0;
   DWORD baseline_handle_count = 0;
   unsigned baseline_tls_slots = 0;
   unsigned baseline_fls_slots = 0;
   heap_snapshot_t first_load_heap_baseline = { 0, 0, ERROR_SUCCESS };
   heap_snapshot_t heap_reference = { 0, 0, ERROR_SUCCESS };
   BOOL heap_reference_ready = FALSE;
   BOOL result_ok = FALSE;
   unsigned idle_count = 0;
   unsigned foreign_count = 0;
   unsigned loaded_path_length = 0;
   (void)setvbuf(stdout, NULL, _IONBF, 0);
   (void)setvbuf(stderr, NULL, _IONBF, 0);

   if (argc == 2 && strcmp(argv[1], "--heap-controls") == 0)
      return run_heap_controls();

   if (argc != 2 || !convert_dll_path(argc == 2 ? argv[1] : NULL, dll_path)) {
      (void)fprintf(stderr,
                    "usage: clvk_runtime_windows.exe <exact-utf8-dll-path>|--heap-controls\n");
      return 2;
   }
#ifdef WaddleRuntimeHeapAudit
   audit_enabled = audit_directory(audit_path, 1024);
   if (audit_enabled != 1) {
      (void)fprintf(stderr, "SETUP_ERROR stage=required_heap_audit_directory\n");
      return 2;
   }
#endif

   msvcrt_module = LoadLibraryW(L"msvcrt.dll");
   if (!msvcrt_module) {
      (void)fprintf(stderr, "SETUP_ERROR stage=pin_msvcrt win32=%lu\n",
                    (unsigned long)GetLastError());
      goto cleanup;
   }
   idle_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
   idle_release_event = CreateEventW(NULL, TRUE, FALSE, NULL);
   if (!idle_ready_event || !idle_release_event) {
      (void)fprintf(stderr, "SETUP_ERROR stage=idle_events win32=%lu\n",
                    (unsigned long)GetLastError());
      goto cleanup;
   }
   for (unsigned index = 0; index < idle_thread_count; index++) {
      idle_contexts[index].ready_event = idle_ready_event;
      idle_contexts[index].release_event = idle_release_event;
      idle_contexts[index].ready_count = &idle_ready_count;
      idle_contexts[index].thread_error = &idle_thread_error;
      idle_threads[index] = CreateThread(NULL, 0, idle_thread_main,
                                         &idle_contexts[index], 0, NULL);
      if (!idle_threads[index]) {
         (void)fprintf(stderr,
                       "SETUP_ERROR stage=idle_thread_create slot=%u win32=%lu\n",
                       index, (unsigned long)GetLastError());
         goto cleanup;
      }
      idle_count++;
   }
   if ((idle_thread_count &&
        WaitForSingleObject(idle_ready_event, ThreadWaitTimeoutMs) !=
           WAIT_OBJECT_0) ||
       InterlockedCompareExchange(&idle_thread_error, 0, 0) != 0 ||
       InterlockedCompareExchange(&idle_ready_count, 0, 0) != (LONG)idle_thread_count) {
      (void)fprintf(stderr, "SETUP_ERROR stage=idle_threads_ready win32=%lu\n",
                    (unsigned long)GetLastError());
      goto cleanup;
   }
#ifdef WaddleRuntimeHeapAudit
   if (audit_stage(audit_path, "baseline"))
      goto cleanup;
#endif

   if (!count_free_tls_slots(&baseline_tls_slots)) {
      (void)fprintf(stderr, "BASELINE_TLS_PROBE_ERROR\n");
      goto cleanup;
   }
   if (!count_free_fls_slots(&baseline_fls_slots)) {
      (void)fprintf(stderr, "BASELINE_FLS_PROBE_ERROR\n");
      goto cleanup;
   }
   if (!GetProcessHandleCount(GetCurrentProcess(), &baseline_handle_count)) {
      (void)fprintf(stderr, "BASELINE_HANDLE_COUNT_ERROR win32=%lu\n",
                    (unsigned long)GetLastError());
      goto cleanup;
   }
   if (!capture_heap_snapshot(&first_load_heap_baseline)) {
      (void)fprintf(stderr,
                    "BASELINE_HEAP_SNAPSHOT_ERROR win32=%lu\n",
                    (unsigned long)first_load_heap_baseline.win32_error);
      goto cleanup;
   }
   (void)printf("BASELINE handles=%lu free_tls=%u free_fls=%u idle_threads=%u "
                "preload_heap_bytes=%llu preload_heap_entries=%llu "
                "heap_policy=warmup_cycle_1_reference_cycle_2\n",
                (unsigned long)baseline_handle_count, baseline_tls_slots,
                baseline_fls_slots, idle_thread_count,
                (unsigned long long)first_load_heap_baseline.busy_bytes,
                (unsigned long long)first_load_heap_baseline.busy_entries);

   for (unsigned cycle = 1; cycle <= WaddleRuntimeLifetimeCount; cycle++) {
      DWORD before_load = 0;
      DWORD after_unload = 0;
      unsigned handle_polls = 0;
      unsigned after_tls = 0;
      unsigned after_fls = 0;
      BOOL handles_balanced;
      BOOL tls_measured;
      BOOL fls_measured;
      BOOL module_absent;
      BOOL lifetime_symbol_absent;
      BOOL foreign_symbol_absent;
      uint32_t lifetime_result = UINT32_MAX;
      heap_snapshot_t heap_observed = { 0, 0, ERROR_SUCCESS };
      unsigned heap_polls = 0;
      BOOL heap_ok = FALSE;
      BOOL foreign_join_ok = TRUE;

      if (!GetProcessHandleCount(GetCurrentProcess(), &before_load) ||
          before_load != baseline_handle_count) {
         (void)fprintf(stderr,
                       "CYCLE_FAIL cycle=%u stage=before_load baseline=%lu "
                       "observed=%lu win32=%lu\n",
                       cycle, (unsigned long)baseline_handle_count,
                       (unsigned long)before_load,
                       (unsigned long)GetLastError());
         goto cleanup;
      }
      module_context.path = dll_path;
      module_context.module = NULL;
      module_context.error = ERROR_SUCCESS;
      module_context.succeeded = FALSE;
      if (!run_bounded_thread(load_module_main, &module_context,
                              "WATCHDOG_FATAL stage=dll_load_timeout\n",
                              0xe8) || !module_context.succeeded) {
         (void)fprintf(stderr, "CYCLE_ERROR cycle=%u stage=load win32=%lu\n",
                       cycle, (unsigned long)module_context.error);
         goto cleanup;
      }
      module = module_context.module;
      loaded_path_length = GetModuleFileNameW(module, loaded_path, PathCapacity);
      if (loaded_path_length == 0 || loaded_path_length >= PathCapacity) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=module_path win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         goto cleanup;
      }

      FARPROC lifetime_address = GetProcAddress(module,
                                                "runtime_lifetime_probe");
      FARPROC foreign_address = GetProcAddress(module,
                                               "runtime_foreign_thread_probe");
      if (!lifetime_address || !foreign_address) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=exports win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         goto cleanup;
      }
      memcpy(&lifetime_probe, &lifetime_address, sizeof(lifetime_probe));
      memcpy(&foreign_probe, &foreign_address, sizeof(foreign_probe));
      primary_context.probe = lifetime_probe;
      primary_context.result = UINT32_MAX;
      if (!run_bounded_thread(primary_thread_main, &primary_context,
                              "WATCHDOG_FATAL stage=primary_probe_timeout\n",
                              0xe9))
         goto cleanup;
      lifetime_result = primary_context.result;
      if (lifetime_result != StatusOk) {
         (void)fprintf(stderr,
                       "CYCLE_FAIL cycle=%u stage=lifetime_probe status=%lu\n",
                       cycle, (unsigned long)lifetime_result);
         goto cleanup;
      }

      foreign_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
      foreign_release_event = CreateEventW(NULL, TRUE, FALSE, NULL);
      if (!foreign_ready_event || !foreign_release_event) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=foreign_events win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         goto cleanup;
      }
      for (unsigned slot = 0; slot < ForeignThreadCount; slot++) {
         foreign_contexts[slot].probe = foreign_probe;
         foreign_contexts[slot].slot = slot;
         foreign_contexts[slot].ready_event = (uintptr_t)foreign_ready_event;
         foreign_contexts[slot].release_event = (uintptr_t)foreign_release_event;
         foreign_contexts[slot].result = UINT32_MAX;
         foreign_threads[slot] = CreateThread(NULL, 0, foreign_thread_main,
                                               &foreign_contexts[slot], 0, NULL);
         if (!foreign_threads[slot]) {
            (void)fprintf(stderr,
                          "CYCLE_ERROR cycle=%u stage=foreign_thread_create "
                          "slot=%u win32=%lu\n",
                          cycle, slot, (unsigned long)GetLastError());
            goto cleanup;
         }
         foreign_count++;
      }
      if (WaitForMultipleObjects(1, &foreign_ready_event, TRUE,
                                 ThreadWaitTimeoutMs) != WAIT_OBJECT_0) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=foreign_threads_ready "
                       "win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         goto cleanup;
      }
      if (!SetEvent(foreign_release_event)) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=foreign_release win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         goto cleanup;
      }
      foreign_join_ok = join_and_close_threads(foreign_threads, foreign_count);
      foreign_count = 0;
      if (!foreign_join_ok) {
         (void)fprintf(stderr,
                       "CYCLE_FATAL cycle=%u stage=foreign_thread_join\n", cycle);
         (void)fflush(stderr);
         fail_closed_process("WATCHDOG_FATAL stage=foreign_thread_live\n", 0xeb);
      }
      for (unsigned slot = 0; slot < ForeignThreadCount; slot++) {
         if (foreign_contexts[slot].result != StatusOk) {
            (void)fprintf(stderr,
                          "CYCLE_FAIL cycle=%u stage=foreign_probe slot=%u "
                          "status=%lu\n",
                          cycle, slot,
                          (unsigned long)foreign_contexts[slot].result);
            goto cleanup;
         }
      }
      BOOL ready_closed = CloseHandle(foreign_ready_event);
      BOOL release_closed = CloseHandle(foreign_release_event);
      if (ready_closed)
         foreign_ready_event = NULL;
      if (release_closed)
         foreign_release_event = NULL;
      if (!ready_closed || !release_closed) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=foreign_event_close "
                       "win32=%lu\n",
                       cycle, (unsigned long)GetLastError());
         goto cleanup;
      }
#ifdef WaddleRuntimeHeapAudit
      char audit_name[64];
      int audit_length = snprintf(audit_name, sizeof audit_name, "loaded_%u", cycle);
      if (audit_length <= 0 || (size_t)audit_length >= sizeof audit_name ||
          audit_stage(audit_path, audit_name))
         goto cleanup;
#endif
      lifetime_probe = NULL;
      foreign_probe = NULL;
      lifetime_address = NULL;
      foreign_address = NULL;
      module_context.path = NULL;
      module_context.module = module;
      module_context.error = ERROR_SUCCESS;
      module_context.succeeded = FALSE;
      if (!run_bounded_thread(free_module_main, &module_context,
                              "WATCHDOG_FATAL stage=dll_unload_timeout\n",
                              0xea) || !module_context.succeeded) {
         (void)fprintf(stderr, "CYCLE_ERROR cycle=%u stage=unload win32=%lu\n",
                       cycle, (unsigned long)module_context.error);
         module = module_context.module;
         goto cleanup;
      }
      module = NULL;

      module = GetModuleHandleW(loaded_path);
      module_absent = module == NULL;
      if (module_absent) {
         lifetime_symbol_absent = TRUE;
         foreign_symbol_absent = TRUE;
      } else {
         lifetime_symbol_absent =
            GetProcAddress(module, "runtime_lifetime_probe") == NULL;
         foreign_symbol_absent =
            GetProcAddress(module, "runtime_foreign_thread_probe") == NULL;
      }
      module = NULL; /* GetModuleHandleW returns a borrowed reference. */
#ifdef WaddleRuntimeHeapAudit
      audit_length = snprintf(audit_name, sizeof audit_name, "unloaded_%u", cycle);
      if (audit_length <= 0 || (size_t)audit_length >= sizeof audit_name ||
          audit_stage(audit_path, audit_name))
         goto cleanup;
#endif

      handles_balanced = wait_for_handle_baseline(baseline_handle_count,
                                                  &after_unload,
                                                  &handle_polls);
      tls_measured = count_free_tls_slots(&after_tls);
      fls_measured = count_free_fls_slots(&after_fls);
      if (!tls_measured || !fls_measured) {
         (void)fprintf(stderr,
                       "CYCLE_ERROR cycle=%u stage=tls_fls_measurement\n", cycle);
         goto cleanup;
      }

      if (cycle == 1) {
         heap_ok = capture_heap_snapshot(&heap_observed);
         heap_polls = 0;
      } else if (cycle == 2) {
         heap_ok = wait_for_heap_snapshot(NULL, &heap_observed, &heap_polls);
         if (heap_ok) {
            heap_reference = heap_observed;
            heap_reference_ready = TRUE;
         }
      } else if (heap_reference_ready) {
         heap_ok = wait_for_heap_snapshot(&heap_reference, &heap_observed,
                                          &heap_polls);
      }
      (void)printf("CYCLE cycle=%u status=%lu before_handles=%lu "
                   "after_handles=%lu handle_polls=%u handles_exact=%u "
                   "module_absent=%u lifetime_export_absent=%u "
                   "foreign_export_absent=%u free_tls=%u tls_exact=%u "
                   "free_fls=%u fls_exact=%u heap_busy_bytes=%llu "
                   "heap_busy_entries=%llu heap_polls=%u heap_ok=%u "
                   "heap_phase=%s heap_win32=%lu\n",
                   cycle, (unsigned long)lifetime_result,
                   (unsigned long)before_load, (unsigned long)after_unload,
                   handle_polls, handles_balanced ? 1u : 0u,
                   module_absent ? 1u : 0u,
                   lifetime_symbol_absent ? 1u : 0u,
                   foreign_symbol_absent ? 1u : 0u, after_tls,
                   after_tls == baseline_tls_slots ? 1u : 0u, after_fls,
                   after_fls == baseline_fls_slots ? 1u : 0u,
                   (unsigned long long)heap_observed.busy_bytes,
                   (unsigned long long)heap_observed.busy_entries, heap_polls,
                   heap_ok ? 1u : 0u,
                   cycle == 1 ? "warmup" : cycle == 2 ? "reference" : "exact",
                   (unsigned long)heap_observed.win32_error);
      if (cycle == 1 && heap_ok)
         (void)printf("FIRST_LOAD_HEAP_CONTROL before_bytes=%llu "
                      "before_entries=%llu after_bytes=%llu after_entries=%llu "
                      "delta_bytes_direction=%s delta_bytes_magnitude=%llu "
                      "delta_entries_direction=%s "
                      "delta_entries_magnitude=%llu diagnostic_only=1\n",
                      (unsigned long long)first_load_heap_baseline.busy_bytes,
                      (unsigned long long)first_load_heap_baseline.busy_entries,
                      (unsigned long long)heap_observed.busy_bytes,
                      (unsigned long long)heap_observed.busy_entries,
                      heap_observed.busy_bytes >=
                            first_load_heap_baseline.busy_bytes
                         ? "increase" : "decrease",
                      (unsigned long long)(heap_observed.busy_bytes >=
                                                   first_load_heap_baseline.busy_bytes
                                               ? heap_observed.busy_bytes -
                                                    first_load_heap_baseline.busy_bytes
                                               : first_load_heap_baseline.busy_bytes -
                                                    heap_observed.busy_bytes),
                      heap_observed.busy_entries >=
                            first_load_heap_baseline.busy_entries
                         ? "increase" : "decrease",
                      (unsigned long long)(heap_observed.busy_entries >=
                                                   first_load_heap_baseline.busy_entries
                                               ? heap_observed.busy_entries -
                                                    first_load_heap_baseline.busy_entries
                                               : first_load_heap_baseline.busy_entries -
                                                    heap_observed.busy_entries));

      if (!handles_balanced || !module_absent || !lifetime_symbol_absent ||
          !foreign_symbol_absent || after_tls != baseline_tls_slots ||
          after_fls != baseline_fls_slots || !heap_ok ||
          (cycle >= 3 && !heap_snapshots_equal(&heap_reference, &heap_observed))) {
         (void)fprintf(stderr, "CYCLE_FAIL cycle=%u\n", cycle);
         goto cleanup;
      }
      if (cycle >= 3)
         (void)printf("HEAP_EXACT cycle=%u reference_bytes=%llu "
                      "reference_entries=%llu\n",
                      cycle, (unsigned long long)heap_reference.busy_bytes,
                      (unsigned long long)heap_reference.busy_entries);
   }

   result_ok = TRUE;
   (void)printf("CYCLES_COMPLETED cycles=%u baseline_handles=%lu "
                "free_tls=%u free_fls=%u heap_reference_bytes=%llu "
                "heap_reference_entries=%llu cleanup_pending=1\n",
                (unsigned)WaddleRuntimeLifetimeCount,
                (unsigned long)baseline_handle_count, baseline_tls_slots,
                baseline_fls_slots,
                (unsigned long long)heap_reference.busy_bytes,
                (unsigned long long)heap_reference.busy_entries);

cleanup:
   if (foreign_release_event)
      (void)SetEvent(foreign_release_event);
   if (foreign_count != 0) {
      BOOL joined = join_and_close_threads(foreign_threads, foreign_count);
      foreign_count = 0;
      if (!joined) {
         (void)fprintf(stderr,
                       "CLEANUP_FATAL stage=foreign_thread_live\n");
         (void)fflush(stderr);
         fail_closed_process("WATCHDOG_FATAL stage=foreign_cleanup_timeout\n",
                             0xec);
      }
   }
   if (foreign_ready_event) {
      if (!CloseHandle(foreign_ready_event))
         result_ok = FALSE;
      foreign_ready_event = NULL;
   }
   if (foreign_release_event) {
      if (!CloseHandle(foreign_release_event))
         result_ok = FALSE;
      foreign_release_event = NULL;
   }
   lifetime_probe = NULL;
   foreign_probe = NULL;
   if (module) {
      module_context.path = NULL;
      module_context.module = module;
      module_context.error = ERROR_SUCCESS;
      module_context.succeeded = FALSE;
      if (!run_bounded_thread(free_module_main, &module_context,
                              "WATCHDOG_FATAL stage=cleanup_dll_unload_timeout\n",
                              0xed) || !module_context.succeeded) {
         (void)fprintf(stderr,
                       "CLEANUP_ERROR stage=module_unload win32=%lu\n",
                       (unsigned long)module_context.error);
         (void)fflush(stderr);
         fail_closed_process("WATCHDOG_FATAL stage=module_reference_retained\n",
                             0xee);
      } else {
         module = NULL;
      }
   }

   if (idle_release_event && !SetEvent(idle_release_event))
      result_ok = FALSE;
   if (idle_count != 0) {
      if (!join_and_close_threads(idle_threads, idle_count))
         fail_closed_process("WATCHDOG_FATAL stage=idle_thread_live\n", 0xef);
      idle_count = 0;
   }
   if (idle_ready_event) {
      if (!CloseHandle(idle_ready_event))
         result_ok = FALSE;
      idle_ready_event = NULL;
   }
   if (idle_release_event) {
      if (!CloseHandle(idle_release_event))
         result_ok = FALSE;
      idle_release_event = NULL;
   }
#ifdef WaddleRuntimeHeapAudit
   if (result_ok && audit_stage(audit_path, "threads_retired"))
      result_ok = FALSE;
#endif
   if (msvcrt_module && !FreeLibrary(msvcrt_module)) {
      (void)fprintf(stderr, "CLEANUP_ERROR stage=unpin_msvcrt win32=%lu\n",
                    (unsigned long)GetLastError());
      result_ok = FALSE;
   } else {
      msvcrt_module = NULL;
   }
   if (!result_ok)
      return 1;
   (void)printf("RUNTIME_LIFETIME_PROBE PASS cycles=%u baseline_handles=%lu "
                "free_tls=%u free_fls=%u heap_reference_bytes=%llu "
                "heap_reference_entries=%llu cleanup=complete\n",
                (unsigned)WaddleRuntimeLifetimeCount,
                (unsigned long)baseline_handle_count, baseline_tls_slots,
                baseline_fls_slots,
                (unsigned long long)heap_reference.busy_bytes,
                (unsigned long long)heap_reference.busy_entries);
   (void)fflush(stdout);
   return 0;
}
