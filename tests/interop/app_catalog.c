/** @file app_catalog.c @brief Deterministic catalog snapshot/diff regressions. */
#include "../../src/interop/app_catalog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation_state_t {
    size_t calls;
    size_t fail_at;
    size_t live;
    size_t bytes;
} allocation_state_t;

static void *test_allocate(void *context, size_t size) {
    allocation_state_t *state = context;
    state->calls++;
    if (state->fail_at != 0 && state->calls == state->fail_at) return NULL;
    void *memory = malloc(size);
    if (memory != NULL) { state->live++; state->bytes += size; }
    return memory;
}

static void test_deallocate(void *context, void *memory, size_t size) {
    allocation_state_t *state = context;
    if (memory == NULL) return;
    assert(state->live != 0 && state->bytes >= size);
    state->live--;
    state->bytes -= size;
    free(memory);
}

static app_catalog_bytes_t text(const char *value) {
    return (app_catalog_bytes_t){ (const uint8_t *)value, strlen(value) };
}

static app_catalog_candidate_t make_candidate(app_catalog_source_t source,
                                              const char *identity,
                                              const char *target,
                                              const char *display,
                                              const char *source_identifier) {
    app_catalog_candidate_t candidate = {0};
    candidate.source = source;
    candidate.identity_kind = AppCatalogIdentityAumid;
    candidate.identity = text(identity);
    candidate.display_name = text(display);
    candidate.target_path = text(target);
    candidate.arguments = text("");
    candidate.working_directory = text("C:\\Apps");
    candidate.aumid = text(identity);
    candidate.source_identifier = text(source_identifier);
    candidate.desktop_shortcut = source == AppCatalogSourceUserDesktop || source == AppCatalogSourcePublicDesktop;
    return candidate;
}

static app_catalog_allocator_t allocator_for(allocation_state_t *state) {
    return (app_catalog_allocator_t){ state, test_allocate, test_deallocate };
}

#ifdef _WIN32
static bool always_cancel(void *context) {
    (void)context;
    return true;
}
#endif

static void expect_key(const app_catalog_entry_t *entry, const char *expected) {
    assert(entry->app_key.length == strlen(expected));
    assert(memcmp(entry->app_key.data, expected, entry->app_key.length) == 0);
}

static void test_single_and_canonical_path(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_candidate_t candidate = make_candidate(AppCatalogSourceAppsFolder, "Acme.App", "C:\\Apps\\Acme.exe", "Acme", "apps:acme");
    app_catalog_snapshot_t snapshot;
    app_catalog_snapshot_init(&snapshot);
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusOk);
    assert(snapshot.count == 1 && snapshot.duplicate_launch_conflicts == 0);
    expect_key(&snapshot.entries[0], "a-Acme_App-806a2774");
    assert(snapshot.entries[0].identity_kind == AppCatalogIdentityAumid);
    assert(snapshot.entries[0].app_flags == 0);
    const app_catalog_entry_t *old_entries = snapshot.entries;
    size_t old_count = snapshot.count;
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    assert(snapshot.entries == old_entries && snapshot.count == old_count);
    app_catalog_snapshot_deinit(&snapshot);
    app_catalog_snapshot_deinit(&snapshot);

    uint8_t path_identity[] = {
        'C',0,':',0,'\\',0,'A',0,'P',0,'P',0,'S',0,'\\',0,
        'E',0,'X',0,'A',0,'M',0,'P',0,'L',0,'E',0,'.',0,'E',0,'X',0,'E',0
    };
    candidate.identity_kind = AppCatalogIdentityCanonicalPath;
    candidate.identity = (app_catalog_bytes_t){ path_identity, sizeof(path_identity) };
    candidate.target_path = text("C:\\Apps\\Example.exe");
    candidate.aumid = (app_catalog_bytes_t){0};
    candidate.source = AppCatalogSourceUserPrograms;
    candidate.desktop_shortcut = false;
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusOk);
    expect_key(&snapshot.entries[0], "p-30cdb70f8c8ad334a38ab6d1abe548c0");
    app_catalog_snapshot_deinit(&snapshot);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_duplicate_merge_and_order(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_candidate_t candidates[3];
    candidates[0] = make_candidate(AppCatalogSourcePublicDesktop, "Vendor.Editor", "C:\\legacy.exe", "Editor old", "Z:\\Editor.lnk");
    candidates[1] = make_candidate(AppCatalogSourceAppsFolder, "Vendor.Editor", "C:\\editor.exe", "Editor", "apps:vendor-editor");
    candidates[2] = make_candidate(AppCatalogSourceUserDesktop, "Vendor.Editor", "C:\\editor.exe", "Editor desktop", "C:\\Users\\u\\Desktop\\Editor.lnk");
    candidates[1].package_family = text("Vendor.Editor_123456789abcd");
    app_catalog_snapshot_t first, second;
    app_catalog_snapshot_init(&first);
    app_catalog_snapshot_init(&second);
    assert(app_catalog_snapshot_build(allocator, candidates, 3, &first) == AppCatalogStatusOk);
    assert(first.count == 1 && first.duplicate_launch_conflicts == 1);
    assert(first.entries[0].app_flags == (InteropAppDesktopShortcut | InteropAppPackaged));
    assert(first.entries[0].target_path.length == strlen("C:\\editor.exe"));
    app_catalog_candidate_t reversed[3] = { candidates[2], candidates[1], candidates[0] };
    assert(app_catalog_snapshot_build(allocator, reversed, 3, &second) == AppCatalogStatusOk);
    assert(second.duplicate_launch_conflicts == first.duplicate_launch_conflicts);
    assert(first.entries[0].app_key.length == second.entries[0].app_key.length);
    assert(memcmp(first.entries[0].target_path.data, second.entries[0].target_path.data, first.entries[0].target_path.length) == 0);
    app_catalog_snapshot_deinit(&first);
    app_catalog_snapshot_deinit(&second);
    app_catalog_candidate_t tied[2] = {
        make_candidate(AppCatalogSourceUserPrograms, "Vendor.Tie", "C:\\z.exe", "Tie Z", "C:\\Programs\\z.lnk"),
        make_candidate(AppCatalogSourceUserPrograms, "Vendor.Tie", "C:\\a.exe", "Tie A", "C:\\Programs\\a.lnk")
    };
    app_catalog_snapshot_init(&first); app_catalog_snapshot_init(&second);
    assert(app_catalog_snapshot_build(allocator, tied, 2, &first) == AppCatalogStatusOk);
    app_catalog_candidate_t tied_reverse[2] = { tied[1], tied[0] };
    assert(app_catalog_snapshot_build(allocator, tied_reverse, 2, &second) == AppCatalogStatusOk);
    assert(first.duplicate_launch_conflicts == 1 && second.duplicate_launch_conflicts == 1);
    assert(memcmp(first.entries[0].target_path.data, "C:\\a.exe", strlen("C:\\a.exe")) == 0);
    assert(memcmp(first.entries[0].target_path.data, second.entries[0].target_path.data, first.entries[0].target_path.length) == 0);
    app_catalog_snapshot_deinit(&first);
    app_catalog_snapshot_deinit(&second);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_complete_tuple_tie_breakers(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_snapshot_t first, second;
    app_catalog_snapshot_init(&first);
    app_catalog_snapshot_init(&second);
    app_catalog_candidate_t candidates[2] = {
        make_candidate(AppCatalogSourceUserPrograms, "Vendor.Tie", "C:\\z.exe", "Z", "same-source"),
        make_candidate(AppCatalogSourceUserPrograms, "Vendor.Tie", "C:\\a.exe", "A", "same-source")
    };
    assert(app_catalog_snapshot_build(allocator, candidates, 2, &first) == AppCatalogStatusOk);
    app_catalog_candidate_t reversed[2] = { candidates[1], candidates[0] };
    assert(app_catalog_snapshot_build(allocator, reversed, 2, &second) == AppCatalogStatusOk);
    assert(memcmp(first.entries[0].target_path.data, "C:\\a.exe", strlen("C:\\a.exe")) == 0);
    app_catalog_snapshot_deinit(&first);
    app_catalog_snapshot_deinit(&second);

    candidates[0] = make_candidate(AppCatalogSourceUserPrograms, "Vendor.Tie", "C:\\same.exe", "Z", "same-source");
    candidates[1] = make_candidate(AppCatalogSourceUserPrograms, "Vendor.Tie", "C:\\same.exe", "A", "same-source");
    candidates[0].arguments = text("z");
    candidates[1].arguments = text("a");
    app_catalog_snapshot_init(&first);
    assert(app_catalog_snapshot_build(allocator, candidates, 2, &first) == AppCatalogStatusOk);
    assert(first.entries[0].arguments.length == 1 && first.entries[0].arguments.data[0] == 'a');
    app_catalog_snapshot_deinit(&first);

    candidates[0].arguments = text("same");
    candidates[1].arguments = text("same");
    candidates[0].working_directory = text("C:\\z");
    candidates[1].working_directory = text("C:\\a");
    app_catalog_snapshot_init(&first);
    assert(app_catalog_snapshot_build(allocator, candidates, 2, &first) == AppCatalogStatusOk);
    assert(memcmp(first.entries[0].working_directory.data, "C:\\a", strlen("C:\\a")) == 0);
    app_catalog_snapshot_deinit(&first);

    candidates[0].working_directory = text("same");
    candidates[1].working_directory = text("same");
    candidates[0].display_name = text("Z");
    candidates[1].display_name = text("A");
    app_catalog_snapshot_init(&first);
    assert(app_catalog_snapshot_build(allocator, candidates, 2, &first) == AppCatalogStatusOk);
    assert(first.entries[0].display_name.data[0] == 'A');
    app_catalog_snapshot_deinit(&first);

    static uint8_t path_identity[] = { 'C',0,':',0,'\\',0,'T',0,'I',0,'E',0,'.',0,'E',0,'X',0,'E',0 };
    candidates[0] = make_candidate(AppCatalogSourceCommonPrograms, "unused", "C:\\tie.exe", "Common", "common-source");
    candidates[1] = make_candidate(AppCatalogSourceAppsFolder, "unused", "C:\\tie.exe", "Apps", "apps-path-source");
    candidates[0].identity_kind = AppCatalogIdentityCanonicalPath;
    candidates[0].identity = (app_catalog_bytes_t){ path_identity, sizeof(path_identity) };
    candidates[0].aumid = (app_catalog_bytes_t){0};
    candidates[1].identity_kind = AppCatalogIdentityCanonicalPath;
    candidates[1].identity = (app_catalog_bytes_t){ path_identity, sizeof(path_identity) };
    candidates[1].aumid = (app_catalog_bytes_t){0};
    app_catalog_snapshot_init(&first);
    assert(app_catalog_snapshot_build(allocator, candidates, 2, &first) == AppCatalogStatusOk);
    assert(memcmp(first.entries[0].display_name.data, "Apps", 4) == 0);
    app_catalog_snapshot_deinit(&first);
    candidates[1].source = AppCatalogSourceUserPrograms;
    candidates[1].source_identifier = text("user-source");
    app_catalog_snapshot_init(&first);
    assert(app_catalog_snapshot_build(allocator, candidates, 2, &first) == AppCatalogStatusOk);
    assert(memcmp(first.entries[0].display_name.data, "Apps", 4) == 0);
    app_catalog_snapshot_deinit(&first);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_null_and_malformed_public_objects(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_candidate_t candidate = make_candidate(AppCatalogSourceAppsFolder, "Object.App", "C:\\Apps\\object.exe", "Object", "apps:object");
    app_catalog_snapshot_t before, after, output;
    app_catalog_snapshot_init(&before);
    app_catalog_snapshot_init(&after);
    app_catalog_snapshot_init(&output);
    app_catalog_snapshot_init(NULL);
    app_catalog_snapshot_deinit(NULL);
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &before) == AppCatalogStatusOk);
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &after) == AppCatalogStatusOk);
    app_catalog_diff_t diff = {0};
    app_catalog_diff_deinit(NULL);
    assert(app_catalog_diff_build(NULL, &after, &diff) == AppCatalogStatusInvalidArgument);
    assert(app_catalog_diff_build(&before, NULL, &diff) == AppCatalogStatusInvalidArgument);
    assert(app_catalog_diff_build(&before, &after, NULL) == AppCatalogStatusInvalidArgument);
    app_catalog_snapshot_t malformed = before;
    malformed.entries = NULL;
    assert(app_catalog_diff_build(&malformed, &after, &diff) == AppCatalogStatusInvalidArgument);
    malformed = before;
    malformed.count = AppCatalogMaxEntries + 1u;
    assert(app_catalog_diff_build(&malformed, &after, &diff) == AppCatalogStatusInvalidArgument);
    malformed = before;
    malformed.allocator.allocate = NULL;
    assert(app_catalog_diff_build(&malformed, &after, &diff) == AppCatalogStatusInvalidArgument);
    malformed = before;
    malformed.allocator.deallocate = NULL;
    assert(app_catalog_diff_build(&malformed, &after, &diff) == AppCatalogStatusInvalidArgument);
    malformed = after;
    assert(app_catalog_diff_build(&before, &malformed, &diff) == AppCatalogStatusOk);
    app_catalog_diff_deinit(&diff);
    malformed = after;
    malformed.entries = NULL;
    assert(app_catalog_diff_build(&before, &malformed, &diff) == AppCatalogStatusInvalidArgument);
    malformed = after;
    malformed.count = AppCatalogMaxEntries + 1u;
    assert(app_catalog_diff_build(&before, &malformed, &diff) == AppCatalogStatusInvalidArgument);
    diff.count = 1;
    assert(app_catalog_diff_build(&before, &after, &diff) == AppCatalogStatusInvalidArgument);
    diff = (app_catalog_diff_t){0};
    app_catalog_snapshot_t invalid_output;
    app_catalog_entry_t sentinel_entry = {0};
    uint8_t sentinel_storage = 0;
    for (size_t field = 0; field < 5; field++) {
        app_catalog_snapshot_init(&invalid_output);
        switch (field) {
            case 0: invalid_output.entries = &sentinel_entry; break;
            case 1: invalid_output.storage = &sentinel_storage; break;
            case 2: invalid_output.count = 1; break;
            case 3: invalid_output.allocation_size = 1; break;
            default: invalid_output.storage_size = 1; break;
        }
        assert(app_catalog_snapshot_build(allocator, &candidate, 1, &invalid_output) == AppCatalogStatusInvalidArgument);
    }
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &output) == AppCatalogStatusOk);
    assert(app_catalog_snapshot_build(allocator, &candidate, 0, &output) == AppCatalogStatusInvalidArgument);
    app_catalog_diff_deinit(&diff);
    app_catalog_snapshot_deinit(&before);
    app_catalog_snapshot_deinit(&after);
    app_catalog_snapshot_deinit(&output);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_validation_atomicity(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_candidate_t candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    app_catalog_snapshot_t snapshot;
    app_catalog_snapshot_init(&snapshot);
    candidate.display_name = (app_catalog_bytes_t){ (const uint8_t *)"\xc0\xaf", 2 };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    assert(snapshot.count == 0 && snapshot.entries == NULL);
    uint8_t too_long_display[256];
    memset(too_long_display, 'A', sizeof(too_long_display));
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.display_name = (app_catalog_bytes_t){ too_long_display, sizeof(too_long_display) };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.display_name = (app_catalog_bytes_t){ NULL, 0 };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.aumid = text("Other.App");
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.aumid = text("Valid.Appx");
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    static const uint8_t NulIdentity[] = { 'V','a','l','i','d','.','A','p','p',0,'x' };
    candidate.identity = (app_catalog_bytes_t){ NulIdentity, sizeof(NulIdentity) };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    uint8_t relative[] = { 'a',0,'\\',0,'b',0 };
    candidate.identity_kind = AppCatalogIdentityCanonicalPath;
    candidate.identity = (app_catalog_bytes_t){ relative, sizeof(relative) };
    candidate.aumid = (app_catalog_bytes_t){0};
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t lowercase_path[] = { 'C',0,':',0,'\\',0,'a',0,'.',0,'e',0,'x',0,'e',0 };
    candidate.identity = (app_catalog_bytes_t){ lowercase_path, sizeof(lowercase_path) };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    candidate = make_candidate(AppCatalogSourceUserPrograms, "Valid.App", "C:\\Apps\\a.exe", "App", "Programs\\a.lnk");
    candidate.desktop_shortcut = true;
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    assert(snapshot.count == 0 && snapshot.entries == NULL);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.package_family = text("not-a-package-family");
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    static const char *InvalidFamilies[] = {
        "A", "_abcdefg123456", "A_abcdefg12345", "A__abcdefg123456",
        "A_ABCDEFGHIJKLM", "A_abcdefg12345!", "A_abcdefg123456_",
        "A!_abcdefg123456",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789x_abcdefg123456"
    };
    for (size_t index = 0; index < sizeof(InvalidFamilies) / sizeof(InvalidFamilies[0]); index++) {
        candidate.package_family = text(InvalidFamilies[index]);
        assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    }
    candidate.package_family = text("Aa1.-_abcdefg123456");
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusOk);
    assert((snapshot.entries[0].app_flags & InteropAppPackaged) != 0);
    app_catalog_snapshot_deinit(&snapshot);
    candidate.package_family = text("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_abcdefg123456");
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusOk);
    app_catalog_snapshot_deinit(&snapshot);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.arguments = (app_catalog_bytes_t){ NULL, 1 };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    static const uint8_t BadTarget[] = { 0xC0, 0xAF };
    candidate.target_path = (app_catalog_bytes_t){ BadTarget, sizeof(BadTarget) };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.working_directory = (app_catalog_bytes_t){ NULL, 1 };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.source_identifier = (app_catalog_bytes_t){0};
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.aumid = (app_catalog_bytes_t){ BadTarget, sizeof(BadTarget) };
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidText);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.identity_kind = (app_catalog_identity_kind_t)99;
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    candidate.source = (app_catalog_source_t)99;
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    candidate.source = (app_catalog_source_t)0;
    assert(app_catalog_snapshot_build(allocator, &candidate, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    app_catalog_snapshot_deinit(&snapshot);
    assert(state.live == 0 && state.bytes == 0);
}

static app_catalog_status_t build_identity(app_catalog_allocator_t allocator,
                                           app_catalog_candidate_t candidate,
                                           app_catalog_snapshot_t *snapshot) {
    return app_catalog_snapshot_build(allocator, &candidate, 1, snapshot);
}

static void test_utf8_and_path_edges(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_snapshot_t snapshot;
    app_catalog_snapshot_init(&snapshot);
    const uint8_t invalid[][5] = {
        { 0xE2, 0x82, 0 }, { 0xF0, 0x9F, 0x92, 0 }, { 0xC2, 'x', 0 },
        { 0xE0, 0x80, 0x80, 0 }, { 0xED, 0xA0, 0x80, 0 }, { 0xF4, 0x90, 0x80, 0x80 },
        { 0xF0, 0x80, 0x80, 0x80 }, { 0xF5, 0x80, 0x80, 0x80 }
    };
    const size_t lengths[] = { 2, 3, 2, 3, 3, 4, 4, 4 };
    for (size_t index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
        app_catalog_candidate_t candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
        candidate.display_name = (app_catalog_bytes_t){ invalid[index], lengths[index] };
        assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidText);
    }
    app_catalog_candidate_t candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    static const uint8_t ValidTwo[] = { 0xC3, 0xA9 };
    candidate.display_name = (app_catalog_bytes_t){ ValidTwo, sizeof(ValidTwo) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusOk);
    app_catalog_snapshot_deinit(&snapshot);

    uint8_t odd_path[] = { 'C', 0, ':' };
    candidate = make_candidate(AppCatalogSourceUserPrograms, "C:\\Apps\\a.exe", "C:\\Apps\\a.exe", "App", "Programs\\a.lnk");
    candidate.identity_kind = AppCatalogIdentityCanonicalPath;
    static uint8_t path_identity_base[] = { 'C',0,':',0,'\\',0,'A',0,'.',0,'E',0,'X',0,'E',0 };
    candidate.identity = (app_catalog_bytes_t){ path_identity_base, sizeof(path_identity_base) };
    candidate.aumid = text("unexpected");
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    candidate.aumid = (app_catalog_bytes_t){0};
    candidate.target_path = (app_catalog_bytes_t){0};
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    candidate.target_path = text("C:\\Apps\\a.exe");
    candidate.identity = (app_catalog_bytes_t){0};
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    candidate.identity = (app_catalog_bytes_t){ odd_path, sizeof(odd_path) };
    candidate.aumid = (app_catalog_bytes_t){0};
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t too_short_path[] = { 'C',0,':',0 };
    candidate.identity = (app_catalog_bytes_t){ too_short_path, sizeof(too_short_path) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t missing_root_separator[] = { 'C',0,':',0,'X',0 };
    candidate.identity = (app_catalog_bytes_t){ missing_root_separator, sizeof(missing_root_separator) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t dot_path[] = { 'C',0,':',0,'\\',0,'.',0,'\\',0,'x',0 };
    candidate.identity = (app_catalog_bytes_t){ dot_path, sizeof(dot_path) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t parent_path[] = { 'C',0,':',0,'\\',0,'.',0,'.',0,'\\',0,'x',0 };
    candidate.identity = (app_catalog_bytes_t){ parent_path, sizeof(parent_path) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t slash_path[] = { 'C',0,':',0,'/',0,'x',0 };
    candidate.identity = (app_catalog_bytes_t){ slash_path, sizeof(slash_path) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t bad_surrogate[] = { 'C',0,':',0,'\\',0,0x00,0xD8 };
    candidate.identity = (app_catalog_bytes_t){ bad_surrogate, sizeof(bad_surrogate) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t lone_low_surrogate[] = { 'C',0,':',0,'\\',0,0x00,0xDC };
    candidate.identity = (app_catalog_bytes_t){ lone_low_surrogate, sizeof(lone_low_surrogate) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t nul_path[] = { 'C',0,':',0,'\\',0,'X',0,0,0 };
    candidate.identity = (app_catalog_bytes_t){ nul_path, sizeof(nul_path) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t unpaired_high_last[] = { 'C',0,':',0,'\\',0,'X',0,0x00,0xD8 };
    candidate.identity = (app_catalog_bytes_t){ unpaired_high_last, sizeof(unpaired_high_last) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t high_before_ascii[] = { 'C',0,':',0,'\\',0,0x00,0xD8,'X',0 };
    candidate.identity = (app_catalog_bytes_t){ high_before_ascii, sizeof(high_before_ascii) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t high_before_private[] = { 'C',0,':',0,'\\',0,0x00,0xD8,0x00,0xE0 };
    candidate.identity = (app_catalog_bytes_t){ high_before_private, sizeof(high_before_private) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t repeated_separator[] = { 'C',0,':',0,'\\',0,'\\',0,'X',0 };
    candidate.identity = (app_catalog_bytes_t){ repeated_separator, sizeof(repeated_separator) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t trailing_separator[] = { '\\',0,'\\',0,'S',0,'E',0,'R',0,'V',0,'E',0,'R',0,'\\',0,'S',0,'H',0,'A',0,'R',0,'E',0,'\\',0 };
    candidate.identity = (app_catalog_bytes_t){ trailing_separator, sizeof(trailing_separator) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t valid_unc[] = { '\\',0,'\\',0,'S',0,'E',0,'R',0,'V',0,'E',0,'R',0,'\\',0,'S',0,'H',0,'A',0,'R',0,'E',0 };
    candidate.identity = (app_catalog_bytes_t){ valid_unc, sizeof(valid_unc) };
    candidate.target_path = text("\\\\SERVER\\SHARE");
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusOk);
    app_catalog_snapshot_deinit(&snapshot);
    uint8_t valid_surrogate[] = { 'C',0,':',0,'\\',0,0x3D,0xD8,0x00,0xDE };
    candidate.identity = (app_catalog_bytes_t){ valid_surrogate, sizeof(valid_surrogate) };
    candidate.target_path = text("C:\\\xF0\x9F\x98\x80");
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusOk);
    app_catalog_snapshot_deinit(&snapshot);
    uint8_t final_dot[] = { 'C',0,':',0,'\\',0,'X',0,'\\',0,'.',0 };
    candidate.identity = (app_catalog_bytes_t){ final_dot, sizeof(final_dot) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t final_parent[] = { 'C',0,':',0,'\\',0,'X',0,'\\',0,'.',0,'.',0 };
    candidate.identity = (app_catalog_bytes_t){ final_parent, sizeof(final_parent) };
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t short_unc[] = { '\\',0,'\\',0,'S',0 };
    candidate.identity = (app_catalog_bytes_t){ short_unc, sizeof(short_unc) };
    candidate.target_path = text("\\\\S");
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t lowercase_drive[] = { 'c',0,':',0,'\\',0,'X',0 };
    candidate.identity = (app_catalog_bytes_t){ lowercase_drive, sizeof(lowercase_drive) };
    candidate.target_path = text("c:\\X");
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t nonletter_drive[] = { '0',0,':',0,'\\',0,'X',0 };
    candidate.identity = (app_catalog_bytes_t){ nonletter_drive, sizeof(nonletter_drive) };
    candidate.target_path = text("0:\\X");
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusInvalidPathIdentity);
    uint8_t drive_root[] = { 'C',0,':',0,'\\',0 };
    candidate.identity = (app_catalog_bytes_t){ drive_root, sizeof(drive_root) };
    candidate.target_path = text("C:\\");
    assert(build_identity(allocator, candidate, &snapshot) == AppCatalogStatusOk);
    app_catalog_snapshot_deinit(&snapshot);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_key_collision_and_unicode(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_candidate_t collisions[2] = {
        make_candidate(AppCatalogSourceAppsFolder, "x\xf0\x96\xb1\xaf" "App", "C:\\a.exe", "One", "apps:a"),
        make_candidate(AppCatalogSourceAppsFolder, "x\xf0\xa1\xaa\x98" "App", "C:\\b.exe", "Two", "apps:b")
    };
    /* Independent SHA-256 vectors produce the same sanitized 32-bit suffix. */
    app_catalog_snapshot_t snapshot;
    app_catalog_snapshot_init(&snapshot);
    assert(app_catalog_snapshot_build(allocator, collisions, 2, &snapshot) == AppCatalogStatusIdentityCollision);
    assert(snapshot.count == 0 && snapshot.entries == NULL);
    app_catalog_candidate_t unicode = make_candidate(AppCatalogSourceAppsFolder, "Éditeur.App", "C:\\unicode.exe", "Éditeur", "apps:unicode");
    assert(app_catalog_snapshot_build(allocator, &unicode, 1, &snapshot) == AppCatalogStatusOk);
    assert(snapshot.count == 1);
    app_catalog_snapshot_deinit(&snapshot);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_diff_kinds_and_noop(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_candidate_t old_candidates[2] = {
        make_candidate(AppCatalogSourceAppsFolder, "App.A", "C:\\a.exe", "A", "apps:a"),
        make_candidate(AppCatalogSourceAppsFolder, "App.B", "C:\\b.exe", "B", "apps:b")
    };
    app_catalog_candidate_t new_candidates[2] = {
        make_candidate(AppCatalogSourceAppsFolder, "App.A", "C:\\a.exe", "A updated", "apps:a"),
        make_candidate(AppCatalogSourceAppsFolder, "App.C", "C:\\c.exe", "C", "apps:c")
    };
    app_catalog_snapshot_t before, after, same;
    app_catalog_snapshot_init(&before); app_catalog_snapshot_init(&after); app_catalog_snapshot_init(&same);
    assert(app_catalog_snapshot_build(allocator, old_candidates, 2, &before) == AppCatalogStatusOk);
    assert(app_catalog_snapshot_build(allocator, new_candidates, 2, &after) == AppCatalogStatusOk);
    assert(app_catalog_snapshot_build(allocator, old_candidates, 2, &same) == AppCatalogStatusOk);
    app_catalog_diff_t diff = {0};
    assert(app_catalog_diff_build(&before, &after, &diff) == AppCatalogStatusOk);
    assert(diff.count == 3);
    assert(diff.changes[0].kind == AppCatalogChangeUpdate);
    assert(diff.changes[1].kind == AppCatalogChangeRemove);
    assert(diff.changes[2].kind == AppCatalogChangeAdd);
    app_catalog_diff_deinit(&diff);
    assert(app_catalog_diff_build(&before, &same, &diff) == AppCatalogStatusOk);
    assert(diff.count == 0 && diff.changes == NULL);
    app_catalog_diff_deinit(&diff);
    app_catalog_snapshot_t empty_before, empty_after;
    app_catalog_snapshot_init(&empty_before);
    app_catalog_snapshot_init(&empty_after);
    assert(app_catalog_snapshot_build(allocator, NULL, 0, &empty_before) == AppCatalogStatusOk);
    assert(app_catalog_snapshot_build(allocator, NULL, 0, &empty_after) == AppCatalogStatusOk);
    assert(app_catalog_diff_build(&empty_before, &empty_after, &diff) == AppCatalogStatusOk);
    assert(diff.count == 0 && diff.changes == NULL);
    app_catalog_diff_deinit(&diff);
    app_catalog_snapshot_deinit(&empty_before);
    app_catalog_snapshot_deinit(&empty_after);
    app_catalog_snapshot_deinit(&before); app_catalog_snapshot_deinit(&after); app_catalog_snapshot_deinit(&same);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_allocator_failure_sweep(void) {
    app_catalog_candidate_t candidates[2] = {
        make_candidate(AppCatalogSourceAppsFolder, "App.One", "C:\\one.exe", "One", "apps:one"),
        make_candidate(AppCatalogSourceUserPrograms, "App.Two", "C:\\two.exe", "Two", "C:\\Programs\\two.lnk")
    };
    bool reached_success = false;
    for (size_t fail_at = 1; fail_at <= 4; fail_at++) {
        allocation_state_t state = { .fail_at = fail_at };
        app_catalog_allocator_t allocator = allocator_for(&state);
        app_catalog_snapshot_t snapshot;
        app_catalog_snapshot_init(&snapshot);
        app_catalog_status_t status = app_catalog_snapshot_build(allocator, candidates, 2, &snapshot);
        if (status == AppCatalogStatusOk) reached_success = true;
        else assert(status == AppCatalogStatusOutOfMemory);
        app_catalog_snapshot_deinit(&snapshot);
        assert(state.live == 0 && state.bytes == 0);
    }
    assert(reached_success);

    allocation_state_t diff_state = {0}, after_snapshot_state = {0};
    app_catalog_allocator_t diff_allocator = allocator_for(&diff_state);
    app_catalog_allocator_t after_allocator = allocator_for(&after_snapshot_state);
    app_catalog_snapshot_t before, after;
    app_catalog_snapshot_init(&before); app_catalog_snapshot_init(&after);
    assert(app_catalog_snapshot_build(diff_allocator, candidates, 2, &before) == AppCatalogStatusOk);
    assert(app_catalog_snapshot_build(after_allocator, candidates, 2, &after) == AppCatalogStatusOk);
    app_catalog_diff_t diff = {0};
    diff_state.fail_at = diff_state.calls + 1;
    assert(app_catalog_diff_build(&before, &after, &diff) == AppCatalogStatusOutOfMemory);
    assert(diff.changes == NULL && diff.count == 0);
    diff_state.fail_at = 0;
    app_catalog_diff_deinit(&diff);
    app_catalog_snapshot_deinit(&before); app_catalog_snapshot_deinit(&after);
    assert(diff_state.live == 0 && after_snapshot_state.live == 0);
}

static void test_public_argument_and_quota_edges(void) {
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    app_catalog_snapshot_t snapshot;
    app_catalog_snapshot_init(&snapshot);
    app_catalog_candidate_t candidate = make_candidate(AppCatalogSourceAppsFolder, "Valid.App", "C:\\Apps\\a.exe", "App", "apps:a");
    assert(app_catalog_snapshot_build((app_catalog_allocator_t){0}, &candidate, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    app_catalog_allocator_t incomplete = allocator;
    incomplete.deallocate = NULL;
    assert(app_catalog_snapshot_build(incomplete, &candidate, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    assert(app_catalog_snapshot_build(allocator, &candidate, AppCatalogMaxEntries + 1u, &snapshot) == AppCatalogStatusQuota);
    assert(app_catalog_snapshot_build(allocator, NULL, 1, &snapshot) == AppCatalogStatusInvalidArgument);
    assert(app_catalog_snapshot_build(allocator, NULL, 0, &snapshot) == AppCatalogStatusOk);
    assert(snapshot.count == 0 && snapshot.allocator.context == &state);
    app_catalog_snapshot_deinit(&snapshot);

    app_catalog_snapshot_init(&snapshot);
    app_catalog_candidate_t *many = calloc(2049, sizeof(*many));
    assert(many != NULL);
    uint8_t *long_aumid = malloc(4096);
    assert(long_aumid != NULL);
    memset(long_aumid, 'A', 4096);
    for (size_t index = 0; index < 2049; index++) {
        many[index] = candidate;
        many[index].identity = (app_catalog_bytes_t){ long_aumid, 4096 };
        many[index].aumid = (app_catalog_bytes_t){ long_aumid, 4096 };
    }
    assert(app_catalog_snapshot_build(allocator, many, 2049, &snapshot) == AppCatalogStatusQuota);
    assert(snapshot.entries == NULL && snapshot.count == 0);
    free(long_aumid);
    free(many);

    const size_t storage_candidate_count = 2081;
    const size_t storage_identity_size = 4000;
    many = calloc(storage_candidate_count, sizeof(*many));
    long_aumid = malloc(storage_candidate_count * storage_identity_size);
    assert(many != NULL && long_aumid != NULL);
    for (size_t index = 0; index < storage_candidate_count; index++) {
        uint8_t *identity = long_aumid + index * storage_identity_size;
        memset(identity, 'A', storage_identity_size);
        size_t value = index;
        for (size_t digit = 0; digit < 4; digit++) {
            identity[storage_identity_size - 1 - digit] = (uint8_t)('0' + value % 10);
            value /= 10;
        }
        many[index] = make_candidate(AppCatalogSourceAppsFolder, "placeholder", "", "A", "apps:x");
        many[index].identity = (app_catalog_bytes_t){ identity, storage_identity_size };
        many[index].aumid = many[index].identity;
        many[index].working_directory = (app_catalog_bytes_t){0};
    }
    assert(app_catalog_snapshot_build(allocator, many, storage_candidate_count, &snapshot) == AppCatalogStatusQuota);
    assert(snapshot.entries == NULL && snapshot.count == 0);
    free(long_aumid);
    free(many);
    app_catalog_snapshot_deinit(&snapshot);
    assert(state.live == 0 && state.bytes == 0);
}

static void test_deinit_rejects_unowned_malformed_buffers(void) {
    app_catalog_snapshot_t malformed_snapshot = {0};
    app_catalog_entry_t invalid_entry = {0};
    uint8_t invalid_storage = 0;
    malformed_snapshot.entries = &invalid_entry;
    malformed_snapshot.storage = &invalid_storage;
    malformed_snapshot.count = 1;
    app_catalog_snapshot_deinit(&malformed_snapshot);
    assert(malformed_snapshot.entries == NULL && malformed_snapshot.storage == NULL &&
           malformed_snapshot.count == 0);

    app_catalog_diff_t malformed_diff = {0};
    app_catalog_change_t invalid_change = {0};
    malformed_diff.changes = &invalid_change;
    malformed_diff.count = 1;
    app_catalog_diff_deinit(&malformed_diff);
    assert(malformed_diff.changes == NULL && malformed_diff.count == 0);
}

static void test_native_cleanup_owner_empty_platform_behavior(void) {
    app_catalog_cleanup_owner_t owner;
    memset(&owner, 0xA5, sizeof(owner));
    app_catalog_cleanup_owner_init(NULL);
    app_catalog_cleanup_owner_init(&owner);
    assert(owner.count == 0 && app_catalog_cleanup_owner_retry(&owner) == AppCatalogStatusOk);
    assert(app_catalog_cleanup_owner_retry(NULL) == AppCatalogStatusInvalidArgument);
}

int main(void) {
    test_single_and_canonical_path();
    test_duplicate_merge_and_order();
    test_complete_tuple_tie_breakers();
    test_null_and_malformed_public_objects();
    test_validation_atomicity();
    test_key_collision_and_unicode();
    test_utf8_and_path_edges();
    test_public_argument_and_quota_edges();
    test_diff_kinds_and_noop();
    test_allocator_failure_sweep();
    test_deinit_rejects_unowned_malformed_buffers();
    test_native_cleanup_owner_empty_platform_behavior();
    app_catalog_snapshot_t unchanged;
    app_catalog_snapshot_init(&unchanged);
    app_catalog_enumeration_options_t options = {0};
#ifndef _WIN32
    assert(app_catalog_enumerate((app_catalog_allocator_t){0}, &options, &unchanged) == AppCatalogStatusUnsupportedPlatform);
#else
    allocation_state_t state = {0};
    app_catalog_allocator_t allocator = allocator_for(&state);
    options.max_nodes = 1;
    options.is_cancelled = always_cancel;
    assert(app_catalog_enumerate(allocator, &options, &unchanged) == AppCatalogStatusCancelled);
    assert(unchanged.entries == NULL && unchanged.count == 0);
    options.is_cancelled = NULL;
    options.deadline_monotonic_ns = 1;
    assert(app_catalog_enumerate(allocator, &options, &unchanged) == AppCatalogStatusTimedOut);
    options.deadline_monotonic_ns = 0;
    options.max_nodes = 65537;
    assert(app_catalog_enumerate(allocator, &options, &unchanged) == AppCatalogStatusInvalidArgument);
#endif
    app_catalog_snapshot_deinit(&unchanged);
    puts("app_catalog: ok");
    return 0;
}
