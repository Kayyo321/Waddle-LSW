/** @file guest_listener.c @brief Standalone Windows VSOCK/AF_UNIX execution listener. */
#include "guest_session.h"
#include <stdio.h>
#include <string.h>

/** @brief Independently declared Viosock device address ABI, native byte order.
 * @note Value-only naturally aligned 12-byte structure, no ownership/thread state. */
typedef struct guest_vsock_address_t {
    uint16_t family; /**< Winsock family returned by installed Viosock driver. */
    uint16_t reserved; /**< Must be zero. */
    uint32_t port; /**< Native-endian port. */
    uint32_t cid; /**< UINT32_MAX binds the local guest CID. */
} guest_vsock_address_t;
/** @brief System AF_UNIX address ABI; borrowed during bind, no shared state. */
typedef struct guest_unix_address_t {
    uint16_t family; /**< AF_UNIX (one). */
    char path[108]; /**< NUL-terminated local socket path, no abstract namespace. */
} guest_unix_address_t;
_Static_assert(sizeof(guest_vsock_address_t) == 12, "Viosock ABI size");
_Static_assert(sizeof(guest_unix_address_t) == 110, "AF_UNIX ABI size");

static int vsock_family(void) {
    HANDLE device = CreateFileW(L"\\\\.\\Viosock", GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) { return -1; }
    DWORD family = 0, returned = 0;
    BOOL valid = DeviceIoControl(device, 0x0801300c, NULL, 0, &family,
                                sizeof(family), &returned, NULL);
    CloseHandle(device);
    if (!valid || returned != sizeof(family) || family == 0 || family > UINT16_MAX) { return -1; }
    return (int)family;
}

/** @brief Standalone guest entry point, owns Winsock and listener lifetime.
 * @param[in] argc CRT argument count. @param[in] argv Nonnull borrowed CRT argv,
 * NUL-terminated strings retained for the entire process lifetime.
 * @return 0 help, 2 invalid options, 1 native startup/listener failure.
 * @note Main thread dispatches sessions sequentially; workers join before reuse. */
int main(int argc, char **argv) {
    const char *path = NULL;
    uint32_t port = 5242;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            puts("waddle-guest-exec [--vsock-port N | --socket-path PATH]");
            return 0;
        }
        if (i + 1 == argc) { goto usage; }
        if (strcmp(argv[i], "--socket-path") == 0 && path == NULL) {
            path = argv[++i];
            if (*path == 0 || strlen(path) >= 108) { goto usage; }
        } else if (strcmp(argv[i], "--vsock-port") == 0) {
            if (guest_port_parse(argv[++i], &port) != 0) { goto usage; }
        } else { goto usage; }
    }
    WSADATA startup;
    if (WSAStartup(MAKEWORD(2, 2), &startup) != 0) { return 1; }
    int result = 1, bound = 0;
    int family = path == NULL ? vsock_family() : AF_UNIX;
    SOCKET listener = INVALID_SOCKET;
    if (family < 0) { fprintf(stderr, "guest: cannot query installed Viosock driver\n"); goto done; }
    listener = WSASocketW(family, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (listener == INVALID_SOCKET) { goto socket_error; }
    int status;
    if (path != NULL) {
        guest_unix_address_t address = {0};
        address.family = AF_UNIX;
        memcpy(address.path, path, strlen(path) + 1);
        status = bind(listener, (const struct sockaddr *)&address, sizeof(address));
    } else {
        guest_vsock_address_t address = {(uint16_t)family, 0, port, UINT32_MAX};
        status = bind(listener, (const struct sockaddr *)&address, sizeof(address));
    }
    if (status != 0) { goto socket_error; }
    bound = 1;
    if (listen(listener, 4) != 0) { goto socket_error; }
    puts("guest: ready");
    fflush(stdout);
    for (;;) {
        SOCKET peer = accept(listener, NULL, NULL);
        if (peer == INVALID_SOCKET) { goto socket_error; }
        DWORD timeout = 30000;
        if (!SetHandleInformation((HANDLE)(uintptr_t)peer, HANDLE_FLAG_INHERIT, 0) ||
            setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) != 0) {
            closesocket(peer);
            continue;
        }
        if (guest_session(peer) != 0) { fprintf(stderr, "guest: session aborted\n"); }
        shutdown(peer, SD_BOTH);
        closesocket(peer);
    }
socket_error:
    fprintf(stderr, "guest: Winsock error %d\n", WSAGetLastError());
done:
    if (listener != INVALID_SOCKET) { closesocket(listener); }
    if (bound && path != NULL) { DeleteFileA(path); }
    WSACleanup();
    return result;
usage:
    fprintf(stderr, "usage: waddle-guest-exec [--vsock-port N | --socket-path PATH]\n");
    return 2;
}
