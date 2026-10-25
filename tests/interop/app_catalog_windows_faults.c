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
#include <limits.h>
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
    CatalogFaultComCreateNull,
    CatalogFaultComPersistNull,
    CatalogFaultComPropertyFailedNonNull,
    CatalogFaultUtf8Oversize,
    CatalogFaultIdentityOversize,
} catalog_fault_kind_t;

typedef struct catalog_fault_state_t {
    catalog_fault_kind_t kind;
    size_t fail_at;
    size_t calls;
} catalog_fault_state_t;

static catalog_fault_state_t catalog_fault_state;
static IShellItem *catalog_fault_apps_folder;
static IEnumShellItems *catalog_fault_apps_enumerator;
static bool catalog_fault_apps_enabled;
static bool catalog_fault_apps_folder_failure;
static bool catalog_fault_apps_bind_failure;
static bool catalog_fault_apps_folder_null_success;
static bool catalog_fault_apps_bind_null_success;
static size_t catalog_fault_property_qi_acquired;
static size_t catalog_fault_property_releases;
static size_t catalog_fault_property_get_value_calls;
static IPropertyStore *catalog_fault_failed_property_store;
static const wchar_t *catalog_fault_final_path_override;

static HRESULT WINAPI catalog_fault_sh_get_known_folder_item(REFKNOWNFOLDERID folder_id,
    KNOWN_FOLDER_FLAG flags, HANDLE token, REFIID iid, void **output) {
    if (!catalog_fault_apps_enabled) return SHGetKnownFolderItem(folder_id, flags, token, iid, output);
    (void)flags;
    (void)token;
    if (output == NULL) return E_POINTER;
    *output = NULL;
    if (catalog_fault_apps_folder_failure) return E_FAIL;
    if (catalog_fault_apps_folder_null_success) return S_OK;
    if (!IsEqualIID(folder_id, &FOLDERID_AppsFolder) || !IsEqualIID(iid, &IID_IShellItem) ||
        catalog_fault_apps_folder == NULL) return E_FAIL;
    IShellItem_AddRef(catalog_fault_apps_folder);
    *output = catalog_fault_apps_folder;
    return S_OK;
}

static bool catalog_fault_hit(catalog_fault_kind_t kind) {
    if (catalog_fault_state.kind != kind) return false;
    catalog_fault_state.calls++;
    return catalog_fault_state.calls == catalog_fault_state.fail_at;
}

static HRESULT WINAPI catalog_fault_co_create_instance(REFCLSID class_id, LPUNKNOWN outer,
    DWORD context, REFIID iid, LPVOID *output) {
    if (catalog_fault_hit(CatalogFaultComCreateNull)) {
        if (output != NULL) *output = NULL;
        return S_OK;
    }
    return CoCreateInstance(class_id, outer, context, iid, output);
}

static HRESULT STDMETHODCALLTYPE catalog_fault_shell_link_query(IShellLinkW *link, REFIID iid, void **output) {
    if (catalog_fault_hit(CatalogFaultComPersistNull) && IsEqualIID(iid, &IID_IPersistFile)) {
        if (output != NULL) *output = NULL;
        return S_OK;
    }
    HRESULT result = link->lpVtbl->QueryInterface(link, iid, output);
    if (IsEqualIID(iid, &IID_IPropertyStore) && SUCCEEDED(result) && output != NULL && *output != NULL &&
        catalog_fault_hit(CatalogFaultComPropertyFailedNonNull)) {
        catalog_fault_property_qi_acquired++;
        catalog_fault_failed_property_store = (IPropertyStore *)*output;
        return E_FAIL;
    }
    return result;
}

static ULONG STDMETHODCALLTYPE catalog_fault_property_store_release(IPropertyStore *store) {
    if (store == catalog_fault_failed_property_store) {
        catalog_fault_property_releases++;
        catalog_fault_failed_property_store = NULL;
    }
    return store->lpVtbl->Release(store);
}

static HRESULT STDMETHODCALLTYPE catalog_fault_property_store_get_value(
    IPropertyStore *store, REFPROPERTYKEY key, PROPVARIANT *value) {
    if (store == catalog_fault_failed_property_store) catalog_fault_property_get_value_calls++;
    return store->lpVtbl->GetValue(store, key, value);
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
    if (catalog_fault_hit(CatalogFaultUtf8Oversize) && output == NULL) return (int)InteropMaxStringBytes + 1;
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
    if (catalog_fault_hit(CatalogFaultIdentityOversize) && destination == NULL) return (int)AppCatalogMaxPathUnits + 1;
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
    if (catalog_fault_final_path_override != NULL) {
        size_t length = wcslen(catalog_fault_final_path_override);
        if (length + 1 > capacity) return (DWORD)(length + 1);
        memcpy(output, catalog_fault_final_path_override, (length + 1) * sizeof(wchar_t));
        return (DWORD)length;
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
#define CoCreateInstance catalog_fault_co_create_instance
#undef IShellLinkW_QueryInterface
#define IShellLinkW_QueryInterface catalog_fault_shell_link_query
#undef IPropertyStore_Release
#define IPropertyStore_Release(This) catalog_fault_property_store_release(This)
#undef IPropertyStore_GetValue
#define IPropertyStore_GetValue(This,key,value) catalog_fault_property_store_get_value(This,key,value)
#define SHGetKnownFolderItem catalog_fault_sh_get_known_folder_item
#include "../../src/interop/app_catalog_windows.c"
#undef SHGetKnownFolderItem
#undef IPropertyStore_GetValue
#undef IPropertyStore_Release
#undef IShellLinkW_QueryInterface
#undef CoCreateInstance
#undef CoInitializeEx
#define IShellLinkW_QueryInterface(This,riid,ppvObject) (This)->lpVtbl->QueryInterface(This,riid,ppvObject)
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
    bool fail_query_nonnull;
    bool fail_store;
    bool fail_parsing_name;
    bool fail_display_name;
    bool fail_property;
    bool null_store_success;
    bool separator_as_ui4;
    ULONG separator_ui4_value;
} catalog_fake_item_t;

typedef struct catalog_fake_enumerator_t {
    IEnumShellItems enumerator_iface;
    ULONG references;
    catalog_fake_item_t **items;
    size_t item_count;
    size_t next_index;
    HRESULT terminal_result;
    ULONG terminal_fetched;
    catalog_fake_item_t *terminal_item;
} catalog_fake_enumerator_t;

static catalog_fake_enumerator_t *catalog_fake_from_enumerator(IEnumShellItems *self) {
    return (catalog_fake_enumerator_t *)((uint8_t *)self - offsetof(catalog_fake_enumerator_t, enumerator_iface));
}

static ULONG STDMETHODCALLTYPE catalog_fake_enumerator_add_ref(IEnumShellItems *self);

static HRESULT STDMETHODCALLTYPE catalog_fake_enumerator_query(IEnumShellItems *self, REFIID iid, void **output) {
    if (output == NULL) return E_POINTER;
    *output = NULL;
    if (!IsEqualIID(iid, &IID_IEnumShellItems)) return E_NOINTERFACE;
    catalog_fake_enumerator_add_ref(self);
    *output = self;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE catalog_fake_enumerator_add_ref(IEnumShellItems *self) {
    return ++catalog_fake_from_enumerator(self)->references;
}

static ULONG STDMETHODCALLTYPE catalog_fake_enumerator_release(IEnumShellItems *self) {
    catalog_fake_enumerator_t *enumerator = catalog_fake_from_enumerator(self);
    if (enumerator->references != 0) enumerator->references--;
    return enumerator->references;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_enumerator_next(IEnumShellItems *self, ULONG requested,
    IShellItem **items, ULONG *fetched) {
    catalog_fake_enumerator_t *enumerator = catalog_fake_from_enumerator(self);
    if (requested != 1 || items == NULL || fetched == NULL) return E_INVALIDARG;
    *items = NULL;
    *fetched = 0;
    if (enumerator->next_index < enumerator->item_count) {
        catalog_fake_item_t *item = enumerator->items[enumerator->next_index++];
        item->references++;
        *items = &item->shell_item;
        *fetched = 1;
        return S_OK;
    }
    if (enumerator->terminal_item != NULL) {
        enumerator->terminal_item->references++;
        *items = &enumerator->terminal_item->shell_item;
    }
    *fetched = enumerator->terminal_fetched;
    return enumerator->terminal_result;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_enumerator_skip(IEnumShellItems *self, ULONG count) {
    (void)self;
    (void)count;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_enumerator_reset(IEnumShellItems *self) {
    catalog_fake_from_enumerator(self)->next_index = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_enumerator_clone(IEnumShellItems *self, IEnumShellItems **output) {
    (void)self;
    if (output != NULL) *output = NULL;
    return E_NOTIMPL;
}

static IEnumShellItemsVtbl CatalogFakeEnumeratorVtbl = {
    .QueryInterface = catalog_fake_enumerator_query,
    .AddRef = catalog_fake_enumerator_add_ref,
    .Release = catalog_fake_enumerator_release,
    .Next = catalog_fake_enumerator_next,
    .Skip = catalog_fake_enumerator_skip,
    .Reset = catalog_fake_enumerator_reset,
    .Clone = catalog_fake_enumerator_clone,
};

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
    if (item->fail_query_nonnull && IsEqualIID(iid, &IID_IShellItem2)) {
        item->references++;
        *object = &item->shell_item2;
        return E_FAIL;
    }
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

static HRESULT STDMETHODCALLTYPE catalog_fake_shell_bind(IShellItem *self, IBindCtx *context,
    REFGUID handler, REFIID iid, void **output) {
    (void)self;
    (void)context;
    if (output == NULL) return E_POINTER;
    *output = NULL;
    if (catalog_fault_apps_bind_failure) return E_FAIL;
    if (catalog_fault_apps_bind_null_success) return S_OK;
    if (catalog_fault_apps_enumerator == NULL || !IsEqualGUID(handler, &BHID_EnumItems) ||
        !IsEqualIID(iid, &IID_IEnumShellItems)) return E_FAIL;
    catalog_fake_enumerator_add_ref(catalog_fault_apps_enumerator);
    *output = catalog_fault_apps_enumerator;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE catalog_fake_shell2_get_store(IShellItem2 *self,
    GETPROPERTYSTOREFLAGS flags, REFIID iid, void **output) {
    (void)flags;
    catalog_fake_item_t *item = catalog_fake_from_shell2(self);
    if (output == NULL) return E_POINTER;
    *output = NULL;
    if (!IsEqualIID(iid, &IID_IPropertyStore)) return E_FAIL;
    if (item->null_store_success) return S_OK;
    if (item->fail_store) return E_FAIL;
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
        if (item->separator_as_ui4) {
            value->vt = VT_UI4;
            value->ulVal = item->separator_ui4_value;
        } else {
            value->vt = VT_BOOL;
            value->boolVal = item->separator ? VARIANT_TRUE : VARIANT_FALSE;
        }
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
    .BindToHandler = catalog_fake_shell_bind,
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

static bool catalog_fault_run_apps_enum_case(catalog_fake_item_t *folder,
    catalog_fake_enumerator_t *enumerator, catalog_fake_item_t **items, size_t item_count,
    bool folder_failure, bool bind_failure, size_t max_nodes, HRESULT terminal_result,
    ULONG terminal_fetched, catalog_fake_item_t *terminal_item, bool expected_ok,
    app_catalog_status_t expected_status, size_t expected_candidates) {
    enumerator->items = items;
    enumerator->item_count = item_count;
    enumerator->next_index = 0;
    enumerator->terminal_result = terminal_result;
    enumerator->terminal_fetched = terminal_fetched;
    enumerator->terminal_item = terminal_item;
    catalog_fault_apps_folder = &folder->shell_item;
    catalog_fault_apps_enumerator = &enumerator->enumerator_iface;
    catalog_fault_apps_enabled = true;
    catalog_fault_apps_folder_failure = folder_failure;
    catalog_fault_apps_bind_failure = bind_failure;

    app_catalog_cleanup_owner_t cleanup_owner;
    app_catalog_cleanup_owner_init(&cleanup_owner);
    app_catalog_enumeration_options_t options = {0};
    options.cleanup_owner = &cleanup_owner;
    options.max_nodes = max_nodes;
    options.deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(30000000000);
    catalog_collector_t collector = {0};
    collector.options = &options;
    collector.cleanup_owner = &cleanup_owner;
    collector.storage = malloc(AppCatalogMaxBytes);
    if (collector.storage == NULL) return false;
    collector.storage_capacity = AppCatalogMaxBytes;
    collector.status = AppCatalogStatusOk;

    bool actual_ok = enumerate_apps_folder(&collector);
    bool ok = actual_ok == expected_ok && collector.status == expected_status &&
              collector.count == expected_candidates && cleanup_owner.count == 0 &&
              folder->references == 1 && enumerator->references == 1;
    for (size_t index = 0; index < item_count; index++) if (items[index]->references != 1) ok = false;
    if (terminal_item != NULL && terminal_item->references != 1) ok = false;
    free(collector.candidates);
    free(collector.storage);
    return ok;
}

static bool catalog_fault_test_apps_folder_enumerator(void) {
    catalog_fake_item_t folder, first, second;
    catalog_fake_item_init(&folder);
    catalog_fake_item_init(&first);
    catalog_fake_item_init(&second);
    first.display_name = L"Fixture enumerated one";
    first.parsing_name = L"shell:AppsFolder\\Fixture!One";
    first.aumid = L"Fixture.Package!One";
    second.display_name = L"Fixture enumerated two";
    second.parsing_name = L"shell:AppsFolder\\Fixture!Two";
    second.aumid = L"Fixture.Package!Two";
    catalog_fake_enumerator_t enumerator = { .enumerator_iface.lpVtbl = &CatalogFakeEnumeratorVtbl, .references = 1 };
    catalog_fake_item_t *one[] = { &first };
    catalog_fake_item_t *two[] = { &first, &second };
    bool ok = true;

    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, true, false, 32,
        S_FALSE, 0, NULL, false, AppCatalogStatusNativeFailure, 0) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, true, 32,
        S_FALSE, 0, NULL, false, AppCatalogStatusNativeFailure, 0) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, false, 32,
        S_FALSE, 0, NULL, true, AppCatalogStatusOk, 0) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 1, false, false, 32,
        S_FALSE, 0, NULL, true, AppCatalogStatusOk, 1) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, false, 32,
        E_FAIL, 0, &first, false, AppCatalogStatusNativeFailure, 0) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, false, 32,
        S_OK, 0, NULL, false, AppCatalogStatusNativeFailure, 0) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, false, 32,
        S_OK, 1, NULL, false, AppCatalogStatusNativeFailure, 0) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, false, 32,
        S_FALSE, 1, NULL, false, AppCatalogStatusNativeFailure, 0) && ok;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, two, 2, false, false, 1,
        S_FALSE, 0, NULL, false, AppCatalogStatusQuota, 1) && ok;

    catalog_fault_apps_folder_failure = false;
    catalog_fault_apps_bind_failure = false;
    catalog_fault_apps_folder_null_success = true;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, false, 32,
        S_FALSE, 0, NULL, false, AppCatalogStatusNativeFailure, 0) && ok;
    catalog_fault_apps_folder_null_success = false;
    catalog_fault_apps_bind_null_success = true;
    ok = catalog_fault_run_apps_enum_case(&folder, &enumerator, one, 0, false, false, 32,
        S_FALSE, 0, NULL, false, AppCatalogStatusNativeFailure, 0) && ok;
    catalog_fault_apps_bind_null_success = false;
    catalog_fault_apps_folder = NULL;
    catalog_fault_apps_enumerator = NULL;
    catalog_fault_apps_enabled = false;
    catalog_fault_apps_folder_failure = false;
    catalog_fault_apps_bind_failure = false;
    catalog_fault_apps_folder_null_success = false;
    catalog_fault_apps_bind_null_success = false;
    return ok && folder.references == 1 && enumerator.references == 1 &&
           first.references == 1 && second.references == 1;
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

    item.fail_query_nonnull = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0 || item.references != 1) ok = false;
    item.fail_query_nonnull = false;

    item.fail_store = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.fail_store = false;

    item.null_store_success = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.null_store_success = false;

    item.fail_property = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.fail_property = false;

    item.display_name = L"";
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.display_name = L"Fixture packaged app";

    item.fail_display_name = true;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.fail_display_name = false;

    item.aumid = NULL;
    item.target_path = NULL;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 0) ok = false;
    item.aumid = L"Fixture.Package!Application";
    item.target_path = target;

    wchar_t long_display[InteropMaxStringBytes + 2];
    for (size_t index = 0; index < InteropMaxStringBytes + 1; index++) long_display[index] = L'D';
    long_display[InteropMaxStringBytes + 1] = L'\0';
    item.display_name = long_display;
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

    wchar_t long_parsing[AppCatalogMaxPathUnits + 2];
    for (size_t index = 0; index < AppCatalogMaxPathUnits + 1; index++) long_parsing[index] = L'P';
    long_parsing[AppCatalogMaxPathUnits + 1] = L'\0';
    item.parsing_name = long_parsing;
    if (!add_apps_folder_item(&collector, &item.shell_item) || collector.count != 1 ||
        collector.candidates[0].source_identifier.length != strlen("Fixture packaged app")) ok = false;
    item.parsing_name = L"shell:AppsFolder\\Fixture!App";
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
    catalog_fault_state = (catalog_fault_state_t){ kind, fail_at, 0 };
    options->deadline_monotonic_ns = GetTickCount64() * UINT64_C(1000000) + UINT64_C(120000000000);
    app_catalog_status_t status = app_catalog_windows_enumerate_fixture_roots(allocator, options, roots, output);
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultNone, 0, 0 };
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

static bool catalog_fault_test_entry_guards(app_catalog_allocator_t allocator,
    app_catalog_enumeration_options_t *options, app_catalog_windows_source_roots_t *roots,
    app_catalog_snapshot_t *output, app_catalog_cleanup_owner_t *cleanup_owner) {
    app_catalog_snapshot_t before = *output;
    app_catalog_allocator_t invalid_allocator = allocator;
    invalid_allocator.allocate = NULL;
    if (app_catalog_windows_enumerate_fixture_roots(invalid_allocator, options, roots, output) != AppCatalogStatusInvalidArgument ||
        memcmp(output, &before, sizeof(before)) != 0) return false;
    invalid_allocator = allocator;
    invalid_allocator.deallocate = NULL;
    if (app_catalog_windows_enumerate_fixture_roots(invalid_allocator, options, roots, output) != AppCatalogStatusInvalidArgument ||
        memcmp(output, &before, sizeof(before)) != 0) return false;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, NULL, roots, output) != AppCatalogStatusInvalidArgument ||
        memcmp(output, &before, sizeof(before)) != 0) return false;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, options, roots, NULL) != AppCatalogStatusInvalidArgument) return false;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, options, NULL, output) != AppCatalogStatusInvalidArgument ||
        memcmp(output, &before, sizeof(before)) != 0) return false;
    app_catalog_windows_source_roots_t invalid_roots = *roots;
    invalid_roots.common_programs = NULL;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, options, &invalid_roots, output) != AppCatalogStatusInvalidArgument ||
        memcmp(output, &before, sizeof(before)) != 0) return false;
    invalid_roots = *roots;
    invalid_roots.common_programs = L"";
    if (app_catalog_windows_enumerate_fixture_roots(allocator, options, &invalid_roots, output) != AppCatalogStatusInvalidArgument ||
        memcmp(output, &before, sizeof(before)) != 0) return false;
    wchar_t oversized_path[AppCatalogMaxPathUnits + 2];
    for (size_t index = 0; index < AppCatalogMaxPathUnits + 1; index++) oversized_path[index] = L'X';
    oversized_path[AppCatalogMaxPathUnits + 1] = L'\0';
    invalid_roots = *roots;
    invalid_roots.common_programs = oversized_path;
    if (app_catalog_windows_enumerate_fixture_roots(allocator, options, &invalid_roots, output) != AppCatalogStatusInvalidArgument ||
        memcmp(output, &before, sizeof(before)) != 0) return false;

    HANDLE held = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (held == NULL) return false;
    cleanup_owner->native_handles[0] = held;
    cleanup_owner->kinds[0] = AppCatalogCleanupHandleWin32;
    cleanup_owner->count = 1;
    app_catalog_enumeration_options_t busy_options = *options;
    busy_options.cleanup_owner = cleanup_owner;
    bool owner_ok = app_catalog_windows_enumerate_fixture_roots(allocator, &busy_options, roots, output) == AppCatalogStatusInvalidArgument &&
                    cleanup_owner->native_handles[0] == held && cleanup_owner->kinds[0] == AppCatalogCleanupHandleWin32 &&
                    cleanup_owner->count == 1 && memcmp(output, &before, sizeof(before)) == 0;
    BOOL closed = CloseHandle(held);
    cleanup_owner->native_handles[0] = NULL;
    cleanup_owner->kinds[0] = AppCatalogCleanupHandleNone;
    cleanup_owner->count = 0;
    if (!owner_ok || !closed) return false;

    output->duplicate_launch_conflicts = 7;
    before = *output;
    bool output_ok = app_catalog_windows_enumerate_fixture_roots(allocator, options, roots, output) == AppCatalogStatusInvalidArgument &&
                     memcmp(output, &before, sizeof(before)) == 0;
    output->duplicate_launch_conflicts = 0;
    return output_ok && catalog_fault_empty_snapshot(output);
}

static bool catalog_fault_cancelled(void *context) {
    const bool *cancelled = context;
    return cancelled != NULL && *cancelled;
}

static bool catalog_fault_test_canonical_and_storage_helpers(const wchar_t *target) {
    app_catalog_cleanup_owner_t owner;
    app_catalog_cleanup_owner_init(&owner);
    catalog_collector_t collector = {0};
    collector.cleanup_owner = &owner;
    collector.status = AppCatalogStatusOk;

    wchar_t canonical[128];
    size_t units = 777;
    catalog_fault_final_path_override = L"\\\\?\\UNC\\server\\share\\target.exe";
    bool ok = canonical_target(&collector, target, canonical, 128, &units) &&
              units == wcslen(L"\\\\server\\share\\target.exe") &&
              wcscmp(canonical, L"\\\\server\\share\\target.exe") == 0;
    catalog_fault_final_path_override = L"relative-target";
    units = 777;
    if (canonical_target(&collector, target, canonical, 128, &units) || units != 777) ok = false;
    catalog_fault_final_path_override = NULL;
    catalog_fault_final_path_override = L"C:\\some-long-target.exe";
    units = 777;
    if (canonical_target(&collector, target, canonical, 8, &units) || units != 777) ok = false;
    catalog_fault_final_path_override = NULL;
    units = 777;
    if (canonical_target(&collector, NULL, canonical, 128, &units) ||
        canonical_target(&collector, target, NULL, 128, &units) ||
        canonical_target(&collector, target, canonical, 7, &units) || units != 777) ok = false;
    if (owner.count != 0) ok = false;

    uint8_t *storage = malloc(AppCatalogMaxBytes);
    if (storage == NULL) return false;
    collector.storage = storage;
    collector.storage_capacity = AppCatalogMaxBytes;
    collector.storage_used = 0;
    collector.status = AppCatalogStatusOk;
    app_catalog_bytes_t empty = collector_copy(&collector, NULL, 0);
    if (empty.data != NULL || empty.length != 0 || collector.storage_used != 0) ok = false;
    if (collector_copy(&collector, NULL, 1).length != 0 || collector.status != AppCatalogStatusQuota ||
        collector.storage_used != 0) ok = false;
    collector.status = AppCatalogStatusOk;
    collector.storage_used = AppCatalogMaxBytes - 1;
    app_catalog_bytes_t last = collector_copy(&collector, "Z", 1);
    if (last.data != storage + AppCatalogMaxBytes - 1 || last.length != 1 || storage[AppCatalogMaxBytes - 1] != 'Z' ||
        collector.storage_used != AppCatalogMaxBytes) ok = false;
    if (collector_copy(&collector, "Q", 1).length != 0 || collector.status != AppCatalogStatusQuota ||
        collector.storage_used != AppCatalogMaxBytes) ok = false;

    collector.storage_used = 0;
    collector.status = AppCatalogStatusOk;
    app_catalog_bytes_t converted = collector_utf8(&collector, L"codec", 5);
    if (converted.length != 5 || memcmp(converted.data, "codec", 5) != 0) ok = false;
    collector.storage_used = 0;
    collector.status = AppCatalogStatusOk;
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultUtf8Conversion, 2, 0 };
    converted = collector_utf8(&collector, L"codec", 5);
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultNone, 0, 0 };
    if (converted.length != 0 || collector.status != AppCatalogStatusInvalidText || collector.storage_used != 0) ok = false;
    collector.status = AppCatalogStatusOk;
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultUtf8Oversize, 1, 0 };
    converted = collector_utf8(&collector, L"codec", 5);
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultNone, 0, 0 };
    if (converted.length != 0 || collector.status != AppCatalogStatusQuota || collector.storage_used != 0) ok = false;
    collector.status = AppCatalogStatusOk;
    if (collector_utf8(&collector, NULL, 1).length != 0 || collector.status != AppCatalogStatusInvalidText) ok = false;
    collector.status = AppCatalogStatusOk;
    if (collector_utf8(&collector, L"x", (size_t)INT_MAX + 1).length != 0 || collector.status != AppCatalogStatusInvalidText) ok = false;

    collector.status = AppCatalogStatusOk;
    if (collector_identity_path(&collector, L"x", 0).length != 0 || collector.status != AppCatalogStatusQuota) ok = false;
    collector.status = AppCatalogStatusOk;
    if (collector_identity_path(&collector, L"x", AppCatalogMaxPathUnits + 1).length != 0 || collector.status != AppCatalogStatusQuota) ok = false;
    collector.status = AppCatalogStatusOk;
    const wchar_t *identity_input = L"C:\\TEMP\\APP.EXE";
    size_t identity_units = wcslen(identity_input);
    app_catalog_bytes_t identity = collector_identity_path(&collector, identity_input, identity_units);
    if (identity.length != identity_units * sizeof(wchar_t) ||
        wcsncmp((const wchar_t *)identity.data, identity_input, identity_units) != 0) ok = false;
    collector.storage_used = 0;
    collector.status = AppCatalogStatusOk;
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultIdentityMapping, 2, 0 };
    identity = collector_identity_path(&collector, identity_input, identity_units);
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultNone, 0, 0 };
    if (identity.length != 0 || collector.status != AppCatalogStatusInvalidPathIdentity || collector.storage_used != 0) ok = false;
    collector.status = AppCatalogStatusOk;
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultIdentityOversize, 1, 0 };
    identity = collector_identity_path(&collector, identity_input, identity_units);
    catalog_fault_state = (catalog_fault_state_t){ CatalogFaultNone, 0, 0 };
    if (identity.length != 0 || collector.status != AppCatalogStatusInvalidPathIdentity || collector.storage_used != 0) ok = false;

    free(storage);
    return ok;
}

static bool catalog_fault_test_value_helpers(void) {
    app_catalog_cleanup_owner_t owner;
    app_catalog_cleanup_owner_init(&owner);
    uint8_t storage[256] = {0};
    catalog_collector_t collector = {0};
    collector.storage = storage;
    collector.storage_capacity = sizeof(storage);
    collector.options = &(app_catalog_enumeration_options_t){ .max_nodes = 1 };
    collector.cleanup_owner = &owner;
    collector.status = AppCatalogStatusOk;

    const struct { const wchar_t *path; const char *expected; } directories[] = {
        { L"C:\\file.exe", "C:\\" },
        { L"C:\\", "C:\\" },
        { L"\\\\server\\share\\file.exe", "\\\\server\\share" },
        { L"plain", "" },
    };
    bool ok = true;
    for (size_t index = 0; index < sizeof(directories) / sizeof(directories[0]); index++) {
        collector.storage_used = 0;
        collector.status = AppCatalogStatusOk;
        app_catalog_bytes_t directory = directory_of_path(&collector, directories[index].path, wcslen(directories[index].path));
        size_t expected_length = strlen(directories[index].expected);
        if (directory.length != expected_length ||
            (expected_length != 0 && memcmp(directory.data, directories[index].expected, expected_length) != 0)) ok = false;
    }

    catalog_fake_item_t item;
    catalog_fake_item_init(&item);
    item.aumid = L"short-id";
    wchar_t buffer[16] = {0};
    size_t units = 999;
    if (property_string(&item.property_store, &PKEY_AppUserModel_ID, buffer, 8, &units) || units != 999) ok = false;
    units = 999;
    if (!property_string(&item.property_store, &PKEY_AppUserModel_ID, buffer, 9, &units) ||
        units != 8 || wcscmp(buffer, L"short-id") != 0) ok = false;
    item.separator_as_ui4 = true;
    item.separator_ui4_value = 1;
    if (!property_bool(&item.property_store, &PKEY_AppUserModel_IsDestListSeparator)) ok = false;
    item.separator_ui4_value = 0;
    if (property_bool(&item.property_store, &PKEY_AppUserModel_IsDestListSeparator)) ok = false;
    if (property_string(NULL, &PKEY_AppUserModel_ID, buffer, 9, &units) ||
        property_string(&item.property_store, &PKEY_AppUserModel_ID, NULL, 9, &units) ||
        property_string(&item.property_store, &PKEY_AppUserModel_ID, buffer, 0, &units) ||
        property_string(&item.property_store, &PKEY_AppUserModel_ID, buffer, 9, NULL) || property_bool(NULL, &PKEY_AppUserModel_IsDestListSeparator)) ok = false;

    bool cancelled = true;
    collector.options = &(app_catalog_enumeration_options_t){ .max_nodes = 1, .is_cancelled = catalog_fault_cancelled, .cancel_context = &cancelled };
    collector.nodes = 0;
    if (collector_checkpoint(&collector) || collector.status != AppCatalogStatusCancelled || collector.nodes != 0) ok = false;
    cancelled = false;
    collector.status = AppCatalogStatusOk;
    collector.options = &(app_catalog_enumeration_options_t){ .max_nodes = 1, .deadline_monotonic_ns = 1 };
    if (collector_checkpoint(&collector) || collector.status != AppCatalogStatusTimedOut || collector.nodes != 0) ok = false;
    collector.status = AppCatalogStatusOk;
    collector.options = &(app_catalog_enumeration_options_t){ .max_nodes = 0 };
    if (collector_checkpoint(&collector) || collector.status != AppCatalogStatusQuota || collector.nodes != 0) ok = false;
    collector.status = AppCatalogStatusOk;
    collector.options = &(app_catalog_enumeration_options_t){ .max_nodes = 1 };
    if (!collector_checkpoint(&collector) || collector.nodes != 1) ok = false;

    collector.options = &(app_catalog_enumeration_options_t){ .max_nodes = 1 };
    collector.count = AppCatalogMaxEntries;
    collector.status = AppCatalogStatusOk;
    if (add_item(&collector, AppCatalogSourceUserPrograms, AppCatalogIdentityAumid, L"id", 2,
                 L"id", 2, L"name", 4, NULL, 0, L"", 0, L"", 0, NULL, 0, NULL, 0, false) ||
        collector.status != AppCatalogStatusQuota) ok = false;
    collector.count = 1;
    collector.status = AppCatalogStatusOk;
    if (add_item(&collector, AppCatalogSourceUserPrograms, AppCatalogIdentityAumid, L"id", 2,
                 L"id", 2, L"name", 4, NULL, 0, L"", 0, L"", 0, NULL, 0, NULL, 0, false) ||
        collector.status != AppCatalogStatusQuota) ok = false;

    HANDLE handles[AppCatalogCleanupMaxHandles + 1] = {0};
    app_catalog_cleanup_owner_t full_owner;
    app_catalog_cleanup_owner_init(&full_owner);
    if (retain_failed_handle(NULL, INVALID_HANDLE_VALUE, AppCatalogCleanupHandleWin32)) ok = false;
    for (size_t index = 0; index < AppCatalogCleanupMaxHandles; index++) {
        handles[index] = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (handles[index] == NULL) {
            for (size_t prior = 0; prior < index; prior++) if (!CloseHandle(handles[prior])) ok = false;
            return false;
        }
    }
    handles[AppCatalogCleanupMaxHandles] = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (handles[AppCatalogCleanupMaxHandles] == NULL) {
        for (size_t index = 0; index < AppCatalogCleanupMaxHandles; index++) if (!CloseHandle(handles[index])) ok = false;
        return false;
    }
    for (size_t index = 0; index < AppCatalogCleanupMaxHandles; index++) {
        full_owner.native_handles[index] = handles[index];
        full_owner.kinds[index] = AppCatalogCleanupHandleWin32;
        full_owner.count++;
    }
    if (retain_failed_handle(&full_owner, handles[AppCatalogCleanupMaxHandles], AppCatalogCleanupHandleWin32) ||
        full_owner.count != AppCatalogCleanupMaxHandles || cleanup_owner_empty(&full_owner) ||
        cleanup_owner_empty(NULL)) ok = false;
    if (!CloseHandle(handles[AppCatalogCleanupMaxHandles])) ok = false;
    if (app_catalog_cleanup_owner_retry(&full_owner) != AppCatalogStatusOk || full_owner.count != 0) ok = false;
    app_catalog_cleanup_owner_init(&full_owner);
    HANDLE retained = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (retained == NULL) return false;
    if (!retain_failed_handle(&full_owner, retained, AppCatalogCleanupHandleWin32)) {
        CloseHandle(retained);
        return false;
    }
    if (!cleanup_owner_empty(&owner) || full_owner.count != 1 ||
        app_catalog_cleanup_owner_retry(&full_owner) != AppCatalogStatusOk || full_owner.count != 0) ok = false;
    return ok;
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
    stage = "private-helper-branches";
    stage_index = 0;
    if (!catalog_fault_test_canonical_and_storage_helpers(target) ||
        !catalog_fault_test_value_helpers()) goto cleanup;
    if (!catalog_fault_test_apps_folder_items(target, &cleanup_owner) ||
        !catalog_fault_test_apps_folder_enumerator()) goto cleanup;
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

    stage = "entry-guard-tests";
    if (!catalog_fault_test_entry_guards(allocator, &options, &roots, &output, &cleanup_owner)) goto cleanup;

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
        { CatalogFaultComCreateNull, AppCatalogStatusNativeFailure },
        { CatalogFaultComPersistNull, AppCatalogStatusNativeFailure },
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

    /* Failed QI may still return an acquired interface; it must be released without use. */
    stage = "com-property-failed-nonnull";
    stage_index = 1;
    size_t acquired_before = catalog_fault_property_qi_acquired;
    size_t releases_before = catalog_fault_property_releases;
    size_t get_value_calls_before = catalog_fault_property_get_value_calls;
    if (catalog_fault_enumerate(allocator, &options, &roots, &output,
                                CatalogFaultComPropertyFailedNonNull, 1) != AppCatalogStatusOk ||
        output.count != 1 || catalog_fault_property_qi_acquired != acquired_before + 1 ||
        catalog_fault_property_releases != releases_before + 1 ||
        catalog_fault_property_get_value_calls != get_value_calls_before ||
        catalog_fault_failed_property_store != NULL) goto cleanup;
    app_catalog_snapshot_deinit(&output);
    app_catalog_snapshot_init(&output);

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
