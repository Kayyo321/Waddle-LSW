/** @file icon_windows_faults.inc @brief Native cleanup failure fixtures. */

static HICON icon_windows_create_test_icon(void)
{
    HBITMAP mask_bitmap = CreateBitmap(16, 32, 1u, 1u, NULL);
    assert(mask_bitmap != NULL);
    ICONINFO icon_info;
    memset(&icon_info, 0, sizeof(icon_info));
    icon_info.fIcon = TRUE;
    icon_info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);
    assert(DeleteObject(mask_bitmap) != 0);
    return icon;
}

static void icon_windows_run_private_helper_tests(void)
{
    uintptr_t start = 0u;
    uintptr_t end = 0u;
    assert(!pointer_range(NULL, 1u, &start, &end));
    assert(!pointer_range(&start, 0u, &start, &end));
    assert(pointer_range(&start, sizeof(start), &start, &end));
    assert(end > start);
    assert(!pointer_range((const void *)(UINTPTR_MAX - 3u), 8u, &start, &end));
    assert(!ranges_overlap(1u, 2u, 2u, 3u));
    assert(ranges_overlap(1u, 3u, 2u, 4u));
    assert(!ranges_overlap(3u, 4u, 1u, 2u));
    assert(fallback_path_length(NULL) == 0u);
    assert(fallback_path_length(L"") == 0u);
    assert(fallback_path_length(L"icon") == 4u);
    wchar_t *long_path = malloc((IconWindowsMaxFallbackPathUnits + 1u) * sizeof(wchar_t));
    assert(long_path != NULL);
    for (size_t index = 0u; index <= IconWindowsMaxFallbackPathUnits; ++index) {
        long_path[index] = L'x';
    }
    long_path[IconWindowsMaxFallbackPathUnits] = L'\0';
    assert(fallback_path_length(long_path) == IconWindowsMaxFallbackPathUnits);
    long_path[IconWindowsMaxFallbackPathUnits] = L'x';
    assert(fallback_path_length(long_path) == SIZE_MAX);
    free(long_path);
    static const uint32_t SupportedDimensions[] = {16u, 24u, 32u, 48u, 64u, 128u, 256u};
    for (size_t index = 0u; index < sizeof(SupportedDimensions) /
                                         sizeof(SupportedDimensions[0]); ++index) {
        assert(supported_dimension(SupportedDimensions[index]));
    }
    assert(!supported_dimension(17u));

    icon_windows_image_t image = {0};
    assert(image_is_empty(&image));
    image.rgba = (uint8_t *)&image;
    assert(!image_is_empty(&image));
    memset(&image, 0, sizeof(image));
    image.stride = 1u;
    assert(!image_is_empty(&image));
    memset(&image, 0, sizeof(image));
    image.width = 1u;
    assert(!image_is_empty(&image));
    memset(&image, 0, sizeof(image));
    image.height = 1u;
    assert(!image_is_empty(&image));
    memset(&image, 0, sizeof(image));
    image.allocation_size = 1u;
    assert(!image_is_empty(&image));
    memset(&image, 0, sizeof(image));
    image.allocator.context = &image;
    assert(!image_is_empty(&image));
    memset(&image, 0, sizeof(image));
    image.allocator.allocate = test_allocate;
    assert(!image_is_empty(&image));
    memset(&image, 0, sizeof(image));
    image.allocator.deallocate = test_deallocate;
    assert(!image_is_empty(&image));

    icon_windows_cleanup_t empty_cleanup = {0};
    assert(cleanup_is_empty(&empty_cleanup));
    empty_cleanup.bitmap_handles[0] = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.bitmap_handles[1] = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.bitmap_handles[2] = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.bitmap_handles[3] = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.icon_handles[0] = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.device_context_handles[0] = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.bitmap_count = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.icon_count = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.device_context_count = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.operation_status = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.reserved_zero = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.operation_error = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));
    memset(&empty_cleanup, 0, sizeof(empty_cleanup));
    empty_cleanup.last_error = 1u;
    assert(!cleanup_is_empty(&empty_cleanup));

    icon_windows_cleanup_t cleanup = {0};
    HBITMAP bitmaps[5] = {0};
    keep_bitmap(&cleanup, NULL);
    for (size_t index = 0u; index < 5u; ++index) {
        void *pixels = NULL;
        bitmaps[index] = create_color_bitmap(1u, &pixels);
        assert(bitmaps[index] != NULL && pixels != NULL);
        keep_bitmap(&cleanup, bitmaps[index]);
    }
    assert(cleanup.bitmap_count == 4u);
    assert(DeleteObject(bitmaps[4]) != 0);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);

    HICON icons[2] = {icon_windows_create_test_icon(), icon_windows_create_test_icon()};
    assert(icons[0] != NULL && icons[1] != NULL);
    keep_icon(&cleanup, NULL);
    keep_icon(&cleanup, icons[0]);
    keep_icon(&cleanup, icons[1]);
    assert(cleanup.icon_count == 1u && cleanup.icon_handles[0] == (uintptr_t)icons[0]);
    assert(DestroyIcon(icons[1]) != 0);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);

    HDC contexts[2] = {CreateCompatibleDC(NULL), CreateCompatibleDC(NULL)};
    assert(contexts[0] != NULL && contexts[1] != NULL);
    keep_dc(&cleanup, NULL);
    keep_dc(&cleanup, contexts[0]);
    keep_dc(&cleanup, contexts[1]);
    assert(cleanup.device_context_count == 1u &&
           cleanup.device_context_handles[0] == (uintptr_t)contexts[0]);
    assert(DeleteDC(contexts[1]) != 0);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);

    icon_windows_cleanup_t close_cleanup = {0};
    HBITMAP null_bitmap = NULL;
    HICON null_icon = NULL;
    HDC null_context = NULL;
    assert(close_bitmap(&null_bitmap, &close_cleanup));
    assert(close_icon(&null_icon, &close_cleanup));
    assert(close_dc(&null_context, &close_cleanup));
    void *failure_pixels = NULL;
    HBITMAP failure_bitmap = create_color_bitmap(1u, &failure_pixels);
    assert(failure_bitmap != NULL && failure_pixels != NULL);
    test_fail_delete_once = true;
    assert(!close_bitmap(&failure_bitmap, &close_cleanup));
    assert(close_cleanup.bitmap_count == 1u &&
           close_cleanup.bitmap_handles[0] != 0u && failure_bitmap != NULL);
    assert(icon_windows_cleanup_retry(&close_cleanup) == IconWindowsStatusOk);
    HICON failure_icon = icon_windows_create_test_icon();
    assert(failure_icon != NULL);
    test_fail_close_icon_once = true;
    assert(!close_icon(&failure_icon, &close_cleanup));
    assert(close_cleanup.icon_count == 1u && close_cleanup.icon_handles[0] != 0u &&
           failure_icon != NULL);
    assert(icon_windows_cleanup_retry(&close_cleanup) == IconWindowsStatusOk);
    HDC failure_context = CreateCompatibleDC(NULL);
    assert(failure_context != NULL);
    test_fail_close_dc_once = true;
    assert(!close_dc(&failure_context, &close_cleanup));
    assert(close_cleanup.device_context_count == 1u &&
           close_cleanup.device_context_handles[0] != 0u && failure_context != NULL);
    assert(icon_windows_cleanup_retry(&close_cleanup) == IconWindowsStatusOk);

    for (uint32_t target = 0u; target < 3u; ++target) {
        failure_pixels = NULL;
        failure_bitmap = create_color_bitmap(1u, &failure_pixels);
        assert(failure_bitmap != NULL && failure_pixels != NULL);
        HBITMAP shell_bitmap = target == 0u ? failure_bitmap : NULL;
        HBITMAP color_bitmap = target == 1u ? failure_bitmap : NULL;
        HBITMAP mask_bitmap = target == 2u ? failure_bitmap : NULL;
        test_fail_delete_once = true;
        assert(!close_locals(&shell_bitmap, &null_bitmap, &color_bitmap,
                             &mask_bitmap, &null_icon, &null_context,
                             &close_cleanup));
        assert(close_cleanup.bitmap_count == 1u &&
               close_cleanup.bitmap_handles[0] == (uintptr_t)failure_bitmap);
        assert(icon_windows_cleanup_retry(&close_cleanup) == IconWindowsStatusOk);
    }
    void *closing_pixels = NULL;
    HBITMAP closing_bitmap = create_color_bitmap(1u, &closing_pixels);
    assert(closing_bitmap != NULL && closing_pixels != NULL &&
           close_bitmap(&closing_bitmap, &close_cleanup));
    HICON closing_icon = icon_windows_create_test_icon();
    assert(closing_icon != NULL && close_icon(&closing_icon, &close_cleanup));
    HDC closing_context = CreateCompatibleDC(NULL);
    assert(closing_context != NULL && close_dc(&closing_context, &close_cleanup));
    assert(close_locals(&null_bitmap, &null_bitmap, &null_bitmap, &null_bitmap,
                        &null_icon, &null_context, &close_cleanup));

    HDC draw_context = CreateCompatibleDC(NULL);
    void *draw_pixels = NULL;
    HBITMAP draw_bitmap = create_color_bitmap(16u, &draw_pixels);
    HICON draw_icon = icon_windows_create_test_icon();
    assert(draw_context != NULL && draw_bitmap != NULL && draw_pixels != NULL &&
           draw_icon != NULL);
    DWORD native_error = ERROR_SUCCESS;
    test_fail_select_object_once = true;
    assert(!draw_icon_to_top_down_dib(draw_context, draw_bitmap, draw_icon, 16u,
                                      &native_error));
    assert(native_error == ERROR_INVALID_HANDLE);
    test_fail_draw_icon_once = true;
    assert(!draw_icon_to_top_down_dib(draw_context, draw_bitmap, draw_icon, 16u,
                                      &native_error));
    assert(native_error == ERROR_INVALID_HANDLE);
    test_fail_restore_object_once = true;
    assert(!draw_icon_to_top_down_dib(draw_context, draw_bitmap, draw_icon, 16u,
                                      &native_error));
    assert(native_error == ERROR_INVALID_HANDLE);
    assert(draw_icon_to_top_down_dib(draw_context, draw_bitmap, draw_icon, 16u,
                                     &native_error));

    uint8_t mask_pixels[16u * 16u * 4u] = {0};
    HBITMAP icon_color_bitmap = NULL;
    HBITMAP icon_mask_bitmap = NULL;
    test_fail_get_icon_info_once = true;
    assert(!apply_legacy_icon_mask(draw_context, draw_icon, 16u, mask_pixels,
                                   &icon_color_bitmap, &icon_mask_bitmap,
                                   &native_error));
    assert(native_error == ERROR_INVALID_HANDLE && icon_color_bitmap == NULL &&
           icon_mask_bitmap == NULL);
    test_force_icon_mask_null_once = true;
    assert(!apply_legacy_icon_mask(draw_context, draw_icon, 16u, mask_pixels,
                                   &icon_color_bitmap, &icon_mask_bitmap,
                                   &native_error));
    assert(native_error == ERROR_INVALID_DATA && icon_mask_bitmap != NULL);
    assert(DeleteObject(icon_mask_bitmap) != 0);
    icon_mask_bitmap = NULL;
    test_fail_mask_dibits_once = true;
    assert(!apply_legacy_icon_mask(draw_context, draw_icon, 16u, mask_pixels,
                                   &icon_color_bitmap, &icon_mask_bitmap,
                                   &native_error));
    assert(icon_mask_bitmap != NULL);
    assert(DeleteObject(icon_mask_bitmap) != 0);
    icon_mask_bitmap = NULL;
    assert(!apply_legacy_icon_mask(draw_context, draw_icon, 64u, mask_pixels,
                                   &icon_color_bitmap, &icon_mask_bitmap,
                                   &native_error));
    assert(native_error == ERROR_INVALID_DATA);
    assert(icon_mask_bitmap != NULL);
    assert(DeleteObject(icon_mask_bitmap) != 0);
    icon_mask_bitmap = NULL;
    HBITMAP malformed_source_mask = CreateBitmap(8, 8, 1u, 1u, NULL);
    assert(malformed_source_mask != NULL);
    ICONINFO malformed_icon_info;
    memset(&malformed_icon_info, 0, sizeof(malformed_icon_info));
    malformed_icon_info.fIcon = TRUE;
    malformed_icon_info.hbmMask = malformed_source_mask;
    HICON malformed_icon = CreateIconIndirect(&malformed_icon_info);
    assert(malformed_icon != NULL);
    assert(DeleteObject(malformed_source_mask) != 0);
    assert(!apply_legacy_icon_mask(draw_context, malformed_icon, 16u, mask_pixels,
                                   &icon_color_bitmap, &icon_mask_bitmap,
                                   &native_error));
    assert(native_error == ERROR_INVALID_DATA && icon_mask_bitmap != NULL);
    assert(DeleteObject(icon_mask_bitmap) != 0);
    assert(DestroyIcon(malformed_icon) != 0);

    BITMAPINFO copy_info;
    memset(&copy_info, 0, sizeof(copy_info));
    copy_info.bmiHeader.biSize = sizeof(copy_info.bmiHeader);
    copy_info.bmiHeader.biWidth = 16;
    copy_info.bmiHeader.biHeight = -16;
    copy_info.bmiHeader.biPlanes = 1u;
    copy_info.bmiHeader.biBitCount = 32u;
    copy_info.bmiHeader.biCompression = BI_RGB;
    bool source_has_alpha = false;
    assert(!copy_bitmap_to_top_down_dib(draw_context, draw_bitmap, 17u, 16u,
                                        mask_pixels, &copy_info, &source_has_alpha,
                                        &native_error));
    assert(native_error == ERROR_INVALID_DATA);
    assert(!copy_bitmap_to_top_down_dib(draw_context, draw_bitmap, 16u, 17u,
                                        mask_pixels, &copy_info, &source_has_alpha,
                                        &native_error));
    assert(native_error == ERROR_INVALID_DATA);
    test_fail_copy_dibits_once = true;
    assert(!copy_bitmap_to_top_down_dib(draw_context, draw_bitmap, 16u, 16u,
                                        mask_pixels, &copy_info, &source_has_alpha,
                                        &native_error));
    assert(native_error == ERROR_GEN_FAILURE);
    assert(DestroyIcon(draw_icon) != 0);
    assert(DeleteDC(draw_context) != 0);
    assert(DeleteObject(draw_bitmap) != 0);
}

static void icon_windows_run_fault_tests(void)
{
    icon_windows_cleanup_t invalid_cleanup = {0};
    assert(icon_windows_cleanup_retry(NULL) == IconWindowsStatusInvalidArgument);
    const uintptr_t unaligned_cleanup_address =
        (uintptr_t)&invalid_cleanup + 1u;
    assert(icon_windows_cleanup_retry(
               (icon_windows_cleanup_t *)unaligned_cleanup_address) ==
           IconWindowsStatusInvalidArgument);
    uintptr_t wrapping_cleanup_address =
        UINTPTR_MAX - sizeof(icon_windows_cleanup_t) + 1u;
    wrapping_cleanup_address -= wrapping_cleanup_address %
        _Alignof(icon_windows_cleanup_t);
    assert(icon_windows_cleanup_retry(
               (icon_windows_cleanup_t *)wrapping_cleanup_address) ==
           IconWindowsStatusInvalidArgument);

    invalid_cleanup.bitmap_count = 5u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.icon_count = 2u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.device_context_count = 2u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.icon_handles[0] = 1u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.device_context_handles[0] = 1u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.reserved_zero = 1u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.bitmap_handles[1] = 1u;
    invalid_cleanup.bitmap_count = 1u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.icon_count = 1u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);
    memset(&invalid_cleanup, 0, sizeof(invalid_cleanup));
    invalid_cleanup.device_context_count = 1u;
    assert(icon_windows_cleanup_retry(&invalid_cleanup) ==
           IconWindowsStatusInvalidArgument);

    void *selected_bits = NULL;
    HBITMAP selected_bitmap = create_color_bitmap(16u, &selected_bits);
    void *ordinary_bits = NULL;
    HBITMAP ordinary_bitmap = create_color_bitmap(16u, &ordinary_bits);
    assert(selected_bitmap != NULL && selected_bits != NULL);
    assert(ordinary_bitmap != NULL && ordinary_bits != NULL);

    HDC device_context = CreateCompatibleDC(NULL);
    assert(device_context != NULL);
    HGDIOBJ previous_bitmap = SelectObject(device_context, selected_bitmap);
    assert(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    icon_windows_cleanup_t cleanup = {0};
    cleanup.bitmap_handles[0] = (uintptr_t)selected_bitmap;
    cleanup.bitmap_handles[1] = (uintptr_t)ordinary_bitmap;
    cleanup.bitmap_count = 2u;

    /* Inject one native cleanup failure while retaining a genuine owned bitmap. */
    test_fail_cleanup_delete_once = true;
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusCleanupRequired);
    assert(cleanup.bitmap_count == 1u);
    assert(cleanup.bitmap_handles[0] == (uintptr_t)selected_bitmap);
    assert(cleanup.bitmap_handles[1] == 0u);
    assert(cleanup.last_error != ERROR_SUCCESS);

    assert(SelectObject(device_context, previous_bitmap) == selected_bitmap);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);
    assert(cleanup.bitmap_count == 0u && cleanup.bitmap_handles[0] == 0u &&
           cleanup.bitmap_handles[1] == 0u);
    assert(DeleteDC(device_context) != 0);

    HDC owned_dc = CreateCompatibleDC(NULL);
    HICON owned_icon = icon_windows_create_test_icon();
    assert(owned_dc != NULL && owned_icon != NULL);
    memset(&cleanup, 0, sizeof(cleanup));
    cleanup.device_context_handles[0] = (uintptr_t)owned_dc;
    cleanup.device_context_count = 1u;
    cleanup.icon_handles[0] = (uintptr_t)owned_icon;
    cleanup.icon_count = 1u;
    test_fail_cleanup_dc_once = true;
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusCleanupRequired);
    assert(cleanup.device_context_count == 1u &&
           cleanup.device_context_handles[0] == (uintptr_t)owned_dc);
    assert(cleanup.icon_count == 0u && cleanup.icon_handles[0] == 0u);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);

    owned_icon = icon_windows_create_test_icon();
    assert(owned_icon != NULL);
    memset(&cleanup, 0, sizeof(cleanup));
    cleanup.icon_handles[0] = (uintptr_t)owned_icon;
    cleanup.icon_count = 1u;
    test_fail_cleanup_icon_once = true;
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusCleanupRequired);
    assert(cleanup.icon_count == 1u && cleanup.icon_handles[0] == (uintptr_t)owned_icon);
    assert(icon_windows_cleanup_retry(&cleanup) == IconWindowsStatusOk);

    icon_windows_image_deinit(NULL);
    icon_windows_image_t empty_image = {0};
    icon_windows_image_deinit(&empty_image);
    assert(empty_image.rgba == NULL && empty_image.stride == 0u);

    allocator_state_t allocation_state = {0};
    icon_windows_image_t owned_image = {0};
    owned_image.rgba = test_allocate(&allocation_state, 4u);
    assert(owned_image.rgba != NULL);
    owned_image.allocation_size = 4u;
    owned_image.allocator.context = &allocation_state;
    owned_image.allocator.allocate = test_allocate;
    owned_image.allocator.deallocate = test_deallocate;
    icon_windows_image_deinit(&owned_image);
    assert(owned_image.rgba == NULL && allocation_state.allocations == 1u &&
           allocation_state.deallocations == 1u &&
           allocation_state.bytes_allocated == allocation_state.bytes_deallocated);

    uint8_t *caller_owned = malloc(4u);
    assert(caller_owned != NULL);
    icon_windows_image_t incomplete_image = {0};
    incomplete_image.rgba = caller_owned;
    incomplete_image.allocation_size = 4u;
    icon_windows_image_deinit(&incomplete_image);
    assert(incomplete_image.rgba == caller_owned && incomplete_image.allocation_size == 4u);
    free(caller_owned);

    caller_owned = malloc(4u);
    assert(caller_owned != NULL);
    incomplete_image = (icon_windows_image_t){0};
    incomplete_image.rgba = caller_owned;
    incomplete_image.allocation_size = 4u;
    incomplete_image.allocator.allocate = test_allocate;
    icon_windows_image_deinit(&incomplete_image);
    assert(incomplete_image.rgba == caller_owned);
    free(caller_owned);

    caller_owned = malloc(4u);
    assert(caller_owned != NULL);
    incomplete_image = (icon_windows_image_t){0};
    incomplete_image.rgba = caller_owned;
    incomplete_image.allocation_size = 0u;
    incomplete_image.allocator.allocate = test_allocate;
    incomplete_image.allocator.deallocate = test_deallocate;
    icon_windows_image_deinit(&incomplete_image);
    assert(incomplete_image.rgba == caller_owned && incomplete_image.allocation_size == 0u);
    free(caller_owned);

    icon_windows_image_t nonempty_without_pixels = {0};
    nonempty_without_pixels.stride = 4u;
    icon_windows_image_deinit(&nonempty_without_pixels);
    assert(nonempty_without_pixels.stride == 4u);
}

static void icon_windows_run_input_guard_tests(IShellItem *item,
                                                icon_windows_allocator_t allocator)
{
    icon_windows_image_t image = {0};
    icon_windows_cleanup_t cleanup = {0};
    assert(icon_windows_extract_rgba(NULL, NULL, 32u, allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(icon_windows_extract_rgba(item, NULL, 32u, allocator, NULL, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(icon_windows_extract_rgba(item, NULL, 32u, allocator, &image, NULL) ==
           IconWindowsStatusInvalidArgument);
    icon_windows_allocator_t invalid_allocator = allocator;
    invalid_allocator.allocate = NULL;
    assert(icon_windows_extract_rgba(item, NULL, 32u, invalid_allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    invalid_allocator = allocator;
    invalid_allocator.deallocate = NULL;
    assert(icon_windows_extract_rgba(item, NULL, 32u, invalid_allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);

    const uintptr_t unaligned_image_address = (uintptr_t)&image + 1u;
    assert(icon_windows_extract_rgba(item, NULL, 32u, allocator,
                                     (icon_windows_image_t *)unaligned_image_address,
                                     &cleanup) == IconWindowsStatusInvalidArgument);
    const uintptr_t unaligned_cleanup_address = (uintptr_t)&cleanup + 1u;
    assert(icon_windows_extract_rgba(item, NULL, 32u, allocator, &image,
                                     (icon_windows_cleanup_t *)unaligned_cleanup_address) ==
           IconWindowsStatusInvalidArgument);

    union {
        max_align_t alignment;
        unsigned char bytes[(IconWindowsMaxFallbackPathUnits + 1u) * sizeof(wchar_t) +
                            256u];
    } alias_arena;
    memset(&alias_arena, 0, sizeof(alias_arena));
    icon_windows_image_t *alias_image =
        (icon_windows_image_t *)(void *)(alias_arena.bytes + 128u);
    icon_windows_cleanup_t *alias_cleanup =
        (icon_windows_cleanup_t *)(void *)(alias_arena.bytes + 128u);
    assert(icon_windows_extract_rgba(item, NULL, 32u, allocator, alias_image,
                                     alias_cleanup) == IconWindowsStatusInvalidArgument);
    assert(icon_windows_extract_rgba((IShellItem *)(void *)alias_image, NULL, 32u,
                                     allocator, alias_image, &cleanup) ==
           IconWindowsStatusInvalidArgument);
    assert(icon_windows_extract_rgba((IShellItem *)(void *)&cleanup, NULL, 32u,
                                     allocator, &image, &cleanup) ==
           IconWindowsStatusInvalidArgument);

    wchar_t *path_in_image = (wchar_t *)(void *)alias_image;
    assert(icon_windows_extract_rgba(item, path_in_image, 32u, allocator, alias_image,
                                     &cleanup) == IconWindowsStatusInvalidArgument);
    wchar_t *path_near_image = (wchar_t *)(void *)((unsigned char *)alias_image - sizeof(wchar_t));
    path_near_image[0] = L'x';
    path_near_image[1] = L'x';
    path_near_image[2] = L'\0';
    assert(icon_windows_extract_rgba(item, path_near_image, 32u, allocator, alias_image,
                                     &cleanup) == IconWindowsStatusInvalidArgument);

    assert(image.rgba == NULL && cleanup.bitmap_count == 0u &&
           cleanup.icon_count == 0u && cleanup.device_context_count == 0u);
}
