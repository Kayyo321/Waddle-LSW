/* Test harness for exact source-extracted teardown helpers. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_handle_t { int tag; } fake_handle_t;
#include "transport_loss_order_actual.h"

static fake_handle_t handle_event={1}, handle_kernel={2}, handle_buffer={3},
    handle_program={4}, handle_queue={5}, handle_context={6};
static char trace_log[8192];
static size_t trace_length;
static int release_fail_id=-1, shutdown_mode, shutdown_calls, platform_status;
static int abandon_status, wait_status, already_abandoned, enqueue_status;
static uint64_t abandoned_identity;
static unsigned long long fake_now;
static jmp_buf fail_target;
static int fail_armed;

static void trace(char value) { if (trace_length+1<sizeof trace_log) { trace_log[trace_length++]=value; trace_log[trace_length]=0; } }
static void reset_state(void) {
    memset(trace_log,0,sizeof trace_log);trace_length=0;release_fail_id=-1;
    shutdown_mode=0;shutdown_calls=0;platform_status=CL_INVALID_PLATFORM;
    abandon_status=RingOk;wait_status=0;enqueue_status=CL_SUCCESS;already_abandoned=0;abandoned_identity=0;fake_now=0;
}

static void fail_closed(const char *message) {
    (void)message;trace('F');
    if (fail_armed) longjmp(fail_target,1);
    _Exit(90);
}

static void fake_wait_retired_bounded(const wchar_t *path,uint64_t identity,ULONGLONG timeout_ms) {
    (void)path;(void)timeout_ms;trace('W');
    if (!identity || wait_status) fail_closed("receipt failed");
}

static int release_one(int id,const void *value,const void *expected,char mark) {
    trace(mark);if(already_abandoned||value!=expected)return CL_INVALID_OPERATION;
    return release_fail_id==id?CL_OUT_OF_RESOURCES:CL_SUCCESS;
}
static cl_int fake_enqueue(cl_command_queue queue,cl_kernel kernel,cl_uint dim,
    const size_t *offset,const size_t *global,const size_t *local,cl_uint waits,
    const cl_event *wait_list,cl_event *event) {
    (void)offset;(void)wait_list;trace('E');
    if(queue!=&handle_queue||kernel!=&handle_kernel||dim!=1||!global||global[0]!=1||
       !local||local[0]!=1||waits)return CL_INVALID_OPERATION;
    if(enqueue_status!=CL_SUCCESS)return enqueue_status;
    *event=&handle_event;return CL_SUCCESS;
}
static cl_int fake_read(cl_command_queue queue,cl_mem buffer,uint32_t blocking,
    size_t offset,size_t bytes,void *destination,cl_uint waits,
    const cl_event *wait_list,cl_event *event) {
    (void)destination;(void)wait_list;trace('R');
    return queue==&handle_queue&&buffer==&handle_buffer&&blocking==CL_TRUE&&
        offset==0&&bytes==sizeof(cl_uint)&&waits==0&&!event?CL_OUT_OF_RESOURCES:CL_INVALID_OPERATION;
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
    if(shutdown_mode==1 || (shutdown_mode==4 && shutdown_calls==1))return ClvkShutdownBusy;
    if(shutdown_mode==2)return -777;
    if(shutdown_mode==3)return shutdown_calls==1?ClvkShutdownOk:-777;
    return ClvkShutdownOk;
}
static venus_ring_status_t fake_abandon(uint64_t identity) {
    trace('a');already_abandoned=1;abandoned_identity=identity;return abandon_status;
}
static ULONGLONG GetTickCount64(void) { return fake_now; }
static void Sleep(unsigned long milliseconds) { fake_now+=milliseconds; }

static api_t make_api(void) {
    api_t api={0};api.release_event=release_event;api.release_kernel=release_kernel;
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

int main(void) {
    if(test_extracted_call()||test_exact_success_order()||test_release_error_still_tears_down_safely()||
       test_receipt_failure_never_releases_or_abandons()||test_shutdown_failures_never_abandon()||
       test_abandon_failure_is_after_safe_teardown()||test_old_abandon_first_order_is_rejected())return 1;
    printf("source-extracted transport-loss retirement/release/shutdown/abandon order: PASS source=%s extraction=%s\n",
        ActualIntegrationSourceSha256,ActualExtractionSha256);return 0;
}
