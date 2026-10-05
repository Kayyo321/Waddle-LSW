#include "waddle/venus_mapping.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int failure_stage;
static int last_descriptor = -1;

int __real_memfd_create(const char *name, unsigned flags);
int __real_ftruncate(int descriptor, off_t length);
int __real_fcntl(int descriptor, int command, ...);
void *__real_mmap(void *address, size_t length, int protection, int flags, int descriptor,
                  off_t offset);
venus_ring_status_t __real_venus_region_init(void *mapping, size_t length, uint32_t capacity);
venus_ring_status_t __real_venus_region_attach(venus_region_view_t *view, void *mapping,
                                               size_t length);

int __wrap_memfd_create(const char *name, unsigned flags) {
    if (failure_stage == 1) {
        errno = EIO;
        return -1;
    }
    last_descriptor = __real_memfd_create(name, flags);
    return last_descriptor;
}
int __wrap_ftruncate(int descriptor, off_t length) {
    if (failure_stage == 2) {
        errno = EIO;
        return -1;
    }
    return __real_ftruncate(descriptor, length);
}
int __wrap_fcntl(int descriptor, int command, ...) {
    if (command == F_ADD_SEALS) {
        if (failure_stage == 3) {
            errno = EIO;
            return -1;
        }
        va_list arguments;
        va_start(arguments, command);
        int seals = va_arg(arguments, int);
        va_end(arguments);
        return __real_fcntl(descriptor, command, seals);
    }
    return __real_fcntl(descriptor, command);
}
void *__wrap_mmap(void *address, size_t length, int protection, int flags, int descriptor,
                  off_t offset) {
    if (failure_stage == 4) {
        errno = EIO;
        return MAP_FAILED;
    }
    return __real_mmap(address, length, protection, flags, descriptor, offset);
}
venus_ring_status_t __wrap_venus_region_init(void *mapping, size_t length, uint32_t capacity) {
    if (failure_stage == 5)
        return RingInvalid;
    return __real_venus_region_init(mapping, length, capacity);
}
venus_ring_status_t __wrap_venus_region_attach(venus_region_view_t *view, void *mapping,
                                               size_t length) {
    if (failure_stage == 6)
        return RingInvalid;
    return __real_venus_region_attach(view, mapping, length);
}

int main(void) {
    venus_mapping_t memory = {0};
    venus_mapping_free(NULL);
    venus_mapping_free(&memory); /* Must not close stdin from a zero record. */
    assert(venus_mapping_create(NULL, 4096, 64) == -1 && errno == EINVAL);
    assert(venus_mapping_create(&memory, 4095, 64) == -1 && errno == EINVAL);
    assert(memory.owns_handle == 0 && memory.mapping == NULL);
    for (failure_stage = 1; failure_stage <= 6; ++failure_stage) {
        last_descriptor = -1;
        assert(venus_mapping_create(&memory, 4096, 1024) == -1);
        assert(errno == (failure_stage < 5 ? EIO : EPROTO));
        assert(memory.mapping == NULL && memory.owns_handle == 0 && memory.owns_mapping == 0);
        assert(memory.view.commands.header == NULL && memory.view.replies.header == NULL);
        if (last_descriptor >= 0)
            assert(fcntl(last_descriptor, F_GETFD) == -1 && errno == EBADF);
    }
    failure_stage = 0;
    assert(venus_mapping_create(&memory, 4096, 1024) == 0);
    int descriptor = (int)memory.native_handle;
    assert(fcntl(descriptor, F_GETFD) & FD_CLOEXEC);
    assert((fcntl(descriptor, F_GET_SEALS) & (F_SEAL_GROW | F_SEAL_SHRINK)) ==
           (F_SEAL_GROW | F_SEAL_SHRINK));
    assert(ftruncate(descriptor, 8192) == -1 && errno == EPERM);
    assert(ftruncate(descriptor, 2048) == -1 && errno == EPERM);
    void *peer_mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    assert(peer_mapping != MAP_FAILED);
    venus_region_view_t peer;
    assert(venus_region_attach(&peer, peer_mapping, 4096) == RingOk);
    uint8_t byte = 0x42, received = 0;
    assert(venus_ring_write(&peer.commands, &byte, 1) == RingOk);
    assert(venus_ring_read(&memory.view.commands, &received, 1) == RingOk && received == byte);
    ++byte;
    assert(venus_ring_write(&memory.view.replies, &byte, 1) == RingOk);
    assert(venus_ring_read(&peer.replies, &received, 1) == RingOk && received == byte);
    venus_region_detach(&peer);
    assert(munmap(peer_mapping, 4096) == 0);
    peer_mapping = NULL;
    errno = EAGAIN;
    venus_mapping_free(&memory);
    assert(errno == EAGAIN && memory.mapping == NULL && memory.mapping_bytes == 0);
    assert(memory.owns_handle == 0 && memory.owns_mapping == 0);
    assert(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
    venus_mapping_free(&memory);
    return 0;
}
