/** @file av_guest.c @brief Native per-game IVSHMEM/Viosock audio-video agent. */
#include <winsock2.h>

#include "av_capture.h"
#include "av_guest_setup.h"
#include "av_ivshmem.h"
#include "av_layout.h"
#include "av_peer.h"
#include "av_wasapi.h"
#include "av_windows.h"
#include <avrt.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
/** @brief Viosock address ABI, native-endian, caller-owned no pointers. */
typedef struct av_vsock_address_t {
    uint16_t family, reserved;
    uint32_t port, cid;
} av_vsock_address_t;
_Static_assert(sizeof(av_vsock_address_t) == 12, "Viosock address ABI");
/** @brief Event-thread-owned per-window capture and pool lease. */
typedef struct guest_window_t {
    av_message_t geometry;
    av_capture_t capture;
    uint64_t sequence;
    int active;
} guest_window_t;
/** @brief Session owns workers/events; joined before borrowed mapping is released. */
typedef struct guest_av_t {
    av_peer_t peer;
    av_ivshmem_t *memory;
    guest_window_t windows[AvMaxWindows];
    HANDLE stop, audio_ready, audio_thread;
    DWORD process_id;
    HRESULT audio_status;
    int failed;
} guest_av_t;
static int pool_free(guest_av_t *session, unsigned pool) {
    for (unsigned i = 0; i < AvVideoBuffers; ++i) {
        window_slot_header_t *slot =
            av_layout_slot(session->memory->mapping, session->memory->length, pool, i);
        if (atomic_load_explicit(&slot->slot_state, memory_order_acquire) != SlotFree)
            return 0;
    }
    return 1;
}
static int window_notification(const av_message_t *message, void *context) {
    guest_av_t *session = context;
    unsigned pool = AvMaxWindows;
    for (unsigned i = 0; i < AvMaxWindows; ++i)
        if (session->windows[i].active &&
            session->windows[i].geometry.window_id == message->window_id) {
            pool = i;
            break;
        }
    if (message->type == MsgWindowCreate) {
        for (unsigned i = 0; i < AvMaxWindows; ++i)
            if (!session->windows[i].active && pool_free(session, i)) {
                pool = i;
                break;
            }
    }
    if (pool == AvMaxWindows) {
        session->failed = 1;
        return -1;
    }
    guest_window_t *window = &session->windows[pool];
    if (message->type == MsgWindowDestroy) {
        window->active = 0;
        av_capture_free(&window->capture);
    } else {
        window->active = 1;
        window->geometry = *message;
        window->geometry.buffer_index = pool;
    }
    av_message_t outgoing = *message;
    if (outgoing.type == MsgWindowCreate)
        outgoing.buffer_index = pool;
    if (av_peer_send(&session->peer, &outgoing) != 0) {
        session->failed = 1;
        return -1;
    }
    return 0;
}
static int host_request(const av_message_t *message, void *context) {
    (void)context;
    if (message->type != MsgWindowGeometry && message->type != MsgWindowClose)
        return -1;
    return av_windows_apply(message);
}
static DWORD WINAPI audio_worker(void *context) {
    guest_av_t *session = context;
    av_wasapi_t audio = {0};
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    session->audio_status =
        FAILED(initialized) ? initialized : av_wasapi_init(&audio, session->process_id);
    SetEvent(session->audio_ready);
    if (SUCCEEDED(session->audio_status)) {
        uint8_t *base = session->memory->mapping;
        audio_ring_header_t *ring = (audio_ring_header_t *)(base + AvAudioHeaderOffset);
        HANDLE events[2] = {session->stop, audio.ready_event};
        ULONGLONG last_log = GetTickCount64();
        for (;;) {
            DWORD wait = WaitForMultipleObjects(2, events, FALSE, 1000);
            if (wait == WAIT_OBJECT_0)
                break;
            if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_TIMEOUT) {
                HRESULT result = av_wasapi_drain(&audio, ring, base + AvPcmOffset,
                                                 AvAudioCapacity * AvAudioFrameBytes);
                if (FAILED(result)) {
                    SetEvent(session->stop);
                    break;
                }
                if (GetTickCount64() - last_log >= 5000) {
                    uint32_t queued =
                        atomic_load_explicit(&ring->write_head, memory_order_acquire) -
                        atomic_load_explicit(&ring->read_head, memory_order_acquire);
                    fprintf(stderr, "AV audio: backlog=%.2fms dropped=%u discontinuities=%u\n",
                            (double)queued * 1000.0 / AvSampleRate,
                            atomic_load_explicit(&ring->overrun_frames, memory_order_acquire),
                            audio.discontinuities);
                    last_log = GetTickCount64();
                }
            } else {
                SetEvent(session->stop);
                break;
            }
        }
    }
    av_wasapi_free(&audio);
    if (SUCCEEDED(initialized))
        CoUninitialize();
    return 0;
}
static void capture_windows(guest_av_t *session) {
    for (unsigned pool = 0; pool < AvMaxWindows; ++pool) {
        guest_window_t *window = &session->windows[pool];
        if (!window->active || (window->geometry.flags & AvWindowMinimized))
            continue;
        if (!window->capture.device && !window->capture.wgc) {
            HRESULT result =
                av_capture_init(&window->capture, (HWND)(uintptr_t)window->geometry.window_id);
            if (FAILED(result)) {
                session->failed = 1;
                return;
            }
        }
        RECT bounds = {.left = window->geometry.x,
                       .top = window->geometry.y,
                       .right = window->geometry.x + (LONG)window->geometry.width,
                       .bottom = window->geometry.y + (LONG)window->geometry.height};
        for (unsigned index = 0; index < AvVideoBuffers; ++index) {
            window_slot_header_t *slot =
                av_layout_slot(session->memory->mapping, session->memory->length, pool, index);
            if (atomic_load_explicit(&slot->slot_state, memory_order_acquire) != SlotFree)
                continue;
            window->capture.sequence = window->sequence;
            uint8_t *pixels = (uint8_t *)session->memory->mapping + av_layout_pixels(pool, index);
            HRESULT result =
                av_capture_frame(&window->capture, &bounds, slot, pixels, AvSlotCapacity);
            if (result == S_OK) {
                window->sequence = slot->frame_sequence;
                av_message_t frame = window->geometry;
                frame.type = MsgFrameReady;
                frame.buffer_index = index;
                frame.sequence = slot->frame_sequence;
                frame.width = slot->width;
                frame.height = slot->height;
                frame.damage_width = slot->width;
                frame.damage_height = slot->height;
                int queued = av_peer_send(&session->peer, &frame);
                if (queued != 0) {
                    av_video_cancel(slot);
                    if (queued < 0)
                        session->failed = 1;
                }
            } else if (FAILED(result)) {
                av_capture_free(&window->capture); /* Retry device/output loss next tick. */
            }
            break;
        }
    }
}
static int guest_session(SOCKET socket, av_ivshmem_t *memory, DWORD process_id, HANDLE target) {
    guest_av_t session = {
        .peer = {.socket = (uintptr_t)socket}, .memory = memory, .process_id = process_id};
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(com))
        return -1;
    int result = -1;
    session.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    session.audio_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!session.stop || !session.audio_ready)
        goto cleanup;
    session.audio_thread = CreateThread(NULL, 0, audio_worker, &session, 0, NULL);
    if (!session.audio_thread)
        goto cleanup;
    if (WaitForSingleObject(session.audio_ready, 15000) != WAIT_OBJECT_0 ||
        FAILED(session.audio_status)) {
        fprintf(stderr, "AV guest: per-process audio activation failed\n");
        goto cleanup;
    }
    if (av_windows_start(process_id, window_notification, &session) != 0)
        goto cleanup;
    DWORD task_index = 0;
    HANDLE priority = AvSetMmThreadCharacteristicsW(L"Games", &task_index);
    while (!session.failed && WaitForSingleObject(session.stop, 0) != WAIT_OBJECT_0) {
        if (WaitForSingleObject(target, 0) == WAIT_OBJECT_0) {
            result = 0;
            break;
        }
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                session.failed = 1;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        int peer_status = av_peer_pump(&session.peer, host_request, &session);
        if (peer_status != 0) {
            result = peer_status == 1 ? 0 : -1;
            break;
        }
        capture_windows(&session);
        MsgWaitForMultipleObjectsEx(1, &session.stop, 7, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (priority)
        AvRevertMmThreadCharacteristics(priority);
cleanup:
    av_windows_stop();
    if (session.stop)
        SetEvent(session.stop);
    if (session.audio_thread) {
        WaitForSingleObject(session.audio_thread, INFINITE);
        CloseHandle(session.audio_thread);
    }
    for (unsigned i = 0; i < AvMaxWindows; ++i)
        av_capture_free(&session.windows[i].capture);
    if (session.audio_ready)
        CloseHandle(session.audio_ready);
    if (session.stop)
        CloseHandle(session.stop);
    CoUninitialize();
    return result;
}
static int vsock_family(void) {
    HANDLE device = CreateFileW(L"\\\\.\\Viosock", GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE)
        return -1;
    DWORD family = 0, returned = 0;
    BOOL valid =
        DeviceIoControl(device, 0x0801300c, NULL, 0, &family, sizeof(family), &returned, NULL);
    CloseHandle(device);
    return valid && returned == sizeof(family) && family && family <= UINT16_MAX ? (int)family : -1;
}
/** @brief Own one per-game guest AV session, including driver mapping and workers.
 * @param[in] argc CRT argument count. @param[in] argv Borrowed NUL-terminated args.
 * @return 0 successful peer teardown/help, 2 invalid PID, 1 native setup/capture error.
 * @note Main owns listener/Winsock/IVSHMEM; audio joins before mapping release.
 */
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("waddle-guest-av.exe PROCESS_ID | --probe | --setup DRIVER_DIRECTORY");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--probe"))
        return av_guest_probe();
    if (argc == 3 && !strcmp(argv[1], "--setup"))
        return av_guest_setup(argv[2]);
    uint32_t process_id = 0;
    if (argc != 2 || av_number_parse(argv[1], strlen(argv[1]), &process_id) != 0)
        return 2;
    /* Retain a synchronization handle to the actual process object: a numeric
     * PID alone can disappear or be reused while the session is running. */
    HANDLE target = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!target || WaitForSingleObject(target, 0) != WAIT_TIMEOUT) {
        if (target) CloseHandle(target);
        fputs("AV guest: target process is absent, inaccessible or already exited\n", stderr);
        return 2;
    }
    av_ivshmem_t memory = {0};
    WSADATA startup;
    if (av_ivshmem_init(&memory, 0) != 0) {
        fputs("AV guest: IVSHMEM driver/mapping not ready\n", stderr);
        CloseHandle(target);
        return 1;
    }
    if (WSAStartup(MAKEWORD(2, 2), &startup) != 0) {
        av_ivshmem_free(&memory);
        CloseHandle(target);
        return 1;
    }
    int family = vsock_family(), result = 1;
    SOCKET listener = family < 0
                          ? INVALID_SOCKET
                          : WSASocketW(family, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    SOCKET peer = INVALID_SOCKET;
    if (listener == INVALID_SOCKET)
        goto cleanup;
    av_vsock_address_t address = {(uint16_t)family, 0, AvControlPort, UINT32_MAX};
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
        goto cleanup;
    puts("AV guest: ready");
    fflush(stdout);
    peer = accept(listener, NULL, NULL);
    if (peer == INVALID_SOCKET)
        goto cleanup;
    u_long nonblocking = 1;
    if (ioctlsocket(peer, FIONBIO, &nonblocking) != 0)
        goto cleanup;
    result = guest_session(peer, &memory, process_id, target) == 0 ? 0 : 1;
cleanup:
    if (peer != INVALID_SOCKET)
        closesocket(peer);
    if (listener != INVALID_SOCKET)
        closesocket(listener);
    WSACleanup();
    av_ivshmem_free(&memory);
    CloseHandle(target);
    return result;
}
