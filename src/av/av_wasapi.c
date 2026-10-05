#define COBJMACROS
#include "av_wasapi.h"
#include <avrt.h>
#include <mmdeviceapi.h>
#include <string.h>

/* The activation blob is an OS ABI, not a vendored SDK header. Windows build
 * 20348: three uint32 fields: PROCESS_LOOPBACK=1, PID, INCLUDE_TREE=0. */
/** @brief Exact OS activation blob, local immutable during async submission. */
typedef struct process_loopback_params_t {
    uint32_t activation_type;
    uint32_t target_process_id;
    uint32_t loopback_mode;
} process_loopback_params_t;
_Static_assert(sizeof(process_loopback_params_t) == 12, "activation blob ABI");
static const GUID IidAgileObject = {
    0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
/** @brief Independently ref-counted callback; HeapAlloc owner is final release.
 * Completion event orders result/client writes before waiting capture thread.
 * API references keep this object alive even after activation timeout.
 */
typedef struct activation_t {
    IActivateAudioInterfaceCompletionHandler handler;
    LONG references;
    HANDLE complete;
    HRESULT result;
    IAudioClient *client;
    process_loopback_params_t params;
    PROPVARIANT variant;
} activation_t;
static ULONG STDMETHODCALLTYPE activation_add_ref(IActivateAudioInterfaceCompletionHandler *self) {
    activation_t *activation = (activation_t *)self;
    return (ULONG)InterlockedIncrement(&activation->references);
}
static ULONG STDMETHODCALLTYPE activation_release(IActivateAudioInterfaceCompletionHandler *self) {
    activation_t *activation = (activation_t *)self;
    ULONG count = (ULONG)InterlockedDecrement(&activation->references);
    if (!count) {
        if (activation->client) {
            IAudioClient_Release(activation->client);
            activation->client = NULL;
        }
        CloseHandle(activation->complete);
        activation->complete = NULL;
        HeapFree(GetProcessHeap(), 0, activation);
        activation = NULL;
    }
    return count;
}
static HRESULT STDMETHODCALLTYPE activation_query(IActivateAudioInterfaceCompletionHandler *self,
                                                  REFIID iid, void **output) {
    if (!output)
        return E_POINTER;
    *output = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) &&
        !IsEqualIID(iid, &IID_IActivateAudioInterfaceCompletionHandler) &&
        !IsEqualIID(iid, &IidAgileObject))
        return E_NOINTERFACE;
    *output = self;
    activation_add_ref(self);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE
activation_complete(IActivateAudioInterfaceCompletionHandler *self,
                    IActivateAudioInterfaceAsyncOperation *operation) {
    activation_t *activation = (activation_t *)self;
    IUnknown *unknown = NULL;
    HRESULT result = E_UNEXPECTED;
    HRESULT call =
        IActivateAudioInterfaceAsyncOperation_GetActivateResult(operation, &result, &unknown);
    activation->result = FAILED(call) ? call : result;
    if (SUCCEEDED(activation->result) && unknown)
        activation->result =
            IUnknown_QueryInterface(unknown, &IID_IAudioClient, (void **)&activation->client);
    else if (SUCCEEDED(activation->result))
        activation->result = E_UNEXPECTED;
    if (unknown)
        IUnknown_Release(unknown);
    SetEvent(activation->complete);
    return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl ActivationVtable = {
    activation_query, activation_add_ref, activation_release, activation_complete};
void av_wasapi_free(av_wasapi_t *audio) {
    if (audio->client)
        IAudioClient_Stop(audio->client);
    if (audio->capture) {
        IAudioCaptureClient_Release(audio->capture);
        audio->capture = NULL;
    }
    if (audio->client) {
        IAudioClient_Release(audio->client);
        audio->client = NULL;
    }
    if (audio->ready_event) {
        CloseHandle(audio->ready_event);
        audio->ready_event = NULL;
    }
    if (audio->mmcss) {
        AvRevertMmThreadCharacteristics(audio->mmcss);
        audio->mmcss = NULL;
    }
    memset(audio, 0, sizeof(*audio));
}
HRESULT av_wasapi_init(av_wasapi_t *audio, DWORD process_id) {
    if (!process_id)
        return E_INVALIDARG;
    activation_t *activation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*activation));
    if (!activation)
        return E_OUTOFMEMORY;
    activation->handler.lpVtbl = &ActivationVtable;
    activation->references = 1;
    activation->result = E_UNEXPECTED;
    activation->complete = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!activation->complete) {
        HeapFree(GetProcessHeap(), 0, activation);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    activation->params.activation_type = 1;
    activation->params.target_process_id = process_id;
    activation->variant.vt = VT_BLOB;
    activation->variant.blob.cbSize = sizeof(activation->params);
    activation->variant.blob.pBlobData = (BYTE *)&activation->params;
    IActivateAudioInterfaceAsyncOperation *operation = NULL;
    HRESULT result =
        ActivateAudioInterfaceAsync(L"VAD\\Process_Loopback", &IID_IAudioClient,
                                    &activation->variant, &activation->handler, &operation);
    if (FAILED(result))
        goto cleanup;
    DWORD wait = WaitForSingleObject(activation->complete, 10000);
    if (wait != WAIT_OBJECT_0) {
        result = HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        goto cleanup;
    }
    result = activation->result;
    if (FAILED(result))
        goto cleanup;
    audio->client = activation->client;
    activation->client = NULL;
    WAVEFORMATEX format = {.wFormatTag = WAVE_FORMAT_PCM,
                           .nChannels = 2,
                           .nSamplesPerSec = 48000,
                           .nAvgBytesPerSec = 192000,
                           .nBlockAlign = 4,
                           .wBitsPerSample = 16,
                           .cbSize = 0};
    result = IAudioClient_Initialize(
        audio->client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, NULL);
    if (FAILED(result))
        goto cleanup;
    result =
        IAudioClient_GetService(audio->client, &IID_IAudioCaptureClient, (void **)&audio->capture);
    if (FAILED(result))
        goto cleanup;
    audio->ready_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!audio->ready_event) {
        result = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    result = IAudioClient_SetEventHandle(audio->client, audio->ready_event);
    if (FAILED(result))
        goto cleanup;
    DWORD task_index = 0;
    audio->mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    result = IAudioClient_Start(audio->client);
cleanup:
    if (operation)
        IActivateAudioInterfaceAsyncOperation_Release(operation);
    activation_release(&activation->handler);
    activation = NULL;
    if (FAILED(result))
        av_wasapi_free(audio);
    return result;
}
HRESULT av_wasapi_drain(av_wasapi_t *audio, audio_ring_header_t *ring, uint8_t *pcm,
                        size_t pcm_len) {
    UINT32 packet = 0;
    HRESULT result = IAudioCaptureClient_GetNextPacketSize(audio->capture, &packet);
    while (SUCCEEDED(result) && packet) {
        BYTE *data = NULL;
        UINT32 frames = 0;
        DWORD flags = 0;
        result = IAudioCaptureClient_GetBuffer(audio->capture, &data, &frames, &flags, NULL, NULL);
        if (FAILED(result))
            return result;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
            ++audio->discontinuities;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            const uint8_t silence[1024] = {0};
            UINT32 remaining = frames;
            while (remaining) {
                UINT32 count = remaining > 256 ? 256 : remaining;
                if (av_audio_write(ring, pcm, pcm_len, silence, (size_t)count * 4) < 0) {
                    result = E_INVALIDARG;
                    break;
                }
                remaining -= count;
            }
        } else if (!data || av_audio_write(ring, pcm, pcm_len, data, (size_t)frames * 4) < 0)
            result = E_INVALIDARG;
        HRESULT release = IAudioCaptureClient_ReleaseBuffer(audio->capture, frames);
        if (FAILED(result))
            return result;
        if (FAILED(release))
            return release;
        audio->captured_frames += frames;
        result = IAudioCaptureClient_GetNextPacketSize(audio->capture, &packet);
    }
    return result;
}
