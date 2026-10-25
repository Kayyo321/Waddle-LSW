/** @file private.c @brief Exercise Linux mapping-owner private range guards. */
#include "../../src/interop/interop_mapping_linux.c"

#include <assert.h>

static void test_pointer_ranges(void) {
    _Alignas(uint64_t) uint8_t bytes[32] = {0};
    pointer_range_t range;
    interop_mapping_linux_t fake_owner = {0};
    int missing_fd = -1;
    assert(!pointer_range(NULL, 1, &range));
    assert(!pointer_range(bytes, 0, &range));
    assert(!pointer_range(bytes, sizeof(bytes), NULL));
    assert(!pointer_range((void *)(UINTPTR_MAX - 3), 4, &range));
    assert(pointer_range((void *)(UINTPTR_MAX - 3), 3, &range));
    assert(range.begin == UINTPTR_MAX - 3 && range.end == UINTPTR_MAX);
    assert(pointer_range(bytes, sizeof(bytes), &range));
    assert(!ranges_overlap((pointer_range_t){1, 2}, (pointer_range_t){2, 3}));
    assert(!ranges_overlap((pointer_range_t){2, 3}, (pointer_range_t){1, 2}));
    assert(ranges_overlap((pointer_range_t){1, 4}, (pointer_range_t){3, 5}));
    fake_owner.mapping_base = bytes;
    fake_owner.mapping_length = sizeof(bytes);
    fake_owner.mapped = true;
    assert(!output_is_disjoint(&fake_owner, bytes + 8, sizeof(uint64_t)));
    assert(!output_is_disjoint(&fake_owner, &fake_owner, sizeof(fake_owner)));
    assert(output_is_disjoint(&fake_owner, &range, sizeof(range)));
    assert(!output_is_disjoint(NULL, &range, sizeof(range)));
    assert(!output_is_disjoint(&fake_owner, NULL, sizeof(range)));
    fake_owner.mapped = false;
    assert(output_is_disjoint(&fake_owner, &range, sizeof(range)));
    fake_owner.mapped = true;
    fake_owner.mapping_base = (void *)(UINTPTR_MAX - 3);
    fake_owner.mapping_length = 8;
    assert(output_is_disjoint(&fake_owner, &range, sizeof(range)));
    assert(!output_is_disjoint((const interop_mapping_linux_t *)(UINTPTR_MAX - 3),
                               &range, sizeof(range)));
    assert(on_owner_thread(NULL) == false);
    assert(release_fd_once(&missing_fd) == 0);
}

void test_mapping_linux_open_busy_states(int borrowed_fd) {
    interop_mapping_linux_t fake_owner = {0};
    fake_owner.owner_thread = pthread_self();
    fake_owner.owned_fd = -1;
    fake_owner.mapped = true;
    assert(interop_mapping_linux_open(&fake_owner, borrowed_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxBusy);
    fake_owner.mapped = false;
    fake_owner.owned_fd = 0;
    assert(interop_mapping_linux_open(&fake_owner, borrowed_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxBusy);
    fake_owner.owned_fd = -1;
    fake_owner.view_published = true;
    assert(interop_mapping_linux_open(&fake_owner, borrowed_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxBusy);
    fake_owner.view_published = false;
    fake_owner.closing = true;
    assert(interop_mapping_linux_open(&fake_owner, borrowed_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxBusy);
}

static void test_backing_enum_rejects_unknown(void) {
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(validate_backing(fd, (interop_mapping_linux_backing_t)99) ==
           InteropMappingLinuxInvalidArgument);
    assert(close(fd) == 0);
}

void test_mapping_linux_private(void) {
    test_pointer_ranges();
    test_backing_enum_rejects_unknown();
}

void test_mapping_linux_duplicate_cloexec(const interop_mapping_linux_t *owner) {
    assert(owner != NULL && owner->owned_fd >= 0);
    assert((fcntl(owner->owned_fd, F_GETFD) & FD_CLOEXEC) != 0);
}

void test_mapping_linux_mapping_output_alias(interop_mapping_linux_t *owner) {
    assert(owner != NULL && owner->mapped);
    assert(interop_mapping_linux_validate_header(owner, 0x1234,
        (interop_shm_header_snapshot_t *)owner->mapping_base) ==
        InteropMappingLinuxInvalidArgument);
}

void test_mapping_linux_mapping_view_alias(interop_mapping_linux_t *owner) {
    assert(owner != NULL && owner->mapped && owner->header_validated);
    assert(interop_mapping_linux_view(owner,
        (interop_mapping_linux_view_t *)owner->mapping_base) ==
        InteropMappingLinuxInvalidArgument);
}
