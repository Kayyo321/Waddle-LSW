/** @file mapping_linux.c @brief Linux mapping-owner public lifecycle tests. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/interop/interop_mapping_linux.h"
#include "../../src/interop/interop_mapping_init.h"
#include "../../src/interop/interop_native_atomic.h"
#include "../../src/av/av_kvmfr.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void test_mapping_linux_private(void);
void test_mapping_linux_open_busy_states(int borrowed_fd);
void test_mapping_linux_duplicate_cloexec(const interop_mapping_linux_t *owner);
void test_mapping_linux_mapping_output_alias(interop_mapping_linux_t *owner);
void test_mapping_linux_mapping_view_alias(interop_mapping_linux_t *owner);

static int fail_fcntl;
static int fail_fcntl_eintr;
static int fail_getfl;
static int fail_fstat;
static int mutate_fstat;
static int fail_ioctl;
static long fake_ioctl_bytes = (long)UINT64_C(0x80000000);
static int fail_mmap;
static int fail_munmap;
static int fail_close;
static int fail_close_eintr;
static int reuse_closed_descriptor;
static int reused_unrelated_fd = -1;
static int fail_calloc;
static int fake_ioctl_capacity;
static int fake_mmap_for_device;
static int fail_layout_validation;
static int fail_atomic_validation;
static char temp_path[64];
static uint8_t initial_metadata[0x4000];

int __real_fcntl(int fd, int command, ...);
int __real_fstat(int fd, struct stat *status);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);
int __real_munmap(void *address, size_t length);
int __real_close(int fd);
void *__real_calloc(size_t count, size_t bytes);
interop_layout_status_t __real_interop_layout_validate_shm_header(
    const uint8_t *header_bytes, size_t header_length, uintptr_t mapping_address,
    uint64_t mapping_length, uint64_t expected_epoch,
    interop_shm_header_snapshot_t *snapshot);
interop_atomic_status_t __real_interop_atomic_validate_mapping(
    uintptr_t mapping_base, uint64_t mapping_length,
    const interop_shm_header_t *header, const interop_slot_descriptor_t *descriptor);

int __wrap_fcntl(int fd, int command, ...) {
    va_list arguments;
    int result;
    if (command == F_GETFL && fail_getfl) {
        fail_getfl = 0;
        errno = EIO;
        return -1;
    }
    if (command == F_GETFD || command == F_GETFL)
        return __real_fcntl(fd, command);
    va_start(arguments, command);
    int argument = va_arg(arguments, int);
    va_end(arguments);
    if (fail_fcntl_eintr) { --fail_fcntl_eintr; errno = EINTR; return -1; }
    if (fail_fcntl) { fail_fcntl = 0; errno = EMFILE; return -1; }
    result = __real_fcntl(fd, command, argument);
    return result;
}

int __wrap_fstat(int fd, struct stat *status) {
    if (fail_fstat) { fail_fstat = 0; errno = EIO; return -1; }
    int result = __real_fstat(fd, status);
    if (result == 0 && mutate_fstat) {
        switch (mutate_fstat) {
        case 1: status->st_mode = (status->st_mode & ~S_IFMT) | S_IFDIR; break;
        case 2: status->st_size = 1; break;
        case 3: status->st_uid = status->st_uid == 0 ? 1 : 0; break;
        case 4: status->st_mode = (status->st_mode & ~07777) | 0640; break;
        case 5: status->st_nlink = 2; break;
        case 6: status->st_mode = (status->st_mode & ~S_IFMT) | S_IFCHR; break;
        case 7: status->st_size = -1; break;
        }
        mutate_fstat = 0;
    }
    return result;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list arguments;
    void *output;
    va_start(arguments, request);
    output = va_arg(arguments, void *);
    va_end(arguments);
    if (fail_ioctl) { fail_ioctl = 0; errno = EIO; return -1; }
    if (fake_ioctl_capacity && request == AvKvmfrGetSize64) {
        *(uint64_t *)output = (uint64_t)fake_ioctl_bytes;
        return 0;
    }
    return __real_ioctl(fd, request, output);
}

void *__wrap_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset) {
    if (fail_mmap) { fail_mmap = 0; errno = ENOMEM; return MAP_FAILED; }
    if (fake_mmap_for_device) {
        fake_mmap_for_device = 0;
        return __real_mmap(NULL, length, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    return __real_mmap(address, length, protection, flags, fd, offset);
}

int __wrap_munmap(void *address, size_t length) {
    if (fail_munmap) { fail_munmap = 0; errno = EIO; return -1; }
    return __real_munmap(address, length);
}

int __wrap_close(int fd) {
    if (fail_close_eintr) {
        fail_close_eintr = 0;
        (void)__real_close(fd);
        errno = EINTR;
        return -1;
    }
    if (fail_close) {
        fail_close = 0;
        (void)__real_close(fd);
        if (reuse_closed_descriptor) {
            reuse_closed_descriptor = 0;
            reused_unrelated_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            assert(reused_unrelated_fd == fd);
        }
        errno = EIO;
        return -1;
    }
    return __real_close(fd);
}

void *__wrap_calloc(size_t count, size_t bytes) {
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return __real_calloc(count, bytes);
}

interop_layout_status_t __wrap_interop_layout_validate_shm_header(
    const uint8_t *header_bytes, size_t header_length, uintptr_t mapping_address,
    uint64_t mapping_length, uint64_t expected_epoch,
    interop_shm_header_snapshot_t *snapshot) {
    if (fail_layout_validation) {
        fail_layout_validation = 0;
        return InteropLayoutStatusInvalidLayout;
    }
    return __real_interop_layout_validate_shm_header(header_bytes, header_length,
        mapping_address, mapping_length, expected_epoch, snapshot);
}

interop_atomic_status_t __wrap_interop_atomic_validate_mapping(
    uintptr_t mapping_base, uint64_t mapping_length,
    const interop_shm_header_t *header, const interop_slot_descriptor_t *descriptor) {
    if (fail_atomic_validation) {
        fail_atomic_validation = 0;
        return InteropAtomicStatusNotLockFree;
    }
    return __real_interop_atomic_validate_mapping(mapping_base, mapping_length,
                                                   header, descriptor);
}

static int make_initialized_sparse_file(void) {
    char path[] = "/tmp/waddle-mapping-linux-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(temp_path[0] == '\0');
    assert(strlen(path) < sizeof(temp_path));
    strcpy(temp_path, path);
    assert(fchmod(fd, 0600) == 0);
    assert(ftruncate(fd, (off_t)UINT64_C(0x80000000)) == 0);
    void *mapping = __real_mmap(NULL, (size_t)UINT64_C(0x80000000),
                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(mapping != MAP_FAILED);
    assert(interop_mapping_initialize((uintptr_t)mapping, UINT64_C(0x80000000),
                                      0x1234, 1) == InteropMappingInitStatusOk);
    memcpy(initial_metadata, mapping, sizeof(initial_metadata));
    assert(__real_munmap(mapping, (size_t)UINT64_C(0x80000000)) == 0);
    return fd;
}

static void remove_sparse_file(void) {
    assert(temp_path[0] != '\0');
    assert(unlink(temp_path) == 0);
    temp_path[0] = '\0';
}

static void assert_metadata_unchanged(int fd) {
    void *mapping = __real_mmap(NULL, sizeof(initial_metadata), PROT_READ,
                                MAP_SHARED, fd, 0);
    assert(mapping != MAP_FAILED);
    assert(memcmp(mapping, initial_metadata, sizeof(initial_metadata)) == 0);
    assert(__real_munmap(mapping, sizeof(initial_metadata)) == 0);
}

static interop_mapping_linux_t *make_owner(void) {
    interop_mapping_linux_t *owner = NULL;
    assert(interop_mapping_linux_create(&owner) == InteropMappingLinuxOk);
    assert(owner != NULL);
    return owner;
}

static void test_lifecycle_and_metadata(void) {
    int source_fd = make_initialized_sparse_file();
    int source_flags = fcntl(source_fd, F_GETFD);
    interop_mapping_linux_t *owner = make_owner();
    interop_shm_header_snapshot_t snapshot;
    interop_mapping_linux_view_t view;
    memset(&snapshot, 0xA5, sizeof(snapshot));
    interop_shm_header_snapshot_t prior_snapshot = snapshot;
    fail_fcntl_eintr = 1;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOk);
    test_mapping_linux_duplicate_cloexec(owner);
    test_mapping_linux_mapping_output_alias(owner);
    assert(interop_mapping_linux_view(owner, &view) == InteropMappingLinuxBusy);
    assert(interop_mapping_linux_validate_header(owner, 0, &snapshot) ==
           InteropMappingLinuxInvalidArgument);
    assert(memcmp(&snapshot, &prior_snapshot, sizeof(snapshot)) == 0);
    assert(interop_mapping_linux_validate_header(owner, 0x4321, &snapshot) ==
           InteropMappingLinuxInvalidHeader);
    assert(memcmp(&snapshot, &prior_snapshot, sizeof(snapshot)) == 0);
    fail_layout_validation = 1;
    assert(interop_mapping_linux_validate_header(owner, 0x1234, &snapshot) ==
           InteropMappingLinuxInvalidHeader);
    assert(memcmp(&snapshot, &prior_snapshot, sizeof(snapshot)) == 0);
    fail_atomic_validation = 1;
    assert(interop_mapping_linux_validate_header(owner, 0x1234, &snapshot) ==
           InteropMappingLinuxNotLockFree);
    assert(memcmp(&snapshot, &prior_snapshot, sizeof(snapshot)) == 0);
    assert(interop_mapping_linux_validate_header(owner, 0x1234, &snapshot) ==
           InteropMappingLinuxOk);
    assert(snapshot.session_epoch == 0x1234);
    test_mapping_linux_mapping_view_alias(owner);
    assert(interop_mapping_linux_view(owner, &view) == InteropMappingLinuxOk);
    assert(view.mapping_base != 0 && view.mapping_length == UINT64_C(0x80000000));
    assert(view.reserved[0] == 0 && view.reserved[1] == 0);
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxMissingQuiescence);
    assert(interop_mapping_linux_close(owner, 1) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    assert(owner == NULL);
    assert(fcntl(source_fd, F_GETFD) == source_flags);
    assert_metadata_unchanged(source_fd);
    assert(close(source_fd) == 0);
    remove_sparse_file();
}

static void test_open_failures(void) {
    int source_fd = make_initialized_sparse_file();
    interop_mapping_linux_t *owner;
    struct stat original;
    assert(fstat(source_fd, &original) == 0);

    owner = make_owner();
    fail_fcntl = 1;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOsError);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);

    owner = make_owner();
    fail_getfl = 1;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOsError);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);

    owner = make_owner();
    fail_fstat = 1;
    fail_close = 1;
    errno = 0;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxCloseDiagnostic);
    assert(errno == EIO);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);

    owner = make_owner();
    fail_mmap = 1;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOsError);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);

    owner = make_owner();
    fail_close = 1;
    fail_mmap = 1;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxCloseDiagnostic);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);

    owner = make_owner();
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingKvmfrCharacter) == InteropMappingLinuxBackingRejected);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    struct stat after;
    assert(fstat(source_fd, &after) == 0 && after.st_ino == original.st_ino &&
           after.st_size == original.st_size && after.st_mode == original.st_mode);
    assert_metadata_unchanged(source_fd);
    assert(close(source_fd) == 0);
    remove_sparse_file();
}

static void test_invalid_arguments_and_file_facts(void) {
    int source_fd = make_initialized_sparse_file();
    interop_mapping_linux_t *owner = NULL;
    assert(interop_mapping_linux_create(NULL) == InteropMappingLinuxInvalidArgument);
    uint8_t misaligned_owner_output[sizeof(interop_mapping_linux_t *) +
                                    _Alignof(interop_mapping_linux_t *) + 1] = {0};
    uintptr_t misaligned_address = (uintptr_t)misaligned_owner_output;
    misaligned_address = (misaligned_address + _Alignof(interop_mapping_linux_t *) - 1) &
                         ~((uintptr_t)_Alignof(interop_mapping_linux_t *) - 1);
    ++misaligned_address;
    assert(interop_mapping_linux_create((interop_mapping_linux_t **)
        (void *)misaligned_address) == InteropMappingLinuxInvalidArgument);
    owner = (interop_mapping_linux_t *)(uintptr_t)1;
    assert(interop_mapping_linux_create(&owner) == InteropMappingLinuxInvalidArgument);
    owner = NULL;
    assert(interop_mapping_linux_create(&owner) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_open(NULL, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_open(owner, -1,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_open(owner, source_fd,
        (interop_mapping_linux_backing_t)99) == InteropMappingLinuxInvalidArgument);

    for (int fact = 1; fact <= 7; ++fact) {
        mutate_fstat = fact;
        assert(interop_mapping_linux_open(owner, source_fd,
            InteropMappingLinuxBackingRegularPrivateFile) ==
            InteropMappingLinuxBackingRejected);
        assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxOk);
    }
    int readonly_fd = open(temp_path, O_RDONLY | O_CLOEXEC);
    assert(readonly_fd >= 0);
    assert(interop_mapping_linux_open(owner, readonly_fd,
        InteropMappingLinuxBackingRegularPrivateFile) ==
        InteropMappingLinuxBackingRejected);
    assert(close(readonly_fd) == 0);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    assert(close(source_fd) == 0);
    remove_sparse_file();
}

static void test_close_retry_and_expired_view(void) {
    int source_fd = make_initialized_sparse_file();
    interop_mapping_linux_t *owner = make_owner();
    interop_shm_header_snapshot_t snapshot;
    interop_mapping_linux_view_t view;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_validate_header(owner, 0x1234, &snapshot) ==
           InteropMappingLinuxOk);
    assert(interop_mapping_linux_view(owner, &view) == InteropMappingLinuxOk);
    fail_munmap = 1;
    assert(interop_mapping_linux_close(owner, 1) == InteropMappingLinuxCleanupPending);
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxBusy);
    assert(interop_mapping_linux_validate_header(owner, 0x1234, &snapshot) ==
           InteropMappingLinuxBusy);
    assert(interop_mapping_linux_view(owner, &view) == InteropMappingLinuxBusy);
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    assert_metadata_unchanged(source_fd);
    assert(close(source_fd) == 0);
    remove_sparse_file();

    source_fd = make_initialized_sparse_file();
    owner = make_owner();
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOk);
    fail_close_eintr = 1;
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxCloseDiagnostic);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    owner = make_owner();
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOk);
    fail_close = 1;
    reuse_closed_descriptor = 1;
    errno = 0;
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxCloseDiagnostic);
    assert(errno == EIO);
    assert(reused_unrelated_fd >= 0 && fcntl(reused_unrelated_fd, F_GETFD) >= 0);
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxOk);
    assert(fcntl(reused_unrelated_fd, F_GETFD) >= 0);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    assert(close(reused_unrelated_fd) == 0);
    reused_unrelated_fd = -1;
    assert(close(source_fd) == 0);
    remove_sparse_file();
}

static void test_kvmfr_kind_uses_capacity_ioctl(void) {
    int source_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    interop_mapping_linux_t *owner = make_owner();
    assert(source_fd >= 0);
    fake_ioctl_capacity = 1;
    fake_mmap_for_device = 1;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingKvmfrCharacter) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    assert(close(source_fd) == 0);
}

static void test_kvmfr_ioctl_failures(void) {
    int source_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    interop_mapping_linux_t *owner;
    assert(source_fd >= 0);
    owner = make_owner();
    fail_ioctl = 1;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingKvmfrCharacter) == InteropMappingLinuxOsError);
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);

    owner = make_owner();
    fake_ioctl_capacity = 1;
    fake_ioctl_bytes = 4096;
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingKvmfrCharacter) == InteropMappingLinuxBackingRejected);
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    fake_ioctl_bytes = (long)UINT64_C(0x80000000);
    assert(close(source_fd) == 0);
}

static void test_validate_and_view_output_guards(void) {
    int source_fd = make_initialized_sparse_file();
    interop_mapping_linux_t *owner = make_owner();
    interop_mapping_linux_view_t view;
    assert(interop_mapping_linux_validate_header(NULL, 1, NULL) ==
           InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_close(NULL, 0) ==
           InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_destroy(NULL) ==
           InteropMappingLinuxInvalidArgument);
    interop_mapping_linux_t *null_owner = NULL;
    assert(interop_mapping_linux_destroy(&null_owner) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_view(owner, NULL) ==
           InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxBusy);
    assert(interop_mapping_linux_validate_header(owner, 0x1234, NULL) ==
           InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_validate_header(owner, 0x1234,
        (interop_shm_header_snapshot_t *)(void *)owner) ==
        InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);

    owner = make_owner();
    assert(interop_mapping_linux_open(owner, source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_validate_header(owner, 0x1234,
        (interop_shm_header_snapshot_t *)(uintptr_t)1) ==
        InteropMappingLinuxInvalidArgument);
    interop_shm_header_snapshot_t snapshot;
    assert(interop_mapping_linux_validate_header(owner, 0x1234, &snapshot) ==
           InteropMappingLinuxOk);
    assert(interop_mapping_linux_validate_header(owner, 0x1234, &snapshot) ==
           InteropMappingLinuxBusy);
    assert(interop_mapping_linux_view(owner,
        (interop_mapping_linux_view_t *)(uintptr_t)1) ==
        InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_view(owner,
        (interop_mapping_linux_view_t *)(void *)owner) ==
        InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_view(owner, &view) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_view(owner, &view) == InteropMappingLinuxBusy);
    assert(interop_mapping_linux_close(owner, 2) == InteropMappingLinuxInvalidArgument);
    assert(interop_mapping_linux_close(owner, 1) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    assert(close(source_fd) == 0);
    remove_sparse_file();
}

static void *wrong_thread_call(void *argument) {
    struct wrong_thread_context_t { interop_mapping_linux_t *owner; int source_fd; };
    struct wrong_thread_context_t *context = argument;
    interop_mapping_linux_t *owner = context->owner;
    interop_shm_header_snapshot_t snapshot;
    interop_mapping_linux_view_t view;
    assert(interop_mapping_linux_close(owner, 0) == InteropMappingLinuxWrongThread);
    assert(interop_mapping_linux_open(owner, context->source_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxWrongThread);
    assert(interop_mapping_linux_validate_header(owner, 1, &snapshot) ==
           InteropMappingLinuxWrongThread);
    assert(interop_mapping_linux_view(owner, &view) == InteropMappingLinuxWrongThread);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxWrongThread);
    return NULL;
}

static void test_wrong_thread_and_owner_allocation_failure(void) {
    interop_mapping_linux_t *owner = NULL;
    pthread_t thread;
    int source_fd = make_initialized_sparse_file();
    struct wrong_thread_context_t { interop_mapping_linux_t *owner; int source_fd; };
    struct wrong_thread_context_t context;
    fail_calloc = 1;
    assert(interop_mapping_linux_create(&owner) == InteropMappingLinuxOutOfMemory);
    assert(owner == NULL);
    owner = make_owner();
    context.owner = owner;
    context.source_fd = source_fd;
    assert(pthread_create(&thread, NULL, wrong_thread_call, &context) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(interop_mapping_linux_destroy(&owner) == InteropMappingLinuxOk);
    assert(close(source_fd) == 0);
    remove_sparse_file();
}

int main(void) {
    test_mapping_linux_private();
    int busy_probe_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(busy_probe_fd >= 0);
    test_mapping_linux_open_busy_states(busy_probe_fd);
    assert(close(busy_probe_fd) == 0);
    test_lifecycle_and_metadata();
    test_open_failures();
    test_invalid_arguments_and_file_facts();
    test_close_retry_and_expired_view();
    test_kvmfr_kind_uses_capacity_ioctl();
    test_kvmfr_ioctl_failures();
    test_validate_and_view_output_guards();
    test_wrong_thread_and_owner_allocation_failure();
    return 0;
}
