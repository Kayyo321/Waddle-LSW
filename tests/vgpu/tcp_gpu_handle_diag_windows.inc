/** @file tcp_gpu_handle_diag_windows.inc @brief Failure attribution for the exact native
 * process-handle retirement check of tcp_gpu_windows.c.
 * Included once by tcp_gpu_windows.c after native_handles and before handle_baseline, and by
 * the Linux host unit test tcp_gpu_handle_diag_host.c through the fake Win32/NT surface in
 * win32_fake/handle_diag_fake.h. Nothing here decides pass or fail: the acceptance rule stays
 * in handle_baseline; this file only records sample statistics and prints text.
 *
 * Provides: handle_check_t and handle_check_record (passive sample statistics), the HandleDiag*
 * constants, and the handle_diag_* types and functions below.
 *
 * Dependency contract. The including translation unit must supply before this file:
 *  - Standard C: size_t, uintptr_t, snprintf, fprintf, fputs, printf, fflush, stdout, stderr,
 *    malloc, realloc, free and memcpy. The fixture uses the C library; the host test redirects
 *    these names to capture and allocation hooks.
 *  - Win32 scalars: DWORD, ULONG, PULONG, LONG, USHORT, WCHAR, PWSTR, ULONG_PTR, PVOID, HANDLE,
 *    HMODULE and NTAPI, and LARGE_INTEGER with a signed 64-bit QuadPart member.
 *  - Win32 functions: GetCurrentProcess, GetModuleHandleA, QueryPerformanceFrequency and
 *    QueryPerformanceCounter. They are kernel32 exports, so no import library is added.
 *  - static int symbol(HMODULE module,const char *name,void *output,size_t bytes): store the
 *    address of the named export of module into output and return zero; return one and leave
 *    output untouched when the export is absent or bytes differs from the size of a function
 *    pointer. tcp_gpu_windows.c defines it before its first use.
 * It uses no other symbol of tcp_gpu_windows.c (not native_handles, handle_baseline or
 * GetProcessHandleCount). ntdll exports are resolved at run time, never linked.
 *
 * Ownership. handle_diag_begin allocates the baseline snapshot inside the caller-owned
 * handle_diag_t and handle_diag_free releases it; every other allocation is local and freed
 * before its function returns. Single fixture thread; no state outside caller structures.
 */
/** @brief Passive statistics of the samples compared by one handle_baseline call.
 * @details Plain data with no ownership; minimum, maximum and last are meaningful only when
 * samples is nonzero. Written by handle_baseline, read by failure and timing diagnostics.
 */
typedef struct handle_check_t {
    unsigned samples;  /**< Nonzero handle counts compared against the baseline so far. */
    DWORD minimum;     /**< Smallest compared count. */
    DWORD maximum;     /**< Largest compared count. */
    DWORD last;        /**< Most recently compared count. */
} handle_check_t;
/** @brief Fold one compared sample into passive check statistics.
 * @param[in,out] check Nonnull caller-owned statistics, updated in place.
 * @param[in] count Nonzero handle count that was just compared against the baseline.
 * @details No allocation, I/O or OS call; cannot influence any handle count or verdict.
 */
static void handle_check_record(handle_check_t *check,DWORD count)
{
    if(!check->samples || count<check->minimum)check->minimum=count;
    if(!check->samples || count>check->maximum)check->maximum=count;
    check->last=count;check->samples++;
}
/* Failure-attribution diagnostics. Every item below is a passive read: QueryPerformanceCounter,
 * malloc/free, and the ntdll NtQueryInformationProcess(ProcessHandleInformation) and
 * NtQueryObject(ObjectTypeInformation) queries, none of which opens, duplicates or closes a
 * handle in this process. They run strictly after the baseline sample, are consulted only to
 * print text, and never change the exit status or the pass/fail decision. All helpers run on
 * the single fixture thread and share no state beyond the caller-owned handle_diag_t. */
/** @brief Information classes and fixed capacities of the handle diagnostics; compile-time constants only. */
enum {
    /** NtQueryInformationProcess class ProcessHandleInformation (PROCESS_HANDLE_SNAPSHOT_INFORMATION). */
    HandleDiagProcessHandleInformation = 51,
    /** NtQueryObject class ObjectTypeInformation (reads the type, never the object or its name). */
    HandleDiagObjectTypeInformation = 2,
    /** First snapshot buffer size in bytes. */
    HandleDiagInitialBytes = 32768,
    /** Largest snapshot buffer in bytes; a larger table is reported as unavailable. */
    HandleDiagMaxBytes = 16777216,
    /** Query attempts before a snapshot is reported as unavailable. */
    HandleDiagMaxAttempts = 6,
    /** Bytes of the stack buffer that receives one OBJECT_TYPE_INFORMATION. */
    HandleDiagTypeBytes = 1024,
    /** Capacity in characters of a printed type name including the terminator. */
    HandleDiagNameChars = 48,
    /** Capacity in characters of the stored "snapshot unavailable" reason including the terminator. */
    HandleDiagReasonChars = 96
};
/** @brief STATUS_INFO_LENGTH_MISMATCH, the only status that asks for a larger buffer. */
static const ULONG HandleDiagStatusInfoLengthMismatch = 0xC0000004UL;
/** @brief ntdll NtQueryInformationProcess ABI; resolved with GetProcAddress, borrowed export. */
typedef LONG (NTAPI *handle_diag_query_process_t)(HANDLE,ULONG,PVOID,ULONG,PULONG);
/** @brief ntdll NtQueryObject ABI; resolved with GetProcAddress, borrowed export. */
typedef LONG (NTAPI *handle_diag_query_object_t)(HANDLE,ULONG,PVOID,ULONG,PULONG);
/** @brief Local PROCESS_HANDLE_SNAPSHOT_INFORMATION header; entries follow it directly. */
typedef struct handle_diag_header_t {
    ULONG_PTR number_of_handles;  /**< Entries that follow the header. */
    ULONG_PTR reserved;           /**< Unused. */
} handle_diag_header_t;
/** @brief Local PROCESS_HANDLE_TABLE_ENTRY_INFO, as written by the kernel. */
typedef struct handle_diag_wire_entry_t {
    HANDLE handle_value;          /**< Handle value in this process. */
    ULONG_PTR handle_count;       /**< Unused. */
    ULONG_PTR pointer_count;      /**< Unused. */
    ULONG granted_access;         /**< Access mask of the handle. */
    ULONG object_type_index;      /**< OS object type index. */
    ULONG handle_attributes;      /**< Unused. */
    ULONG reserved;               /**< Unused. */
} handle_diag_wire_entry_t;
_Static_assert(sizeof(handle_diag_header_t)==2*sizeof(ULONG_PTR),"ProcessHandleInformation header layout");
_Static_assert(sizeof(handle_diag_wire_entry_t)==3*sizeof(ULONG_PTR)+4*sizeof(ULONG) && sizeof(HANDLE)==sizeof(ULONG_PTR),"PROCESS_HANDLE_TABLE_ENTRY_INFO layout");
/** @brief Leading UNICODE_STRING of OBJECT_TYPE_INFORMATION; the characters follow in the same buffer. */
typedef struct handle_diag_type_info_t {
    USHORT length;                /**< Type name bytes, excluding any terminator. */
    USHORT maximum_length;        /**< Type name capacity in bytes. */
    PWSTR buffer;                 /**< Characters, inside the queried buffer. */
} handle_diag_type_info_t;
/** @brief One handle of a snapshot; values only, no reference to any kernel object. */
typedef struct handle_diag_entry_t {
    uintptr_t handle_value;       /**< Handle value in this process. */
    ULONG type_index;             /**< OS object type index. */
    ULONG granted_access;         /**< Access mask of the handle. */
} handle_diag_entry_t;
/** @brief Heap copy of the process handle table at one instant. */
typedef struct handle_diag_set_t {
    handle_diag_entry_t *entries; /**< Owned malloc array of count records, NULL when count is zero. */
    size_t count;                 /**< Records in entries. */
    char unavailable[HandleDiagReasonChars]; /**< Empty when the set is valid, else why capture failed. */
} handle_diag_set_t;
/** @brief Failure-attribution context created right after the baseline sample.
 * @details Owned by main; handle_diag_free releases the baseline snapshot on every exit path.
 */
typedef struct handle_diag_t {
    unsigned long long frequency;  /**< QueryPerformanceCounter ticks per second, zero if unavailable. */
    unsigned long long start;      /**< Counter value captured right after the baseline sample. */
    handle_diag_query_process_t query_process;  /**< Borrowed ntdll export or NULL. */
    handle_diag_query_object_t query_object;    /**< Borrowed ntdll export or NULL. */
    handle_diag_set_t baseline;    /**< Owned snapshot taken after the baseline sample. */
} handle_diag_t;
/** @brief Release a snapshot and clear it.
 * @param[in,out] set Nonnull; entries are freed and NULLed, count and reason cleared.
 * @details Idempotent; safe on a zero-initialized set. Single fixture thread.
 */
static void handle_diag_set_free(handle_diag_set_t *set)
{
    free(set->entries);set->entries=NULL;set->count=0;set->unavailable[0]=0;
}
/** @brief Copy the process handle table into a heap array via NtQueryInformationProcess(51).
 * @param[in] diag Nonnull context holding the resolved ntdll exports.
 * @param[out] set Nonnull set; cleared first, then owns a malloc array on success.
 * @return Zero on success; one when unavailable, with the reason text left in set->unavailable.
 * @details The raw kernel buffer is private to this call and freed on every path; the grown
 * size is capped at HandleDiagMaxBytes. The query reads kernel state only and opens no handle.
 */
static int handle_diag_capture(const handle_diag_t *diag,handle_diag_set_t *set)
{
    handle_diag_set_free(set);
    unsigned char *raw=NULL;handle_diag_entry_t *entries=NULL;handle_diag_header_t header={0};
    size_t bytes=HandleDiagInitialBytes,count=0;int failed=1;
    if(!diag->query_process){snprintf(set->unavailable,sizeof set->unavailable,"NtQueryInformationProcess export missing");return 1;}
    for(unsigned attempt=0;;attempt++){
        unsigned char *grown=realloc(raw,bytes);
        if(!grown){snprintf(set->unavailable,sizeof set->unavailable,"allocation of %llu bytes failed",(unsigned long long)bytes);goto done;}
        raw=grown;ULONG needed=0;
        LONG status=diag->query_process(GetCurrentProcess(),HandleDiagProcessHandleInformation,raw,(ULONG)bytes,&needed);
        if(status>=0)break;
        if((ULONG)status!=HandleDiagStatusInfoLengthMismatch || attempt+1>=HandleDiagMaxAttempts){
            snprintf(set->unavailable,sizeof set->unavailable,"NtQueryInformationProcess status=0x%08lx",(unsigned long)(ULONG)status);goto done;
        }
        size_t wanted=needed>bytes ? (size_t)needed : bytes;
        if(wanted>(size_t)HandleDiagMaxBytes || bytes==(size_t)HandleDiagMaxBytes){snprintf(set->unavailable,sizeof set->unavailable,"snapshot exceeds %d bytes",(int)HandleDiagMaxBytes);goto done;}
        bytes=wanted>(size_t)HandleDiagMaxBytes/2 ? (size_t)HandleDiagMaxBytes : wanted*2;
    }
    memcpy(&header,raw,sizeof header);
    if(header.number_of_handles>(bytes-sizeof header)/sizeof(handle_diag_wire_entry_t)){
        snprintf(set->unavailable,sizeof set->unavailable,"invalid snapshot layout handles=%llu bytes=%llu",(unsigned long long)header.number_of_handles,(unsigned long long)bytes);goto done;
    }
    count=(size_t)header.number_of_handles;
    if(count){
        entries=malloc(count*sizeof *entries);
        if(!entries){snprintf(set->unavailable,sizeof set->unavailable,"allocation of %llu entries failed",(unsigned long long)count);goto done;}
    }
    for(size_t index=0;index<count;index++){
        handle_diag_wire_entry_t wire;memcpy(&wire,raw+sizeof header+index*sizeof wire,sizeof wire);
        entries[index]=(handle_diag_entry_t){(uintptr_t)wire.handle_value,wire.object_type_index,wire.granted_access};
    }
    set->entries=entries;set->count=count;entries=NULL;failed=0;
done:
    free(entries);free(raw);
    return failed;
}
/** @brief Report whether a snapshot holds a handle with identical value, type index and access.
 * @param[in] set Nonnull valid set, borrowed. @param[in] entry Nonnull record, borrowed.
 * @return One when present, else zero. No allocation, no OS call.
 */
static int handle_diag_contains(const handle_diag_set_t *set,const handle_diag_entry_t *entry)
{
    for(size_t index=0;index<set->count;index++){
        const handle_diag_entry_t *other=&set->entries[index];
        if(other->handle_value==entry->handle_value && other->type_index==entry->type_index && other->granted_access==entry->granted_access)return 1;
    }
    return 0;
}
/** @brief Name an OS object type from a live handle of that type index.
 * @param[in] diag Nonnull context holding the NtQueryObject export.
 * @param[in] live Nonnull valid snapshot taken just before; its handles are queried.
 * @param[in] type_index OS object type index to name.
 * @param[out] name Nonnull buffer of size bytes; always NUL-terminated, "unknown" on failure.
 * @param[in] size Capacity of name, at least one.
 * @details Uses only NtQueryObject(ObjectTypeInformation), which reads the type and never the
 * object, so it cannot block on pipes; object names are never queried. A handle that closed
 * since the snapshot just fails and the next live handle of the type is tried.
 */
static void handle_diag_type_name(const handle_diag_t *diag,const handle_diag_set_t *live,ULONG type_index,char *name,size_t size)
{
    if(!size)return;
    snprintf(name,size,"unknown");
    if(!diag->query_object)return;
    for(size_t index=0;index<live->count;index++){
        if(live->entries[index].type_index!=type_index)continue;
        _Alignas(16) unsigned char buffer[HandleDiagTypeBytes]={0};
        if(diag->query_object((HANDLE)live->entries[index].handle_value,HandleDiagObjectTypeInformation,buffer,sizeof buffer,NULL)<0)continue;
        handle_diag_type_info_t info;memcpy(&info,buffer,sizeof info);
        const uintptr_t begin=(uintptr_t)buffer,end=begin+sizeof buffer,text=(uintptr_t)info.buffer;
        if(!info.length || (info.length&1u) || text<begin+sizeof info || text>end || info.length>end-text)continue;
        size_t used=0;
        for(size_t offset=0;offset<info.length && used+1<size;offset+=2){
            WCHAR unit;memcpy(&unit,(const unsigned char *)text+offset,sizeof unit);
            name[used++]=(unit>=0x20 && unit<0x7f) ? (char)unit : '?';
        }
        name[used]=0;return;
    }
}
/** @brief Print every handle of source that is absent from other.
 * @param[in] diag Nonnull context. @param[in] label Static text, "added" or "removed".
 * @param[in] source,other Nonnull valid snapshots, borrowed. @param[in] live Nonnull snapshot used to name types.
 * @return Number of lines printed to stderr.
 */
static size_t handle_diag_print_delta(const handle_diag_t *diag,const char *label,const handle_diag_set_t *source,const handle_diag_set_t *other,const handle_diag_set_t *live)
{
    size_t printed=0;
    for(size_t index=0;index<source->count;index++){
        const handle_diag_entry_t *entry=&source->entries[index];char name[HandleDiagNameChars];
        if(handle_diag_contains(other,entry))continue;
        handle_diag_type_name(diag,live,entry->type_index,name,sizeof name);
        fprintf(stderr,"handle_diag: %s value=0x%llx type_index=%lu type=%s access=0x%08lx\n",label,(unsigned long long)entry->handle_value,(unsigned long)entry->type_index,name,(unsigned long)entry->granted_access);
        printed++;
    }
    return printed;
}
/** @brief Start failure attribution right after the baseline sample.
 * @param[out] diag Nonnull zero-initialized context; owned by the caller, released by handle_diag_free.
 * @details Records the QueryPerformanceCounter origin first, resolves the ntdll exports without a
 * new link dependency, then snapshots the handle table. Must be called after the baseline sample
 * and before the first lifetime. Never fails: an unavailable snapshot is only remembered as text.
 */
static void handle_diag_begin(handle_diag_t *diag)
{
    LARGE_INTEGER frequency={0},ticks={0};
    if(QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0 && QueryPerformanceCounter(&ticks)){
        diag->frequency=(unsigned long long)frequency.QuadPart;diag->start=(unsigned long long)ticks.QuadPart;
    }
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    if(!ntdll || symbol(ntdll,"NtQueryInformationProcess",&diag->query_process,sizeof diag->query_process))diag->query_process=NULL;
    if(!ntdll || symbol(ntdll,"NtQueryObject",&diag->query_object,sizeof diag->query_object))diag->query_object=NULL;
    (void)handle_diag_capture(diag,&diag->baseline);
}
/** @brief Release the baseline snapshot owned by a context.
 * @param[in,out] diag Nonnull context; idempotent, safe on a zero-initialized context.
 */
static void handle_diag_free(handle_diag_t *diag)
{
    handle_diag_set_free(&diag->baseline);
}
/** @brief Milliseconds since the baseline was captured.
 * @param[in] diag Nonnull context. @return Elapsed whole milliseconds, zero if the counter is unavailable.
 * @details Reads QueryPerformanceCounter only; overflow-free integer arithmetic.
 */
static unsigned long long handle_diag_now_ms(const handle_diag_t *diag)
{
    LARGE_INTEGER ticks={0};
    if(!diag->frequency || !QueryPerformanceCounter(&ticks) || (unsigned long long)ticks.QuadPart<diag->start)return 0;
    const unsigned long long delta=(unsigned long long)ticks.QuadPart-diag->start;
    return delta/diag->frequency*1000u+delta%diag->frequency*1000u/diag->frequency;
}
/** @brief Print the baseline snapshot outcome to stdout.
 * @param[in] diag Nonnull context. @param[in] baseline The count sampled just before the snapshot,
 * printed so a snapshot that disagrees with the count is recognisable.
 * @details Informational only; flushes stdout.
 */
static void handle_diag_describe_baseline(const handle_diag_t *diag,DWORD baseline)
{
    if(diag->baseline.unavailable[0])printf("handle_diag: snapshot unavailable (baseline: %s)\n",diag->baseline.unavailable);
    else printf("handle_diag: baseline snapshot handles=%llu process_handle_count=%lu\n",(unsigned long long)diag->baseline.count,(unsigned long)baseline);
    fflush(stdout);
}
/** @brief Print one timing line for a lifetime whose retirement check passed.
 * @param[in] iteration Zero-based lifetime index. @param[in] check Nonnull statistics of the passed check.
 * @param[in] start_ms,check_start_ms,check_done_ms Milliseconds since the baseline at lifetime
 * start, check start and first matching sample.
 * @details Stdout only, prefixed handle_diag: so the supervisors' PASS parsing is untouched.
 */
static void handle_diag_iteration(unsigned iteration,const handle_check_t *check,unsigned long long start_ms,unsigned long long check_start_ms,unsigned long long check_done_ms)
{
    printf("handle_diag: lifetime=%u iteration_index=%u samples=%u count=%lu start_ms=%llu check_start_ms=%llu check_done_ms=%llu\n",
        iteration+1,iteration,check->samples,(unsigned long)check->last,start_ms,check_start_ms,check_done_ms);fflush(stdout);
}
/** @brief Print failure attribution for a failed retirement check; stderr only.
 * @param[in] diag Nonnull context. @param[in] baseline The baseline count that was not reached.
 * @param[in] check Nonnull statistics of the failed check. @param[in] iteration Zero-based lifetime index.
 * @param[in] check_start_ms Milliseconds since the baseline when the check began.
 * @details Prints samples, minimum, maximum, last compared sample and timing, then takes a second
 * snapshot and lists handles added and removed relative to the baseline snapshot. Called only
 * after the verdict is final; the second snapshot is private and freed here. Any unavailable
 * step prints handle_diag: snapshot unavailable (...) and continues.
 */
static void handle_diag_report(const handle_diag_t *diag,DWORD baseline,const handle_check_t *check,unsigned iteration,unsigned long long check_start_ms)
{
    fprintf(stderr,"handle_diag: retirement failed lifetime=%u iteration_index=%u baseline=%lu samples=%u",iteration+1,iteration,(unsigned long)baseline,check->samples);
    if(check->samples)fprintf(stderr," min=%lu max=%lu last=%lu",(unsigned long)check->minimum,(unsigned long)check->maximum,(unsigned long)check->last);
    else fputs(" min=none max=none last=none",stderr);
    fprintf(stderr," check_start_ms=%llu elapsed_ms=%llu\n",check_start_ms,handle_diag_now_ms(diag));
    handle_diag_set_t current={0};
    if(diag->baseline.unavailable[0])fprintf(stderr,"handle_diag: snapshot unavailable (baseline: %s)\n",diag->baseline.unavailable);
    else if(handle_diag_capture(diag,&current))fprintf(stderr,"handle_diag: snapshot unavailable (failure: %s)\n",current.unavailable);
    else{
        const size_t added=handle_diag_print_delta(diag,"added",&current,&diag->baseline,&current);
        const size_t removed=handle_diag_print_delta(diag,"removed",&diag->baseline,&current,&current);
        fprintf(stderr,"handle_diag: snapshot baseline_handles=%llu current_handles=%llu added=%llu removed=%llu\n",
            (unsigned long long)diag->baseline.count,(unsigned long long)current.count,(unsigned long long)added,(unsigned long long)removed);
    }
    handle_diag_set_free(&current);fflush(stderr);
}
