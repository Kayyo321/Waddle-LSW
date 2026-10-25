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

    icon_windows_image_t nonempty_without_pixels = {0};
    nonempty_without_pixels.stride = 4u;
    icon_windows_image_deinit(&nonempty_without_pixels);
    assert(nonempty_without_pixels.stride == 4u);
}
