#ifndef WaddleWindowsSocketOwnerApiDoubleH
#define WaddleWindowsSocketOwnerApiDoubleH
/** @file windows_socket_owner_api_double.h
 * @brief Narrow Linux-only WinSock surface used by owner unit fixtures.
 */
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

typedef int SOCKET;
typedef uint32_t DWORD;
typedef intptr_t HANDLE;
typedef unsigned long u_long;
typedef unsigned int socklen_t_windows_t;
typedef struct linger LINGER;

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define HANDLE_FLAG_INHERIT 1u
#define FIONBIO 1L
#undef POLLRDNORM
#undef POLLWRNORM
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT
#define WSAEWOULDBLOCK 10035
#define WSAEINTR 10004
#define WSAENOTSOCK 10038
#define WSAECONNRESET 10054
#define WSAENETDOWN 10050
#define WSAEINPROGRESS 10036
#define WSAEINVAL 10022
#define WSAENOBUFS 10055
#define WSAEFAULT 10014

DWORD GetCurrentThreadId(void);
int GetHandleInformation(HANDLE handle, DWORD *flags);
int interop_test_ioctlsocket(SOCKET socket_value, long command, u_long *argument);
int interop_test_closesocket(SOCKET socket_value);
int interop_test_getsockopt(SOCKET socket_value, int level, int option_name,
                            char *option_value, int *option_length);
int interop_test_getpeername(SOCKET socket_value, struct sockaddr *address,
                             int *address_length);
int interop_test_recv(SOCKET socket_value, char *buffer, int length, int flags);
int interop_test_send(SOCKET socket_value, const char *buffer, int length, int flags);
int WSAGetLastError(void);
void windows_socket_owner_test_fail_get_handle(int error_code);
void windows_socket_owner_test_set_inheritable(int inheritable);
void windows_socket_owner_test_set_so_error(int value, int fail_error);
void windows_socket_owner_test_set_linger_on(int enabled);
void windows_socket_owner_test_set_linger_length(int returned_length);
void windows_socket_owner_test_fail_linger(int error_code);
void windows_socket_owner_test_fail_get_peer(int error_code);
void windows_socket_owner_test_set_socket_type(int socket_type, int returned_length);
void windows_socket_owner_test_set_peer_length(int returned_length);
void windows_socket_owner_test_fail_ioctl(int error_code);
void windows_socket_owner_test_register_pair(int first, int second);
void windows_socket_owner_test_mark_closed(int socket_value);
ssize_t windows_socket_owner_test_stream_send(int socket_value, const void *buffer,
                                              size_t length);
ssize_t windows_socket_owner_test_stream_recv(int socket_value, void *buffer,
                                              size_t length);
int windows_socket_owner_test_stream_shutdown(int socket_value, int how);
short windows_socket_owner_test_poll_revents(int socket_value, short events);

#if defined(InteropWindowsSocketOwnerImplementation)
#define ioctlsocket interop_test_ioctlsocket
#define closesocket interop_test_closesocket
#define getsockopt interop_test_getsockopt
#define getpeername interop_test_getpeername
#define recv interop_test_recv
#define send interop_test_send
#endif
#endif
