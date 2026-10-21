/** @file native_csr_heap_windows.inc
 * @brief Bounded read-only inventory of the target's exact CSR port heap.
 *
 * This qualification-only helper is included after dxvk_heap_audit_windows.inc.
 * It borrows (never creates or destroys) the loaded System32 ntdll module and
 * CsrPortHeap handle. The trusted controller supplies the SHA-256 for that
 * exact ntdll image in WADDLE_CSR_HEAP_NTDLL_SHA256 and verifies the image
 * before and after the target run. No module is loaded, memory is allocated,
 * or ownership transition performed here.
 */
#include <stdint.h>
#include <string.h>

#define NativeCsrMaxHeaps 256u
#define NativeCsrMaxWalkEntries 4096u
#define NativeCsrMaxBusyRows 1024u
#define NativeCsrHeapRva ((uintptr_t)0x001d5e68u)
#define NativeCsrShaBytes 64u

#ifdef _WIN32
_Static_assert(sizeof(WCHAR) == 2u, "Windows path record requires UTF-16 code units");
#endif

/** @brief Baseline-pinned runner digest; the sole lifecycle thread owns it. */
static char native_csr_initial_sha256[NativeCsrShaBytes + 1u];
static int native_csr_digest_initialized;

/** @brief One copied busy row from HeapWalk, never a borrowed entry pointer. */
typedef struct native_csr_heap_row_t {
    uintptr_t user;
    DWORD bytes;
    WORD flags;
} native_csr_heap_row_t;

/** @brief Fixed-size snapshot captured while the exact heap is locked. */
typedef struct native_csr_heap_snapshot_t {
    HMODULE module;
    uintptr_t module_base;
    HANDLE heap;
    DWORD process_heap_count;
    DWORD walk_entries;
    DWORD busy_count;
    WCHAR module_path[MAX_PATH];
    char trusted_sha256[NativeCsrShaBytes + 1u];
    native_csr_heap_row_t rows[NativeCsrMaxBusyRows];
} native_csr_heap_snapshot_t;

/** @brief Accept only lowercase SHA-256 text supplied by the trusted runner. */
static int native_csr_digest(char *output)
{
    char input[NativeCsrShaBytes + 1u] = {0};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA("WADDLE_CSR_HEAP_NTDLL_SHA256",
        input, (DWORD)sizeof input);
    if (length != NativeCsrShaBytes) return 0;
    for (DWORD index = 0; index < NativeCsrShaBytes; ++index) {
        const char value = input[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return 0;
    }
    if (native_csr_digest_initialized) {
        if (memcmp(input, native_csr_initial_sha256, sizeof input)) return 0;
    } else {
        memcpy(native_csr_initial_sha256, input, sizeof input);
        native_csr_digest_initialized = 1;
    }
    memcpy(output, input, sizeof input);
    return 1;
}

/** @brief Resolve the already-loaded exact 64-bit System32 ntdll without refcount. */
static int native_csr_module(native_csr_heap_snapshot_t *snapshot)
{
    WCHAR expected[MAX_PATH] = {0};
    UINT system_length = GetSystemDirectoryW(expected, MAX_PATH);
    if (!system_length || system_length >= MAX_PATH) return 0;
    const UINT suffix_space = expected[system_length - 1u] == L'\\' ? 10u : 11u;
    if (system_length + suffix_space > MAX_PATH) return 0;
    if (expected[system_length - 1u] != L'\\') expected[system_length++] = L'\\';
    static const WCHAR suffix[] = L"ntdll.dll";
    memcpy(expected + system_length, suffix, sizeof suffix);

    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (!module) return 0;
    WCHAR actual[MAX_PATH] = {0};
    const DWORD copied = GetModuleFileNameW(module, actual, MAX_PATH);
    if (!copied || copied >= MAX_PATH || _wcsicmp(actual, expected)) return 0;
    const uintptr_t base = (uintptr_t)module;
    if (sizeof(uintptr_t) != 8u || base > UINTPTR_MAX - NativeCsrHeapRva) return 0;
    snapshot->module = module;
    snapshot->module_base = base;
    memcpy(snapshot->module_path, actual, (size_t)(copied + 1u) * sizeof(WCHAR));
    return native_csr_digest(snapshot->trusted_sha256);
}

/** @brief Capture the exact CsrPortHeap busy table using bounded HeapWalk. */
static int native_csr_capture(native_csr_heap_snapshot_t *snapshot)
{
    if (!snapshot) return 0;
    memset(snapshot, 0, sizeof *snapshot);
    if (!native_csr_module(snapshot)) return 0;

    HANDLE heaps[NativeCsrMaxHeaps] = {0};
    const DWORD heap_count = GetProcessHeaps(NativeCsrMaxHeaps, heaps);
    if (!heap_count || heap_count > NativeCsrMaxHeaps) return 0;
    for (DWORD index = 0; index < heap_count; ++index) {
        if (!heaps[index]) return 0;
        for (DWORD prior = 0; prior < index; ++prior) if (heaps[index] == heaps[prior]) return 0;
    }
    snapshot->process_heap_count = heap_count;

    HANDLE heap = NULL;
    SIZE_T bytes_read = 0;
    const uintptr_t address = snapshot->module_base + NativeCsrHeapRva;
    if (!ReadProcessMemory(GetCurrentProcess(), (const void *)address, &heap,
            sizeof heap, &bytes_read) || bytes_read != sizeof heap || !heap) return 0;
    unsigned memberships = 0;
    for (DWORD index = 0; index < heap_count; ++index) memberships += heaps[index] == heap;
    if (memberships != 1u) return 0;
    snapshot->heap = heap;

    if (!HeapLock(heap)) return 0;
    int valid = 1;
    PROCESS_HEAP_ENTRY entry;
    memset(&entry, 0, sizeof entry);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        if (!HeapWalk(heap, &entry)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) valid = 0;
            break;
        }
        if (snapshot->walk_entries == NativeCsrMaxWalkEntries) {
            valid = 0;
            break;
        }
        ++snapshot->walk_entries;
        if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
            if (!entry.lpData || snapshot->busy_count == NativeCsrMaxBusyRows) {
                valid = 0;
                break;
            }
            const uintptr_t user = (uintptr_t)entry.lpData;
            for (DWORD prior = 0; prior < snapshot->busy_count; ++prior) {
                if (snapshot->rows[prior].user == user) valid = 0;
            }
            if (!valid) break;
            snapshot->rows[snapshot->busy_count++] = (native_csr_heap_row_t){
                .user = user, .bytes = entry.cbData, .flags = entry.wFlags};
        }
    }
    if (!HeapUnlock(heap)) valid = 0;
    return valid;
}

/** @brief Emit the copied snapshot after HeapUnlock; no formatting under lock. */
static int native_csr_emit(const char *stage, const native_csr_heap_snapshot_t *snapshot)
{
    if (!stage || !snapshot) return 0;
    const size_t stage_length = strlen(stage);
    if (!stage_length || stage_length >= 32u) return 0;
    for (size_t index = 0; index < stage_length; ++index) {
        const char value = stage[index];
        if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_')) return 0;
    }
    if (printf("CSR_HEAP_V1 stage=%s pid=%lu ntdll_base=%016llx csr_heap=%016llx process_heaps=%lu walk_entries=%lu busy=%lu ntdll_sha256=%s ntdll_path_utf16le=",
            stage, (unsigned long)GetCurrentProcessId(), (unsigned long long)snapshot->module_base,
            (unsigned long long)(uintptr_t)snapshot->heap, (unsigned long)snapshot->process_heap_count,
            (unsigned long)snapshot->walk_entries, (unsigned long)snapshot->busy_count,
            snapshot->trusted_sha256) < 0) return 0;
    for (size_t index = 0; snapshot->module_path[index]; ++index) {
        if (printf("%04x", (unsigned)snapshot->module_path[index]) < 0) return 0;
    }
    if (puts("") == EOF) return 0;
    for (DWORD index = 0; index < snapshot->busy_count; ++index) {
        const native_csr_heap_row_t *row = &snapshot->rows[index];
        if (printf("CSR_HEAP_BUSY stage=%s index=%lu user=%016llx bytes=%lu flags=%04x\n",
                stage, (unsigned long)index, (unsigned long long)row->user,
                (unsigned long)row->bytes, (unsigned)row->flags) < 0) return 0;
    }
    return fflush(stdout) == 0;
}

/** @brief Capture CSR ownership, then use the existing bounded stage handshake.
 * @param[in] directory Existing supervisor-owned absolute audit directory.
 * @param[in] stage Trusted lowercase stage literal. @return Zero only if the
 * heap inventory is complete, no CSR user blocks remain, and audit_stage passes.
 * @note Sole fixture thread after candidate workers have retired. Snapshot rows
 * are copied under HeapLock and all output/handshake occurs after HeapUnlock.
 */
static int native_csr_stage(const wchar_t *directory, const char *stage)
{
    native_csr_heap_snapshot_t snapshot;
    if (!native_csr_capture(&snapshot)) return 1;
    if (!native_csr_emit(stage, &snapshot)) return 1;
    const int stage_result = audit_stage(directory, stage);
    return stage_result || snapshot.busy_count ? 1 : 0;
}
