/** @file app_catalog.c @brief Bounded immutable catalog construction and diff. */
#include "app_catalog.h"
#include "app_catalog_validation.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct normalized_candidate_t {
    const app_catalog_candidate_t *candidate;
    uint8_t key[InteropMaxAppKeyBytes];
    size_t key_length;
} normalized_candidate_t;

static bool allocator_valid(app_catalog_allocator_t allocator) {
    return allocator.allocate != NULL && allocator.deallocate != NULL;
}

static app_catalog_status_t candidate_validate(const app_catalog_candidate_t *candidate,
                                               normalized_candidate_t *normalized) {
    if (candidate == NULL || normalized == NULL) return AppCatalogStatusInvalidArgument;
    if (candidate->source < AppCatalogSourceAppsFolder || candidate->source > AppCatalogSourcePublicDesktop)
        return AppCatalogStatusInvalidArgument;
    bool is_desktop = candidate->source == AppCatalogSourceUserDesktop || candidate->source == AppCatalogSourcePublicDesktop;
    if (candidate->desktop_shortcut != is_desktop) return AppCatalogStatusInvalidArgument;
    if (!app_catalog_utf8_valid(candidate->display_name.data, candidate->display_name.length, 255, false) ||
        !app_catalog_utf8_valid(candidate->target_path.data, candidate->target_path.length, InteropMaxStringBytes, true) ||
        !app_catalog_utf8_valid(candidate->arguments.data, candidate->arguments.length, InteropMaxStringBytes, true) ||
        !app_catalog_utf8_valid(candidate->working_directory.data, candidate->working_directory.length, InteropMaxStringBytes, true) ||
        !app_catalog_utf8_valid(candidate->source_identifier.data, candidate->source_identifier.length, AppCatalogMaxSourceBytes, false) ||
        !app_catalog_utf8_valid(candidate->aumid.data, candidate->aumid.length, InteropMaxStringBytes, true) ||
        !app_catalog_utf8_valid(candidate->package_family.data, candidate->package_family.length, InteropMaxStringBytes, true) ||
        !app_catalog_package_family_valid(candidate->package_family.data, candidate->package_family.length)) return AppCatalogStatusInvalidText;
    if (candidate->identity_kind == AppCatalogIdentityAumid) {
        if (!app_catalog_utf8_valid(candidate->identity.data, candidate->identity.length, InteropMaxStringBytes, false) ||
            candidate->identity.length != candidate->aumid.length ||
            (candidate->identity.length != 0 && memcmp(candidate->identity.data, candidate->aumid.data, candidate->identity.length) != 0))
            return AppCatalogStatusInvalidText;
        size_t key_length = 0;
        int result = interop_app_key_from_aumid(candidate->identity.data, candidate->identity.length,
                                                normalized->key, sizeof(normalized->key), &key_length);
        if (result != InteropErrorNone) return result == InteropErrorQuotaExceeded ? AppCatalogStatusQuota : AppCatalogStatusInvalidText;
        normalized->key_length = key_length;
    } else if (candidate->identity_kind == AppCatalogIdentityCanonicalPath) {
        if (candidate->aumid.length != 0 || candidate->target_path.length == 0 ||
            !app_catalog_path_identity_valid(candidate->identity.data, candidate->identity.length, AppCatalogMaxPathUnits))
            return AppCatalogStatusInvalidPathIdentity;
        static const uint8_t HexDigits[] = "0123456789abcdef";
        uint8_t digest[InteropHashBytes];
        interop_sha256(candidate->identity.data, candidate->identity.length, digest);
        normalized->key[0] = 'p';
        normalized->key[1] = '-';
        for (size_t index = 0; index < 16; index++) {
            normalized->key[2 + index * 2] = HexDigits[digest[index] >> 4];
            normalized->key[3 + index * 2] = HexDigits[digest[index] & 0x0F];
        }
        normalized->key_length = 34;
    } else return AppCatalogStatusInvalidArgument;
    normalized->candidate = candidate;
    return AppCatalogStatusOk;
}

static int byte_compare(app_catalog_bytes_t left, app_catalog_bytes_t right) {
    size_t shared = left.length < right.length ? left.length : right.length;
    int result = shared == 0 ? 0 : memcmp(left.data, right.data, shared);
    if (result != 0) return result;
    return left.length < right.length ? -1 : left.length > right.length ? 1 : 0;
}

static int normalized_compare(const void *left_pointer, const void *right_pointer) {
    const normalized_candidate_t *left = left_pointer;
    const normalized_candidate_t *right = right_pointer;
    app_catalog_bytes_t left_key = { left->key, left->key_length };
    app_catalog_bytes_t right_key = { right->key, right->key_length };
    int result = byte_compare(left_key, right_key);
    if (result != 0) return result;
    /* The frozen key prefixes make AUMID and path key namespaces disjoint. */
    return byte_compare(left->candidate->identity, right->candidate->identity);
}

static unsigned source_rank(const app_catalog_candidate_t *candidate) {
    if (candidate->source == AppCatalogSourceAppsFolder && candidate->identity_kind == AppCatalogIdentityAumid) return 0;
    switch (candidate->source) {
        case AppCatalogSourceUserPrograms: return 1;
        case AppCatalogSourceCommonPrograms: return 2;
        case AppCatalogSourceAppsFolder: return 2;
        case AppCatalogSourceUserDesktop: return 3;
        case AppCatalogSourcePublicDesktop: return 4;
    }
    return UINT_MAX;
}

static int selected_compare(const normalized_candidate_t *left, const normalized_candidate_t *right) {
    unsigned left_rank = source_rank(left->candidate);
    unsigned right_rank = source_rank(right->candidate);
    if (left_rank != right_rank) return left_rank < right_rank ? -1 : 1;
    int result = byte_compare(left->candidate->source_identifier, right->candidate->source_identifier);
    if (result != 0) return result;
    result = byte_compare(left->candidate->target_path, right->candidate->target_path);
    if (result != 0) return result;
    result = byte_compare(left->candidate->arguments, right->candidate->arguments);
    if (result != 0) return result;
    result = byte_compare(left->candidate->working_directory, right->candidate->working_directory);
    if (result != 0) return result;
    result = byte_compare(left->candidate->display_name, right->candidate->display_name);
    if (result != 0) return result;
    return 0;
}

static bool launch_tuple_equal(const app_catalog_candidate_t *left, const app_catalog_candidate_t *right) {
    return byte_compare(left->target_path, right->target_path) == 0 &&
           byte_compare(left->arguments, right->arguments) == 0 &&
           byte_compare(left->working_directory, right->working_directory) == 0;
}

static bool snapshot_empty(const app_catalog_snapshot_t *snapshot) {
    return snapshot != NULL && snapshot->entries == NULL && snapshot->storage == NULL && snapshot->count == 0 &&
           snapshot->allocation_size == 0 && snapshot->storage_size == 0;
}

void app_catalog_snapshot_init(app_catalog_snapshot_t *snapshot) {
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
}

void app_catalog_snapshot_deinit(app_catalog_snapshot_t *snapshot) {
    if (snapshot == NULL) return;
    app_catalog_allocator_t allocator = snapshot->allocator;
    if (snapshot->entries != NULL && allocator_valid(allocator)) allocator.deallocate(allocator.context, snapshot->entries, snapshot->allocation_size);
    if (snapshot->storage != NULL && allocator_valid(allocator)) allocator.deallocate(allocator.context, snapshot->storage, snapshot->storage_size);
    memset(snapshot, 0, sizeof(*snapshot));
}

static size_t candidate_copy_size(const app_catalog_candidate_t *candidate) {
    return candidate->identity.length + candidate->display_name.length + candidate->target_path.length +
           candidate->arguments.length + candidate->working_directory.length + candidate->aumid.length;
}

static size_t candidate_input_size(const app_catalog_candidate_t *candidate) {
    size_t size = candidate_copy_size(candidate);
    size += candidate->source_identifier.length;
    size += candidate->package_family.length;
    return size;
}

static void assign_slice(app_catalog_bytes_t *destination, uint8_t **cursor, app_catalog_bytes_t source) {
    destination->length = source.length;
    destination->data = source.length == 0 ? NULL : *cursor;
    if (source.length != 0) {
        memcpy(*cursor, source.data, source.length);
        *cursor += source.length;
    }
}

app_catalog_status_t app_catalog_snapshot_build(app_catalog_allocator_t allocator,
                                                const app_catalog_candidate_t *candidates,
                                                size_t count,
                                                app_catalog_snapshot_t *output) {
    if (!allocator_valid(allocator) || output == NULL || !snapshot_empty(output) || (count != 0 && candidates == NULL))
        return AppCatalogStatusInvalidArgument;
    if (count > AppCatalogMaxEntries) return AppCatalogStatusQuota;
    if (count == 0) {
        app_catalog_snapshot_t empty = {0};
        empty.allocator = allocator;
        *output = empty;
        return AppCatalogStatusOk;
    }
    size_t normalized_size = count * sizeof(normalized_candidate_t);
    normalized_candidate_t *normalized = allocator.allocate(allocator.context, normalized_size);
    if (normalized == NULL) return AppCatalogStatusOutOfMemory;
    app_catalog_status_t status = AppCatalogStatusOk;
    size_t input_bytes = 0;
    for (size_t index = 0; index < count; index++) {
        status = candidate_validate(&candidates[index], &normalized[index]);
        if (status != AppCatalogStatusOk) goto cleanup_normalized;
        size_t bytes = candidate_input_size(&candidates[index]);
        if (bytes > AppCatalogMaxBytes - input_bytes) { status = AppCatalogStatusQuota; goto cleanup_normalized; }
        input_bytes += bytes;
    }
    qsort(normalized, count, sizeof(*normalized), normalized_compare);
    size_t entry_count = 0;
    size_t storage_size = 0;
    size_t duplicate_launch_conflicts = 0;
    for (size_t start = 0; start < count;) {
        size_t end = start + 1;
        while (end < count && byte_compare((app_catalog_bytes_t){ normalized[start].key, normalized[start].key_length },
                                           (app_catalog_bytes_t){ normalized[end].key, normalized[end].key_length }) == 0) end++;
        for (size_t index = start + 1; index < end; index++) {
            if (normalized[index].candidate->identity_kind != normalized[start].candidate->identity_kind ||
                byte_compare(normalized[index].candidate->identity, normalized[start].candidate->identity) != 0) {
                status = AppCatalogStatusIdentityCollision;
                goto cleanup_normalized;
            }
        }
        size_t selected = start;
        bool desktop_shortcut = false;
        bool packaged = false;
        for (size_t index = start; index < end; index++) {
            if (selected_compare(&normalized[index], &normalized[selected]) < 0) selected = index;
            desktop_shortcut |= normalized[index].candidate->desktop_shortcut;
            packaged |= normalized[index].candidate->package_family.length != 0;
        }
        const app_catalog_candidate_t *chosen = normalized[selected].candidate;
        for (size_t index = start; index < end; index++)
            if (!launch_tuple_equal(chosen, normalized[index].candidate)) duplicate_launch_conflicts++;
        size_t bytes = normalized[selected].key_length + candidate_copy_size(chosen);
        if (bytes > AppCatalogMaxBytes - storage_size) { status = AppCatalogStatusQuota; goto cleanup_normalized; }
        storage_size += bytes;
        entry_count++;
        start = end;
    }
    size_t entries_size = entry_count * sizeof(app_catalog_entry_t);
    app_catalog_entry_t *entries = allocator.allocate(allocator.context, entries_size);
    if (entries == NULL) { status = AppCatalogStatusOutOfMemory; goto cleanup_normalized; }
    uint8_t *storage = allocator.allocate(allocator.context, storage_size == 0 ? 1 : storage_size);
    if (storage == NULL) {
        allocator.deallocate(allocator.context, entries, entries_size);
        status = AppCatalogStatusOutOfMemory;
        goto cleanup_normalized;
    }
    size_t output_index = 0;
    uint8_t *cursor = storage;
    for (size_t start = 0; start < count;) {
        size_t end = start + 1;
        while (end < count && byte_compare((app_catalog_bytes_t){ normalized[start].key, normalized[start].key_length },
                                           (app_catalog_bytes_t){ normalized[end].key, normalized[end].key_length }) == 0) end++;
        size_t selected = start;
        bool desktop_shortcut = false;
        bool packaged = false;
        for (size_t index = start; index < end; index++) {
            if (selected_compare(&normalized[index], &normalized[selected]) < 0) selected = index;
            desktop_shortcut |= normalized[index].candidate->desktop_shortcut;
            packaged |= normalized[index].candidate->package_family.length != 0;
        }
        const app_catalog_candidate_t *chosen = normalized[selected].candidate;
        app_catalog_entry_t *entry = &entries[output_index++];
        assign_slice(&entry->app_key, &cursor, (app_catalog_bytes_t){ normalized[selected].key, normalized[selected].key_length });
        assign_slice(&entry->identity, &cursor, chosen->identity);
        entry->identity_kind = chosen->identity_kind;
        assign_slice(&entry->display_name, &cursor, chosen->display_name);
        assign_slice(&entry->target_path, &cursor, chosen->target_path);
        assign_slice(&entry->arguments, &cursor, chosen->arguments);
        assign_slice(&entry->working_directory, &cursor, chosen->working_directory);
        assign_slice(&entry->aumid, &cursor, chosen->aumid);
        entry->app_flags = (desktop_shortcut ? InteropAppDesktopShortcut : 0u) | (packaged ? InteropAppPackaged : 0u);
        start = end;
    }
    allocator.deallocate(allocator.context, normalized, normalized_size);
    app_catalog_snapshot_t built = {0};
    built.allocator = allocator;
    built.entries = entries;
    built.count = entry_count;
    built.allocation_size = entries_size;
    built.storage = storage;
    built.storage_size = storage_size == 0 ? 1 : storage_size;
    built.duplicate_launch_conflicts = duplicate_launch_conflicts;
    *output = built;
    return AppCatalogStatusOk;

cleanup_normalized:
    allocator.deallocate(allocator.context, normalized, normalized_size);
    return status;
}

void app_catalog_diff_deinit(app_catalog_diff_t *diff) {
    if (diff == NULL) return;
    if (diff->changes != NULL && allocator_valid(diff->allocator))
        diff->allocator.deallocate(diff->allocator.context, diff->changes, diff->allocation_size);
    memset(diff, 0, sizeof(*diff));
}

static bool entry_equal(const app_catalog_entry_t *left, const app_catalog_entry_t *right) {
    return left->identity_kind == right->identity_kind && left->app_flags == right->app_flags &&
           byte_compare(left->identity, right->identity) == 0 && byte_compare(left->display_name, right->display_name) == 0 &&
           byte_compare(left->target_path, right->target_path) == 0 && byte_compare(left->arguments, right->arguments) == 0 &&
           byte_compare(left->working_directory, right->working_directory) == 0 && byte_compare(left->aumid, right->aumid) == 0;
}

static bool diff_empty(const app_catalog_diff_t *diff) {
    return diff != NULL && diff->changes == NULL && diff->count == 0 && diff->allocation_size == 0;
}

app_catalog_status_t app_catalog_diff_build(const app_catalog_snapshot_t *before,
                                            const app_catalog_snapshot_t *after,
                                            app_catalog_diff_t *output) {
    if (before == NULL || after == NULL || output == NULL || !diff_empty(output) ||
        (before->count != 0 && before->entries == NULL) || (after->count != 0 && after->entries == NULL) ||
        before->count > AppCatalogMaxEntries || after->count > AppCatalogMaxEntries ||
        !allocator_valid(before->allocator) || !allocator_valid(after->allocator)) return AppCatalogStatusInvalidArgument;
    size_t capacity = before->count + after->count;
    size_t allocation_size = capacity * sizeof(app_catalog_change_t);
    app_catalog_change_t *changes = capacity == 0 ? NULL : before->allocator.allocate(before->allocator.context, allocation_size);
    if (capacity != 0 && changes == NULL) return AppCatalogStatusOutOfMemory;
    size_t before_index = 0;
    size_t after_index = 0;
    size_t change_count = 0;
    while (before_index < before->count || after_index < after->count) {
        if (before_index == before->count) {
            const app_catalog_entry_t *entry = &after->entries[after_index++];
            changes[change_count++] = (app_catalog_change_t){ AppCatalogChangeAdd, entry->app_key, NULL, entry };
            continue;
        }
        if (after_index == after->count) {
            const app_catalog_entry_t *entry = &before->entries[before_index++];
            changes[change_count++] = (app_catalog_change_t){ AppCatalogChangeRemove, entry->app_key, entry, NULL };
            continue;
        }
        const app_catalog_entry_t *old_entry = &before->entries[before_index];
        const app_catalog_entry_t *new_entry = &after->entries[after_index];
        int comparison = byte_compare(old_entry->app_key, new_entry->app_key);
        if (comparison < 0) {
            changes[change_count++] = (app_catalog_change_t){ AppCatalogChangeRemove, old_entry->app_key, old_entry, NULL };
            before_index++;
        } else if (comparison > 0) {
            changes[change_count++] = (app_catalog_change_t){ AppCatalogChangeAdd, new_entry->app_key, NULL, new_entry };
            after_index++;
        } else {
            if (!entry_equal(old_entry, new_entry))
                changes[change_count++] = (app_catalog_change_t){ AppCatalogChangeUpdate, new_entry->app_key, old_entry, new_entry };
            before_index++;
            after_index++;
        }
    }
    if (change_count == 0 && changes != NULL) {
        before->allocator.deallocate(before->allocator.context, changes, allocation_size);
        changes = NULL;
        allocation_size = 0;
    }
    app_catalog_diff_t built = {0};
    built.allocator = before->allocator;
    built.changes = changes;
    built.count = change_count;
    built.allocation_size = allocation_size;
    *output = built;
    return AppCatalogStatusOk;
}

void app_catalog_cleanup_owner_init(app_catalog_cleanup_owner_t *owner) {
    if (owner != NULL) memset(owner, 0, sizeof(*owner));
}

#ifndef _WIN32
app_catalog_status_t app_catalog_cleanup_owner_retry(app_catalog_cleanup_owner_t *owner) {
    if (owner == NULL) return AppCatalogStatusInvalidArgument;
    if (owner->count == 0) return AppCatalogStatusOk;
    return AppCatalogStatusUnsupportedPlatform;
}

app_catalog_status_t app_catalog_enumerate(app_catalog_allocator_t allocator,
                                           const app_catalog_enumeration_options_t *options,
                                           app_catalog_snapshot_t *output) {
    (void)allocator;
    (void)options;
    (void)output;
    return AppCatalogStatusUnsupportedPlatform;
}
#endif
