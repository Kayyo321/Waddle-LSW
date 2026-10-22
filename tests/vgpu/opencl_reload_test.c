/** @file opencl_reload_test.c @brief Actual reload support with Win32 API doubles.
 * Host pthreads exercise real ready/publication/join sequencing; there is no
 * Windows loader, OpenCL, native GPU or physical acceptance claim.
 */
#define _GNU_SOURCE
#include <assert.h>
#ifdef NDEBUG
#error Assertion-elided ownership controls are forbidden
#endif
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "win32_fake/opencl_reload_fake.h"

enum { FakeNone,FakeEvent,FakeThread,FakeSetReady,FakeSetGo,FakeWaitReady,
       FakeWaitThread,FakeExitCode,FakeClose,FakeTlsFree,FakeFlsFree,FakeHandles,
       FakeTime,FakeLoad,FakeFreeModule,FakeTlsAlloc,FakeFlsAlloc,FakeKinds };
typedef struct fake_handle_t {
    int thread,go,signalled,joined;
    pthread_mutex_t lock;pthread_cond_t condition;pthread_t native;
    DWORD (*entry)(void *);void *argument;DWORD result;
} fake_handle_t;
static fake_handle_t *fake_owners[128];
static pthread_mutex_t fake_owner_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint fake_calls[FakeKinds],fake_kind,fake_ordinal,fake_next_tid;
static _Thread_local DWORD fake_tid,fake_last_error;
static DWORD fake_allocation_error=ERROR_NOT_ENOUGH_MEMORY;
static void SetLastError(DWORD error) { fake_last_error=error; }
static DWORD GetLastError(void) { return fake_last_error; }
static unsigned fake_tls_limit=8,fake_fls_limit=12;
static unsigned char fake_tls[8192],fake_fls[8192];
static int fake_duplicate_tid,fake_zero_tid,fake_module_mode,fake_handle_delta,fake_module_live;
static unsigned fake_alloc_count,fake_close_count;
static jmp_buf fake_fatal_target;
static int fake_fatal_expected,fake_fatal_seen;
static unsigned checks,groups;
static FILE *fake_output;
static char fake_diagnostics[8192];
static unsigned fake_diagnostic_count;
#define Check(value) do { ++checks;assert(value); } while(0)
static int fake_fault(unsigned kind) {
    unsigned call=atomic_fetch_add(&fake_calls[kind],1)+1;
    return atomic_load(&fake_kind)==kind && call==atomic_load(&fake_ordinal);
}
static unsigned fake_live(void) {
    unsigned count=0;pthread_mutex_lock(&fake_owner_lock);
    for(unsigned i=0;i<128;++i)count+=fake_owners[i]!=NULL;
    pthread_mutex_unlock(&fake_owner_lock);return count;
}
static fake_handle_t *fake_new(void) {
    fake_handle_t *handle=calloc(1,sizeof *handle);assert(handle);
    assert(!pthread_mutex_init(&handle->lock,NULL));assert(!pthread_cond_init(&handle->condition,NULL));
    pthread_mutex_lock(&fake_owner_lock);unsigned i=0;while(i<128 && fake_owners[i])++i;assert(i<128);
    fake_owners[i]=handle;++fake_alloc_count;pthread_mutex_unlock(&fake_owner_lock);return handle;
}
static BOOL CloseHandle(HANDLE raw) {
    if(fake_fault(FakeClose))return FALSE;
    fake_handle_t *handle=raw;assert(handle);assert(!handle->thread || handle->joined);
    pthread_mutex_lock(&fake_owner_lock);unsigned i=0;while(i<128 && fake_owners[i]!=handle)++i;assert(i<128);
    fake_owners[i]=NULL;++fake_close_count;pthread_mutex_unlock(&fake_owner_lock);
    assert(!pthread_cond_destroy(&handle->condition));assert(!pthread_mutex_destroy(&handle->lock));free(handle);return TRUE;
}
static DWORD GetCurrentThreadId(void) {
    if(fake_zero_tid)return 0;
    if(fake_duplicate_tid)return 77;
    if(!fake_tid)fake_tid=atomic_fetch_add(&fake_next_tid,1)+1;
    return fake_tid;
}
static BOOL SetEvent(HANDLE raw) {
    fake_handle_t *handle=raw;assert(handle && !handle->thread);
    if(fake_fault(handle->go ? FakeSetGo : FakeSetReady))return FALSE;
    pthread_mutex_lock(&handle->lock);handle->signalled=1;pthread_cond_broadcast(&handle->condition);
    pthread_mutex_unlock(&handle->lock);return TRUE;
}
static DWORD WaitForSingleObject(HANDLE raw,DWORD timeout) {
    fake_handle_t *handle=raw;assert(handle);
    if(handle->thread) {
        if(fake_fault(FakeWaitThread))return WAIT_TIMEOUT;
        if(!handle->joined){assert(!pthread_join(handle->native,NULL));handle->joined=1;}
    } else {
        if(!handle->go && fake_fault(FakeWaitReady))return WAIT_TIMEOUT;
        pthread_mutex_lock(&handle->lock);
        while(!handle->signalled) {
            if(timeout!=INFINITE) {
                struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));++deadline.tv_sec;
                int status=pthread_cond_timedwait(&handle->condition,&handle->lock,&deadline);
                if(status==ETIMEDOUT){pthread_mutex_unlock(&handle->lock);return WAIT_TIMEOUT;}
                assert(!status);
            } else assert(!pthread_cond_wait(&handle->condition,&handle->lock));
        }
        pthread_mutex_unlock(&handle->lock);
    }
    return WAIT_OBJECT_0;
}
static ULONGLONG GetTickCount64(void) { return 100; }
static BOOL GetExitCodeThread(HANDLE raw,DWORD *code) {
    if(fake_fault(FakeExitCode))return FALSE;
    fake_handle_t *handle=raw;assert(handle->joined);*code=handle->result;return TRUE;
}
static HANDLE CreateEventW(void *security,BOOL manual,BOOL initial,const wchar_t *name) {
    (void)security;(void)manual;(void)initial;(void)name;
    if(fake_fault(FakeEvent))return NULL;
    fake_handle_t *handle=fake_new();handle->go=(atomic_load(&fake_calls[FakeEvent])%2)==0;return handle;
}
static void *fake_thread_entry(void *opaque) {
    fake_handle_t *handle=opaque;handle->result=handle->entry(handle->argument);return NULL;
}
static HANDLE CreateThread(void *security,size_t stack,DWORD (*entry)(void *),void *argument,DWORD flags,DWORD *tid) {
    (void)security;(void)stack;(void)flags;(void)tid;
    if(fake_fault(FakeThread))return NULL;
    fake_handle_t *handle=fake_new();handle->thread=1;handle->entry=entry;handle->argument=argument;
    assert(!pthread_create(&handle->native,NULL,fake_thread_entry,handle));return handle;
}
static DWORD fake_index(unsigned char *slots,unsigned limit) {
    for(unsigned i=0;i<limit;++i)if(!slots[i]){slots[i]=1;return i;}
    SetLastError(ERROR_NO_MORE_ITEMS);return TLS_OUT_OF_INDEXES;
}
static DWORD TlsAlloc(void) {
    if(fake_fault(FakeTlsAlloc)){if(fake_allocation_error)SetLastError(fake_allocation_error);return TLS_OUT_OF_INDEXES;}
    return fake_index(fake_tls,fake_tls_limit);
}
static DWORD FlsAlloc(void *callback) {
    assert(!callback);
    if(fake_fault(FakeFlsAlloc)){if(fake_allocation_error)SetLastError(fake_allocation_error);return FLS_OUT_OF_INDEXES;}
    return fake_index(fake_fls,fake_fls_limit);
}
static BOOL TlsFree(DWORD index) { if(fake_fault(FakeTlsFree))return FALSE;assert(fake_tls[index]);fake_tls[index]=0;SetLastError(ERROR_SUCCESS);return TRUE; }
static BOOL FlsFree(DWORD index) { if(fake_fault(FakeFlsFree))return FALSE;assert(fake_fls[index]);fake_fls[index]=0;SetLastError(ERROR_SUCCESS);return TRUE; }
static HANDLE GetCurrentProcess(void) { return (HANDLE)(uintptr_t)1; }
static BOOL GetProcessHandleCount(HANDLE process,DWORD *count) {
    assert(process==GetCurrentProcess());if(fake_fault(FakeHandles))return FALSE;
    *count=50+fake_live()+(unsigned)fake_handle_delta;return TRUE;
}
static DWORD GetModuleFileNameW(HMODULE module,wchar_t *path,DWORD capacity) {
    assert(module);if(fake_module_mode==1)return 0;if(fake_module_mode==2)return capacity;
    const wchar_t *name=fake_module_mode==3 ? L"C:\\other.dll" : L"C:\\candidate\\OpenCL.dll";
    wcscpy(path,name);return (DWORD)wcslen(name);
}
static int _wcsicmp(const wchar_t *left,const wchar_t *right) {
    while(*left && towlower(*left)==towlower(*right)){++left;++right;}
    return (int)towlower(*left)-(int)towlower(*right);
}
static BOOL GetProcessTimes(HANDLE process,FILETIME *created,FILETIME *exited,FILETIME *kernel,FILETIME *user) {
    (void)process;(void)exited;(void)kernel;(void)user;
    if(fake_fault(FakeTime))return FALSE;
    created->dwLowDateTime=55;created->dwHighDateTime=1;return TRUE;
}
static DWORD GetCurrentProcessId(void) { return 1234; }
static HMODULE LoadLibraryW(const wchar_t *name) {
    assert(!wcscmp(name,L"msvcrt.dll"));if(fake_fault(FakeLoad))return NULL;
    ++fake_module_live;return (HMODULE)(uintptr_t)2;
}
static BOOL FreeLibrary(HMODULE module) {
    assert(module==(HMODULE)(uintptr_t)2);if(fake_fault(FakeFreeModule))return FALSE;
    assert(fake_module_live);--fake_module_live;return TRUE;
}
static void fake_cleanup(void) {
    atomic_store(&fake_kind,FakeNone);
    for(unsigned i=0;i<128;++i)if(fake_owners[i] && !fake_owners[i]->thread)SetEvent(fake_owners[i]);
    for(unsigned i=0;i<128;++i)if(fake_owners[i] && fake_owners[i]->thread)WaitForSingleObject(fake_owners[i],INFINITE);
    for(unsigned i=0;i<128;++i)if(fake_owners[i])CloseHandle(fake_owners[i]);
    memset(fake_tls,0,sizeof fake_tls);memset(fake_fls,0,sizeof fake_fls);fake_module_live=0;
}
static _Noreturn void fail_closed(const char *message) {
    assert(fake_fatal_expected);assert(strstr(message,"OpenCL reload fatal:"));fake_fatal_seen=1;
    fake_cleanup();if(fake_output){fclose(fake_output);fake_output=NULL;}longjmp(fake_fatal_target,1);
}
/* Copy only the new failure record while retaining ordinary FILE output. No
 * Win32 ownership/count/query behavior is replaced by this observer. */
static int fake_fprintf(FILE *output,const char *format,...) {
    va_list arguments;va_start(arguments,format);
    if(output==stderr && !strncmp(format,"OPENCL_RELOAD_DIAG ",19)) {
        va_list copy;va_copy(copy,arguments);
        size_t used=strlen(fake_diagnostics);
        int size=vsnprintf(fake_diagnostics+used,sizeof fake_diagnostics-used,format,copy);
        va_end(copy);assert(size>=0 && (size_t)size<sizeof fake_diagnostics-used);
        ++fake_diagnostic_count;
    }
    int result=vfprintf(output,format,arguments);va_end(arguments);return result;
}
#define fprintf fake_fprintf
#include "opencl_reload_windows.inc"
#undef fprintf

static void reset(void) {
    Check(fake_live()==0);Check(fake_module_live==0);
    for(unsigned i=0;i<8192;++i){assert(!fake_tls[i]);assert(!fake_fls[i]);}
    for(unsigned i=0;i<FakeKinds;++i)atomic_store(&fake_calls[i],0);
    atomic_store(&fake_kind,FakeNone);atomic_store(&fake_ordinal,1);atomic_store(&fake_next_tid,100);
    fake_tls_limit=8;fake_fls_limit=12;fake_duplicate_tid=fake_zero_tid=fake_module_mode=fake_handle_delta=0;
    fake_alloc_count=fake_close_count=0;fake_fatal_seen=fake_fatal_expected=0;
    fake_allocation_error=ERROR_NOT_ENOUGH_MEMORY;SetLastError(ERROR_SUCCESS);
    fake_diagnostics[0]='\0';fake_diagnostic_count=0;
}
static void fault(unsigned kind,unsigned ordinal) { atomic_store(&fake_kind,kind);atomic_store(&fake_ordinal,ordinal); }
static int callback(void *opaque) { return *(int *)opaque; }
static void test_pools(void) {
    ++groups;reset();reload_pool_t pool={0};int result=0;
    Check(reload_pool_start(&pool,0)==1);Check(reload_pool_start(&pool,5)==1);
    Check(!reload_pool_start(&pool,2));Check(pool.slots[0].tid!=pool.slots[1].tid);
    Check(!reload_pool_call(&pool,callback,&result));Check(fake_alloc_count==fake_close_count);
    reset();memset(&pool,0,sizeof pool);result=7;Check(!reload_pool_start(&pool,2));Check(reload_pool_call(&pool,callback,&result)==1);
    for(unsigned ordinal=1;ordinal<=4;++ordinal){reset();memset(&pool,0,sizeof pool);fault(FakeEvent,ordinal);Check(reload_pool_start(&pool,2)==1);Check(!fake_live());}
    for(unsigned ordinal=1;ordinal<=2;++ordinal){reset();memset(&pool,0,sizeof pool);fault(FakeThread,ordinal);Check(reload_pool_start(&pool,2)==1);Check(!fake_live());}
    reset();memset(&pool,0,sizeof pool);fault(FakeWaitReady,1);Check(reload_pool_start(&pool,2)==1);Check(!fake_live());
    reset();memset(&pool,0,sizeof pool);fake_duplicate_tid=1;Check(reload_pool_start(&pool,2)==1);
    reset();memset(&pool,0,sizeof pool);fake_zero_tid=1;Check(reload_pool_start(&pool,1)==1);
    reset();memset(&pool,0,sizeof pool);fault(FakeSetReady,1);Check(reload_pool_start(&pool,1)==1);
    reset();memset(&pool,0,sizeof pool);Check(!reload_pool_start(&pool,1));fault(FakeExitCode,1);Check(reload_pool_finish(&pool)==1);
}
static void test_probes(void) {
    ++groups;reset();reload_counts_t a,b;Check(!reload_measure(&a));Check(a.tls==8 && a.fls==12 && a.handles==50);
    Check(!reload_control(0));Check(!reload_control(1));Check(!reload_measure(&b));Check(reload_equal(&a,&b));
    b.handles++;Check(!reload_equal(&a,&b));b=a;b.tls--;Check(!reload_equal(&a,&b));b=a;b.fls--;Check(!reload_equal(&a,&b));
    unsigned count=0;fake_tls_limit=ReloadTlsCapacity;Check(reload_indices(0,&count)==1);Check(count==ReloadTlsCapacity);
    fake_fls_limit=ReloadFlsCapacity;Check(reload_indices(1,&count)==1);Check(count==ReloadFlsCapacity);
    /* Unexpected errors at zero/partial success must reject and retire every
     * acquired index. A stale exhaustion status cannot mask an unset error. */
    const DWORD errors[]={ERROR_SUCCESS,ERROR_NOT_ENOUGH_MEMORY,5};
    for(unsigned fls=0;fls<2;++fls)for(unsigned partial=0;partial<2;++partial)
        for(unsigned e=0;e<sizeof errors/sizeof errors[0];++e) {
            reset();fault(fls ? FakeFlsAlloc : FakeTlsAlloc,partial ? 4 : 1);
            fake_allocation_error=errors[e];SetLastError(ERROR_NO_MORE_ITEMS);
            count=99;Check(reload_indices((int)fls,&count)==1);Check(count==(partial ? 3u : 0u));
            for(unsigned i=0;i<8192;++i){assert(!fake_tls[i]);assert(!fake_fls[i]);}
            Check(!fake_live());
        }
    for(unsigned fls=0;fls<2;++fls) {
        reset();if(fls)fake_fls_limit=0;else fake_tls_limit=0;
        Check(reload_indices((int)fls,&count)==0);Check(count==0);
        reset();fault(fls ? FakeFlsAlloc : FakeTlsAlloc,1);Check(reload_control((int)fls)==1);
        reset();fault(fls ? FakeFlsAlloc : FakeTlsAlloc,fls ? 14 : 10);Check(reload_control((int)fls)==1);
    }
    reset();fault(FakeHandles,1);Check(reload_measure(&a)==1);
    reset();fake_tls_limit=0;Check(reload_control(0)==1);reset();fake_fls_limit=0;Check(reload_control(1)==1);
    reset();fault(FakeHandles,2);Check(reload_control(0)==1);reset();fault(FakeHandles,3);Check(reload_control(1)==1);
    reset();fault(FakeHandles,1);Check(reload_control(0)==1);
    reset();for(fake_module_mode=0;fake_module_mode<4;++fake_module_mode)
        Check(reload_module_exact((HMODULE)(uintptr_t)3,L"c:\\candidate\\OPENCL.DLL")==!fake_module_mode);
}
typedef struct run_state_t { unsigned cycles,stage_calls,absent_calls,preinit_calls;int bad_cycle,bad_stage,bad_absent,bad_preinit;
    const char *record;int custom_record; } run_state_t;
static const char FakePreinitRecord[]="user32,winsock,bcrypt,dxgi_factory,search_order,package_family";
static char fake_run_head[4096];
static int cycle_call(void *opaque,reload_cycle_result_t *result) {
    run_state_t *state=opaque;++state->cycles;
    assert(fake_live()==ReloadIdleThreads*3);assert(fake_module_live==1);
    *result=(reload_cycle_result_t){1000+state->cycles,{501,502},1,1};
    switch(state->bad_cycle) {
    case 1:return 1;case 2:result->session=0;break;case 3:result->foreign[0]=0;break;
    case 4:result->foreign[1]=0;break;case 5:result->foreign[1]=501;break;
    case 6:result->module_exact=0;break;case 7:result->modules_absent=0;break;
    case 8:fake_handle_delta=1;break;case 9:(void)TlsAlloc();break;case 10:(void)FlsAlloc(NULL);break;
    default:break;
    }
    return 0;
}
static int absent_call(void *opaque) { run_state_t *state=opaque;return ++state->absent_calls!=(unsigned)state->bad_absent; }
static int stage_call(void *opaque,const char *stage) {
    run_state_t *state=opaque;++state->stage_calls;
    Check(!strncmp(stage,"reload_",7));return state->stage_calls==(unsigned)state->bad_stage;
}
/** Pre-initialization must run before any measurement, idle thread or module reference. */
static int preinit_call(void *opaque) {
    run_state_t *state=opaque;++state->preinit_calls;
    Check(atomic_load(&fake_calls[FakeHandles])==0);Check(atomic_load(&fake_calls[FakeTlsAlloc])==0);
    Check(atomic_load(&fake_calls[FakeFlsAlloc])==0);Check(atomic_load(&fake_calls[FakeTime])==0);
    Check(!fake_live());Check(!fake_module_live);Check(state->cycles==0 && state->stage_calls==0);
    return state->bad_preinit;
}
static int run_case(run_state_t *state,const wchar_t *path) {
    reload_api_t api={cycle_call,absent_call,stage_call,preinit_call,state->custom_record ? state->record : FakePreinitRecord,state};
    FILE *output=tmpfile();assert(output);
    fake_output=output;int result=reload_run(&api,path,output);
    rewind(output);size_t got=fread(fake_run_head,1,sizeof fake_run_head-1,output);fake_run_head[got]='\0';
    assert(!fclose(output));fake_output=NULL;return result;
}
static void test_run(void) {
    ++groups;reset();run_state_t state={0};Check(!run_case(&state,L"C:\\candidate\\OpenCL.dll"));Check(state.cycles==1100);Check(state.stage_calls==2201);Check(!fake_live());
    Check(state.preinit_calls==1);
    Check(!strncmp(fake_run_head,"OPENCL_RELOAD_V1 {\"event\":\"baseline\"",33));
    Check(strstr(fake_run_head,",\"os_preinit\":\"user32,winsock,bcrypt,dxgi_factory,search_order,package_family\",\"module_utf16\":\"")!=NULL);
    const wchar_t *invalid[]={L"",L"C:",L"relative",L"C:/bad"};
    for(unsigned i=0;i<4;++i){reset();state=(run_state_t){0};Check(run_case(&state,invalid[i])==1);Check(state.cycles==0);}
    wchar_t *long_path=calloc(ReloadPathCapacity+1,sizeof *long_path);assert(long_path);
    for(unsigned i=0;i<ReloadPathCapacity;++i)long_path[i]=L'x';
    reset();state=(run_state_t){0};Check(run_case(&state,long_path)==1);free(long_path);
    for(int bad=1;bad<=10;++bad){reset();state=(run_state_t){.bad_cycle=bad};Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);Check(state.cycles==1);fake_cleanup();}
    for(int bad=1;bad<=3;++bad){reset();state=(run_state_t){.bad_absent=bad};Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);}
    for(int bad=1;bad<=2;++bad){reset();state=(run_state_t){.bad_stage=bad};Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);}
    reset();state=(run_state_t){.bad_stage=2201};Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);Check(state.cycles==1100);
    const unsigned failures[]={FakeTime,FakeLoad,FakeEvent,FakeThread};
    for(unsigned i=0;i<sizeof failures/sizeof failures[0];++i){reset();state=(run_state_t){0};fault(failures[i],1);Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);}
    for(unsigned ordinal=1;ordinal<=5;++ordinal){reset();state=(run_state_t){0};fault(FakeHandles,ordinal);Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);}
}
static void test_fatal(void) {
    ++groups;
    const unsigned faults[]={FakeSetGo,FakeWaitThread,FakeClose,FakeTlsFree,FakeFlsFree,FakeFreeModule};
    for(unsigned i=0;i<sizeof faults/sizeof faults[0];++i) {
        reset();fake_fatal_expected=1;
        if(!setjmp(fake_fatal_target)) {
            fault(faults[i],1);
            if(faults[i]==FakeTlsFree || faults[i]==FakeFlsFree){unsigned count;reload_indices(faults[i]==FakeFlsFree,&count);}
            else if(faults[i]==FakeFreeModule){run_state_t state={.bad_stage=1};(void)run_case(&state,L"C:\\candidate\\OpenCL.dll");}
            else { reload_pool_t pool={0};Check(!reload_pool_start(&pool,1));(void)reload_pool_finish(&pool); }
            assert(!"fatal failed to terminate ownership path");
        }
        Check(fake_fatal_seen);Check(!fake_live());
    }
}
static void test_failure_diagnostics(void) {
    ++groups;reset();run_state_t state={0};
    Check(!run_case(&state,L"C:\\candidate\\OpenCL.dll"));
    Check(!fake_diagnostic_count);Check(!fake_diagnostics[0]);
    const char *predicates[]={"api->cycle(api->context,&result)","!result.session",
        "!result.foreign[0]","!result.foreign[1]","result.foreign[0]==result.foreign[1]",
        "result.module_exact!=1","result.modules_absent!=1",
        "!reload_equal(&baseline,&observed)","!reload_equal(&baseline,&observed)",
        "!reload_equal(&baseline,&observed)"};
    for(int bad=1;bad<=10;++bad) {
        reset();state=(run_state_t){.bad_cycle=bad};
        Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
        Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=cycle cycle=1 predicate="));
        Check(strstr(fake_diagnostics,predicates[bad-1]));
        Check(strstr(fake_diagnostics,"raw_snapshots=1 entry=[50,8,12] baseline=[62,8,12]"));
        if(bad==8)Check(strstr(fake_diagnostics,"observed=[63,8,12]"));
        if(bad==9)Check(strstr(fake_diagnostics,"observed=[62,7,12]"));
        if(bad==10)Check(strstr(fake_diagnostics,"observed=[62,8,11]"));
        if(bad>=8)Check(strstr(fake_diagnostics,"session=00000000000003e9 foreign=[501,502] module_exact=1 modules_absent=1"));
        Check(state.cycles==1);Check(state.stage_calls==1);Check(!fake_live());fake_cleanup();
    }
    reset();state=(run_state_t){.bad_stage=1};
    Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
    Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=cycle cycle=1 predicate=api->stage(api->context,stage)"));
    Check(strstr(fake_diagnostics,"session=0000000000000000 foreign=[0,0] module_exact=0 modules_absent=0"));
    Check(state.cycles==0);Check(atomic_load(&fake_calls[FakeHandles])==3);
    reset();state=(run_state_t){.bad_absent=3};
    Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
    Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=cycle cycle=1 predicate=!api->absent(api->context)"));
    Check(atomic_load(&fake_calls[FakeHandles])==3);
    reset();state=(run_state_t){0};fault(FakeHandles,4);
    Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
    Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=cycle cycle=1 predicate=reload_measure(&observed)"));
    Check(strstr(fake_diagnostics,"observed=[62,8,12]"));
    reset();state=(run_state_t){0};fault(FakeTime,1);
    Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
    Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=entry cycle=0 predicate=!GetProcessTimes"));
    Check(atomic_load(&fake_calls[FakeHandles])==0);
    reset();state=(run_state_t){.bad_stage=2201};
    Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
    Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=cleanup_final cycle=1100 predicate=api->stage(api->context,\"reload_cleanup\")"));
    Check(!fake_live());Check(!fake_module_live);
}
static void test_preinit(void) {
    ++groups;reset();run_state_t state={.bad_preinit=1};
    Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
    Check(state.preinit_calls==1);Check(state.cycles==0);Check(state.stage_calls==0);
    Check(atomic_load(&fake_calls[FakeHandles])==0);Check(atomic_load(&fake_calls[FakeTime])==0);
    Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=os_preinit cycle=0 predicate=api->preinit(api->context)!=0"));
    Check(!strstr(fake_run_head,"\"event\":\"baseline\""));Check(strstr(fake_run_head,"OpenCL reload FAILED"));
    Check(!fake_live());Check(!fake_module_live);
    char long_record[66];memset(long_record,'a',65);long_record[65]='\0';
    const char *unsafe[]={NULL,"","Upper","a\"b","user32;x","a b",long_record};
    for(unsigned i=0;i<sizeof unsafe/sizeof unsafe[0];++i) {
        reset();state=(run_state_t){.record=unsafe[i],.custom_record=1};
        Check(run_case(&state,L"C:\\candidate\\OpenCL.dll")==1);
        Check(state.preinit_calls==0);Check(state.cycles==0);Check(atomic_load(&fake_calls[FakeHandles])==0);
        Check(fake_diagnostic_count==1);Check(strstr(fake_diagnostics,"stage=entry cycle=0 predicate=!reload_record_safe(api->preinit_record)"));
    }
    long_record[64]='\0';
    reset();state=(run_state_t){.record=long_record,.custom_record=1};
    Check(!run_case(&state,L"C:\\candidate\\OpenCL.dll"));Check(state.preinit_calls==1);Check(state.cycles==1100);
    Check(strstr(fake_run_head,long_record)!=NULL);
    reset();state=(run_state_t){.record="a_0,z9",.custom_record=1};
    Check(!run_case(&state,L"C:\\candidate\\OpenCL.dll"));Check(state.cycles==1100);
}
int main(void) {
    test_pools();test_probes();test_run();test_fatal();test_failure_diagnostics();test_preinit();reset();
    printf("OPENCL_RELOAD_HOST groups=%u checks=%u PASS (API doubles only)\n",groups,checks);return 0;
}
