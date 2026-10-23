/** @file native_csr_heap_test.c @brief Portable exact CSR heap helper controls.
 * The included production helper is exercised against bounded Win32 doubles.
 * These controls do not represent native Windows or acceptance evidence.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int audit_stage(const wchar_t *directory, const char *stage)
{
    (void)directory;
    (void)stage;
    return 0;
}

#include "native_csr_heap_windows.inc"

typedef PVOID (NTAPI *csr_allocate_capture_buffer_t)(ULONG, ULONG);
typedef VOID (NTAPI *csr_free_capture_buffer_t)(PVOID);

int main(void)
{
    if (setvbuf(stdout, NULL, _IONBF, 0)) return 9;
    native_csr_heap_snapshot_t snapshot;
    if (!native_csr_capture(&snapshot) || snapshot.busy_count != 0) return 10;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC allocate_export = ntdll ? GetProcAddress(ntdll, "CsrAllocateCaptureBuffer") : NULL;
    FARPROC free_export = ntdll ? GetProcAddress(ntdll, "CsrFreeCaptureBuffer") : NULL;
    csr_allocate_capture_buffer_t allocate_capture = NULL;
    csr_free_capture_buffer_t free_capture = NULL;
    _Static_assert(sizeof allocate_capture == sizeof allocate_export, "x64 function address ABI");
    memcpy(&allocate_capture, &allocate_export, sizeof allocate_capture);
    memcpy(&free_capture, &free_export, sizeof free_capture);
    if (!allocate_capture || !free_capture) return 11;
    PVOID captured = allocate_capture(0u, 12345u);
    if (!captured) return 12;
    int result = 0;
    if (!native_csr_capture(&snapshot) || snapshot.busy_count != 1 ||
        snapshot.rows[0].user != (uintptr_t)captured || snapshot.rows[0].bytes != 12380u ||
        !(snapshot.rows[0].flags & PROCESS_HEAP_ENTRY_BUSY)) result = 13;
    else if (!native_csr_emit("csr_live", &snapshot)) result = 14;
    free_capture(captured);
    captured = NULL;
    if (!result && (!native_csr_capture(&snapshot) || snapshot.busy_count != 0)) result = 15;
    if (!result && !native_csr_emit("csr_freed", &snapshot)) result = 16;
    if (!result && native_csr_stage(L"C:\\csr-control", "csr_control_end")) result = 17;
    if (captured) free_capture(captured);
    if (result) {
        fprintf(stderr, "Native CSR retained-owner calibration failed code=%d\n", result);
        return result;
    }
    puts("Native CSR retained-owner calibration PASS: exact CsrAllocateCaptureBuffer row retired");
    return 0;
}

#else
#include "win32_fake/native_csr_fake.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NativeCsrTestCheck(value) do { ++checks; if (!(value)) { \
    fprintf(stderr,"CSR helper check failed line=%d: %s\n",__LINE__,#value); exit(1); } } while (0)

static unsigned checks;
static DWORD last_error;
static int env_valid;
static char env_digest[65];
static int path_valid;
static UINT system_length_override;
static WCHAR system_path_override[MAX_PATH];
static int module_valid;
static int module_path_valid;
static DWORD module_path_result;
static uintptr_t module_base_override;
static DWORD heap_return_count;
static HANDLE heap_list[256];
static HANDLE selected_heap;
static int process_handle_valid;
static int read_fail;
static int read_short;
static int read_partial_success;
static int lock_fail;
static int unlock_fail;
static int walk_fail;
static DWORD walk_fail_index;
static PROCESS_HEAP_ENTRY walk_rows[4097];
static DWORD walk_row_count;
static DWORD walk_index;
static int cursor_error;
static unsigned lock_count;
static unsigned unlock_count;
static unsigned audit_count;
static int audit_result;
static int fake_printf_failure;
static int fake_puts_failure;
static int fake_fflush_failure;
static WCHAR current_module_path[MAX_PATH];
static const WCHAR SystemPath[] = L"C:\\Windows\\System32";
static HANDLE const CurrentHeap = (HANDLE)(uintptr_t)0x12345000u;
static HMODULE const CurrentModule = (HMODULE)(uintptr_t)0x70000000u;
static const WCHAR AuditPath[] = L"C:\\csr-audit";

static void reset_fake(void)
{
    last_error = ERROR_SUCCESS; env_valid = 1;
    memset(env_digest, 'a', 64); env_digest[64] = '\0';
    path_valid = module_valid = module_path_valid = 1;
    system_length_override = 0; memset(system_path_override, 0, sizeof system_path_override);
    module_path_result = 0; module_base_override = (uintptr_t)CurrentModule;
    heap_return_count = 1; memset(heap_list, 0, sizeof heap_list); heap_list[0] = CurrentHeap;
    selected_heap = CurrentHeap; process_handle_valid = 1;
    read_fail = read_short = read_partial_success = lock_fail = unlock_fail = walk_fail = 0;
    walk_fail_index = 0; memset(walk_rows, 0, sizeof walk_rows);
    walk_row_count = walk_index = 0; cursor_error = 0;
    lock_count = unlock_count = audit_count = 0; audit_result = 0;
    fake_printf_failure = fake_puts_failure = fake_fflush_failure = 0;
    wcscpy(current_module_path, L"C:\\Windows\\System32\\ntdll.dll");
}

DWORD GetEnvironmentVariableA(const char *name, char *buffer, DWORD capacity)
{
    if (strcmp(name, "WADDLE_CSR_HEAP_NTDLL_SHA256") || !env_valid) {
        last_error = ERROR_ENVVAR_NOT_FOUND; return 0;
    }
    if (capacity < sizeof env_digest) return (DWORD)strlen(env_digest);
    memcpy(buffer, env_digest, sizeof env_digest); return 64;
}
void SetLastError(DWORD error) { last_error = error; }
DWORD GetLastError(void) { return last_error; }
UINT GetSystemDirectoryW(WCHAR *buffer, UINT capacity)
{
    if (!path_valid) return 0;
    if (system_length_override) {
        if (capacity <= system_length_override) return system_length_override + 1u;
        memcpy(buffer, system_path_override, system_length_override * sizeof *buffer);
        buffer[system_length_override] = 0;
        return system_length_override;
    }
    const size_t length = wcslen(SystemPath);
    if (capacity <= length) return (UINT)(length + 1);
    wcscpy(buffer, SystemPath); return (UINT)length;
}
HMODULE GetModuleHandleW(const WCHAR *name)
{
    return module_valid && !wcscmp(name, L"ntdll.dll") ? (HMODULE)module_base_override : NULL;
}
DWORD GetModuleFileNameW(HMODULE module, WCHAR *buffer, DWORD capacity)
{
    if (module != (HMODULE)module_base_override || !module_path_valid) return 0;
    if (module_path_result) return module_path_result;
    const size_t length = wcslen(current_module_path);
    if (capacity <= length) return capacity;
    wcscpy(buffer, current_module_path); return (DWORD)length;
}
int _wcsicmp(const WCHAR *left, const WCHAR *right)
{
    while (*left && *right) {
        WCHAR a = *left, b = *right;
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a != b) return a < b ? -1 : 1;
        ++left; ++right;
    }
    return *left ? 1 : (*right ? -1 : 0);
}
DWORD GetProcessHeaps(DWORD capacity, HANDLE *heaps)
{
    const DWORD copied = heap_return_count < capacity ? heap_return_count : capacity;
    if (heaps) memcpy(heaps, heap_list, (size_t)copied * sizeof *heaps);
    return heap_return_count;
}
HANDLE GetCurrentProcess(void) { return process_handle_valid ? (HANDLE)(intptr_t)-1 : NULL; }
DWORD GetCurrentProcessId(void) { return 4455; }
int ReadProcessMemory(HANDLE process, const void *address, void *buffer, SIZE_T bytes, SIZE_T *read)
{
    if (read) *read = 0;
    if (read_fail || process != (HANDLE)(intptr_t)-1 || address != (const void *)(module_base_override + (uintptr_t)0x001d5e68u) ||
        bytes != sizeof selected_heap || !buffer) return 0;
    memcpy(buffer, &selected_heap, sizeof selected_heap);
    if (read) *read = (read_short || read_partial_success) ? bytes - 1u : bytes;
    return !read_short;
}
int HeapLock(HANDLE heap) { ++lock_count; return heap == CurrentHeap && !lock_fail; }
int HeapUnlock(HANDLE heap) { ++unlock_count; return heap == CurrentHeap && !unlock_fail; }
int HeapWalk(HANDLE heap, PROCESS_HEAP_ENTRY *entry)
{
    if (heap != CurrentHeap) { last_error = ERROR_ACCESS_DENIED; return 0; }
    if (walk_fail && walk_index == walk_fail_index) { last_error = ERROR_ACCESS_DENIED; return 0; }
    if (walk_index >= walk_row_count) { last_error = ERROR_NO_MORE_ITEMS; return 0; }
    if ((walk_index == 0 && entry->lpData) ||
        (walk_index > 0 && entry->lpData != walk_rows[walk_index-1u].lpData)) {
        cursor_error = 1; last_error = ERROR_ACCESS_DENIED; return 0;
    }
    *entry = walk_rows[walk_index++]; return 1;
}
int audit_stage(const WCHAR *directory, const char *stage)
{
    ++audit_count;
    NativeCsrTestCheck(!wcscmp(directory, AuditPath));
    NativeCsrTestCheck(stage && *stage);
    return audit_result;
}

static int native_csr_test_printf(const char *format, ...)
{
    if (fake_printf_failure == 1 && !strncmp(format,"CSR_HEAP_V1",11)) return -1;
    if (fake_printf_failure == 2 && !strcmp(format,"%04x")) return -1;
    if (fake_printf_failure == 3 && !strncmp(format,"CSR_HEAP_BUSY",13)) return -1;
    if (fake_printf_failure == 4 && !strncmp(format,"CSR_HEAP_API",12)) return -1;
    va_list arguments; va_start(arguments,format); int result=vprintf(format,arguments); va_end(arguments); return result;
}
static int native_csr_test_puts(const char *text)
{
    return fake_puts_failure ? EOF : puts(text);
}
static int native_csr_test_fflush(FILE *stream)
{
    return fake_fflush_failure ? EOF : fflush(stream);
}
#define printf native_csr_test_printf
#define puts native_csr_test_puts
#define fflush native_csr_test_fflush
#include "native_csr_heap_windows.inc"
#undef printf
#undef puts
#undef fflush

static void test_empty_success_and_stage(void)
{
    reset_fake(); native_csr_heap_snapshot_t snapshot;
    NativeCsrTestCheck(native_csr_capture(&snapshot));
    NativeCsrTestCheck(snapshot.module_base == (uintptr_t)CurrentModule && snapshot.heap == CurrentHeap);
    NativeCsrTestCheck(snapshot.process_heap_count == 1 && snapshot.walk_entries == 0 && snapshot.busy_count == 0);
    NativeCsrTestCheck(snapshot.process_heaps[0] == CurrentHeap);
    heap_return_count=3;
    heap_list[0]=(HANDLE)(uintptr_t)0x21000000u;
    heap_list[1]=CurrentHeap;
    heap_list[2]=(HANDLE)(uintptr_t)0x23000000u;
    NativeCsrTestCheck(native_csr_capture(&snapshot));
    NativeCsrTestCheck(snapshot.process_heap_count==3);
    NativeCsrTestCheck(!memcmp(snapshot.process_heaps,heap_list,3u*sizeof(HANDLE)));
    reset_fake();
    NativeCsrTestCheck(native_csr_capture(&snapshot));
    NativeCsrTestCheck(lock_count == 1 && unlock_count == 1);
    NativeCsrTestCheck(native_csr_stage(AuditPath, "baseline") == 0);
    NativeCsrTestCheck(audit_count == 1 && lock_count == 2 && unlock_count == 2);
}

static void test_live_row_is_rejected_after_capture_ack(void)
{
    reset_fake(); walk_row_count=1;
    walk_rows[0]=(PROCESS_HEAP_ENTRY){.lpData=(void *)(uintptr_t)0x12346000u,.cbData=12380,.wFlags=PROCESS_HEAP_ENTRY_BUSY};
    native_csr_heap_snapshot_t snapshot;
    NativeCsrTestCheck(native_csr_capture(&snapshot));
    NativeCsrTestCheck(snapshot.walk_entries==1 && snapshot.busy_count==1);
    NativeCsrTestCheck(snapshot.rows[0].user==(uintptr_t)0x12346000u && snapshot.rows[0].bytes==12380);
    walk_index=0;
    NativeCsrTestCheck(native_csr_stage(AuditPath,"unloaded_1") == 1);
    NativeCsrTestCheck(audit_count==1 && unlock_count==2);
}

static void test_environment_and_system_identity_fail_closed(void)
{
    native_csr_heap_snapshot_t snapshot;
    reset_fake();env_valid=0;NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(!lock_count);
    reset_fake();env_digest[0]='A';NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();env_digest[63]='g';NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();path_valid=0;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();module_valid=0;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();module_path_valid=0;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();module_path_result=MAX_PATH;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();wcscpy(current_module_path,L"C:\\Other\\ntdll.dll");NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();system_length_override=MAX_PATH;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();system_length_override=250;for(UINT i=0;i<250;i++)system_path_override[i]=L'x';NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();system_length_override=(UINT)wcslen(SystemPath);
    memcpy(system_path_override,SystemPath,system_length_override*sizeof(WCHAR));
    system_path_override[system_length_override++]=L'\\';
    NativeCsrTestCheck(native_csr_capture(&snapshot));
    reset_fake();env_digest[0]='b';NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();module_base_override=UINTPTR_MAX-NativeCsrHeapRva+1u;NativeCsrTestCheck(!native_csr_capture(&snapshot));
}

static void test_heap_membership_and_read_failures(void)
{
    native_csr_heap_snapshot_t snapshot;
    reset_fake();heap_return_count=0;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();heap_return_count=NativeCsrMaxHeaps+1u;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();heap_return_count=2;heap_list[1]=CurrentHeap;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();selected_heap=(HANDLE)(uintptr_t)0x98765000u;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();read_fail=1;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();read_short=1;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();read_partial_success=1;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();selected_heap=NULL;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();process_handle_valid=0;NativeCsrTestCheck(!native_csr_capture(&snapshot));
    reset_fake();heap_list[0]=NULL;NativeCsrTestCheck(!native_csr_capture(&snapshot));
}

static void test_walk_errors_bounds_and_unlock(void)
{
    native_csr_heap_snapshot_t snapshot;
    reset_fake();lock_fail=1;NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(lock_count==1 && !unlock_count);
    reset_fake();walk_row_count=1;walk_fail=1;walk_fail_index=0;NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(lock_count==1 && unlock_count==1);
    reset_fake();unlock_fail=1;NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(unlock_count==1);
    reset_fake();walk_row_count=NativeCsrMaxWalkEntries+1u;
    for(DWORD i=0;i<walk_row_count;i++)walk_rows[i]=(PROCESS_HEAP_ENTRY){.lpData=(void *)(uintptr_t)(0x20000000u+i*16u),.cbData=16,.wFlags=0};
    NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(snapshot.walk_entries==NativeCsrMaxWalkEntries && unlock_count==1 && !cursor_error);
    reset_fake();walk_row_count=NativeCsrMaxBusyRows+1u;
    for(DWORD i=0;i<walk_row_count;i++)walk_rows[i]=(PROCESS_HEAP_ENTRY){.lpData=(void *)(uintptr_t)(0x30000000u+i*16u),.cbData=16,.wFlags=PROCESS_HEAP_ENTRY_BUSY};
    NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(snapshot.busy_count==NativeCsrMaxBusyRows && unlock_count==1);
    reset_fake();walk_row_count=1;walk_rows[0]=(PROCESS_HEAP_ENTRY){.wFlags=PROCESS_HEAP_ENTRY_BUSY};
    NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(unlock_count==1);
    reset_fake();walk_row_count=2;
    walk_rows[0]=(PROCESS_HEAP_ENTRY){.lpData=(void *)(uintptr_t)0x12346000u,.cbData=8,.wFlags=PROCESS_HEAP_ENTRY_BUSY};
    walk_rows[1]=walk_rows[0];NativeCsrTestCheck(!native_csr_capture(&snapshot));NativeCsrTestCheck(unlock_count==1);
    reset_fake();native_csr_heap_snapshot_t empty={0};empty.module_path[0]=L'C';
    NativeCsrTestCheck(!native_csr_emit(NULL,&empty));NativeCsrTestCheck(!native_csr_emit("baseline",NULL));
    NativeCsrTestCheck(!native_csr_emit("",&empty));NativeCsrTestCheck(!native_csr_emit("bad/stage",&empty));
    NativeCsrTestCheck(!native_csr_emit("Bad",&empty));NativeCsrTestCheck(!native_csr_emit("bad-",&empty));
    fake_printf_failure=1;NativeCsrTestCheck(!native_csr_emit("baseline",&empty));fake_printf_failure=0;
    fake_printf_failure=2;NativeCsrTestCheck(!native_csr_emit("baseline",&empty));fake_printf_failure=0;
    fake_puts_failure=1;NativeCsrTestCheck(!native_csr_emit("baseline",&empty));fake_puts_failure=0;
    empty.busy_count=1;empty.rows[0]=(native_csr_heap_row_t){.user=1,.bytes=2,.flags=PROCESS_HEAP_ENTRY_BUSY};
    fake_printf_failure=3;NativeCsrTestCheck(!native_csr_emit("baseline",&empty));fake_printf_failure=0;
    empty.process_heap_count=1;empty.process_heaps[0]=CurrentHeap;
    fake_printf_failure=4;NativeCsrTestCheck(!native_csr_emit("baseline",&empty));fake_printf_failure=0;
    empty.busy_count=0;fake_fflush_failure=1;NativeCsrTestCheck(!native_csr_emit("baseline",&empty));fake_fflush_failure=0;
    reset_fake();audit_result=1;NativeCsrTestCheck(native_csr_stage(AuditPath,"baseline") == 1);NativeCsrTestCheck(unlock_count==1 && audit_count==1);
}

int main(void)
{
    test_empty_success_and_stage();
    test_live_row_is_rejected_after_capture_ack();
    test_environment_and_system_identity_fail_closed();
    test_heap_membership_and_read_failures();
    test_walk_errors_bounds_and_unlock();
    printf("CSR heap helper 5 groups %u checks PASS (Win32 API doubles only)\n",checks);
    return 0;
}
#endif
