#include "av_pipewire.h"
#include <dlfcn.h>
#include <spa/param/audio/format-utils.h>
#include <string.h>
#include <errno.h>

static void playback_process(void *context) {
    av_pipewire_t *audio = context;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(audio->stream);
    if (!buffer)
        return;
    struct spa_buffer *spa_buffer = buffer->buffer;
    if (!spa_buffer || spa_buffer->n_datas != 1 || !spa_buffer->datas[0].data ||
        !spa_buffer->datas[0].chunk) {
        atomic_fetch_add_explicit(&audio->invalid_buffers, 1, memory_order_acq_rel);
        pw_stream_queue_buffer(audio->stream, buffer);
        return;
    }
    struct spa_data *data = &spa_buffer->datas[0];
    uint32_t frames = data->maxsize / AvAudioFrameBytes;
    if (buffer->requested && buffer->requested < frames)
        frames = (uint32_t)buffer->requested;
    size_t bytes = (size_t)frames * AvAudioFrameBytes;
    int64_t copied = av_audio_read(audio->ring, audio->pcm, audio->pcm_len, data->data, bytes,
                                   384); /* 8ms backlog ceiling. */
    if (copied < 0) {
        memset(data->data, 0, bytes);
        copied = 0;
        atomic_fetch_add_explicit(&audio->invalid_buffers, 1, memory_order_acq_rel);
    }
    atomic_fetch_add_explicit(&audio->underrun_frames, frames - (uint32_t)copied,
                              memory_order_acq_rel);
    data->chunk->offset = 0;
    data->chunk->stride = AvAudioFrameBytes;
    data->chunk->size = (uint32_t)bytes;
    buffer->size = frames;
    pw_stream_queue_buffer(audio->stream, buffer);
}
static void playback_state(void *context, enum pw_stream_state previous,
                           enum pw_stream_state state, const char *error) {
    (void)previous;
    (void)error;
    av_pipewire_t *audio = context;
    atomic_store_explicit(&audio->state, state, memory_order_release);
    pw_thread_loop_signal(audio->loop, false);
}
int av_pipewire_ready(const av_pipewire_t *audio) {
    int state = atomic_load_explicit(&audio->state, memory_order_acquire);
    return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
}
static const struct pw_stream_events PlaybackEvents = {.version = PW_VERSION_STREAM_EVENTS,
                                                       .process = playback_process,
                                                       .state_changed = playback_state};
void av_pipewire_free(av_pipewire_t *audio) {
    if (audio->loop)
        pw_thread_loop_stop(audio->loop);
    if (audio->stream) {
        pw_stream_disconnect(audio->stream);
        pw_stream_destroy(audio->stream);
        audio->stream = NULL;
    }
    if (audio->loop) {
        pw_thread_loop_destroy(audio->loop);
        audio->loop = NULL;
    }
    if (audio->library_initialized) {
        pw_deinit();
        audio->library_initialized = 0;
    }
    if (audio->bus_library) {
        /* RTKit closes its connections but libdbus retains process caches. Our
         * standalone host owns the only PipeWire/D-Bus lifecycle; release those
         * caches after all workers/modules stop, before the final library close. */
        typedef void (*av_bus_shutdown_t)(void);
        av_bus_shutdown_t shutdown_bus = NULL;
        void *procedure = dlsym(audio->bus_library, "dbus_shutdown");
        memcpy(&shutdown_bus, &procedure, sizeof(shutdown_bus));
        if (shutdown_bus) shutdown_bus();
        dlclose(audio->bus_library);
        audio->bus_library = NULL;
    }
    audio->ring = NULL;
    audio->pcm = NULL;
    audio->pcm_len = 0;
}
int av_pipewire_init(av_pipewire_t *audio, audio_ring_header_t *ring, const uint8_t *pcm,
                     size_t pcm_len) {
    if (ring->sample_rate != AvSampleRate || ring->channels != 2 || ring->format != 1 ||
        ring->capacity_frames < 2 || ring->capacity_frames > AvMaxAudioFrames ||
        (ring->capacity_frames & (ring->capacity_frames - 1)) ||
        pcm_len < (size_t)ring->capacity_frames * AvAudioFrameBytes)
        return -1;
    audio->ring = ring;
    audio->pcm = pcm;
    audio->pcm_len = pcm_len;
    atomic_init(&audio->underrun_frames, 0);
    atomic_init(&audio->invalid_buffers, 0);
    atomic_init(&audio->state, PW_STREAM_STATE_UNCONNECTED);
    pw_init(NULL, NULL);
    audio->library_initialized = 1;
    audio->loop = pw_thread_loop_new("waddle-av-audio", NULL);
    if (!audio->loop)
        goto fail;
    struct pw_properties *properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_NODE_NAME, "waddle-av-playback", PW_KEY_NODE_LATENCY, "128/48000", NULL);
    if (!properties)
        goto fail;
    /* pw_stream_new_simple takes ownership of properties, including failure. */
    audio->stream = pw_stream_new_simple(pw_thread_loop_get_loop(audio->loop), "Waddle game audio",
                                         properties, &PlaybackEvents, audio);
    audio->bus_library = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_NOLOAD);
    if (!audio->stream)
        goto fail;
    uint8_t storage[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    struct spa_audio_info_raw format = {.format = SPA_AUDIO_FORMAT_S16_LE,
                                        .rate = AvSampleRate,
                                        .channels = 2,
                                        .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}};
    const struct spa_pod *params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    if (pw_stream_connect(audio->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                              PW_STREAM_FLAG_RT_PROCESS,
                          params, 1) < 0 ||
        pw_thread_loop_start(audio->loop) < 0)
        goto fail;
    pw_thread_loop_lock(audio->loop);
    struct timespec deadline;
    int wait_status = pw_thread_loop_get_time(audio->loop, &deadline, 5000000000LL);
    while (!wait_status && !av_pipewire_ready(audio) &&
           atomic_load_explicit(&audio->state, memory_order_acquire) != PW_STREAM_STATE_ERROR)
        wait_status = pw_thread_loop_timed_wait_full(audio->loop, &deadline);
    int ready = av_pipewire_ready(audio);
    pw_thread_loop_unlock(audio->loop);
    if (!ready) {
        errno = wait_status == -ETIMEDOUT ? ETIMEDOUT : ENOTCONN;
        goto fail;
    }
    return 0;
fail:
    av_pipewire_free(audio);
    return -1;
}
