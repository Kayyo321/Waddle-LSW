/* Test harness for exact source-extracted teardown helpers. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct fake_handle_t { int tag; } fake_handle_t;
#include "transport_loss_order_actual.h"

static fake_handle_t handle_event={1}, handle_kernel={2}, handle_buffer={3},
    handle_program={4}, handle_queue={5}, handle_context={6}, handle_device={7}, handle_thread={8};
static char trace_log[8192];
static size_t trace_length;
static int release_fail_id=-1, shutdown_mode, shutdown_calls, platform_status;
static int abandon_status, wait_status, already_abandoned, enqueue_status;
static uint64_t abandoned_identity;
static unsigned long long fake_now;
static jmp_buf fail_target;
static int fail_armed;

static int full_mode,setup_failure_stage,setup_failure_kind,initial_busy_wrong;
static unsigned owned_mask,created_mask,released_mask;
static int thread_owned,thread_joined,thread_failure,thread_exit_override,shutdown_terminal;
static int stop_status,read_status,retain_release_error;
static uint64_t session_identity;
static unsigned (*pending_entry)(void *);
static void *pending_argument;
static unsigned thread_result;
enum { ThreadCreate=1,ThreadWait,ThreadExit,ThreadClose };


static void trace(char value) { if (trace_length+1<sizeof trace_log) { trace_log[trace_length++]=value; trace_log[trace_length]=0; } }
static void reset_state(void) {
    memset(trace_log,0,sizeof trace_log);trace_length=0;release_fail_id=-1;
    shutdown_mode=0;shutdown_calls=0;platform_status=CL_INVALID_PLATFORM;
    abandon_status=RingOk;wait_status=0;enqueue_status=CL_SUCCESS;already_abandoned=0;abandoned_identity=0;fake_now=0;

    full_mode=setup_failure_stage=setup_failure_kind=initial_busy_wrong=0;
    owned_mask=created_mask=released_mask=0;thread_owned=thread_joined=thread_failure=shutdown_terminal=0;
    thread_exit_override=-1;stop_status=RingOk;read_status=CL_OUT_OF_RESOURCES;retain_release_error=0;
    session_identity=0x1234;pending_entry=NULL;pending_argument=NULL;thread_result=0;

}

static void fail_closed(const char *message) {
    (void)message;trace('F');
    if (fail_armed) longjmp(fail_target,1);
    _Exit(90);
}

static void fake_wait_retired_bounded(const wchar_t *path,uint64_t identity,ULONGLONG timeout_ms) {
    (void)path;(void)timeout_ms;trace('W');
    if (!identity || wait_status) fail_closed("receipt failed");
    if(full_mode&&(!path||wcscmp(path,L"receipt")||identity!=session_identity||timeout_ms!=90000||thread_owned))
        fail_closed("wrong proof or unjoined caller");
}

static int release_one(int id,const void *value,const void *expected,char mark) {
    trace(mark);if(already_abandoned||value!=expected)return CL_INVALID_OPERATION;
    if(full_mode) {
        unsigned bit=1u<<(unsigned)id;
        if(!(owned_mask&bit))return CL_INVALID_OPERATION;
        if(!(retain_release_error&&release_fail_id==id)){owned_mask&=~bit;released_mask|=bit;}
    }
    return release_fail_id==id?CL_OUT_OF_RESOURCES:CL_SUCCESS;
}
static cl_int fake_enqueue(cl_command_queue queue,cl_kernel kernel,cl_uint dim,
    const size_t *offset,const size_t *global,const size_t *local,cl_uint waits,
    const cl_event *wait_list,cl_event *event) {
    (void)offset;(void)wait_list;trace('E');
    if(queue!=&handle_queue||kernel!=&handle_kernel||dim!=1||!global||global[0]!=1||
       !local||local[0]!=1||waits)return CL_INVALID_OPERATION;
    if(enqueue_status!=CL_SUCCESS)return enqueue_status;
    *event=&handle_event;if(full_mode){owned_mask|=1;created_mask|=1;}return CL_SUCCESS;
}
static cl_int fake_read(cl_command_queue queue,cl_mem buffer,uint32_t blocking,
    size_t offset,size_t bytes,void *destination,cl_uint waits,
    const cl_event *wait_list,cl_event *event) {
    (void)destination;(void)wait_list;trace('R');
    return queue==&handle_queue&&buffer==&handle_buffer&&blocking==CL_TRUE&&
        offset==0&&bytes==sizeof(cl_uint)&&waits==0&&!event?read_status:CL_INVALID_OPERATION;
}
static cl_int release_event(cl_event value){return release_one(0,value,&handle_event,'e');}
static cl_int release_kernel(cl_kernel value){return release_one(1,value,&handle_kernel,'k');}
static cl_int release_buffer(cl_mem value){return release_one(2,value,&handle_buffer,'b');}
static cl_int release_program(cl_program value){return release_one(3,value,&handle_program,'p');}
static cl_int release_queue(cl_command_queue value){return release_one(4,value,&handle_queue,'q');}
static cl_int release_context(cl_context value){return release_one(5,value,&handle_context,'c');}
static cl_int fake_get_platform_ids(cl_uint count,cl_platform_id *platforms,cl_uint *found) {
    (void)count;(void)platforms;if(found)*found=0;trace('g');return platform_status;
}
static int32_t fake_shutdown(uint32_t abi) {
    trace('s');++shutdown_calls;
    if(abi!=ClvkShutdownAbi)return -777;
    if(full_mode&&owned_mask)return initial_busy_wrong?ClvkShutdownOk:ClvkShutdownBusy;
    if(shutdown_mode==1 || (shutdown_mode==4 && shutdown_calls==1))return ClvkShutdownBusy;
    if(shutdown_mode==2)return -777;
    if(shutdown_mode==3)return shutdown_calls==1?ClvkShutdownOk:-777;
    shutdown_terminal=1;return ClvkShutdownOk;
}
static venus_ring_status_t fake_abandon(uint64_t identity) {
    trace('a');if(full_mode&&(!shutdown_terminal||owned_mask||thread_owned||identity!=session_identity))fail_closed("unsafe abandon");
    already_abandoned=1;abandoned_identity=identity;return abandon_status;
}
static ULONGLONG GetTickCount64(void) { return fake_now; }
static void Sleep(unsigned long milliseconds) { fake_now+=milliseconds; }

/* Setup doubles own one bit per successful CL return. Thread doubles execute
 * the actual callback only at the join seam; no native concurrency is claimed. */
static void require_fake(int condition) {
    if(!condition){fprintf(stderr,"invalid SDK/Win32 double use trace=%s\n",trace_log);abort();}
}
static fake_handle_t *setup_owner(int stage,fake_handle_t *handle,cl_int *status,char mark) {
    trace(mark);require_fake(full_mode&&status&&!already_abandoned);
    *status=CL_SUCCESS;
    if(setup_failure_stage==stage) {
        *status=setup_failure_kind==2?CL_SUCCESS:CL_OUT_OF_RESOURCES;
        if(setup_failure_kind!=1)return NULL;
    }
    unsigned bit=1u<<(unsigned)(handle->tag-1);
    require_fake(!(owned_mask&bit));owned_mask|=bit;created_mask|=bit;return handle;
}
static cl_context fake_create_context(const void *properties,cl_uint count,const cl_device_id *devices,
    void *notify,void *user,cl_int *status) {
    require_fake(!properties&&count==1&&devices&&devices[0]==&handle_device&&!notify&&!user);
    return setup_owner(1,&handle_context,status,'C');
}
static cl_command_queue fake_create_queue(cl_context context,cl_device_id device,uint64_t flags,cl_int *status) {
    require_fake(context==&handle_context&&device==&handle_device&&!flags);
    return setup_owner(2,&handle_queue,status,'Q');
}
static cl_program fake_create_program(cl_context context,cl_uint count,const char **sources,const size_t *lengths,cl_int *status) {
    require_fake(context==&handle_context&&count==1&&sources&&sources[0]&&!strcmp(sources[0],TransportLossKernel)&&!lengths);
    return setup_owner(3,&handle_program,status,'P');
}
static cl_int fake_build_program(cl_program program,cl_uint count,const cl_device_id *devices,
    const char *options,void *notify,void *user) {
    require_fake(program==&handle_program&&count==1&&devices&&devices[0]==&handle_device&&options&&!options[0]&&!notify&&!user);
    trace('B');return setup_failure_stage==4?CL_OUT_OF_RESOURCES:CL_SUCCESS;
}
static cl_kernel fake_create_kernel(cl_program program,const char *name,cl_int *status) {
    require_fake(program==&handle_program&&name&&!strcmp(name,"transport_loss"));
    return setup_owner(5,&handle_kernel,status,'K');
}
static cl_mem fake_create_buffer(cl_context context,uint64_t flags,size_t size,void *host,cl_int *status) {
    require_fake(context==&handle_context&&flags==CL_MEM_READ_WRITE&&size==sizeof(cl_uint)&&!host);
    return setup_owner(6,&handle_buffer,status,'M');
}
static cl_int fake_set_arg(cl_kernel kernel,cl_uint index,size_t size,const void *value) {
    cl_mem buffer=NULL;require_fake(kernel==&handle_kernel&&!index&&size==sizeof buffer&&value);
    memcpy(&buffer,value,sizeof buffer);require_fake(buffer==&handle_buffer);
    trace('A');return setup_failure_stage==7?CL_OUT_OF_RESOURCES:CL_SUCCESS;
}
static uintptr_t _beginthreadex(void *security,unsigned stack,unsigned (*entry)(void *),void *arg,unsigned flags,unsigned *tid) {
    trace('T');require_fake(!security&&!stack&&entry&&arg&&!flags&&!tid&&!thread_owned);
    if(thread_failure==ThreadCreate)return 0;
    pending_entry=entry;pending_argument=arg;thread_owned=1;return (uintptr_t)&handle_thread;
}
static DWORD WaitForSingleObject(HANDLE thread,DWORD timeout) {
    trace('J');require_fake(thread==&handle_thread&&thread_owned&&!thread_joined&&timeout==65000);
    if(thread_failure==ThreadWait)return 258;
    thread_result=pending_entry(pending_argument);pending_entry=NULL;pending_argument=NULL;thread_joined=1;
    if(thread_exit_override>=0)thread_result=(unsigned)thread_exit_override;
    return WAIT_OBJECT_0;
}
static BOOL GetExitCodeThread(HANDLE thread,DWORD *code) {
    trace('X');require_fake(thread==&handle_thread&&thread_owned&&thread_joined&&code);
    if(thread_failure==ThreadExit)return 0;
    *code=thread_result;return 1;
}
static BOOL CloseHandle(HANDLE thread) {
    trace('H');require_fake(thread==&handle_thread&&thread_owned&&thread_joined);
    if(thread_failure==ThreadClose)return 0;
    thread_owned=0;return 1;
}
static uint64_t fake_session(void) { trace('I');return session_identity; }
static venus_ring_status_t fake_stop(void) {
    trace('t');require_fake(!owned_mask&&shutdown_terminal&&!thread_owned);
    return stop_status;
}

static api_t make_api(void) {
    api_t api={0};api.create_context=fake_create_context;api.create_queue=fake_create_queue;
    api.create_program=fake_create_program;api.build_program=fake_build_program;api.create_kernel=fake_create_kernel;
    api.create_buffer=fake_create_buffer;api.set_arg=fake_set_arg;api.release_event=release_event;api.release_kernel=release_kernel;
    api.release_buffer=release_buffer;api.release_program=release_program;
    api.release_queue=release_queue;api.release_context=release_context;
    api.get_platform_ids=fake_get_platform_ids;api.enqueue_kernel=fake_enqueue;
    api.read_buffer=fake_read;return api;
}
static int owners_are_null(cl_event e,cl_kernel k,cl_mem b,cl_program p,cl_command_queue q,cl_context c) {
    return !e&&!k&&!b&&!p&&!q&&!c;
}
#define Check(x) do { if(!(x)){fprintf(stderr,"order fixture failed %s:%d: %s trace=%s\n",__FILE__,__LINE__,#x,trace_log);return 1;} } while(0)

static int test_extracted_call(void) {
    reset_state();api_t api=make_api();transport_loss_call_t call={0};
    call.api=&api;call.queue=&handle_queue;call.kernel=&handle_kernel;call.buffer=&handle_buffer;
    unsigned result=run_transport_loss_call(&call);
    Check(result==0&&call.enqueue_status==CL_SUCCESS&&call.read_status==CL_OUT_OF_RESOURCES);
    Check(call.kernel_event==&handle_event&&strcmp(trace_log,"ER")==0);
    reset_state();enqueue_status=CL_OUT_OF_RESOURCES;call.kernel_event=NULL;
    Check(run_transport_loss_call(&call)==1 && call.kernel_event==NULL && strcmp(trace_log,"E")==0);
    return 0;
}

static int test_exact_success_order(void) {
    reset_state();api_t api=make_api();cl_event e=&handle_event;cl_kernel k=&handle_kernel;
    cl_mem b=&handle_buffer;cl_program p=&handle_program;cl_command_queue q=&handle_queue;
    cl_context c=&handle_context;shutdown_mode=0;
    int result=release_transport_loss_after_retirement(&api,fake_shutdown,L"receipt",0x1234,
        fake_abandon,&e,&k,&b,&p,&q,&c);
    Check(result==0&&owners_are_null(e,k,b,p,q,c));
    Check(strcmp(trace_log,"Wekbpqcssga")==0);
    Check(abandoned_identity==0x1234&&shutdown_calls==2);
    reset_state();shutdown_mode=4;
    Check(release_transport_loss_after_retirement(&api,fake_shutdown,L"receipt",0x1234,
        fake_abandon,&e,&k,&b,&p,&q,&c)==0);
    Check(strcmp(trace_log,"Wsssga")==0 && fake_now==1 && shutdown_calls==3);
    return 0;
}

static int test_release_error_still_tears_down_safely(void) {
    for(int failed_id=0;failed_id<6;++failed_id){
        reset_state();api_t api=make_api();release_fail_id=failed_id;
        cl_event e=&handle_event;cl_kernel k=&handle_kernel;cl_mem b=&handle_buffer;
        cl_program p=&handle_program;cl_command_queue q=&handle_queue;cl_context c=&handle_context;
        int result=release_transport_loss_after_retirement(&api,fake_shutdown,L"receipt",0x2345,
            fake_abandon,&e,&k,&b,&p,&q,&c);
        Check(result==1&&owners_are_null(e,k,b,p,q,c));
        Check(strncmp(trace_log,"Wekbpqc",7)==0&&abandoned_identity==0x2345);
        Check(trace_log[strlen(trace_log)-1]=='a');
    }
    return 0;
}

static int test_receipt_failure_never_releases_or_abandons(void) {
    reset_state();api_t api=make_api();wait_status=1;cl_event e=&handle_event;
    cl_kernel k=&handle_kernel;cl_mem b=&handle_buffer;cl_program p=&handle_program;
    cl_command_queue q=&handle_queue;cl_context c=&handle_context;fail_armed=1;
    if(setjmp(fail_target)==0){
        (void)release_transport_loss_after_retirement(&api,fake_shutdown,L"bad",0x3456,
            fake_abandon,&e,&k,&b,&p,&q,&c);
        fail_armed=0;return 1;
    }
    fail_armed=0;Check(strcmp(trace_log,"WF")==0);
    Check(e==&handle_event&&k==&handle_kernel&&b==&handle_buffer&&p==&handle_program&&q==&handle_queue&&c==&handle_context);
    Check(shutdown_calls==0&&abandoned_identity==0);return 0;
}

static int expect_shutdown_fail(int mode,int platform) {
    reset_state();api_t api=make_api();shutdown_mode=mode;platform_status=platform;
    cl_event e=&handle_event;cl_kernel k=&handle_kernel;cl_mem b=&handle_buffer;
    cl_program p=&handle_program;cl_command_queue q=&handle_queue;cl_context c=&handle_context;
    fail_armed=1;
    if(setjmp(fail_target)==0){
        (void)release_transport_loss_after_retirement(&api,fake_shutdown,L"receipt",0x4567,
            fake_abandon,&e,&k,&b,&p,&q,&c);
        fail_armed=0;return 1;
    }
    fail_armed=0;
    return abandoned_identity==0&&trace_log[strlen(trace_log)-1]=='F';
}
static int test_shutdown_failures_never_abandon(void) {
    Check(expect_shutdown_fail(1,CL_INVALID_PLATFORM)); /* bounded Busy */
    Check(expect_shutdown_fail(2,CL_INVALID_PLATFORM)); /* non-Ok status */
    Check(expect_shutdown_fail(3,CL_INVALID_PLATFORM)); /* non-idempotent shutdown */
    Check(expect_shutdown_fail(0,CL_SUCCESS)); /* platform remains callable */
    return 0;
}
static int test_abandon_failure_is_after_safe_teardown(void) {
    reset_state();api_t api=make_api();abandon_status=-9;
    cl_event e=NULL;cl_kernel k=NULL;cl_mem b=NULL;cl_program p=NULL;
    cl_command_queue q=NULL;cl_context c=NULL;fail_armed=1;
    if(setjmp(fail_target)==0){
        (void)release_transport_loss_after_retirement(&api,fake_shutdown,L"receipt",0x5678,
            fake_abandon,&e,&k,&b,&p,&q,&c);
        fail_armed=0;return 1;
    }
    fail_armed=0;Check(abandoned_identity==0x5678&&shutdown_calls==2);
    Check(strcmp(trace_log,"WssgaF")==0);return 0;
}

/* Prove the mocks reject the historical unsafe order as a negative control. */
static int test_old_abandon_first_order_is_rejected(void) {
    reset_state();api_t api=make_api();cl_event e=&handle_event;cl_kernel k=&handle_kernel;
    cl_mem b=&handle_buffer;cl_program p=&handle_program;cl_command_queue q=&handle_queue;
    cl_context c=&handle_context;
    (void)fake_abandon(0x6789);
    /* The ICD mock rejects destructor dispatch after its owner was abandoned. */
    int result=release_transport_loss_owners(&api,&e,&k,&b,&p,&q,&c);
    Check(result==1&&owners_are_null(e,k,b,p,q,c));
    Check(trace_log[0]=='a'&&trace_log[1]=='e');return 0;
}

/* Full actual function controls: assertions include independent expected traces
 * and ownership counts, so an early abandon/stop is rejected by the SDK doubles. */
static void begin_full(void) { reset_state();full_mode=1; }
static int invoke_full(void) {
    api_t api=make_api();return run_transport_loss(&api,&handle_device,fake_shutdown,L"receipt",fake_stop,fake_session,fake_abandon);
}
static int test_full_success_and_result_errors(void) {
    begin_full();Check(invoke_full()==0);
    Check(strcmp(trace_log,"ICQPBKMATJERXHsWekbpqcssga")==0);
    Check(!owned_mask&&!thread_owned&&created_mask==63&&released_mask==63&&abandoned_identity==0x1234);
    begin_full();read_status=CL_SUCCESS;Check(invoke_full()==1);
    Check(strcmp(trace_log,"ICQPBKMATJERXHsWekbpqcssga")==0&&!owned_mask&&!thread_owned);
    begin_full();enqueue_status=CL_OUT_OF_RESOURCES;Check(invoke_full()==1);
    Check(strcmp(trace_log,"ICQPBKMATJEXHsWkbpqcssga")==0&&!owned_mask&&!thread_owned);
    /* Distinguish an API callback failure from an inconsistent exit-code report. */
    begin_full();enqueue_status=CL_OUT_OF_RESOURCES;thread_exit_override=0;Check(invoke_full()==1);
    Check(strstr(trace_log,"XHsWkbpqcssga")&&!owned_mask&&!thread_owned);
    begin_full();thread_exit_override=1;Check(invoke_full()==1);
    Check(strstr(trace_log,"XHsWekbpqcssga")&&!owned_mask&&!thread_owned);
    for(int id=0;id<6;++id){
        begin_full();release_fail_id=id;Check(invoke_full()==1);
        Check(!owned_mask&&!thread_owned&&released_mask==created_mask&&abandoned_identity==0x1234);
    }
    return 0;
}
static int test_full_setup_failures(void) {
    for(int stage=1;stage<=7;++stage)for(int kind=0;kind<3;++kind) {
        begin_full();setup_failure_stage=stage;setup_failure_kind=kind;
        Check(invoke_full()==1);Check(!strchr(trace_log,'T')&&!strchr(trace_log,'W')&&!strchr(trace_log,'a'));
        Check(!owned_mask&&!thread_owned&&created_mask==released_mask&&shutdown_terminal);
        Check(strlen(trace_log)>=4&&!strcmp(trace_log+strlen(trace_log)-4,"ssgt"));
    }
    /* Failed normal stop retains its original session until exact proof. */
    begin_full();setup_failure_stage=7;stop_status=-1;Check(invoke_full()==1);
    Check(strcmp(trace_log,"ICQPBKMAkbpqcssgtWa")==0&&abandoned_identity==0x1234&&!owned_mask);
    begin_full();setup_failure_stage=7;release_fail_id=2;Check(invoke_full()==1);
    Check(!strchr(trace_log,'W')&&!strchr(trace_log,'a')&&!owned_mask&&released_mask==created_mask);
    return 0;
}
static int test_full_early_fatal_paths(void) {
    for(volatile int mode=ThreadCreate;mode<=ThreadClose;++mode) {
        begin_full();thread_failure=mode;fail_armed=1;
        if(setjmp(fail_target)==0){(void)invoke_full();fail_armed=0;return 1;}
        fail_armed=0;
        Check(!strchr(trace_log,'W')&&!strchr(trace_log,'a')&&!strchr(trace_log,'s')&&!released_mask);
        Check(!shutdown_terminal&&owned_mask==created_mask);
        if(mode==ThreadCreate)Check(!thread_owned);
        else Check(thread_owned);
        /* The real fail_closed terminates the process. Fake owners are static;
         * reset discards the model only after checking they were NOT released. */
    }
    begin_full();session_identity=0;fail_armed=1;
    if(setjmp(fail_target)==0){(void)invoke_full();fail_armed=0;return 1;}
    fail_armed=0;Check(!strcmp(trace_log,"IF")&&!owned_mask&&!thread_owned);
    begin_full();initial_busy_wrong=1;fail_armed=1;
    if(setjmp(fail_target)==0){(void)invoke_full();fail_armed=0;return 1;}
    fail_armed=0;Check(!strchr(trace_log,'W')&&!strchr(trace_log,'a')&&!released_mask&&!thread_owned);
    Check(!strcmp(trace_log,"ICQPBKMATJERXHsF"));
    return 0;
}
static int test_full_late_fatal_paths(void) {
    for(volatile int before_api=0;before_api<2;++before_api) {
        begin_full();wait_status=1;
        if(before_api){setup_failure_stage=7;stop_status=-1;}
        fail_armed=1;
        if(setjmp(fail_target)==0){(void)invoke_full();fail_armed=0;return 1;}
        fail_armed=0;Check(!strchr(trace_log,'a')&&!thread_owned);
        if(before_api)Check(!strcmp(trace_log,"ICQPBKMAkbpqcssgtWF")&&!owned_mask&&shutdown_terminal);
        else Check(!strcmp(trace_log,"ICQPBKMATJERXHsWF")&&owned_mask==created_mask&&!released_mask&&!shutdown_terminal);
        begin_full();abandon_status=-9;
        if(before_api){setup_failure_stage=7;stop_status=-1;}
        fail_armed=1;
        if(setjmp(fail_target)==0){(void)invoke_full();fail_armed=0;return 1;}
        fail_armed=0;Check(!owned_mask&&!thread_owned&&shutdown_terminal&&abandoned_identity==0x1234);
        Check(!strcmp(trace_log+strlen(trace_log)-2,"aF"));
    }
    begin_full();release_fail_id=2;retain_release_error=1;fail_armed=1;
    if(setjmp(fail_target)==0){(void)invoke_full();fail_armed=0;return 1;}
    fail_armed=0;Check(!strchr(trace_log,'a')&&owned_mask==(1u<<2)&&fake_now>=5000&&!thread_owned);
    return 0;
}

int main(void) {
    if(test_extracted_call()||test_exact_success_order()||test_release_error_still_tears_down_safely()||
       test_receipt_failure_never_releases_or_abandons()||test_shutdown_failures_never_abandon()||
       test_abandon_failure_is_after_safe_teardown()||test_old_abandon_first_order_is_rejected()||
       test_full_success_and_result_errors()||test_full_setup_failures()||test_full_early_fatal_paths()||
       test_full_late_fatal_paths())return 1;
    printf("source-extracted transport-loss retirement/release/shutdown/abandon order: PASS source=%s extraction=%s\n",
        ActualIntegrationSourceSha256,ActualExtractionSha256);return 0;
}
