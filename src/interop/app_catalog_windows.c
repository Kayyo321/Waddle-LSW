/** @file app_catalog_windows.c @brief Read-only Windows shell catalog enumeration. */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "app_catalog.h"
#include "app_catalog_windows_private.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct catalog_collector_t {
    app_catalog_candidate_t *candidates;
    size_t count;
    size_t candidate_capacity;
    uint8_t *storage;
    size_t storage_used;
    size_t storage_capacity;
    const app_catalog_enumeration_options_t *options;
    app_catalog_cleanup_owner_t *cleanup_owner;
    app_catalog_native_close_fault_fn_t close_fault;
    void *close_fault_context;
    size_t nodes;
    app_catalog_status_t status;
} catalog_collector_t;

/* SDK PROPERTYKEY values from the frozen shell contract; local names keep
 * MinGW and older Windows SDK headers source-compatible. */
static const PROPERTYKEY CatalogLinkTargetParsingPath =
    { { 0xB9B4B3FC, 0x2B51, 0x4A42, { 0xB5, 0xD8, 0x32, 0x41, 0x46, 0xAF, 0xCF, 0x25 } }, 2 };
static const PROPERTYKEY CatalogPackageFamilyName =
    { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 17 };

static bool cleanup_owner_empty(const app_catalog_cleanup_owner_t *owner) {
    return owner != NULL && owner->count == 0;
}

static bool retain_failed_handle(app_catalog_cleanup_owner_t *owner, HANDLE handle,
                                 app_catalog_cleanup_handle_kind_t kind) {
    if (owner == NULL || owner->count >= AppCatalogCleanupMaxHandles) return false;
    owner->native_handles[owner->count] = handle;
    owner->kinds[owner->count] = kind;
    owner->count++;
    return true;
}

static bool close_file_handle(HANDLE handle, app_catalog_cleanup_owner_t *owner,
                              app_catalog_native_close_fault_fn_t fault, void *fault_context) {
    if ((fault == NULL || !fault(fault_context, AppCatalogCleanupHandleWin32)) && CloseHandle(handle)) return true;
    (void)retain_failed_handle(owner, handle, AppCatalogCleanupHandleWin32);
    return false;
}

static bool close_search_handle(HANDLE handle, app_catalog_cleanup_owner_t *owner,
                                app_catalog_native_close_fault_fn_t fault, void *fault_context) {
    if ((fault == NULL || !fault(fault_context, AppCatalogCleanupHandleFind)) && FindClose(handle)) return true;
    (void)retain_failed_handle(owner, handle, AppCatalogCleanupHandleFind);
    return false;
}

static bool canonical_target(catalog_collector_t *collector, const wchar_t *source,
                             wchar_t *output, size_t capacity, size_t *units);

static bool collector_checkpoint(catalog_collector_t *collector) {
    if (collector->options->is_cancelled != NULL && collector->options->is_cancelled(collector->options->cancel_context)) {
        collector->status = AppCatalogStatusCancelled;
        return false;
    }
    if (collector->options->deadline_monotonic_ns != 0 && GetTickCount64() * UINT64_C(1000000) >= collector->options->deadline_monotonic_ns) {
        collector->status = AppCatalogStatusTimedOut;
        return false;
    }
    if (collector->nodes >= collector->options->max_nodes) {
        collector->status = AppCatalogStatusQuota;
        return false;
    }
    collector->nodes++;
    return true;
}

static app_catalog_bytes_t collector_copy(catalog_collector_t *collector, const void *bytes, size_t length) {
    if (length == 0) return (app_catalog_bytes_t){0};
    if (bytes == NULL || length > AppCatalogMaxBytes - collector->storage_used) {
        collector->status = AppCatalogStatusQuota;
        return (app_catalog_bytes_t){0};
    }
    size_t needed = collector->storage_used + length;
    uint8_t *destination = collector->storage + collector->storage_used;
    memcpy(destination, bytes, length);
    collector->storage_used = needed;
    return (app_catalog_bytes_t){ destination, length };
}

static app_catalog_bytes_t collector_utf8(catalog_collector_t *collector, const wchar_t *wide, size_t units) {
    if (units == 0) return (app_catalog_bytes_t){0};
    if (wide == NULL || units > INT_MAX) {
        collector->status = AppCatalogStatusInvalidText;
        return (app_catalog_bytes_t){0};
    }
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)units, NULL, 0, NULL, NULL);
    if (length <= 0 || (size_t)length > InteropMaxStringBytes) {
        collector->status = length <= 0 ? AppCatalogStatusInvalidText : AppCatalogStatusQuota;
        return (app_catalog_bytes_t){0};
    }
    char *temporary = malloc((size_t)length);
    if (temporary == NULL) {
        collector->status = AppCatalogStatusOutOfMemory;
        return (app_catalog_bytes_t){0};
    }
    int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)units, temporary, length, NULL, NULL);
    app_catalog_bytes_t result = written == length ? collector_copy(collector, temporary, (size_t)length) : (app_catalog_bytes_t){0};
    if (written != length) collector->status = AppCatalogStatusInvalidText;
    free(temporary);
    return result;
}

static app_catalog_bytes_t collector_identity_path(catalog_collector_t *collector, const wchar_t *canonical, size_t units) {
    if (units == 0 || units > AppCatalogMaxPathUnits || units > INT_MAX) {
        collector->status = AppCatalogStatusQuota;
        return (app_catalog_bytes_t){0};
    }
    int mapped_units = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, canonical, (int)units,
                                     NULL, 0, NULL, NULL, 0);
    if (mapped_units <= 0 || (size_t)mapped_units > AppCatalogMaxPathUnits) {
        collector->status = AppCatalogStatusInvalidPathIdentity;
        return (app_catalog_bytes_t){0};
    }
    wchar_t *upper = malloc((size_t)mapped_units * sizeof(wchar_t));
    if (upper == NULL) {
        collector->status = AppCatalogStatusOutOfMemory;
        return (app_catalog_bytes_t){0};
    }
    int written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, canonical, (int)units,
                               upper, mapped_units, NULL, NULL, 0);
    app_catalog_bytes_t result = written == mapped_units ?
        collector_copy(collector, upper, (size_t)mapped_units * sizeof(wchar_t)) : (app_catalog_bytes_t){0};
    if (written != mapped_units) collector->status = AppCatalogStatusInvalidPathIdentity;
    free(upper);
    return result;
}

static bool canonical_target(catalog_collector_t *collector, const wchar_t *source,
                             wchar_t *output, size_t capacity, size_t *units) {
    if (source == NULL || output == NULL || units == NULL || capacity > MAXDWORD || capacity < 8) return false;
    if (!cleanup_owner_empty(collector->cleanup_owner)) {
        collector->status = AppCatalogStatusInvalidArgument;
        return false;
    }
    HANDLE file = CreateFileW(source, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD length = GetFinalPathNameByHandleW(file, output, (DWORD)capacity,
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    bool success = length != 0 && length < capacity;
    if (success) {
        if (length >= 8 && _wcsnicmp(output, L"\\\\?\\UNC\\", 8) == 0) {
            memmove(output + 2, output + 8, ((size_t)length - 8 + 1) * sizeof(wchar_t));
            output[0] = L'\\';
            output[1] = L'\\';
            length -= 6;
        } else if (length >= 4 && wcsncmp(output, L"\\\\?\\", 4) == 0) {
            memmove(output, output + 4, ((size_t)length - 4 + 1) * sizeof(wchar_t));
            length -= 4;
        }
        if (!(((output[0] >= L'A' && output[0] <= L'Z') || (output[0] >= L'a' && output[0] <= L'z')) && output[1] == L':') &&
            !(output[0] == L'\\' && output[1] == L'\\')) success = false;
    }
    if (!close_file_handle(file, collector->cleanup_owner,
                           collector->close_fault, collector->close_fault_context)) {
        collector->status = AppCatalogStatusNativeFailure;
        success = false;
    }
    if (!success) return false;
    *units = length;
    return true;
}

static app_catalog_bytes_t directory_of_path(catalog_collector_t *collector, const wchar_t *path, size_t units) {
    size_t separator = units;
    while (separator != 0 && path[separator - 1] != L'\\' && path[separator - 1] != L'/') separator--;
    size_t length = separator == 0 ? 0 : separator - 1;
    if (length == 2 && path[1] == L':') length = 3;
    else if (length == 0) length = separator;
    return collector_utf8(collector, path, length);
}

static bool add_item(catalog_collector_t *collector, app_catalog_source_t source,
                     app_catalog_identity_kind_t kind, const wchar_t *identity_wide, size_t identity_units,
                     const wchar_t *aumid_wide, size_t aumid_units, const wchar_t *name_wide, size_t name_units,
                     const wchar_t *target_wide, size_t target_units, const wchar_t *arguments_wide, size_t arguments_units,
                     const wchar_t *working_wide, size_t working_units, const wchar_t *package_wide,
                     size_t package_units, const wchar_t *source_id_wide, size_t source_id_units, bool desktop) {
    (void)target_units;
    if (collector->count >= AppCatalogMaxEntries || collector->count >= collector->options->max_nodes) {
        collector->status = AppCatalogStatusQuota;
        return false;
    }
    wchar_t canonical[AppCatalogMaxPathUnits + 1];
    size_t canonical_units = 0;
    bool has_target = target_wide != NULL && target_units != 0;
    if (has_target && !canonical_target(collector, target_wide, canonical, AppCatalogMaxPathUnits, &canonical_units)) {
        if (collector->status == AppCatalogStatusOk) collector->status = AppCatalogStatusNativeFailure;
        return false;
    }
    app_catalog_candidate_t candidate = {0};
    candidate.source = source;
    candidate.identity_kind = kind;
    candidate.display_name = collector_utf8(collector, name_wide, name_units);
    candidate.target_path = has_target ? collector_utf8(collector, canonical, canonical_units) : (app_catalog_bytes_t){0};
    candidate.arguments = collector_utf8(collector, arguments_wide, arguments_units);
    candidate.working_directory = working_units == 0 && has_target ? directory_of_path(collector, canonical, canonical_units) :
        collector_utf8(collector, working_wide, working_units);
    candidate.source_identifier = collector_utf8(collector, source_id_wide, source_id_units);
    candidate.package_family = collector_utf8(collector, package_wide, package_units);
    candidate.desktop_shortcut = desktop;
    if (kind == AppCatalogIdentityAumid) {
        candidate.identity = collector_utf8(collector, identity_wide, identity_units);
        candidate.aumid = collector_utf8(collector, aumid_wide, aumid_units);
    } else {
        candidate.identity = collector_identity_path(collector, canonical, canonical_units);
    }
    if (collector->status != AppCatalogStatusOk) return false;
    if (collector->count == collector->candidate_capacity) {
        size_t next = collector->candidate_capacity == 0 ? 32 : collector->candidate_capacity * 2;
        if (next > AppCatalogMaxEntries) next = AppCatalogMaxEntries;
        app_catalog_candidate_t *grown = realloc(collector->candidates, next * sizeof(*grown));
        if (grown == NULL) {
            collector->status = AppCatalogStatusOutOfMemory;
            return false;
        }
        collector->candidates = grown;
        collector->candidate_capacity = next;
    }
    collector->candidates[collector->count++] = candidate;
    return true;
}

static bool property_string(IPropertyStore *store, REFPROPERTYKEY key, wchar_t *buffer, size_t capacity, size_t *units) {
    if (store == NULL || buffer == NULL || capacity == 0 || units == NULL) return false;
    PROPVARIANT value;
    PropVariantInit(&value);
    HRESULT result = IPropertyStore_GetValue(store, key, &value);
    if (FAILED(result)) { PropVariantClear(&value); return false; }
    const wchar_t *text = NULL;
    if (value.vt == VT_LPWSTR) text = value.pwszVal;
    else if (value.vt == VT_BSTR) text = value.bstrVal;
    if (text == NULL) { PropVariantClear(&value); return false; }
    size_t length = 0;
    while (length < capacity && text[length] != L'\0') length++;
    if (length == capacity) { PropVariantClear(&value); return false; }
    memcpy(buffer, text, (length + 1) * sizeof(wchar_t));
    *units = length;
    PropVariantClear(&value);
    return true;
}

static bool property_bool(IPropertyStore *store, REFPROPERTYKEY key) {
    if (store == NULL) return false;
    PROPVARIANT value;
    PropVariantInit(&value);
    HRESULT result = IPropertyStore_GetValue(store, key, &value);
    bool enabled = SUCCEEDED(result) && ((value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE) ||
                                        (value.vt == VT_UI4 && value.ulVal != 0));
    PropVariantClear(&value);
    return enabled;
}

static bool add_apps_folder_item(catalog_collector_t *collector, IShellItem *item) {
    if (!collector_checkpoint(collector)) return false;
    IShellItem2 *item2 = NULL;
    HRESULT result = IShellItem_QueryInterface(item, &IID_IShellItem2, (void **)&item2);
    if (FAILED(result) || item2 == NULL) return true;
    IPropertyStore *store = NULL;
    result = IShellItem2_GetPropertyStore(item2, GPS_DEFAULT, &IID_IPropertyStore, (void **)&store);
    IShellItem2_Release(item2);
    if (FAILED(result)) return true;
    if (property_bool(store, &PKEY_AppUserModel_IsDestListSeparator)) {
        IPropertyStore_Release(store);
        return true;
    }
    wchar_t name[InteropMaxStringBytes + 1], aumid[InteropMaxStringBytes + 1], package[InteropMaxStringBytes + 1];
    wchar_t target[AppCatalogMaxPathUnits + 1], source_id[AppCatalogMaxPathUnits + 1];
    size_t name_units = 0, aumid_units = 0, package_units = 0, target_units = 0, source_id_units = 0;
    PWSTR display_name = NULL;
    bool have_name = SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_NORMALDISPLAY, &display_name)) && display_name != NULL;
    if (have_name) {
        while (name_units <= InteropMaxStringBytes && display_name[name_units] != L'\0') name_units++;
        if (name_units > InteropMaxStringBytes) have_name = false;
        else memcpy(name, display_name, (name_units + 1) * sizeof(wchar_t));
    }
    CoTaskMemFree(display_name);
    bool have_aumid = property_string(store, &PKEY_AppUserModel_ID, aumid, InteropMaxStringBytes + 1, &aumid_units);
    bool have_target = property_string(store, &CatalogLinkTargetParsingPath, target, AppCatalogMaxPathUnits + 1, &target_units);
    bool have_package = property_string(store, &CatalogPackageFamilyName, package, InteropMaxStringBytes + 1, &package_units);
    IPropertyStore_Release(store);
    if (!have_name || name_units == 0 || (!have_target && (!have_aumid || aumid_units == 0))) return true;
    PWSTR parsing_name = NULL;
    if (FAILED(IShellItem_GetDisplayName(item, SIGDN_DESKTOPABSOLUTEPARSING, &parsing_name)) || parsing_name == NULL) {
        /* Shell item identifiers may not expose a parsing name; use display name as a stable fallback. */
        memcpy(source_id, name, (name_units + 1) * sizeof(wchar_t));
        source_id_units = name_units;
    } else {
        while (source_id_units <= AppCatalogMaxPathUnits && parsing_name[source_id_units] != L'\0') source_id_units++;
        if (source_id_units > AppCatalogMaxPathUnits) {
            memcpy(source_id, name, (name_units + 1) * sizeof(wchar_t));
            source_id_units = name_units;
        } else memcpy(source_id, parsing_name, (source_id_units + 1) * sizeof(wchar_t));
    }
    CoTaskMemFree(parsing_name);
    app_catalog_source_t source = AppCatalogSourceAppsFolder;
    app_catalog_identity_kind_t kind = have_aumid && aumid_units != 0 ? AppCatalogIdentityAumid : AppCatalogIdentityCanonicalPath;
    bool added = add_item(collector, source, kind,
                          kind == AppCatalogIdentityAumid ? aumid : target,
                          kind == AppCatalogIdentityAumid ? aumid_units : target_units,
                          kind == AppCatalogIdentityAumid ? aumid : NULL,
                          kind == AppCatalogIdentityAumid ? aumid_units : 0,
                          name, name_units, have_target ? target : NULL, have_target ? target_units : 0, L"", 0, L"", 0,
                          have_package ? package : L"", have_package ? package_units : 0,
                          source_id, source_id_units, false);
    return added;
}

static bool enumerate_apps_folder(catalog_collector_t *collector) {
    IShellItem *folder = NULL;
    HRESULT result = SHGetKnownFolderItem(&FOLDERID_AppsFolder, KF_FLAG_DEFAULT, NULL, &IID_IShellItem, (void **)&folder);
    if (FAILED(result)) { collector->status = AppCatalogStatusNativeFailure; return false; }
    IEnumShellItems *enumerator = NULL;
    result = IShellItem_BindToHandler(folder, NULL, &BHID_EnumItems, &IID_IEnumShellItems, (void **)&enumerator);
    IShellItem_Release(folder);
    if (FAILED(result)) { collector->status = AppCatalogStatusNativeFailure; return false; }
    bool ok = true;
    for (;;) {
        IShellItem *item = NULL;
        ULONG fetched = 0;
        result = IEnumShellItems_Next(enumerator, 1, &item, &fetched);
        if (result == S_FALSE && fetched == 0 && item == NULL) break;
        if (result != S_OK || fetched != 1 || item == NULL) {
            if (item != NULL) IShellItem_Release(item);
            collector->status = AppCatalogStatusNativeFailure;
            ok = false;
            break;
        }
        if (!add_apps_folder_item(collector, item)) ok = false;
        IShellItem_Release(item);
        if (!ok) break;
    }
    IEnumShellItems_Release(enumerator);
    return ok;
}

static bool shortcut_item(catalog_collector_t *collector, app_catalog_source_t source,
                          const wchar_t *shortcut_path, bool desktop) {
    if (!collector_checkpoint(collector)) return false;
    IShellLinkW *link = NULL;
    HRESULT result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (FAILED(result)) { collector->status = AppCatalogStatusNativeFailure; return false; }
    IPersistFile *persist = NULL;
    result = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist);
    if (FAILED(result)) { IShellLinkW_Release(link); collector->status = AppCatalogStatusNativeFailure; return false; }
    result = IPersistFile_Load(persist, shortcut_path, STGM_READ);
    IPersistFile_Release(persist);
    if (FAILED(result)) { IShellLinkW_Release(link); return true; }
    wchar_t target[AppCatalogMaxPathUnits + 1], arguments[InteropMaxStringBytes + 1], working[InteropMaxStringBytes + 1];
    wchar_t description[InteropMaxStringBytes + 1];
    WIN32_FIND_DATAW find_data;
    memset(&find_data, 0, sizeof(find_data));
    HRESULT target_result = IShellLinkW_GetPath(link, target, AppCatalogMaxPathUnits, &find_data, 0);
    HRESULT args_result = IShellLinkW_GetArguments(link, arguments, InteropMaxStringBytes + 1);
    HRESULT work_result = IShellLinkW_GetWorkingDirectory(link, working, InteropMaxStringBytes + 1);
    HRESULT desc_result = IShellLinkW_GetDescription(link, description, InteropMaxStringBytes + 1);
    IPropertyStore *properties = NULL;
    wchar_t aumid[InteropMaxStringBytes + 1], package[InteropMaxStringBytes + 1];
    size_t aumid_units = 0, package_units = 0;
    bool have_aumid = SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **)&properties)) && properties != NULL &&
                      property_string(properties, &PKEY_AppUserModel_ID, aumid, InteropMaxStringBytes + 1, &aumid_units);
    bool have_package = properties != NULL &&
                        property_string(properties, &CatalogPackageFamilyName, package, InteropMaxStringBytes + 1, &package_units);
    if (properties != NULL) IPropertyStore_Release(properties);
    IShellLinkW_Release(link);
    if (FAILED(target_result) || target[0] == L'\0') return true;
    size_t target_units = 0, arguments_units = 0, working_units = 0, description_units = 0;
    while (target_units < AppCatalogMaxPathUnits && target[target_units] != L'\0') target_units++;
    if (SUCCEEDED(args_result)) while (arguments_units < InteropMaxStringBytes && arguments[arguments_units] != L'\0') arguments_units++;
    if (SUCCEEDED(work_result)) while (working_units < InteropMaxStringBytes && working[working_units] != L'\0') working_units++;
    if (SUCCEEDED(desc_result)) while (description_units < InteropMaxStringBytes && description[description_units] != L'\0') description_units++;
    if (target_units == AppCatalogMaxPathUnits || arguments_units == InteropMaxStringBytes ||
        working_units == InteropMaxStringBytes || description_units == InteropMaxStringBytes) return true;
    if (description_units == 0) {
        const wchar_t *base = wcsrchr(shortcut_path, L'\\');
        base = base == NULL ? shortcut_path : base + 1;
        while (description_units < InteropMaxStringBytes && base[description_units] != L'\0') description_units++;
        if (description_units > InteropMaxStringBytes) return true;
        memcpy(description, base, (description_units + 1) * sizeof(wchar_t));
        wchar_t *extension = wcsrchr(description, L'.');
        if (extension != NULL && _wcsicmp(extension, L".lnk") == 0) { *extension = 0; description_units = (size_t)(extension - description); }
    }
    app_catalog_identity_kind_t identity_kind = have_aumid && aumid_units != 0 ? AppCatalogIdentityAumid : AppCatalogIdentityCanonicalPath;
    return add_item(collector, source, identity_kind,
                    identity_kind == AppCatalogIdentityAumid ? aumid : target,
                    identity_kind == AppCatalogIdentityAumid ? aumid_units : target_units,
                    identity_kind == AppCatalogIdentityAumid ? aumid : NULL,
                    identity_kind == AppCatalogIdentityAumid ? aumid_units : 0,
                    description, description_units, target, target_units,
                    arguments, arguments_units, working, working_units,
                    have_package ? package : NULL, have_package ? package_units : 0,
                    shortcut_path, wcslen(shortcut_path), desktop);
}

static bool has_lnk_extension(const wchar_t *name) {
    const wchar_t *extension = wcsrchr(name, L'.');
    return extension != NULL && _wcsicmp(extension, L".lnk") == 0;
}

static bool enumerate_shortcut_directory(catalog_collector_t *collector, const wchar_t *directory,
                                         app_catalog_source_t source, bool desktop, bool recursive, size_t depth) {
    if (depth > 64 || !collector_checkpoint(collector)) {
        if (depth > 64) collector->status = AppCatalogStatusQuota;
        return false;
    }
    size_t base_length = wcslen(directory);
    if (base_length == 0 || base_length + 3 >= AppCatalogMaxPathUnits) { collector->status = AppCatalogStatusQuota; return false; }
    wchar_t pattern[AppCatalogMaxPathUnits + 1];
    memcpy(pattern, directory, base_length * sizeof(wchar_t));
    size_t pattern_length = base_length;
    if (pattern[pattern_length - 1] != L'\\') pattern[pattern_length++] = L'\\';
    pattern[pattern_length++] = L'*';
    pattern[pattern_length] = 0;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return true;
        collector->status = AppCatalogStatusNativeFailure;
        return false;
    }
    bool ok = true;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (!collector_checkpoint(collector)) { ok = false; break; }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
        size_t name_length = wcslen(data.cFileName);
        if (base_length + name_length + 2 >= AppCatalogMaxPathUnits) { collector->status = AppCatalogStatusQuota; ok = false; break; }
        wchar_t child[AppCatalogMaxPathUnits + 1];
        memcpy(child, directory, base_length * sizeof(wchar_t));
        size_t child_length = base_length;
        if (child_length != 0 && child[child_length - 1] != L'\\') child[child_length++] = L'\\';
        memcpy(child + child_length, data.cFileName, (name_length + 1) * sizeof(wchar_t));
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (recursive && !enumerate_shortcut_directory(collector, child, source, desktop, true, depth + 1)) { ok = false; break; }
        } else if (has_lnk_extension(data.cFileName) && !shortcut_item(collector, source, child, desktop)) { ok = false; break; }
    } while (FindNextFileW(find, &data));
    DWORD error = GetLastError();
    if (!close_search_handle(find, collector->cleanup_owner,
                             collector->close_fault, collector->close_fault_context)) {
        collector->status = AppCatalogStatusNativeFailure;
        ok = false;
    }
    if (ok && error != ERROR_NO_MORE_FILES) { collector->status = AppCatalogStatusNativeFailure; ok = false; }
    return ok;
}

static bool enumerate_known_folder(catalog_collector_t *collector, REFKNOWNFOLDERID folder_id,
                                   app_catalog_source_t source, bool desktop, bool recursive) {
    PWSTR path = NULL;
    HRESULT result = SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, NULL, &path);
    if (FAILED(result) || path == NULL) {
        CoTaskMemFree(path);
        collector->status = AppCatalogStatusNativeFailure;
        return false;
    }
    bool ok = enumerate_shortcut_directory(collector, path, source, desktop, recursive, 0);
    CoTaskMemFree(path);
    return ok;
}

static app_catalog_status_t enumerate_internal(app_catalog_allocator_t allocator,
                                               const app_catalog_enumeration_options_t *options,
                                               const app_catalog_windows_source_roots_t *roots,
                                               app_catalog_snapshot_t *output) {
    if (allocator.allocate == NULL || allocator.deallocate == NULL || options == NULL || output == NULL ||
        !cleanup_owner_empty(options->cleanup_owner) ||
        output->entries != NULL || output->storage != NULL || output->count != 0 ||
        output->allocation_size != 0 || output->storage_size != 0 || output->duplicate_launch_conflicts != 0 ||
        output->allocator.allocate != NULL || output->allocator.deallocate != NULL ||
        options->max_nodes == 0 || options->max_nodes > 65536) return AppCatalogStatusInvalidArgument;
    if (roots != NULL) {
        const wchar_t *paths[] = { roots->user_programs, roots->common_programs, roots->user_desktop, roots->public_desktop };
        for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); index++) {
            if (paths[index] == NULL) return AppCatalogStatusInvalidArgument;
            size_t length = 0;
            while (length <= AppCatalogMaxPathUnits && paths[index][length] != L'\0') length++;
            if (length == 0 || length > AppCatalogMaxPathUnits) return AppCatalogStatusInvalidArgument;
        }
    }
    if (options->is_cancelled != NULL && options->is_cancelled(options->cancel_context)) return AppCatalogStatusCancelled;
    if (options->deadline_monotonic_ns != 0 && GetTickCount64() * UINT64_C(1000000) >= options->deadline_monotonic_ns)
        return AppCatalogStatusTimedOut;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return AppCatalogStatusNativeFailure;
    catalog_collector_t collector = {
        .options = options,
        .cleanup_owner = options->cleanup_owner,
        .close_fault = roots == NULL ? NULL : roots->close_fault,
        .close_fault_context = roots == NULL ? NULL : roots->close_fault_context,
        .status = AppCatalogStatusOk,
    };
    collector.storage = malloc(AppCatalogMaxBytes);
    if (collector.storage == NULL) {
        CoUninitialize();
        return AppCatalogStatusOutOfMemory;
    }
    collector.storage_capacity = AppCatalogMaxBytes;
    bool ok = roots == NULL ?
        (enumerate_apps_folder(&collector) &&
         enumerate_known_folder(&collector, &FOLDERID_Programs, AppCatalogSourceUserPrograms, false, true) &&
         enumerate_known_folder(&collector, &FOLDERID_CommonPrograms, AppCatalogSourceCommonPrograms, false, true) &&
         enumerate_known_folder(&collector, &FOLDERID_Desktop, AppCatalogSourceUserDesktop, true, false) &&
         enumerate_known_folder(&collector, &FOLDERID_PublicDesktop, AppCatalogSourcePublicDesktop, true, false)) :
        (enumerate_shortcut_directory(&collector, roots->user_programs, AppCatalogSourceUserPrograms, false, true, 0) &&
         enumerate_shortcut_directory(&collector, roots->common_programs, AppCatalogSourceCommonPrograms, false, true, 0) &&
         enumerate_shortcut_directory(&collector, roots->user_desktop, AppCatalogSourceUserDesktop, true, false, 0) &&
         enumerate_shortcut_directory(&collector, roots->public_desktop, AppCatalogSourcePublicDesktop, true, false, 0));
    app_catalog_status_t status = collector.status;
    if (ok && status == AppCatalogStatusOk)
        status = app_catalog_snapshot_build(allocator, collector.candidates, collector.count, output);
    free(collector.candidates);
    free(collector.storage);
    CoUninitialize();
    return status;
}

app_catalog_status_t app_catalog_cleanup_owner_retry(app_catalog_cleanup_owner_t *owner) {
    if (owner == NULL) return AppCatalogStatusInvalidArgument;
    if (owner->count > AppCatalogCleanupMaxHandles) return AppCatalogStatusInvalidArgument;
    while (owner->count != 0) {
        size_t index = owner->count - 1;
        HANDLE handle = (HANDLE)owner->native_handles[index];
        BOOL closed = FALSE;
        if (handle == NULL) return AppCatalogStatusInvalidArgument;
        if (owner->kinds[index] == AppCatalogCleanupHandleWin32) closed = CloseHandle(handle);
        else if (owner->kinds[index] == AppCatalogCleanupHandleFind) closed = FindClose(handle);
        else return AppCatalogStatusInvalidArgument;
        if (!closed) return AppCatalogStatusNativeFailure;
        owner->native_handles[index] = NULL;
        owner->kinds[index] = AppCatalogCleanupHandleNone;
        owner->count--;
    }
    return AppCatalogStatusOk;
}

app_catalog_status_t app_catalog_enumerate(app_catalog_allocator_t allocator,
                                           const app_catalog_enumeration_options_t *options,
                                           app_catalog_snapshot_t *output) {
    return enumerate_internal(allocator, options, NULL, output);
}

app_catalog_status_t app_catalog_windows_enumerate_fixture_roots(
    app_catalog_allocator_t allocator,
    const app_catalog_enumeration_options_t *options,
    const app_catalog_windows_source_roots_t *roots,
    app_catalog_snapshot_t *output) {
    if (roots == NULL) return AppCatalogStatusInvalidArgument;
    return enumerate_internal(allocator, options, roots, output);
}
#endif
