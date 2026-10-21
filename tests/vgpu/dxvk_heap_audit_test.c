#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
    DWORD dwFileAttributes;
    DWORD nNumberOfLinks;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} BY_HANDLE_FILE_INFORMATION;
typedef struct fake_handle_t *HANDLE;

#define INVALID_FILE_ATTRIBUTES ((DWORD)0xffffffffu)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_SUCCESS 0u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_ENVVAR_NOT_FOUND 203u
#define ERROR_SHARING_VIOLATION 32u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define CREATE_NEW 1u
#define OPEN_EXISTING 3u
#define FILE_TYPE_DISK 1u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 8u

struct fake_handle_t {
    int kind;
    int open;
};

static struct fake_handle_t ready_handle;
static struct fake_handle_t ack_handle;
static DWORD last_error;
static ULONGLONG fake_tick;
static int env_present;
static int env_error;
static wchar_t env_path[1100];
static int directory_valid;
static DWORD directory_attributes;
static int ack_already_exists;
static int poll_calls;
static int transient_not_found;
static int transient_sharing;
static DWORD open_error;
static int create_ready_error;
static int fail_write;
static int short_write;
static int fail_close_ready;
static int fail_close_ack;
static int fail_process_times;
static int fail_handle_count;
static int fail_multibyte;
static int fake_file_type;
static int fail_file_info;
static int fail_read;
static DWORD ack_attributes;
static DWORD ack_links;
static DWORD ack_size_high;
static DWORD ack_size_low;
static int ack_bad_bytes;
static int ack_short_read;
static int fail_record_format;
static int oversized_record_format;
static int fail_expected_format;
static int oversized_expected_format;
static int ack_read_calls;
static int close_ready_calls;
static int close_ack_calls;
static char ready_record[256];

static void reset_fake(void)
{
    ready_handle = (struct fake_handle_t){0, 0};
    ack_handle = (struct fake_handle_t){1, 0};
    last_error = ERROR_SUCCESS;
    fake_tick = 0;
    env_present = 1;
    env_error = ERROR_ENVVAR_NOT_FOUND;
    wcscpy(env_path, L"C:\\audit");
    directory_valid = 1;
    directory_attributes = FILE_ATTRIBUTE_DIRECTORY;
    ack_already_exists = 0;
    poll_calls = 0;
    transient_not_found = 0;
    transient_sharing = 0;
    open_error = ERROR_SUCCESS;
    create_ready_error = 0;
    fail_write = 0;
    short_write = 0;
    fail_close_ready = 0;
    fail_close_ack = 0;
    fail_process_times = 0;
    fail_handle_count = 0;
    fail_multibyte = 0;
    fake_file_type = FILE_TYPE_DISK;
    fail_file_info = 0;
    fail_read = 0;
    ack_attributes = FILE_ATTRIBUTE_NORMAL;
    ack_links = 1;
    ack_size_high = 0;
    ack_size_low = (DWORD)strlen("continue=K0\n");
    ack_bad_bytes = 0;
    ack_short_read = 0;
    fail_record_format = 0;
    oversized_record_format = 0;
    fail_expected_format = 0;
    oversized_expected_format = 0;
    ack_read_calls = 0;
    close_ready_calls = 0;
    close_ack_calls = 0;
    memset(ready_record, 0, sizeof ready_record);
}

static DWORD GetLastError(void) { return last_error; }
static void SetLastError(DWORD value) { last_error = value; }

static DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *buffer, DWORD capacity)
{
    (void)name;
    if (!env_present) { last_error = (DWORD)env_error; return 0; }
    size_t length = wcslen(env_path);
    if (length + 1 > capacity) return (DWORD)(length + 1);
    wmemcpy(buffer, env_path, length + 1);
    return (DWORD)length;
}

static DWORD GetFileAttributesW(const wchar_t *path)
{
    size_t length = wcslen(path);
    if (length >= 9 && wcscmp(path + length - 9, L".continue") == 0) {
        if (ack_already_exists) return FILE_ATTRIBUTE_NORMAL;
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_FILE_ATTRIBUTES;
    }
    if (directory_valid) return directory_attributes;
    last_error = ERROR_ACCESS_DENIED;
    return INVALID_FILE_ATTRIBUTES;
}

static int MultiByteToWideChar(unsigned int code_page, DWORD flags, const char *input,
                               int input_count, wchar_t *output, int output_count)
{
    if (fail_multibyte || code_page != CP_UTF8 || flags != MB_ERR_INVALID_CHARS || input_count != -1)
        return 0;
    size_t length = strlen(input);
    if (length + 1 > (size_t)output_count) return 0;
    for (size_t index = 0; index <= length; index++) output[index] = (unsigned char)input[index];
    return (int)length + 1;
}

static int GetProcessTimes(void *process, FILETIME *created, FILETIME *exited,
                           FILETIME *kernel, FILETIME *user)
{
    (void)process; (void)exited; (void)kernel; (void)user;
    if (fail_process_times) return 0;
    created->dwLowDateTime = 0x12345678u;
    created->dwHighDateTime = 0x87654321u;
    return 1;
}

static int GetProcessHandleCount(void *process, DWORD *handles)
{
    (void)process;
    if (fail_handle_count) return 0;
    *handles = 7;
    return 1;
}

static int fake_snprintf(char *buffer, size_t capacity, const char *format, ...)
{
    int *forced_error = strstr(format, "pid=") ? &fail_record_format : &fail_expected_format;
    int *forced_large = strstr(format, "pid=") ? &oversized_record_format : &oversized_expected_format;
    if (*forced_error) { *forced_error = 0; return -1; }
    if (*forced_large) { *forced_large = 0; return (int)capacity; }
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    return result;
}

static void *GetCurrentProcess(void) { return (void *)(intptr_t)1; }
static DWORD GetCurrentProcessId(void) { return 42; }
static ULONGLONG GetTickCount64(void) { return fake_tick; }
static void Sleep(DWORD milliseconds) { fake_tick += milliseconds; }

static HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD sharing, void *security,
                          DWORD creation, DWORD attributes, HANDLE template_file)
{
    (void)access; (void)sharing; (void)security; (void)attributes; (void)template_file;
    size_t length = wcslen(path);
    if (creation == CREATE_NEW) {
        if (create_ready_error || ready_handle.open) {
            last_error = create_ready_error ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
            return INVALID_HANDLE_VALUE;
        }
        ready_handle.open = 1;
        return &ready_handle;
    }
    if (length >= 9 && wcscmp(path + length - 9, L".continue") == 0) {
        int poll = poll_calls++;
        if (poll < transient_not_found) { last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
        if (poll < transient_not_found + transient_sharing) {
            last_error = ERROR_SHARING_VIOLATION; return INVALID_HANDLE_VALUE;
        }
        if (poll < transient_not_found + transient_sharing + 1 && open_error != ERROR_SUCCESS) {
            last_error = open_error; return INVALID_HANDLE_VALUE;
        }
        ack_handle.open = 1;
        return &ack_handle;
    }
    last_error = ERROR_ACCESS_DENIED;
    return INVALID_HANDLE_VALUE;
}

static int WriteFile(HANDLE file, const void *data, DWORD size, DWORD *written, void *overlapped)
{
    (void)overlapped;
    if (file != &ready_handle || fail_write || size >= sizeof ready_record) return 0;
    memcpy(ready_record, data, size);
    *written = short_write && size != 0 ? size - 1 : size;
    return 1;
}

static int CloseHandle(HANDLE file)
{
    if (file == &ready_handle) {
        close_ready_calls++;
        ready_handle.open = 0;
        if (fail_close_ready) { last_error = ERROR_ACCESS_DENIED; return 0; }
        return 1;
    }
    if (file == &ack_handle) {
        close_ack_calls++;
        ack_handle.open = 0;
        if (fail_close_ack) { last_error = ERROR_ACCESS_DENIED; return 0; }
        return 1;
    }
    last_error = ERROR_ACCESS_DENIED;
    return 0;
}

static DWORD GetFileType(HANDLE file)
{
    return file == &ack_handle ? (DWORD)fake_file_type : 0;
}

static int GetFileInformationByHandle(HANDLE file, BY_HANDLE_FILE_INFORMATION *info)
{
    if (file != &ack_handle || fail_file_info) return 0;
    info->dwFileAttributes = ack_attributes;
    info->nNumberOfLinks = ack_links;
    info->nFileSizeHigh = ack_size_high;
    info->nFileSizeLow = ack_size_low;
    return 1;
}

static int ReadFile(HANDLE file, void *buffer, DWORD capacity, DWORD *count, void *overlapped)
{
    static const char expected[] = "continue=K0\n";
    (void)overlapped;
    ack_read_calls++;
    if (file != &ack_handle || fail_read || capacity < sizeof expected - 1) return 0;
    memcpy(buffer, expected, sizeof expected - 1);
    if (ack_bad_bytes) ((char *)buffer)[0] = 'X';
    *count = ack_short_read ? (DWORD)(sizeof expected - 2) : (DWORD)(sizeof expected - 1);
    return 1;
}

#define snprintf fake_snprintf
#include "dxvk_heap_audit_windows.inc"
#undef snprintf

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int test_directory_inputs(void)
{
    wchar_t output[32];
    reset_fake();
    CHECK(audit_directory(output, 32) == 1);
    env_present = 0;
    CHECK(audit_directory(output, 32) == 0);
    env_present = 1;
    wcscpy(env_path, L"relative");
    CHECK(audit_directory(output, 32) == -1);
    wcscpy(env_path, L"C:");
    CHECK(audit_directory(output, 32) == -1);
    wcscpy(env_path, L"C:/audit");
    CHECK(audit_directory(output, 32) == -1);
    wcscpy(env_path, L"C:\\audit");
    CHECK(audit_directory(output, 4) == -1);
    directory_valid = 0;
    CHECK(audit_directory(output, 32) == -1);
    directory_valid = 1;
    directory_attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    CHECK(audit_directory(output, 32) == -1);
    env_present = 0;
    env_error = ERROR_ACCESS_DENIED;
    CHECK(audit_directory(output, 32) == -1);
    return 0;
}

static int test_acknowledgement_success_and_transient_errors(void)
{
    reset_fake();
    transient_not_found = 2;
    transient_sharing = 3;
    CHECK(audit_stage(L"C:\\audit", "K0") == 0);
    CHECK(poll_calls == 6 && close_ready_calls == 1 && close_ack_calls == 1);
    CHECK(strstr(ready_record, "stage=K0\n") != NULL);
    return 0;
}

static int test_acknowledgement_rejects_bad_objects(void)
{
    reset_fake();
    fake_file_type = 2;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    CHECK(close_ack_calls == 1);
    reset_fake();
    fail_file_info = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    ack_attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    ack_attributes |= FILE_ATTRIBUTE_DIRECTORY;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    ack_links = 2;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    ack_size_high = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    ack_size_low++;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_read = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    ack_short_read = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    ack_bad_bytes = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_close_ack = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    return 0;
}

static int test_bounded_failures(void)
{
    reset_fake();
    ack_already_exists = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_multibyte = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_process_times = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_handle_count = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    create_ready_error = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_write = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    short_write = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_close_ready = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    open_error = ERROR_ACCESS_DENIED;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    CHECK(poll_calls == 1);
    reset_fake();
    fail_record_format = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    oversized_record_format = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    fail_expected_format = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    oversized_expected_format = 1;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    reset_fake();
    transient_sharing = 6001;
    CHECK(audit_stage(L"C:\\audit", "K0") == 1);
    CHECK(poll_calls >= 6000 && fake_tick >= 60000);
    reset_fake();
    wchar_t long_path[1100];
    wmemset(long_path, L'A', 1050);
    long_path[1050] = L'\0';
    CHECK(audit_stage(long_path, "K0") == 1);
    wmemset(long_path, L'A', 1014);
    long_path[1014] = L'\0';
    CHECK(audit_stage(long_path, "K0") == 1);
    return 0;
}

int main(void)
{
    CHECK(test_directory_inputs() == 0);
    CHECK(test_acknowledgement_success_and_transient_errors() == 0);
    CHECK(test_acknowledgement_rejects_bad_objects() == 0);
    CHECK(test_bounded_failures() == 0);
    puts("DXVK heap audit stage tests passed");
    return 0;
}
