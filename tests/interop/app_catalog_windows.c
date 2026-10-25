/** @file app_catalog_windows.c @brief Read-only STA catalog native fixtures. */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "../../src/interop/app_catalog.h"
#include "../../src/interop/app_catalog_validation.h"
#include "../../src/interop/app_catalog_windows_private.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_allocator_state_t {
    size_t live_allocations;
    size_t live_bytes;
} test_allocator_state_t;

typedef struct close_fault_state_t {
    app_catalog_cleanup_handle_kind_t next_kind;
    bool armed;
} close_fault_state_t;

static void *test_allocate(void *context, size_t size) {
    test_allocator_state_t *state = context;
    void *memory = malloc(size);
    if (memory != NULL) {
        state->live_allocations++;
        state->live_bytes += size;
    }
    return memory;
}

static void test_deallocate(void *context, void *memory, size_t size) {
    test_allocator_state_t *state = context;
    if (memory == NULL) return;
    if (state->live_allocations == 0 || state->live_bytes < size) abort();
    state->live_allocations--;
    state->live_bytes -= size;
    free(memory);
}

static bool always_cancel(void *context) {
    (void)context;
    return true;
}

static bool fail_next_close(void *context, app_catalog_cleanup_handle_kind_t kind) {
    close_fault_state_t *state = context;
    if (!state->armed || state->next_kind != kind) return false;
    state->armed = false;
    return true;
}

static bool create_directory(const wchar_t *path) {
    return CreateDirectoryW(path, NULL) != 0;
}

static bool make_child_path(wchar_t *output, size_t capacity, const wchar_t *parent, const wchar_t *child) {
    size_t parent_length = wcslen(parent);
    size_t child_length = wcslen(child);
    if (parent_length + child_length + 2 > capacity) return false;
    memcpy(output, parent, parent_length * sizeof(wchar_t));
    if (parent_length != 0 && output[parent_length - 1] != L'\\') output[parent_length++] = L'\\';
    memcpy(output + parent_length, child, (child_length + 1) * sizeof(wchar_t));
    return true;
}

static bool test_cleanup_owner_retry(app_catalog_cleanup_owner_t *owner,
                                     const wchar_t *target,
                                     const wchar_t *fixture_root) {
    HANDLE file = CreateFileW(target, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    owner->native_handles[0] = file;
    owner->kinds[0] = AppCatalogCleanupHandleWin32;
    owner->count = 1;
    if (app_catalog_cleanup_owner_retry(owner) != AppCatalogStatusOk || owner->count != 0) return false;

    wchar_t pattern[MAX_PATH];
    if (!make_child_path(pattern, MAX_PATH, fixture_root, L"*")) return false;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) return false;
    owner->native_handles[0] = search;
    owner->kinds[0] = AppCatalogCleanupHandleFind;
    owner->count = 1;
    return app_catalog_cleanup_owner_retry(owner) == AppCatalogStatusOk && owner->count == 0;
}

static bool create_shortcut(const wchar_t *link_path, const wchar_t *target,
                            const wchar_t *description, const wchar_t *arguments) {
    IShellLinkW *link = NULL;
    HRESULT result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                      &IID_IShellLinkW, (void **)&link);
    if (FAILED(result) || link == NULL) return false;
    bool ok = SUCCEEDED(IShellLinkW_SetPath(link, target)) &&
              SUCCEEDED(IShellLinkW_SetDescription(link, description)) &&
              SUCCEEDED(IShellLinkW_SetArguments(link, arguments));
    IPersistFile *persist = NULL;
    if (ok) ok = SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist)) && persist != NULL;
    if (ok) ok = SUCCEEDED(IPersistFile_Save(persist, link_path, TRUE));
    if (persist != NULL) IPersistFile_Release(persist);
    IShellLinkW_Release(link);
    return ok;
}

/* Hash only the two .lnk files created by this test inside its private temp directory. */
static bool hash_fixture_file(const wchar_t *path, char output[65],
                              app_catalog_cleanup_owner_t *cleanup_owner) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER file_size;
    bool ok = GetFileSizeEx(file, &file_size) != 0 && file_size.QuadPart > 0 &&
              file_size.QuadPart <= 1024 * 1024;
    uint8_t *bytes = ok ? malloc((size_t)file_size.QuadPart) : NULL;
    if (ok && bytes == NULL) ok = false;
    DWORD bytes_read = 0;
    if (ok) ok = ReadFile(file, bytes, (DWORD)file_size.QuadPart, &bytes_read, NULL) != 0 &&
                 bytes_read == (DWORD)file_size.QuadPart;
    if (CloseHandle(file) == 0) {
        ok = false;
        if (cleanup_owner->count < AppCatalogCleanupMaxHandles) {
            cleanup_owner->native_handles[cleanup_owner->count] = file;
            cleanup_owner->kinds[cleanup_owner->count] = AppCatalogCleanupHandleWin32;
            cleanup_owner->count++;
        }
    }
    if (ok) {
        static const char HexDigits[] = "0123456789abcdef";
        uint8_t digest[InteropHashBytes];
        interop_sha256(bytes, (size_t)file_size.QuadPart, digest);
        for (size_t index = 0; index < sizeof(digest); index++) {
            output[index * 2] = HexDigits[digest[index] >> 4];
            output[index * 2 + 1] = HexDigits[digest[index] & 0x0F];
        }
        output[64] = '\0';
    }
    free(bytes);
    return ok;
}

static bool entry_identity_matches(const app_catalog_entry_t *entry) {
    if (entry->identity_kind == AppCatalogIdentityAumid) {
        uint8_t key[InteropMaxAppKeyBytes];
        size_t key_length = 0;
        if (entry->identity.length != entry->aumid.length ||
            memcmp(entry->identity.data, entry->aumid.data, entry->identity.length) != 0 ||
            interop_app_key_from_aumid(entry->aumid.data, entry->aumid.length, key, sizeof(key), &key_length) != InteropErrorNone)
            return false;
        return key_length == entry->app_key.length && memcmp(key, entry->app_key.data, key_length) == 0;
    }
    if (entry->identity_kind != AppCatalogIdentityCanonicalPath ||
        !app_catalog_path_identity_valid(entry->identity.data, entry->identity.length,
                                         AppCatalogMaxPathUnits)) return false;
    static const uint8_t HexDigits[] = "0123456789abcdef";
    uint8_t digest[InteropHashBytes];
    uint8_t key[34] = { 'p', '-' };
    interop_sha256(entry->identity.data, entry->identity.length, digest);
    for (size_t index = 0; index < 16; index++) {
        key[2 + index * 2] = HexDigits[digest[index] >> 4];
        key[3 + index * 2] = HexDigits[digest[index] & 0x0F];
    }
    return entry->app_key.length == sizeof(key) && memcmp(entry->app_key.data, key, sizeof(key)) == 0;
}

#define Require(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "app_catalog_windows:%d: fixture assertion failed: %s\n", __LINE__, #condition); \
        ok = false; \
        goto cleanup; \
    } \
} while (0)

int main(void) {
    bool ok = true;
    bool com_initialized = false;
    wchar_t temporary_name[MAX_PATH];
    wchar_t fixture_root[MAX_PATH];
    wchar_t temp_file[MAX_PATH];
    wchar_t roots_storage[4][MAX_PATH];
    wchar_t program_link[MAX_PATH];
    wchar_t desktop_link[MAX_PATH];
    wchar_t target[MAX_PATH];
    char program_link_hash[65] = {0};
    char desktop_link_hash[65] = {0};
    memset(temporary_name, 0, sizeof(temporary_name));
    memset(fixture_root, 0, sizeof(fixture_root));
    memset(temp_file, 0, sizeof(temp_file));
    memset(roots_storage, 0, sizeof(roots_storage));
    memset(program_link, 0, sizeof(program_link));
    memset(desktop_link, 0, sizeof(desktop_link));
    memset(target, 0, sizeof(target));
    test_allocator_state_t allocation = {0};
    app_catalog_allocator_t allocator = { &allocation, test_allocate, test_deallocate };
    app_catalog_snapshot_t before, after, smoke;
    app_catalog_snapshot_t failed;
    app_catalog_snapshot_init(&before);
    app_catalog_snapshot_init(&after);
    app_catalog_snapshot_init(&smoke);
    app_catalog_snapshot_init(&failed);
    app_catalog_diff_t diff = {0};
    app_catalog_cleanup_owner_t cleanup_owner;
    app_catalog_cleanup_owner_init(&cleanup_owner);
    app_catalog_enumeration_options_t options = {0};
    options.cleanup_owner = &cleanup_owner;
    app_catalog_windows_source_roots_t roots = {0};
    bool temp_file_exists = false;
    bool fixture_exists = false;
    bool program_exists = false;
    bool common_exists = false;
    bool user_desktop_exists = false;
    bool public_desktop_exists = false;
    bool program_link_exists = false;
    bool desktop_link_exists = false;

    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    Require(SUCCEEDED(initialized));
    com_initialized = true;

    DWORD module_length = GetModuleFileNameW(NULL, target, MAX_PATH);
    Require(module_length != 0 && module_length < MAX_PATH);
    DWORD temp_length = GetTempPathW(MAX_PATH, temporary_name);
    Require(temp_length != 0 && temp_length < MAX_PATH);
    Require(GetTempFileNameW(temporary_name, L"wac", 0, temp_file) != 0);
    temp_file_exists = true;
    Require(DeleteFileW(temp_file) != 0);
    temp_file_exists = false;
    wcscpy(fixture_root, temp_file);
    Require(create_directory(fixture_root));
    fixture_exists = true;
    Require(make_child_path(roots_storage[0], MAX_PATH, fixture_root, L"user-programs"));
    Require(make_child_path(roots_storage[1], MAX_PATH, fixture_root, L"common-programs"));
    Require(make_child_path(roots_storage[2], MAX_PATH, fixture_root, L"user-desktop"));
    Require(make_child_path(roots_storage[3], MAX_PATH, fixture_root, L"public-desktop"));
    Require(create_directory(roots_storage[0])); program_exists = true;
    Require(create_directory(roots_storage[1])); common_exists = true;
    Require(create_directory(roots_storage[2])); user_desktop_exists = true;
    Require(create_directory(roots_storage[3])); public_desktop_exists = true;
    Require(test_cleanup_owner_retry(&cleanup_owner, target, fixture_root));
    Require(make_child_path(program_link, MAX_PATH, roots_storage[0], L"catalog-fixture.lnk"));
    Require(make_child_path(desktop_link, MAX_PATH, roots_storage[2], L"catalog-fixture.lnk"));
    roots = (app_catalog_windows_source_roots_t){
        .user_programs = roots_storage[0],
        .common_programs = roots_storage[1],
        .user_desktop = roots_storage[2],
        .public_desktop = roots_storage[3],
    };

    options.max_nodes = 64;
    options.deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(30000000000);
    options.is_cancelled = always_cancel;
    Require(app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &before) == AppCatalogStatusCancelled);
    Require(before.count == 0 && before.entries == NULL);
    options.is_cancelled = NULL;
    options.deadline_monotonic_ns = 1;
    Require(app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &before) == AppCatalogStatusTimedOut);
    options.deadline_monotonic_ns = 0;
    options.max_nodes = 1;
    Require(app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &before) == AppCatalogStatusQuota);
    options.max_nodes = 64;

    Require(create_shortcut(program_link, target, L"Catalog fixture", L""));
    program_link_exists = true;
    Require(hash_fixture_file(program_link, program_link_hash, &cleanup_owner));
    close_fault_state_t close_fault = { AppCatalogCleanupHandleWin32, true };
    roots.close_fault = fail_next_close;
    roots.close_fault_context = &close_fault;
    Require(app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &failed) == AppCatalogStatusNativeFailure);
    Require(failed.count == 0 && failed.entries == NULL && cleanup_owner.count == 1);
    Require(cleanup_owner.kinds[0] == AppCatalogCleanupHandleWin32);
    Require(app_catalog_cleanup_owner_retry(&cleanup_owner) == AppCatalogStatusOk && cleanup_owner.count == 0);

    close_fault = (close_fault_state_t){ AppCatalogCleanupHandleFind, true };
    Require(app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &failed) == AppCatalogStatusNativeFailure);
    Require(failed.count == 0 && failed.entries == NULL && cleanup_owner.count == 1);
    Require(cleanup_owner.kinds[0] == AppCatalogCleanupHandleFind);
    Require(app_catalog_cleanup_owner_retry(&cleanup_owner) == AppCatalogStatusOk && cleanup_owner.count == 0);
    roots.close_fault = NULL;
    roots.close_fault_context = NULL;
    Require(app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &before) == AppCatalogStatusOk);
    Require(before.count == 1 && before.entries[0].identity_kind == AppCatalogIdentityCanonicalPath);
    Require(before.entries[0].app_flags == 0 && before.duplicate_launch_conflicts == 0);
    Require(before.entries[0].target_path.length != 0 && entry_identity_matches(&before.entries[0]));

    Require(create_shortcut(desktop_link, target, L"Catalog fixture desktop", L"--fixture"));
    desktop_link_exists = true;
    Require(hash_fixture_file(desktop_link, desktop_link_hash, &cleanup_owner));
    printf("fixture_sha256 programs=%s desktop=%s\n", program_link_hash, desktop_link_hash);
    Require(app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &after) == AppCatalogStatusOk);
    Require(after.count == 1 && after.entries[0].identity_kind == AppCatalogIdentityCanonicalPath);
    Require(after.entries[0].app_flags == InteropAppDesktopShortcut);
    Require(after.duplicate_launch_conflicts == 1 && entry_identity_matches(&after.entries[0]));
    Require(after.entries[0].arguments.length == 0);
    Require(app_catalog_diff_build(&before, &after, &diff) == AppCatalogStatusOk);
    Require(diff.count == 1 && diff.changes[0].kind == AppCatalogChangeUpdate);
    Require(diff.changes[0].before == &before.entries[0] && diff.changes[0].after == &after.entries[0]);
    app_catalog_diff_deinit(&diff);

    app_catalog_enumeration_options_t smoke_options = {0};
    smoke_options.cleanup_owner = &cleanup_owner;
    smoke_options.max_nodes = 65536;
    smoke_options.deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(30000000000);
    Require(app_catalog_enumerate(allocator, &smoke_options, &smoke) == AppCatalogStatusOk);
    Require(smoke.count <= AppCatalogMaxEntries);
    for (size_t index = 0; index < smoke.count; index++) Require(entry_identity_matches(&smoke.entries[index]));

cleanup:
    app_catalog_diff_deinit(&diff);
    app_catalog_snapshot_deinit(&before);
    app_catalog_snapshot_deinit(&after);
    app_catalog_snapshot_deinit(&failed);
    app_catalog_snapshot_deinit(&smoke);
    if (cleanup_owner.count != 0) {
        if (app_catalog_cleanup_owner_retry(&cleanup_owner) != AppCatalogStatusOk) {
            fprintf(stderr, "app_catalog_windows: native close remains quarantined\n");
            ok = false;
        }
    }
    if (desktop_link_exists) DeleteFileW(desktop_link);
    if (program_link_exists) DeleteFileW(program_link);
    if (public_desktop_exists) RemoveDirectoryW(roots_storage[3]);
    if (user_desktop_exists) RemoveDirectoryW(roots_storage[2]);
    if (common_exists) RemoveDirectoryW(roots_storage[1]);
    if (program_exists) RemoveDirectoryW(roots_storage[0]);
    if (fixture_exists) RemoveDirectoryW(fixture_root);
    if (temp_file_exists) DeleteFileW(temp_file);
    if (com_initialized) CoUninitialize();
    if (allocation.live_allocations != 0 || allocation.live_bytes != 0) {
        fprintf(stderr, "app_catalog_windows: leaked %zu allocations / %zu bytes\n",
                allocation.live_allocations, allocation.live_bytes);
        ok = false;
    }
    if (ok) puts("app_catalog_windows: ok");
    return ok ? 0 : 1;
}

#undef Require
#else
int main(void) { return 0; }
#endif
