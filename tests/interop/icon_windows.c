/** @file icon_windows.c @brief Native Shell extraction and GDI ownership fixtures. */
#ifdef _WIN32
#define IconWindowsTestSeams
#include "../../src/interop/icon_windows.c"
#include "../../src/interop/png_writer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef InteropNativeCoverageProfile
#include <gcov.h>

static void profile_assert_failure(const char *condition, const char *file, int line)
{
    (void)fprintf(stderr, "native fixture assertion failed: %s (%s:%d)\n",
                  condition, file, line);
    __gcov_dump();
    exit(1);
}

#undef assert
#define assert(condition) \
    ((condition) ? (void)0 : profile_assert_failure(#condition, __FILE__, __LINE__))
#endif

typedef struct allocator_state_t {
    bool fail_allocate;
    size_t allocations;
    size_t deallocations;
    size_t bytes_allocated;
    size_t bytes_deallocated;
} allocator_state_t;

static void *test_allocate(void *context, size_t size)
{
    allocator_state_t *state = context;
    if (state->fail_allocate) {
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        ++state->allocations;
        state->bytes_allocated += size;
    }
    return memory;
}

static void test_deallocate(void *context, void *memory, size_t size)
{
    allocator_state_t *state = context;
    ++state->deallocations;
    state->bytes_deallocated += size;
    free(memory);
}

static HBITMAP create_color_bitmap(uint32_t dimension, void **bits_out)
{
    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = (LONG)dimension;
    info.bmiHeader.biHeight = -(LONG)dimension;
    info.bmiHeader.biPlanes = 1u;
    info.bmiHeader.biBitCount = 32u;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &info, DIB_RGB_COLORS, bits_out, NULL, 0u);
}

static void test_owned_gdi_fixtures(void)
{
    const uint32_t Dimension = 32u;
    void *source_bits = NULL;
    HBITMAP source_bitmap = create_color_bitmap(Dimension, &source_bits);
    assert(source_bitmap != NULL && source_bits != NULL);
    uint8_t *source_bytes = source_bits;
    for (size_t offset = 0u; offset < (size_t)Dimension * Dimension * 4u; offset += 4u) {
        source_bytes[offset] = 16u;
        source_bytes[offset + 1u] = 32u;
        source_bytes[offset + 2u] = 64u;
        source_bytes[offset + 3u] = 128u;
    }

    HDC device_context = CreateCompatibleDC(NULL);
    assert(device_context != NULL);
    void *destination_bits = NULL;
    HBITMAP destination_bitmap = create_color_bitmap(Dimension, &destination_bits);
    assert(destination_bitmap != NULL && destination_bits != NULL);
    BITMAPINFO destination_info;
    memset(&destination_info, 0, sizeof(destination_info));
    destination_info.bmiHeader.biSize = sizeof(destination_info.bmiHeader);
    destination_info.bmiHeader.biWidth = (LONG)Dimension;
    destination_info.bmiHeader.biHeight = -(LONG)Dimension;
    destination_info.bmiHeader.biPlanes = 1u;
    destination_info.bmiHeader.biBitCount = 32u;
    destination_info.bmiHeader.biCompression = BI_RGB;
    DWORD error = ERROR_SUCCESS;
    assert(copy_bitmap_to_top_down_dib(device_context, source_bitmap, Dimension, Dimension,
                                       destination_bits, &destination_info, &error));
    assert(memcmp(source_bits, destination_bits, (size_t)Dimension * Dimension * 4u) == 0);

    uint8_t pixel[4] = {0};
    const uint8_t PremultipliedPixel[4] = {16u, 32u, 64u, 128u};
    convert_premultiplied_bgra_to_rgba(pixel, PremultipliedPixel);
    assert(pixel[0] == 128u && pixel[1] == 64u && pixel[2] == 32u && pixel[3] == 128u);
    const uint8_t TransparentPixel[4] = {255u, 10u, 99u, 0u};
    convert_premultiplied_bgra_to_rgba(pixel, TransparentPixel);
    assert(pixel[0] == 0u && pixel[1] == 0u && pixel[2] == 0u && pixel[3] == 0u);
    const uint8_t OpaquePixel[4] = {16u, 32u, 64u, 255u};
    convert_premultiplied_bgra_to_rgba(pixel, OpaquePixel);
    assert(pixel[0] == 64u && pixel[1] == 32u && pixel[2] == 16u && pixel[3] == 255u);
    const uint8_t OverPremultipliedPixel[4] = {255u, 128u, 64u, 1u};
    convert_premultiplied_bgra_to_rgba(pixel, OverPremultipliedPixel);
    assert(pixel[0] == 255u && pixel[1] == 255u && pixel[2] == 255u && pixel[3] == 1u);

    HBITMAP mask_bitmap = CreateBitmap((int)Dimension, (int)Dimension, 1u, 1u, NULL);
    assert(mask_bitmap != NULL);
    ICONINFO icon_info;
    memset(&icon_info, 0, sizeof(icon_info));
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = source_bitmap;
    icon_info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);
    assert(icon != NULL);
    memset(destination_bits, 0, (size_t)Dimension * Dimension * 4u);
    assert(draw_icon_to_top_down_dib(device_context, destination_bitmap, icon,
                                     Dimension, &error));
    assert(DestroyIcon(icon) != 0);
    assert(DeleteObject(mask_bitmap) != 0);

    WORD monochrome_mask_bits[64u * 2u] = {0};
    for (size_t row = 0u; row < Dimension; ++row) {
        /* CreateBitmap stores bottom-up rows; AND is the visual top half. */
        monochrome_mask_bits[(row + Dimension) * 2u] = 0x8000u;
    }
    HBITMAP monochrome_mask = CreateBitmap((int)Dimension, (int)(Dimension * 2u),
                                            1u, 1u, monochrome_mask_bits);
    assert(monochrome_mask != NULL);
    ICONINFO monochrome_info;
    memset(&monochrome_info, 0, sizeof(monochrome_info));
    monochrome_info.fIcon = TRUE;
    monochrome_info.hbmMask = monochrome_mask;
    HICON monochrome_icon = CreateIconIndirect(&monochrome_info);
    assert(monochrome_icon != NULL);
    memset(destination_bits, 0, (size_t)Dimension * Dimension * 4u);
    assert(draw_icon_to_top_down_dib(device_context, destination_bitmap, monochrome_icon,
                                     Dimension, &error));
    HBITMAP returned_color = NULL;
    HBITMAP returned_mask = NULL;
    assert(apply_legacy_icon_mask(device_context, monochrome_icon, Dimension,
                                  destination_bits, &returned_color, &returned_mask,
                                  &error));
    assert(returned_color == NULL && returned_mask != NULL);
    const uint8_t *masked_pixels = destination_bits;
    (void)fprintf(stderr, "monochrome mask alpha sample: %u %u %u %u; neighbors %u %u %u %u\n",
                  masked_pixels[0], masked_pixels[1], masked_pixels[2], masked_pixels[3],
                  masked_pixels[4], masked_pixels[5], masked_pixels[6], masked_pixels[7]);
    assert(masked_pixels[3] == 0u && masked_pixels[7] == 255u);
    assert(DeleteObject(returned_mask) != 0);
    assert(DestroyIcon(monochrome_icon) != 0);
    assert(DeleteObject(monochrome_mask) != 0);

    monochrome_mask_bits[64u] = 0x8000u;
    HBITMAP inversion_mask = CreateBitmap((int)Dimension, (int)(Dimension * 2u),
                                           1u, 1u, monochrome_mask_bits);
    assert(inversion_mask != NULL);
    monochrome_info.hbmMask = inversion_mask;
    HICON inversion_icon = CreateIconIndirect(&monochrome_info);
    assert(inversion_icon != NULL);
    returned_color = NULL;
    returned_mask = NULL;
    assert(!apply_legacy_icon_mask(device_context, inversion_icon, Dimension,
                                   destination_bits, &returned_color, &returned_mask,
                                   &error));
    assert(error == ERROR_NOT_SUPPORTED && returned_color == NULL && returned_mask != NULL);
    assert(DeleteObject(returned_mask) != 0);
    assert(DestroyIcon(inversion_icon) != 0);
    assert(DeleteObject(inversion_mask) != 0);

    assert(DeleteDC(device_context) != 0);
    assert(DeleteObject(destination_bitmap) != 0);
    assert(DeleteObject(source_bitmap) != 0);
}

static IShellItem *create_explorer_shell_item(wchar_t *path, size_t path_capacity)
{
    const UINT Length = GetWindowsDirectoryW(path, (UINT)path_capacity);
    assert(Length > 0u && (size_t)Length + 14u < path_capacity);
    path[Length] = L'\\';
    path[Length + 1u] = L'e';
    path[Length + 2u] = L'x';
    path[Length + 3u] = L'p';
    path[Length + 4u] = L'l';
    path[Length + 5u] = L'o';
    path[Length + 6u] = L'r';
    path[Length + 7u] = L'e';
    path[Length + 8u] = L'r';
    path[Length + 9u] = L'.';
    path[Length + 10u] = L'e';
    path[Length + 11u] = L'x';
    path[Length + 12u] = L'e';
    path[Length + 13u] = L'\0';
    IShellItem *item = NULL;
    assert(SUCCEEDED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem,
                                                (void **)&item)));
    return item;
}

static void test_shell_extraction(void)
{
    const HRESULT ApartmentResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    assert(SUCCEEDED(ApartmentResult));
    wchar_t path[32768];
    IShellItem *item = create_explorer_shell_item(path, sizeof(path) / sizeof(path[0]));
    allocator_state_t allocation_state = {0};
    icon_windows_allocator_t allocator = {
        .context = &allocation_state,
        .allocate = test_allocate,
        .deallocate = test_deallocate
    };
    const uint32_t Dimensions[IconWindowsSupportedDimensionCount] = {
        16u, 24u, 32u, 48u, 64u, 128u, 256u
    };

    icon_windows_image_t image = {0};
    icon_windows_cleanup_t cleanup = {0};
    assert(icon_windows_extract_rgba(item, path, 17u, allocator, &image, &cleanup) ==
           IconWindowsStatusUnsupportedDimension);
    assert(image.rgba == NULL && cleanup.bitmap_count == 0u);
    uintptr_t bad_image_address = UINTPTR_MAX - sizeof(icon_windows_image_t) + 1u;
    bad_image_address -= bad_image_address % _Alignof(icon_windows_image_t);
    icon_windows_image_t *bad_image = (icon_windows_image_t *)bad_image_address;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, bad_image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(cleanup.bitmap_count == 0u && image.rgba == NULL);

    union {
        max_align_t alignment;
        unsigned char bytes[sizeof(icon_windows_image_t) +
                           (IconWindowsMaxFallbackPathUnits + 1u) * sizeof(wchar_t)];
    } overlap_arena;
    memset(&overlap_arena, 0, sizeof(overlap_arena));
    wchar_t *overlap_path = (wchar_t *)(void *)overlap_arena.bytes;
    icon_windows_image_t *overlap_image =
        (icon_windows_image_t *)(void *)(overlap_arena.bytes + 16u);
    for (size_t index = 0u; index < 8u; ++index) {
        overlap_path[index] = L'x';
    }
    overlap_path[8] = L'\0';
    assert(icon_windows_extract_rgba(item, overlap_path, 32u, allocator,
                                     overlap_image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(overlap_image->rgba == NULL && overlap_image->width == 0u &&
           cleanup.bitmap_count == 0u);
    assert(icon_windows_extract_rgba(item, (const wchar_t *)(const void *)&image, 32u,
                                     allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    const wchar_t *near_path_end = (const wchar_t *)(uintptr_t)(UINTPTR_MAX - 3u);
    assert(icon_windows_extract_rgba(item, near_path_end, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    wchar_t oversized_path[IconWindowsMaxFallbackPathUnits + 1u];
    for (size_t index = 0u; index < IconWindowsMaxFallbackPathUnits + 1u; ++index) {
        oversized_path[index] = L'x';
    }
    assert(icon_windows_extract_rgba(item, oversized_path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    allocation_state.fail_allocate = true;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusOutOfMemory);
    assert(image.rgba == NULL && cleanup.bitmap_count == 0u && cleanup.icon_count == 0u &&
           cleanup.device_context_count == 0u);
    allocation_state.fail_allocate = false;

    for (size_t index = 0u; index < IconWindowsSupportedDimensionCount; ++index) {
        assert(icon_windows_extract_rgba(item, path, Dimensions[index], allocator,
                                         &image, &cleanup) == IconWindowsStatusOk);
        assert(image.rgba != NULL && image.width == Dimensions[index] &&
               image.height == Dimensions[index] && image.stride == Dimensions[index] * 4u);
        size_t png_size = 0u;
        assert(png_writer_encoded_size(Dimensions[index], &png_size) == PngWriterStatusOk);
        uint8_t *png = malloc(png_size);
        assert(png != NULL);
        size_t png_written = 0u;
        assert(png_writer_encode_rgba8(image.rgba, image.stride, Dimensions[index], png,
                                       png_size, &png_written) == PngWriterStatusOk);
        static const uint8_t Signature[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
        assert(png_written == png_size && memcmp(png, Signature, sizeof(Signature)) == 0);
        free(png);
        icon_windows_image_deinit(&image);
        assert(image.rgba == NULL);
    }

    const uint32_t FallbackDimensions[3] = {16u, 32u, 48u};
    test_fail_shell_factory_query = true;
    for (size_t index = 0u; index < 3u; ++index) {
        assert(icon_windows_extract_rgba(item, path, FallbackDimensions[index], allocator,
                                         &image, &cleanup) == IconWindowsStatusOk);
        assert(image.rgba != NULL && image.width == FallbackDimensions[index]);
        icon_windows_image_deinit(&image);
    }
    test_fail_shell_factory_query = false;
    test_shell_image_dimension = 64u;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusOk);
    assert(image.rgba != NULL && image.width == 32u && image.height == 32u);
    icon_windows_image_deinit(&image);
    test_shell_image_dimension = 0u;

    test_fail_delete_once = true;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusCleanupRequired);
    assert(image.rgba == NULL && cleanup.bitmap_count == 1u &&
           cleanup.bitmap_handles[0] != 0u && cleanup.operation_status == IconWindowsStatusOk &&
           cleanup.operation_error == ERROR_SUCCESS && cleanup.last_error == ERROR_BUSY);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);
    assert(cleanup.bitmap_count == 0u && cleanup.bitmap_handles[0] == 0u);
    assert(allocation_state.allocations == allocation_state.deallocations);
    assert(allocation_state.bytes_allocated == allocation_state.bytes_deallocated);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);

    test_fail_shell_image_get = true;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusOk);
    icon_windows_image_deinit(&image);
    test_fail_shell_image_get = false;

    test_fail_shell_image_get = true;
    test_fail_fallback_lookup = true;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusNativeFailure);
    assert(image.rgba == NULL && cleanup.bitmap_count == 0u && cleanup.icon_count == 0u);
    test_fail_fallback_lookup = false;
    test_fail_image_list_query = true;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusNativeFailure);
    assert(image.rgba == NULL && cleanup.bitmap_count == 0u && cleanup.icon_count == 0u);
    test_fail_image_list_query = false;
    test_fail_image_list_icon = true;
    assert(icon_windows_extract_rgba(item, path, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusNativeFailure);
    assert(image.rgba == NULL && cleanup.bitmap_count == 0u && cleanup.icon_count == 0u);
    test_fail_image_list_icon = false;
    test_fail_shell_image_get = false;

    test_fail_shell_factory_query = true;
    assert(icon_windows_extract_rgba(item, NULL, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusNativeFailure);
    assert(image.rgba == NULL && cleanup.bitmap_count == 0u && cleanup.icon_count == 0u);
    test_fail_shell_factory_query = false;
    icon_windows_cleanup_t malformed_cleanup = {0};
    malformed_cleanup.bitmap_count = 5u;
    assert(icon_windows_cleanup_retry(&malformed_cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(allocation_state.allocations == allocation_state.deallocations);
    assert(allocation_state.bytes_allocated == allocation_state.bytes_deallocated);
    (void)IShellItem_Release(item);
    CoUninitialize();
}

int main(void)
{
    test_owned_gdi_fixtures();
    test_shell_extraction();
    return 0;
}

#else
#include "../../src/interop/icon_windows.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void *unused_allocate(void *context, size_t size)
{
    (void)context;
    (void)size;
    return NULL;
}

static void unused_deallocate(void *context, void *memory, size_t size)
{
    (void)context;
    (void)memory;
    (void)size;
}

int main(void)
{
    icon_windows_image_t image = {0};
    icon_windows_cleanup_t cleanup = {0};
    const icon_windows_allocator_t allocator = {
        .context = NULL,
        .allocate = unused_allocate,
        .deallocate = unused_deallocate
    };
    IShellItem *item = (IShellItem *)(uintptr_t)1u;
    assert(icon_windows_extract_rgba(item, NULL, 16u, allocator, &image, &cleanup) ==
           IconWindowsStatusUnsupportedPlatform);
    assert(icon_windows_extract_rgba(item, NULL, 17u, allocator, &image, &cleanup) ==
           IconWindowsStatusUnsupportedDimension);
    assert(icon_windows_extract_rgba(NULL, NULL, 16u, allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusUnsupportedPlatform);
    icon_windows_image_deinit(&image);
    assert(image.rgba == NULL && image.width == 0u);
    return 0;
}
#endif
