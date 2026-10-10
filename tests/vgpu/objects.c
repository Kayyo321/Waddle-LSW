/** @file objects.c @brief Native loader-word ABI and private Vulkan handle lifetime fixture. */
#include "waddle/venus_objects.h"
#include <assert.h>
#include <stdlib.h>
#ifndef _WIN32
#include <vulkan/vk_icd.h>
#endif
/** @brief Fixture external object kind for a Vulkan instance. */
static const uint32_t InstanceKind = 1;
/** @brief Fixture external object kind for a Vulkan buffer. */
static const uint32_t BufferKind = 9;
int main(void) {
    venus_object_t *slots = calloc(8, sizeof(*slots));
    assert(slots);
    venus_objects_t objects = {0};
    unsigned cookie = 1;
    assert(venus_objects_init(&objects, slots, 8, 123, &cookie) == RingOk);
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        venus_object_t *instance = NULL, *buffer = NULL, *found = NULL;
        assert(venus_objects_reserve(&objects, InstanceKind, 0, 1, &instance) == RingOk);
        assert(instance->loader_data == VenusObjectsLoaderMagic);
#ifndef _WIN32
        assert(valid_loader_magic_value(instance));
        set_loader_magic_value(instance);
#endif
        instance->loader_data = 0x1000;
        assert(venus_objects_lookup(&objects, instance->handle, InstanceKind, 1, &found) == RingOk);
        assert(found == instance);
        assert(venus_objects_reserve(&objects, BufferKind, instance->id, 0, &buffer) == RingOk);
        assert(buffer->handle >> 32 == 123);
        assert(venus_objects_lookup_id(&objects, buffer->id, BufferKind, &found) == RingOk);
        assert(found == buffer);
        assert(venus_objects_release(&objects, instance->handle, InstanceKind, 1) == RingAgain);
        uint64_t retired = buffer->handle;
        assert(venus_objects_release(&objects, retired, BufferKind, 0) == RingOk);
        assert(venus_objects_lookup(&objects, retired, BufferKind, 0, &found) == RingInvalid);
        assert(!found);
        assert(venus_objects_release(&objects, instance->handle, InstanceKind, 1) == RingOk);
        assert(!objects.live_count);
    }
    venus_objects_free(&objects);
    venus_objects_free(&objects);
    free(slots);
    slots = NULL;
    return 0;
}
