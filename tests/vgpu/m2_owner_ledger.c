/** @file m2_owner_ledger.c @brief Allocation-free exact identity qualification. */
#include "m2_owner_ledger.h"
static m2_owner_entry_t *entry_for(m2_owner_ledger_t *ledger, uint64_t id) {
    if (id) for (size_t i = 0; i < M2OwnerCapacity; ++i)
        if (ledger->entries[i].id == id) return &ledger->entries[i];
    ledger->failures |= M2OwnerInvalid;
    return NULL;
}
uint64_t m2_owner_reserve(m2_owner_ledger_t *ledger, uint32_t kind, uintptr_t parent) {
    if (!ledger) return 0;
    if ((kind != M2OwnerDc && kind != M2OwnerWindow) ||
        (kind == M2OwnerDc ? !parent : parent != 0)) {
        ledger->failures |= M2OwnerInvalid;
        return 0;
    }
    if (ledger->failures) return 0;
    if (!ledger->next_id || ledger->next_id == UINT64_MAX || ledger->acquired == UINT64_MAX) {
        ledger->failures |= M2OwnerLimit;
        return 0;
    }
    for (size_t i = 0; i < M2OwnerCapacity; ++i) if (!ledger->entries[i].id) {
        const uint64_t id = ledger->next_id++;
        ledger->entries[i] = (m2_owner_entry_t){.id=id, .parent=parent, .kind=kind};
        return id;
    }
    ledger->failures |= M2OwnerLimit;
    return 0;
}
int m2_owner_acquired(m2_owner_ledger_t *ledger, uint64_t id, uintptr_t value) {
    if (!ledger) return 0;
    m2_owner_entry_t *entry = entry_for(ledger, id);
    if (!entry) return 0;
    if (entry->value) { ledger->failures |= M2OwnerInvalid; return 0; }
    if (!value) { *entry = (m2_owner_entry_t){0}; return 1; }
    entry->value = value;
    ++ledger->acquired;
    int unique = 1;
    for (size_t i = 0; i < M2OwnerCapacity; ++i) {
        const m2_owner_entry_t *other = &ledger->entries[i];
        if (other != entry && other->id && other->kind == entry->kind &&
            other->parent == entry->parent && other->value == value) unique = 0;
    }
    if (!unique) ledger->failures |= M2OwnerDuplicate;
    return unique;
}
uint64_t m2_owner_lookup(m2_owner_ledger_t *ledger, uint32_t kind, uintptr_t parent, uintptr_t value) {
    if (!ledger) return 0;
    uint64_t id = 0;
    if (value) for (size_t i = 0; i < M2OwnerCapacity; ++i) {
        const m2_owner_entry_t *entry = &ledger->entries[i];
        if (entry->id && entry->kind == kind && entry->parent == parent && entry->value == value) {
            if (id) { ledger->failures |= M2OwnerDuplicate; return 0; }
            id = entry->id;
        }
    }
    if (!id) ledger->failures |= M2OwnerInvalid;
    return id;
}
int m2_owner_retired(m2_owner_ledger_t *ledger, uint64_t id, int succeeded) {
    if (!ledger) return 0;
    m2_owner_entry_t *entry = entry_for(ledger, id);
    if (!entry) return 0;
    if (!entry->value) { ledger->failures |= M2OwnerInvalid; return 0; }
    if (!succeeded) { ledger->failures |= M2OwnerReleaseFailed; return 0; }
    *entry = (m2_owner_entry_t){0};
    ++ledger->retired;
    return 1;
}
int m2_owner_snapshot(const m2_owner_ledger_t *ledger, uint32_t version, m2_owner_snapshot_t *output, size_t bytes) {
    if (!ledger || !output || version != M2OwnerVersion || bytes != sizeof *output) return 0;
    m2_owner_snapshot_t snapshot = {.version=M2OwnerVersion, .bytes=sizeof snapshot,
        .next_id=ledger->next_id, .acquired=ledger->acquired, .retired=ledger->retired,
        .failures=ledger->failures};
    for (size_t i = 0; i < M2OwnerCapacity; ++i) {
        const m2_owner_entry_t *entry = &ledger->entries[i];
        if (!entry->id) {
            if (entry->value || entry->parent || entry->kind) snapshot.failures |= M2OwnerInconsistent;
            continue;
        }
        if (entry->id >= ledger->next_id ||
            (entry->kind != M2OwnerDc && entry->kind != M2OwnerWindow) ||
            (entry->kind == M2OwnerDc ? !entry->parent : entry->parent != 0))
            snapshot.failures |= M2OwnerInconsistent;
        if (entry->value) {
            ++snapshot.live;
            if (!snapshot.first_live_id) {
                snapshot.first_live_id=entry->id;
                snapshot.first_live_parent=entry->parent;
                snapshot.first_live_value=entry->value;
            }
        } else ++snapshot.pending;
    }
    if (!snapshot.next_id || snapshot.acquired >= snapshot.next_id || snapshot.retired > snapshot.acquired ||
        snapshot.acquired - snapshot.retired != snapshot.live)
        snapshot.failures |= M2OwnerInconsistent;
    *output = snapshot;
    return 1;
}
int m2_owner_clean(const m2_owner_snapshot_t *snapshot) {
    return snapshot && snapshot->version == M2OwnerVersion && snapshot->bytes == sizeof *snapshot &&
        snapshot->next_id > snapshot->acquired && !snapshot->live && !snapshot->pending && !snapshot->failures &&
        !snapshot->first_live_id && !snapshot->first_live_parent && !snapshot->first_live_value &&
        !snapshot->reserved && snapshot->acquired == snapshot->retired;
}
