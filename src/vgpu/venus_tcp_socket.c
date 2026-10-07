/** @file venus_tcp_socket.c @brief Bounded deadline-aware native TCP socket ownership. */
#include "waddle/venus_tcp.h"
#include <limits.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
typedef SOCKET native_socket_t;
#define NativeInvalid INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int native_socket_t;
#define NativeInvalid (-1)
#endif
static native_socket_t native_handle(const venus_tcp_socket_t *owner)
{
    return (native_socket_t)owner->handle;
}
uint64_t venus_tcp_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
#endif
}
void venus_tcp_scrub(void *bytes, size_t length)
{
    volatile uint8_t *cursor=bytes;
    if (!cursor) return;
    while (length--) *cursor++=0;
}
static venus_ring_status_t health(uint64_t deadline, const _Atomic uint32_t *cancel)
{
    if (cancel && atomic_load_explicit(cancel, memory_order_acquire)) return RingCancelled;
    uint64_t now=venus_tcp_now_ms();
    if (!now) return RingClosed;
    return now>=deadline ? RingTimeout : RingOk;
}
static int would_block(void)
{
#ifdef _WIN32
    int error=WSAGetLastError();
    return error==WSAEWOULDBLOCK || error==WSAEINTR || error==WSAEINPROGRESS;
#else
    return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR || errno==EINPROGRESS;
#endif
}
/* Configure the sole unpublished native owner. Header/payload exchanges must
 * not wait for Nagle/delayed ACK before the peer can decode a complete request.
 * Caller closes the handle and balances runtime ownership on every failure. */
static int configure_socket(native_socket_t handle)
{
    int no_delay=1;
    if(setsockopt(handle,IPPROTO_TCP,TCP_NODELAY,(const char *)&no_delay,sizeof no_delay))return 0;
#ifdef _WIN32
    u_long enabled=1;
    return ioctlsocket(handle, FIONBIO, &enabled)==0;
#else
    int flags=fcntl(handle, F_GETFL, 0);
    if (flags<0 || fcntl(handle, F_SETFL, flags|O_NONBLOCK)) return 0;
    flags=fcntl(handle, F_GETFD, 0);
    return flags>=0 && fcntl(handle, F_SETFD, flags|FD_CLOEXEC)==0;
#endif
}
static void close_native(native_socket_t handle)
{
#ifdef _WIN32
    closesocket(handle);
#else
    close(handle);
#endif
}
static int runtime_acquire(void)
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2,2), &data)==0;
#else
    return 1;
#endif
}
static void runtime_release(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}
void venus_tcp_socket_close(venus_tcp_socket_t *owner)
{
    if (!owner || !owner->initialized) return;
    native_socket_t handle=native_handle(owner);
    memset(owner, 0, sizeof *owner);
    close_native(handle);
    runtime_release();
}
static venus_ring_status_t fresh(venus_tcp_socket_t *owner)
{
    if (!owner || owner->initialized) return RingInvalid;
    if (!runtime_acquire()) return RingClosed;
    native_socket_t handle=socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle==NativeInvalid) { runtime_release(); return RingClosed; }
    if (!configure_socket(handle)) { close_native(handle); runtime_release(); return RingClosed; }
    owner->handle=(uintptr_t)handle;
    owner->initialized=1;
    return RingOk;
}
static venus_ring_status_t wait_ready(native_socket_t handle, int writing,
    uint64_t deadline, const _Atomic uint32_t *cancel)
{
    for (;;) {
        venus_ring_status_t status=health(deadline, cancel);
        if (status!=RingOk) return status;
        uint64_t now=venus_tcp_now_ms();
        if (now>=deadline) return RingTimeout;
        uint64_t remaining=deadline-now;
        int timeout=(int)(remaining>10 ? 10 : remaining);
#ifdef _WIN32
        fd_set ready, errors;
        FD_ZERO(&ready); FD_ZERO(&errors);
        FD_SET(handle,&ready); FD_SET(handle,&errors);
        struct timeval interval={.tv_sec=0,.tv_usec=timeout*1000};
        int result=select(0, writing ? NULL : &ready, writing ? &ready : NULL, &errors, &interval);
#else
        struct pollfd descriptor={.fd=handle,.events=(short)(writing ? POLLOUT : POLLIN)};
        int result=poll(&descriptor, 1, timeout);
#endif
        if (result>0) return RingOk; /* send/recv/SO_ERROR determines precise hangup status. */
        if (result<0 && !would_block()) return RingClosed;
    }
}
venus_ring_status_t venus_tcp_socket_connect(venus_tcp_socket_t *owner, const char *host,
    uint32_t port, uint64_t deadline, const _Atomic uint32_t *cancel)
{
    if (!owner || owner->initialized || !host || !port || port>65535 ||
        (strcmp(host,"127.0.0.1") && strcmp(host,"10.0.2.2"))) return RingInvalid;
    venus_ring_status_t status=health(deadline, cancel);
    if (status!=RingOk) return status;
    venus_tcp_socket_t staged={0};
    status=fresh(&staged);
    if (status!=RingOk) return status;
    struct sockaddr_in address={0};
    address.sin_family=AF_INET;
    address.sin_port=htons((uint16_t)port);
    address.sin_addr.s_addr=htonl(!strcmp(host,"127.0.0.1") ? UINT32_C(0x7f000001) : UINT32_C(0x0a000202));
    if (connect(native_handle(&staged), (const struct sockaddr *)&address, sizeof address)) {
        if (!would_block()) { status=RingClosed; goto fail; }
        status=wait_ready(native_handle(&staged), 1, deadline, cancel);
        if (status!=RingOk) goto fail;
        int error=0;
#ifdef _WIN32
        int extent=sizeof error;
#else
        socklen_t extent=sizeof error;
#endif
        if (getsockopt(native_handle(&staged), SOL_SOCKET, SO_ERROR, (char *)&error, &extent) || error) {
            status=RingClosed; goto fail;
        }
    }
    *owner=staged;
    return RingOk;
fail:
    venus_tcp_socket_close(&staged);
    return status;
}
venus_ring_status_t venus_tcp_socket_listen(venus_tcp_socket_t *owner, uint32_t port, uint32_t *bound_port)
{
    if (bound_port) *bound_port=0;
    if (!owner || owner->initialized || !bound_port || port>65535) return RingInvalid;
    venus_tcp_socket_t staged={0};
    venus_ring_status_t status=fresh(&staged);
    if (status!=RingOk) return status;
    struct sockaddr_in address={0};
    address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    address.sin_port=htons((uint16_t)port);
    if (bind(native_handle(&staged),(const struct sockaddr *)&address,sizeof address) || listen(native_handle(&staged),1)) goto fail;
#ifdef _WIN32
    int extent=sizeof address;
#else
    socklen_t extent=sizeof address;
#endif
    if (getsockname(native_handle(&staged),(struct sockaddr *)&address,&extent)) goto fail;
    *bound_port=ntohs(address.sin_port);
    *owner=staged;
    return RingOk;
fail:
    venus_tcp_socket_close(&staged);
    return RingClosed;
}
venus_ring_status_t venus_tcp_socket_accept(const venus_tcp_socket_t *listener,
    venus_tcp_socket_t *owner, uint64_t deadline, const _Atomic uint32_t *cancel)
{
    if (!listener || !listener->initialized || !owner || owner==listener || owner->initialized) return RingInvalid;
    for (;;) {
        venus_ring_status_t status=health(deadline,cancel);
        if (status!=RingOk) return status;
        native_socket_t handle=accept(native_handle(listener),NULL,NULL);
        if (handle!=NativeInvalid) {
            if (!runtime_acquire()) { close_native(handle); return RingClosed; }
            if (!configure_socket(handle)) { close_native(handle); runtime_release(); return RingClosed; }
            owner->handle=(uintptr_t)handle; owner->initialized=1;
            return RingOk;
        }
        if (!would_block()) return RingClosed;
        status=wait_ready(native_handle(listener),0,deadline,cancel);
        if (status!=RingOk) return status;
    }
}
venus_ring_status_t venus_tcp_socket_send(venus_tcp_socket_t *owner, const void *bytes,
    size_t length, uint64_t deadline, const _Atomic uint32_t *cancel)
{
    if (!owner || !owner->initialized || (!bytes && length) || length>VenusTcpMaxCommandBytes) return RingInvalid;
    const uint8_t *cursor=bytes;
    size_t sent=0;
    while (sent<length) {
        venus_ring_status_t status=health(deadline,cancel);
        if (status!=RingOk) return status;
#ifdef _WIN32
        int count=send(native_handle(owner),(const char *)cursor+sent,(int)(length-sent),0);
#else
        ssize_t count=send(native_handle(owner),cursor+sent,length-sent,MSG_NOSIGNAL);
#endif
        if (count>0) { sent+=(size_t)count; continue; }
        if (!count || !would_block()) return RingClosed;
        status=wait_ready(native_handle(owner),1,deadline,cancel);
        if (status!=RingOk) return status;
    }
    return RingOk;
}
venus_ring_status_t venus_tcp_socket_receive(venus_tcp_socket_t *owner, void *bytes,
    size_t length, size_t *received, uint64_t deadline, const _Atomic uint32_t *cancel)
{
    if (received) *received=0;
    if (owner) owner->received_eof=0;
    if (!owner || !owner->initialized || (!bytes && length) || !received || length>VenusTcpMaxCommandBytes) return RingInvalid;
    uint8_t *cursor=bytes;
    while (*received<length) {
        venus_ring_status_t status=health(deadline,cancel);
        if (status!=RingOk) return status;
#ifdef _WIN32
        int count=recv(native_handle(owner),(char *)cursor+*received,(int)(length-*received),0);
#else
        ssize_t count=recv(native_handle(owner),cursor+*received,length-*received,0);
#endif
        if (count>0) { *received+=(size_t)count; continue; }
        if (!count) { owner->received_eof=1; return RingClosed; }
        if (!would_block()) return RingClosed;
        status=wait_ready(native_handle(owner),0,deadline,cancel);
        if (status!=RingOk) return status;
    }
    return RingOk;
}
venus_ring_status_t venus_tcp_socket_shutdown_write(venus_tcp_socket_t *owner)
{
    if (!owner || !owner->initialized) return RingInvalid;
#ifdef _WIN32
    return shutdown(native_handle(owner),SD_SEND) ? RingClosed : RingOk;
#else
    return shutdown(native_handle(owner),SHUT_WR) ? RingClosed : RingOk;
#endif
}
venus_ring_status_t venus_tcp_random(void *bytes, size_t length)
{
    if (!bytes || !length || length>32) return RingInvalid;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm=NULL;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_RNG_ALGORITHM,MS_PRIMITIVE_PROVIDER,0)!=0) return RingClosed;
    NTSTATUS generated=BCryptGenRandom(algorithm,bytes,(ULONG)length,0);
    NTSTATUS closed=BCryptCloseAlgorithmProvider(algorithm,0);
    return generated==0 && closed==0 ? RingOk : RingClosed;
#else
    ssize_t count=getrandom(bytes,length,GRND_NONBLOCK);
    return count==(ssize_t)length ? RingOk : RingClosed;
#endif
}
