/** @file venus_stream_windows.c @brief Overlapped VirtIO-Serial lifecycle I/O. */
#include "venus_stream.h"
#include <windows.h>

venus_ring_status_t venus_stream_open(venus_channel_t *channel) {
    HANDLE stream = (HANDLE)channel->stream;
    DWORD flags;
    if (!stream || stream == INVALID_HANDLE_VALUE || !GetHandleInformation(stream, &flags))
        return RingInvalid;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event)
        return RingInvalid;
    channel->event = (intptr_t)event;
    return RingOk;
}

void venus_stream_free(venus_channel_t *channel) {
    if (channel->event)
        CloseHandle((HANDLE)channel->event);
    channel->event = 0;
}

uint64_t venus_stream_time_ms(void) {
    uint64_t now = GetTickCount64();
    return now == UINT64_MAX ? 0 : now + 1;
}

venus_ring_status_t venus_stream_io(venus_channel_t *channel, void *buffer, size_t length,
                                    int writing, size_t *transferred) {
    *transferred = 0;
    HANDLE stream = (HANDLE)channel->stream;
    OVERLAPPED operation = {0};
    operation.hEvent = (HANDLE)channel->event;
    if (!ResetEvent(operation.hEvent))
        return RingClosed;
    DWORD bytes = 0;
    BOOL done = writing ? WriteFile(stream, buffer, (DWORD)length, &bytes, &operation)
                        : ReadFile(stream, buffer, (DWORD)length, &bytes, &operation);
    if (!done) {
        if (GetLastError() != ERROR_IO_PENDING)
            return RingClosed;
        DWORD waiting = WaitForSingleObject(operation.hEvent, 1);
        if (waiting != WAIT_OBJECT_0)
            (void)CancelIoEx(stream, &operation);
        done = GetOverlappedResult(stream, &operation, &bytes, TRUE);
        if (!done)
            return waiting == WAIT_TIMEOUT && GetLastError() == ERROR_OPERATION_ABORTED
                       ? RingAgain
                       : RingClosed;
        if (waiting != WAIT_OBJECT_0 && waiting != WAIT_TIMEOUT)
            return RingClosed;
    }
    if (!bytes)
        return RingClosed;
    *transferred = bytes;
    return RingOk;
}
