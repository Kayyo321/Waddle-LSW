#ifdef _WIN32
#include "opencl_slot_certificate_windows.inc"
#endif
/** @file opencl_reload_windows.inc
 * @brief Qualification-only exact full-DLL reload ownership checks.
 * Included by the Windows fixture and by API-double controls. No DLL-owned
 * pointer is freed here. Main serializes pools, probes and the cycle callback.
 * fail_closed must terminate without unloading beneath an unretired owner.
 */
/** Fixed capacities/budgets; no run-time relaxation or reduced passing run. */
enum { ReloadCycles=1100, ReloadIdleThreads=4, ReloadForeignThreads=2,
       ReloadPoolCapacity=4, ReloadTlsCapacity=2048, ReloadFlsCapacity=4097,
       ReloadReadyMs=10000, ReloadCycleMs=210000, ReloadPathCapacity=32768 };
/** Borrowed callback/argument remain live until the owning thread is joined. */
typedef int (*reload_call_t)(void *);
/** One thread's parent-owned storage; publication is through ready/go events. */
typedef struct reload_thread_t {
    HANDLE thread,ready,go;
    DWORD tid;
    reload_call_t call;
    void *argument;
} reload_thread_t;
/** Fixed pool owns every nonnull handle; only its creating caller retires it. */
typedef struct reload_pool_t { reload_thread_t slots[ReloadPoolCapacity]; } reload_pool_t;
/** Measured process resources; probe indices are fully freed before return. */
typedef struct reload_counts_t { DWORD handles; unsigned tls,fls; } reload_counts_t;
/** Output copied only after real callers, shutdown and module refs retire. */
typedef struct reload_cycle_result_t {
    uint64_t session;
    DWORD foreign[ReloadForeignThreads];
    unsigned module_exact,modules_absent;
} reload_cycle_result_t;
/** Immutable orchestration callbacks; state is borrowed through the full run. */
typedef struct reload_api_t {
    int (*cycle)(void *,reload_cycle_result_t *);
    int (*absent)(void *);
    int (*stage)(void *,const char *);
    void *context;
} reload_api_t;

/** @brief Close an exact owned handle; failed retirement cannot authorize unload.
 * @param[in,out] owner Nonnull slot, optionally null handle. Serial owner only.
 */
static void reload_close(HANDLE *owner) {
    if(*owner) {
        if(!CloseHandle(*owner))fail_closed("OpenCL reload fatal: owned handle close failed");
        *owner=NULL;
    }
}
/** @brief Raw foreign/idle thread; parent storage/events outlive its join.
 * @return Callback status, or1 for readiness/wait error. Does not close owners.
 */
static DWORD WINAPI reload_thread_main(void *opaque) {
    reload_thread_t *slot=opaque;
    slot->tid=GetCurrentThreadId();
    if(!slot->tid || !SetEvent(slot->ready) || WaitForSingleObject(slot->go,INFINITE)!=WAIT_OBJECT_0)return 1;
    return slot->call ? (DWORD)slot->call(slot->argument) : 0;
}
/** @brief Release parked threads, join all created owners, then close all events.
 * @param[in,out] pool Nonnull, parent-owned, no concurrent mutation.
 * @return1 if a joined callback failed, otherwise0. Live threads fail closed.
 * Null callbacks cancel parked work. Published callbacks are never overwritten.
 */
static int reload_pool_finish(reload_pool_t *pool) {
    int failed=0;
    for(unsigned i=0;i<ReloadPoolCapacity;++i)
        if(pool->slots[i].thread && !SetEvent(pool->slots[i].go))
            fail_closed("OpenCL reload fatal: owned thread release failed");
    ULONGLONG start=GetTickCount64();
    for(unsigned i=0;i<ReloadPoolCapacity;++i) {
        reload_thread_t *slot=&pool->slots[i];
        if(slot->thread) {
            ULONGLONG elapsed=GetTickCount64()-start;
            if(elapsed>=ReloadCycleMs || WaitForSingleObject(slot->thread,(DWORD)(ReloadCycleMs-elapsed))!=WAIT_OBJECT_0)
                fail_closed("OpenCL reload fatal: caller did not retire");
            DWORD code=1;
            if(!GetExitCodeThread(slot->thread,&code))failed=1;
            if(code)failed=1;
            reload_close(&slot->thread);
        }
        slot->call=NULL;slot->argument=NULL;
        reload_close(&slot->ready);reload_close(&slot->go);
    }
    return failed;
}
/** @brief Create and prove readiness of count raw threads before any DLL load.
 * @param[out] pool Zero-initialized fixed storage owned by this caller.
 * @param[in] count1..4. Events/threads are retired on ordinary setup failure.
 * @return0 on readiness;1 after complete cancellation/cleanup on setup error.
 */
static int reload_pool_start(reload_pool_t *pool,unsigned count) {
    if(!count || count>ReloadPoolCapacity)return 1;
    for(unsigned i=0;i<count;++i) {
        reload_thread_t *slot=&pool->slots[i];
        slot->ready=CreateEventW(NULL,TRUE,FALSE,NULL);
        slot->go=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(!slot->ready || !slot->go)goto failed;
        slot->thread=CreateThread(NULL,0,reload_thread_main,slot,0,NULL);
        if(!slot->thread)goto failed;
    }
    for(unsigned i=0;i<count;++i) {
        reload_thread_t *slot=&pool->slots[i];
        if(WaitForSingleObject(slot->ready,ReloadReadyMs)!=WAIT_OBJECT_0 || !slot->tid)goto failed;
        for(unsigned j=0;j<i;++j)if(pool->slots[j].tid==slot->tid)goto failed;
    }
    return 0;
failed:
    (void)reload_pool_finish(pool);return 1;
}
/** @brief Publish the two borrowed calls before releasing either ready caller.
 * @param[in,out] pool Ready two-thread foreign pool, sole owner.
 * @param[in] call Nonnull callback; argument borrowed until both joins.
 * @return Combined callback result after complete thread/event retirement.
 */
static int reload_pool_call(reload_pool_t *pool,reload_call_t call,void *argument) {
    for(unsigned i=0;i<ReloadForeignThreads;++i) {
        pool->slots[i].call=call;pool->slots[i].argument=argument;
    }
    return reload_pool_finish(pool);
}
/** @brief Exhaust free TLS or FLS indices, then free every temporary index.
 * @param[in] fls0 selects TLS,1 selects FLS. Sole quiescent caller.
 * @param[out] count Nonnull count; invalid unless return0.
 * @return1 on missing bounded exhaustion or unexpected allocation error.
 * Only ERROR_NO_MORE_ITEMS qualifies this target: Win32 does not guarantee
 * that every allocation failure means exhaustion. Native calibration is required.
 * A failed free is fatal after all possible owned indices have been freed.
 */
static int reload_indices(int fls,unsigned *count) {
#ifdef _WIN32
    if(!fls) {
        slot_certificate_t certificate;
        slot_status_t status=slot_certificate_probe(&certificate);
        *count=certificate.count;
        if(status!=SlotOk || certificate.fatal)
            fprintf(stderr,"OpenCL reload probe allocation failed kind=tls certificate_status=%u fatal=%u acquired=%u freed=%u errors=%lu,%lu\n",
                (unsigned)status,certificate.fatal,certificate.acquired,certificate.freed,
                (unsigned long)certificate.first_error,(unsigned long)certificate.refill_error);
        if(certificate.fatal)fail_closed("OpenCL reload fatal: TLS certificate ownership/retirement failed");
        return status!=SlotOk;
    }
#endif
    DWORD indices[ReloadFlsCapacity];unsigned used=0;
    const unsigned capacity=fls ? ReloadFlsCapacity : ReloadTlsCapacity;
    int exhausted=0,failed=0,unexpected=0;DWORD allocation_error=ERROR_SUCCESS;
    while(used<capacity) {
        SetLastError(ERROR_SUCCESS);
        DWORD index=fls ? FlsAlloc(NULL) : TlsAlloc();
        DWORD error=GetLastError();
        if(index==(fls ? FLS_OUT_OF_INDEXES : TLS_OUT_OF_INDEXES)) {
            allocation_error=error;exhausted=error==ERROR_NO_MORE_ITEMS;
            unexpected=!exhausted;break;
        }
        indices[used++]=index;
    }
    *count=used;
    while(used) {
        DWORD index=indices[--used];
        if(!(fls ? FlsFree(index) : TlsFree(index)))failed=1;
    }
    if(failed)fail_closed("OpenCL reload fatal: probe index retirement failed");
    if(unexpected)fprintf(stderr,"OpenCL reload probe allocation failed kind=%s error=%lu acquired=%u\n",
        fls ? "fls" : "tls",(unsigned long)allocation_error,*count);
    return !exhausted;
}
/** @brief Measure exact quiescent resources without retaining temporary owners.
 * @param[out] counts Nonnull copied snapshot. Serial main-thread call.
 * @return0 only if both bounded index probes and native count query succeed.
 */
static int reload_measure(reload_counts_t *counts) {
    return reload_indices(0,&counts->tls) || reload_indices(1,&counts->fls) ||
        !GetProcessHandleCount(GetCurrentProcess(),&counts->handles);
}
/** @brief Pure equality of all measured fields; no tolerance or masking. */
static int reload_equal(const reload_counts_t *left,const reload_counts_t *right) {
    return left->handles==right->handles && left->tls==right->tls && left->fls==right->fls;
}
/** @brief Verify a borrowed module reference resolves to the exact requested path.
 * @param[in] module Borrowed loaded module; expected nonnull absolute UTF16 path.
 * @return1 on exact case-insensitive Windows path equality,0 on query/truncation.
 */
static int reload_module_exact(HMODULE module,const wchar_t *expected) {
    wchar_t actual[ReloadPathCapacity];
    DWORD count=GetModuleFileNameW(module,actual,ReloadPathCapacity);
    return count && count<ReloadPathCapacity && !_wcsicmp(actual,expected);
}
/** @brief Print a complete UTF16-code-unit path token; caller validated length.
 * @param[in] output Borrowed writable stream; path borrowed nonnull string.
 */
static void reload_path(FILE *output,const wchar_t *path) {
    for(;*path;++path)fprintf(output,"%04x",(unsigned)*path);
}
/** @brief Execute1100 real module graphs in one process with exact retirement.
 * @param[in] api Nonnull immutable callbacks and borrowed state.
 * @param[in] module_path Exact absolute candidate path; max32767 UTF16 units.
 * @param[in] output Borrowed stream; every record is provisional until terminal.
 * @return0 only after1100 cycles and final owned cleanup/entry-baseline recovery.
 * Heap acceptance is deliberately separate; no first-load owner waiver exists.
 */
static int reload_run(const reload_api_t *api,const wchar_t *module_path,FILE *output) {
    reload_counts_t entry={0},baseline={0},observed={0};
    reload_pool_t idle={0};HMODULE crt=NULL;int failed=1;
    FILETIME created,exited,kernel,user;
    size_t path_length=0;while(path_length<ReloadPathCapacity && module_path[path_length])++path_length;
    if(path_length<3 || path_length>=ReloadPathCapacity || module_path[1]!=L':' || module_path[2]!=L'\\' ||
       !api->absent(api->context) || !GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user) || reload_measure(&entry))goto cleanup;
    uint64_t creation=((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime;
    DWORD pid=GetCurrentProcessId();
    crt=LoadLibraryW(L"msvcrt.dll");
    if(!crt || reload_pool_start(&idle,ReloadIdleThreads) || reload_measure(&baseline))goto cleanup;
    fprintf(output,"OPENCL_RELOAD_V1 {\"event\":\"baseline\",\"pid\":%lu,\"creation\":\"%016llx\",\"cycles\":%u,\"contexts_per_cycle\":3,\"idle_threads\":4,\"handles\":%lu,\"tls\":%u,\"fls\":%u,\"heap_acceptance\":false,\"entry\":[%lu,%u,%u],\"module_utf16\":\"",
        (unsigned long)pid,(unsigned long long)creation,ReloadCycles,(unsigned long)baseline.handles,baseline.tls,baseline.fls,(unsigned long)entry.handles,entry.tls,entry.fls);
    reload_path(output,module_path);fputs("\"}\n",output);fflush(output);
    for(unsigned cycle=1;cycle<=ReloadCycles;++cycle) {
        char stage[40];reload_cycle_result_t result={0};
        if(!api->absent(api->context) || reload_measure(&observed) || !reload_equal(&baseline,&observed))goto cleanup;
        snprintf(stage,sizeof stage,"reload_before_%u",cycle);
        if(api->stage(api->context,stage) || api->cycle(api->context,&result) ||
           !result.session || !result.foreign[0] || !result.foreign[1] || result.foreign[0]==result.foreign[1] ||
           result.module_exact!=1 || result.modules_absent!=1 || !api->absent(api->context) ||
           reload_measure(&observed) || !reload_equal(&baseline,&observed))goto cleanup;
        fprintf(output,"OPENCL_RELOAD_V1 {\"event\":\"cycle\",\"cycle\":%u,\"pid\":%lu,\"creation\":\"%016llx\",\"session\":\"%016llx\",\"foreign\":[%lu,%lu],\"contexts\":3,\"module_exact\":1,\"modules_absent\":1,\"handles\":%lu,\"tls\":%u,\"fls\":%u}\n",
            cycle,(unsigned long)pid,(unsigned long long)creation,(unsigned long long)result.session,
            (unsigned long)result.foreign[0],(unsigned long)result.foreign[1],(unsigned long)observed.handles,observed.tls,observed.fls);fflush(output);
        snprintf(stage,sizeof stage,"reload_after_%u",cycle);
        if(api->stage(api->context,stage))goto cleanup;
    }
    failed=0;
cleanup:
    if(reload_pool_finish(&idle))failed=1;
    if(crt && !FreeLibrary(crt))fail_closed("OpenCL reload fatal: CRT reference retirement failed");
    if(!failed && (reload_measure(&observed) || !reload_equal(&entry,&observed) || api->stage(api->context,"reload_cleanup")))failed=1;
    if(failed)fputs("OpenCL reload FAILED\n",output);
    else fprintf(output,"OPENCL_RELOAD_V1 {\"event\":\"complete\",\"cycles\":%u,\"contexts\":%u,\"cleanup\":true,\"heap_acceptance\":false,\"entry\":[%lu,%u,%u]}\n",ReloadCycles,ReloadCycles*3,(unsigned long)observed.handles,observed.tls,observed.fls);
    fflush(output);return failed;
}
/** @brief Native index leak control, using the actual positive-run equality.
 * @param[in] fls Select one explicitly owned TLS/FLS index; no candidate loads.
 * @return0 only when retention rejects and exact cleanup restores every field.
 */
static int reload_control(int fls) {
    reload_counts_t before={0},retained={0},after={0};
    if(reload_measure(&before))return 1;
    SetLastError(ERROR_SUCCESS);
    DWORD owner=fls ? FlsAlloc(NULL) : TlsAlloc();
    DWORD error=GetLastError();
    if(owner==(fls ? FLS_OUT_OF_INDEXES : TLS_OUT_OF_INDEXES)) {
        fprintf(stderr,"OpenCL reload control allocation failed kind=%s error=%lu\n",
            fls ? "fls" : "tls",(unsigned long)error);return 1;
    }
    int rejected=!reload_measure(&retained) && !reload_equal(&before,&retained) &&
        before.handles==retained.handles && (fls ? retained.fls+1==before.fls : retained.tls+1==before.tls);
    if(!(fls ? FlsFree(owner) : TlsFree(owner)))fail_closed("OpenCL reload fatal: control index retirement failed");
    if(!rejected || reload_measure(&after) || !reload_equal(&before,&after))return 1;
    printf("OPENCL_RELOAD_CONTROL kind=%s rejected=1 cleanup=1\n",fls ? "fls" : "tls");return 0;
}
