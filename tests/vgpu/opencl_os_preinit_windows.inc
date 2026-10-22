/** @file opencl_os_preinit_windows.inc
 * @brief Approved OS-only first-use initialization before persistent reload baselines.
 *
 * Implements the 2026-10-22 amendment to OPENCL_RELOAD_CONTRACT.md. Windows keeps
 * process-lifetime state after the first use of USER32, Winsock, bcrypt and DXGI (see
 * OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md). Before the entry baseline the fixture runs
 * exactly four Windows-component operations, in order, and nothing else. No Waddle, clvk
 * or Vulkan loader code runs here. Exact baseline equality is still required afterwards.
 *
 * Ownership: every module reference, Winsock startup, socket and COM reference acquired
 * by a step is released by that same step before it returns, on success and on every
 * failure path. Nothing is retained by this include.
 * Thread safety: sole main thread, before any fixture worker exists; not reentrant.
 * The caller supplies the Win32 declarations (windows.h natively, a fake for tests).
 */

/** Exact witness string recorded with the baseline; fixed order of the four steps. */
#define OsPreinitRecord "user32,winsock,bcrypt,dxgi_factory"

/** Fixed Winsock constants; winsock2.h is not included after windows.h. */
enum { OsPreinitWinsockVersion = 0x0202, OsPreinitAfInet = 2, OsPreinitSockStream = 1,
       OsPreinitTcp = 6, OsPreinitWsaDataBytes = 512, OsPreinitReleaseSlot = 2 };

typedef int (WINAPI *os_preinit_startup_t)(WORD, void *);
typedef uintptr_t (WINAPI *os_preinit_socket_t)(int, int, int);
typedef int (WINAPI *os_preinit_close_t)(uintptr_t);
typedef int (WINAPI *os_preinit_cleanup_t)(void);
typedef int32_t (WINAPI *os_preinit_factory_t)(const GUID *, void **); /* HRESULT is a 32-bit LONG */
typedef unsigned long (WINAPI *os_preinit_release_t)(void *);

_Static_assert(sizeof(os_preinit_startup_t) == sizeof(FARPROC) && sizeof(os_preinit_socket_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_close_t) == sizeof(FARPROC) && sizeof(os_preinit_cleanup_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_factory_t) == sizeof(FARPROC),
               "export storage must match FARPROC exactly");

/** IID_IDXGIFactory1 {770aae78-f26f-4dba-a829-253c83d1b387}. */
static const GUID OsPreinitFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

/** @brief Load one System32 module by bare name.
 * @param[in] name Borrowed nonnull module file name without a directory.
 * @param[out] module Receives an owned module reference on success; NULL on failure.
 * @return Nonzero only when the caller now owns one reference to release with FreeLibrary.
 */
static int os_preinit_load(const wchar_t *name, HMODULE *module)
{
    *module = LoadLibraryExW(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return *module != NULL;
}

/** @brief Resolve one export into caller storage without casting function types.
 * @param[in] module Borrowed live module reference.
 * @param[in] name Borrowed nonnull export name.
 * @param[out] target Function-pointer storage of exactly sizeof(FARPROC) bytes (checked
 *                    statically for every caller type); untouched on failure.
 * @return Nonzero only when the export exists.
 */
static int os_preinit_resolve(HMODULE module, const char *name, void *target)
{
    FARPROC address = GetProcAddress(module, name);
    if (!address) return 0;
    memcpy(target, &address, sizeof address);
    return 1;
}

/** @brief Load and free System32 USER32, which connects the process to win32k.
 * @return Zero on success; nonzero after releasing everything acquired.
 */
static int os_preinit_user32(void)
{
    HMODULE module = NULL;
    if (!os_preinit_load(L"user32.dll", &module)) return 1;
    return !FreeLibrary(module);
}

/** @brief One balanced Winsock startup, IPv4 TCP socket open/close and cleanup.
 * @return Zero on success; nonzero after closing the socket, balancing a successful
 *         startup and freeing the module reference.
 */
static int os_preinit_winsock(void)
{
    HMODULE module = NULL;
    os_preinit_startup_t startup = NULL;
    os_preinit_socket_t open_socket = NULL;
    os_preinit_close_t close_socket = NULL;
    os_preinit_cleanup_t cleanup = NULL;
    union { unsigned char bytes[OsPreinitWsaDataBytes]; uint64_t align; } data;
    if (!os_preinit_load(L"ws2_32.dll", &module)) return 1;
    int failed = !os_preinit_resolve(module, "WSAStartup", &startup) ||
                 !os_preinit_resolve(module, "socket", &open_socket) ||
                 !os_preinit_resolve(module, "closesocket", &close_socket) ||
                 !os_preinit_resolve(module, "WSACleanup", &cleanup);
    if (!failed) {
        memset(&data, 0, sizeof data);
        if (startup(OsPreinitWinsockVersion, data.bytes) != 0) {
            failed = 1;
        } else {
            uintptr_t handle = open_socket(OsPreinitAfInet, OsPreinitSockStream, OsPreinitTcp);
            if (handle == ~(uintptr_t)0) failed = 1;
            else if (close_socket(handle) != 0) failed = 1;
            if (cleanup() != 0) failed = 1;
        }
    }
    if (!FreeLibrary(module)) failed = 1;
    return failed;
}

/** @brief Load and free System32 bcrypt, which registers its providers.
 * @return Zero on success; nonzero after releasing everything acquired.
 */
static int os_preinit_bcrypt(void)
{
    HMODULE module = NULL;
    if (!os_preinit_load(L"bcrypt.dll", &module)) return 1;
    return !FreeLibrary(module);
}

/** @brief Create and release one IDXGIFactory1; no adapter is enumerated.
 * @return Zero on success; nonzero after releasing a returned factory and the module.
 */
static int os_preinit_dxgi_factory(void)
{
    HMODULE module = NULL;
    os_preinit_factory_t create = NULL;
    void *factory = NULL;
    if (!os_preinit_load(L"dxgi.dll", &module)) return 1;
    int failed = !os_preinit_resolve(module, "CreateDXGIFactory1", &create);
    if (!failed) {
        int32_t result = create(&OsPreinitFactory1, &factory);
        if (result < 0) failed = 1;
        if (factory) {
            void **table = *(void ***)factory;
            os_preinit_release_t release = NULL;
            memcpy(&release, &table[OsPreinitReleaseSlot], sizeof release);
            release(factory);
            factory = NULL;
        } else {
            failed = 1;
        }
    }
    if (!FreeLibrary(module)) failed = 1;
    return failed;
}

/** @brief Run the four approved OS-only first-use operations in their fixed order.
 * @return Zero only when all four steps succeeded; stops at the first failing step.
 * @note Callers must stop the run before measuring any baseline when this fails.
 */
static int os_preinitialize(void)
{
    return os_preinit_user32() || os_preinit_winsock() || os_preinit_bcrypt() ||
           os_preinit_dxgi_factory();
}
