/** @file icon_windows.c @brief Read-only Shell icon acquisition and RGBA conversion. */
#include "icon_windows.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool pointer_range(const void *pointer, size_t length, uintptr_t *start, uintptr_t *end)
{
    if (pointer == NULL || length == 0u) {
        return false;
    }
    const uintptr_t address = (uintptr_t)pointer;
    if (address > UINTPTR_MAX - length) {
        return false;
    }
    *start = address;
    *end = address + length;
    return true;
}

static bool ranges_overlap(uintptr_t first_start,
                           uintptr_t first_end,
                           uintptr_t second_start,
                           uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

#ifdef _WIN32
#include <shellapi.h>
#include <commoncontrols.h>

#ifdef IconWindowsTestSeams
static bool test_fail_shell_factory_query;
static bool test_fail_shell_image_get;
static bool test_fail_fallback_lookup;
static bool test_fail_image_list_query;
static bool test_fail_image_list_icon;
static bool test_fail_delete_once;
static bool test_fail_cleanup_delete_once;
static bool test_fail_cleanup_dc_once;
static bool test_fail_cleanup_icon_once;
static bool test_fail_close_dc_once;
static bool test_fail_close_icon_once;
static bool test_fail_get_icon_info_once;
static bool test_force_icon_mask_null_once;
static bool test_fail_mask_dibits_once;
static bool test_fail_select_object_once;
static bool test_fail_draw_icon_once;
static bool test_fail_restore_object_once;
static bool test_fail_create_dc_once;
static bool test_fail_create_dib_once;
static uint32_t test_shell_image_dimension;
static uint32_t test_corrupt_shell_source_field_once;
static uint32_t test_corrupt_output_dib_field_once;
static bool test_corrupt_output_dib_size_once;
static uint32_t test_last_native_stage;
static uint32_t test_last_native_error;
static uint32_t test_last_native_detail;
static int32_t test_last_shell_result;
static int32_t test_last_output_bitmap_width;
static int32_t test_last_output_bitmap_height;
static uint16_t test_last_output_bits_pixel;
static int32_t test_last_output_header_height;
static uint16_t test_last_output_header_bits_pixel;
static uint32_t test_last_output_compression;
static int32_t test_last_output_description_bytes;
#endif

static const uint32_t SupportedDimensions[IconWindowsSupportedDimensionCount] = {
    16u, 24u, 32u, 48u, 64u, 128u, 256u
};
static const IID IidImageList = {
    0x46eb5926u, 0x582eu, 0x4017u, {0x9fu, 0xdfu, 0xe8u, 0x99u, 0x8du, 0xaau, 0x09u, 0x50u}
};

static bool supported_dimension(uint32_t dimension)
{
    for (size_t index = 0u; index < IconWindowsSupportedDimensionCount; ++index) {
        if (SupportedDimensions[index] == dimension) {
            return true;
        }
    }
    return false;
}

static bool image_is_empty(const icon_windows_image_t *image)
{
    return image->rgba == NULL && image->stride == 0u && image->width == 0u &&
           image->height == 0u && image->allocation_size == 0u &&
           image->allocator.context == NULL && image->allocator.allocate == NULL &&
           image->allocator.deallocate == NULL;
}

static bool cleanup_is_empty(const icon_windows_cleanup_t *cleanup)
{
    return cleanup->bitmap_handles[0] == 0u && cleanup->bitmap_handles[1] == 0u &&
           cleanup->bitmap_handles[2] == 0u && cleanup->bitmap_handles[3] == 0u &&
           cleanup->icon_handles[0] == 0u && cleanup->device_context_handles[0] == 0u &&
           cleanup->bitmap_count == 0u && cleanup->icon_count == 0u &&
           cleanup->device_context_count == 0u && cleanup->operation_status == 0u &&
           cleanup->reserved_zero == 0u && cleanup->operation_error == 0u &&
           cleanup->last_error == 0u;
}

static size_t fallback_path_length(const wchar_t *path)
{
    if (path == NULL) {
        return 0u;
    }
    for (size_t index = 0u; index <= IconWindowsMaxFallbackPathUnits; ++index) {
        if (path[index] == L'\0') {
            return index;
        }
    }
    return SIZE_MAX;
}

static void keep_bitmap(icon_windows_cleanup_t *cleanup, HBITMAP bitmap)
{
    if (bitmap != NULL && cleanup->bitmap_count < 4u) {
        cleanup->bitmap_handles[cleanup->bitmap_count++] = (uintptr_t)bitmap;
    }
}

static void keep_icon(icon_windows_cleanup_t *cleanup, HICON icon)
{
    if (icon != NULL && cleanup->icon_count < 1u) {
        cleanup->icon_handles[cleanup->icon_count++] = (uintptr_t)icon;
    }
}

static void keep_dc(icon_windows_cleanup_t *cleanup, HDC device_context)
{
    if (device_context != NULL && cleanup->device_context_count < 1u) {
        cleanup->device_context_handles[cleanup->device_context_count++] =
            (uintptr_t)device_context;
    }
}

static bool close_bitmap(HBITMAP *bitmap, icon_windows_cleanup_t *cleanup)
{
    if (*bitmap == NULL) {
        return true;
    }
#ifdef IconWindowsTestSeams
    if (test_fail_delete_once) {
        test_fail_delete_once = false;
        SetLastError(ERROR_BUSY);
        cleanup->last_error = GetLastError();
        keep_bitmap(cleanup, *bitmap);
        return false;
    }
#endif
    if (DeleteObject(*bitmap) == 0) {
        cleanup->last_error = GetLastError();
        keep_bitmap(cleanup, *bitmap);
        return false;
    }
    *bitmap = NULL;
    return true;
}

static bool close_icon(HICON *icon, icon_windows_cleanup_t *cleanup)
{
    if (*icon == NULL) {
        return true;
    }
#ifdef IconWindowsTestSeams
    if (test_fail_close_icon_once) {
        test_fail_close_icon_once = false;
        SetLastError(ERROR_BUSY);
        cleanup->last_error = GetLastError();
        keep_icon(cleanup, *icon);
        return false;
    }
#endif
    if (DestroyIcon(*icon) == 0) {
        cleanup->last_error = GetLastError();
        keep_icon(cleanup, *icon);
        return false;
    }
    *icon = NULL;
    return true;
}

static bool close_dc(HDC *device_context, icon_windows_cleanup_t *cleanup)
{
    if (*device_context == NULL) {
        return true;
    }
#ifdef IconWindowsTestSeams
    if (test_fail_close_dc_once) {
        test_fail_close_dc_once = false;
        SetLastError(ERROR_BUSY);
        cleanup->last_error = GetLastError();
        keep_dc(cleanup, *device_context);
        return false;
    }
#endif
    if (DeleteDC(*device_context) == 0) {
        cleanup->last_error = GetLastError();
        keep_dc(cleanup, *device_context);
        return false;
    }
    *device_context = NULL;
    return true;
}

static bool close_locals(HBITMAP *shell_bitmap,
                         HBITMAP *dib_bitmap,
                         HBITMAP *icon_color_bitmap,
                         HBITMAP *icon_mask_bitmap,
                         HICON *icon,
                         HDC *device_context,
                         icon_windows_cleanup_t *cleanup)
{
    bool closed = true;
    /* Closing the DC first deselects a DIB section before its deletion. */
    if (!close_dc(device_context, cleanup)) {
        closed = false;
    }
    if (!close_bitmap(dib_bitmap, cleanup)) {
        closed = false;
    }
    if (!close_bitmap(shell_bitmap, cleanup)) {
        closed = false;
    }
    if (!close_bitmap(icon_color_bitmap, cleanup)) {
        closed = false;
    }
    if (!close_bitmap(icon_mask_bitmap, cleanup)) {
        closed = false;
    }
    if (!close_icon(icon, cleanup)) {
        closed = false;
    }
    return closed;
}

static void normalize_premultiplied_bgra(uint8_t *pixel)
{
    const uint32_t Alpha = pixel[3];
    uint32_t red = 0u;
    uint32_t green = 0u;
    uint32_t blue = 0u;
    if (Alpha != 0u) {
        blue = ((uint32_t)pixel[0] * 255u + Alpha / 2u) / Alpha;
        green = ((uint32_t)pixel[1] * 255u + Alpha / 2u) / Alpha;
        red = ((uint32_t)pixel[2] * 255u + Alpha / 2u) / Alpha;
        if (red > 255u) red = 255u;
        if (green > 255u) green = 255u;
        if (blue > 255u) blue = 255u;
    }
    pixel[0] = (uint8_t)red;
    pixel[1] = (uint8_t)green;
    pixel[2] = (uint8_t)blue;
    pixel[3] = (uint8_t)Alpha;
}

static void convert_premultiplied_bgra_to_rgba(uint8_t *rgba, const uint8_t *bgra)
{
    rgba[0] = bgra[0];
    rgba[1] = bgra[1];
    rgba[2] = bgra[2];
    rgba[3] = bgra[3];
    normalize_premultiplied_bgra(rgba);
}

static bool apply_legacy_icon_mask(HDC device_context,
                                   HICON icon,
                                   uint32_t dimension,
                                   uint8_t *dib_pixels,
                                   HBITMAP *color_bitmap_out,
                                   HBITMAP *mask_bitmap_out,
                                   DWORD *error_out)
{
    ICONINFO icon_info;
    memset(&icon_info, 0, sizeof(icon_info));
#ifdef IconWindowsTestSeams
    if (test_fail_get_icon_info_once) {
        test_fail_get_icon_info_once = false;
        *error_out = ERROR_INVALID_HANDLE;
        return false;
    }
#endif
    if (GetIconInfo(icon, &icon_info) == 0) {
        *error_out = GetLastError();
        return false;
    }
    *color_bitmap_out = icon_info.hbmColor;
    *mask_bitmap_out = icon_info.hbmMask;
#ifdef IconWindowsTestSeams
    if (test_force_icon_mask_null_once) {
        test_force_icon_mask_null_once = false;
        icon_info.hbmMask = NULL;
    }
#endif
    if (icon_info.hbmMask == NULL || dimension > 48u) {
        *error_out = ERROR_INVALID_DATA;
        return false;
    }
    BITMAP mask_description;
    memset(&mask_description, 0, sizeof(mask_description));
    if (GetObjectW(icon_info.hbmMask, sizeof(mask_description), &mask_description) !=
            sizeof(mask_description) ||
        mask_description.bmWidth != (LONG)dimension ||
        (mask_description.bmHeight != (LONG)dimension &&
         mask_description.bmHeight != (LONG)(dimension * 2u))) {
        *error_out = ERROR_INVALID_DATA;
        return false;
    }
    const uint32_t MaskHeight = (uint32_t)mask_description.bmHeight;
    const bool MonochromeMask = icon_info.hbmColor == NULL && MaskHeight == dimension * 2u;
    uint32_t mask_pixels[48u * 96u];
    BITMAPINFO mask_info;
    memset(&mask_info, 0, sizeof(mask_info));
    mask_info.bmiHeader.biSize = sizeof(mask_info.bmiHeader);
    mask_info.bmiHeader.biWidth = (LONG)dimension;
    mask_info.bmiHeader.biHeight = -(LONG)MaskHeight;
    mask_info.bmiHeader.biPlanes = 1u;
    mask_info.bmiHeader.biBitCount = 32u;
    mask_info.bmiHeader.biCompression = BI_RGB;
#ifdef IconWindowsTestSeams
    int mask_rows = 0;
    if (test_fail_mask_dibits_once) {
        test_fail_mask_dibits_once = false;
        SetLastError(ERROR_GEN_FAILURE);
    } else {
        mask_rows = GetDIBits(device_context, icon_info.hbmMask, 0u, MaskHeight,
                              mask_pixels, &mask_info, DIB_RGB_COLORS);
    }
#else
    const int mask_rows = GetDIBits(device_context, icon_info.hbmMask, 0u, MaskHeight,
                                    mask_pixels, &mask_info, DIB_RGB_COLORS);
#endif
    if (mask_rows != (int)MaskHeight) {
        *error_out = GetLastError();
        return false;
    }
    if (MonochromeMask) {
        const size_t XorPlaneOffset = (size_t)dimension * dimension;
        for (size_t index = 0u; index < (size_t)dimension * dimension; ++index) {
            const bool AndBit = ((mask_pixels[index] >> 16u) & 0xffu) != 0u;
            const bool XorBit = ((mask_pixels[XorPlaneOffset + index] >> 16u) & 0xffu) != 0u;
            if (AndBit && XorBit) {
                *error_out = ERROR_NOT_SUPPORTED;
                return false;
            }
        }
    }
    bool has_alpha = false;
    const size_t PixelCount = (size_t)dimension * dimension;
    for (size_t index = 0u; index < PixelCount; ++index) {
        has_alpha = has_alpha || dib_pixels[index * 4u + 3u] != 0u;
    }
    for (uint32_t row = 0u; row < dimension; ++row) {
        for (uint32_t column = 0u; column < dimension; ++column) {
            const size_t Index = (size_t)row * dimension + column;
            const bool Transparent = ((mask_pixels[Index] >> 16u) & 0xffu) != 0u;
            if (!has_alpha) {
                dib_pixels[Index * 4u + 3u] = Transparent ? 0u : 255u;
            } else if (Transparent) {
                dib_pixels[Index * 4u + 3u] = 0u;
            }
        }
    }
    return true;
}

static bool copy_bitmap_to_top_down_dib(HDC device_context,
                                        HBITMAP source_bitmap,
                                        uint32_t source_width,
                                        uint32_t source_height,
                                        void *destination_bits,
                                        BITMAPINFO *destination_info,
                                        bool *source_has_alpha_out,
                                        DWORD *error_out)
{
    BITMAP source_description;
    memset(&source_description, 0, sizeof(source_description));
    if (GetObjectW(source_bitmap, sizeof(source_description), &source_description) !=
            sizeof(source_description) ||
        source_description.bmWidth != (LONG)source_width ||
        source_description.bmHeight != (LONG)source_height ||
        source_description.bmWidth <= 0 || source_description.bmHeight <= 0) {
        *error_out = ERROR_INVALID_DATA;
        return false;
    }
    DIBSECTION source_dib_section;
    memset(&source_dib_section, 0, sizeof(source_dib_section));
    const int dib_section_bytes = GetObjectW(source_bitmap, sizeof(source_dib_section),
                                             &source_dib_section);
    *source_has_alpha_out = dib_section_bytes == sizeof(source_dib_section) &&
        source_dib_section.dsBmih.biBitCount == 32u &&
        source_dib_section.dsBmih.biCompression == BI_RGB;
    const int Rows = GetDIBits(device_context, source_bitmap, 0u, source_height,
                               destination_bits, destination_info, DIB_RGB_COLORS);
    if (Rows != (int)source_height) {
        *error_out = GetLastError();
        return false;
    }
    if (!*source_has_alpha_out) {
        uint8_t *pixels = destination_bits;
        const size_t pixel_count = (size_t)source_width * (size_t)source_height;
        for (size_t index = 0u; index < pixel_count; ++index) {
            pixels[index * 4u + 3u] = 255u;
        }
    }
    return true;
}

static bool draw_icon_to_top_down_dib(HDC device_context,
                                      HBITMAP destination_bitmap,
                                      HICON icon,
                                      uint32_t dimension,
                                      DWORD *error_out)
{
#ifdef IconWindowsTestSeams
    if (test_fail_select_object_once) {
        test_fail_select_object_once = false;
        SetLastError(ERROR_INVALID_HANDLE);
        *error_out = GetLastError();
        return false;
    }
#endif
    HGDIOBJ previous = SelectObject(device_context, destination_bitmap);
    if (previous == NULL || previous == HGDI_ERROR) {
        *error_out = GetLastError();
        return false;
    }
#ifdef IconWindowsTestSeams
    const BOOL Drawn = test_fail_draw_icon_once ? FALSE :
        DrawIconEx(device_context, 0, 0, icon, (int)dimension,
                   (int)dimension, 0u, NULL, DI_NORMAL);
    test_fail_draw_icon_once = false;
    if (Drawn == 0) {
        SetLastError(ERROR_INVALID_HANDLE);
    }
#else
    const BOOL Drawn = DrawIconEx(device_context, 0, 0, icon, (int)dimension,
                                  (int)dimension, 0u, NULL, DI_NORMAL);
#endif
    if (Drawn == 0) {
        *error_out = GetLastError();
        (void)SelectObject(device_context, previous);
        return false;
    }
#ifdef IconWindowsTestSeams
    if (test_fail_restore_object_once) {
        test_fail_restore_object_once = false;
        SetLastError(ERROR_INVALID_HANDLE);
        *error_out = GetLastError();
        return false;
    }
#endif
    const HGDIOBJ restored = SelectObject(device_context, previous);
    if (restored == NULL || restored == HGDI_ERROR) {
        *error_out = GetLastError();
        return false;
    }
    return true;
}

static icon_windows_status_t extract_native_image(IShellItem *item,
                                                  const wchar_t *fallback_path,
                                                  uint32_t dimension,
                                                  icon_windows_allocator_t allocator,
                                                  icon_windows_image_t *image_out,
                                                  icon_windows_cleanup_t *cleanup_out)
{
    icon_windows_status_t operation_status = IconWindowsStatusNativeFailure;
    HRESULT shell_result = E_FAIL;
    DWORD native_error = ERROR_GEN_FAILURE;
    IShellItemImageFactory *factory = NULL;
    IImageList *image_list = NULL;
    HBITMAP shell_bitmap = NULL;
    HBITMAP dib_bitmap = NULL;
    HBITMAP icon_color_bitmap = NULL;
    HBITMAP icon_mask_bitmap = NULL;
    HICON icon = NULL;
    HDC device_context = NULL;
    void *dib_pixels = NULL;
    uint8_t *owned_pixels = NULL;
    BITMAPINFO bitmap_info;
    memset(&bitmap_info, 0, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
    uint32_t source_width = dimension;
    uint32_t source_height = dimension;
    uint32_t native_stage = 1u;
    uint32_t native_detail = 0u;
    (void)native_stage;
    (void)native_detail;
    bitmap_info.bmiHeader.biWidth = (LONG)source_width;
    bitmap_info.bmiHeader.biHeight = -(LONG)source_height;
    bitmap_info.bmiHeader.biPlanes = 1u;
    bitmap_info.bmiHeader.biBitCount = 32u;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

#ifdef IconWindowsTestSeams
    shell_result = test_fail_shell_factory_query ? E_NOINTERFACE :
        IShellItem_QueryInterface(item, &IID_IShellItemImageFactory, (void **)&factory);
#else
    shell_result = IShellItem_QueryInterface(item, &IID_IShellItemImageFactory,
                                             (void **)&factory);
#endif
    if (SUCCEEDED(shell_result)) {
        native_stage = 2u;
        uint32_t shell_dimension = dimension;
#ifdef IconWindowsTestSeams
        if (test_shell_image_dimension != 0u) {
            shell_dimension = test_shell_image_dimension;
        }
#endif
        SIZE requested_size = {(LONG)shell_dimension, (LONG)shell_dimension};
#ifdef IconWindowsTestSeams
        shell_result = test_fail_shell_image_get ? E_FAIL :
            IShellItemImageFactory_GetImage(
                factory, requested_size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                &shell_bitmap);
#else
        shell_result = IShellItemImageFactory_GetImage(
            factory, requested_size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
            &shell_bitmap);
#endif
    }
    if (FAILED(shell_result) || shell_bitmap == NULL) {
        native_stage = 3u;
        const size_t PathUnits = fallback_path_length(fallback_path);
        if (fallback_path != NULL && PathUnits != 0u && PathUnits != SIZE_MAX &&
            (dimension == 16u || dimension == 32u || dimension == 48u)) {
            SHFILEINFOW file_info;
            memset(&file_info, 0, sizeof(file_info));
            const UINT Flags = SHGFI_SYSICONINDEX |
                (dimension == 16u ? SHGFI_SMALLICON : SHGFI_LARGEICON);
#ifdef IconWindowsTestSeams
            const DWORD_PTR IconIndex = test_fail_fallback_lookup ? 0u :
                SHGetFileInfoW(fallback_path, 0u, &file_info, sizeof(file_info), Flags);
#else
            const DWORD_PTR IconIndex =
                SHGetFileInfoW(fallback_path, 0u, &file_info, sizeof(file_info), Flags);
#endif
            if (IconIndex != 0u) {
                const int ImageListSize = dimension == 16u ? SHIL_SMALL :
                    (dimension == 32u ? SHIL_LARGE : SHIL_EXTRALARGE);
#ifdef IconWindowsTestSeams
                shell_result = test_fail_image_list_query ? E_FAIL :
                    SHGetImageList(ImageListSize, &IidImageList,
                                   (void **)&image_list);
#else
                shell_result = SHGetImageList(ImageListSize, &IidImageList,
                                              (void **)&image_list);
#endif
                if (SUCCEEDED(shell_result) && image_list != NULL) {
#ifdef IconWindowsTestSeams
                    shell_result = test_fail_image_list_icon ? E_FAIL :
                        IImageList_GetIcon(image_list, file_info.iIcon,
                                           ILD_TRANSPARENT, &icon);
#else
                    shell_result = IImageList_GetIcon(image_list, file_info.iIcon,
                                                      ILD_TRANSPARENT, &icon);
#endif
                }
            }
        }
        if (icon == NULL) {
            native_error = FAILED(shell_result) ? (DWORD)HRESULT_CODE(shell_result) :
                                                  ERROR_NOT_FOUND;
            goto cleanup;
        }
    }

    if (shell_bitmap != NULL) {
        native_stage = 4u;
        BITMAP source_description;
        memset(&source_description, 0, sizeof(source_description));
        int source_description_bytes = GetObjectW(
            shell_bitmap, sizeof(source_description), &source_description);
#ifdef IconWindowsTestSeams
        switch (test_corrupt_shell_source_field_once) {
        case 1u: source_description_bytes = 0; break;
        case 2u: source_description.bmWidth = 0; break;
        case 3u: source_description.bmHeight = 0; break;
        case 4u: source_description.bmWidth =
                    (LONG)IconWindowsMaxNativeSourceDimension + 1; break;
        case 5u: source_description.bmHeight =
                    (LONG)IconWindowsMaxNativeSourceDimension + 1; break;
        default: break;
        }
        test_corrupt_shell_source_field_once = 0u;
#endif
        if (source_description_bytes != sizeof(source_description) ||
            source_description.bmWidth <= 0 || source_description.bmHeight <= 0 ||
            source_description.bmWidth > (LONG)IconWindowsMaxNativeSourceDimension ||
            source_description.bmHeight > (LONG)IconWindowsMaxNativeSourceDimension) {
            native_error = ERROR_INVALID_DATA;
            goto cleanup;
        }
        source_width = (uint32_t)source_description.bmWidth;
        source_height = (uint32_t)source_description.bmHeight;
        bitmap_info.bmiHeader.biWidth = (LONG)source_width;
        bitmap_info.bmiHeader.biHeight = -(LONG)source_height;
    }

#ifdef IconWindowsTestSeams
    if (test_fail_create_dc_once) {
        test_fail_create_dc_once = false;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        device_context = NULL;
    } else {
        device_context = CreateCompatibleDC(NULL);
    }
#else
    device_context = CreateCompatibleDC(NULL);
#endif
    native_stage = 5u;
    if (device_context == NULL) {
        native_error = GetLastError();
        goto cleanup;
    }
#ifdef IconWindowsTestSeams
    if (test_fail_create_dib_once) {
        test_fail_create_dib_once = false;
        SetLastError(ERROR_INVALID_DATA);
        dib_bitmap = NULL;
    } else {
        dib_bitmap = CreateDIBSection(device_context, &bitmap_info, DIB_RGB_COLORS,
                                      &dib_pixels, NULL, 0u);
    }
#else
    dib_bitmap = CreateDIBSection(device_context, &bitmap_info, DIB_RGB_COLORS,
                                  &dib_pixels, NULL, 0u);
#endif
    native_stage = 6u;
    if (dib_bitmap == NULL || dib_pixels == NULL) {
        native_detail = dib_bitmap == NULL ? 1u : 2u;
        native_error = GetLastError();
        goto cleanup;
    }
    DIBSECTION output_description;
    memset(&output_description, 0, sizeof(output_description));
    const int output_description_bytes = GetObjectW(
        dib_bitmap, sizeof(output_description), &output_description);
    int checked_output_description_bytes = output_description_bytes;
#ifdef IconWindowsTestSeams
    if (test_corrupt_output_dib_size_once) {
        checked_output_description_bytes = 0;
    }
    test_corrupt_output_dib_size_once = false;
    if (test_corrupt_output_dib_field_once != 0u) {
        switch (test_corrupt_output_dib_field_once) {
        case 1u: output_description.dsBm.bmWidth += 1; break;
        case 2u: output_description.dsBm.bmHeight += 1; break;
        case 3u: output_description.dsBm.bmBitsPixel = 24u; break;
        case 4u: output_description.dsBmih.biHeight += 1; break;
        case 5u: output_description.dsBmih.biBitCount = 24u; break;
        case 6u: output_description.dsBmih.biCompression = BI_RLE8; break;
        default: break;
        }
        test_corrupt_output_dib_field_once = 0u;
    }
    test_last_output_description_bytes = output_description_bytes;
    test_last_output_bitmap_width = output_description.dsBm.bmWidth;
    test_last_output_bitmap_height = output_description.dsBm.bmHeight;
    test_last_output_bits_pixel = output_description.dsBm.bmBitsPixel;
    test_last_output_header_height = output_description.dsBmih.biHeight;
    test_last_output_header_bits_pixel = output_description.dsBmih.biBitCount;
    test_last_output_compression = output_description.dsBmih.biCompression;
#endif
    /* GetObject may normalize the DIBSECTION height sign; creation requested top-down. */
    if (checked_output_description_bytes != sizeof(output_description)) {
        native_detail = 3u;
    } else if (output_description.dsBm.bmWidth != (LONG)source_width) {
        native_detail = 4u;
    } else if (output_description.dsBm.bmHeight != (LONG)source_height) {
        native_detail = 5u;
    } else if (output_description.dsBm.bmBitsPixel != 32u) {
        native_detail = 6u;
    } else if (output_description.dsBmih.biHeight != -(LONG)source_height &&
               output_description.dsBmih.biHeight != (LONG)source_height) {
        native_detail = 7u;
    } else if (output_description.dsBmih.biBitCount != 32u) {
        native_detail = 8u;
    } else if (output_description.dsBmih.biCompression != BI_RGB) {
        native_detail = 9u;
    }
    if (native_detail >= 3u && native_detail <= 9u) {
        native_error = ERROR_INVALID_DATA;
        goto cleanup;
    }
    if (shell_bitmap != NULL) {
        native_stage = 7u;
        bool source_has_alpha = false;
        if (!copy_bitmap_to_top_down_dib(device_context, shell_bitmap, source_width,
                                         source_height,
                                         dib_pixels, &bitmap_info, &source_has_alpha,
                                         &native_error)) {
            goto cleanup;
        }
    } else if (!draw_icon_to_top_down_dib(device_context, dib_bitmap, icon,
                                          dimension, &native_error)) {
        goto cleanup;
    } else if (!apply_legacy_icon_mask(device_context, icon, dimension, dib_pixels,
                                       &icon_color_bitmap, &icon_mask_bitmap,
                                       &native_error)) {
        goto cleanup;
    }

    const size_t RowBytes = (size_t)dimension * 4u;
    const size_t ImageSize = RowBytes * (size_t)dimension;
    native_stage = 8u;
    owned_pixels = allocator.allocate(allocator.context, ImageSize);
    if (owned_pixels == NULL) {
        operation_status = IconWindowsStatusOutOfMemory;
        native_error = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    const uint8_t *Source = (const uint8_t *)dib_pixels;
    const size_t SourceStride = (size_t)source_width * 4u;
    for (uint32_t row = 0u; row < dimension; ++row) {
        const uint32_t SourceRow = (uint32_t)(((uint64_t)row * source_height) / dimension);
        for (uint32_t column = 0u; column < dimension; ++column) {
            const uint32_t SourceColumn =
                (uint32_t)(((uint64_t)column * source_width) / dimension);
            const size_t TargetOffset = (size_t)row * RowBytes + (size_t)column * 4u;
            const size_t SourceOffset = (size_t)SourceRow * SourceStride +
                                         (size_t)SourceColumn * 4u;
            convert_premultiplied_bgra_to_rgba(owned_pixels + TargetOffset,
                                                Source + SourceOffset);
        }
    }

    operation_status = IconWindowsStatusOk;

cleanup:
#ifdef IconWindowsTestSeams
    test_last_native_stage = native_stage;
    test_last_native_error = native_error;
    test_last_native_detail = native_detail;
    test_last_shell_result = (int32_t)shell_result;
#endif
    if (factory != NULL) {
        (void)IShellItemImageFactory_Release(factory);
        factory = NULL;
    }
    if (image_list != NULL) {
        (void)IImageList_Release(image_list);
        image_list = NULL;
    }
    if (!close_locals(&shell_bitmap, &dib_bitmap, &icon_color_bitmap,
                      &icon_mask_bitmap, &icon, &device_context,
                      cleanup_out)) {
        cleanup_out->operation_status = (uint32_t)operation_status;
        cleanup_out->operation_error = operation_status == IconWindowsStatusOk ?
            ERROR_SUCCESS : native_error;
        if (owned_pixels != NULL) {
            allocator.deallocate(allocator.context, owned_pixels,
                                 (size_t)dimension * (size_t)dimension * 4u);
            owned_pixels = NULL;
        }
        return IconWindowsStatusCleanupRequired;
    }
    if (operation_status != IconWindowsStatusOk) {
        if (owned_pixels != NULL) {
            allocator.deallocate(allocator.context, owned_pixels,
                                 (size_t)dimension * (size_t)dimension * 4u);
        }
        return operation_status;
    }

    image_out->rgba = owned_pixels;
    image_out->stride = (size_t)dimension * 4u;
    image_out->width = dimension;
    image_out->height = dimension;
    image_out->allocation_size = (size_t)dimension * (size_t)dimension * 4u;
    image_out->allocator = allocator;
    return IconWindowsStatusOk;
}

icon_windows_status_t icon_windows_extract_rgba(IShellItem *item,
                                                const wchar_t *fallback_path,
                                                uint32_t dimension,
                                                icon_windows_allocator_t allocator,
                                                icon_windows_image_t *image_out,
                                                icon_windows_cleanup_t *cleanup_out)
{
    uintptr_t image_start = 0u;
    uintptr_t image_end = 0u;
    uintptr_t cleanup_start = 0u;
    uintptr_t cleanup_end = 0u;
    uintptr_t item_start = 0u;
    uintptr_t item_end = 0u;
    uintptr_t bounded_path_start = 0u;
    uintptr_t bounded_path_end = 0u;
    if (item == NULL || image_out == NULL || cleanup_out == NULL ||
        ((uintptr_t)image_out % _Alignof(icon_windows_image_t)) != 0u ||
        ((uintptr_t)cleanup_out % _Alignof(icon_windows_cleanup_t)) != 0u ||
        allocator.allocate == NULL || allocator.deallocate == NULL ||
        !pointer_range(image_out, sizeof(*image_out), &image_start, &image_end) ||
        !pointer_range(cleanup_out, sizeof(*cleanup_out), &cleanup_start, &cleanup_end) ||
        !pointer_range(item, sizeof(void *), &item_start, &item_end) ||
        ranges_overlap(image_start, image_end, cleanup_start, cleanup_end) ||
        ranges_overlap(image_start, image_end, item_start, item_end) ||
        ranges_overlap(cleanup_start, cleanup_end, item_start, item_end)) {
        return IconWindowsStatusInvalidArgument;
    }
    if (fallback_path != NULL) {
        const uintptr_t PathAddress = (uintptr_t)fallback_path;
        if (!pointer_range(fallback_path,
                           (IconWindowsMaxFallbackPathUnits + 1u) * sizeof(wchar_t),
                           &bounded_path_start, &bounded_path_end) ||
            (PathAddress >= image_start && PathAddress < image_end) ||
            (PathAddress >= cleanup_start && PathAddress < cleanup_end) ||
            (PathAddress >= item_start && PathAddress < item_end)) {
            return IconWindowsStatusInvalidArgument;
        }
    }
    const size_t PathUnits = fallback_path_length(fallback_path);
    if (PathUnits == SIZE_MAX) {
        return IconWindowsStatusInvalidArgument;
    }
    if (fallback_path != NULL) {
        uintptr_t path_start = 0u;
        uintptr_t path_end = 0u;
        if (!pointer_range(fallback_path, (PathUnits + 1u) * sizeof(wchar_t),
                           &path_start, &path_end) ||
            ranges_overlap(path_start, path_end, image_start, image_end) ||
            ranges_overlap(path_start, path_end, cleanup_start, cleanup_end) ||
            ranges_overlap(path_start, path_end, item_start, item_end)) {
            return IconWindowsStatusInvalidArgument;
        }
    }
    if (!image_is_empty(image_out) || !cleanup_is_empty(cleanup_out)) {
        return IconWindowsStatusInvalidArgument;
    }
    if (!supported_dimension(dimension)) {
        return IconWindowsStatusUnsupportedDimension;
    }
    (void)bounded_path_start;
    (void)bounded_path_end;
    return extract_native_image(item, fallback_path, dimension, allocator,
                                image_out, cleanup_out);
}

icon_windows_status_t icon_windows_cleanup_retry(icon_windows_cleanup_t *cleanup)
{
    uintptr_t cleanup_start = 0u;
    uintptr_t cleanup_end = 0u;
    if (cleanup == NULL || ((uintptr_t)cleanup % _Alignof(icon_windows_cleanup_t)) != 0u ||
        !pointer_range(cleanup, sizeof(*cleanup), &cleanup_start, &cleanup_end) ||
        cleanup->bitmap_count > 4u || cleanup->icon_count > 1u ||
        cleanup->device_context_count > 1u || cleanup->reserved_zero != 0u) {
        return IconWindowsStatusInvalidArgument;
    }
    for (uint32_t index = 0u; index < 4u; ++index) {
        if ((index < cleanup->bitmap_count) != (cleanup->bitmap_handles[index] != 0u)) {
            return IconWindowsStatusInvalidArgument;
        }
    }
    if ((cleanup->icon_count == 1u) != (cleanup->icon_handles[0] != 0u) ||
        (cleanup->device_context_count == 1u) !=
            (cleanup->device_context_handles[0] != 0u)) {
        return IconWindowsStatusInvalidArgument;
    }
    if (cleanup->bitmap_count == 0u && cleanup->icon_count == 0u &&
        cleanup->device_context_count == 0u) {
        return cleanup_is_empty(cleanup) ? IconWindowsStatusOk :
                                           IconWindowsStatusInvalidArgument;
    }
    bool closed = true;
    for (uint32_t index = 0u; index < cleanup->device_context_count; ++index) {
        HDC device_context = (HDC)cleanup->device_context_handles[index];
        BOOL deleted = FALSE;
#ifdef IconWindowsTestSeams
        if (device_context != NULL && test_fail_cleanup_dc_once) {
            test_fail_cleanup_dc_once = false;
            SetLastError(ERROR_BUSY);
        } else if (device_context != NULL) {
            deleted = DeleteDC(device_context);
        }
#else
        if (device_context != NULL) {
            deleted = DeleteDC(device_context);
        }
#endif
        if (device_context != NULL && deleted != 0) {
            cleanup->device_context_handles[index] = 0u;
        } else if (device_context != NULL) {
            cleanup->last_error = GetLastError();
            closed = false;
        }
    }
    for (uint32_t index = 0u; index < cleanup->bitmap_count; ++index) {
        HBITMAP bitmap = (HBITMAP)cleanup->bitmap_handles[index];
        BOOL deleted = FALSE;
#ifdef IconWindowsTestSeams
        if (bitmap != NULL && test_fail_cleanup_delete_once) {
            test_fail_cleanup_delete_once = false;
            SetLastError(ERROR_BUSY);
        } else if (bitmap != NULL) {
            deleted = DeleteObject(bitmap);
        }
#else
        if (bitmap != NULL) {
            deleted = DeleteObject(bitmap);
        }
#endif
        if (bitmap != NULL && deleted != 0) {
            cleanup->bitmap_handles[index] = 0u;
        } else if (bitmap != NULL) {
            cleanup->last_error = GetLastError();
            closed = false;
        }
    }
    for (uint32_t index = 0u; index < cleanup->icon_count; ++index) {
        HICON icon = (HICON)cleanup->icon_handles[index];
        BOOL destroyed = FALSE;
#ifdef IconWindowsTestSeams
        if (icon != NULL && test_fail_cleanup_icon_once) {
            test_fail_cleanup_icon_once = false;
            SetLastError(ERROR_BUSY);
        } else if (icon != NULL) {
            destroyed = DestroyIcon(icon);
        }
#else
        if (icon != NULL) {
            destroyed = DestroyIcon(icon);
        }
#endif
        if (icon != NULL && destroyed != 0) {
            cleanup->icon_handles[index] = 0u;
        } else if (icon != NULL) {
            cleanup->last_error = GetLastError();
            closed = false;
        }
    }
    uint32_t retained = 0u;
    for (uint32_t index = 0u; index < cleanup->bitmap_count; ++index) {
        if (cleanup->bitmap_handles[index] != 0u) {
            cleanup->bitmap_handles[retained++] = cleanup->bitmap_handles[index];
        }
    }
    while (retained < 2u) {
        cleanup->bitmap_handles[retained++] = 0u;
    }
    cleanup->bitmap_count = 0u;
    while (cleanup->bitmap_count < 2u &&
           cleanup->bitmap_handles[cleanup->bitmap_count] != 0u) {
        ++cleanup->bitmap_count;
    }
    if (cleanup->icon_handles[0] == 0u) cleanup->icon_count = 0u;
    if (cleanup->device_context_handles[0] == 0u) cleanup->device_context_count = 0u;
    if (!closed) {
        return IconWindowsStatusCleanupRequired;
    }
    memset(cleanup, 0, sizeof(*cleanup));
    return IconWindowsStatusOk;
}

#else

static bool image_is_empty(const icon_windows_image_t *image)
{
    return image != NULL && image->rgba == NULL && image->stride == 0u &&
           image->width == 0u && image->height == 0u && image->allocation_size == 0u &&
           image->allocator.context == NULL && image->allocator.allocate == NULL &&
           image->allocator.deallocate == NULL;
}

static bool cleanup_is_empty(const icon_windows_cleanup_t *cleanup)
{
    if (cleanup == NULL) {
        return false;
    }
    for (size_t index = 0u; index < 4u; ++index) {
        if (cleanup->bitmap_handles[index] != 0u) return false;
    }
    for (size_t index = 0u; index < 1u; ++index) {
        if (cleanup->icon_handles[index] != 0u ||
            cleanup->device_context_handles[index] != 0u) return false;
    }
    return cleanup->bitmap_count == 0u && cleanup->icon_count == 0u &&
           cleanup->device_context_count == 0u && cleanup->operation_status == 0u &&
           cleanup->reserved_zero == 0u && cleanup->operation_error == 0u &&
           cleanup->last_error == 0u;
}

icon_windows_status_t icon_windows_extract_rgba(IShellItem *item,
                                                const wchar_t *fallback_path,
                                                uint32_t dimension,
                                                icon_windows_allocator_t allocator,
                                                icon_windows_image_t *image_out,
                                                icon_windows_cleanup_t *cleanup_out)
{
    (void)fallback_path;
    uintptr_t image_start = 0u;
    uintptr_t image_end = 0u;
    uintptr_t cleanup_start = 0u;
    uintptr_t cleanup_end = 0u;
    uintptr_t item_start = 0u;
    uintptr_t item_end = 0u;
    if (item == NULL || image_out == NULL || cleanup_out == NULL ||
        ((uintptr_t)image_out % _Alignof(icon_windows_image_t)) != 0u ||
        ((uintptr_t)cleanup_out % _Alignof(icon_windows_cleanup_t)) != 0u ||
        allocator.allocate == NULL || allocator.deallocate == NULL ||
        !pointer_range(image_out, sizeof(*image_out), &image_start, &image_end) ||
        !pointer_range(cleanup_out, sizeof(*cleanup_out), &cleanup_start, &cleanup_end) ||
        !pointer_range(item, sizeof(void *), &item_start, &item_end) ||
        ranges_overlap(image_start, image_end, cleanup_start, cleanup_end) ||
        ranges_overlap(image_start, image_end, item_start, item_end) ||
        ranges_overlap(cleanup_start, cleanup_end, item_start, item_end)) {
        return IconWindowsStatusInvalidArgument;
    }
    if (fallback_path != NULL) {
        uintptr_t path_start = 0u;
        uintptr_t path_end = 0u;
        if (!pointer_range(fallback_path,
                           (IconWindowsMaxFallbackPathUnits + 1u) * sizeof(wchar_t),
                           &path_start, &path_end) ||
            ranges_overlap(path_start, path_end, image_start, image_end) ||
            ranges_overlap(path_start, path_end, cleanup_start, cleanup_end) ||
            ranges_overlap(path_start, path_end, item_start, item_end)) {
            return IconWindowsStatusInvalidArgument;
        }
    }
    if (!image_is_empty(image_out) || !cleanup_is_empty(cleanup_out)) {
        return IconWindowsStatusInvalidArgument;
    }
    const uint32_t SupportedDimensions[IconWindowsSupportedDimensionCount] = {
        16u, 24u, 32u, 48u, 64u, 128u, 256u
    };
    bool supported = false;
    for (size_t index = 0u; index < IconWindowsSupportedDimensionCount; ++index) {
        if (SupportedDimensions[index] == dimension) {
            supported = true;
            break;
        }
    }
    if (!supported) {
        return IconWindowsStatusUnsupportedDimension;
    }
    return IconWindowsStatusUnsupportedPlatform;
}

icon_windows_status_t icon_windows_cleanup_retry(icon_windows_cleanup_t *cleanup)
{
    uintptr_t cleanup_start = 0u;
    uintptr_t cleanup_end = 0u;
    if (cleanup == NULL || ((uintptr_t)cleanup % _Alignof(icon_windows_cleanup_t)) != 0u ||
        !pointer_range(cleanup, sizeof(*cleanup), &cleanup_start, &cleanup_end)) {
        return IconWindowsStatusInvalidArgument;
    }
    if (!cleanup_is_empty(cleanup)) {
        return IconWindowsStatusInvalidArgument;
    }
    return IconWindowsStatusUnsupportedPlatform;
}

#endif

void icon_windows_image_deinit(icon_windows_image_t *image)
{
    if (image == NULL) {
        return;
    }
    if (image->rgba == NULL) {
        if (image_is_empty(image)) {
            memset(image, 0, sizeof(*image));
        }
        return;
    }
    if (image->allocator.allocate == NULL || image->allocator.deallocate == NULL ||
        image->allocation_size == 0u) {
        return;
    }
    image->allocator.deallocate(image->allocator.context, image->rgba,
                                image->allocation_size);
    memset(image, 0, sizeof(*image));
}
