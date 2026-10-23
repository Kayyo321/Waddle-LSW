/** @file opencl_fls_certificate_windows.inc
 * @brief Exact-target FLS marker/count certificate, no generic NT layout claim.
 * Include after opencl_slot_certificate_windows.inc for checked RPM/admission.
 * Sole quiescent caller owns all storage; static native-test storage is exclusive
 * to its main thread. No heap, candidate, borrowed frees or metadata writes.
 */
/** Eight fixed groups cover0..4079; public slots exclude permanently reserved0. */
enum { FlsSlotCount=4080,FlsPublicCount=4079,FlsGroupCount=8,FlsContextRva=0x1ceeb0 };
/** Exact copied graph. Header is diagnostic: no opaque lock/list pointer walk. */
typedef struct fls_snapshot_t {
    slot_snapshot_t target;
    uint64_t module,context,groups[FlsGroupCount];
    uint32_t counts[FlsGroupCount];
    unsigned char header[0x60];
    uint64_t markers[FlsSlotCount];
} fls_snapshot_t;
/** Five fixed snapshots and recorded public tokens; caller provides storage.
 * count valid only onSlotOk. fatal always requires process termination before
 * candidate unload/reuse, regardless of any later snapshot diagnostic status.
 */
typedef struct fls_certificate_t {
    slot_status_t status;
    unsigned count,fatal,acquired,freed;
    DWORD first_error,refill_error;
    fls_snapshot_t baseline,full,hole,refilled,final;
    DWORD indices[FlsPublicCount];
} fls_certificate_t;
/** @brief Validate one fixed group extent; no arithmetic before bounds proof.
 * @param[in] address Borrowed raw pointer. capacity One fixed group capacity.
 * @return1 for aligned user range encompassing8+24*capacity bytes.
 */
static int fls_group_range(uint64_t address,unsigned capacity) {
    uint64_t size=8+24*(uint64_t)capacity;
    return address>=UINT64_C(0x10000) && !(address&7) &&
        address<=UINT64_C(0x7fffffffffff)-(size-1);
}
/** @brief Decode the admitted image's eight bounded groups using exact RPM.
 * @param[out] out Caller-owned storage; partial raw evidence retained on error.
 * @return SlotOk or read/layout error. Allocates no memory/handles/indices.
 * Every count includes reserved0. A callback marker may be a user pointer or
 * UINT64_MAX(NULL callback); transientUINT64_MAX-1 and other noncanonical values
 * reject. Baseline marker values are later compared exactly, never normalized.
 */
static slot_status_t fls_snapshot(fls_snapshot_t *out) {
    memset(out,0,sizeof *out);
    slot_status_t status=slot_snapshot(&out->target);
    if(status!=SlotOk)return status;
    out->module=(uint64_t)(uintptr_t)GetModuleHandleW(L"ntdll.dll");
    if(out->module<UINT64_C(0x10000) || (out->module&0xffff) ||
       out->module>UINT64_C(0x7fffffffffff)-FlsContextRva-sizeof out->header)return SlotLayoutFailed;
    out->context=out->module+FlsContextRva;
    if(!slot_read(out->context,out->header,sizeof out->header))return SlotReadFailed;
    for(unsigned g=0;g<FlsGroupCount;++g) {
        memcpy(&out->groups[g],out->header+8+8*g,8);
        uint64_t group=out->groups[g];unsigned capacity=16u<<g;
        if(!group)continue;
        if(!fls_group_range(group,capacity))return SlotLayoutFailed;
        uint64_t end=group+8+24*(uint64_t)capacity;
        if(group<out->context+sizeof out->header && out->context<end)return SlotLayoutFailed;
        for(unsigned earlier=0;earlier<g;++earlier) {
            uint64_t previous=out->groups[earlier];
            if(previous && group<previous+8+24*(uint64_t)(16u<<earlier) && previous<end)return SlotLayoutFailed;
        }
        if(!slot_read(group,&out->counts[g],4))return SlotReadFailed;
        if(out->counts[g]>capacity)return SlotLayoutFailed;
        unsigned occupied=0;
        for(unsigned j=0;j<capacity;) {
            unsigned char records[120];unsigned count=capacity-j;
            if(count>5)count=5;
            if(!slot_read(group+8+24*(uint64_t)j,records,24*count))return SlotReadFailed;
            for(unsigned k=0;k<count;++k) {
                uint64_t marker;memcpy(&marker,records+8+24*k,8);
                if(marker && marker!=UINT64_MAX &&
                   (marker<UINT64_C(0x10000) || marker>UINT64_C(0x7fffffffffff)))return SlotLayoutFailed;
                out->markers[capacity+j+k-16]=marker;
                if(marker)++occupied;
            }
            j+=count;
        }
        if(out->counts[g]!=occupied)return SlotLayoutFailed;
    }
    return SlotOk;
}
/** @brief Verify borrowed identity and retained group pointers without warming.
 * @param[in] base,now Complete copied snapshots, owned by the same serial caller.
 * @return1 if module/context/PEB/TEB and every existing group identity persist.
 * A baseline-null group may be allocated later; its occupancy is checked below.
 */
static int fls_identity(const fls_snapshot_t *base,const fls_snapshot_t *now) {
    if(base->module!=now->module || base->context!=now->context ||
       base->target.teb!=now->target.teb || base->target.peb!=now->target.peb)return 0;
    for(unsigned g=0;g<FlsGroupCount;++g)
        if(base->groups[g] && base->groups[g]!=now->groups[g])return 0;
    return 1;
}
/** @brief Require exact preexisting markers plus only this probe's owned slots.
 * @param[in] base,now Complete snapshots. owned Fixed4080-byte ownership map.
 * @param[in] full1 additionally requires all eight groups/all4080 markers full.
 * @return1 only for exact transitions; counts were independently decoded/proved.
 */
static int fls_matches(const fls_snapshot_t *base,const fls_snapshot_t *now,
                       const unsigned char *owned,int full) {
    if(!fls_identity(base,now))return 0;
    for(unsigned g=0;g<FlsGroupCount;++g)if(full && !now->groups[g])return 0;
    for(unsigned i=0;i<FlsSlotCount;++i) {
        uint64_t expected=owned[i] ? UINT64_MAX : base->markers[i];
        if(now->markers[i]!=expected || (full && !expected))return 0;
    }
    return 1;
}
/** @brief Check successful public ID before claiming or indexing any owner.
 * @param[in] index Returned successful ID. base/owned Borrowed complete storage.
 * @return1 only for unique1..4079 and a baseline-free slot.
 */
static int fls_new_token(DWORD index,const fls_snapshot_t *base,const unsigned char *owned) {
    return index>0 && index<FlsSlotCount && !base->markers[index] && !owned[index];
}
/** @brief Prove target-specific FLS exhaustion and exact public-slot retirement.
 * @param[out] result Nonnull fixed caller storage (approximately180KB), exclusive.
 * @return SlotOk only after full bitmap/count, one-slot reuse, repeat error8 and
 * reverse cleanup proof. All failures reject; fatal requires process termination.
 * Group0/reserved0 must already exist at entry; missing reserved0 is unsupported
 * before allocations. Newly created empty OS group metadata may remain; no live
 * public marker or nonbaseline counter value is waived. No generic259 fallback.
 */
static slot_status_t fls_certificate_probe(fls_certificate_t *result) {
    unsigned char owned[FlsSlotCount]={0};unsigned used=0;int free_failed=0;
    memset(result,0,sizeof *result);result->status=SlotUnsupported;
    if(!slot_admitted())return result->status;
    result->status=fls_snapshot(&result->baseline);
    if(result->status!=SlotOk)return result->status;
    if(!result->baseline.groups[0] || result->baseline.markers[0]!=UINT64_MAX) {
        result->status=SlotUnsupported;return result->status;
    }
    for(;;) {
        SetLastError(ERROR_SUCCESS);DWORD index=FlsAlloc(NULL);DWORD error=GetLastError();
        if(index==FLS_OUT_OF_INDEXES) { result->first_error=error;break; }
        if(!fls_new_token(index,&result->baseline,owned)) {
            result->status=SlotTokenFailed;result->fatal=1;goto cleanup;
        }
        result->indices[used++]=index;owned[index]=1;
    }
    result->acquired=used;
    if(result->first_error!=ERROR_NOT_ENOUGH_MEMORY) { result->status=SlotFailureStatus;goto cleanup; }
    if(!used) { result->status=SlotNoOwnedToken;goto cleanup; }
    result->status=fls_snapshot(&result->full);
    if(result->status!=SlotOk)goto cleanup;
    if(!fls_matches(&result->baseline,&result->full,owned,1)) { result->status=SlotBitmapFailed;goto cleanup; }
    DWORD released=result->indices[used-1];
    if(!FlsFree(released)) { free_failed=1;goto cleanup; }
    --used;++result->freed;owned[released]=0;
    result->status=fls_snapshot(&result->hole);
    if(result->status!=SlotOk)goto cleanup;
    if(!fls_matches(&result->baseline,&result->hole,owned,0) || !fls_identity(&result->full,&result->hole)) {
        result->status=SlotBitmapFailed;goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);DWORD refill=FlsAlloc(NULL);DWORD refill_error=GetLastError();
    if(refill==FLS_OUT_OF_INDEXES) { result->refill_error=refill_error;result->status=SlotRefillFailed;goto cleanup; }
    if(refill!=released) { result->status=SlotTokenFailed;result->fatal=1;goto cleanup; }
    result->indices[used++]=refill;owned[refill]=1;
    SetLastError(ERROR_SUCCESS);DWORD extra=FlsAlloc(NULL);result->refill_error=GetLastError();
    if(extra!=FLS_OUT_OF_INDEXES) { result->fatal=1;result->status=SlotTokenFailed;goto cleanup; }
    if(result->refill_error!=ERROR_NOT_ENOUGH_MEMORY) { result->status=SlotFailureStatus;goto cleanup; }
    result->status=fls_snapshot(&result->refilled);
    if(result->status!=SlotOk)goto cleanup;
    if(!fls_matches(&result->baseline,&result->refilled,owned,1) || !fls_identity(&result->full,&result->refilled))result->status=SlotBitmapFailed;
cleanup:
    if(!result->acquired)result->acquired=used;
    unsigned count=used;
    while(used) {
        DWORD index=result->indices[--used];
        if(!FlsFree(index))free_failed=1;
        else { ++result->freed;owned[index]=0; }
    }
    slot_status_t final_status=fls_snapshot(&result->final);
    if(free_failed) { result->fatal=1;result->status=SlotRetirementFailed; }
    else if(final_status!=SlotOk)result->status=final_status;
    else if(!fls_matches(&result->baseline,&result->final,owned,0))result->status=SlotBitmapFailed;
    if(result->status==SlotOk && !fls_identity(&result->full,&result->final))result->status=SlotBitmapFailed;
    if(result->status==SlotOk)result->count=count;
    return result->status;
}
