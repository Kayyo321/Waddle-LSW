/** @file icon_windows_ownership.c
 * @brief Linux allocator-owner lifecycle checks for icon image deinitialization.
 *
 * These checks exercise the platform-independent image owner only. They do
 * not exercise or claim native Windows Shell, COM, or GDI behavior.
 */
#include "../../src/interop/icon_windows.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation_record_t {
    void *expected_context;
    void *allocated_pointer;
    size_t allocated_size;
    size_t allocation_calls;
    size_t deallocation_calls;
    void *deallocated_pointer;
    size_t deallocated_size;
    void *deallocated_context;
} allocation_record_t;

static void *tracked_allocate(void *context, size_t size)
{
    allocation_record_t *record = context;
    void *memory = malloc(size);
    if (memory != NULL) {
        record->allocation_calls++;
        record->allocated_pointer = memory;
        record->allocated_size = size;
    }
    return memory;
}

static void tracked_deallocate(void *context, void *memory, size_t size)
{
    allocation_record_t *record = context;
    record->deallocation_calls++;
    record->deallocated_context = context;
    record->deallocated_pointer = memory;
    record->deallocated_size = size;
    free(memory);
}

static bool image_is_zero(const icon_windows_image_t *image)
{
    return image->rgba == NULL && image->stride == 0u && image->width == 0u &&
           image->height == 0u && image->allocation_size == 0u &&
           image->allocator.context == NULL && image->allocator.allocate == NULL &&
           image->allocator.deallocate == NULL;
}

static void test_owned_image_deinitializes_once(void)
{
    allocation_record_t record = {0};
    record.expected_context = &record;
    const icon_windows_allocator_t allocator = {
        .context = &record,
        .allocate = tracked_allocate,
        .deallocate = tracked_deallocate
    };
    const size_t allocation_size = 4u * 4u * 4u;
    icon_windows_image_t image = {0};
    image.rgba = allocator.allocate(allocator.context, allocation_size);
    assert(image.rgba != NULL);
    memset(image.rgba, 0xA5, allocation_size);
    image.stride = 4u * 4u;
    image.width = 4u;
    image.height = 4u;
    image.allocation_size = allocation_size;
    image.allocator = allocator;
    assert(record.allocation_calls == 1u);
    assert(record.allocated_pointer == image.rgba);
    assert(record.allocated_size == allocation_size);

    icon_windows_image_deinit(&image);
    assert(image_is_zero(&image));
    assert(record.deallocation_calls == 1u);
    assert(record.deallocated_context == record.expected_context);
    assert(record.deallocated_pointer == record.allocated_pointer);
    assert(record.deallocated_size == record.allocated_size);

    icon_windows_image_deinit(&image);
    icon_windows_image_deinit(NULL);
    assert(image_is_zero(&image));
    assert(record.deallocation_calls == 1u);
}

static void test_incomplete_owner_is_preserved(void)
{
    allocation_record_t record = {0};
    record.expected_context = &record;
    const size_t allocation_size = 32u;
    uint8_t *memory = tracked_allocate(&record, allocation_size);
    assert(memory != NULL);
    memset(memory, 0x5A, allocation_size);

    icon_windows_image_t image = {0};
    image.rgba = memory;
    image.stride = 8u;
    image.width = 2u;
    image.height = 2u;
    image.allocation_size = allocation_size;
    image.allocator.context = &record;
    image.allocator.allocate = tracked_allocate;
    /* Missing deallocate means the image owner cannot safely be discharged. */
    const icon_windows_image_t before = image;

    icon_windows_image_deinit(&image);
    assert(memcmp(&image, &before, sizeof(image)) == 0);
    assert(record.deallocation_calls == 0u);

    /* The test still owns the allocation and releases it explicitly. */
    tracked_deallocate(&record, memory, allocation_size);
    memset(&image, 0, sizeof(image));
    assert(image_is_zero(&image));
    assert(record.deallocation_calls == 1u);
    assert(record.deallocated_context == record.expected_context);
    assert(record.deallocated_pointer == record.allocated_pointer);
    assert(record.deallocated_size == record.allocated_size);
}

#ifndef _WIN32
static void unused_deallocate(void *context, void *memory, size_t size)
{
    (void)context;
    (void)memory;
    (void)size;
}

static void test_non_windows_guards(void)
{
    allocation_record_t record = {0};
    const icon_windows_allocator_t allocator = {
        .context = &record,
        .allocate = tracked_allocate,
        .deallocate = unused_deallocate
    };
    icon_windows_image_t image = {0};
    icon_windows_cleanup_t cleanup = {0};
    IShellItem *item = (IShellItem *)(uintptr_t)1u;
    assert(icon_windows_extract_rgba(item, NULL, 16u, allocator, &image, &cleanup) ==
           IconWindowsStatusUnsupportedPlatform);
    assert(image_is_zero(&image));
    assert(cleanup.bitmap_count == 0u && cleanup.icon_count == 0u &&
           cleanup.device_context_count == 0u);
    assert(icon_windows_extract_rgba(item, NULL, 17u, allocator, &image, &cleanup) ==
           IconWindowsStatusUnsupportedDimension);
    assert(image_is_zero(&image));
    assert(icon_windows_extract_rgba(NULL, NULL, 16u, allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(image_is_zero(&image));
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusUnsupportedPlatform);
}
#endif

int main(void)
{
    test_owned_image_deinitializes_once();
    test_incomplete_owner_is_preserved();
#ifndef _WIN32
    test_non_windows_guards();
#endif
    return 0;
}
