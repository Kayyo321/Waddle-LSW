/** @file icon_windows_faults.inc @brief Native cleanup failure fixtures. */

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
}
