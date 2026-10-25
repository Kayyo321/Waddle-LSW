/*
 * Fixture for the session catalog handler. It drives the real protocol codec, the real Zig catalog
 * runtime and a real rooted manifest store in a temporary directory, through a fake feature sender
 * that records every outbound attempt. Usage: session_catalog_handler <work-directory>.
 */
#define _XOPEN_SOURCE 700

#include "session_catalog_handler.h"

#include "../../include/waddle/interop_protocol.h"
#include "session_control_host.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define Check(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

enum { FixtureMaxAttempts = 64, FixtureFrameBytes = 256 };

/* --- fake sender and clocks ------------------------------------------------------------ */

typedef struct fake_sender_t {
    uint8_t frames[FixtureMaxAttempts][FixtureFrameBytes];
    size_t lengths[FixtureMaxAttempts];
    int results[FixtureMaxAttempts];
    uint32_t attempts;
    uint32_t next_result_index;
    int scripted_results[4];
    uint32_t scripted_count;
    int always_full;
} fake_sender_t;

static int fake_send(void *context, interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length)
{
    fake_sender_t *fake = context;
    int result = 0;
    (void)sender;
    Check(length <= FixtureFrameBytes);
    if (fake->always_full) return HostFeatureQueueFull;
    if (fake->next_result_index < fake->scripted_count) {
        result = fake->scripted_results[fake->next_result_index];
        fake->next_result_index += 1;
    }
    Check(fake->attempts < FixtureMaxAttempts);
    memcpy(fake->frames[fake->attempts], frame, length);
    fake->lengths[fake->attempts] = length;
    fake->results[fake->attempts] = result;
    fake->attempts += 1;
    return result;
}

typedef struct fake_clock_t {
    uint64_t monotonic_ms;
    uint64_t wall_seconds;
} fake_clock_t;

static uint64_t fake_monotonic(void *context)
{
    return ((fake_clock_t *)context)->monotonic_ms;
}

static uint64_t fake_wall(void *context)
{
    return ((fake_clock_t *)context)->wall_seconds;
}

/* Strong definition of the production sender reference, so the default path can be exercised
 * without linking the control runtime. Frames sent through the default path land in default_sink. */
static fake_sender_t *default_sink;

int interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length)
{
    Check(default_sink != NULL);
    return fake_send(default_sink, sender, frame, length);
}

/* --- guest frame encoding -------------------------------------------------------------- */

static size_t finish_guest(interop_writer_t *writer, uint16_t type, uint8_t *frame)
{
    size_t length = 0;
    Check(interop_writer_finish(writer, type, 0, InteropFromGuest, &length) == 0);
    memcpy(frame, writer->frame, length);
    return length;
}

static size_t guest_begin_or_end(uint16_t type, uint8_t *frame)
{
    uint8_t storage[FixtureFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, storage, sizeof storage);
    return finish_guest(&writer, type, frame);
}

static size_t guest_app(uint16_t type, const char *key, const uint8_t *hash, const char *name,
                        uint32_t flags, const uint8_t *sizes, uint16_t sizes_length, uint8_t *frame)
{
    uint8_t storage[FixtureFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, storage, sizeof storage);
    Check(interop_put_bytes(&writer, InteropTagAppKey, key, (uint16_t)strlen(key)) == 0);
    if (hash != NULL) Check(interop_put_bytes(&writer, InteropTagContentHash, hash, 32) == 0);
    Check(interop_put_bytes(&writer, InteropTagDisplayName, name, (uint16_t)strlen(name)) == 0);
    Check(interop_put_uint(&writer, InteropTagAppFlags, flags) == 0);
    if (sizes != NULL) Check(interop_put_bytes(&writer, InteropTagIconSizes, sizes, sizes_length) == 0);
    return finish_guest(&writer, type, frame);
}

/* A bare header with an empty payload, for types the codec refuses to encode from the guest. */
static size_t raw_header(uint16_t type, uint8_t *frame)
{
    uint32_t magic = InteropMagic;
    uint16_t version = InteropVersion;
    int shift;
    memset(frame, 0, InteropHeaderBytes);
    for (shift = 0; shift < 4; ++shift) frame[shift] = (uint8_t)(magic >> (8 * shift));
    frame[4] = (uint8_t)version;
    frame[5] = (uint8_t)(version >> 8);
    frame[6] = (uint8_t)type;
    frame[7] = (uint8_t)(type >> 8);
    return InteropHeaderBytes;
}

static size_t guest_remove(const char *key, uint8_t *frame)
{
    uint8_t storage[FixtureFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, storage, sizeof storage);
    Check(interop_put_bytes(&writer, InteropTagAppKey, key, (uint16_t)strlen(key)) == 0);
    return finish_guest(&writer, InteropMsgCatalogRemove, frame);
}

static size_t guest_transfer_begin(uint32_t id, uint64_t total, const char *mime, uint8_t *frame)
{
    uint8_t storage[FixtureFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, storage, sizeof storage);
    Check(interop_put_uint(&writer, InteropTagTransfer, id) == 0);
    Check(interop_put_uint(&writer, InteropTagTotalBytes, total) == 0);
    Check(interop_put_bytes(&writer, InteropTagMime, mime, (uint16_t)strlen(mime)) == 0);
    return finish_guest(&writer, InteropMsgTransferBegin, frame);
}

static size_t guest_transfer_chunk(uint32_t id, uint64_t offset, const uint8_t *data, uint16_t length, uint8_t *frame)
{
    uint8_t storage[FixtureFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, storage, sizeof storage);
    Check(interop_put_uint(&writer, InteropTagTransfer, id) == 0);
    Check(interop_put_uint(&writer, InteropTagOffset, offset) == 0);
    Check(interop_put_bytes(&writer, InteropTagData, data, length) == 0);
    return finish_guest(&writer, InteropMsgTransferChunk, frame);
}

static size_t guest_transfer_end(uint32_t id, const uint8_t *hash, uint16_t hash_length, uint8_t *frame)
{
    uint8_t storage[FixtureFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, storage, sizeof storage);
    Check(interop_put_bytes(&writer, InteropTagContentHash, hash, hash_length) == 0);
    Check(interop_put_uint(&writer, InteropTagTransfer, id) == 0);
    return finish_guest(&writer, InteropMsgTransferEnd, frame);
}

static size_t guest_transfer_cancel(uint32_t id, uint32_t status, uint8_t *frame)
{
    uint8_t storage[FixtureFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, storage, sizeof storage);
    Check(interop_put_uint(&writer, InteropTagTransfer, id) == 0);
    Check(interop_put_uint(&writer, InteropTagStatus, status) == 0);
    return finish_guest(&writer, InteropMsgTransferCancel, frame);
}

/* --- host frame decoding --------------------------------------------------------------- */

static void expect_host_frame(const fake_sender_t *fake, uint32_t attempt, uint16_t type,
                              interop_header_t *header)
{
    Check(attempt < fake->attempts);
    Check(interop_header_decode(fake->frames[attempt], fake->lengths[attempt], header) == 0);
    Check(header->type == type);
    Check(header->length + InteropHeaderBytes == fake->lengths[attempt]);
}

static uint32_t host_uint(const fake_sender_t *fake, uint32_t attempt, uint16_t tag)
{
    uint64_t value = 0;
    const uint8_t *payload = fake->frames[attempt] + InteropHeaderBytes;
    size_t length = fake->lengths[attempt] - InteropHeaderBytes;
    Check(interop_field_uint(payload, length, tag, 4, &value) == 0);
    return (uint32_t)value;
}

static void host_bytes(const fake_sender_t *fake, uint32_t attempt, uint16_t tag, const uint8_t **value, uint16_t *length)
{
    const uint8_t *payload = fake->frames[attempt] + InteropHeaderBytes;
    size_t payload_length = fake->lengths[attempt] - InteropHeaderBytes;
    Check(interop_field_get(payload, payload_length, tag, 0, value, length) == 0);
}

static uint32_t host_icon_size(const fake_sender_t *fake, uint32_t attempt)
{
    uint64_t value = 0;
    const uint8_t *payload = fake->frames[attempt] + InteropHeaderBytes;
    size_t length = fake->lengths[attempt] - InteropHeaderBytes;
    Check(interop_field_uint(payload, length, InteropTagIconSize, 2, &value) == 0);
    return (uint32_t)value;
}

/* --- fixture setup --------------------------------------------------------------------- */

typedef struct fixture_t {
    char root[PATH_MAX];
    char data_path[PATH_MAX];
    char config_path[PATH_MAX];
    int state_fd;
    int data_fd;
    int config_fd;
    catalog_runtime_t *runtime;
} fixture_t;

static void make_fixture(fixture_t *fixture, const char *work)
{
    static unsigned scenario;
    char root[PATH_MAX];
    char state[PATH_MAX];
    char data[PATH_MAX];
    char config[PATH_MAX];
    catalog_runtime_config_t settings;
    static const char launcher[] = "/usr/bin/waddle";
    static const char device[] = "testdev";
    static const char version[] = "0.2.0";
    memset(fixture, 0, sizeof *fixture);
    /* Each scenario owns a fresh runtime and store so that results do not depend on order. */
    scenario += 1;
    Check(snprintf(root, sizeof root, "%s/fixture-%u-%u", work, (unsigned)getpid(), scenario) > 0);
    memcpy(fixture->root, root, sizeof root);
    Check(mkdir(root, 0700) == 0 || errno == EEXIST);
    Check(snprintf(state, sizeof state, "%s/state", root) > 0);
    Check(snprintf(data, sizeof data, "%s/data", root) > 0);
    Check(snprintf(config, sizeof config, "%s/config", root) > 0);
    Check(mkdir(state, 0700) == 0 || errno == EEXIST);
    Check(mkdir(data, 0755) == 0 || errno == EEXIST);
    Check(mkdir(config, 0755) == 0 || errno == EEXIST);
    fixture->state_fd = open(state, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    fixture->data_fd = open(data, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    fixture->config_fd = open(config, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Check(fixture->state_fd >= 0 && fixture->data_fd >= 0 && fixture->config_fd >= 0);
    Check(realpath(data, fixture->data_path) != NULL);
    Check(realpath(config, fixture->config_path) != NULL);
    memset(&settings, 0, sizeof settings);
    settings.manifest_dir_fd = fixture->state_fd;
    settings.data_root_fd = fixture->data_fd;
    settings.config_root_fd = fixture->config_fd;
    settings.menu_policy = CatalogRuntimeMenuInstalled;
    settings.shortcuts = CatalogRuntimeShortcutsNone;
    settings.data_root = (const uint8_t *)fixture->data_path;
    settings.data_root_length = strlen(fixture->data_path);
    settings.config_root = (const uint8_t *)fixture->config_path;
    settings.config_root_length = strlen(fixture->config_path);
    settings.launcher_path = (const uint8_t *)launcher;
    settings.launcher_path_length = sizeof launcher - 1;
    settings.device = (const uint8_t *)device;
    settings.device_length = sizeof device - 1;
    settings.waddle_version = (const uint8_t *)version;
    settings.waddle_version_length = sizeof version - 1;
    Check(catalog_runtime_create(&settings, &fixture->runtime) == CatalogRuntimeOk);
    Check(fixture->runtime != NULL);
}

static void destroy_fixture(fixture_t *fixture)
{
    catalog_runtime_destroy(fixture->runtime);
    close(fixture->state_fd);
    close(fixture->data_fd);
    close(fixture->config_fd);
}

static int file_exists(const fixture_t *fixture, const char *relative)
{
    char path[PATH_MAX];
    struct stat status;
    Check(snprintf(path, sizeof path, "%s/%s", fixture->data_path, relative) > 0);
    return stat(path, &status) == 0;
}

static void clock_set(fake_clock_t *clock, uint64_t monotonic_ms)
{
    clock->monotonic_ms = monotonic_ms;
    clock->wall_seconds = 1792944000u; /* 2026-10-25T12:00:00Z */
}

static void handler_bind(session_catalog_handler_t *handler, fake_sender_t *fake, fake_clock_t *clock,
                         catalog_runtime_t *runtime)
{
    session_catalog_handler_init(handler, runtime);
    handler->send = fake_send;
    handler->send_context = fake;
    handler->monotonic = fake_monotonic;
    handler->clock_context = clock;
    handler->wall = fake_wall;
}

/* --- scenarios ------------------------------------------------------------------------- */

/*
 * A request refused by the sender is retained by the handler. Removing its application before the
 * sender recovers must neither send the request nor read the released key (ASan).
 */
static void test_removed_app_requests_are_never_sent(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    static const uint8_t sizes[] = {16, 0, 32, 0};
    fake_sender_t fake;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    size_t length;
    uint32_t attempt;
    memset(&fake, 0, sizeof fake);
    fake.always_full = 1;
    clock_set(&clock, 70000);
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    table = session_catalog_handler_table(&handler);

    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogBegin, 0, frame, length, NULL) == 0);
    length = guest_app(InteropMsgCatalogAdd, "a-Victim", NULL, "Victim", 0, sizes, sizeof sizes, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);
    length = guest_begin_or_end(InteropMsgCatalogEnd, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);
    Check(table.on_tick(table.context, 70000, NULL) == 0);
    Check(handler.has_pending == 1);

    length = guest_remove("a-Victim", frame);
    Check(table.on_frame(table.context, InteropMsgCatalogRemove, 0, frame, length, NULL) == 0);

    /* The sender recovers. Only cancels may be sent now; no request for the removed key. */
    fake.always_full = 0;
    clock.monotonic_ms = 70500;
    Check(table.on_tick(table.context, 70500, NULL) == 0);
    for (attempt = 0; attempt < fake.attempts; ++attempt) {
        interop_header_t header;
        Check(interop_header_decode(fake.frames[attempt], fake.lengths[attempt], &header) == 0);
        Check(header.type != InteropMsgIconRequest);
    }
}

/* A sender that never drains must neither abort the host nor make the pump spin. */
static void test_never_draining_sender(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    fake_sender_t fake;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    catalog_runtime_action_t action;
    size_t length;
    uint32_t i;
    uint32_t drained = 0;
    memset(&fake, 0, sizeof fake);
    fake.always_full = 1;
    clock_set(&clock, 60000);
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    table = session_catalog_handler_table(&handler);
    for (i = 0; i < 10000; ++i) {
        length = guest_transfer_begin(100000u + i, 10, "image/png", frame);
        Check(table.on_frame(table.context, InteropMsgTransferBegin, 0, frame, length, NULL) == 0);
        clock.monotonic_ms += 1;
    }
    Check(handler.rejected_frames == 0);
    Check(handler.has_pending == 1);
    /* The retained action is due at most one retry delay away; a refused retry moves the next
     * deadline a full delay ahead, so the pump cannot spin on a sender that never drains. */
    Check(table.next_deadline_ms(table.context, clock.monotonic_ms) <= clock.monotonic_ms + 100);
    Check(table.on_tick(table.context, clock.monotonic_ms, NULL) == 0);
    Check(table.next_deadline_ms(table.context, clock.monotonic_ms) == clock.monotonic_ms + 100);
    while (catalog_runtime_next_action(fixture->runtime, &action) == 1) drained += 1;
    Check(drained <= 64);
}

static void test_retention_and_header_mismatch(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    fake_sender_t fake;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    size_t length;
    memset(&fake, 0, sizeof fake);
    fake.scripted_results[0] = HostFeatureQueueFull;
    fake.scripted_results[1] = HostFeatureQueueFull;
    fake.scripted_count = 2;
    clock_set(&clock, 40000);
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    table = session_catalog_handler_table(&handler);

    /* A frame whose header type differs from the dispatched type is ignored. */
    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);
    Check(handler.rejected_frames == 1);

    /* An unknown transfer identifier is refused with a NotFound cancel whose key is empty. A send
     * refused with QueueFull keeps that cancel for the next callback, twice, then delivers it. */
    length = guest_transfer_begin(4242, 4, "image/png", frame);
    Check(table.on_frame(table.context, InteropMsgTransferBegin, 0, frame, length, NULL) == 0);
    Check(fake.attempts == 1);
    Check(handler.has_pending == 1);
    /* A refused send is retried after the retry delay, never immediately. */
    Check(table.next_deadline_ms(table.context, 40000) == 40100);
    Check(table.on_tick(table.context, 40000, NULL) == 0);
    Check(fake.attempts == 1);
    Check(table.on_tick(table.context, 40100, NULL) == 0);
    Check(fake.attempts == 2);
    Check(handler.has_pending == 1);
    Check(table.on_tick(table.context, 40200, NULL) == 0);
    Check(fake.attempts == 3);
    Check(handler.has_pending == 0);
    Check(fake.lengths[0] == fake.lengths[1] && fake.lengths[1] == fake.lengths[2]);
    Check(memcmp(fake.frames[0], fake.frames[2], fake.lengths[0]) == 0);
    expect_host_frame(&fake, 2, InteropMsgTransferCancel, &(interop_header_t){0});
    Check(host_uint(&fake, 2, InteropTagTransfer) == 4242);
    Check(host_uint(&fake, 2, InteropTagStatus) == CatalogRuntimeCancelNotFound);
}

static void test_default_send_and_wall_overflow(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    static const uint8_t sizes[] = {16, 0};
    fake_sender_t sink;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    size_t length;
    memset(&sink, 0, sizeof sink);
    default_sink = &sink;
    clock_set(&clock, 50000);

    /* No injected send or clocks: frames go through the production sender reference, and the
     * production monotonic and wall clocks drive the callbacks. */
    session_catalog_handler_init(&handler, fixture->runtime);
    table = session_catalog_handler_table(&handler);
    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogBegin, 0, frame, length, NULL) == 0);
    length = guest_app(InteropMsgCatalogAdd, "a-Default", NULL, "Default", 0, sizes, sizeof sizes, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);
    length = guest_begin_or_end(InteropMsgCatalogEnd, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);
    Check(table.on_tick(table.context, 50000, NULL) == 0);
    Check(sink.attempts == 1);
    expect_host_frame(&sink, 0, InteropMsgIconRequest, &(interop_header_t){0});

    /* An unowned type that validates from the guest is drained without dispatch. */
    length = raw_header(InteropMsgPong, frame);
    Check(table.on_frame(table.context, InteropMsgPong, 0, frame, length, NULL) == 0);
    Check(handler.rejected_frames == 0);

    /* A wall clock that formats a five-digit year cannot fit the manifest timestamp. */
    handler_bind(&handler, &sink, &clock, fixture->runtime);
    clock.wall_seconds = 253402300800ull;
    Check(table.on_tick(table.context, 50001, NULL) == 1);
    default_sink = NULL;
}

static void test_table_and_ownership(void)
{
    session_catalog_handler_t handler;
    session_handler_t table;
    session_catalog_handler_init(&handler, NULL);
    table = session_catalog_handler_table(&handler);
    Check(strcmp(table.name, "catalog") == 0);
    Check(table.context == &handler);
    Check(table.capabilities == InteropCapCatalog);
    Check(table.poll_fds == NULL && table.on_fds == NULL);
    Check(table.owns_type(table.context, InteropMsgCatalogBegin) == 1);
    Check(table.owns_type(table.context, InteropMsgCatalogEnd) == 1);
    Check(table.owns_type(table.context, InteropMsgTransferBegin) == 1);
    Check(table.owns_type(table.context, InteropMsgTransferCancel) == 1);
    Check(table.owns_type(table.context, InteropMsgCatalogEnd + 1) == 0);
    Check(table.owns_type(table.context, InteropMsgPing) == 0);
    Check(table.owns_type(table.context, InteropMsgTransferCancel + 1) == 0);
    Check(table.on_ready(table.context, InteropCapCatalog, NULL) == 0);
    Check(table.next_deadline_ms(table.context, 42) == UINT64_MAX);
}

static void test_sync_requests_icon_and_publishes(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    uint8_t hash[32] = {0};
    static const uint8_t sizes[] = {16, 0};
    fake_sender_t fake;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    size_t length;
    interop_header_t header;
    const uint8_t *key = NULL;
    uint16_t key_length = 0;
    uint32_t id;
    uint8_t png[64];
    uint8_t digest[32];

    memset(&fake, 0, sizeof fake);
    clock_set(&clock, 1000);
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    table = session_catalog_handler_table(&handler);

    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogBegin, 0, frame, length, NULL) == 0);
    hash[0] = 0x31;
    length = guest_app(InteropMsgCatalogAdd, "a-Alpha", hash, "Alpha", 0, sizes, sizeof sizes, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);
    length = guest_begin_or_end(InteropMsgCatalogEnd, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);

    /* Tick publishes the desktop entry and emits exactly one IconRequest for size 16. */
    Check(table.on_tick(table.context, 1000, NULL) == 0);
    Check(fake.attempts == 1);
    expect_host_frame(&fake, 0, InteropMsgIconRequest, &header);
    host_bytes(&fake, 0, InteropTagAppKey, &key, &key_length);
    Check(key_length == 7 && memcmp(key, "a-Alpha", 7) == 0);
    Check(host_icon_size(&fake, 0) == 16);
    id = host_uint(&fake, 0, InteropTagTransfer);
    Check(id != 0);
    Check(file_exists(fixture, "applications/waddle.testdev.a-Alpha.desktop"));
    Check(table.next_deadline_ms(table.context, 1000) == 1000 + 10000);

    /* A valid PNG arrives in three frames: begin, chunk, end. */
    memset(png, 0, sizeof png);
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    png[15] = 13;
    memcpy(png + 12, "IHDR", 4);
    png[19] = 16;
    png[23] = 16;
    png[24] = 8;
    png[25] = 6;
    interop_sha256(png, sizeof png, digest);
    length = guest_transfer_begin(id, sizeof png, "image/png", frame);
    Check(table.on_frame(table.context, InteropMsgTransferBegin, 0, frame, length, NULL) == 0);
    length = guest_transfer_chunk(id, 0, png, sizeof png, frame);
    Check(table.on_frame(table.context, InteropMsgTransferChunk, 0, frame, length, NULL) == 0);
    length = guest_transfer_end(id, digest, 32, frame);
    Check(table.on_frame(table.context, InteropMsgTransferEnd, 0, frame, length, NULL) == 0);
    Check(table.on_tick(table.context, 2000, NULL) == 0);
    Check(fake.attempts == 1);
    Check(handler.rejected_frames == 0);
    catalog_runtime_t *runtime = fixture->runtime;
    catalog_runtime_stats_t stats;
    Check(catalog_runtime_stats(runtime, &stats) == CatalogRuntimeOk);
    Check(stats.apps == 1 && stats.outstanding == 0 && stats.icon_bytes == sizeof png);
}

static void test_exact_outbound_frames_and_queue_full(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    static const uint8_t sizes[] = {32, 0};
    fake_sender_t fake;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    size_t length;
    interop_header_t header;
    memset(&fake, 0, sizeof fake);
    fake.scripted_results[0] = HostFeatureQueueFull;
    fake.scripted_count = 1;
    clock_set(&clock, 5000);
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    table = session_catalog_handler_table(&handler);

    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogBegin, 0, frame, length, NULL) == 0);
    length = guest_app(InteropMsgCatalogAdd, "a-Beta", NULL, "Beta", 0, sizes, sizeof sizes, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);
    length = guest_begin_or_end(InteropMsgCatalogEnd, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);

    /* The first attempt is refused by the sender; the request is retained and re-sent unchanged. */
    Check(table.on_tick(table.context, 5000, NULL) == 0);
    Check(fake.attempts == 1);
    Check(table.next_deadline_ms(table.context, 5000) == 5100);
    Check(table.on_tick(table.context, 5001, NULL) == 0);
    Check(fake.attempts == 1);
    Check(table.on_tick(table.context, 5100, NULL) == 0);
    Check(fake.attempts == 2);
    Check(fake.lengths[0] == fake.lengths[1]);
    Check(memcmp(fake.frames[0], fake.frames[1], fake.lengths[0]) == 0);
    expect_host_frame(&fake, 1, InteropMsgIconRequest, &header);
    Check(host_icon_size(&fake, 1) == 32);

    /* Inbound unsolicited frames of other types are refused without reaching the runtime. */
    length = raw_header(InteropMsgPing, frame);
    Check(table.on_frame(table.context, InteropMsgPing, 0, frame, length, NULL) == 0);
    Check(handler.rejected_frames == 1);
}

static void test_hostile_frames(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];

    fake_sender_t fake;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    size_t length;
    uint32_t before;
    memset(&fake, 0, sizeof fake);
    clock_set(&clock, 9000);
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    table = session_catalog_handler_table(&handler);

    /* Header length mismatch and wrong type in the header are ignored. */
    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogBegin, 0, frame, length - 4, NULL) == 0);
    length = guest_begin_or_end(InteropMsgCatalogEnd, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);
    before = (uint32_t)handler.rejected_frames;
    Check(before == 1);

    /* A payload that fails codec validation: a Remove body relabelled as CatalogAdd lacks the
     * required display name, so the codec refuses it and the handler counts and ignores it. */
    length = guest_remove("a-Bad", frame);
    frame[6] = (uint8_t)InteropMsgCatalogAdd;
    frame[7] = (uint8_t)(InteropMsgCatalogAdd >> 8);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);
    Check(handler.rejected_frames == 2);

    /* Runtime-level refusal: an invalid key that the codec accepts syntactically is still ignored. */
    length = guest_app(InteropMsgCatalogUpdate, "a-Unknown", NULL, "Unknown", 0, NULL, 0, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogUpdate, 0, frame, length, NULL) == 0);

    /* Removing an unknown key is a no-op and needs no frame. */
    length = guest_app(InteropMsgCatalogAdd, "a-Gamma", NULL, "Gamma", 0, NULL, 0, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);

    /* Transfer frames with unknown identifiers are cancelled with NotFound. */
    {
        uint8_t data[4] = {1, 2, 3, 4};
        uint8_t digest[32] = {0};
        length = guest_transfer_begin(777, 4, "image/png", frame);
        Check(table.on_frame(table.context, InteropMsgTransferBegin, 0, frame, length, NULL) == 0);
        length = guest_transfer_chunk(777, 0, data, 4, frame);
        Check(table.on_frame(table.context, InteropMsgTransferChunk, 0, frame, length, NULL) == 0);
        length = guest_transfer_end(777, digest, 32, frame);
        Check(table.on_frame(table.context, InteropMsgTransferEnd, 0, frame, length, NULL) == 0);


        length = guest_transfer_cancel(777, InteropCancelled, frame);
        Check(table.on_frame(table.context, InteropMsgTransferCancel, 0, frame, length, NULL) == 0);
    }
    Check(fake.attempts == 3);
    {
        interop_header_t header;
        uint32_t attempt;
        for (attempt = 0; attempt < fake.attempts; ++attempt) {
            expect_host_frame(&fake, attempt, InteropMsgTransferCancel, &header);
            Check(host_uint(&fake, attempt, InteropTagStatus) == CatalogRuntimeCancelNotFound);
        }
    }
    Check(handler.rejected_frames == 2);

    /* Guest cancellation of an unknown identifier and a second remove are no-ops. */
    length = guest_transfer_cancel(999, InteropCancelled, frame);
    Check(table.on_frame(table.context, InteropMsgTransferCancel, 0, frame, length, NULL) == 0);
    length = guest_remove("a-Gamma", frame);
    Check(table.on_frame(table.context, InteropMsgCatalogRemove, 0, frame, length, NULL) == 0);
    length = guest_remove("a-Gamma", frame);
    Check(table.on_frame(table.context, InteropMsgCatalogRemove, 0, frame, length, NULL) == 0);
    Check(handler.rejected_frames == 2);
}

static void test_transfer_refusals_and_timeouts(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    static const uint8_t sizes[] = {16, 0};
    fake_sender_t fake;
    fake_clock_t clock;
    session_catalog_handler_t handler;
    session_handler_t table;
    size_t length;
    uint32_t id;
    uint8_t data[16] = {0};
    memset(&fake, 0, sizeof fake);
    clock_set(&clock, 20000);
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    table = session_catalog_handler_table(&handler);

    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogBegin, 0, frame, length, NULL) == 0);
    length = guest_app(InteropMsgCatalogAdd, "a-Delta", NULL, "Delta", 0, sizes, sizeof sizes, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);
    length = guest_begin_or_end(InteropMsgCatalogEnd, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);
    Check(table.on_tick(table.context, 20000, NULL) == 0);
    Check(fake.attempts == 1);
    id = host_uint(&fake, 0, InteropTagTransfer);

    /* A wrong MIME type is refused as Unsupported and cancelled. */
    length = guest_transfer_begin(id, 16, "text/plain", frame);
    Check(table.on_frame(table.context, InteropMsgTransferBegin, 0, frame, length, NULL) == 0);
    Check(fake.attempts == 2);
    Check(host_uint(&fake, 1, InteropTagStatus) == CatalogRuntimeCancelUnsupported);

    /* The next request follows the backoff; an announced size above the cap is TooLarge. */
    Check(table.on_tick(table.context, 25000, NULL) == 0);
    Check(fake.attempts == 3);
    id = host_uint(&fake, 2, InteropTagTransfer);
    length = guest_transfer_begin(id, 2 * 1024 * 1024, "image/png", frame);
    Check(table.on_frame(table.context, InteropMsgTransferBegin, 0, frame, length, NULL) == 0);
    Check(fake.attempts == 4);
    Check(host_uint(&fake, 3, InteropTagStatus) == CatalogRuntimeCancelTooLarge);

    /* A gap is a guest error; the request then times out after its deadline. */
    Check(table.on_tick(table.context, 60000, NULL) == 0);
    Check(fake.attempts == 5);
    id = host_uint(&fake, 4, InteropTagTransfer);
    length = guest_transfer_begin(id, sizeof data, "image/png", frame);
    Check(table.on_frame(table.context, InteropMsgTransferBegin, 0, frame, length, NULL) == 0);
    length = guest_transfer_chunk(id, 4, data, sizeof data, frame);
    Check(table.on_frame(table.context, InteropMsgTransferChunk, 0, frame, length, NULL) == 0);
    Check(fake.attempts == 6);
    Check(host_uint(&fake, 5, InteropTagStatus) == CatalogRuntimeCancelGuestError);
    Check(table.on_tick(table.context, 80000, NULL) == 0);
    Check(fake.attempts >= 6);
}

static void test_clock_defaults_closing_and_wall_failure(const fixture_t *fixture)
{
    uint8_t frame[FixtureFrameBytes];
    session_catalog_handler_t handler;
    session_handler_t table;
    fake_sender_t fake;
    fake_clock_t clock;
    size_t length;
    memset(&fake, 0, sizeof fake);
    clock_set(&clock, 1);

    /* Defaults: CLOCK_MONOTONIC for frames, time(NULL) for `updated`, and the weak sender
     * reference is NULL in this fixture so the outbound send reports Unsupported and is dropped. */
    session_catalog_handler_init(&handler, fixture->runtime);
    table = session_catalog_handler_table(&handler);
    length = guest_begin_or_end(InteropMsgCatalogBegin, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogBegin, 0, frame, length, NULL) == 0);
    length = guest_app(InteropMsgCatalogAdd, "a-Eps", NULL, "Eps", 0, NULL, 0, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogAdd, 0, frame, length, NULL) == 0);
    length = guest_begin_or_end(InteropMsgCatalogEnd, frame);
    Check(table.on_frame(table.context, InteropMsgCatalogEnd, 0, frame, length, NULL) == 0);
    Check(table.on_tick(table.context, 1, NULL) == 0);

    /* A wall clock outside the representable calendar fails the tick. */
    handler_bind(&handler, &fake, &clock, fixture->runtime);
    clock.wall_seconds = UINT64_MAX / 4;
    Check(table.on_tick(table.context, 2, NULL) == 1);
    clock.wall_seconds = 1792944000u;
    Check(table.on_tick(table.context, 3, NULL) == 0);

    /* Closing resets the in-memory snapshot and clears any retained action. */
    handler.has_pending = 1;
    Check(table.next_deadline_ms(table.context, 3) == 3);
    table.on_closing(table.context, 0, NULL);
    Check(handler.has_pending == 0);
    {
        catalog_runtime_stats_t stats;
        Check(catalog_runtime_stats(fixture->runtime, &stats) == CatalogRuntimeOk);
        Check(stats.apps == 0);
    }
}

int main(int argc, char **argv)
{
    fixture_t fixture;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <work-directory>\n", argv[0]);
        return 2;
    }
    test_table_and_ownership();
    make_fixture(&fixture, argv[1]);
    test_sync_requests_icon_and_publishes(&fixture);
    destroy_fixture(&fixture);

    make_fixture(&fixture, argv[1]);
    test_exact_outbound_frames_and_queue_full(&fixture);
    destroy_fixture(&fixture);

    make_fixture(&fixture, argv[1]);
    test_hostile_frames(&fixture);
    destroy_fixture(&fixture);

    make_fixture(&fixture, argv[1]);
    test_transfer_refusals_and_timeouts(&fixture);
    destroy_fixture(&fixture);

    make_fixture(&fixture, argv[1]);
    test_clock_defaults_closing_and_wall_failure(&fixture);
    destroy_fixture(&fixture);

    make_fixture(&fixture, argv[1]);
    test_removed_app_requests_are_never_sent(&fixture);
    destroy_fixture(&fixture);

    make_fixture(&fixture, argv[1]);
    test_never_draining_sender(&fixture);
    destroy_fixture(&fixture);

    make_fixture(&fixture, argv[1]);
    test_retention_and_header_mismatch(&fixture);
    test_default_send_and_wall_overflow(&fixture);
    destroy_fixture(&fixture);
    puts("session catalog handler tests passed");
    return 0;
}
