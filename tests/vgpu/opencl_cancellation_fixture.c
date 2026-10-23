/** @file opencl_cancellation_fixture.c @brief Controls for extracted transport-loss owners. */
#include "transport_loss_actual.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Check(value) do { if (!(value)) { \
    fprintf(stderr,"cancellation fixture check failed at %s:%d: %s\n",__FILE__,__LINE__,#value); \
    return 1; \
} } while (0)

typedef struct fake_handle_t { unsigned value; } fake_handle_t;

static fake_handle_t queue_handle={1},kernel_handle={2},buffer_handle={3},program_handle={4},
    context_handle={5},event_handle={6};
static cl_command_queue expected_queue;
static cl_kernel expected_kernel;
static cl_mem expected_buffer;
static cl_event *expected_event_pointer;
static cl_uint *expected_output_pointer;
static cl_int enqueue_result=CL_SUCCESS;
static cl_int read_result=CL_SUCCESS;
static unsigned enqueue_calls,read_calls;
static int captured_call_arguments;

static cl_int fake_enqueue_kernel(cl_command_queue queue,cl_kernel kernel,cl_uint work_dim,
    const size_t *global_offset,const size_t *global_size,const size_t *local_size,
    cl_uint wait_count,const cl_event *wait_list,cl_event *event)
{
    ++enqueue_calls;
    captured_call_arguments=queue==expected_queue && kernel==expected_kernel && work_dim==1 &&
        global_offset==NULL && global_size && global_size[0]==1 && local_size &&
        local_size[0]==1 && wait_count==0 && wait_list==NULL && event==expected_event_pointer;
    if (enqueue_result==CL_SUCCESS) *event=&event_handle;
    return enqueue_result;
}

static cl_int fake_read_buffer(cl_command_queue queue,cl_mem buffer,cl_bool blocking,
    size_t offset,size_t bytes,void *destination,cl_uint wait_count,
    const cl_event *wait_list,cl_event *event)
{
    ++read_calls;
    captured_call_arguments=captured_call_arguments && queue==expected_queue && buffer==expected_buffer &&
        blocking==CL_TRUE && offset==0 && bytes==sizeof(cl_uint) &&
        destination==(void *)expected_output_pointer && wait_count==0 && wait_list==NULL && event==NULL;
    if (read_result==CL_SUCCESS && destination && bytes==sizeof(cl_uint))
        *(cl_uint *)destination=0x51a7u;
    return read_result;
}

static api_t make_api(void)
{
    api_t api={0};
    api.enqueue_kernel=fake_enqueue_kernel;
    api.read_buffer=fake_read_buffer;
    return api;
}

static int test_transport_call_success(void)
{
    api_t api=make_api();
    transport_loss_call_t call={0};
    expected_queue=&queue_handle; expected_kernel=&kernel_handle; expected_buffer=&buffer_handle;
    enqueue_result=CL_SUCCESS; read_result=CL_SUCCESS;
    enqueue_calls=read_calls=0; captured_call_arguments=1;
    call.api=&api; call.queue=expected_queue; call.kernel=expected_kernel;
    call.buffer=expected_buffer; call.enqueue_status=CL_INVALID_OPERATION;
    call.read_status=CL_INVALID_OPERATION;
    expected_event_pointer=&call.kernel_event; expected_output_pointer=&call.output;
    unsigned result=run_transport_loss_call(&call);
    Check(result==0 && enqueue_calls==1 && read_calls==1 && captured_call_arguments);
    Check(call.enqueue_status==CL_SUCCESS && call.read_status==CL_SUCCESS);
    Check(call.kernel_event==&event_handle && call.output==0x51a7u);
    return 0;
}

static int test_transport_call_enqueue_failure(void)
{
    api_t api=make_api();
    transport_loss_call_t call={0};
    enqueue_result=CL_OUT_OF_RESOURCES; read_result=CL_SUCCESS;
    enqueue_calls=read_calls=0; captured_call_arguments=1;
    call.api=&api; call.queue=expected_queue; call.kernel=expected_kernel;
    call.buffer=expected_buffer; call.read_status=CL_INVALID_OPERATION;
    expected_event_pointer=&call.kernel_event; expected_output_pointer=&call.output;
    unsigned result=run_transport_loss_call(&call);
    Check(result==1 && enqueue_calls==1 && read_calls==0 && captured_call_arguments);
    Check(call.enqueue_status==CL_OUT_OF_RESOURCES && call.read_status==CL_INVALID_OPERATION);
    Check(call.kernel_event==NULL);
    return 0;
}

static int test_transport_call_read_failure(void)
{
    api_t api=make_api();
    transport_loss_call_t call={0};
    enqueue_result=CL_SUCCESS; read_result=CL_OUT_OF_RESOURCES;
    enqueue_calls=read_calls=0; captured_call_arguments=1;
    call.api=&api; call.queue=expected_queue; call.kernel=expected_kernel;
    call.buffer=expected_buffer;
    expected_event_pointer=&call.kernel_event; expected_output_pointer=&call.output;
    unsigned result=run_transport_loss_call(&call);
    Check(result==0 && enqueue_calls==1 && read_calls==1 && captured_call_arguments);
    Check(call.enqueue_status==CL_SUCCESS && call.read_status==CL_OUT_OF_RESOURCES);
    Check(call.kernel_event==&event_handle);
    return 0;
}

enum release_id_t { ReleaseEvent,ReleaseKernel,ReleaseBuffer,ReleaseProgram,ReleaseQueue,ReleaseContext };
static int release_order[6];
static unsigned release_calls;
static int release_failure=-1;

static cl_int record_release(enum release_id_t id,const void *actual,const void *expected)
{
    if (release_calls<sizeof release_order/sizeof *release_order)
        release_order[release_calls]=(int)id;
    ++release_calls;
    if (actual!=expected) return CL_INVALID_OPERATION;
    return release_failure==(int)id ? CL_OUT_OF_RESOURCES : CL_SUCCESS;
}

static cl_int fake_release_event(cl_event value) { return record_release(ReleaseEvent,value,&event_handle); }
static cl_int fake_release_kernel(cl_kernel value) { return record_release(ReleaseKernel,value,&kernel_handle); }
static cl_int fake_release_buffer(cl_mem value) { return record_release(ReleaseBuffer,value,&buffer_handle); }
static cl_int fake_release_program(cl_program value) { return record_release(ReleaseProgram,value,&program_handle); }
static cl_int fake_release_queue(cl_command_queue value) { return record_release(ReleaseQueue,value,&queue_handle); }
static cl_int fake_release_context(cl_context value) { return record_release(ReleaseContext,value,&context_handle); }

static api_t make_release_api(void)
{
    api_t api={0};
    api.release_event=fake_release_event;
    api.release_kernel=fake_release_kernel;
    api.release_buffer=fake_release_buffer;
    api.release_program=fake_release_program;
    api.release_queue=fake_release_queue;
    api.release_context=fake_release_context;
    return api;
}

static int owners_are_null(cl_event event,cl_kernel kernel,cl_mem buffer,cl_program program,
    cl_command_queue queue,cl_context context)
{
    return !event && !kernel && !buffer && !program && !queue && !context;
}

static int test_null_owners(void)
{
    api_t api=make_release_api();
    cl_event event=NULL; cl_kernel kernel=NULL; cl_mem buffer=NULL; cl_program program=NULL;
    cl_command_queue queue=NULL; cl_context context=NULL;
    release_calls=0;
    Check(release_transport_loss_owners(&api,&event,&kernel,&buffer,&program,&queue,&context)==0);
    Check(release_calls==0 && owners_are_null(event,kernel,buffer,program,queue,context));
    return 0;
}

static int test_reverse_release_and_failures(void)
{
    static const int expected[]={ReleaseEvent,ReleaseKernel,ReleaseBuffer,ReleaseProgram,
        ReleaseQueue,ReleaseContext};
    api_t api=make_release_api();
    for (int failure=-1;failure<6;++failure) {
        cl_event event=&event_handle; cl_kernel kernel=&kernel_handle; cl_mem buffer=&buffer_handle;
        cl_program program=&program_handle; cl_command_queue queue=&queue_handle;
        cl_context context=&context_handle;
        release_calls=0; release_failure=failure;
        int result=release_transport_loss_owners(&api,&event,&kernel,&buffer,&program,&queue,&context);
        Check(result==(failure<0 ? 0 : 1));
        Check(release_calls==6 && owners_are_null(event,kernel,buffer,program,queue,context));
        for (size_t index=0;index<sizeof expected/sizeof *expected;++index)
            Check(release_order[index]==expected[index]);
    }
    return 0;
}

int main(void)
{
    if (test_transport_call_success() || test_transport_call_enqueue_failure() ||
        test_transport_call_read_failure() || test_null_owners() ||
        test_reverse_release_and_failures()) return 1;
    printf("transport-loss fixture passed source=%s extraction=%s\n",
        ActualIntegrationSourceSha256,ActualExtractionSha256);
    return 0;
}

#undef Check
