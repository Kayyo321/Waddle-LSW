/** @file slot_runtime_abi.c @brief Opaque slot-runtime C ABI regression fixture. */
#include "interop_slot_runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void fill_view(waddle_slot_view_t *view) {
    memset(view, 0xa5, sizeof(*view));
}

static void exercise_runtime(uint64_t epoch) {
    waddle_slot_runtime_handle_t *handle = NULL;
    assert(waddle_slot_runtime_create(epoch, &handle) == WaddleSlotOk);
    assert(handle != NULL);

    waddle_slot_runtime_handle_t *saved_handle = handle;
    assert(waddle_slot_runtime_destroy(&handle) == WaddleSlotInvalidLifecycle);
    assert(handle == saved_handle);
    assert(waddle_slot_runtime_request_geometry(NULL, epoch, 1, 1, 64, 64) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_request_geometry((waddle_slot_runtime_handle_t *)((uintptr_t)handle + 1), epoch, 1, 1, 64, 64) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_request_geometry((waddle_slot_runtime_handle_t *)UINTPTR_MAX, epoch, 1, 1, 64, 64) == WaddleSlotInvalidArgument);

    waddle_slot_view_t views[3];
    memset(views, 0x5a, sizeof(views));
    unsigned char views_before[sizeof(views)];
    memcpy(views_before, views, sizeof(views));
    assert(waddle_slot_runtime_admit_window(handle, epoch + 1, 100, 1, 64, 64, views) == WaddleSlotWrongSessionEpoch);
    assert(memcmp(views, views_before, sizeof(views)) == 0);
    assert(waddle_slot_runtime_admit_window(handle, epoch, 100, 1, 64, 64, NULL) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_admit_window(handle, epoch, 100, 1, 64, 64, (waddle_slot_view_t *)((uintptr_t)views + 1)) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_admit_window(handle, epoch, 100, 1, 64, 64, (waddle_slot_view_t *)handle) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_admit_window(handle, epoch, 100, 1, 64, 64, (waddle_slot_view_t *)(UINTPTR_MAX - 7)) == WaddleSlotInvalidArgument);
    assert(memcmp(views, views_before, sizeof(views)) == 0);

    assert(waddle_slot_runtime_admit_window(handle, epoch, 100, 1, 64, 64, views) == WaddleSlotOk);
    assert(views[0].descriptor_index == 0 && views[1].descriptor_index == 1 && views[2].descriptor_index == 2);
    assert(views[0].window == 100 && views[0].generation == 1);
    assert(views[0].width == 64 && views[0].height == 64 && views[0].pixel_bytes == 64u * 64u * 4u);
    assert(views[0].data_bytes >= views[0].pixel_bytes);

    waddle_slot_view_t output;
    fill_view(&output);
    unsigned char output_before[sizeof(output)];
    memcpy(output_before, &output, sizeof(output));
    assert(waddle_slot_runtime_prepare_publish(handle, epoch + 1, 100, 1, 0, 1, &output) == WaddleSlotWrongSessionEpoch);
    assert(memcmp(&output, output_before, sizeof(output)) == 0);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 3, 1, &output) == WaddleSlotInvalidSlot);
    assert(memcmp(&output, output_before, sizeof(output)) == 0);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 0, 0, &output) == WaddleSlotInvalidSequence);
    assert(memcmp(&output, &output_before, sizeof(output)) == 0);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 0, 1, NULL) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 0, 1, (waddle_slot_view_t *)((uintptr_t)&output + 1)) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 0, 1, (waddle_slot_view_t *)handle) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 0, 1, (waddle_slot_view_t *)(UINTPTR_MAX - 7)) == WaddleSlotInvalidArgument);
    assert(memcmp(&output, &output_before, sizeof(output)) == 0);

    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 0, 1, &output) == WaddleSlotOk);
    assert(output.descriptor_index == 0 && output.data_offset == views[0].data_offset);
    assert(waddle_slot_runtime_mark_writer_entered(handle, epoch + 1, 100, 1, 0, 1) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_commit_publish(handle, epoch + 1, 100, 1, 0, 1) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_cancel_unentered(handle, epoch + 1, 100, 1, 0, 1) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_mark_writer_entered(handle, epoch, 100, 1, 0, 1) == WaddleSlotOk);
    assert(waddle_slot_runtime_commit_publish(handle, epoch, 100, 1, 0, 1) == WaddleSlotOk);
    assert(waddle_slot_runtime_release_frame(handle, epoch, 100, 1, 0, 2) == WaddleSlotWrongRelease);

    assert(waddle_slot_runtime_request_geometry(handle, epoch + 1, 100, 1, 2000, 1200) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_request_geometry(handle, epoch, 100, 1, 2000, 1200) == WaddleSlotOk);
    assert(waddle_slot_runtime_release_frame(handle, epoch, 100, 1, 0, 1) == WaddleSlotOk);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 1, 2, &output) == WaddleSlotOk);
    assert(output.data_bytes >= views[1].data_bytes);
    assert(waddle_slot_runtime_cancel_unentered(handle, epoch, 100, 1, 1, 2) == WaddleSlotOk);

    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 2, 3, &output) == WaddleSlotOk);
    assert(waddle_slot_runtime_mark_writer_entered(handle, epoch, 100, 1, 2, 3) == WaddleSlotOk);
    assert(waddle_slot_runtime_cancel_entered(handle, epoch + 1, 100, 1, 2, 3) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_cancel_entered(handle, epoch, 100, 1, 2, 3) == WaddleSlotOk);
    assert(waddle_slot_runtime_retire_window(handle, epoch + 1, 100, 1) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_retire_window(handle, epoch, 100, 1) == WaddleSlotOk);
    assert(waddle_slot_runtime_prepare_publish(handle, epoch, 100, 1, 0, 4, &output) == WaddleSlotUnknownWindow);

    assert(waddle_slot_runtime_begin_drain(handle, epoch, 2) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_begin_drain(handle, epoch + 1, 0) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_begin_drain(handle, epoch, 0) == WaddleSlotMissingProof);
    assert(waddle_slot_runtime_begin_drain(handle, epoch, 1) == WaddleSlotOk);
    assert(waddle_slot_runtime_confirm_host_drained(handle, epoch, 2) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_confirm_host_drained(handle, epoch + 1, 1) == WaddleSlotWrongSessionEpoch);
    assert(waddle_slot_runtime_confirm_host_drained(handle, epoch, 0) == WaddleSlotMissingProof);
    assert(waddle_slot_runtime_destroy(&handle) == WaddleSlotInvalidLifecycle);
    assert(handle == saved_handle);
    assert(waddle_slot_runtime_confirm_host_drained(handle, epoch, 1) == WaddleSlotOk);
    assert(waddle_slot_runtime_destroy(&handle) == WaddleSlotOk);
    assert(handle == NULL);
    assert(waddle_slot_runtime_destroy(&handle) == WaddleSlotOk);
}

int main(void) {
    waddle_slot_runtime_handle_t *sentinel = (waddle_slot_runtime_handle_t *)(uintptr_t)0x1234;
    assert(waddle_slot_runtime_create(7, NULL) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_create(7, (waddle_slot_runtime_handle_t **)((uintptr_t)&sentinel + 1)) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_create(7, (waddle_slot_runtime_handle_t **)(UINTPTR_MAX - 3)) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_create(0, &sentinel) == WaddleSlotInvalidArgument);
    assert(sentinel == (waddle_slot_runtime_handle_t *)(uintptr_t)0x1234);
    sentinel = NULL;
    assert(waddle_slot_runtime_create(0, &sentinel) == WaddleSlotInvalidSessionEpoch);
    assert(sentinel == NULL);
    assert(waddle_slot_runtime_destroy(NULL) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_destroy((waddle_slot_runtime_handle_t **)((uintptr_t)&sentinel + 1)) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_destroy((waddle_slot_runtime_handle_t **)(UINTPTR_MAX - 3)) == WaddleSlotInvalidArgument);
    assert(waddle_slot_runtime_destroy(&sentinel) == WaddleSlotOk);
    for (uint64_t epoch = 100; epoch < 104; epoch++) exercise_runtime(epoch);
    puts("opaque slot runtime ABI tests passed");
    return 0;
}
