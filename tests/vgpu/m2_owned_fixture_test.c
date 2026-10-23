/** @file m2_owned_fixture_test.c @brief Actual fixture controller over Win32 doubles.
 * No Windows/GPU execution. Assertions stay active with optimization. Sole test
 * thread owns all synthetic state; no allocation or real native handles exist.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "m2_ownership_probe.h"
/* Exact boundary widths/signatures needed by the included Windows controller. */
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *HWND;
typedef void *HDC;
typedef void *HMODULE;
typedef void *HMENU;
typedef void *HINSTANCE;
typedef void (*FARPROC)(void);
#define MAX_PATH 260
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4u
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2u
#define _stricmp strcasecmp
static const char Candidate[]="C:\\M2\\waddle_vulkan_experimental.dll";
static const uintptr_t Module=0x1000, Window=0x2000, Dc=0x3000;
static unsigned checks, api_calls[4], query_calls[2], bind_fault, export_calls;
static int create_ok=1, destroy_ok=1, get_ok=1, release_ok=1, query_ok[2]={1,1};
static m2_icd_snapshot_t query_icd;
static m2_owner_snapshot_t query_dc;
#define Check(value) do { ++checks; if (!(value)) { fprintf(stderr,"M2 controller check failed line=%d: %s\n",__LINE__,#value); exit(1); } } while(0)
static int fake_icd_query(uint32_t version,m2_icd_snapshot_t *out,size_t bytes) {
    ++query_calls[0];Check(version==1 && bytes==144 && out);*out=query_icd;return query_ok[0];
}
static int fake_dc_query(uint32_t version,m2_owner_snapshot_t *out,size_t bytes) {
    ++query_calls[1];Check(version==1 && bytes==72 && out);*out=query_dc;return query_ok[1];
}
static DWORD GetFullPathNameA(const char *path,DWORD capacity,char *out,char **part) {
    Check(capacity==MAX_PATH && !part && path);strcpy(out,Candidate);
    if(bind_fault==1)return 0;
    if(bind_fault==2)return MAX_PATH;
    return (DWORD)strlen(out);
}
static HMODULE GetModuleHandleA(const char *path) { Check(!strcmp(path,Candidate));return bind_fault==3 ? NULL : (HMODULE)Module; }
static DWORD GetModuleFileNameA(HMODULE module,char *out,DWORD capacity) {
    Check(module==(HMODULE)Module && capacity==MAX_PATH);strcpy(out,bind_fault==6 ? "C:\\other.dll" : Candidate);
    if(bind_fault==4)return 0;
    if(bind_fault==5)return MAX_PATH;
    return (DWORD)strlen(out);
}
static FARPROC GetProcAddress(HMODULE module,const char *name) {
    Check(module==(HMODULE)Module);++export_calls;
    if((bind_fault==7 && export_calls==1) || (bind_fault==8 && export_calls==2))return NULL;
    FARPROC result=NULL;
    if(!strcmp(name,"venus_icd_m2_snapshot")){int (*fn)(uint32_t,m2_icd_snapshot_t*,size_t)=fake_icd_query;memcpy(&result,&fn,sizeof result);}
    else {Check(!strcmp(name,"venus_m2_dc_snapshot"));int (*fn)(uint32_t,m2_owner_snapshot_t*,size_t)=fake_dc_query;memcpy(&result,&fn,sizeof result);}
    return result;
}
static BOOL GetModuleHandleExA(DWORD flags,const char *address,HMODULE *module) {
    Check(flags==6 && address && module);*module=(HMODULE)(Module+(bind_fault==10));return bind_fault!=9;
}
static HWND CreateWindowExA(DWORD extended,const char *class_name,const char *title,DWORD style,
    int x,int y,int width,int height,HWND parent,HMENU menu,HINSTANCE instance,void *parameter) {
    Check(extended==0 && !strcmp(class_name,"STATIC") && !strcmp(title,"test") && style==0 &&
          x==1 && y==2 && width==3 && height==4 && !parent && !menu && !instance && !parameter);
    ++api_calls[0];return create_ok ? (HWND)Window : NULL;
}
static BOOL DestroyWindow(HWND window) { Check(window==(HWND)Window);++api_calls[1];return destroy_ok; }
static HDC GetDC(HWND window) { Check(window==(HWND)Window);++api_calls[2];return get_ok ? (HDC)Dc : NULL; }
static int ReleaseDC(HWND window,HDC dc) { Check(window==(HWND)Window && dc==(HDC)Dc);++api_calls[3];return release_ok; }
#include "tcp_gpu_m2_owned_windows.inc"

static void reset(void) {
    m2_owned=(m2_owned_fixture_t){0};bind_fault=0;export_calls=0;
    create_ok=destroy_ok=get_ok=release_ok=1;query_ok[0]=query_ok[1]=1;
    memset(api_calls,0,sizeof api_calls);memset(query_calls,0,sizeof query_calls);
    query_icd=(m2_icd_snapshot_t){.version=1,.bytes=144,.binding=1,.object_next_id=UINT64_C(0x100000001),.wsi_next_id=UINT64_C(0x100000001)};
    query_dc=(m2_owner_snapshot_t){.version=1,.bytes=72,.next_id=1};
}
static HWND create_window(void) { return m2_owned_create_window(0,"STATIC","test",0,1,2,3,4,NULL,NULL,NULL,NULL); }
static void begin(void) { reset();Check(m2_owned_begin(Candidate,UINT64_C(0x1234567890abcdef))); }
static void one_lifetime(void) {
    HWND window=create_window();Check(window==(HWND)Window);
    HDC dc=m2_owned_get_dc(window);Check(dc==(HDC)Dc);
    Check(m2_owned_release_dc(window,dc));Check(m2_owned_destroy_window(window));
    ++query_icd.object_next_id;++query_icd.wsi_next_id;
    ++query_dc.next_id;++query_dc.acquired;++query_dc.retired;
}
static int boundary(unsigned lifetime) { return m2_owned_boundary(UINT64_C(0x1234567890abcdef),"wsi_lifetime",lifetime,200+lifetime); }
static void binding_controls(void) {
    reset();Check(!m2_owned_begin(NULL,1));Check(!m2_owned_begin(Candidate,0));Check(!m2_owned_begin("",1));
    char long_path[MAX_PATH+1];memset(long_path,'x',MAX_PATH);long_path[MAX_PATH]=0;Check(!m2_owned_begin(long_path,1));
    Check(!m2_owned_begin("C:\\bad\n",1));Check(!m2_owned_begin("C:\\bad\177",1));Check(!m2_owned_begin("relative.dll",1));
    for(unsigned fault=1;fault<=10;fault++){reset();bind_fault=fault;Check(!m2_owned_begin(Candidate,1));Check(!m2_owned.enabled);}
    reset();FARPROC output=NULL;Check(!m2_owned_export((HMODULE)Module,"venus_icd_m2_snapshot",&output,0));Check(!output);
    begin();Check(!m2_owned_begin(Candidate,1));Check(m2_owned.session==UINT64_C(0x1234567890abcdef));
}
static void wrapper_controls(void) {
    reset();HWND window=create_window();HDC dc=m2_owned_get_dc(window);Check(m2_owned_release_dc(window,dc));Check(m2_owned_destroy_window(window));
    Check(!m2_owned.windows.next_id && !m2_owned.dcs.next_id);
    begin();one_lifetime();Check(api_calls[0]==1 && api_calls[1]==1 && api_calls[2]==1 && api_calls[3]==1);
    begin();create_ok=0;Check(!create_window());Check(m2_owned.windows.next_id==2 && !m2_owned.windows.acquired);
    get_ok=0;Check(!m2_owned_get_dc((HWND)Window));Check(m2_owned.dcs.next_id==2 && !m2_owned.dcs.acquired);
    begin();m2_owned.windows.failures=1;m2_owned.dcs.failures=1;Check(!create_window());Check(!m2_owned_get_dc((HWND)Window));Check(!api_calls[0] && !api_calls[2]);
    begin();Check(!m2_owned_destroy_window((HWND)Window));Check(!m2_owned_release_dc((HWND)Window,(HDC)Dc));Check(!api_calls[1] && !api_calls[3]);
    begin();window=create_window();dc=m2_owned_get_dc(window);destroy_ok=release_ok=0;
    Check(!m2_owned_release_dc(window,dc));Check(!m2_owned_destroy_window(window));
    Check(m2_owned.windows.acquired==1 && !m2_owned.windows.retired && m2_owned.windows.failures==8);
    Check(m2_owned.dcs.acquired==1 && !m2_owned.dcs.retired && m2_owned.dcs.failures==8);
    destroy_ok=release_ok=1;Check(m2_owned_release_dc(window,dc));Check(m2_owned_destroy_window(window));
    Check(m2_owned.windows.failures==8 && m2_owned.dcs.failures==8);
    for(int failed=0;failed<2;failed++) {
        begin();window=create_window();dc=m2_owned_get_dc(window);destroy_ok=release_ok=!failed;
        Check(!create_window());Check(!m2_owned_get_dc(window));Check(api_calls[0]==2 && api_calls[1]==1 && api_calls[2]==2 && api_calls[3]==1);
        Check(m2_owned.windows.acquired==2 && m2_owned.windows.retired==(uint64_t)!failed);
        Check(m2_owned.dcs.acquired==2 && m2_owned.dcs.retired==(uint64_t)!failed);
        Check(m2_owned.windows.failures && m2_owned.dcs.failures);
    }
    /* A full ledger rejects before acquiring another native identity. */
    begin();for(unsigned i=0;i<M2OwnerCapacity;i++)Check(m2_owner_reserve(&m2_owned.windows,M2OwnerWindow,0));
    Check(!create_window());Check(!api_calls[0]);
    begin();for(unsigned i=0;i<M2OwnerCapacity;i++)Check(m2_owner_reserve(&m2_owned.dcs,M2OwnerDc,Window));
    Check(!m2_owned_get_dc((HWND)Window));Check(!api_calls[2]);
}
static void predicate_controls(void) {
    reset();Check(m2_owned_icd_clean(&query_icd));
    for(unsigned i=0;i<6;i++) {
        m2_icd_snapshot_t value=query_icd;uint32_t replacement=i==5 ? 0 : 2;
        memcpy((char*)&value+i*4,&replacement,4);Check(!m2_owned_icd_clean(&value));
    }
    for(unsigned i=0;i<13;i++) {
        m2_icd_snapshot_t value=query_icd;uint64_t replacement=1;
        memcpy((char*)&value+40+i*8,&replacement,8);Check(!m2_owned_icd_clean(&value));
    }
    const uint64_t bad_ids[]={0,1,UINT64_C(0x100000000),UINT64_C(0x200000001)};
    for(unsigned i=0;i<4;i++) {
        m2_icd_snapshot_t value=query_icd;value.object_next_id=bad_ids[i];Check(!m2_owned_icd_clean(&value));
        value=query_icd;value.wsi_next_id=bad_ids[i];Check(!m2_owned_icd_clean(&value));
    }
    m2_owner_snapshot_t old=query_dc,current=query_dc;
    Check(!m2_owned_advanced(&current,&old));current.acquired=1;Check(!m2_owned_advanced(&current,&old));
    current.next_id=2;Check(m2_owned_advanced(&current,&old));current.acquired=2;Check(!m2_owned_advanced(&current,&old));
}
static void boundary_controls(void) {
    reset();Check(!boundary(0));begin();Check(!boundary(1));Check(!boundary(9));
    Check(!m2_owned_boundary(1,"wsi_lifetime",0,1));Check(!m2_owned_boundary(m2_owned.session,"compute",0,1));
    Check(!m2_owned_boundary(m2_owned.session,"wsi_lifetime",0,0));
    for(unsigned i=0;i<2;i++){begin();query_ok[i]=0;Check(!boundary(0));Check(!m2_owned.initialized);}
    begin();query_ok[0]=2;Check(!boundary(0));
    begin();Check(boundary(0));Check(!boundary(0));Check(!boundary(2));Check(!boundary(1));Check(m2_owned.lifetime==0);
    begin();query_icd.objects=1;Check(!boundary(0));Check(query_icd.objects==1);
    begin();query_dc.live=1;Check(!boundary(0));
    begin();(void)m2_owned_get_dc((HWND)Window);Check(!boundary(0));
    begin();(void)create_window();Check(!boundary(0));
    for(unsigned which=0;which<6;which++) {
        begin();Check(boundary(0));one_lifetime();
        if(which==0){query_icd.object_next_id+=UINT64_C(1)<<32;query_icd.wsi_next_id+=UINT64_C(1)<<32;}
        if(which==1)query_icd.object_next_id=m2_owned.previous_icd.object_next_id;
        if(which==2)query_icd.wsi_next_id=m2_owned.previous_icd.wsi_next_id;
        if(which==3)query_dc=m2_owned.previous_sink;
        if(which==4)m2_owned.dcs=(m2_owner_ledger_t){.next_id=1};
        if(which==5)m2_owned.windows=(m2_owner_ledger_t){.next_id=1};
        Check(!boundary(1));Check(m2_owned.lifetime==0);
    }
    begin();Check(boundary(0));for(unsigned i=1;i<=8;i++){one_lifetime();Check(boundary(i));}Check(!boundary(9));
}
/** @brief Emit synthetic C-serialized records for an independent Python consumer.
 * @param[in] workload Supported literal. @return Zero synthetic success. No GPU.
 */
static int emit_workload(const char *workload) {
    const char *labels[]={"failed-replacement-retained-acquired-image","zero-area-sticky-out-of-date-replacement-and-restore",
        "resize-capabilities-out-of-date-and-replacements","destroyed-HWND-surface-loss-and-retirement-precedence"};
    begin();printf("Authenticated native GPU session=1234567890abcdef workload=%s public_api=1.0\n",workload);
    puts("Native repeated application handle baseline=199");Check(m2_owned_boundary(m2_owned.session,workload,0,199));
    for(unsigned i=1;i<=8;i++) {
        if(!strcmp(workload,"triangle_present"))for(unsigned mode=0;mode<2;mode++)for(unsigned frame=1;frame<=3;frame++)
            printf("Native Vulkan actual swapchain mode=%s frame=%u exact4096RGB/BGRA PASS\n",mode ? "IMMEDIATE" : "FIFO",frame);
        else for(unsigned label=0;label<4;label++)printf("Native WSI lifetime case=%s PASS\n",labels[label]);
        one_lifetime();Check(m2_owned_boundary(m2_owned.session,workload,i,199+i));
        printf("Native actual %s lifetime=%u device=NVIDIA GeForce RTX 5080 exact-output PASS\n",workload,i);
    }
    puts("Actual native GPU outputs and matching retirement Ack PASS; independent host result still required");return 0;
}
/** @brief Run four API-double groups, or explicit synthetic serialization mode.
 * @return Zero all assertions pass, one invalid invocation/failure. No native run.
 */
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--emit-workload") && (!strcmp(argv[2],"triangle_present") || !strcmp(argv[2],"wsi_lifetime")))return emit_workload(argv[2]);
    if(argc!=1)return 1;
    binding_controls();wrapper_controls();predicate_controls();boundary_controls();
    printf("M2 owned fixture 4 groups %u checks PASS (API doubles only)\n",checks);return 0;
}
