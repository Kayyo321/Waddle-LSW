/** @file opencl_os_preinit_windows.inc
 * @brief Approved OS-only first-use initialization before persistent reload baselines.
 *
 * Implements amendments 1-3 (2026-10-22) to OPENCL_RELOAD_CONTRACT.md. Windows keeps
 * process-lifetime state after the first use of USER32, Winsock, bcrypt, DXGI, default-order
 * DLL search, the package state repository and a nonblocking Winsock connect (see
 * OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md). Before the entry baseline the fixture runs
 * exactly seven Windows-component operations, in order, and nothing else. No Waddle, clvk or Vulkan loader code runs here. Exact baseline
 * equality is still required afterwards.
 *
 * Ownership: every module reference, Winsock startup, socket and COM reference acquired
 * by a step is released by that same step before it returns, on success and on every
 * failure path. Nothing is retained by this include.
 * Thread safety: sole main thread, before any fixture worker exists; not reentrant.
 * The caller supplies the Win32 declarations (windows.h natively, a fake for tests).
 */

/** Exact witness string recorded with the baseline; fixed order of the seven steps. */
#define OsPreinitRecord "user32,winsock,bcrypt,dxgi_factory,search_order,package_family,winsock_connect"

/** Fixed Winsock constants; winsock2.h is not included after windows.h. */
enum { OsPreinitWinsockVersion = 0x0202, OsPreinitAfInet = 2, OsPreinitSockStream = 1,
       OsPreinitTcp = 6, OsPreinitWsaDataBytes = 512, OsPreinitReleaseSlot = 2,
       OsPreinitPathCapacity = 520, OsPreinitStatusSuccess = 0, OsPreinitStatusInsufficientBuffer = 122,
       OsPreinitWouldBlock = 10035, OsPreinitLevelTcp = 6, OsPreinitTcpNoDelay = 1, OsPreinitSelectSeconds = 2 };
/** FIONBIO as defined by winsock2.h: _IOW('f', 126, u_long). */
#define OsPreinitFionbio ((long)0x8004667eUL)
/** Loopback 127.0.0.1 in network byte order as stored in a little-endian uint32_t. */
#define OsPreinitLoopback 0x0100007fu

typedef int (WINAPI *os_preinit_startup_t)(WORD, void *);
typedef uintptr_t (WINAPI *os_preinit_socket_t)(int, int, int);
typedef int (WINAPI *os_preinit_close_t)(uintptr_t);
typedef int (WINAPI *os_preinit_cleanup_t)(void);
typedef int32_t (WINAPI *os_preinit_factory_t)(const GUID *, void **); /* HRESULT is a 32-bit LONG */
typedef unsigned long (WINAPI *os_preinit_release_t)(void *);
typedef int32_t (WINAPI *os_preinit_packages_t)(const wchar_t *, uint32_t *, wchar_t **, uint32_t *, wchar_t *); /* LONG */
typedef int (WINAPI *os_preinit_bind_t)(uintptr_t, const void *, int);
typedef int (WINAPI *os_preinit_listen_t)(uintptr_t, int);
typedef int (WINAPI *os_preinit_name_t)(uintptr_t, void *, int *);
typedef int (WINAPI *os_preinit_option_t)(uintptr_t, int, int, const char *, int);
typedef int (WINAPI *os_preinit_ioctl_t)(uintptr_t, long, unsigned long *);
typedef int (WINAPI *os_preinit_connect_t)(uintptr_t, const void *, int);
typedef int (WINAPI *os_preinit_select_t)(int, void *, void *, void *, const void *);
typedef int (WINAPI *os_preinit_error_t)(void);
/** sockaddr_in, fd_set and timeval layouts of the Windows ABI; winsock2.h is not included. */
typedef struct os_preinit_address_t { uint16_t family, port; uint32_t address; unsigned char zero[8]; } os_preinit_address_t;
typedef struct os_preinit_set_t { unsigned count; uintptr_t sockets[64]; } os_preinit_set_t;
typedef struct os_preinit_wait_t { int32_t seconds, microseconds; } os_preinit_wait_t;

_Static_assert(sizeof(os_preinit_startup_t) == sizeof(FARPROC) && sizeof(os_preinit_socket_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_close_t) == sizeof(FARPROC) && sizeof(os_preinit_cleanup_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_factory_t) == sizeof(FARPROC) && sizeof(os_preinit_packages_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_bind_t) == sizeof(FARPROC) && sizeof(os_preinit_select_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_address_t) == 16 && sizeof(os_preinit_wait_t) == 8,
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

/** @brief Resolve one module by bare name through the default search order, then free it.
 *
 * The first name-based resolution makes ntdll open and keep its Session Manager key; a
 * full-path load does not. A bare name searches the application directory first, so the step
 * fails closed unless the loaded module is exactly <System32>\dwmapi.dll.
 * @return Zero on success; nonzero after freeing the module reference.
 */
static int os_preinit_search_order(void)
{
    wchar_t system[OsPreinitPathCapacity], loaded[OsPreinitPathCapacity];
    static const wchar_t Name[] = L"\\dwmapi.dll";
    HMODULE module = LoadLibraryW(L"dwmapi.dll");
    if (!module) return 1;
    unsigned length = GetSystemDirectoryW(system, OsPreinitPathCapacity);
    unsigned got = GetModuleFileNameW(module, loaded, OsPreinitPathCapacity);
    int failed = !length || length + sizeof Name / sizeof Name[0] > OsPreinitPathCapacity ||
                 !got || got >= OsPreinitPathCapacity;
    if (!failed) {
        memcpy(system + length, Name, sizeof Name);
        failed = _wcsicmp(system, loaded) != 0;
    }
    if (!FreeLibrary(module)) failed = 1;
    return failed;
}

/** @brief One size-only package-family query that initializes the OS state repository.
 * kernel32 is borrowed through GetModuleHandleW; no module reference or buffer is owned.
 * @return Zero only for ERROR_SUCCESS or ERROR_INSUFFICIENT_BUFFER.
 */
static int os_preinit_package_family(void)
{
    os_preinit_packages_t query = NULL;
    uint32_t count = 0, length = 0;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel || !os_preinit_resolve(kernel, "GetPackagesByPackageFamily", &query)) return 1;
    int32_t status = query(L"Microsoft.D3DMappingLayers_8wekyb3d8bbwe", &count, NULL, &length, NULL);
    return status != OsPreinitStatusSuccess && status != OsPreinitStatusInsufficientBuffer;
}

/** @brief One nonblocking 127.0.0.1 loopback connect completed with select (amendment 3).
 *
 * Opens a listener on an ephemeral loopback port and a TCP_NODELAY nonblocking client, connects
 * (immediate success or WSAEWOULDBLOCK only), waits at most two seconds for writability, then
 * closes both sockets, balances WSAStartup and frees the module. No accept and no data.
 * @return Zero on success; nonzero after releasing every socket, startup and module reference.
 */
static int os_preinit_winsock_connect(void)
{
    HMODULE module = NULL;
    os_preinit_startup_t startup = NULL;
    os_preinit_cleanup_t cleanup = NULL;
    os_preinit_socket_t open_socket = NULL;
    os_preinit_close_t close_socket = NULL;
    os_preinit_bind_t bind_socket = NULL;
    os_preinit_listen_t listen_socket = NULL;
    os_preinit_name_t socket_name = NULL;
    os_preinit_option_t set_option = NULL;
    os_preinit_ioctl_t io_control = NULL;
    os_preinit_connect_t connect_socket = NULL;
    os_preinit_select_t select_sockets = NULL;
    os_preinit_error_t last_error = NULL;
    union { unsigned char bytes[OsPreinitWsaDataBytes]; uint64_t align; } data;
    if (!os_preinit_load(L"ws2_32.dll", &module)) return 1;
    int failed = !os_preinit_resolve(module, "WSAStartup", &startup) ||
                 !os_preinit_resolve(module, "WSACleanup", &cleanup) ||
                 !os_preinit_resolve(module, "socket", &open_socket) ||
                 !os_preinit_resolve(module, "closesocket", &close_socket) ||
                 !os_preinit_resolve(module, "bind", &bind_socket) ||
                 !os_preinit_resolve(module, "listen", &listen_socket) ||
                 !os_preinit_resolve(module, "getsockname", &socket_name) ||
                 !os_preinit_resolve(module, "setsockopt", &set_option) ||
                 !os_preinit_resolve(module, "ioctlsocket", &io_control) ||
                 !os_preinit_resolve(module, "connect", &connect_socket) ||
                 !os_preinit_resolve(module, "select", &select_sockets) ||
                 !os_preinit_resolve(module, "WSAGetLastError", &last_error);
    if (!failed) {
        memset(&data, 0, sizeof data);
        if (startup(OsPreinitWinsockVersion, data.bytes) != 0) {
            failed = 1;
        } else {
            const uintptr_t invalid = ~(uintptr_t)0;
            uintptr_t server = open_socket(OsPreinitAfInet, OsPreinitSockStream, OsPreinitTcp);
            uintptr_t client = server == invalid ? invalid : open_socket(OsPreinitAfInet, OsPreinitSockStream, OsPreinitTcp);
            os_preinit_address_t address = {OsPreinitAfInet, 0, OsPreinitLoopback, {0}};
            int size = (int)sizeof address, nodelay = 1;
            unsigned long nonblocking = 1;
            failed = client == invalid ||
                     bind_socket(server, &address, (int)sizeof address) != 0 ||
                     listen_socket(server, 1) != 0 ||
                     socket_name(server, &address, &size) != 0 || size != (int)sizeof address ||
                     set_option(client, OsPreinitLevelTcp, OsPreinitTcpNoDelay, (const char *)&nodelay, (int)sizeof nodelay) != 0 ||
                     io_control(client, OsPreinitFionbio, &nonblocking) != 0;
            if (!failed && connect_socket(client, &address, (int)sizeof address) != 0 &&
                last_error() != OsPreinitWouldBlock) failed = 1;
            if (!failed) {
                os_preinit_set_t writable = {1, {client}};
                os_preinit_wait_t wait = {OsPreinitSelectSeconds, 0};
                if (select_sockets(0, NULL, &writable, NULL, &wait) != 1 || writable.count != 1 ||
                    writable.sockets[0] != client) failed = 1;
            }
            if (client != invalid && close_socket(client) != 0) failed = 1;
            if (server != invalid && close_socket(server) != 0) failed = 1;
            if (cleanup() != 0) failed = 1;
        }
    }
    if (!FreeLibrary(module)) failed = 1;
    return failed;
}

/** @brief Run the seven approved OS-only first-use operations in their fixed order.
 * @return Zero only when all seven steps succeeded; stops at the first failing step.
 * @note Callers must stop the run before measuring any baseline when this fails.
 */
static int os_preinitialize(void)
{
    return os_preinit_user32() || os_preinit_winsock() || os_preinit_bcrypt() ||
           os_preinit_dxgi_factory() || os_preinit_search_order() || os_preinit_package_family() ||
           os_preinit_winsock_connect();
}
