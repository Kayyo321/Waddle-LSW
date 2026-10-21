/** @file tcp_gpu_m2_owned_windows.inc @brief Opt-in, exact-owner native fixture gate.
 * Sole fixture UI thread owns fixed ledgers and copied snapshots. Borrowed module,
 * exports and HWND parents never gain a reference or authorize an unrelated close.
 * The legacy invocation passes through the original Win32 calls unchanged.
 */
#include "m2_ownership_probe.h"
/** @brief Borrowed qualification query ABI; no output ownership transfer. */
typedef int (*m2_icd_query_t)(uint32_t, m2_icd_snapshot_t *, size_t);
/** @brief Borrowed real sink query ABI; no output ownership transfer. */
typedef int (*m2_dc_query_t)(uint32_t, m2_owner_snapshot_t *, size_t);
/** @brief Sole-thread fixture state; fixed storage, no dynamically owned memory. */
typedef struct m2_owned_fixture_t {
    int enabled, initialized; /**< Mode and successfully observed initial boundary. */
    unsigned lifetime; /**< Last accepted boundary, initially zero. */
    uint64_t session; /**< Exact authenticated session, retained by bootstrap. */
    char module_path[MAX_PATH]; /**< Exact absolute ASCII loaded path, copied. */
    m2_icd_query_t icd_query; /**< Borrowed export, valid until bootstrap/loader unload. */
    m2_dc_query_t dc_query; /**< Borrowed export from that same loaded module. */
    m2_owner_ledger_t windows, dcs; /**< Exact fixture acquisitions, independent of sink. */
    m2_icd_snapshot_t previous_icd; /**< Copied accepted monotonic identity marks. */
    m2_owner_snapshot_t previous_sink, previous_dcs, previous_windows; /**< Prior counters. */
} m2_owned_fixture_t;
static m2_owned_fixture_t m2_owned;

/** @brief Resolve an export only if its address belongs to the exact borrowed DLL.
 * @param[in] module Nonnull borrowed module. @param[in] name Nonnull export name.
 * @param[out] output Exclusive function pointer; written only on success.
 * @param[in] bytes Exact function-pointer size. @return One success, zero missing/
 * forwarded export or malformed size. Sole thread; no module reference acquired.
 */
static int m2_owned_export(HMODULE module, const char *name, void *output, size_t bytes)
{
    FARPROC procedure = GetProcAddress(module, name);
    const char *address = NULL; HMODULE owner = NULL;
    _Static_assert(sizeof address == sizeof procedure, "Win64 function address");
    if (!procedure || bytes != sizeof procedure) return 0;
    memcpy(&address, &procedure, sizeof address);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            address, &owner) || owner != module) return 0;
    memcpy(output, &procedure, bytes);
    return 1;
}
/** @brief Bind qualification to an already loaded exact absolute candidate path.
 * @param[in] path Nonnull absolute ASCII path shorter than MAX_PATH.
 * @param[in] session Nonzero authenticated identity. @return One ready, zero bad
 * path/module/export/session. Sole thread, called once before any fixture owners;
 * never loads/unloads a module, opens a file or resets a previously enabled gate.
 */
static int m2_owned_begin(const char *path, uint64_t session)
{
    char absolute[MAX_PATH] = {0}, loaded[MAX_PATH] = {0};
    if (m2_owned.enabled || !session || !path) return 0;
    const size_t length = strlen(path);
    if (!length || length >= MAX_PATH) return 0;
    for (size_t i = 0; i < length; ++i) if ((unsigned char)path[i] < 32 || (unsigned char)path[i] > 126) return 0;
    const DWORD count = GetFullPathNameA(path, MAX_PATH, absolute, NULL);
    if (!count || count >= MAX_PATH || _stricmp(path, absolute)) return 0;
    HMODULE module = GetModuleHandleA(absolute);
    if (!module) return 0;
    const DWORD copied = GetModuleFileNameA(module, loaded, MAX_PATH);
    if (!copied || copied >= MAX_PATH || _stricmp(absolute, loaded)) return 0;
    m2_icd_query_t icd_query = NULL; m2_dc_query_t dc_query = NULL;
    if (!m2_owned_export(module, "venus_icd_m2_snapshot", &icd_query, sizeof icd_query) ||
        !m2_owned_export(module, "venus_m2_dc_snapshot", &dc_query, sizeof dc_query)) return 0;
    m2_owned = (m2_owned_fixture_t){.enabled=1, .session=session, .icd_query=icd_query,
        .dc_query=dc_query, .windows={.next_id=1}, .dcs={.next_id=1}};
    memcpy(m2_owned.module_path, loaded, copied + 1);
    return 1;
}
/** @brief Reserve before CreateWindowExA, recording even a duplicate native result.
 * @param[in] All arguments Borrowed original Win32 arguments; no strings retained.
 * @return Owned HWND or NULL. Sole thread. On failed observation only the newly
 * acquired exact owner is destroyed; failed actual destruction remains recorded.
 */
static HWND m2_owned_create_window(DWORD extended, const char *class_name, const char *title,
    DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void *parameter)
{
    if (!m2_owned.enabled) return CreateWindowExA(extended,class_name,title,style,x,y,width,height,parent,menu,instance,parameter);
    const uint64_t id = m2_owner_reserve(&m2_owned.windows, M2OwnerWindow, 0);
    if (!id) return NULL;
    HWND window = CreateWindowExA(extended,class_name,title,style,x,y,width,height,parent,menu,instance,parameter);
    if (!m2_owner_acquired(&m2_owned.windows, id, (uintptr_t)window)) {
        if (window) (void)m2_owner_retired(&m2_owned.windows, id, DestroyWindow(window));
        return NULL;
    }
    return window;
}
/** @brief Destroy only a fixture-acquired HWND, using the actual native result.
 * @param[in] window Owned HWND, never a borrowed sink parent. @return Native
 * success/zero failure; a failed owner remains recorded. Sole fixture thread.
 */
static BOOL m2_owned_destroy_window(HWND window)
{
    if (!m2_owned.enabled) return DestroyWindow(window);
    const uint64_t id = m2_owner_lookup(&m2_owned.windows, M2OwnerWindow, 0, (uintptr_t)window);
    if (!id) return 0;
    return m2_owner_retired(&m2_owned.windows, id, DestroyWindow(window));
}
/** @brief Reserve before fixture GetDC; every successful acquisition has an ID.
 * @param[in] window Borrowed original HWND. @return Owned HDC/NULL. Sole thread;
 * duplicate observation attempts actual cleanup of only its own new acquisition.
 */
static HDC m2_owned_get_dc(HWND window)
{
    if (!m2_owned.enabled) return GetDC(window);
    const uint64_t id = m2_owner_reserve(&m2_owned.dcs, M2OwnerDc, (uintptr_t)window);
    if (!id) return NULL;
    HDC dc = GetDC(window);
    if (!m2_owner_acquired(&m2_owned.dcs, id, (uintptr_t)dc)) {
        if (dc) (void)m2_owner_retired(&m2_owned.dcs, id, ReleaseDC(window, dc));
        return NULL;
    }
    return dc;
}
/** @brief Retire only the matching fixture GetDC acquisition on actual success.
 * @param[in] window Borrowed parent. @param[in] dc Owned matching HDC.
 * @return One retired/zero failure. Sole thread; unknown/ambiguous owners cannot
 * reach ReleaseDC, and failures remain sticky even if later cleanup succeeds.
 */
static int m2_owned_release_dc(HWND window, HDC dc)
{
    if (!m2_owned.enabled) return ReleaseDC(window, dc);
    const uint64_t id = m2_owner_lookup(&m2_owned.dcs, M2OwnerDc, (uintptr_t)window, (uintptr_t)dc);
    if (!id) return 0;
    return m2_owner_retired(&m2_owned.dcs, id, ReleaseDC(window, dc));
}
/** @brief Require a complete healthy empty ICD snapshot, independent of totals.
 * @param[in] value Nonnull borrowed snapshot. @return One clean/zero rejected.
 * Pure, thread-safe; does not clear faults, free objects or invoke a query.
 */
static int m2_owned_icd_clean(const m2_icd_snapshot_t *value)
{
    if (value->version != M2IcdVersion || value->bytes != sizeof *value || value->flags ||
        value->command_state || value->lost || value->binding != 1 ||
        !(value->object_next_id >> 32) || !(uint32_t)value->object_next_id ||
        !(uint32_t)value->wsi_next_id || (value->object_next_id >> 32) != (value->wsi_next_id >> 32)) return 0;
    const uint64_t owners[] = {value->first_live_id, value->objects, value->resources,
        value->surfaces, value->swapchains, value->images, value->acquired_images,
        value->backings, value->mappings, value->caches, value->tickets, value->references, value->reservations};
    for (size_t i = 0; i < sizeof owners / sizeof *owners; ++i) if (owners[i]) return 0;
    return 1;
}
/** @brief Require actual new successful acquisitions and monotonic reservation IDs.
 * @param[in] current,previous Nonnull clean copied snapshots. @return One advanced,
 * zero stale/rewound/inconsistent. Pure/thread-safe; no owner/state mutation.
 */
static int m2_owned_advanced(const m2_owner_snapshot_t *current, const m2_owner_snapshot_t *previous)
{
    return current->acquired > previous->acquired && current->next_id > previous->next_id &&
        current->next_id - previous->next_id >= current->acquired - previous->acquired;
}
/** @brief Emit one ABI snapshot as an ordered JSON array.
 * @param[in] value Nonnull borrowed value. Sole stdout writer; owns no resources.
 */
static void m2_owned_print_owner(const m2_owner_snapshot_t *value)
{
    printf("[%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u]", value->version, value->bytes,
        (unsigned long long)value->next_id, (unsigned long long)value->acquired,
        (unsigned long long)value->retired, (unsigned long long)value->first_live_id,
        (unsigned long long)value->first_live_parent, (unsigned long long)value->first_live_value,
        value->live, value->pending, value->failures, value->reserved);
}
/** @brief Observe and validate one quiescent initial/per-lifetime boundary.
 * @param[in] session Exact current bootstrap session. @param[in] workload Fixed
 * supported literal. @param[in] lifetime Initial zero then 1..8, once each.
 * @param[in] handles Nonzero diagnostic process count, never an ownership oracle.
 * @return One complete healthy advancing evidence, zero failure. Sole thread;
 * valid query output is printed even if unhealthy. Query failure emits no partial
 * record. Previous accepted evidence updates only on complete success. No cleanup.
 */
static int m2_owned_boundary(uint64_t session, const char *workload, unsigned lifetime, DWORD handles)
{
    m2_icd_snapshot_t icd = {0}; m2_owner_snapshot_t sink = {0}, dcs = {0}, windows = {0};
    if (!m2_owned.enabled || session != m2_owned.session || !handles ||
        (strcmp(workload,"triangle_present") && strcmp(workload,"wsi_lifetime")) ||
        lifetime > 8 || (m2_owned.initialized ? lifetime != m2_owned.lifetime + 1 : lifetime != 0)) return 0;
    if (m2_owned.icd_query(M2IcdVersion,&icd,sizeof icd) != 1 ||
        m2_owned.dc_query(M2OwnerVersion,&sink,sizeof sink) != 1 ||
        !m2_owner_snapshot(&m2_owned.dcs,M2OwnerVersion,&dcs,sizeof dcs) ||
        !m2_owner_snapshot(&m2_owned.windows,M2OwnerVersion,&windows,sizeof windows)) return 0;
    printf("M2_OWNED_V1 {\"session\":\"%016llx\",\"workload\":\"%s\",\"lifetime\":%u,\"module_hex\":\"",
        (unsigned long long)session,workload,lifetime);
    for (size_t i = 0; m2_owned.module_path[i]; ++i) printf("%02x",(unsigned char)m2_owned.module_path[i]);
    printf("\",\"handles\":%lu,\"icd\":[%u,%u,%u,%u,%ld,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu],\"sink_dc\":",
        (unsigned long)handles,icd.version,icd.bytes,icd.flags,icd.command_state,(long)icd.lost,icd.binding,
        (unsigned long long)icd.object_next_id,(unsigned long long)icd.wsi_next_id,(unsigned long long)icd.first_live_id,
        (unsigned long long)icd.objects,(unsigned long long)icd.resources,(unsigned long long)icd.surfaces,
        (unsigned long long)icd.swapchains,(unsigned long long)icd.images,(unsigned long long)icd.acquired_images,
        (unsigned long long)icd.backings,(unsigned long long)icd.mappings,(unsigned long long)icd.caches,
        (unsigned long long)icd.tickets,(unsigned long long)icd.references,(unsigned long long)icd.reservations);
    m2_owned_print_owner(&sink); fputs(",\"fixture_dc\":",stdout); m2_owned_print_owner(&dcs);
    fputs(",\"fixture_window\":",stdout); m2_owned_print_owner(&windows); puts("}"); fflush(stdout);
    if (!m2_owned_icd_clean(&icd) || !m2_owner_clean(&sink) || !m2_owner_clean(&dcs) || !m2_owner_clean(&windows)) return 0;
    if (m2_owned.initialized && ((icd.object_next_id >> 32) != (m2_owned.previous_icd.object_next_id >> 32) ||
        icd.object_next_id <= m2_owned.previous_icd.object_next_id || icd.wsi_next_id <= m2_owned.previous_icd.wsi_next_id ||
        !m2_owned_advanced(&sink,&m2_owned.previous_sink) || !m2_owned_advanced(&dcs,&m2_owned.previous_dcs) ||
        !m2_owned_advanced(&windows,&m2_owned.previous_windows))) return 0;
    m2_owned.previous_icd=icd; m2_owned.previous_sink=sink; m2_owned.previous_dcs=dcs; m2_owned.previous_windows=windows;
    m2_owned.initialized=1; m2_owned.lifetime=lifetime;
    return 1;
}

/** @brief Print a raw failure-time owner query, never an acceptance boundary.
 * @param[in] stream Borrowed nonnull stderr/test stream. @param[in] name Static
 * owner group. @param[in] ok Query status. @param[in] value Borrowed copied data;
 * not interpreted when the query failed. Sole fixture thread, no owner mutation.
 */
static inline void m2_owned_gui_print_snapshot(FILE *stream, const char *name, int ok,
                                              const m2_owner_snapshot_t *value)
{
    fprintf(stream,"gui_diag: owner_group=%s query=%d",name,ok);
    if (ok) fprintf(stream," version=%u bytes=%u next_id=%llu acquired=%llu retired=%llu first_live_id=%llu parent=%llx value=%llx live=%u pending=%u failures=%u reserved=%u",
        value->version,value->bytes,(unsigned long long)value->next_id,
        (unsigned long long)value->acquired,(unsigned long long)value->retired,
        (unsigned long long)value->first_live_id,(unsigned long long)value->first_live_parent,
        (unsigned long long)value->first_live_value,value->live,value->pending,value->failures,value->reserved);
    else fputs(" unavailable=1",stream);
    fputc('\n',stream);
}
/** @brief Read exactly three native-owner ledgers only after an existing GUI failure.
 * @param[in] stream Borrowed output; NULL is ignored. No acceptance evaluation,
 * previous-boundary update, reset, cleanup or extra native owner acquisition.
 * Disabled legacy mode reports that these ledgers are unavailable. Sole thread.
 */
static inline void m2_owned_gui_failure(FILE *stream)
{
    if (!stream) return;
    if (!m2_owned.enabled) { fputs("gui_diag: ownership_mode=0 ledgers_unavailable=1\n",stream); return; }
    m2_owner_snapshot_t sink={0},dcs={0},windows={0};
    const int sink_ok=m2_owned.dc_query(M2OwnerVersion,&sink,sizeof sink);
    const int dc_ok=m2_owner_snapshot(&m2_owned.dcs,M2OwnerVersion,&dcs,sizeof dcs);
    const int window_ok=m2_owner_snapshot(&m2_owned.windows,M2OwnerVersion,&windows,sizeof windows);
    fprintf(stream,"gui_diag: session=%016llx last_accepted_lifetime=%u failure_observation_only=1\n",
        (unsigned long long)m2_owned.session,m2_owned.lifetime);
    m2_owned_gui_print_snapshot(stream,"sink_dc",sink_ok,&sink);
    m2_owned_gui_print_snapshot(stream,"fixture_dc",dc_ok,&dcs);
    m2_owned_gui_print_snapshot(stream,"fixture_window",window_ok,&windows);
    fflush(stream);
}
