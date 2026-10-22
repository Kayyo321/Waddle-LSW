/** @file opencl_os_preinit_test.c @brief Actual OS pre-initialization include with Win32 API doubles.
 *
 * Exercises the exact include compiled into the Windows fixture. Every fakeable operation of
 * the success path is failed once in turn; after each failure the test proves that every module
 * reference, Winsock startup, socket and DXGI factory reference was released. No Windows loader,
 * Winsock, DXGI, native execution or GPU acceptance is claimed.
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
enum { FakeModules = 6, FakeTraceCapacity = 64, FakeTraceText = 64 };
enum { FactoryNormal, FactoryFailWithObject, FactoryFailWithoutObject, FactorySuccessWithoutObject };

static struct fake_module_t fake_modules[FakeModules] = {
    {L"user32.dll", 0}, {L"ws2_32.dll", 0}, {L"bcrypt.dll", 0}, {L"dxgi.dll", 0}, {L"dwmapi.dll", 0},
    {L"kernel32.dll", 0}};
static int fake_wrong_dwmapi_path, fake_package_status, fake_long_system, fake_truncated_module;
static char fake_trace[FakeTraceCapacity][FakeTraceText];
static unsigned fake_trace_count, fake_step, fake_fail_step;
static int fake_startups, fake_cleanups, fake_sockets_open, fake_sockets_closed;
static int fake_factories, fake_factory_releases, fake_factory_mode;
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

static int WINAPI fake_cleanup(void)
{
    assert(fake_startups > fake_cleanups);
    fake_record("WSACleanup");
    ++fake_cleanups;
    return fake_fails() ? -1 : 0;
}

static unsigned long WINAPI fake_factory_release(void *object);
static void *fake_factory_table[3] = {NULL, NULL, NULL};
static struct { void **table; } fake_factory_object = {fake_factory_table};

static unsigned long WINAPI fake_factory_release(void *object)
{
    assert(object == &fake_factory_object && fake_factories > fake_factory_releases);
    fake_record("Release");
    ++fake_factory_releases;
    return 0;
}

static int32_t WINAPI fake_create_factory(const GUID *riid, void **out)
{
    static const unsigned char expected[8] = {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87};
    assert(riid && out && *out == NULL && riid->Data1 == 0x770aae78u && riid->Data2 == 0xf26f &&
           riid->Data3 == 0x4dba && !memcmp(riid->Data4, expected, sizeof expected));
    fake_record("CreateDXGIFactory1");
    int failed = fake_fails();
    int mode = failed ? FactoryNormal : fake_factory_mode;
    if (failed) return INT32_C(-2005270527);  /* DXGI_ERROR_INVALID_CALL 0x887a0001 */
    if (mode == FactoryFailWithoutObject) return INT32_C(-2147467259);  /* E_FAIL 0x80004005 */
    if (mode == FactorySuccessWithoutObject) return 0;
    void *release = NULL;
    unsigned long (WINAPI *function)(void *) = fake_factory_release;
    memcpy(&release, &function, sizeof function);
    fake_factory_table[2] = release;
    *out = &fake_factory_object;
    ++fake_factories;
    return mode == FactoryFailWithObject ? INT32_C(-2147467259) : 0;
}

static int32_t WINAPI fake_packages(const wchar_t *family, uint32_t *count, wchar_t **names, uint32_t *length, wchar_t *buffer)
{
    assert(family && !wcscmp(family, L"Microsoft.D3DMappingLayers_8wekyb3d8bbwe") && count && length && !names && !buffer &&
           *count == 0 && *length == 0);
    fake_record("GetPackagesByPackageFamily");
    if (fake_fails()) return 2;  /* ERROR_FILE_NOT_FOUND */
    return fake_package_status;
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
    if (module == &fake_modules[3] && !strcmp(name, "CreateDXGIFactory1")) FakeExport(fake_create_factory);
    if (module == &fake_modules[5] && !strcmp(name, "GetPackagesByPackageFamily")) FakeExport(fake_packages);
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
    "load dxgi.dll", "resolve CreateDXGIFactory1", "CreateDXGIFactory1", "Release", "free dxgi.dll",
    "search dwmapi.dll", "GetSystemDirectoryW", "GetModuleFileNameW", "free dwmapi.dll",
    "borrow kernel32.dll", "resolve GetPackagesByPackageFamily", "GetPackagesByPackageFamily"};
enum { ExpectedSteps = sizeof ExpectedTrace / sizeof ExpectedTrace[0] - 1 /* Release is not fakeable */ };

static void reset(unsigned fail_step, int factory_mode)
{
    for (unsigned i = 0; i < FakeModules; ++i) fake_modules[i].references = 0;
    fake_trace_count = fake_step = 0;
    fake_fail_step = fail_step;
    fake_startups = fake_cleanups = fake_sockets_open = fake_sockets_closed = 0;
    fake_factories = fake_factory_releases = 0;
    fake_factory_mode = factory_mode;
    fake_wrong_dwmapi_path = 0;
    fake_package_status = 0;
    fake_long_system = fake_truncated_module = 0;
}

/** @brief Every acquired owner is released, whatever the outcome. */
static void assert_released(void)
{
    for (unsigned i = 0; i < FakeModules; ++i) assert(fake_modules[i].references == 0);
    assert(fake_startups == fake_cleanups);
    assert(fake_sockets_open == fake_sockets_closed);
    assert(fake_factories == fake_factory_releases);
}

int main(void)
{
    reset(0, FactoryNormal);
    assert(os_preinitialize() == 0);
    assert(fake_trace_count == sizeof ExpectedTrace / sizeof ExpectedTrace[0]);
    for (unsigned i = 0; i < fake_trace_count; ++i) assert(!strcmp(fake_trace[i], ExpectedTrace[i]));
    assert(fake_step == ExpectedSteps && fake_startups == 1 && fake_sockets_open == 1 && fake_factories == 1);
    assert_released();
    assert(!strcmp(OsPreinitRecord, "user32,winsock,bcrypt,dxgi_factory,search_order,package_family"));
    const unsigned success_calls = fake_trace_count;

    unsigned failures = 0;
    for (unsigned step = 1; step <= ExpectedSteps; ++step) {
        reset(step, FactoryNormal);
        assert(os_preinitialize() != 0);
        assert(fake_step >= step && fake_trace_count >= step);
        /* The failing operation is the step-th fakeable trace entry (Release is the only unfakeable
         * call). Its step may still release what it acquired, but no later step may start. */
        unsigned failing_index = step - 1 + (step > 17u);  /* skip the Release entry */
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

    static const int Modes[] = {FactoryFailWithObject, FactoryFailWithoutObject, FactorySuccessWithoutObject};
    for (unsigned i = 0; i < sizeof Modes / sizeof Modes[0]; ++i) {
        reset(0, Modes[i]);
        assert(os_preinitialize() != 0);
        assert(fake_modules[3].references == 0 && !strcmp(fake_trace[fake_trace_count - 1], "free dxgi.dll"));
        assert(fake_factories == (Modes[i] == FactoryFailWithObject));
        assert_released();
        ++failures;
    }
    reset(0, FactoryNormal);
    fake_wrong_dwmapi_path = 1;  /* a planted application-directory dwmapi.dll is rejected and freed */
    assert(os_preinitialize() != 0);
    assert(!strcmp(fake_trace[fake_trace_count - 1], "free dwmapi.dll"));
    assert_released();
    ++failures;
    for (int mode = 0; mode < 2; ++mode) {
        reset(0, FactoryNormal);
        if (mode) fake_truncated_module = 1; else fake_long_system = 1;
        assert(os_preinitialize() != 0);
        assert(!strcmp(fake_trace[fake_trace_count - 1], "free dwmapi.dll"));
        assert_released();
        ++failures;
    }
    reset(0, FactoryNormal);
    fake_package_status = 122;  /* ERROR_INSUFFICIENT_BUFFER: package present, accepted */
    assert(os_preinitialize() == 0);
    assert_released();
    static const int32_t Statuses[] = {2, 5, 87, 1168, -1};
    for (unsigned i = 0; i < sizeof Statuses / sizeof Statuses[0]; ++i) {
        reset(0, FactoryNormal);
        fake_package_status = Statuses[i];
        assert(os_preinitialize() != 0);
        assert(!strcmp(fake_trace[fake_trace_count - 1], "GetPackagesByPackageFamily"));
        assert_released();
        ++failures;
    }
    printf("OpenCL OS pre-initialization doubles: success trace %u calls, %u failure paths released all owners\n",
           success_calls, failures);
    return 0;
}
