/** @file m2_ownership_probe.c @brief Fixed exact identities around actual Win32 APIs. */
#include "m2_ownership_probe.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static m2_owner_ledger_t dc_owners = {.next_id=1};
#ifdef WaddleM2OwnershipFaults
static uint32_t release_fault;
#endif
uintptr_t venus_m2_dc_acquire(uintptr_t hwnd) {
    const uint64_t id = m2_owner_reserve(&dc_owners, M2OwnerDc, hwnd);
    if (!id) return 0;
    const uintptr_t dc = (uintptr_t)GetDC((HWND)hwnd);
    if (!m2_owner_acquired(&dc_owners, id, dc)) {
        /* A duplicate result still owns its reserved slot. Retire only this
         * successful acquisition; a failed actual release remains recorded. */
        if (dc) (void)m2_owner_retired(&dc_owners, id, ReleaseDC((HWND)hwnd, (HDC)dc));
        return 0;
    }
    return dc;
}
int venus_m2_dc_release(uintptr_t hwnd, uintptr_t dc) {
    const uint64_t id = m2_owner_lookup(&dc_owners, M2OwnerDc, hwnd, dc);
    if (!id) return 0;
#ifdef WaddleM2OwnershipFaults
    const uint32_t fault = release_fault;
    release_fault = M2DcNormal;
    if (fault == M2DcOmitRelease) return 1; /* Deliberate live owner, not a real retirement. */
    if (fault == M2DcFailRelease) return m2_owner_retired(&dc_owners, id, 0);
#endif
    return m2_owner_retired(&dc_owners, id, ReleaseDC((HWND)hwnd, (HDC)dc));
}
int venus_m2_dc_snapshot(uint32_t version, m2_owner_snapshot_t *output, size_t bytes) {
    return m2_owner_snapshot(&dc_owners, version, output, bytes);
}
#ifdef WaddleM2OwnershipFaults
int venus_m2_dc_test_fault(uint32_t fault) {
    if (fault > M2DcFailRelease || release_fault != M2DcNormal) return 0;
    release_fault = fault;
    return 1;
}
int venus_m2_dc_test_cleanup(void) {
    int clean = 1;
    for (size_t i = 0; i < M2OwnerCapacity; ++i) {
        const m2_owner_entry_t entry = dc_owners.entries[i];
        if (!entry.id) continue;
        if (!entry.value) { clean = 0; continue; }
        if (!m2_owner_retired(&dc_owners, entry.id, ReleaseDC((HWND)entry.parent, (HDC)entry.value))) clean = 0;
    }
    return clean;
}
int venus_m2_dc_test_reset(void) {
    for (size_t i = 0; i < M2OwnerCapacity; ++i) if (dc_owners.entries[i].id) return 0;
    dc_owners = (m2_owner_ledger_t){.next_id=1};
    release_fault = M2DcNormal;
    return 1;
}
#endif
