/** @file opencl_slot_certificate_test.c
 * @brief Execute the actual bounded certificate with strict Win32 API doubles.
 * Every simulated successful allocation has an explicit owner. Fault controls
 * preserve borrowed tokens, then retire remaining known owners before reset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <wchar.h>
static unsigned native_calls,native_fail_after;
static DWORD native_tls_alloc(void) {
    if(native_fail_after && ++native_calls==native_fail_after) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);return TLS_OUT_OF_INDEXES;
    }
    return TlsAlloc();
}
#define TlsAlloc native_tls_alloc
#include "opencl_slot_certificate_windows.inc"
#undef TlsAlloc
/** Print every copied identity/word and acquired token, including failed probes. */
static void native_record(const char *name,const slot_certificate_t *value) {
    const slot_snapshot_t *snapshots[]={&value->baseline,&value->full,&value->hole,&value->refilled,&value->final};
    printf("TLS_SLOT_NATIVE {\"case\":\"%s\",\"status\":%u,\"fatal\":%u,\"count\":%u,\"acquired\":%u,\"freed\":%u,\"errors\":[%lu,%lu],\"snapshots\":[",
        name,(unsigned)value->status,value->fatal,value->count,value->acquired,value->freed,
        (unsigned long)value->first_error,(unsigned long)value->refill_error);
    for(unsigned i=0;i<5;++i) {
        const slot_snapshot_t *s=snapshots[i];
        printf("%s{\"teb\":\"%016llx\",\"peb\":\"%016llx\",\"descriptors\":[\"%016llx\",\"%016llx\"],\"expansion\":\"%016llx\",\"words\":[",
            i ? "," : "",(unsigned long long)s->teb,(unsigned long long)s->peb,
            (unsigned long long)s->descriptor[0],(unsigned long long)s->descriptor[1],(unsigned long long)s->expansion);
        for(unsigned w=0;w<SlotTlsWords;++w)printf("%s%lu",w ? "," : "",(unsigned long)s->words[w]);
        printf("]}");
    }
    printf("],\"tokens\":[");
    for(unsigned i=0;i<value->acquired;++i)printf("%s%lu",i ? "," : "",(unsigned long)value->indices[i]);
    printf("]}\n");fflush(stdout);
}
int main(int argc,char **argv) {
    slot_certificate_t evidence[4];unsigned complete=0;int failed=1;
    const char *names[]={"baseline","early_error8","retained_index","restored"};
    int unsupported=argc==2 && !strcmp(argv[1],"--expect-unsupported");
    if(argc!=1 && !unsupported)return 1;
    slot_status_t status=slot_certificate_probe(&evidence[complete++]);
    if(unsupported) {
        native_record(names[0],&evidence[0]);
        if(status!=SlotUnsupported || native_calls)return 1;
        puts("TLS_SLOT_NATIVE_UNSUPPORTED PASS");return 0;
    }
    if(status!=SlotOk || evidence[0].fatal || evidence[0].count<3)goto output;
    native_calls=0;native_fail_after=3;
    status=slot_certificate_probe(&evidence[complete++]);native_fail_after=0;
    if(status!=SlotBitmapFailed || evidence[1].fatal || evidence[1].acquired!=2 || evidence[1].freed!=2 ||
       memcmp(evidence[1].baseline.words,evidence[1].final.words,sizeof evidence[1].final.words))goto output;
    SetLastError(ERROR_SUCCESS);DWORD held=TlsAlloc();DWORD error=GetLastError();
    if(held==TLS_OUT_OF_INDEXES) { fprintf(stderr,"TLS_SLOT_NATIVE held allocation failed error=%lu\n",(unsigned long)error);goto output; }
    status=slot_certificate_probe(&evidence[complete++]);
    int mismatch=status!=SlotOk || evidence[2].fatal || evidence[2].count+1!=evidence[0].count ||
        held>=SlotTlsCount || !(evidence[2].final.words[held/32]&(UINT32_C(1)<<(held%32)));
    if(!TlsFree(held)) { fputs("TLS_SLOT_NATIVE held retirement failed\n",stderr);failed=2;goto output; }
    if(mismatch)goto output;
    status=slot_certificate_probe(&evidence[complete++]);
    if(status!=SlotOk || evidence[3].fatal || evidence[3].count!=evidence[0].count ||
       memcmp(evidence[0].baseline.words,evidence[3].final.words,sizeof evidence[3].final.words))goto output;
    failed=0;
output:
    /* No stdio between measured states: output cannot create intervening TLS. */
    for(unsigned i=0;i<complete;++i)native_record(names[i],&evidence[i]);
    if(!failed)puts("TLS_SLOT_NATIVE PASS cases=4 early_error8_rejected=1 retained_index_rejected=1 owned=0");
    else puts("TLS_SLOT_NATIVE FAILED");
    return failed;
}
#else
#include "win32_fake/opencl_slot_certificate_fake.h"
#include "opencl_slot_certificate_windows.inc"

enum { TebAddress=0x100000,PebAddress=0x200000,DescriptorAddress=0x300000,
       ExpansionAddress=0x400000 };
typedef struct fake_state_t {
    unsigned char teb[0x1800],peb[0x300],descriptors[64],expansion[0x4000];
    unsigned char owned[SlotTlsCount];
    unsigned reads,snapshots,allocations,live,frees,borrowed_frees;
    unsigned read_fail,read_short,alloc_fail,token_at,token_value,free_fail;
    unsigned snapshot_mutate,mutation,failure_mutation;
    DWORD error,fail_error,second_error;
    unsigned env_bad,directory_bad,module_bad,path_bad;
    uint64_t teb_address;
} fake_state_t;
static fake_state_t state;
static unsigned checks,cases;
#define Check(value) do { ++checks;if(!(value)) { fprintf(stderr,"FAIL line%u: %s\n",__LINE__,#value);exit(1); } } while(0)
static void put32(unsigned char *out,uint32_t value) { memcpy(out,&value,4); }
static void put64(unsigned char *out,uint64_t value) { memcpy(out,&value,8); }
static uint32_t word(unsigned i) {
    uint32_t value;memcpy(&value,state.peb+(i<2 ? 0x80+4*i : 0x240+4*(i-2)),4);return value;
}
static void set_word(unsigned i,uint32_t value) { put32(state.peb+(i<2 ? 0x80+4*i : 0x240+4*(i-2)),value); }
static void set_bit(unsigned index,int value) {
    uint32_t mask=UINT32_C(1)<<(index%32),bits=word(index/32);
    set_word(index/32,value ? bits|mask : bits&~mask);
}
static void reset(void) {
    Check(state.live==0);Check(state.borrowed_frees==0);
    memset(&state,0,sizeof state);state.fail_error=ERROR_NOT_ENOUGH_MEMORY;state.teb_address=TebAddress;
    put64(state.teb+0x60,PebAddress);put64(state.peb+0x78,DescriptorAddress);
    put64(state.peb+0x238,DescriptorAddress+32);
    put32(state.peb+0x118,10);put32(state.peb+0x11c,0);
    uint16_t build=26300;memcpy(state.peb+0x120,&build,2);
    put32(state.descriptors,64);put64(state.descriptors+8,PebAddress+0x80);
    put32(state.descriptors+32,1024);put64(state.descriptors+40,PebAddress+0x240);
    set_bit(0,1);set_bit(37,1);
}
/** Mutation controls corrupt copied OS state only, never the actual helper. */
static void mutate(unsigned kind) {
    switch(kind) {
    case 1:put32(state.peb+0x118,11);break;
    case 2:put32(state.peb+0x11c,1);break;
    case 3:memset(state.peb+0x120,0,2);break;
    case 4:put64(state.peb+0x238,DescriptorAddress);break;
    case 5:put64(state.peb+0x78,DescriptorAddress+1);break;
    case 6:put32(state.descriptors,63);break;
    case 7:put32(state.descriptors+32,1023);break;
    case 8:put64(state.descriptors+8,PebAddress+0x84);break;
    case 9:put64(state.descriptors+40,PebAddress+0x248);break;
    case 10:put64(state.teb+0x60,PebAddress+1);break;
    case 11:put64(state.teb+0x60,UINT64_MAX);break;
    case 12:put64(state.peb+0x78,0);break;
    case 13:put64(state.teb+0x1780,0);break;
    case 14:put64(state.teb+0x1780,ExpansionAddress+0x1000);break;
    case 15:set_bit(0,0);break;
    case 16:set_bit(1,0);break;
    case 17:set_bit(2,1);break;
    case 18:put64(state.peb+0x78,DescriptorAddress+16);memcpy(state.descriptors+16,state.descriptors,16);break;
    case 19:state.teb_address=TebAddress+1;break;
    case 20:state.teb_address=UINT64_MAX-0x50;break;
    case 21:put64(state.teb+0x1780,1);break;
    case 22:put64(state.teb+0x1780,ExpansionAddress+1);break;
    case 23:put64(state.teb+0x1780,UINT64_C(0xffff800000000000));break;
    case 24:put64(state.teb+0x1780,UINT64_C(0x7ffffffff000));break;
    case 25:put64(state.teb+0x1780,ExpansionAddress+0x3000);break;
    default:Check(0);
    }
}
static void SetLastError(DWORD error) { state.error=error; }
static DWORD GetLastError(void) { return state.error; }
static DWORD TlsAlloc(void) {
    ++state.allocations;Check(state.error==ERROR_SUCCESS);
    if(state.token_at==state.allocations)return state.token_value;
    if(state.alloc_fail!=state.allocations)for(unsigned i=0;i<SlotTlsCount;++i) {
        if(!(word(i/32)&(UINT32_C(1)<<(i%32)))) {
            Check(!state.owned[i]);state.owned[i]=1;++state.live;set_bit(i,1);
            if(i>=64)put64(state.teb+0x1780,ExpansionAddress);
            return i;
        }
    }
    if(state.failure_mutation) { unsigned mutation=state.failure_mutation;state.failure_mutation=0;mutate(mutation); }
    state.error=state.second_error && state.allocations==1089 ? state.second_error : state.fail_error;return TLS_OUT_OF_INDEXES;
}
static BOOL TlsFree(DWORD index) {
    ++state.frees;
    if(state.free_fail==state.frees)return 0;
    if(index>=SlotTlsCount || !state.owned[index]) { ++state.borrowed_frees;return 0; }
    state.owned[index]=0;--state.live;set_bit(index,0);return 1;
}
static void *NtCurrentTeb(void) {
    ++state.snapshots;
    if(state.snapshot_mutate==state.snapshots)mutate(state.mutation);
    return (void *)(uintptr_t)state.teb_address;
}
static HANDLE GetCurrentProcess(void) { return (HANDLE)(uintptr_t)UINT64_MAX; }
static BOOL ReadProcessMemory(HANDLE process,const void *address,void *output,SIZE_T size,SIZE_T *read) {
    Check(process==GetCurrentProcess());++state.reads;*read=0;
    if(state.read_fail==state.reads)return 0;
    uint64_t value=(uint64_t)(uintptr_t)address;
    const unsigned char *source=NULL;
    if(value>=TebAddress && value-TebAddress<=sizeof state.teb && size<=sizeof state.teb-(value-TebAddress))source=state.teb+(value-TebAddress);
    if(value>=PebAddress && value-PebAddress<=sizeof state.peb && size<=sizeof state.peb-(value-PebAddress))source=state.peb+(value-PebAddress);
    if(value>=DescriptorAddress && value-DescriptorAddress<=sizeof state.descriptors && size<=sizeof state.descriptors-(value-DescriptorAddress))source=state.descriptors+(value-DescriptorAddress);
    if(value>=ExpansionAddress && value-ExpansionAddress<=sizeof state.expansion && size<=sizeof state.expansion-(value-ExpansionAddress))source=state.expansion+(value-ExpansionAddress);
    if(!source)return 0;
    memcpy(output,source,size);*read=state.read_short==state.reads ? size-1 : size;return 1;
}
static DWORD GetEnvironmentVariableA(const char *name,char *output,DWORD size) {
    Check(!strcmp(name,"WADDLE_OPENCL_SLOT_TARGET"));Check(size==sizeof SlotTarget);
    if(state.env_bad==1)return 0;
    if(state.env_bad==2)return size+1;
    memcpy(output,SlotTarget,sizeof SlotTarget);if(state.env_bad==3)output[0]='X';
    return sizeof SlotTarget-1;
}
static UINT GetSystemDirectoryW(wchar_t *output,UINT capacity) {
    Check(capacity==SlotPathCapacity);wcscpy(output,L"C:\\Windows\\System32");
    if(state.directory_bad==1)return 0;
    if(state.directory_bad==2)return capacity;
    return (UINT)wcslen(output);
}
static HMODULE GetModuleHandleW(const wchar_t *name) {
    unsigned id=!wcscmp(name,L"ntdll.dll") ? 1 : 2;
    if(state.module_bad==id)return NULL;
    return (HMODULE)(uintptr_t)id;
}
static DWORD GetModuleFileNameW(HMODULE module,wchar_t *output,DWORD capacity) {
    Check(capacity==SlotPathCapacity);uintptr_t id=(uintptr_t)module;
    Check(id==1 || id==2);
    if(state.path_bad==1)return 0;
    if(state.path_bad==2)return capacity;
    wcscpy(output,state.path_bad==3 ? L"D:\\untrusted.dll" : (id==1 ? L"c:\\windows\\system32\\NTDLL.dll" : L"C:\\Windows\\System32\\kernelbase.dll"));
    return (DWORD)wcslen(output);
}
static int _wcsicmp(const wchar_t *left,const wchar_t *right) {
    for(;*left && towlower(*left)==towlower(*right);++left,++right) {}
    return (int)towlower(*left)-(int)towlower(*right);
}
static slot_certificate_t probe(slot_status_t expected,unsigned fatal) {
    slot_certificate_t result;Check(slot_certificate_probe(&result)==expected);
    Check(result.status==expected);Check(result.fatal==fatal);Check(state.borrowed_frees==0);
    if(expected!=SlotOk)Check(result.count==0);
    ++cases;return result;
}
static void positive_cases(void) {
    reset();slot_certificate_t result=probe(SlotOk,0);
    Check(result.count==1086);Check(result.acquired==1086);Check(result.freed==1087);
    Check(result.first_error==8 && result.refill_error==8);Check(!state.live);
    Check(!memcmp(result.baseline.words,result.final.words,sizeof result.final.words));
    reset();for(unsigned i=0;i<SlotTlsWords;++i)set_word(i,0);
    result=probe(SlotOk,0);Check(result.count==1088);Check(result.freed==1089);
    reset();for(unsigned i=0;i<SlotTlsWords;++i)set_word(i,UINT32_MAX);
    set_bit(100,0);put64(state.teb+0x1780,ExpansionAddress);
    result=probe(SlotOk,0);Check(result.count==1);Check(result.indices[0]==100);
    reset();for(unsigned i=0;i<SlotTlsWords;++i)set_word(i,UINT32_C(0x55555555));
    result=probe(SlotOk,0);Check(result.count==544);
}
static void admission_and_layout_cases(void) {
    /* DLL file versions26100 do not identify the measured PEB build26300. */
    reset();uint16_t wrong_build=26100;memcpy(state.peb+0x120,&wrong_build,2);
    probe(SlotLayoutFailed,0);Check(!state.allocations);
    for(unsigned i=1;i<=3;++i) { reset();state.env_bad=i;probe(SlotUnsupported,0);Check(!state.reads && !state.allocations); }
    for(unsigned i=1;i<=2;++i) { reset();state.directory_bad=i;probe(SlotUnsupported,0); }
    for(unsigned i=1;i<=2;++i) { reset();state.module_bad=i;probe(SlotUnsupported,0); }
    for(unsigned i=1;i<=3;++i) { reset();state.path_bad=i;probe(SlotUnsupported,0); }
    for(unsigned i=1;i<=12;++i) {
        reset();state.snapshot_mutate=1;state.mutation=i;
        probe((i==5 || i==12) ? SlotReadFailed : SlotLayoutFailed,0);Check(!state.allocations);
    }
    for(unsigned i=19;i<=20;++i) { reset();state.snapshot_mutate=1;state.mutation=i;probe(SlotReadFailed,0); }
    for(unsigned i=21;i<=25;++i) {
        reset();state.snapshot_mutate=2;state.mutation=i;
        probe(i==25 ? SlotReadFailed : SlotLayoutFailed,0);Check(!state.live);
    }
    reset();unsigned char data[128];
    Check(!slot_read(0,data,1));Check(!slot_read(UINT64_MAX,data,1));
    Check(!slot_read(PebAddress,data,0));Check(!slot_read(PebAddress,data,129));
    Check(!slot_read(UINT64_C(0x7fffffffffff),data,2));Check(!slot_read(ExpansionAddress+0x4000,data,8));
    reset();probe(SlotOk,0);unsigned total_reads=state.reads;
    for(unsigned short_read=0;short_read<2;++short_read)for(unsigned i=1;i<=total_reads;++i) {
        reset();if(short_read)state.read_short=i;else state.read_fail=i;
        probe(SlotReadFailed,0);Check(!state.live);
    }
}
static void token_and_failure_cases(void) {
    const unsigned tokens[]={1,0,SlotTlsCount,UINT32_MAX-1};
    for(unsigned i=0;i<sizeof tokens/sizeof tokens[0];++i) {
        reset();state.token_at=3;state.token_value=tokens[i];probe(SlotTokenFailed,1);Check(!state.live);
    }
    const DWORD errors[]={0,5,8,259};
    for(unsigned i=0;i<sizeof errors/sizeof errors[0];++i)for(unsigned at=1;at<=4;at+=3) {
        reset();state.alloc_fail=at;state.fail_error=errors[i];
        probe(errors[i]!=8 ? SlotFailureStatus : (at==1 ? SlotNoOwnedToken : SlotBitmapFailed),0);Check(!state.live);
    }
    reset();for(unsigned i=0;i<SlotTlsWords;++i)set_word(i,UINT32_MAX);
    probe(SlotNoOwnedToken,0);
    reset();state.fail_error=259;probe(SlotFailureStatus,0);Check(!state.live);
    const unsigned bad_mutations[]={13,15,16,18};
    for(unsigned i=0;i<sizeof bad_mutations/sizeof bad_mutations[0];++i) {
        reset();state.failure_mutation=bad_mutations[i];probe(SlotBitmapFailed,0);Check(!state.live);
    }
    reset();state.alloc_fail=3;state.failure_mutation=17;probe(SlotBitmapFailed,0);Check(!state.live);
    reset();put64(state.teb+0x1780,ExpansionAddress+0x1000);probe(SlotBitmapFailed,0);Check(!state.live);
}
static void refill_and_retirement_cases(void) {
    reset();state.alloc_fail=1088;probe(SlotRefillFailed,0);Check(!state.live);
    reset();state.token_at=1088;state.token_value=0;probe(SlotTokenFailed,1);Check(!state.live);
    reset();state.second_error=5;probe(SlotFailureStatus,0);Check(!state.live);
    reset();state.token_at=1089;state.token_value=1;probe(SlotTokenFailed,1);Check(!state.live);
    reset();state.snapshot_mutate=3;state.mutation=14;probe(SlotBitmapFailed,0);Check(!state.live);
    reset();state.snapshot_mutate=3;state.mutation=16;probe(SlotBitmapFailed,0);Check(!state.live);
    reset();state.snapshot_mutate=4;state.mutation=14;probe(SlotBitmapFailed,0);Check(!state.live);
    reset();state.snapshot_mutate=4;state.mutation=16;probe(SlotBitmapFailed,0);Check(!state.live);
    reset();state.snapshot_mutate=5;state.mutation=15;probe(SlotBitmapFailed,0);Check(!state.live);
    for(unsigned at=1;at<=1087;at+=543) {
        reset();state.free_fail=at;probe(SlotRetirementFailed,1);
        Check(state.live==(at==1 ? 0u : 1u));state.free_fail=0;
        for(unsigned i=0;i<SlotTlsCount;++i)if(state.owned[i])Check(TlsFree(i));
        Check(!state.live);
    }
    /* Direct comparisons expose every independent identity guard. */
    reset();slot_snapshot_t base={0},now={0};uint32_t owned[SlotTlsWords]={0};
    Check(slot_snapshot(&base)==SlotOk);now=base;Check(slot_matches(&base,&now,owned,0));
    now.teb++;Check(!slot_matches(&base,&now,owned,0));now=base;
    now.peb++;Check(!slot_matches(&base,&now,owned,0));now=base;
    now.descriptor[0]++;Check(!slot_matches(&base,&now,owned,0));now=base;
    now.descriptor[1]++;Check(!slot_matches(&base,&now,owned,0));now=base;
    now.expansion=ExpansionAddress;Check(!slot_matches(&base,&now,owned,1));
    base.expansion=ExpansionAddress+0x1000;Check(!slot_matches(&base,&now,owned,0));
}
int main(void) {
    positive_cases();admission_and_layout_cases();token_and_failure_cases();refill_and_retirement_cases();
    Check(!state.live && !state.borrowed_frees);
    printf("TLS_SLOT_CERTIFICATE_TEST PASS groups=4 cases=%u checks=%u owned=0 borrowed_frees=0\n",cases,checks);
    return 0;
}

#endif
