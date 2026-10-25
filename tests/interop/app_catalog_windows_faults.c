/** @file app_catalog_windows_faults.c @brief Native provider failure qualification.
 *
 * This private fixture includes the exact production provider source once
 * with one-shot, fail-before-consumption wrappers around selected Win32
 * calls and CRT allocations. Every successful call still reaches the native
 * implementation. It owns only a unique temporary root and real shortcut
 * files beneath it.
 */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "../../src/interop/app_catalog.h"
#include "../../src/interop/app_catalog_windows_private.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum catalog_fault_kind_t {
    CatalogFaultNone,
    CatalogFaultMalloc,
    CatalogFaultRealloc,
    CatalogFaultUtf8Conversion,
    CatalogFaultIdentityMapping,
    CatalogFaultCreateFile,
    CatalogFaultFinalPath,
    CatalogFaultFindFirst,
    CatalogFaultFindNext,
    CatalogFaultFindClose,
    CatalogFaultCloseHandle,
    CatalogFaultComInitialize,
} catalog_fault_kind_t;

typedef struct catalog_fault_state_t {
    catalog_fault_kind_t kind;
    size_t fail_at;
    size_t calls;
} catalog_fault_state_t;

static catalog_fault_state_t CatalogFaultState;

static bool catalog_fault_hit(catalog_fault_kind_t kind) {
    if (CatalogFaultState.kind != kind) return false;
    CatalogFaultState.calls++;
    return CatalogFaultState.calls == CatalogFaultState.fail_at;
}

static void *catalog_fault_malloc(size_t size) {
    if (catalog_fault_hit(CatalogFaultMalloc)) return NULL;
    return malloc(size);
}

static void *catalog_fault_realloc(void *pointer, size_t size) {
    if (catalog_fault_hit(CatalogFaultRealloc)) return NULL;
    return realloc(pointer, size);
}

static int WINAPI catalog_fault_wide_char_to_multi_byte(UINT code_page, DWORD flags,
    LPCWCH wide, int wide_length, LPSTR output, int output_length,
    LPCCH default_character, LPBOOL used_default_character) {
    if (catalog_fault_hit(CatalogFaultUtf8Conversion)) {
        SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    return WideCharToMultiByte(code_page, flags, wide, wide_length, output, output_length,
                               default_character, used_default_character);
}

static int WINAPI catalog_fault_lcmap_string_ex(LPCWSTR locale, DWORD flags,
    LPCWSTR source, int source_length, LPWSTR destination, int destination_length,
    LPNLSVERSIONINFO version, LPVOID reserved, LPARAM sort_handle) {
    if (catalog_fault_hit(CatalogFaultIdentityMapping)) {
        SetLastError(ERROR_INVALID_DATA);
        return 0;
    }
    return LCMapStringEx(locale, flags, source, source_length, destination,
                         destination_length, version, reserved, sort_handle);
}

static HANDLE WINAPI catalog_fault_create_file_w(LPCWSTR path, DWORD access,
    DWORD share, LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD attributes,
    HANDLE template_file) {
    if (catalog_fault_hit(CatalogFaultCreateFile)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(path, access, share, security, creation, attributes, template_file);
}

static DWORD WINAPI catalog_fault_get_final_path(HANDLE file, LPWSTR output,
    DWORD capacity, DWORD flags) {
    if (catalog_fault_hit(CatalogFaultFinalPath)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    return GetFinalPathNameByHandleW(file, output, capacity, flags);
}

static HANDLE WINAPI catalog_fault_find_first(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
    if (catalog_fault_hit(CatalogFaultFindFirst)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstFileW(pattern, data);
}

static BOOL WINAPI catalog_fault_find_next(HANDLE search, LPWIN32_FIND_DATAW data) {
    if (catalog_fault_hit(CatalogFaultFindNext)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return FindNextFileW(search, data);
}

static BOOL WINAPI catalog_fault_find_close(HANDLE search) {
    if (catalog_fault_hit(CatalogFaultFindClose)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return FindClose(search);
}

static BOOL WINAPI catalog_fault_close_handle(HANDLE handle) {
    if (catalog_fault_hit(CatalogFaultCloseHandle)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return CloseHandle(handle);
}

static HRESULT WINAPI catalog_fault_co_initialize(LPVOID reserved, DWORD flags) {
    if (catalog_fault_hit(CatalogFaultComInitialize)) return RPC_E_CHANGED_MODE;
    return CoInitializeEx(reserved, flags);
}

#define malloc catalog_fault_malloc
#define realloc catalog_fault_realloc
#define WideCharToMultiByte catalog_fault_wide_char_to_multi_byte
#define LCMapStringEx catalog_fault_lcmap_string_ex
#define CreateFileW catalog_fault_create_file_w
#define GetFinalPathNameByHandleW catalog_fault_get_final_path
#define FindFirstFileW catalog_fault_find_first
#define FindNextFileW catalog_fault_find_next
#define FindClose catalog_fault_find_close
#define CloseHandle catalog_fault_close_handle
#define CoInitializeEx catalog_fault_co_initialize
#include "../../src/interop/app_catalog_windows.c"
#undef CoInitializeEx
#undef CloseHandle
#undef FindClose
#undef FindNextFileW
#undef FindFirstFileW
#undef GetFinalPathNameByHandleW
#undef CreateFileW
#undef LCMapStringEx
#undef WideCharToMultiByte
#undef realloc
#undef malloc

typedef struct catalog_fake_item_t {
    IShellItem shell_item;
    IShellItem2 shell_item2;
    IPropertyStore property_store;
    ULONG references;
    const wchar_t *display_name;
    const wchar_t *parsing_name;
    const wchar_t *aumid;
    const wchar_t *target_path;
    const wchar_t *package_family;
    bool separator;
    bool fail_query;
    bool fail_store;
    bool fail_parsing_name;
    bool fail_display_name;
    bool fail_property;
} catalog_fake_item_t;

static catalog_fake_item_t *catalog_fake_from_shell(IShellItem *self) {
    return (catalog_fake_item_t *)((uint8_t *)self - offsetof(catalog_fake_item_t, shell_item));
}

static catalog_fake_item_t *catalog_fake_from_shell2(IShellItem2 *self) {
    return (catalog_fake_item_t *)((uint8_t *)self - offsetof(catalog_fake_item_t, shell_item2));
}

static catalog_fake_item_t *catalog_fake_from_store(IPropertyStore *self) {
    return (catalog_fake_item_t *)((uint8_t *)self - offsetof(catalog_fake_item_t, property_store));
}

static HRESULT STDMETHODCALLTYPE catalog_fake_shell_query(IShellItem *self, REFIID iid, void **object) {
    catalog_fake_item_t *item = catalog_fake_from_shell(self);
    if (object == NULL) return E_POINTER;
    *object = NULL;
    if (item->fail_query || !IsEqualIID(iid, &IID_IShellItem2)) return E_NOINTERFACE;
    item->references++;
    *object = &item->shell_item2;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE catalog_fake_shell_add_ref(IShellItem *self) {
    return ++catalog_fake_from_shell(self)->references;
}

static ULONG STDMETHODCALLTYPE catalog_fake_shell_release(IShellItem *self) {
    catalog_fake_item_t *item = catalog_fake_from_shell(self);
    if (item->references != 0) item->references--;
    return item->references;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_shell_get_name(IShellItem *self, SIGDN kind, LPWSTR *output) {
    catalog_fake_item_t *item = catalog_fake_from_shell(self);
    if (output == NULL) return E_POINTER;
    *output = NULL;
    if ((kind == SIGDN_NORMALDISPLAY && item->fail_display_name) ||
        (kind == SIGDN_DESKTOPABSOLUTEPARSING && item->fail_parsing_name)) return E_FAIL;
    const wchar_t *source = kind == SIGDN_NORMALDISPLAY ? item->display_name : item->parsing_name;
    if (source == NULL) return E_FAIL;
    size_t units = wcslen(source) + 1;
    *output = CoTaskMemAlloc(units * sizeof(wchar_t));
    if (*output == NULL) return E_OUTOFMEMORY;
    memcpy(*output, source, units * sizeof(wchar_t));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_shell2_get_store(IShellItem2 *self,
    GETPROPERTYSTOREFLAGS flags, REFIID iid, void **output) {
    (void)flags;
    catalog_fake_item_t *item = catalog_fake_from_shell2(self);
    if (output == NULL) return E_POINTER;
    *output = NULL;
    if (item->fail_store || !IsEqualIID(iid, &IID_IPropertyStore)) return E_FAIL;
    item->references++;
    *output = &item->property_store;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_shell2_query(IShellItem2 *self, REFIID iid, void **output) {
    catalog_fake_item_t *item = catalog_fake_from_shell2(self);
    if (output == NULL) return E_POINTER;
    *output = NULL;
    if (!IsEqualIID(iid, &IID_IShellItem2)) return E_NOINTERFACE;
    item->references++;
    *output = &item->shell_item2;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE catalog_fake_shell2_add_ref(IShellItem2 *self) {
    return ++catalog_fake_from_shell2(self)->references;
}

static ULONG STDMETHODCALLTYPE catalog_fake_shell2_release(IShellItem2 *self) {
    catalog_fake_item_t *item = catalog_fake_from_shell2(self);
    if (item->references != 0) item->references--;
    return item->references;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_store_query(IPropertyStore *self, REFIID iid, void **output) {
    (void)self;
    (void)iid;
    if (output != NULL) *output = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE catalog_fake_store_add_ref(IPropertyStore *self) {
    return ++catalog_fake_from_store(self)->references;
}

static ULONG STDMETHODCALLTYPE catalog_fake_store_release(IPropertyStore *self) {
    catalog_fake_item_t *item = catalog_fake_from_store(self);
    if (item->references != 0) item->references--;
    return item->references;
}

static HRESULT catalog_fake_property_string(PROPVARIANT *value, const wchar_t *text) {
    if (text == NULL) return S_FALSE;
    value->vt = VT_BSTR;
    value->bstrVal = SysAllocString(text);
    return value->bstrVal == NULL ? E_OUTOFMEMORY : S_OK;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_store_get_value(IPropertyStore *self,
    REFPROPERTYKEY key, PROPVARIANT *value) {
    catalog_fake_item_t *item = catalog_fake_from_store(self);
    const PROPERTYKEY *property_key = key;
    if (value == NULL) return E_POINTER;
    PropVariantInit(value);
    if (item->fail_property) return E_FAIL;
    if (property_key->pid == PKEY_AppUserModel_IsDestListSeparator.pid &&
        IsEqualGUID(&property_key->fmtid, &PKEY_AppUserModel_IsDestListSeparator.fmtid)) {
        value->vt = VT_BOOL;
        value->boolVal = item->separator ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }
    if (property_key->pid == PKEY_AppUserModel_ID.pid &&
        IsEqualGUID(&property_key->fmtid, &PKEY_AppUserModel_ID.fmtid)) return catalog_fake_property_string(value, item->aumid);
    if (property_key->pid == CatalogLinkTargetParsingPath.pid &&
        IsEqualGUID(&property_key->fmtid, &CatalogLinkTargetParsingPath.fmtid)) return catalog_fake_property_string(value, item->target_path);
    if (property_key->pid == CatalogPackageFamilyName.pid &&
        IsEqualGUID(&property_key->fmtid, &CatalogPackageFamilyName.fmtid)) return catalog_fake_property_string(value, item->package_family);
    return S_FALSE;
}

static IShellItemVtbl CatalogFakeShellItemVtbl = {
    .QueryInterface = catalog_fake_shell_query,
    .AddRef = catalog_fake_shell_add_ref,
    .Release = catalog_fake_shell_release,
    .GetDisplayName = catalog_fake_shell_get_name,
};

static IShellItem2Vtbl CatalogFakeShellItem2Vtbl = {
    .QueryInterface = catalog_fake_shell2_query,
    .AddRef = catalog_fake_shell2_add_ref,
    .Release = catalog_fake_shell2_release,
    .GetPropertyStore = catalog_fake_shell2_get_store,
};

static IPropertyStoreVtbl CatalogFakePropertyStoreVtbl = {
    .QueryInterface = catalog_fake_store_query,
    .AddRef = catalog_fake_store_add_ref,
    .Release = catalog_fake_store_release,
    .GetValue = catalog_fake_store_get_value,
};

static void catalog_fake_item_init(catalog_fake_item_t *item) {
    memset(item, 0, sizeof(*item));
    item->shell_item.lpVtbl = &CatalogFakeShellItemVtbl;
    item->shell_item2.lpVtbl = &CatalogFakeShellItem2Vtbl;
    item->property_store.lpVtbl = &CatalogFakePropertyStoreVtbl;
    item->references = 1;
}

static bool catalog_fault_test_apps_folder_items(const wchar_t *target,
                                                  app_catalog_cleanup_owner_t *cleanup_owner) {
    app_catalog_enumeration_options_t options = {0};
    options.cleanup_owner = cleanup_owner;
    options.max_nodes = 32;
    options.deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(30000000000);
    catalog_collector_t collector = {0};
    collector.options = &options;
    collector.cleanup_owner = cleanup_owner;
    collector.storage = malloc(AppCatalogMaxBytes);
    if (collector.storage == NULL) return false;
    collector.storage_capacity = AppCatalogMaxBytes;
    collector.status = AppCatalogStatusOk;

    catalog_fake_item_t item;
    catalog_fake_item_init(&item);
    item.display_name = L"Fixture packaged app";
    item.parsing_name = L"shell:AppsFolder\\Fixture!App";
    item.aumid = L"Fixture.Package!Application";
    item.target_path = target;
    item.package_family = L"Fixture.Package_1234567890abc";
    bool ok = true;

    item.separator = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.separator = false;

    item.fail_query = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.fail_query = false;

    item.fail_store = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.fail_store = false;

    item.fail_property = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.fail_property = false;

    item.display_name = L"";
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.display_name = L"Fixture packaged app";

    item.fail_parsing_name = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 1 ||
        collector.candidates[0].identity_kind != AppCatalogIdentityAumid || collector.candidates[0].target_path.length == 0) ok = false;
    item.fail_parsing_name = false;
    free(collector.candidates);
    collector.candidates = NULL;
    collector.count = 0;
    collector.candidate_capacity = 0;
    collector.storage_used = 0;

    item.target_path = NULL;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 1 ||
        collector.candidates[0].identity_kind != AppCatalogIdentityAumid || collector.candidates[0].target_path.length != 0 ||
        collector.candidates[0].working_directory.length != 0) ok = false;
    free(collector.candidates);
    collector.candidates = NULL;
    collector.count = 0;
    collector.candidate_capacity = 0;
    collector.storage_used = 0;

    item.aumid = NULL;
    item.target_path = target;
    item.parsing_name = NULL;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 1 ||
        collector.candidates[0].identity_kind != AppCatalogIdentityCanonicalPath) ok = false;
    if (collector.status != AppCatalogStatusOk || cleanup_owner->count != 0) ok = false;
    if (item.references != 1) ok = false;
    free(collector.candidates);
    free(collector.storage);
    return ok;
}

static bool catalog_fault_make_path(wchar_t *output, size_t capacity,
                                    const wchar_t *parent, const wchar_t *child) {
    size_t parent_units = wcslen(parent);
    size_t child_units = wcslen(child);
    if (parent_units + child_units + 2 > capacity) return false;
    memcpy(output, parent, parent_units * sizeof(wchar_t));
    if (parent_units != 0 && output[parent_units - 1] != L'\\') output[parent_units++] = L'\\';
    memcpy(output + parent_units, child, (child_units + 1) * sizeof(wchar_t));
    return true;
}

static bool catalog_fault_create_shortcut(const wchar_t *link_path, const wchar_t *target) {
    IShellLinkW *link = NULL;
    HRESULT result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                      &IID_IShellLinkW, (void **)&link);
    if (FAILED(result) || link == NULL) return false;
    bool ok = SUCCEEDED(IShellLinkW_SetPath(link, target)) &&
              SUCCEEDED(IShellLinkW_SetDescription(link, L"Catalog fault fixture"));
    IPersistFile *persist = NULL;
    if (ok) ok = SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist)) && persist != NULL;
    if (ok) ok = SUCCEEDED(IPersistFile_Save(persist, link_path, TRUE));
    if (persist != NULL) IPersistFile_Release(persist);
    IShellLinkW_Release(link);
    return ok;
}

static app_catalog_status_t catalog_fault_enumerate(
    app_catalog_allocator_t allocator,
    app_catalog_enumeration_options_t *options,
    app_catalog_windows_source_roots_t *roots,
    app_catalog_snapshot_t *output,
    catalog_fault_kind_t kind,
    size_t fail_at) {
    CatalogFaultState = (catalog_fault_state_t){ kind, fail_at, 0 };
    options->deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(120000000000);
    app_catalog_status_t status = app_catalog_windows_enumerate_fixture_roots(allocator, options, roots, output);
    CatalogFaultState = (catalog_fault_state_t){ CatalogFaultNone, 0, 0 };
    return status;
}

static void *catalog_fault_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void catalog_fault_deallocate(void *context, void *memory, size_t size) {
    (void)context;
    (void)size;
    free(memory);
}

static bool catalog_fault_empty_snapshot(const app_catalog_snapshot_t *snapshot) {
    return snapshot->entries == NULL && snapshot->storage == NULL && snapshot->count == 0 &&
           snapshot->allocation_size == 0 && snapshot->storage_size == 0 &&
           snapshot->duplicate_launch_conflicts == 0 && snapshot->allocator.allocate == NULL &&
           snapshot->allocator.deallocate == NULL;
}

static bool catalog_fault_cancelled(void *context) {
    const bool *cancelled = context;
    return cancelled != NULL && *cancelled;
}

static void catalog_fault_remove_tree(const wchar_t *root, const wchar_t *link_directory,
                                      const wchar_t *target_path, size_t link_count) {
    (void)root;
    wchar_t leaf[MAX_PATH];
    for (size_t index = 0; index < link_count; index++) {
        wchar_t name[32];
        (void)swprintf(name, sizeof(name) / sizeof(name[0]), L"fault-%02zu.lnk", index);
        if (catalog_fault_make_path(leaf, MAX_PATH, link_directory, name)) (void)DeleteFileW(leaf);
    }
    (void)DeleteFileW(target_path);
    (void)link_directory;
}

int app_catalog_windows_fault_fixture_run(void) {
    bool ok = false;
    bool com_initialized = false;
    const char *stage = "setup";
    size_t stage_index = 0;
    wchar_t temp_directory[MAX_PATH] = {0};
    wchar_t temp_file[MAX_PATH] = {0};
    wchar_t root[MAX_PATH] = {0};
    wchar_t programs[MAX_PATH] = {0};
    wchar_t common[MAX_PATH] = {0};
    wchar_t desktop[MAX_PATH] = {0};
    wchar_t public_desktop[MAX_PATH] = {0};
    wchar_t target[MAX_PATH] = {0};
    wchar_t link_path[MAX_PATH] = {0};
    size_t link_count = 0;
    app_catalog_cleanup_owner_t cleanup_owner;
    app_catalog_cleanup_owner_init(&cleanup_owner);
    app_catalog_snapshot_t output;
    app_catalog_snapshot_init(&output);
    app_catalog_enumeration_options_t options = {0};
    options.cleanup_owner = &cleanup_owner;
    options.max_nodes = 1024;
    options.deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(120000000000);
    app_catalog_allocator_t allocator = { NULL, catalog_fault_allocate, catalog_fault_deallocate };
    app_catalog_windows_source_roots_t roots = {0};

    HRESULT init_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(init_result)) goto cleanup;
    com_initialized = true;
    DWORD temp_length = GetTempPathW(MAX_PATH, temp_directory);
    if (temp_length == 0 || temp_length >= MAX_PATH || GetTempFileNameW(temp_directory, L"acf", 0, temp_file) == 0) goto cleanup;
    if (!DeleteFileW(temp_file)) goto cleanup;
    if (wcscpy_s(root, MAX_PATH, temp_file) != 0 || !CreateDirectoryW(root, NULL)) goto cleanup;
    if (!catalog_fault_make_path(programs, MAX_PATH, root, L"programs") || !CreateDirectoryW(programs, NULL)) goto cleanup;
    if (!catalog_fault_make_path(common, MAX_PATH, root, L"common") || !CreateDirectoryW(common, NULL)) goto cleanup;
    if (!catalog_fault_make_path(desktop, MAX_PATH, root, L"desktop") || !CreateDirectoryW(desktop, NULL)) goto cleanup;
    if (!catalog_fault_make_path(public_desktop, MAX_PATH, root, L"public-desktop") || !CreateDirectoryW(public_desktop, NULL)) goto cleanup;
    if (!catalog_fault_make_path(target, MAX_PATH, root, L"target.exe")) goto cleanup;
    HANDLE target_file = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (target_file == INVALID_HANDLE_VALUE) goto cleanup;
    if (CloseHandle(target_file) == 0) {
        cleanup_owner.native_handles[0] = target_file;
        cleanup_owner.kinds[0] = AppCatalogCleanupHandleWin32;
        cleanup_owner.count = 1;
        goto cleanup;
    }
    if (!catalog_fault_test_apps_folder_items(target, &cleanup_owner)) goto cleanup;
    for (size_t index = 0; index < 33; index++) {
        wchar_t name[32];
        (void)swprintf(name, sizeof(name) / sizeof(name[0]), L"fault-%02zu.lnk", index);
        if (!catalog_fault_make_path(link_path, MAX_PATH, programs, name)) goto cleanup;
        link_count++;
        if (!catalog_fault_create_shortcut(link_path, target)) goto cleanup;
    }
    roots = (app_catalog_windows_source_roots_t){
        .user_programs = programs,
        .common_programs = common,
        .user_desktop = desktop,
        .public_desktop = public_desktop,
    };

    /* Entry guards and all provider-side allocation sites fail before output publication. */
    bool allocation_sweep_completed = false;
    for (size_t allocation_index = 1; allocation_index <= 512; allocation_index++) {
        stage = "malloc-failure-sweep";
        stage_index = allocation_index;
        app_catalog_status_t status = catalog_fault_enumerate(allocator, &options, &roots, &output,
                                                               CatalogFaultMalloc, allocation_index);
        if (status == AppCatalogStatusOk) {
            if (output.count != 1 || cleanup_owner.count != 0) goto cleanup;
            app_catalog_snapshot_deinit(&output);
            app_catalog_snapshot_init(&output);
            allocation_sweep_completed = true;
            break;
        }
        if (status != AppCatalogStatusOutOfMemory || !catalog_fault_empty_snapshot(&output) || cleanup_owner.count != 0) goto cleanup;
        app_catalog_snapshot_deinit(&output);
        app_catalog_snapshot_init(&output);
    }
    if (!allocation_sweep_completed) goto cleanup;
    stage = "realloc-failure";
    stage_index = 1;
    if (catalog_fault_enumerate(allocator, &options, &roots, &output, CatalogFaultRealloc, 1) != AppCatalogStatusOutOfMemory ||
        !catalog_fault_empty_snapshot(&output) || cleanup_owner.count != 0) goto cleanup;
    app_catalog_snapshot_deinit(&output);
    app_catalog_snapshot_init(&output);

    const struct {
        catalog_fault_kind_t kind;
        app_catalog_status_t expected;
    } failures[] = {
        { CatalogFaultUtf8Conversion, AppCatalogStatusInvalidText },
        { CatalogFaultIdentityMapping, AppCatalogStatusInvalidPathIdentity },
        { CatalogFaultFindFirst, AppCatalogStatusNativeFailure },
        { CatalogFaultFindNext, AppCatalogStatusNativeFailure },
        { CatalogFaultComInitialize, AppCatalogStatusNativeFailure },
    };
    for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); index++) {
        stage = "native-api-failure";
        stage_index = index;
        app_catalog_status_t status = catalog_fault_enumerate(allocator, &options, &roots, &output,
                                                               failures[index].kind, 1);
        if (status != failures[index].expected || !catalog_fault_empty_snapshot(&output) || cleanup_owner.count != 0) goto cleanup;
        app_catalog_snapshot_deinit(&output);
        app_catalog_snapshot_init(&output);
    }

    /* A missing optional canonical target is a stale item, not a global error. */
    stage = "optional-target-create-failure";
    stage_index = 1;
    if (catalog_fault_enumerate(allocator, &options, &roots, &output, CatalogFaultCreateFile, 1) != AppCatalogStatusOk ||
        output.count != 1 || cleanup_owner.count != 0) goto cleanup;
    app_catalog_snapshot_deinit(&output);
    app_catalog_snapshot_init(&output);
    stage = "optional-target-final-path-failure";
    stage_index = 1;
    if (catalog_fault_enumerate(allocator, &options, &roots, &output, CatalogFaultFinalPath, 1) != AppCatalogStatusOk ||
        output.count != 1 || cleanup_owner.count != 0) goto cleanup;
    app_catalog_snapshot_deinit(&output);
    app_catalog_snapshot_init(&output);

    for (size_t close_index = 0; close_index < 2; close_index++) {
        stage = "close-owner-retry";
        stage_index = close_index;
        catalog_fault_kind_t kind = close_index == 0 ? CatalogFaultCloseHandle : CatalogFaultFindClose;
        app_catalog_status_t status = catalog_fault_enumerate(allocator, &options, &roots, &output, kind, 1);
        app_catalog_cleanup_handle_kind_t expected_kind = close_index == 0 ? AppCatalogCleanupHandleWin32 : AppCatalogCleanupHandleFind;
        if (status != AppCatalogStatusNativeFailure || !catalog_fault_empty_snapshot(&output) || cleanup_owner.count != 1 ||
            cleanup_owner.kinds[0] != expected_kind) goto cleanup;
        if (app_catalog_cleanup_owner_retry(&cleanup_owner) != AppCatalogStatusOk || cleanup_owner.count != 0) goto cleanup;
        app_catalog_snapshot_deinit(&output);
        app_catalog_snapshot_init(&output);
    }

    /* Public entry guards leave the initialized snapshot untouched. */
    stage = "public-entry-guards";
    stage_index = 0;
    if (catalog_fault_enumerate(allocator, &options, &roots, &output, CatalogFaultNone, 0) != AppCatalogStatusOk ||
        output.count != 1) goto cleanup;
    app_catalog_snapshot_deinit(&output);
    app_catalog_snapshot_init(&output);
    app_catalog_enumeration_options_t invalid_options = options;
    invalid_options.max_nodes = 0;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, &invalid_options, &roots, &output) != AppCatalogStatusInvalidArgument ||
        !catalog_fault_empty_snapshot(&output)) goto cleanup;
    invalid_options = options;
    invalid_options.max_nodes = 65537;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, &invalid_options, &roots, &output) != AppCatalogStatusInvalidArgument ||
        !catalog_fault_empty_snapshot(&output)) goto cleanup;
    invalid_options = options;
    invalid_options.max_nodes = 1;
    stage = "cancel-timeout-quota";
    stage_index = 1;
    bool cancel_now = true;
    invalid_options.is_cancelled = catalog_fault_cancelled;
    invalid_options.cancel_context = &cancel_now;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, &invalid_options, &roots, &output) != AppCatalogStatusCancelled ||
        !catalog_fault_empty_snapshot(&output)) goto cleanup;
    invalid_options.is_cancelled = NULL;
    invalid_options.cancel_context = NULL;
    invalid_options.deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) - UINT64_C(1);
    if (app_catalog_windows_enumerate_fixture_roots(allocator, &invalid_options, &roots, &output) != AppCatalogStatusTimedOut ||
        !catalog_fault_empty_snapshot(&output)) goto cleanup;
    invalid_options = options;
    invalid_options.max_nodes = 1;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, &invalid_options, &roots, &output) != AppCatalogStatusQuota ||
        !catalog_fault_empty_snapshot(&output)) goto cleanup;

    options.deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(120000000000);
    stage = "successful-enumeration";
    stage_index = 0;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, &options, &roots, &output) != AppCatalogStatusOk ||
        output.count != 1 || cleanup_owner.count != 0) goto cleanup;
    app_catalog_snapshot_deinit(&output);
    ok = true;

cleanup:
    app_catalog_snapshot_deinit(&output);
    if (cleanup_owner.count != 0 && app_catalog_cleanup_owner_retry(&cleanup_owner) != AppCatalogStatusOk) ok = false;
    catalog_fault_remove_tree(root, programs, target, link_count);
    if (programs[0] != L'\0') (void)RemoveDirectoryW(programs);
    if (common[0] != L'\0') (void)RemoveDirectoryW(common);
    if (desktop[0] != L'\0') (void)RemoveDirectoryW(desktop);
    if (public_desktop[0] != L'\0') (void)RemoveDirectoryW(public_desktop);
    if (root[0] != L'\0') (void)RemoveDirectoryW(root);
    if (temp_file[0] != L'\0') (void)DeleteFileW(temp_file);
    if (com_initialized) CoUninitialize();
    if (!ok) fprintf(stderr, "app_catalog_windows: provider fault fixture failed at %s[%zu]\n", stage, stage_index);
    return ok ? 0 : 1;
}

#else
int app_catalog_windows_fault_fixture_run(void) { return 0; }
#endif
