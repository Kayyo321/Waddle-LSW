/** @file opencl_transport_gate_test.c @brief POSIX controls for the qualification cancel gate. */
#define _POSIX_C_SOURCE 200809L
#include "opencl_transport_gate.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum injected_io_t {
    InjectNone,
    InjectFchmodFailure,
    InjectFstatFailure,
    InjectNonRegularFile,
    InjectWrongMode,
    InjectWriteFailure,
    InjectWriteZero,
    InjectEintrAndPartial,
    InjectCloseFailure,
    InjectPollFailure,
    InjectPollEintr,
    InjectClockFailure
} injected_io_t;

static injected_io_t InjectedIo;
static unsigned InjectedCalls;

#define Check(value) do { if (!(value)) { \
    fprintf(stderr,"opencl transport gate check failed at %s:%d: %s\n",__FILE__,__LINE__,#value); \
    exit(1); \
} } while (0)

int __real_fchmod(int descriptor,mode_t mode);
int __real_fstat(int descriptor,struct stat *info);
ssize_t __real_write(int descriptor,const void *bytes,size_t length);
int __real_close(int descriptor);
int __real_poll(struct pollfd *descriptors,nfds_t count,int timeout);
uint64_t __real_venus_tcp_now_ms(void);

int __wrap_fchmod(int descriptor,mode_t mode)
{
    if (InjectedIo==InjectFchmodFailure) { InjectedCalls++; errno=EIO; return -1; }
    return __real_fchmod(descriptor,mode);
}

int __wrap_fstat(int descriptor,struct stat *info)
{
    int result=__real_fstat(descriptor,info);
    if (result==0 && InjectedIo==InjectFstatFailure) { InjectedCalls++; errno=EIO; return -1; }
    if (result==0 && InjectedIo==InjectNonRegularFile) { InjectedCalls++; info->st_mode=0600; }
    if (result==0 && InjectedIo==InjectWrongMode) { InjectedCalls++; info->st_mode=(info->st_mode&~0777)|0644; }
    return result;
}

ssize_t __wrap_write(int descriptor,const void *bytes,size_t length)
{
    if (InjectedIo==InjectWriteFailure) { InjectedCalls++; errno=EIO; return -1; }
    if (InjectedIo==InjectWriteZero) { InjectedCalls++; return 0; }
    if (InjectedIo==InjectEintrAndPartial) {
        InjectedCalls++;
        if (InjectedCalls==1) { errno=EINTR; return -1; }
        if (length>5) length=5;
    }
    return __real_write(descriptor,bytes,length);
}

int __wrap_close(int descriptor)
{
    int result=__real_close(descriptor);
    if (InjectedIo==InjectCloseFailure) { InjectedCalls++; errno=EIO; return -1; }
    return result;
}

int __wrap_poll(struct pollfd *descriptors,nfds_t count,int timeout)
{
    if (InjectedIo==InjectPollFailure) { InjectedCalls++; errno=EIO; return -1; }
    if (InjectedIo==InjectPollEintr && InjectedCalls++==0) { errno=EINTR; return -1; }
    return __real_poll(descriptors,count,timeout);
}

uint64_t __wrap_venus_tcp_now_ms(void)
{
    if (InjectedIo==InjectClockFailure) { InjectedCalls++; return 0; }
    return __real_venus_tcp_now_ms();
}

typedef struct gate_thread_args_t {
    const _Atomic uint32_t *cancel;
    uint64_t deadline_ms;
    venus_ring_status_t result;
} gate_thread_args_t;

static size_t open_fd_count(void)
{
    DIR *directory=opendir("/proc/self/fd");
    Check(directory!=NULL);
    size_t count=0;
    struct dirent *entry;
    while ((entry=readdir(directory))!=NULL)
        if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    Check(closedir(directory)==0);
    return count;
}

static uint64_t now_ms(void)
{
    uint64_t value=venus_tcp_now_ms();
    Check(value!=0);
    return value;
}

static void *gate_thread(void *opaque)
{
    gate_thread_args_t *args=opaque;
    args->result=venus_opencl_transport_gate_wait(UINT64_C(0x1234abcd),77,18,
        args->deadline_ms,args->cancel);
    return NULL;
}

static void test_invalid_configuration(void)
{
    _Atomic uint32_t cancel=0;
    Check(unsetenv("WADDLE_OPENCL_CANCEL_GATE")==0);
    Check(venus_opencl_transport_gate_wait(1,1,18,now_ms()+100,&cancel)==RingInvalid);
    Check(venus_opencl_transport_gate_wait(1,1,18,now_ms()+100,NULL)==RingInvalid);
    Check(venus_opencl_transport_gate_wait(1,0,18,now_ms()+100,&cancel)==RingInvalid);
    Check(venus_opencl_transport_gate_wait(1,1,18,0,&cancel)==RingInvalid);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE","relative-marker",1)==0);
    Check(venus_opencl_transport_gate_wait(1,1,18,now_ms()+100,&cancel)==RingInvalid);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE","/tmp/invalid\nmarker",1)==0);
    Check(venus_opencl_transport_gate_wait(1,1,18,now_ms()+100,&cancel)==RingInvalid);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE","/tmp/invalid\177marker",1)==0);
    Check(venus_opencl_transport_gate_wait(1,1,18,now_ms()+100,&cancel)==RingInvalid);
    char too_long[4097];
    memset(too_long,'a',sizeof too_long-1);
    too_long[0]='/'; too_long[sizeof too_long-1]='\0';
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",too_long,1)==0);
    Check(venus_opencl_transport_gate_wait(1,1,18,now_ms()+100,&cancel)==RingInvalid);
    Check(venus_opencl_transport_gate_wait(0,1,18,now_ms()+100,&cancel)==RingInvalid);
    Check(venus_opencl_transport_gate_wait(1,1,17,now_ms()+100,&cancel)==RingInvalid);
}

static void test_creation_failures(const char *directory)
{
    _Atomic uint32_t cancel=0;
    char marker[512],target[512];
    Check(snprintf(marker,sizeof marker,"%s/collision",directory)>0);
    Check(snprintf(target,sizeof target,"%s/symlink-target",directory)>0);
    FILE *existing=fopen(marker,"wb");
    Check(existing!=NULL);
    Check(fputs("preserve",existing)>=0);
    Check(fclose(existing)==0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    Check(venus_opencl_transport_gate_wait(1,1,18,now_ms()+100,&cancel)==RingClosed);
    char bytes[16]={0};
    existing=fopen(marker,"rb");
    Check(existing!=NULL && fread(bytes,1,8,existing)==8 && fclose(existing)==0);
    Check(memcmp(bytes,"preserve",8)==0);

    Check(snprintf(marker,sizeof marker,"%s/symlink",directory)>0);
    Check(symlink(target,marker)==0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    Check(venus_opencl_transport_gate_wait(1,2,18,now_ms()+100,&cancel)==RingClosed);
    struct stat info;
    Check(lstat(marker,&info)==0 && S_ISLNK(info.st_mode));

    Check(snprintf(marker,sizeof marker,"%s/missing/marker",directory)>0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    Check(venus_opencl_transport_gate_wait(1,3,18,now_ms()+100,&cancel)==RingClosed);
}

static void test_deadline(const char *directory)
{
    _Atomic uint32_t cancel=0;
    char marker[512];
    Check(snprintf(marker,sizeof marker,"%s/timeout",directory)>0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    uint64_t start=now_ms();
    venus_ring_status_t status=venus_opencl_transport_gate_wait(0x1234,4,18,start+30,&cancel);
    uint64_t elapsed=now_ms()-start;
    Check(status==RingTimeout && elapsed>=20 && elapsed<1000);
    Check(access(marker,F_OK)==0);
}

static void test_expired_deadline_and_clock_failure(const char *directory)
{
    _Atomic uint32_t cancel=0;
    char marker[512];
    Check(snprintf(marker,sizeof marker,"%s/expired",directory)>0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    uint64_t current=now_ms();
    Check(current>1);
    Check(venus_opencl_transport_gate_wait(9,8,18,current-1,&cancel)==RingTimeout);
    Check(access(marker,F_OK)==0);
    Check(unlink(marker)==0);
    Check(snprintf(marker,sizeof marker,"%s/clock-failure",directory)>0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    InjectedIo=InjectClockFailure; InjectedCalls=0;
    Check(venus_opencl_transport_gate_wait(9,9,18,UINT64_MAX,&cancel)==RingClosed);
    Check(InjectedCalls==1 && access(marker,F_OK)==0);
    InjectedIo=InjectNone;
}

static void test_injected_io_paths(const char *directory)
{
    static const struct {
        const char *name;
        injected_io_t mode;
        venus_ring_status_t expected;
        int marker_remains;
    } cases[]={
        {"fchmod-failure",InjectFchmodFailure,RingClosed,0},
        {"fstat-failure",InjectFstatFailure,RingClosed,0},
        {"nonregular",InjectNonRegularFile,RingClosed,0},
        {"wrong-mode",InjectWrongMode,RingClosed,0},
        {"write-failure",InjectWriteFailure,RingClosed,0},
        {"write-zero",InjectWriteZero,RingClosed,0},
        {"close-failure",InjectCloseFailure,RingClosed,0},
        {"poll-failure",InjectPollFailure,RingClosed,1}
    };
    _Atomic uint32_t cancel=0;
    char marker[512];
    for (size_t index=0;index<sizeof cases/sizeof cases[0];++index) {
        Check(snprintf(marker,sizeof marker,"%s/%s",directory,cases[index].name)>0);
        Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
        InjectedIo=cases[index].mode; InjectedCalls=0;
        Check(venus_opencl_transport_gate_wait(2,index+1,18,now_ms()+100,&cancel)==cases[index].expected);
        Check(InjectedCalls>0);
        Check((access(marker,F_OK)==0)==cases[index].marker_remains);
        if (cases[index].marker_remains) Check(unlink(marker)==0);
        InjectedIo=InjectNone; InjectedCalls=0;
    }

    Check(snprintf(marker,sizeof marker,"%s/write-eintr-partial",directory)>0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    atomic_store_explicit(&cancel,1,memory_order_release);
    InjectedIo=InjectEintrAndPartial; InjectedCalls=0;
    Check(venus_opencl_transport_gate_wait(2,99,18,UINT64_MAX,&cancel)==RingCancelled);
    Check(InjectedCalls>=2 && access(marker,F_OK)==0);
    Check(unlink(marker)==0);
    InjectedIo=InjectNone; InjectedCalls=0;

    Check(snprintf(marker,sizeof marker,"%s/poll-eintr",directory)>0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    atomic_store_explicit(&cancel,0,memory_order_release);
    InjectedIo=InjectPollEintr; InjectedCalls=0;
    Check(venus_opencl_transport_gate_wait(2,100,18,now_ms()+20,&cancel)==RingTimeout);
    Check(InjectedCalls>0 && access(marker,F_OK)==0);
    Check(unlink(marker)==0);
    InjectedIo=InjectNone; InjectedCalls=0;
}

static void test_authenticated_marker_and_cancel(const char *directory)
{
    _Atomic uint32_t cancel=0;
    char marker[512],expected[256],actual[256]={0};
    Check(snprintf(marker,sizeof marker,"%s/cancel",directory)>0);
    Check(setenv("WADDLE_OPENCL_CANCEL_GATE",marker,1)==0);
    size_t before=open_fd_count();
    gate_thread_args_t args={.cancel=&cancel,.deadline_ms=now_ms()+2000,.result=RingClosed};
    pthread_t thread;
    Check(pthread_create(&thread,NULL,gate_thread,&args)==0);
    uint64_t end=now_ms()+1000;
    struct stat info;
    while (lstat(marker,&info)<0 && now_ms()<end) (void)poll(NULL,0,1);
    int expected_bytes=snprintf(expected,sizeof expected,
        "WADDLE_OPENCL_CANCEL_GATE_V1\nsession=%016" PRIx64 "\nsequence=77\nopcode=18\npid=%ld\n",
        UINT64_C(0x1234abcd),(long)getpid());
    Check(expected_bytes>0 && (size_t)expected_bytes<sizeof actual);
    size_t actual_bytes=0;
    uint64_t witness_deadline=now_ms()+1000;
    int complete=0;
    do {
        FILE *file=fopen(marker,"rb");
        if (file) {
            actual_bytes=fread(actual,1,sizeof actual-1,file);
            int read_error=ferror(file);
            int close_result=fclose(file);
            complete=!read_error && close_result==0 &&
                actual_bytes==(size_t)expected_bytes &&
                memcmp(actual,expected,actual_bytes)==0;
        }
        if (!complete) (void)poll(NULL,0,1);
    } while (!complete && now_ms()<witness_deadline);
    Check(complete);
    Check(lstat(marker,&info)==0 && S_ISREG(info.st_mode) && (info.st_mode&0777)==0600);
    size_t while_waiting=open_fd_count();
    uint64_t close_deadline=now_ms()+200;
    while (while_waiting>before && now_ms()<close_deadline) {
        (void)poll(NULL,0,1);
        while_waiting=open_fd_count();
    }
    Check(while_waiting==before);
    atomic_store_explicit(&cancel,1,memory_order_release);
    Check(pthread_join(thread,NULL)==0);
    Check(args.result==RingCancelled);
    Check(open_fd_count()==before);
}

int main(void)
{
    char directory_template[]="/tmp/waddle-opencl-gate-XXXXXX";
    char *directory=mkdtemp(directory_template);
    Check(directory!=NULL);
    size_t initial_fds=open_fd_count();
    test_invalid_configuration();
    test_creation_failures(directory);
    test_deadline(directory);
    test_expired_deadline_and_clock_failure(directory);
    test_injected_io_paths(directory);
    test_authenticated_marker_and_cancel(directory);
    Check(open_fd_count()==initial_fds);
    char path[512];
    const char *names[]={"collision","symlink","timeout","cancel","clock-failure"};
    for (size_t index=0;index<sizeof names/sizeof names[0];++index) {
        Check(snprintf(path,sizeof path,"%s/%s",directory,names[index])>0);
        Check(unlink(path)==0);
    }
    Check(rmdir(directory)==0);
    Check(unsetenv("WADDLE_OPENCL_CANCEL_GATE")==0);
    return 0;
}

#undef Check
