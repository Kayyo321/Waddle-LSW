/** @file opencl_os_preinit_windows.inc
 * @brief Approved OS-only first-use initialization before persistent reload baselines.
 *
 * Implements amendments 1-4 (2026-10-22) to OPENCL_RELOAD_CONTRACT.md. Windows keeps
 * process-lifetime state after the first use of USER32, Winsock, bcrypt, dxgi.dll, default-order
 * DLL search, the package state repository, a nonblocking Winsock connect, the current-user
 * registry hive, the bcrypt RNG provider and D3DKMT adapter enumeration (see
 * OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md). Before the entry baseline the fixture runs
 * exactly ten Windows-component operations, in order, and nothing else. No Waddle, clvk or Vulkan loader code runs here. Exact baseline
 * equality is still required afterwards.
 *
 * Ownership: every module reference, Winsock startup and socket acquired
 * by a step is released by that same step before it returns, on success and on every
 * failure path. Nothing is retained by this include.
 * Thread safety: sole main thread, before any fixture worker exists; not reentrant.
 * The caller supplies the Win32 declarations (windows.h natively, a fake for tests).
 */

/** Exact witness string recorded with the baseline; fixed order of the ten steps. */
#define OsPreinitRecord "user32,winsock,bcrypt,dxgi_module,search_order,package_family,winsock_connect,user_hive,random,d3dkmt_adapters"

/** Fixed Winsock constants; winsock2.h is not included after windows.h. */
enum { OsPreinitWinsockVersion = 0x0202, OsPreinitAfInet = 2, OsPreinitSockStream = 1,
       OsPreinitTcp = 6, OsPreinitWsaDataBytes = 512,
       OsPreinitPathCapacity = 520, OsPreinitStatusSuccess = 0, OsPreinitStatusInsufficientBuffer = 122,
       OsPreinitWouldBlock = 10035, OsPreinitLevelTcp = 6, OsPreinitTcpNoDelay = 1, OsPreinitSelectSeconds = 2 };
/** FIONBIO as defined by winsock2.h: _IOW('f', 126, u_long). */
#define OsPreinitFionbio ((long)0x8004667eUL)
/** Loopback 127.0.0.1 in network byte order as stored in a little-endian uint32_t. */
#define OsPreinitLoopback 0x0100007fu
/** Registry, bcrypt and D3DKMT constants; winreg.h, bcrypt.h and d3dkmthk.h are not included. */
enum { OsPreinitKeyRead = 0x20019, OsPreinitRandomBytes = 16 };
/** Predefined HKEY_CURRENT_USER, (HKEY)(ULONG_PTR)(LONG)0x80000001: a sign-extended handle value. */
#define OsPreinitCurrentUser ((void *)(intptr_t)INT32_C(-2147483647))

typedef int (WINAPI *os_preinit_startup_t)(WORD, void *);
typedef uintptr_t (WINAPI *os_preinit_socket_t)(int, int, int);
typedef int (WINAPI *os_preinit_close_t)(uintptr_t);
typedef int (WINAPI *os_preinit_cleanup_t)(void);
typedef int32_t (WINAPI *os_preinit_packages_t)(const wchar_t *, uint32_t *, wchar_t **, uint32_t *, wchar_t *); /* LONG */
typedef int (WINAPI *os_preinit_bind_t)(uintptr_t, const void *, int);
typedef int (WINAPI *os_preinit_listen_t)(uintptr_t, int);
typedef int (WINAPI *os_preinit_name_t)(uintptr_t, void *, int *);
typedef int (WINAPI *os_preinit_option_t)(uintptr_t, int, int, const char *, int);
typedef int (WINAPI *os_preinit_ioctl_t)(uintptr_t, long, unsigned long *);
typedef int (WINAPI *os_preinit_connect_t)(uintptr_t, const void *, int);
typedef int (WINAPI *os_preinit_select_t)(int, void *, void *, void *, const void *);
typedef int (WINAPI *os_preinit_error_t)(void);
typedef int32_t (WINAPI *os_preinit_open_key_t)(void *, const wchar_t *, uint32_t, uint32_t, void **); /* LSTATUS */
typedef int32_t (WINAPI *os_preinit_close_key_t)(void *);
typedef int32_t (WINAPI *os_preinit_open_provider_t)(void **, const wchar_t *, const wchar_t *, uint32_t); /* NTSTATUS */
typedef int32_t (WINAPI *os_preinit_random_t)(void *, unsigned char *, uint32_t, uint32_t);
typedef int32_t (WINAPI *os_preinit_close_provider_t)(void *, uint32_t);
/** D3DKMT_ENUMADAPTERS2 of the x64 ABI: adapter count, then the adapter array pointer. */
typedef struct os_preinit_adapters_t { uint32_t count; void *adapters; } os_preinit_adapters_t;
typedef int32_t (WINAPI *os_preinit_enum_adapters_t)(os_preinit_adapters_t *);
/** sockaddr_in, fd_set and timeval layouts of the Windows ABI; winsock2.h is not included. */
typedef struct os_preinit_address_t { uint16_t family, port; uint32_t address; unsigned char zero[8]; } os_preinit_address_t;
typedef struct os_preinit_set_t { unsigned count; uintptr_t sockets[64]; } os_preinit_set_t;
typedef struct os_preinit_wait_t { int32_t seconds, microseconds; } os_preinit_wait_t;

_Static_assert(sizeof(os_preinit_startup_t) == sizeof(FARPROC) && sizeof(os_preinit_socket_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_close_t) == sizeof(FARPROC) && sizeof(os_preinit_cleanup_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_packages_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_bind_t) == sizeof(FARPROC) && sizeof(os_preinit_select_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_address_t) == 16 && sizeof(os_preinit_wait_t) == 8 &&
               sizeof(os_preinit_open_key_t) == sizeof(FARPROC) && sizeof(os_preinit_close_key_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_open_provider_t) == sizeof(FARPROC) && sizeof(os_preinit_random_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_close_provider_t) == sizeof(FARPROC) && sizeof(os_preinit_enum_adapters_t) == sizeof(FARPROC) &&
               sizeof(os_preinit_adapters_t) == 2 * sizeof(void *),
               "export storage must match FARPROC exactly");

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

/** @brief Load and free dxgi.dll once; no DXGI factory is created.
 *
 * Amendment 4 (2026-10-22): with loader patch 0002 no reload cycle creates a DXGI factory, but
 * the loader still loads and frees dxgi.dll. Creating a factory here opened an RPC/ALPC client
 * connection whose later idle cleanup attached RPCRT4 per-thread state (a thread handle and an
 * event) to a thread-pool worker, which broke exact handle equality in cycle 3.
 * @return Zero on success; nonzero when the load fails or after a failed release.
 */
static int os_preinit_dxgi_module(void)
{
    HMODULE module = NULL;
    if (!os_preinit_load(L"dxgi.dll", &module)) return 1;
    return !FreeLibrary(module);
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

/** @brief Open and close one current-user key, as the loader's settings lookup does.
 *
 * Amendment 4: the first current-user access makes the registry API open the user hive
 * (HKU\<SID>) and cache that key for the process lifetime. The predefined HKEY_CURRENT_USER
 * handle itself is never closed. advapi32 is loaded from System32 and freed here.
 * @return Zero on success; nonzero after closing an opened key and freeing the module.
 */
static int os_preinit_user_hive(void)
{
    HMODULE module = NULL;
    os_preinit_open_key_t open_key = NULL;
    os_preinit_close_key_t close_key = NULL;
    void *key = NULL;
    if (!os_preinit_load(L"advapi32.dll", &module)) return 1;
    int failed = !os_preinit_resolve(module, "RegOpenKeyExW", &open_key) ||
                 !os_preinit_resolve(module, "RegCloseKey", &close_key);
    if (!failed) {
        if (open_key(OsPreinitCurrentUser, L"Software", 0, OsPreinitKeyRead, &key) != OsPreinitStatusSuccess) failed = 1;
        else if (close_key(key) != OsPreinitStatusSuccess) failed = 1;
    }
    if (!FreeLibrary(module)) failed = 1;
    return failed;
}

/** @brief Open the bcrypt RNG provider, generate 16 bytes and close it.
 *
 * Amendment 4: these are exactly the calls venus_tcp_random makes. The first provider open
 * loads bcryptprimitives.dll, which Windows keeps loaded with its ETW registrations and an
 * open device file. The bytes are discarded and zeroed.
 * @return Zero on success; nonzero after closing an opened provider and freeing the module.
 */
static int os_preinit_random(void)
{
    HMODULE module = NULL;
    os_preinit_open_provider_t open_provider = NULL;
    os_preinit_random_t generate = NULL;
    os_preinit_close_provider_t close_provider = NULL;
    void *provider = NULL;
    unsigned char bytes[OsPreinitRandomBytes];
    if (!os_preinit_load(L"bcrypt.dll", &module)) return 1;
    int failed = !os_preinit_resolve(module, "BCryptOpenAlgorithmProvider", &open_provider) ||
                 !os_preinit_resolve(module, "BCryptGenRandom", &generate) ||
                 !os_preinit_resolve(module, "BCryptCloseAlgorithmProvider", &close_provider);
    if (!failed) {
        if (open_provider(&provider, L"RNG", L"Microsoft Primitive Provider", 0) != OsPreinitStatusSuccess) {
            failed = 1;
        } else {
            if (generate(provider, bytes, sizeof bytes, 0) != OsPreinitStatusSuccess) failed = 1;
            if (close_provider(provider, 0) != OsPreinitStatusSuccess) failed = 1;
        }
    }
    memset(bytes, 0, sizeof bytes);
    if (!FreeLibrary(module)) failed = 1;
    return failed;
}

/** @brief One D3DKMTEnumAdapters2 count query, as the loader's driver-store layer scan makes.
 *
 * Amendment 4: the first call loads dxcore.dll, which Windows keeps loaded with its ETW
 * registrations. No adapter array is requested, so no adapter handle is opened.
 * @return Zero on success; nonzero after freeing the gdi32 reference.
 */
static int os_preinit_d3dkmt_adapters(void)
{
    HMODULE module = NULL;
    os_preinit_enum_adapters_t enumerate = NULL;
    if (!os_preinit_load(L"gdi32.dll", &module)) return 1;
    int failed = !os_preinit_resolve(module, "D3DKMTEnumAdapters2", &enumerate);
    if (!failed) {
        os_preinit_adapters_t adapters = {0, NULL};
        if (enumerate(&adapters) != OsPreinitStatusSuccess) failed = 1;
    }
    if (!FreeLibrary(module)) failed = 1;
    return failed;
}

/** @brief Run the ten OS-only first-use operations in their fixed order.
 * @return Zero only when all ten steps succeeded; stops at the first failing step.
 * @note Callers must stop the run before measuring any baseline when this fails.
 */
static int os_preinitialize(void)
{
    return os_preinit_user32() || os_preinit_winsock() || os_preinit_bcrypt() ||
           os_preinit_dxgi_module() || os_preinit_search_order() || os_preinit_package_family() ||
           os_preinit_winsock_connect() || os_preinit_user_hive() || os_preinit_random() ||
           os_preinit_d3dkmt_adapters();
}
