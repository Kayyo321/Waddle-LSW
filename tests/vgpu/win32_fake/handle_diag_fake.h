/** @file handle_diag_fake.h @brief Test-only Win32/NT subset, capture streams and allocation
 * hooks for the Linux host test of tcp_gpu_handle_diag_windows.inc; not Windows SDK code.
 * @note Win32 identifiers intentionally match the external ABI the .inc consumes. Include
 * this file after every system header the test needs: it defines the function-like macros
 * malloc, realloc, free and printf and redirects stdout and stderr to in-memory capture
 * streams, so a system header included later would be corrupted. Code that must reach the
 * real allocator or real streams uses (malloc)(n), (free)(p) and the real_* accessors.
 * Single-threaded; all state is static to the including translation unit.
 */
#ifndef WaddleTestHandleDiagFakeH
/** @brief Test include guard; no state. */
#define WaddleTestHandleDiagFakeH
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Calling convention marker of NT exports; empty on the Linux host. */
#define NTAPI
/** @brief Win32 boolean result. */
typedef int BOOL;
/** @brief Win32 32-bit unsigned scalar; the Windows LLP64 unsigned long. */
typedef uint32_t DWORD;
/** @brief NT 32-bit unsigned scalar; the Windows LLP64 unsigned long. */
typedef uint32_t ULONG;
/** @brief Borrowed pointer to one ULONG. */
typedef ULONG *PULONG;
/** @brief Win32 32-bit signed scalar, also the NTSTATUS representation. */
typedef int32_t LONG;
/** @brief Win32 16-bit unsigned scalar. */
typedef uint16_t USHORT;
/** @brief Win32 UTF-16 code unit. */
typedef uint16_t WCHAR;
/** @brief Borrowed pointer to UTF-16 code units. */
typedef WCHAR *PWSTR;
/** @brief Pointer-sized unsigned scalar. */
typedef uintptr_t ULONG_PTR;
/** @brief Untyped borrowed pointer. */
typedef void *PVOID;
/** @brief Opaque handle token; the process pseudo-handle is (HANDLE)-1. */
typedef void *HANDLE;
/** @brief Opaque module token returned by GetModuleHandleA. */
typedef void *HMODULE;
/** @brief Borrowed NUL-terminated narrow string. */
typedef const char *LPCSTR;
/** @brief Generic function pointer returned by GetProcAddress. */
typedef void (*FARPROC)(void);
/** @brief Signed 64-bit counter; QuadPart is the only member the .inc reads. */
typedef union { int64_t QuadPart; struct { DWORD LowPart; LONG HighPart; }; } LARGE_INTEGER;

/** @brief Fixed capacities of the fake call log and the recorded allocation sizes. */
enum { HandleDiagFakeLogBytes = 1024, HandleDiagFakeSizeSlots = 16 };

/** @brief All fake state: scripted Win32 results, call log, allocator hooks and capture.
 * @details Reset with memset before each test case while the capture streams are closed.
 */
typedef struct handle_diag_fake_t {
    int frequency_fails;          /**< Nonzero: QueryPerformanceFrequency returns FALSE. */
    int counter_fails;            /**< Nonzero: QueryPerformanceCounter returns FALSE. */
    int64_t frequency;            /**< Value stored by QueryPerformanceFrequency. */
    int64_t counter;              /**< Value stored by QueryPerformanceCounter. */
    int ntdll_present;            /**< Nonzero: GetModuleHandleA("ntdll.dll") succeeds. */
    FARPROC query_process_export; /**< Result for "NtQueryInformationProcess", NULL when absent. */
    FARPROC query_object_export;  /**< Result for "NtQueryObject", NULL when absent. */
    char calls[HandleDiagFakeLogBytes]; /**< Space-separated ordered log of fake calls. */
    unsigned malloc_calls;        /**< malloc calls made by the code under test. */
    unsigned realloc_calls;       /**< realloc calls made by the code under test. */
    unsigned free_calls;          /**< free calls made by the code under test. */
    unsigned fail_malloc_at;      /**< One-based malloc call that returns NULL; zero never fails. */
    unsigned fail_realloc_at;     /**< One-based realloc call that returns NULL; zero never fails. */
    long outstanding;             /**< Blocks allocated by the code under test and not yet freed. */
    long lowest_outstanding;      /**< Lowest outstanding value seen; negative means a foreign free. */
    unsigned zero_size_reallocs;  /**< realloc calls with size zero; the code under test makes none. */
    size_t malloc_sizes[HandleDiagFakeSizeSlots];   /**< Sizes of the first malloc calls. */
    size_t realloc_sizes[HandleDiagFakeSizeSlots];  /**< Sizes of the first realloc calls. */
    FILE *out;                    /**< Memory stream that stands in for stdout. */
    char *out_text;               /**< Buffer behind out, valid after fflush or fclose. */
    size_t out_size;              /**< Length behind out, valid after fflush or fclose. */
    FILE *err;                    /**< Memory stream that stands in for stderr. */
    char *err_text;               /**< Buffer behind err, valid after fflush or fclose. */
    size_t err_size;              /**< Length behind err, valid after fflush or fclose. */
} handle_diag_fake_t;

/** @brief The one fake state instance of this translation unit. */
static handle_diag_fake_t handle_diag_fake;

/** @brief The process stdout, reachable after the stdout macro below redirects the name.
 * @return Borrowed real stream; never owned by the caller.
 */
static inline FILE *handle_diag_fake_real_stdout(void) { return stdout; }
/** @brief The process stderr, reachable after the stderr macro below redirects the name.
 * @return Borrowed real stream; never owned by the caller.
 */
static inline FILE *handle_diag_fake_real_stderr(void) { return stderr; }

/** @brief Append one entry to the ordered fake call log, dropping text that does not fit.
 * @param[in] entry Nonnull static label. @param[in] detail Optional suffix, NULL for none.
 */
static inline void handle_diag_fake_log(const char *entry, const char *detail)
{
    const size_t used = strlen(handle_diag_fake.calls);
    snprintf(handle_diag_fake.calls + used, sizeof handle_diag_fake.calls - used, "%s%s%s",
        used ? " " : "", entry, detail ? detail : "");
}

/** @brief Counting malloc hook; the real allocator runs unless a failure is scripted.
 * @param[in] bytes Requested size. @return New block owned by the caller, or NULL on a scripted failure.
 */
static inline void *handle_diag_fake_malloc(size_t bytes)
{
    handle_diag_fake.malloc_calls++;
    if (handle_diag_fake.malloc_calls <= HandleDiagFakeSizeSlots)
        handle_diag_fake.malloc_sizes[handle_diag_fake.malloc_calls - 1] = bytes;
    if (handle_diag_fake.fail_malloc_at == handle_diag_fake.malloc_calls) return NULL;
    void *block = malloc(bytes);
    if (block) handle_diag_fake.outstanding++;
    return block;
}

/** @brief Counting realloc hook; a scripted failure leaves the original block untouched.
 * @param[in] block Owned block or NULL. @param[in] bytes New size.
 * @return Resized or new block, or NULL on a scripted or real failure.
 */
static inline void *handle_diag_fake_realloc(void *block, size_t bytes)
{
    handle_diag_fake.realloc_calls++;
    if (handle_diag_fake.realloc_calls <= HandleDiagFakeSizeSlots)
        handle_diag_fake.realloc_sizes[handle_diag_fake.realloc_calls - 1] = bytes;
    if (!bytes) handle_diag_fake.zero_size_reallocs++;
    if (handle_diag_fake.fail_realloc_at == handle_diag_fake.realloc_calls) return NULL;
    void *grown = realloc(block, bytes);
    if (grown && !block) handle_diag_fake.outstanding++;
    return grown;
}

/** @brief Counting free hook.
 * @param[in] block Owned block or NULL; freed with the real allocator.
 */
static inline void handle_diag_fake_free(void *block)
{
    handle_diag_fake.free_calls++;
    if (block) {
        handle_diag_fake.outstanding--;
        if (handle_diag_fake.outstanding < handle_diag_fake.lowest_outstanding)
            handle_diag_fake.lowest_outstanding = handle_diag_fake.outstanding;
    }
    free(block);
}

/** @brief Open both capture streams; aborts if the C library cannot provide them. */
static inline void handle_diag_fake_capture_open(void)
{
    handle_diag_fake.out = open_memstream(&handle_diag_fake.out_text, &handle_diag_fake.out_size);
    handle_diag_fake.err = open_memstream(&handle_diag_fake.err_text, &handle_diag_fake.err_size);
    if (!handle_diag_fake.out || !handle_diag_fake.err) abort();
}

/** @brief Close both capture streams and release their buffers with the real allocator. */
static inline void handle_diag_fake_capture_close(void)
{
    if (handle_diag_fake.out) fclose(handle_diag_fake.out);
    if (handle_diag_fake.err) fclose(handle_diag_fake.err);
    (free)(handle_diag_fake.out_text);
    (free)(handle_diag_fake.err_text);
    handle_diag_fake.out = handle_diag_fake.err = NULL;
    handle_diag_fake.out_text = handle_diag_fake.err_text = NULL;
    handle_diag_fake.out_size = handle_diag_fake.err_size = 0;
}

/** @brief Discard everything captured so far. */
static inline void handle_diag_fake_capture_reset(void)
{
    handle_diag_fake_capture_close();
    handle_diag_fake_capture_open();
}

/** @brief Flush and return everything the code under test wrote to stdout.
 * @return Borrowed NUL-terminated text, valid until the next write or reset.
 */
static inline const char *handle_diag_fake_out_text(void)
{
    fflush(handle_diag_fake.out);
    return handle_diag_fake.out_text ? handle_diag_fake.out_text : "";
}

/** @brief Flush and return everything the code under test wrote to stderr.
 * @return Borrowed NUL-terminated text, valid until the next write or reset.
 */
static inline const char *handle_diag_fake_err_text(void)
{
    fflush(handle_diag_fake.err);
    return handle_diag_fake.err_text ? handle_diag_fake.err_text : "";
}

/** @brief Bytes of stdout output that reached the stream buffer without any flush by this test.
 * @return Length visible without calling fflush; equals the full output only if the code under
 * test flushed stdout itself.
 */
static inline size_t handle_diag_fake_out_visible_size(void) { return handle_diag_fake.out_size; }
/** @brief Bytes of stderr output that reached the stream buffer without any flush by this test.
 * @return Length visible without calling fflush; equals the full output only if the code under
 * test flushed stderr itself.
 */
static inline size_t handle_diag_fake_err_visible_size(void) { return handle_diag_fake.err_size; }

/** @brief Fake process pseudo-handle.
 * @return (HANDLE)-1, the value the real GetCurrentProcess returns.
 */
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }

/** @brief Scripted performance-counter frequency.
 * @param[out] value Nonnull; receives the scripted frequency on success.
 * @return Nonzero on success, zero when scripted to fail.
 */
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *value)
{
    handle_diag_fake_log("qpf", NULL);
    if (handle_diag_fake.frequency_fails) return 0;
    value->QuadPart = handle_diag_fake.frequency;
    return 1;
}

/** @brief Scripted performance-counter value.
 * @param[out] value Nonnull; receives the scripted counter on success.
 * @return Nonzero on success, zero when scripted to fail.
 */
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *value)
{
    handle_diag_fake_log("qpc", NULL);
    if (handle_diag_fake.counter_fails) return 0;
    value->QuadPart = handle_diag_fake.counter;
    return 1;
}

/** @brief Scripted module lookup; only "ntdll.dll" can succeed.
 * @param[in] name Nonnull module name, borrowed.
 * @return Opaque nonnull module token, or NULL when absent.
 */
static inline HMODULE GetModuleHandleA(LPCSTR name)
{
    handle_diag_fake_log("gmh:", name);
    if (strcmp(name, "ntdll.dll") != 0 || !handle_diag_fake.ntdll_present) return NULL;
    return (HMODULE)&handle_diag_fake;
}

/** @brief Scripted export lookup for the two ntdll exports the .inc resolves.
 * @param[in] module Token returned by GetModuleHandleA. @param[in] name Nonnull export name.
 * @return Scripted function pointer, or NULL for a foreign module or an unknown or absent export.
 */
static inline FARPROC GetProcAddress(HMODULE module, LPCSTR name)
{
    handle_diag_fake_log("gpa:", name);
    if (module != (HMODULE)&handle_diag_fake) return NULL;
    if (strcmp(name, "NtQueryInformationProcess") == 0) return handle_diag_fake.query_process_export;
    if (strcmp(name, "NtQueryObject") == 0) return handle_diag_fake.query_object_export;
    return NULL;
}

#undef stdout
#undef stderr
/** @brief The code under test writes stdout into the capture stream. */
#define stdout (handle_diag_fake.out)
/** @brief The code under test writes stderr into the capture stream. */
#define stderr (handle_diag_fake.err)
/** @brief printf is routed to the stdout capture stream. */
#define printf(...) fprintf((handle_diag_fake.out), __VA_ARGS__)
/** @brief malloc is routed to the counting hook; (malloc)(n) reaches the real allocator. */
#define malloc(bytes) handle_diag_fake_malloc(bytes)
/** @brief realloc is routed to the counting hook. */
#define realloc(block, bytes) handle_diag_fake_realloc((block), (bytes))
/** @brief free is routed to the counting hook; (free)(p) reaches the real allocator. */
#define free(block) handle_diag_fake_free(block)
#endif
