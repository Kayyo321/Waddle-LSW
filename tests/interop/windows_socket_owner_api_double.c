/** @file windows_socket_owner_api_double.c
 * @brief Linux execution adapter for the production WinSock socket owner.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "windows_socket_owner_api_double.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

static int last_wsa_error;
static int next_get_handle_error;
static int next_handle_inheritable;
static int next_socket_error;
static int next_socket_error_failure;
static int next_linger_on;
static int next_linger_error;
static int next_linger_length = (int)sizeof(struct linger);
static int next_get_peer_error;
static int next_socket_type = SOCK_STREAM;
static int next_socket_type_length = (int)sizeof(int);
static int next_peer_length = (int)sizeof(struct sockaddr);
static int next_ioctl_error;

enum { TestPairCount = 64, TestQueueBytes = 262144 };
typedef struct test_pair_t {
    int socket_values[2];
    uint8_t queues[2][TestQueueBytes];
    size_t starts[2];
    size_t lengths[2];
    int write_closed[2];
} test_pair_t;
static test_pair_t test_pairs[TestPairCount];

void windows_socket_owner_test_fail_get_handle(int error_code)
{
    next_get_handle_error = error_code;
}

void windows_socket_owner_test_set_inheritable(int inheritable)
{
    next_handle_inheritable = inheritable != 0;
}

void windows_socket_owner_test_set_so_error(int value, int fail_error)
{
    next_socket_error = value;
    next_socket_error_failure = fail_error;
}

void windows_socket_owner_test_set_linger_on(int enabled)
{
    next_linger_on = enabled != 0;
}

void windows_socket_owner_test_set_linger_length(int returned_length)
{
    next_linger_length = returned_length;
}

void windows_socket_owner_test_fail_linger(int error_code)
{
    next_linger_error = error_code;
}

void windows_socket_owner_test_fail_get_peer(int error_code)
{
    next_get_peer_error = error_code;
}

void windows_socket_owner_test_set_socket_type(int socket_type, int returned_length)
{
    next_socket_type = socket_type;
    next_socket_type_length = returned_length;
}

void windows_socket_owner_test_set_peer_length(int returned_length)
{
    next_peer_length = returned_length;
}

void windows_socket_owner_test_fail_ioctl(int error_code)
{
    next_ioctl_error = error_code;
}

static test_pair_t *find_pair(int socket_value, int *side_out)
{
    size_t i;
    for (i = 0; i < TestPairCount; ++i) {
        if (test_pairs[i].socket_values[0] == socket_value) {
            *side_out = 0;
            return &test_pairs[i];
        }
        if (test_pairs[i].socket_values[1] == socket_value) {
            *side_out = 1;
            return &test_pairs[i];
        }
    }
    return NULL;
}

void windows_socket_owner_test_register_pair(int first, int second)
{
    size_t i;
    for (i = 0; i < TestPairCount; ++i) {
        if (test_pairs[i].socket_values[0] == first ||
            test_pairs[i].socket_values[1] == first ||
            test_pairs[i].socket_values[0] == second ||
            test_pairs[i].socket_values[1] == second)
            memset(&test_pairs[i], 0, sizeof(test_pairs[i]));
    }
    for (i = 0; i < TestPairCount; ++i) {
        if (test_pairs[i].socket_values[0] == 0 &&
            test_pairs[i].socket_values[1] == 0) {
            memset(&test_pairs[i], 0, sizeof(test_pairs[i]));
            test_pairs[i].socket_values[0] = first;
            test_pairs[i].socket_values[1] = second;
            return;
        }
    }
    abort();
}

void windows_socket_owner_test_mark_closed(int socket_value)
{
    int side = 0;
    test_pair_t *pair = find_pair(socket_value, &side);
    if (pair != NULL) {
        pair->write_closed[side] = 1;
        if (pair->write_closed[0] && pair->write_closed[1])
            memset(pair, 0, sizeof(*pair));
    }
}

ssize_t windows_socket_owner_test_stream_send(int socket_value,
                                              const void *buffer,
                                              size_t length)
{
    int side = 0;
    size_t available;
    size_t amount;
    test_pair_t *pair = find_pair(socket_value, &side);
    if (pair == NULL || pair->write_closed[side]) {
        errno = EBADF;
        return -1;
    }
    available = TestQueueBytes - pair->lengths[1 - side];
    if (available == 0) {
        errno = EAGAIN;
        return -1;
    }
    amount = length < available ? length : available;
    size_t end = (pair->starts[1 - side] + pair->lengths[1 - side]) % TestQueueBytes;
    size_t first_part = amount < TestQueueBytes - end ? amount : TestQueueBytes - end;
    memcpy(pair->queues[1 - side] + end, buffer, first_part);
    if (first_part < amount)
        memcpy(pair->queues[1 - side], (const uint8_t *)buffer + first_part,
               amount - first_part);
    pair->lengths[1 - side] += amount;
    return (ssize_t)amount;
}

ssize_t windows_socket_owner_test_stream_recv(int socket_value, void *buffer,
                                              size_t length)
{
    int side = 0;
    size_t amount;
    size_t first_part;
    test_pair_t *pair = find_pair(socket_value, &side);
    if (pair == NULL) {
        errno = EBADF;
        return -1;
    }
    if (pair->lengths[side] == 0) {
        if (pair->write_closed[1 - side])
            return 0;
        errno = EAGAIN;
        return -1;
    }
    amount = length < pair->lengths[side] ? length : pair->lengths[side];
    first_part = amount < TestQueueBytes - pair->starts[side] ?
                 amount : TestQueueBytes - pair->starts[side];
    memcpy(buffer, pair->queues[side] + pair->starts[side], first_part);
    if (first_part < amount)
        memcpy((uint8_t *)buffer + first_part, pair->queues[side], amount - first_part);
    pair->starts[side] = (pair->starts[side] + amount) % TestQueueBytes;
    pair->lengths[side] -= amount;
    return (ssize_t)amount;
}

int windows_socket_owner_test_stream_shutdown(int socket_value, int how)
{
    int side = 0;
    test_pair_t *pair = find_pair(socket_value, &side);
    if (pair == NULL) {
        errno = EBADF;
        return -1;
    }
    if (how == SHUT_WR || how == SHUT_RDWR)
        pair->write_closed[side] = 1;
    return 0;
}

short windows_socket_owner_test_poll_revents(int socket_value, short events)
{
    int side = 0;
    test_pair_t *pair = find_pair(socket_value, &side);
    short result = 0;
    if (pair == NULL)
        return POLLNVAL;
    if ((events & POLLIN) != 0 &&
        (pair->lengths[side] != 0 || pair->write_closed[1 - side]))
        result |= POLLIN;
    if ((events & POLLOUT) != 0 && !pair->write_closed[side] &&
        pair->lengths[1 - side] < TestQueueBytes)
        result |= POLLOUT;
    return result;
}

DWORD GetCurrentThreadId(void)
{
    return (DWORD)(uintptr_t)pthread_self();
}

int GetHandleInformation(HANDLE handle, DWORD *flags)
{
    struct stat status;
    if (flags == NULL) {
        last_wsa_error = WSAEFAULT;
        return 0;
    }
    if (next_get_handle_error != 0) {
        last_wsa_error = next_get_handle_error;
        next_get_handle_error = 0;
        return 0;
    }
    if (fstat((int)handle, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        last_wsa_error = WSAENOTSOCK;
        return 0;
    }
    *flags = next_handle_inheritable ? HANDLE_FLAG_INHERIT : 0;
    next_handle_inheritable = 0;
    return 1;
}

int interop_test_ioctlsocket(SOCKET socket_value, long command, u_long *argument)
{
    int flags;
    if (next_ioctl_error != 0) {
        last_wsa_error = next_ioctl_error;
        next_ioctl_error = 0;
        return SOCKET_ERROR;
    }
    if (command != FIONBIO || argument == NULL) {
        last_wsa_error = WSAEINVAL;
        return SOCKET_ERROR;
    }
    flags = fcntl(socket_value, F_GETFL);
    if (flags < 0 || fcntl(socket_value, F_SETFL,
                           *argument != 0 ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) != 0) {
        last_wsa_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    return 0;
}

int interop_test_closesocket(SOCKET socket_value)
{
    int result = close(socket_value);
    if (result != 0)
        last_wsa_error = errno == EINTR ? WSAEINTR :
                         (errno == EAGAIN || errno == EWOULDBLOCK) ? WSAEWOULDBLOCK : WSAENOTSOCK;
    return result;
}

int interop_test_getsockopt(SOCKET socket_value, int level, int option_name,
                            char *option_value, int *option_length)
{
    struct stat status;
    if (option_length == NULL) {
        last_wsa_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (fstat(socket_value, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        last_wsa_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    if (level != SOL_SOCKET) {
        last_wsa_error = WSAEINVAL;
        return SOCKET_ERROR;
    }
    if (option_name == SO_TYPE && *option_length >= (int)sizeof(int)) {
        *(int *)(void *)option_value = next_socket_type;
        *option_length = next_socket_type_length;
        next_socket_type = SOCK_STREAM;
        next_socket_type_length = (int)sizeof(int);
        return 0;
    }
    if (option_name == SO_LINGER && *option_length >= (int)sizeof(struct linger)) {
        if (next_linger_error != 0) {
            last_wsa_error = next_linger_error;
            next_linger_error = 0;
            return SOCKET_ERROR;
        }
        struct linger value = {next_linger_on, 1};
        next_linger_on = 0;
        memcpy(option_value, &value, sizeof(value));
        *option_length = next_linger_length;
        next_linger_length = (int)sizeof(value);
        return 0;
    }
    if (option_name == SO_ERROR && *option_length >= (int)sizeof(int)) {
        if (next_socket_error_failure != 0) {
            last_wsa_error = next_socket_error_failure;
            next_socket_error_failure = 0;
            return SOCKET_ERROR;
        }
        *(int *)(void *)option_value = next_socket_error;
        next_socket_error = 0;
        *option_length = (int)sizeof(int);
        return 0;
    }
    last_wsa_error = WSAEINVAL;
    return SOCKET_ERROR;
}

int interop_test_getpeername(SOCKET socket_value, struct sockaddr *address,
                             int *address_length)
{
    struct stat status;
    if (address_length == NULL) {
        last_wsa_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (next_get_peer_error != 0) {
        last_wsa_error = next_get_peer_error;
        next_get_peer_error = 0;
        return SOCKET_ERROR;
    }
    if (fstat(socket_value, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        last_wsa_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    if (*address_length < (int)sizeof(struct sockaddr)) {
        last_wsa_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    memset(address, 0, (size_t)*address_length);
    address->sa_family = AF_UNIX;
    *address_length = next_peer_length;
    next_peer_length = (int)sizeof(struct sockaddr);
    return 0;
}

int interop_test_recv(SOCKET socket_value, char *buffer, int length, int flags)
{
    (void)flags;
    int result = (int)recv(socket_value, buffer, (size_t)length, flags);
    if (result == SOCKET_ERROR)
        last_wsa_error = errno == EINTR ? WSAEINTR :
                         (errno == EAGAIN || errno == EWOULDBLOCK) ? WSAEWOULDBLOCK : WSAENOTSOCK;
    return result;
}

int interop_test_send(SOCKET socket_value, const char *buffer, int length, int flags)
{
    int result = (int)send(socket_value, buffer, (size_t)length, flags);
    if (result == SOCKET_ERROR) {
        last_wsa_error = errno == EINTR ? WSAEINTR :
                         (errno == EAGAIN || errno == EWOULDBLOCK) ? WSAEWOULDBLOCK : WSAENOTSOCK;
    }
    return result;
}

int WSAGetLastError(void)
{
    return last_wsa_error;
}
