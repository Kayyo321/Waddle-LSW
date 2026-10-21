/** @file opencl_slot_certificate_windows.inc
 * @brief Qualification-only TLS bitmap certificate for one admitted AMD64 OS.
 * Include after Win32/stdint/string/wchar declarations. No heap allocation,
 * borrowed-owner release, PEB/TEB writes, warming or candidate calls occur here.
 * The sole caller must quiesce other TLS users for the entire probe. Environment
 * admission is an input from the trusted runner, not authentication: that runner
 * must verify the actual System32 binaries, CodeView records and versions before
 * and after the process, binding them to the fixture and raw evidence receipt.
 */
#if !defined(__x86_64__) && !defined(_M_X64)
#error TLS certificate requires AMD64
#endif
/** Fixed target layout/capacity; every snapshot has exactly34 bitmap words. */
enum { SlotTlsCount=1088, SlotTlsWords=34, SlotPathCapacity=512 };
/** Exact external admission token; never inferred from ERROR_NOT_ENOUGH_MEMORY. */
static const char SlotTarget[]=
    "ntdll=651ac9041af8e6b5ebd331d53163244c0a27d36d1433f0f15cac1f28c5f2e82c;"
    "kernelbase=5c29b73622ce0976e1ada566f31f2197ba0f8beaea89c3ec033366126172c3d0";
/** All nonzero statuses reject; fatal additionally requires process termination. */
typedef enum slot_status_t {
    SlotOk,SlotUnsupported,SlotReadFailed,SlotLayoutFailed,SlotTokenFailed,
    SlotFailureStatus,SlotNoOwnedToken,SlotBitmapFailed,SlotRefillFailed,
    SlotRetirementFailed
} slot_status_t;
/** Copied read-only state; pointers are identities, never directly dereferenced. */
typedef struct slot_snapshot_t {
    uint64_t teb,peb,descriptor[2],expansion;
    uint32_t words[SlotTlsWords];
} slot_snapshot_t;
/** Caller-owned evidence, initialized on every invocation. count valid iffSlotOk.
 * fatal means an ambiguous returned token or failed owned free: terminate before
 * any candidate unload/reuse. Known owners are still retired as far as possible.
 * No field is a permission to free a baseline, duplicate or out-of-range token.
 */
typedef struct slot_certificate_t {
    slot_status_t status;
    unsigned count,fatal,acquired,freed;
    DWORD first_error,refill_error;
    slot_snapshot_t baseline,full,hole,refilled,final;
    DWORD indices[SlotTlsCount];
} slot_certificate_t;
_Static_assert(sizeof(void *)==8,"TLS certificate supports AMD64 only");
/** @brief Read a fixed own-process range, rejecting null/overflow/kernel ranges.
 * @param[in] address Raw borrowed identity, never dereferenced by C.
 * @param[out] target Nonnull caller storage of size bytes,1..128.
 * @return1 only on an exact successful read. Serial caller; creates no handles.
 */
static int slot_read(uint64_t address,void *target,size_t size) {
    SIZE_T read=0;
    if(address<UINT64_C(0x10000) || address>UINT64_C(0x7fffffffffff) ||
       !size || size>128 || size-1>UINT64_C(0x7fffffffffff)-address)return 0;
    return ReadProcessMemory(GetCurrentProcess(),(const void *)(uintptr_t)address,
                             target,size,&read) && read==size;
}
/** @brief Require explicit runner admission and borrowed native module paths.
 * @return1 for exact token and both module paths under actual System32 directory.
 * No LoadLibrary, filesystem writes, file handles or authentication claim.
 */
static int slot_admitted(void) {
    char token[sizeof SlotTarget];wchar_t directory[SlotPathCapacity];
    DWORD length=GetEnvironmentVariableA("WADDLE_OPENCL_SLOT_TARGET",token,sizeof token);
    if(length!=sizeof SlotTarget-1 || memcmp(token,SlotTarget,sizeof SlotTarget))return 0;
    UINT count=GetSystemDirectoryW(directory,SlotPathCapacity);
    if(!count || count>=SlotPathCapacity-16)return 0;
    const wchar_t *names[2]={L"ntdll.dll",L"kernelbase.dll"};
    for(unsigned i=0;i<2;++i) {
        wchar_t expected[SlotPathCapacity],actual[SlotPathCapacity];
        memcpy(expected,directory,count*sizeof(wchar_t));expected[count]=L'\\';
        wcscpy(expected+count+1,names[i]);
        HMODULE module=GetModuleHandleW(names[i]);
        if(!module)return 0;
        DWORD got=GetModuleFileNameW(module,actual,SlotPathCapacity);
        if(!got || got>=SlotPathCapacity || _wcsicmp(actual,expected))return 0;
    }
    return 1;
}
/** @brief Capture the corrected PDB pointer layout through bounded RPM only.
 * @param[out] out Nonnull caller-owned snapshot; partial output invalid on error.
 * @return SlotOk,SlotReadFailed orSlotLayoutFailed. No locks/owners acquired.
 * TlsBitmap/+0x78 and TlsExpansionBitmap/+0x238 are POINTERS to16B descriptors;
 * their buffers must identify the inline8B/+0x80 and128B/+0x240 arrays exactly.
 */
static slot_status_t slot_snapshot(slot_snapshot_t *out) {
    uint32_t major=0,minor=0;uint16_t build=0;
#ifdef _WIN32
    /* Read the SDK-equivalent identity directly. GCC14 diagnoses the SDK's
     * offset-address C lvalue as out of bounds; no C pointer is dereferenced. */
    __asm__ __volatile__("movq %%gs:0x30, %0" : "=r" (out->teb));
#else
    out->teb=(uint64_t)(uintptr_t)NtCurrentTeb();
#endif
    if((out->teb&7) || out->teb>UINT64_C(0x7fffffffe878) || !slot_read(out->teb+0x60,&out->peb,8) ||
       !slot_read(out->teb+0x1780,&out->expansion,8))return SlotReadFailed;
    if(out->expansion) {
        uint64_t value;
        if(out->expansion<UINT64_C(0x10000) || (out->expansion&7) ||
           out->expansion>UINT64_C(0x7fffffffe000))return SlotLayoutFailed;
        if(!slot_read(out->expansion,&value,8) || !slot_read(out->expansion+0x1ff8,&value,8))return SlotReadFailed;
    }
    if((out->peb&7) || out->peb>UINT64_C(0x7ffffffffd80))return SlotLayoutFailed;
    if(!slot_read(out->peb+0x118,&major,4) || !slot_read(out->peb+0x11c,&minor,4) ||
       !slot_read(out->peb+0x120,&build,2) ||
       !slot_read(out->peb+0x78,&out->descriptor[0],8) ||
       !slot_read(out->peb+0x238,&out->descriptor[1],8))return SlotReadFailed;
    if(major!=10 || minor!=0 || build!=26300 || out->descriptor[0]==out->descriptor[1])return SlotLayoutFailed;
    for(unsigned i=0;i<2;++i) {
        unsigned char descriptor[16];uint32_t bits;uint64_t buffer;
        if((out->descriptor[i]&7) || !slot_read(out->descriptor[i],descriptor,sizeof descriptor))return SlotReadFailed;
        memcpy(&bits,descriptor,4);memcpy(&buffer,descriptor+8,8);
        if(bits!=(i ? 1024u : 64u) || buffer!=out->peb+(i ? 0x240u : 0x80u))return SlotLayoutFailed;
        if(!slot_read(buffer,out->words+(i ? 2 : 0),i ? 128 : 8))return SlotReadFailed;
    }
    return SlotOk;
}
/** @brief Require identities and exact baseline|owned words, without tolerances.
 * @param[in] base,now Nonnull complete snapshots. owned Exactly34 fixed words.
 * @param[in] full1 additionally requires all1088 bits set and expansion nonnull.
 * @return1 on exact state. A previously allocated expansion array cannot move.
 */
static int slot_matches(const slot_snapshot_t *base,const slot_snapshot_t *now,
                        const uint32_t *owned,int full) {
    if(base->teb!=now->teb || base->peb!=now->peb ||
       base->descriptor[0]!=now->descriptor[0] || base->descriptor[1]!=now->descriptor[1] ||
       (base->expansion && base->expansion!=now->expansion) || (full && !now->expansion))return 0;
    for(unsigned i=0;i<SlotTlsWords;++i) {
        uint32_t expected=base->words[i]|owned[i];
        if(now->words[i]!=expected || (full && expected!=UINT32_MAX))return 0;
    }
    return 1;
}
/** @brief Check a returned token before claiming ownership or touching a bitmap.
 * @param[in] index Returned successful TLS token. base/owned fixed34-word maps.
 * @return1 only if inrange, not preexisting, and not already owned.
 */
static int slot_new_token(DWORD index,const slot_snapshot_t *base,const uint32_t *owned) {
    return index<SlotTlsCount && !((base->words[index/32]|owned[index/32])&(UINT32_C(1)<<(index%32)));
}
/** @brief Produce the exact-target TLS exhaustion/retirement certificate.
 * @param[out] result Nonnull caller storage, owns no allocation after SlotOk.
 * @return Result status. Any failure rejects; fatal requires immediate process
 * termination by caller after evidence capture, never continuing to unload DLLs.
 * Every successful, uniquely identifiable nonbaseline token is stored before any
 * later failure, then reverse-freed. Ambiguous tokens are never borrowed-closed.
 */
static slot_status_t slot_certificate_probe(slot_certificate_t *result) {
    uint32_t owned[SlotTlsWords]={0};unsigned used=0;int free_failed=0;
    memset(result,0,sizeof *result);result->status=SlotUnsupported;
    if(!slot_admitted())return result->status;
    result->status=slot_snapshot(&result->baseline);
    if(result->status!=SlotOk)return result->status;
    for(;;) {
        SetLastError(ERROR_SUCCESS);DWORD index=TlsAlloc();DWORD error=GetLastError();
        if(index==TLS_OUT_OF_INDEXES) { result->first_error=error;break; }
        if(!slot_new_token(index,&result->baseline,owned)) {
            result->status=SlotTokenFailed;result->fatal=1;goto cleanup;
        }
        result->indices[used++]=index;owned[index/32]|=UINT32_C(1)<<(index%32);
    }
    result->acquired=used;
    if(result->first_error!=ERROR_NOT_ENOUGH_MEMORY) { result->status=SlotFailureStatus;goto cleanup; }
    if(!used) { result->status=SlotNoOwnedToken;goto cleanup; }
    result->status=slot_snapshot(&result->full);
    if(result->status!=SlotOk)goto cleanup;
    if(!slot_matches(&result->baseline,&result->full,owned,1)) { result->status=SlotBitmapFailed;goto cleanup; }
    DWORD released=result->indices[used-1];
    if(!TlsFree(released)) { free_failed=1;goto cleanup; }
    --used;++result->freed;owned[released/32]&=~(UINT32_C(1)<<(released%32));
    result->status=slot_snapshot(&result->hole);
    if(result->status!=SlotOk)goto cleanup;
    if(!slot_matches(&result->baseline,&result->hole,owned,0) ||
       result->hole.expansion!=result->full.expansion) { result->status=SlotBitmapFailed;goto cleanup; }
    SetLastError(ERROR_SUCCESS);DWORD refill=TlsAlloc();DWORD refill_error=GetLastError();
    if(refill==TLS_OUT_OF_INDEXES) { result->refill_error=refill_error;result->status=SlotRefillFailed;goto cleanup; }
    if(refill!=released) { result->status=SlotTokenFailed;result->fatal=1;goto cleanup; }
    result->indices[used++]=refill;owned[refill/32]|=UINT32_C(1)<<(refill%32);
    SetLastError(ERROR_SUCCESS);DWORD extra=TlsAlloc();result->refill_error=GetLastError();
    if(extra!=TLS_OUT_OF_INDEXES) {
        result->fatal=1;result->status=SlotTokenFailed;goto cleanup;
    }
    if(result->refill_error!=ERROR_NOT_ENOUGH_MEMORY) { result->status=SlotFailureStatus;goto cleanup; }
    result->status=slot_snapshot(&result->refilled);
    if(result->status!=SlotOk)goto cleanup;
    if(!slot_matches(&result->baseline,&result->refilled,owned,1) ||
       result->refilled.expansion!=result->full.expansion)result->status=SlotBitmapFailed;
cleanup:
    if(!result->acquired)result->acquired=used;
    unsigned count=used;
    while(used) {
        DWORD index=result->indices[--used];
        if(!TlsFree(index))free_failed=1;
        else { ++result->freed;owned[index/32]&=~(UINT32_C(1)<<(index%32)); }
    }
    slot_status_t final_status=slot_snapshot(&result->final);
    if(free_failed) { result->fatal=1;result->status=SlotRetirementFailed; }
    else if(final_status!=SlotOk)result->status=final_status;
    else if(!slot_matches(&result->baseline,&result->final,owned,0))result->status=SlotBitmapFailed;
    if(result->status==SlotOk)result->count=count;
    return result->status;
}
