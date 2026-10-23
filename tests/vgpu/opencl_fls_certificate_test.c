/** @file opencl_fls_certificate_test.c
 * @brief Actual target certificate exercised with API doubles or real Win32.
 * Static evidence storage is exclusively owned by this single-threaded main.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
static unsigned native_calls,native_fail_after;
static DWORD native_fls_alloc(PFLS_CALLBACK_FUNCTION callback) {
    if(native_fail_after && ++native_calls==native_fail_after) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FLS_OUT_OF_INDEXES;
    }
    return FlsAlloc(callback);
}
#define FlsAlloc native_fls_alloc
#include "opencl_slot_certificate_windows.inc"
#include "opencl_fls_certificate_windows.inc"
#undef FlsAlloc
/** Full raw graph/markers, recorded after all measured states to avoid stdio. */
static void native_record(const char *name,const fls_certificate_t *value) {
    const fls_snapshot_t *snapshots[]={&value->baseline,&value->full,&value->hole,&value->refilled,&value->final};
    printf("FLS_SLOT_NATIVE {\"case\":\"%s\",\"status\":%u,\"fatal\":%u,\"count\":%u,\"acquired\":%u,\"freed\":%u,\"errors\":[%lu,%lu],\"snapshots\":[",
        name,(unsigned)value->status,value->fatal,value->count,value->acquired,value->freed,
        (unsigned long)value->first_error,(unsigned long)value->refill_error);
    for(unsigned i=0;i<5;++i) {
        const fls_snapshot_t *s=snapshots[i];
        printf("%s{\"teb\":\"%016llx\",\"peb\":\"%016llx\",\"module\":\"%016llx\",\"context\":\"%016llx\",\"header\":\"",
            i ? "," : "",(unsigned long long)s->target.teb,(unsigned long long)s->target.peb,
            (unsigned long long)s->module,(unsigned long long)s->context);
        for(unsigned b=0;b<sizeof s->header;++b)printf("%02x",s->header[b]);
        printf("\",\"groups\":[");
        for(unsigned g=0;g<FlsGroupCount;++g)printf("%s\"%016llx\"",g ? "," : "",(unsigned long long)s->groups[g]);
        printf("],\"counts\":[");
        for(unsigned g=0;g<FlsGroupCount;++g)printf("%s%lu",g ? "," : "",(unsigned long)s->counts[g]);
        printf("],\"markers\":[");
        for(unsigned j=0;j<FlsSlotCount;++j)printf("%s\"%016llx\"",j ? "," : "",(unsigned long long)s->markers[j]);
        printf("]}");
    }
    printf("],\"tokens\":[");
    for(unsigned i=0;i<value->acquired;++i)printf("%s%lu",i ? "," : "",(unsigned long)value->indices[i]);
    printf("]}\n");fflush(stdout);
}
/** Static~720KB prevents four certificates from consuming a native thread stack. */
static fls_certificate_t native_evidence[4];
int main(int argc,char **argv) {
    (void)slot_certificate_probe; /* TLS is separately executed/gated by its unit. */
    unsigned complete=0;int failed=1;
    const char *names[]={"baseline","early_error8","retained_index","restored"};
    int unsupported=argc==2 && !strcmp(argv[1],"--expect-unsupported");
    if(argc!=1 && !unsupported)return 1;
    fls_certificate_t *e=native_evidence;
    slot_status_t status=fls_certificate_probe(&e[complete++]);
    if(unsupported) {
        native_record(names[0],&e[0]);
        if(status!=SlotUnsupported || native_calls)return 1;
        puts("FLS_SLOT_NATIVE_UNSUPPORTED PASS");return 0;
    }
    if(status!=SlotOk || e[0].fatal || e[0].count<3)goto output;
    native_calls=0;native_fail_after=3;
    status=fls_certificate_probe(&e[complete++]);native_fail_after=0;
    if(status!=SlotBitmapFailed || e[1].fatal || e[1].acquired!=2 || e[1].freed!=2 ||
       memcmp(e[1].baseline.markers,e[1].final.markers,sizeof e[1].final.markers))goto output;
    SetLastError(ERROR_SUCCESS);DWORD held=FlsAlloc(NULL);DWORD error=GetLastError();
    if(held==FLS_OUT_OF_INDEXES) { fprintf(stderr,"FLS_SLOT_NATIVE held allocation failed error=%lu\n",(unsigned long)error);goto output; }
    status=fls_certificate_probe(&e[complete++]);
    int mismatch=status!=SlotOk || e[2].fatal || e[2].count+1!=e[0].count ||
        held==0 || held>=FlsSlotCount || e[2].final.markers[held]!=UINT64_MAX;
    if(!FlsFree(held)) { fputs("FLS_SLOT_NATIVE held retirement failed\n",stderr);failed=2;goto output; }
    if(mismatch)goto output;
    status=fls_certificate_probe(&e[complete++]);
    if(status!=SlotOk || e[3].fatal || e[3].count!=e[0].count ||
       memcmp(e[0].baseline.markers,e[3].final.markers,sizeof e[3].final.markers))goto output;
    failed=0;
output:
    for(unsigned i=0;i<complete;++i)native_record(names[i],&e[i]);
    if(!failed)printf("FLS_SLOT_NATIVE PASS cases=4 early_error8_rejected=1 retained_index_rejected=1 owned=0 certificate_bytes=%zu\n",sizeof(fls_certificate_t));
    else puts("FLS_SLOT_NATIVE FAILED");
    return failed;
}
#else
#include <wctype.h>
#include "win32_fake/opencl_fls_certificate_fake.h"
#include "opencl_slot_certificate_windows.inc"
#include "opencl_fls_certificate_windows.inc"
enum { TebAddress=0x100000,PebAddress=0x200000,DescriptorAddress=0x300000,
       GroupAddress=0x400000,GroupSpacing=0x10000,GroupStorage=8+24*2048 };
static const uint64_t NtdllAddress=UINT64_C(0x180000000),AliasAddress=UINT64_C(0x190000000);
typedef struct fake_state_t {
    unsigned char teb[0x1800],peb[0x300],descriptors[64],header[0x60];
    unsigned char groups[FlsGroupCount][GroupStorage],alias[GroupStorage],owned[FlsSlotCount];
    unsigned reads,snapshots,allocations,live,frees,borrowed_frees,next;
    unsigned read_fail,read_short,alloc_fail,token_at,token_value,free_fail;
    unsigned mutate_snapshot,mutation,mutate_failure;
    unsigned bad_admission,bad_target;
    DWORD error,fail_error,second_error;
    uint64_t module;
    unsigned selected[512],selected_count;
} fake_state_t;
static fake_state_t state;
static fls_certificate_t result;
static unsigned cases,checks;
#define Check(value) do { ++checks;if(!(value)) { fprintf(stderr,"FAIL line%u: %s\n",__LINE__,#value);exit(1); } } while(0)
static void put32(unsigned char *out,uint32_t value) { memcpy(out,&value,4); }
static void put64(unsigned char *out,uint64_t value) { memcpy(out,&value,8); }
static uint32_t get32(const unsigned char *in) { uint32_t value;memcpy(&value,in,4);return value; }
static uint64_t get64(const unsigned char *in) { uint64_t value;memcpy(&value,in,8);return value; }
static unsigned group_of(unsigned index) {
    unsigned g=0;while(index>=(32u<<g)-16)++g;Check(g<FlsGroupCount);return g;
}
static unsigned char *group_bytes(unsigned g) {
    return g==7 && get64(state.header+8+8*g)==AliasAddress ? state.alias : state.groups[g];
}
static uint64_t marker(unsigned index) {
    unsigned g=group_of(index);return get64(group_bytes(g)+16+24*(index-((16u<<g)-16)));
}
static void set_marker(unsigned index,uint64_t value) {
    unsigned g=group_of(index),j=index-((16u<<g)-16);uint64_t old=marker(index);
    unsigned char *bytes=group_bytes(g);uint32_t count=get32(bytes);
    if(!old && value)++count;
    if(old && !value)--count;
    put32(bytes,count);put64(bytes+16+24*j,value);
}
static void make_group(unsigned g) { put64(state.header+8+8*g,GroupAddress+GroupSpacing*g); }
static void reset(void) {
    Check(!state.live && !state.borrowed_frees);memset(&state,0,sizeof state);
    state.fail_error=8;state.module=NtdllAddress;state.next=1;
    put64(state.teb+0x60,PebAddress);put64(state.peb+0x78,DescriptorAddress);
    put64(state.peb+0x238,DescriptorAddress+32);put32(state.peb+0x118,10);
    uint16_t build=26300;memcpy(state.peb+0x120,&build,2);
    put32(state.descriptors,64);put64(state.descriptors+8,PebAddress+0x80);
    put32(state.descriptors+32,1024);put64(state.descriptors+40,PebAddress+0x240);
    make_group(0);set_marker(0,UINT64_MAX);set_marker(2,UINT64_C(0x12345678));
}
static void mutate(unsigned mutation) {
    switch(mutation) {
    case 1:put64(state.header+8,0);break;
    case 2:set_marker(0,0);break;
    case 3:set_marker(0,UINT64_C(0x12345000));break;
    case 4:put64(state.header+8,1);break;
    case 5:put64(state.header+8,GroupAddress+1);break;
    case 6:put64(state.header+8,UINT64_C(0xffff800000000000));break;
    case 7:put64(state.header+8,UINT64_C(0x7ffffffffff8));break;
    case 8:put64(state.header+8,NtdllAddress+FlsContextRva);break;
    case 9:put64(state.header+16,GroupAddress);break;
    case 10:put64(state.header+16,GroupAddress+8);break;
    case 11:put32(state.groups[0],17);break;
    case 12:put32(state.groups[0],get32(state.groups[0])+1);break;
    case 13:set_marker(2,UINT64_MAX-1);break;
    case 14:set_marker(2,1);break;
    case 15:set_marker(2,UINT64_C(0x800000000000));break;
    case 16:set_marker(2,UINT64_C(0x12345000));break;
    case 17:set_marker(1,0);break;
    case 18:set_marker(3,UINT64_MAX);break;
    case 19:put64(state.header+8,GroupAddress+0x8000);break;
    case 20:put64(state.header+64,0);break;
    case 21:memcpy(state.alias,state.groups[7],GroupStorage);put64(state.header+64,AliasAddress);break;
    case 22:put64(state.header+8,0);make_group(1);break;
    case 23:put64(state.header+16,GroupAddress-0x1000);break;
    default:Check(0);
    }
}
static void SetLastError(DWORD error) { state.error=error; }
static DWORD GetLastError(void) { return state.error; }
static DWORD TlsAlloc(void) { Check(0);return TLS_OUT_OF_INDEXES; }
static BOOL TlsFree(DWORD index) { (void)index;Check(0);return 0; }
static DWORD FlsAlloc(void *callback) {
    Check(callback==NULL && state.error==ERROR_SUCCESS);++state.allocations;
    if(state.token_at==state.allocations)return state.token_value;
    if(state.alloc_fail!=state.allocations) {
        while(state.next<FlsSlotCount && marker(state.next))++state.next;
        if(state.next<FlsSlotCount) {
            unsigned index=state.next++;make_group(group_of(index));Check(!state.owned[index]);
            state.owned[index]=1;++state.live;set_marker(index,UINT64_MAX);return index;
        }
    }
    if(state.mutate_failure) { unsigned mutation=state.mutate_failure;state.mutate_failure=0;mutate(mutation); }
    state.error=state.second_error && state.allocations==4081 ? state.second_error : state.fail_error;
    return FLS_OUT_OF_INDEXES;
}
static BOOL FlsFree(DWORD index) {
    ++state.frees;if(state.free_fail==state.frees)return 0;
    if(index==0 || index>=FlsSlotCount || !state.owned[index]) { ++state.borrowed_frees;return 0; }
    state.owned[index]=0;--state.live;set_marker(index,0);if(index<state.next)state.next=index;return 1;
}
static void *NtCurrentTeb(void) { return (void *)(uintptr_t)TebAddress; }
static HANDLE GetCurrentProcess(void) { return (HANDLE)(uintptr_t)UINT64_MAX; }
static BOOL ReadProcessMemory(HANDLE process,const void *address,void *output,SIZE_T size,SIZE_T *read) {
    Check(process==GetCurrentProcess());++state.reads;*read=0;
    uint64_t value=(uint64_t)(uintptr_t)address;const unsigned char *source=NULL;int selected=0;
    if(value==NtdllAddress+FlsContextRva) {
        ++state.snapshots;if(state.mutate_snapshot==state.snapshots)mutate(state.mutation);
        if(size<=sizeof state.header)source=state.header;
        selected=1;
    }
    if(value>=TebAddress && value-TebAddress<=sizeof state.teb && size<=sizeof state.teb-(value-TebAddress))source=state.teb+(value-TebAddress);
    if(value>=PebAddress && value-PebAddress<=sizeof state.peb && size<=sizeof state.peb-(value-PebAddress))source=state.peb+(value-PebAddress);
    if(value>=DescriptorAddress && value-DescriptorAddress<=sizeof state.descriptors && size<=sizeof state.descriptors-(value-DescriptorAddress))source=state.descriptors+(value-DescriptorAddress);
    for(unsigned g=0;g<FlsGroupCount;++g) {
        uint64_t base=GroupAddress+GroupSpacing*g;unsigned bytes=8+24*(16u<<g);
        if(value>=base && value-base<=bytes && size<=bytes-(value-base)) {
            source=state.groups[g]+(value-base);
            if(value==base || value==base+8 || value+size==base+bytes)selected=1;
        }
    }
    if(value>=AliasAddress && value-AliasAddress<=sizeof state.alias && size<=sizeof state.alias-(value-AliasAddress))source=state.alias+(value-AliasAddress);
    if(selected) { Check(state.selected_count<512);state.selected[state.selected_count++]=state.reads; }
    if(state.read_fail==state.reads || !source)return 0;
    memcpy(output,source,size);*read=state.read_short==state.reads ? size-1 : size;return 1;
}
static DWORD GetEnvironmentVariableA(const char *name,char *output,DWORD size) {
    Check(!strcmp(name,"WADDLE_OPENCL_SLOT_TARGET") && size==sizeof SlotTarget);
    if(state.bad_admission)return 0;
    memcpy(output,SlotTarget,sizeof SlotTarget);return sizeof SlotTarget-1;
}
static UINT GetSystemDirectoryW(wchar_t *output,UINT capacity) { Check(capacity==SlotPathCapacity);wcscpy(output,L"C:\\Windows\\System32");return (UINT)wcslen(output); }
static HMODULE GetModuleHandleW(const wchar_t *name) { return (HMODULE)(uintptr_t)(!wcscmp(name,L"ntdll.dll") ? state.module : NtdllAddress+0x200000); }
static DWORD GetModuleFileNameW(HMODULE module,wchar_t *output,DWORD capacity) {
    Check(capacity==SlotPathCapacity);
    wcscpy(output,(uint64_t)(uintptr_t)module==state.module ? L"C:\\Windows\\System32\\ntdll.dll" : L"C:\\Windows\\System32\\kernelbase.dll");return (DWORD)wcslen(output);
}
static int _wcsicmp(const wchar_t *left,const wchar_t *right) {
    for(;*left && towlower(*left)==towlower(*right);++left,++right) {}
    return (int)towlower(*left)-(int)towlower(*right);
}
static void probe(slot_status_t expected,unsigned fatal) {
    Check(fls_certificate_probe(&result)==expected);Check(result.status==expected);
    Check(result.fatal==fatal && !state.borrowed_frees);if(expected!=SlotOk)Check(!result.count);++cases;
}
static void positive_cases(void) {
    reset();probe(SlotOk,0);Check(result.count==4078 && result.freed==4079 && !state.live);
    Check(!memcmp(result.baseline.markers,result.final.markers,sizeof result.final.markers));
    for(unsigned g=1;g<FlsGroupCount;++g)Check(!result.baseline.groups[g] && result.final.groups[g] && !result.final.counts[g]);
    Check(result.baseline.markers[2]==UINT64_C(0x12345678) && result.final.markers[2]==UINT64_C(0x12345678));
    reset();set_marker(2,0);probe(SlotOk,0);Check(result.count==4079);
    reset();for(unsigned g=0;g<FlsGroupCount;++g)make_group(g);
    for(unsigned i=1;i<FlsSlotCount;++i)set_marker(i,UINT64_MAX);
    set_marker(4079,0);probe(SlotOk,0);Check(result.count==1 && result.indices[0]==4079);
    reset();for(unsigned g=0;g<FlsGroupCount;++g)make_group(g);
    for(unsigned i=1;i<FlsSlotCount;i+=2)set_marker(i,UINT64_C(0x76543210));
    probe(SlotOk,0);Check(result.count==2038);
}
static void admission_layout_cases(void) {
    reset();state.bad_admission=1;probe(SlotUnsupported,0);Check(!state.allocations && !state.reads);
    reset();put32(state.peb+0x118,11);probe(SlotLayoutFailed,0);
    reset();state.read_fail=1;probe(SlotReadFailed,0);
    const uint64_t bad_modules[]={1,NtdllAddress+1,UINT64_C(0xffff800000000000)};
    for(unsigned i=0;i<3;++i) { reset();state.module=bad_modules[i];probe(SlotLayoutFailed,0); }
    for(unsigned i=1;i<=15;++i) {
        reset();state.mutate_snapshot=1;state.mutation=i;
        probe(i<=3 ? SlotUnsupported : SlotLayoutFailed,0);Check(!state.allocations);
    }
    reset();state.mutate_snapshot=1;state.mutation=19;probe(SlotReadFailed,0);
    reset();state.mutate_snapshot=1;state.mutation=22;probe(SlotUnsupported,0);
    reset();state.mutate_snapshot=1;state.mutation=23;probe(SlotReadFailed,0);
    reset();probe(SlotOk,0);unsigned selected[512],count=state.selected_count;memcpy(selected,state.selected,count*sizeof *selected);
    for(unsigned short_read=0;short_read<2;++short_read)for(unsigned i=0;i<count;++i) {
        reset();if(short_read)state.read_short=selected[i];else state.read_fail=selected[i];
        probe(SlotReadFailed,0);Check(!state.live);
    }
}
static void token_failure_cases(void) {
    const DWORD tokens[]={0,1,2,4080,UINT32_MAX-1};
    for(unsigned i=0;i<5;++i) { reset();state.token_at=3;state.token_value=tokens[i];probe(SlotTokenFailed,1);Check(!state.live); }
    const DWORD errors[]={0,5,8,259};
    for(unsigned i=0;i<4;++i)for(unsigned at=1;at<=4;at+=3) {
        reset();state.alloc_fail=at;state.fail_error=errors[i];
        probe(errors[i]!=8 ? SlotFailureStatus : (at==1 ? SlotNoOwnedToken : SlotBitmapFailed),0);Check(!state.live);
    }
    reset();for(unsigned g=0;g<FlsGroupCount;++g)make_group(g);
    for(unsigned i=1;i<FlsSlotCount;++i)set_marker(i,UINT64_MAX);
    probe(SlotNoOwnedToken,0);
    for(unsigned mutation=16;mutation<=18;++mutation) {
        reset();state.mutate_failure=mutation;if(mutation==18)state.alloc_fail=2;
        probe(SlotBitmapFailed,0);Check(!state.live);
    }
    reset();state.mutate_failure=20;probe(SlotBitmapFailed,0);Check(!state.live);
    reset();state.fail_error=259;probe(SlotFailureStatus,0);Check(!state.live);
}
static void refill_cleanup_cases(void) {
    reset();state.alloc_fail=4080;probe(SlotRefillFailed,0);Check(!state.live);
    reset();state.token_at=4080;state.token_value=2;probe(SlotTokenFailed,1);Check(!state.live);
    reset();state.token_at=4081;state.token_value=1;probe(SlotTokenFailed,1);Check(!state.live);
    reset();state.second_error=5;probe(SlotFailureStatus,0);Check(!state.live);
    for(unsigned phase=3;phase<=5;++phase) {
        reset();state.mutate_snapshot=phase;state.mutation=16;probe(SlotBitmapFailed,0);Check(!state.live);
    }
    for(unsigned phase=3;phase<=5;++phase) {
        reset();state.mutate_snapshot=phase;state.mutation=21;probe(SlotBitmapFailed,0);Check(!state.live);
    }
    for(unsigned at=1;at<=4079;at+=2039) {
        reset();state.free_fail=at;probe(SlotRetirementFailed,1);Check(state.live==(at==1 ? 0u : 1u));
        state.free_fail=0;for(unsigned i=1;i<FlsSlotCount;++i)if(state.owned[i])Check(FlsFree(i));Check(!state.live);
    }
    reset();probe(SlotOk,0);fls_snapshot_t *base=&result.baseline,*now=&result.final;
    unsigned char owned[FlsSlotCount]={0};
    Check(fls_identity(base,now));now->module++;Check(!fls_identity(base,now));Check(!fls_matches(base,now,owned,0));now->module--;
    now->context++;Check(!fls_identity(base,now));now->context--;
    now->target.teb++;Check(!fls_identity(base,now));now->target.teb--;
    now->target.peb++;Check(!fls_identity(base,now));now->target.peb--;
    now->groups[0]++;Check(!fls_identity(base,now));now->groups[0]--;
    Check(!fls_matches(base,now,owned,1));
}
int main(void) {
    (void)slot_certificate_probe; /* Separately executed by the unchanged TLS unit. */
    positive_cases();admission_layout_cases();token_failure_cases();refill_cleanup_cases();
    Check(!state.live && !state.borrowed_frees);
    printf("FLS_SLOT_CERTIFICATE_TEST PASS groups=4 cases=%u checks=%u owned=0 borrowed_frees=0 certificate_bytes=%zu\n",cases,checks,sizeof(fls_certificate_t));return 0;
}
#endif
