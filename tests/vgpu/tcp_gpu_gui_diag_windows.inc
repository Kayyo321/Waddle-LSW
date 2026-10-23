/** @file tcp_gpu_gui_diag_windows.inc @brief Bounded passive HWND diagnostics.
 * Windows are borrowed observations, never owners. This is a window inventory,
 * not a complete USER object table or creator attribution. HWND reuse/races can
 * invalidate observations; faults/truncation are explicit and never change a gate.
 * All calls run on the existing fixture thread; no messages, waits or UI changes.
 */
enum { GuiDiagCapacity=128, GuiDiagVisitLimit=4096, GuiDiagDepthLimit=32, GuiDiagClassBytes=128 };
/** @brief Incomplete observation flags, independent of acceptance. */
enum { GuiDiagCapacityFault=1, GuiDiagVisitFault=2, GuiDiagDepthFault=4,
       GuiDiagDuplicate=8, GuiDiagQueryFault=16, GuiDiagClassTruncated=32,
       GuiDiagEnumerationFault=64, GuiDiagIdentityChanged=128 };
/** @brief Copied identity/attributes; all HWND values remain borrowed. */
typedef struct gui_diag_window_t {
    uintptr_t hwnd, parent, owner;
    DWORD pid, tid, style, extended_style;
    int visible;
    char class_name[GuiDiagClassBytes];
} gui_diag_window_t;
/** @brief Fixed sole-thread snapshot. No allocation, references or cleanup. */
typedef struct gui_diag_snapshot_t {
    DWORD pid, flags, last_error;
    unsigned count, visited;
    gui_diag_window_t windows[GuiDiagCapacity];
} gui_diag_snapshot_t;

/** @brief Record a native query failure without clearing any existing evidence. */
static void gui_diag_fault(gui_diag_snapshot_t *snapshot, DWORD flag, DWORD error)
{
    snapshot->flags |= flag;
    if (error) snapshot->last_error = error;
}
/** @brief Find a previously observed HWND; NULL means absent, never invalidates it. */
static const gui_diag_window_t *gui_diag_find(const gui_diag_snapshot_t *snapshot, uintptr_t hwnd)
{
    for (unsigned i=0; i<snapshot->count; ++i)
        if (snapshot->windows[i].hwnd==hwnd) return &snapshot->windows[i];
    return NULL;
}
/** @brief Observe one window plus descendants; zero stops a bounded enumeration.
 * @param[in,out] snapshot Exclusive fixed storage. @param[in] window Borrowed.
 * @param[in] depth Current bounded traversal depth. No owner/state mutation.
 */
static int gui_diag_visit(gui_diag_snapshot_t *snapshot, HWND window, unsigned depth);
/** @brief Walk immediate children/message roots with explicit API/cycle bounds. */
static int gui_diag_children(gui_diag_snapshot_t *snapshot, HWND parent, unsigned depth)
{
    HWND after=NULL;
    for (;;) {
        SetLastError(0);
        HWND child=FindWindowExA(parent,after,NULL,NULL);
        DWORD error=GetLastError();
        if (!child) {
            if (error) gui_diag_fault(snapshot,GuiDiagEnumerationFault,error);
            return error==0;
        }
        if (child==after || child==parent) {
            gui_diag_fault(snapshot,GuiDiagDuplicate,0); return 0;
        }
        if (!gui_diag_visit(snapshot,child,depth)) return 0;
        after=child;
    }
}
static int gui_diag_visit(gui_diag_snapshot_t *snapshot, HWND window, unsigned depth)
{
    if (depth>GuiDiagDepthLimit) { gui_diag_fault(snapshot,GuiDiagDepthFault,0); return 0; }
    if (snapshot->visited==GuiDiagVisitLimit) { gui_diag_fault(snapshot,GuiDiagVisitFault,0); return 0; }
    ++snapshot->visited;
    DWORD pid=0;
    SetLastError(0);
    DWORD tid=GetWindowThreadProcessId(window,&pid);
    if (!tid || !pid) { gui_diag_fault(snapshot,GuiDiagQueryFault,GetLastError()); return 1; }
    if (pid==snapshot->pid) {
        if (gui_diag_find(snapshot,(uintptr_t)window)) { gui_diag_fault(snapshot,GuiDiagDuplicate,0); return 0; }
        if (snapshot->count==GuiDiagCapacity) { gui_diag_fault(snapshot,GuiDiagCapacityFault,0); return 0; }
        gui_diag_window_t *entry=&snapshot->windows[snapshot->count++];
        entry->hwnd=(uintptr_t)window; entry->pid=pid; entry->tid=tid;
        SetLastError(0);
        int length=GetClassNameA(window,entry->class_name,GuiDiagClassBytes);
        if (length<=0) gui_diag_fault(snapshot,GuiDiagQueryFault,GetLastError());
        else if (length>=GuiDiagClassBytes-1) gui_diag_fault(snapshot,GuiDiagClassTruncated,0);
        entry->class_name[GuiDiagClassBytes-1]=0;
        SetLastError(0);
        entry->style=(DWORD)GetWindowLongPtrA(window,GWL_STYLE);
        DWORD error=GetLastError(); if (error) gui_diag_fault(snapshot,GuiDiagQueryFault,error);
        SetLastError(0);
        entry->extended_style=(DWORD)GetWindowLongPtrA(window,GWL_EXSTYLE);
        error=GetLastError(); if (error) gui_diag_fault(snapshot,GuiDiagQueryFault,error);
        /* Zero is a valid parent/owner/visibility result, never a guessed error. */
        entry->parent=(uintptr_t)GetAncestor(window,GA_PARENT);
        entry->owner=(uintptr_t)GetWindow(window,GW_OWNER);
        entry->visible=IsWindowVisible(window)!=0;
        DWORD final_pid=0;
        if (GetWindowThreadProcessId(window,&final_pid)!=tid || final_pid!=pid)
            gui_diag_fault(snapshot,GuiDiagIdentityChanged,0);
    }
    return gui_diag_children(snapshot,window,depth+1);
}
/** @brief EnumWindows callback; visits foreign parents to find owned children. */
static BOOL CALLBACK gui_diag_top(HWND window, LPARAM argument)
{
    return gui_diag_visit((gui_diag_snapshot_t *)(uintptr_t)argument,window,0);
}
/** @brief Copy a bounded window inventory after the unchanged baseline sample.
 * @param[out] snapshot Nonnull exclusive storage, overwritten; no dynamic owner.
 * Errors remain diagnostic flags. No acceptance result, retries, messages or waits.
 */
static void gui_diag_capture(gui_diag_snapshot_t *snapshot)
{
    const DWORD saved_error=GetLastError();
    *snapshot=(gui_diag_snapshot_t){.pid=GetCurrentProcessId()};
    SetLastError(0);
    if (!EnumWindows(gui_diag_top,(LPARAM)(uintptr_t)snapshot))
        gui_diag_fault(snapshot,GuiDiagEnumerationFault,GetLastError());
    /* Keep the second inventory bounded even if the first was incomplete. */
    (void)gui_diag_children(snapshot,HWND_MESSAGE,0);
    SetLastError(saved_error);
}
/** @brief Emit one copied entry; class bytes use hex to prevent log injection. */
static void gui_diag_print_window(const char *phase, const gui_diag_window_t *entry)
{
    fprintf(stderr,"gui_diag: phase=%s hwnd=%llx pid=%lu tid=%lu parent=%llx owner=%llx style=%08lx exstyle=%08lx visible=%d class_hex=",
        phase,(unsigned long long)entry->hwnd,(unsigned long)entry->pid,(unsigned long)entry->tid,
        (unsigned long long)entry->parent,(unsigned long long)entry->owner,
        (unsigned long)entry->style,(unsigned long)entry->extended_style,entry->visible);
    for (size_t i=0; entry->class_name[i]; ++i) fprintf(stderr,"%02x",(unsigned char)entry->class_name[i]);
    fputc('\n',stderr);
}
/** @brief Print failure-only before/after identity evidence; never accepts a run.
 * @param[in] before Borrowed captured inventory. @param[in] gdi/user values The
 * original four gate samples, never replaced or resampled. Sole fixture thread.
 */
static void gui_diag_report(const gui_diag_snapshot_t *before, DWORD gdi_before, DWORD gdi_after,
                            DWORD user_before, DWORD user_after)
{
    gui_diag_snapshot_t after;
    gui_diag_capture(&after);
    fprintf(stderr,"gui_diag: failed_samples gdi=%lu->%lu user=%lu->%lu window_inventory_only=1\n",
        (unsigned long)gdi_before,(unsigned long)gdi_after,(unsigned long)user_before,(unsigned long)user_after);
    const gui_diag_snapshot_t *snapshots[]={before,&after};
    const char *phases[]={"before","after"};
    for (unsigned phase=0; phase<2; ++phase) {
        const gui_diag_snapshot_t *snapshot=snapshots[phase];
        fprintf(stderr,"gui_diag: phase=%s count=%u visited=%u flags=%lu last_error=%lu\n",phases[phase],
            snapshot->count,snapshot->visited,(unsigned long)snapshot->flags,(unsigned long)snapshot->last_error);
        for (unsigned i=0; i<snapshot->count; ++i) gui_diag_print_window(phases[phase],&snapshot->windows[i]);
        for (unsigned i=0; i<snapshot->count; ++i) {
            const gui_diag_window_t *entry=&snapshot->windows[i];
            const gui_diag_window_t *other=gui_diag_find(snapshots[1-phase],entry->hwnd);
            if (!other || other->pid!=entry->pid || other->tid!=entry->tid || strcmp(other->class_name,entry->class_name))
                gui_diag_print_window(phase ? "added" : "removed",entry);
        }
    }
    fflush(stderr);
}
