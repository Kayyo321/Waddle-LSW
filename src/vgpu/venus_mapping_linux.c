#include "venus_bounds.h"
#include "waddle/venus_mapping.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void venus_mapping_free(venus_mapping_t *memory) {
    if (!memory)
        return;
    int saved_errno = errno;
    venus_region_detach(&memory->view);
    if (memory->owns_mapping)
        munmap(memory->mapping, memory->mapping_bytes);
    if (memory->owns_handle)
        close((int)memory->native_handle);
    memset(memory, 0, sizeof(*memory));
    errno = saved_errno;
}

int venus_mapping_create(venus_mapping_t *memory, size_t mapping_bytes, uint32_t capacity) {
    if (!memory) {
        errno = EINVAL;
        return -1;
    }
    memset(memory, 0, sizeof(*memory));
    if (!venus_bounds_region_size(mapping_bytes, capacity)) {
        errno = EINVAL;
        return -1;
    }
    int descriptor = memfd_create("waddle_venus", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0)
        return -1;
    memory->native_handle = descriptor;
    memory->owns_handle = 1;
    if (ftruncate(descriptor, (off_t)mapping_bytes) != 0)
        goto fail;
    if (fcntl(descriptor, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
        goto fail;
    void *mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED)
        goto fail;
    memory->mapping = mapping;
    memory->mapping_bytes = mapping_bytes;
    memory->owns_mapping = 1;
    if (venus_region_init(mapping, mapping_bytes, capacity) != RingOk ||
        venus_region_attach(&memory->view, mapping, mapping_bytes) != RingOk) {
        errno = EPROTO;
        goto fail;
    }
    return 0;
fail:
    venus_mapping_free(memory);
    return -1;
}
