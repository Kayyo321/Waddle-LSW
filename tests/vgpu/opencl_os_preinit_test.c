/** @file opencl_os_preinit_test.c @brief Actual OS pre-initialization include with Win32 API doubles.
 *
 * Exercises the exact include compiled into the Windows fixture. Every fakeable operation of
 * the success path is failed once in turn; after each failure the test proves that every module
 * reference, Winsock startup, socket, registry key and RNG provider was released. No Windows loader, Winsock, DXGI,
 * native execution or GPU acceptance is claimed.
 */
#include <assert.h>
#ifdef NDEBUG
#error Assertion-elided ownership controls are forbidden
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "win32_fake/opencl_os_preinit_fake.h"

/** One fake System32 module with an exact reference count. */
struct fake_module_t { const wchar_t *name; int references; };
enum { FakeModules = 8, FakeTraceCapacity = 96, FakeTraceText = 64 };

static struct fake_module_t fake_modules[FakeModules] = {
    {L"user32.dll", 0}, {L"ws2_32.dll", 0}, {L"bcrypt.dll", 0}, {L"dxgi.dll", 0}, {L"dwmapi.dll", 0},
    {L"kernel32.dll", 0}, {L"advapi32.dll", 0}, {L"gdi32.dll", 0}};
static int fake_wrong_dwmapi_path, fake_package_status, fake_long_system, fake_truncated_module;
static int fake_connect_immediate, fake_name_size_wrong, fake_select_mode, fake_last_error = 10035;
typedef struct { uint16_t family, port; uint32_t address; unsigned char zero[8]; } fake_address_t;
typedef struct { unsigned count; uintptr_t sockets[64]; } fake_set_t;
static char fake_trace[FakeTraceCapacity][FakeTraceText];
static unsigned fake_trace_count, fake_step, fake_fail_step;
static int fake_startups, fake_cleanups, fake_sockets_open, fake_sockets_closed;
static int fake_keys_open, fake_keys_closed, fake_providers_open, fake_providers_closed;
static int fake_key_object, fake_provider_object;
static uintptr_t fake_next_socket = 0x40;

/** @brief Count one fakeable operation; returns nonzero when it is the injected failure. */
static int fake_fails(void)
{
    ++fake_step;
    return fake_fail_step != 0 && fake_step == fake_fail_step;
}

/** @brief Append one exact trace record; aborts on overflow. */
static void fake_record(const char *text)
{
    assert(fake_trace_count < FakeTraceCapacity && strlen(text) < FakeTraceText);
    strcpy(fake_trace[fake_trace_count++], text);
}

static int WINAPI fake_startup(WORD version, void *data)
{
    assert(version == 0x0202 && data);
    fake_record("WSAStartup");
    if (fake_fails()) return 10091;
    ++fake_startups;
    return 0;
}

static uintptr_t WINAPI fake_socket(int family, int type, int protocol)
{
    assert(family == 2 && type == 1 && protocol == 6 && fake_startups > fake_cleanups);
    fake_record("socket");
    if (fake_fails()) return ~(uintptr_t)0;
    ++fake_sockets_open;
    return fake_next_socket++;
}

static int WINAPI fake_closesocket(uintptr_t socket)
{
    assert(socket >= 0x40 && socket < fake_next_socket);
    fake_record("closesocket");
    ++fake_sockets_closed;
    return fake_fails() ? -1 : 0;
}

static int WINAPI fake_bind(uintptr_t socket, const void *address, int size)
{
    const fake_address_t *a = address;
    assert(socket >= 0x40 && a && size == 16 && a->family == 2 && a->port == 0 && a->address == 0x0100007fu);
    fake_record("bind");
    return fake_fails() ? -1 : 0;
}

static int WINAPI fake_listen(uintptr_t socket, int backlog)
{
    assert(socket >= 0x40 && backlog == 1);
    fake_record("listen");
    return fake_fails() ? -1 : 0;
}

static int WINAPI fake_getsockname(uintptr_t socket, void *address, int *size)
{
    assert(socket >= 0x40 && address && size && *size == 16);
    fake_record("getsockname");
    if (fake_fails()) return -1;
    ((fake_address_t *)address)->port = 0x3930;
    if (fake_name_size_wrong) *size = 8;
    return 0;
}

static int WINAPI fake_setsockopt(uintptr_t socket, int level, int name, const char *value, int size)
{
    int enabled = 0;
    assert(socket >= 0x40 && level == 6 && name == 1 && value && size == (int)sizeof enabled);
    memcpy(&enabled, value, sizeof enabled);
    assert(enabled == 1);
    fake_record("setsockopt");
    return fake_fails() ? -1 : 0;
}

static int WINAPI fake_ioctlsocket(uintptr_t socket, long command, unsigned long *value)
{
    assert(socket >= 0x40 && command == (long)0x8004667eUL && value && *value == 1);
    fake_record("ioctlsocket");
    return fake_fails() ? -1 : 0;
}

static int WINAPI fake_connect(uintptr_t socket, const void *address, int size)
{
    const fake_address_t *a = address;
    assert(socket >= 0x40 && a && size == 16 && a->port == 0x3930 && a->address == 0x0100007fu);
    fake_record("connect");
    if (fake_fails()) { fake_last_error = 10061; return -1; }  /* WSAECONNREFUSED */
    return fake_connect_immediate ? 0 : -1;
}

static int WINAPI fake_wsa_last_error(void)
{
    fake_record("WSAGetLastError");
    return fake_fails() ? 10061 : fake_last_error;
}

static int WINAPI fake_select(int ignored, void *read, void *write, void *except, const void *wait)
{
    const int32_t *time = wait;
    fake_set_t *writable = write;
    assert(!ignored && !read && !except && writable && writable->count == 1 && time && time[0] == 2 && time[1] == 0);
    fake_record("select");
    if (fake_fails()) return 0;  /* timeout */
    if (fake_select_mode == 1) writable->count = 0;
    if (fake_select_mode == 2) writable->sockets[0] = 0x1000;
    return 1;
}

static int WINAPI fake_cleanup(void)
{
    assert(fake_startups > fake_cleanups);
    fake_record("WSACleanup");
    ++fake_cleanups;
    return fake_fails() ? -1 : 0;
}

static int32_t WINAPI fake_packages(const wchar_t *family, uint32_t *count, wchar_t **names, uint32_t *length, wchar_t *buffer)
{
    assert(family && !wcscmp(family, L"Microsoft.D3DMappingLayers_8wekyb3d8bbwe") && count && length && !names && !buffer &&
           *count == 0 && *length == 0);
    fake_record("GetPackagesByPackageFamily");
    if (fake_fails()) return 2;  /* ERROR_FILE_NOT_FOUND */
    return fake_package_status;
}

static int32_t WINAPI fake_open_key(void *root, const wchar_t *name, uint32_t options, uint32_t access, void **key)
{
    assert(root == (void *)(intptr_t)INT32_C(-2147483647) && name && !wcscmp(name, L"Software") && options == 0 &&
           access == 0x20019 && key);
    fake_record("RegOpenKeyExW");
    if (fake_fails()) return 2;  /* ERROR_FILE_NOT_FOUND */
    ++fake_keys_open;
    *key = &fake_key_object;
    return 0;
}

static int32_t WINAPI fake_close_key(void *key)
{
    assert(key == &fake_key_object && fake_keys_open > fake_keys_closed);
    fake_record("RegCloseKey");
    ++fake_keys_closed;
    return fake_fails() ? 6 : 0;  /* ERROR_INVALID_HANDLE */
}

static int32_t WINAPI fake_open_provider(void **provider, const wchar_t *algorithm, const wchar_t *implementation, uint32_t flags)
{
    assert(provider && algorithm && !wcscmp(algorithm, L"RNG") && implementation &&
           !wcscmp(implementation, L"Microsoft Primitive Provider") && flags == 0);
    fake_record("BCryptOpenAlgorithmProvider");
    if (fake_fails()) return INT32_C(-1073741275);  /* STATUS_NOT_FOUND 0xc0000225 */
    ++fake_providers_open;
    *provider = &fake_provider_object;
    return 0;
}

static int32_t WINAPI fake_generate(void *provider, unsigned char *bytes, uint32_t length, uint32_t flags)
{
    assert(provider == &fake_provider_object && bytes && length == 16 && flags == 0);
    fake_record("BCryptGenRandom");
    if (fake_fails()) return INT32_C(-1073741823);  /* STATUS_UNSUCCESSFUL */
    memset(bytes, 0xa5, length);
    return 0;
}

static int32_t WINAPI fake_close_provider(void *provider, uint32_t flags)
{
    assert(provider == &fake_provider_object && flags == 0 && fake_providers_open > fake_providers_closed);
    fake_record("BCryptCloseAlgorithmProvider");
    ++fake_providers_closed;
    return fake_fails() ? INT32_C(-1073741816) : 0;  /* STATUS_INVALID_HANDLE */
}

typedef struct { uint32_t count; void *adapters; } fake_adapters_t;
static int32_t WINAPI fake_enum_adapters(fake_adapters_t *adapters)
{
    assert(adapters && adapters->count == 0 && adapters->adapters == NULL);
    fake_record("D3DKMTEnumAdapters2");
    if (fake_fails()) return INT32_C(-1073741823);  /* STATUS_UNSUCCESSFUL */
    adapters->count = 2;
    return 0;
}

/** @brief Convert one fake function to FARPROC storage without a function-type cast. */
#define FakeExport(function) do { FARPROC address; __typeof__(&function) source = function; \
    _Static_assert(sizeof source == sizeof address, "fake export size"); \
    memcpy(&address, &source, sizeof address); return address; } while (0)

static HMODULE LoadLibraryExW(const wchar_t *name, HANDLE file, DWORD flags)
{
    assert(name && file == NULL && flags == LOAD_LIBRARY_SEARCH_SYSTEM32 && !wcschr(name, L'\\'));
    char text[FakeTraceText];
    snprintf(text, sizeof text, "load %ls", name);
    fake_record(text);
    if (fake_fails()) return NULL;
    for (unsigned i = 0; i < FakeModules; ++i)
        if (!wcscmp(fake_modules[i].name, name)) { ++fake_modules[i].references; return &fake_modules[i]; }
    assert(!"unexpected module");
    return NULL;
}

static FARPROC GetProcAddress(HMODULE module, const char *name)
{
    assert(module && (module->references > 0 || module == &fake_modules[5]) && name);  /* kernel32 is always resident */
    char text[FakeTraceText];
    snprintf(text, sizeof text, "resolve %s", name);
    fake_record(text);
    if (fake_fails()) return NULL;
    if (module == &fake_modules[1] && !strcmp(name, "WSAStartup")) FakeExport(fake_startup);
    if (module == &fake_modules[1] && !strcmp(name, "socket")) FakeExport(fake_socket);
    if (module == &fake_modules[1] && !strcmp(name, "closesocket")) FakeExport(fake_closesocket);
    if (module == &fake_modules[1] && !strcmp(name, "WSACleanup")) FakeExport(fake_cleanup);
    if (module == &fake_modules[5] && !strcmp(name, "GetPackagesByPackageFamily")) FakeExport(fake_packages);
    if (module == &fake_modules[1] && !strcmp(name, "bind")) FakeExport(fake_bind);
    if (module == &fake_modules[1] && !strcmp(name, "listen")) FakeExport(fake_listen);
    if (module == &fake_modules[1] && !strcmp(name, "getsockname")) FakeExport(fake_getsockname);
    if (module == &fake_modules[1] && !strcmp(name, "setsockopt")) FakeExport(fake_setsockopt);
    if (module == &fake_modules[1] && !strcmp(name, "ioctlsocket")) FakeExport(fake_ioctlsocket);
    if (module == &fake_modules[1] && !strcmp(name, "connect")) FakeExport(fake_connect);
    if (module == &fake_modules[1] && !strcmp(name, "select")) FakeExport(fake_select);
    if (module == &fake_modules[1] && !strcmp(name, "WSAGetLastError")) FakeExport(fake_wsa_last_error);
    if (module == &fake_modules[6] && !strcmp(name, "RegOpenKeyExW")) FakeExport(fake_open_key);
    if (module == &fake_modules[6] && !strcmp(name, "RegCloseKey")) FakeExport(fake_close_key);
    if (module == &fake_modules[2] && !strcmp(name, "BCryptOpenAlgorithmProvider")) FakeExport(fake_open_provider);
    if (module == &fake_modules[2] && !strcmp(name, "BCryptGenRandom")) FakeExport(fake_generate);
    if (module == &fake_modules[2] && !strcmp(name, "BCryptCloseAlgorithmProvider")) FakeExport(fake_close_provider);
    if (module == &fake_modules[7] && !strcmp(name, "D3DKMTEnumAdapters2")) FakeExport(fake_enum_adapters);
    assert(!"unexpected export");
    return NULL;
}

static HMODULE LoadLibraryW(const wchar_t *name)
{
    assert(name && !wcscmp(name, L"dwmapi.dll"));  /* bare name: default search order */
    fake_record("search dwmapi.dll");
    if (fake_fails()) return NULL;
    ++fake_modules[4].references;
    return &fake_modules[4];
}

static HMODULE GetModuleHandleW(const wchar_t *name)
{
    assert(name && !wcscmp(name, L"kernel32.dll"));
    fake_record("borrow kernel32.dll");
    if (fake_fails()) return NULL;
    return &fake_modules[5];  /* borrowed: no reference */
}

static unsigned GetSystemDirectoryW(wchar_t *buffer, unsigned capacity)
{
    static const wchar_t System[] = L"C:\\Windows\\System32";
    assert(buffer && capacity >= sizeof System / sizeof System[0]);
    fake_record("GetSystemDirectoryW");
    if (fake_fails()) return 0;
    memcpy(buffer, System, sizeof System);
    if (fake_long_system) return capacity - 2;  /* no room left for the module name */
    return (unsigned)(sizeof System / sizeof System[0] - 1);
}

static DWORD GetModuleFileNameW(HMODULE module, wchar_t *buffer, DWORD capacity)
{
    assert(module == &fake_modules[4] && module->references > 0 && buffer && capacity > 64);
    fake_record("GetModuleFileNameW");
    if (fake_fails()) return 0;
    const wchar_t *path = fake_wrong_dwmapi_path ? L"C:\\fixture\\dwmapi.dll" : L"c:\\windows\\system32\\DWMAPI.DLL";
    wcscpy(buffer, path);
    if (fake_truncated_module) return capacity;  /* Windows truncation reports the full capacity */
    return (DWORD)wcslen(path);
}

static int _wcsicmp(const wchar_t *left, const wchar_t *right)
{
    for (;; ++left, ++right) {
        wint_t a = towlower((wint_t)*left), b = towlower((wint_t)*right);
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
}

static BOOL FreeLibrary(HMODULE module)
{
    assert(module && module->references > 0);
    char text[FakeTraceText];
    snprintf(text, sizeof text, "free %ls", module->name);
    fake_record(text);
    --module->references;  /* The reference is released even when the reported result fails. */
    return !fake_fails();
}

#include "opencl_os_preinit_windows.inc"

static const char *const ExpectedTrace[] = {
    "load user32.dll", "free user32.dll",
    "load ws2_32.dll", "resolve WSAStartup", "resolve socket", "resolve closesocket", "resolve WSACleanup",
    "WSAStartup", "socket", "closesocket", "WSACleanup", "free ws2_32.dll",
    "load bcrypt.dll", "free bcrypt.dll",
    "load dxgi.dll", "free dxgi.dll",
    "search dwmapi.dll", "GetSystemDirectoryW", "GetModuleFileNameW", "free dwmapi.dll",
    "borrow kernel32.dll", "resolve GetPackagesByPackageFamily", "GetPackagesByPackageFamily",
    "load ws2_32.dll", "resolve WSAStartup", "resolve WSACleanup", "resolve socket", "resolve closesocket", "resolve bind",
    "resolve listen", "resolve getsockname", "resolve setsockopt", "resolve ioctlsocket", "resolve connect", "resolve select",
    "resolve WSAGetLastError", "WSAStartup", "socket", "socket", "bind", "listen", "getsockname", "setsockopt", "ioctlsocket",
    "connect", "WSAGetLastError", "select", "closesocket", "closesocket", "WSACleanup", "free ws2_32.dll",
    "load advapi32.dll", "resolve RegOpenKeyExW", "resolve RegCloseKey", "RegOpenKeyExW", "RegCloseKey", "free advapi32.dll",
    "load bcrypt.dll", "resolve BCryptOpenAlgorithmProvider", "resolve BCryptGenRandom", "resolve BCryptCloseAlgorithmProvider",
    "BCryptOpenAlgorithmProvider", "BCryptGenRandom", "BCryptCloseAlgorithmProvider", "free bcrypt.dll",
    "load gdi32.dll", "resolve D3DKMTEnumAdapters2", "D3DKMTEnumAdapters2", "free gdi32.dll"};
enum { ExpectedSteps = sizeof ExpectedTrace / sizeof ExpectedTrace[0] };

static void reset(unsigned fail_step)
{
    for (unsigned i = 0; i < FakeModules; ++i) fake_modules[i].references = 0;
    fake_trace_count = fake_step = 0;
    fake_fail_step = fail_step;
    fake_startups = fake_cleanups = fake_sockets_open = fake_sockets_closed = 0;
    fake_keys_open = fake_keys_closed = fake_providers_open = fake_providers_closed = 0;
    fake_wrong_dwmapi_path = 0;
    fake_package_status = 0;
    fake_long_system = fake_truncated_module = 0;
    fake_connect_immediate = fake_name_size_wrong = fake_select_mode = 0;
    fake_last_error = 10035;  /* WSAEWOULDBLOCK */
}

/** @brief Every acquired owner is released, whatever the outcome. */
static void assert_released(void)
{
    for (unsigned i = 0; i < FakeModules; ++i) assert(fake_modules[i].references == 0);
    assert(fake_startups == fake_cleanups);
    assert(fake_sockets_open == fake_sockets_closed);
    assert(fake_keys_open == fake_keys_closed && fake_providers_open == fake_providers_closed);
}

int main(void)
{
    reset(0);
    assert(os_preinitialize() == 0);
    assert(fake_trace_count == sizeof ExpectedTrace / sizeof ExpectedTrace[0]);
    for (unsigned i = 0; i < fake_trace_count; ++i) assert(!strcmp(fake_trace[i], ExpectedTrace[i]));
    assert(fake_step == ExpectedSteps && fake_startups == 2 && fake_sockets_open == 3 && fake_keys_open == 1 &&
           fake_providers_open == 1);
    assert_released();
    assert(!strcmp(OsPreinitRecord, "user32,winsock,bcrypt,dxgi_module,search_order,package_family,winsock_connect,"
                                   "user_hive,random,d3dkmt_adapters"));
    const unsigned success_calls = fake_trace_count;

    unsigned failures = 0;
    for (unsigned step = 1; step <= ExpectedSteps; ++step) {
        reset(step);
        assert(os_preinitialize() != 0);
        assert(fake_step >= step && fake_trace_count >= step);
        /* The failing operation is the step-th trace entry; every operation is fakeable. Its step
         * may still release what it acquired, but no later step may start. */
        unsigned failing_index = step - 1;
        assert(failing_index < fake_trace_count);
        const char *failing = fake_trace[failing_index], *module = NULL;
        for (unsigned i = 0; i <= failing_index; ++i) {
            if (!strncmp(fake_trace[i], "load ", 5)) module = fake_trace[i] + 5;
            if (!strcmp(fake_trace[i], "search dwmapi.dll")) module = "dwmapi.dll";
            if (!strcmp(fake_trace[i], "borrow kernel32.dll")) module = NULL;
        }
        const char *last = fake_trace[fake_trace_count - 1];
        if (module) {
            char released[FakeTraceText];
            snprintf(released, sizeof released, "free %s", module);
            assert(!strcmp(last, released) || (last == failing && fake_trace_count == failing_index + 1));
        } else {
            assert(last == failing && fake_trace_count == failing_index + 1);  /* borrowed kernel32: nothing to release */
        }
        for (unsigned i = failing_index + 1; i < fake_trace_count; ++i)
            assert(strncmp(fake_trace[i], "load ", 5) && strncmp(fake_trace[i], "search ", 7) && strncmp(fake_trace[i], "borrow ", 7));
        assert_released();
        ++failures;
    }

    reset(0);
    fake_wrong_dwmapi_path = 1;  /* a planted application-directory dwmapi.dll is rejected and freed */
    assert(os_preinitialize() != 0);
    assert(!strcmp(fake_trace[fake_trace_count - 1], "free dwmapi.dll"));
    assert_released();
    ++failures;
    for (int mode = 0; mode < 2; ++mode) {
        reset(0);
        if (mode) fake_truncated_module = 1; else fake_long_system = 1;
        assert(os_preinitialize() != 0);
        assert(!strcmp(fake_trace[fake_trace_count - 1], "free dwmapi.dll"));
        assert_released();
        ++failures;
    }
    reset(0);
    fake_package_status = 122;  /* ERROR_INSUFFICIENT_BUFFER: package present, accepted */
    assert(os_preinitialize() == 0);
    assert_released();
    static const int32_t Statuses[] = {2, 5, 87, 1168, -1};
    for (unsigned i = 0; i < sizeof Statuses / sizeof Statuses[0]; ++i) {
        reset(0);
        fake_package_status = Statuses[i];
        assert(os_preinitialize() != 0);
        assert(!strcmp(fake_trace[fake_trace_count - 1], "GetPackagesByPackageFamily"));
        assert_released();
        ++failures;
    }
    reset(0);
    fake_connect_immediate = 1;  /* immediate loopback success skips WSAGetLastError */
    assert(os_preinitialize() == 0);
    for (unsigned i = 0; i < fake_trace_count; ++i) assert(strcmp(fake_trace[i], "WSAGetLastError"));
    assert_released();
    for (int mode = 0; mode < 3; ++mode) {
        reset(0);
        if (mode == 0) fake_name_size_wrong = 1; else fake_select_mode = mode;
        assert(os_preinitialize() != 0);
        assert(!strcmp(fake_trace[fake_trace_count - 1], "free ws2_32.dll"));
        assert_released();
        ++failures;
    }
    printf("OpenCL OS pre-initialization doubles: success trace %u calls, %u failure paths released all owners\n",
           success_calls, failures);
    return 0;
}
