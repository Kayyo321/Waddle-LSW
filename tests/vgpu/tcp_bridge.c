/** @file tcp_bridge.c @brief Actual controller fault harness; no fabricated receiver success. */
#include "waddle/venus_tcp.h"
#include "waddle/venus_guest.h"
#include "waddle/venus_worker.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
static const char *fault_name;
static unsigned fault_call=1,fault_seen;
static int clock_once,lost_reaping_test,destroy_lost_test;
static int fail(const char *name)
{
    if (!fault_name || strcmp(name,fault_name)) return 0;
    return ++fault_seen==fault_call;
}
#define main controller_main
#include "../../src/vgpu/tcp_bridge_main.c"
#undef main
int __real_open(const char *,int,...);
int __wrap_open(const char *path,int flags,...)
{
    mode_t mode=0;
    if (flags&O_CREAT) { va_list arguments; va_start(arguments,flags); mode=(mode_t)va_arg(arguments,int); va_end(arguments); }
    if (fail("open")) { errno=EIO; return -1; }
    return __real_open(path,flags,mode);
}
int __real_fstat(int,struct stat *);
int __wrap_fstat(int fd,struct stat *information)
{
    if (fail("fstat")) { errno=EIO; return -1; }
    int result=__real_fstat(fd,information);
    if (!result && fail("owner")) information->st_uid=(uid_t)(geteuid()+1);
    return result;
}
ssize_t __real_read(int,void *,size_t);
ssize_t __wrap_read(int fd,void *bytes,size_t length)
{
    if (fail("read_interrupt")) { errno=EINTR; return -1; }
    if (fail("read")) { errno=EIO; return -1; }
    return __real_read(fd,bytes,length);
}
ssize_t __real_write(int,const void *,size_t);
ssize_t __wrap_write(int fd,const void *bytes,size_t length)
{
    if (fail("write_interrupt")) { errno=EINTR; return -1; }
    if (fail("write")) { errno=EIO; return -1; }
    if (fail("write_zero")) return 0;
    return __real_write(fd,bytes,length);
}
int __real_close(int);
int __wrap_close(int fd)
{
    int result=__real_close(fd); /* Even reported close errors actually retire the native owner. */
    if (fail("close")) { errno=EIO; return -1; }
    return result;
}
int __real_fsync(int);
int __wrap_fsync(int fd) { if (fail("fsync")) { errno=EIO; return -1; } return __real_fsync(fd); }
int __real_setitimer(int,const struct itimerval *,struct itimerval *);
int __wrap_setitimer(int which,const struct itimerval *value,struct itimerval *old)
{ if (fail("timer")) { errno=EIO; return -1; } return __real_setitimer(which,value,old); }
int __real_sigaction(int,const struct sigaction *,struct sigaction *);
int __wrap_sigaction(int signal_number,const struct sigaction *action,struct sigaction *old)
{ if (fail("signal")) { errno=EIO; return -1; } return __real_sigaction(signal_number,action,old); }
int __real_memfd_create(const char *,unsigned);
int __wrap_memfd_create(const char *name,unsigned flags)
{ if (fail("memfd")) { errno=EIO; return -1; } return __real_memfd_create(name,flags); }
int __real_ftruncate(int,off_t);
int __wrap_ftruncate(int fd,off_t size)
{ if (fail("truncate")) { errno=EIO; return -1; } return __real_ftruncate(fd,size); }
void *__real_mmap(void *,size_t,int,int,int,off_t);
void *__wrap_mmap(void *address,size_t size,int protections,int flags,int fd,off_t offset)
{ if (fail("map")) { errno=ENOMEM; return MAP_FAILED; } return __real_mmap(address,size,protections,flags,fd,offset); }
int __real_munmap(void *,size_t);
int __wrap_munmap(void *address,size_t size)
{ int result=__real_munmap(address,size); if (fail("unmap")) { errno=EIO; return -1; } return result; }
int __real_socketpair(int,int,int,int[2]);
int __wrap_socketpair(int domain,int type,int protocol,int pair[2])
{ if (fail("socketpair")) { errno=EIO; return -1; } return __real_socketpair(domain,type,protocol,pair); }
uint64_t __real_venus_tcp_now_ms(void);
uint64_t __wrap_venus_tcp_now_ms(void)
{ if (clock_once) { clock_once=0; return 0; } if (fail("clock")) return 0; if (fail("clock_overflow")) return UINT64_MAX; return __real_venus_tcp_now_ms(); }
#define StatusWrapper(name,parameters,arguments) \
venus_ring_status_t __real_##name parameters; \
venus_ring_status_t __wrap_##name parameters { if (fail(#name)) return RingCorrupt; return __real_##name arguments; }
StatusWrapper(venus_region_init,(void *mapping,size_t bytes,uint32_t capacity),(mapping,bytes,capacity))
StatusWrapper(venus_session_init,(venus_session_t *session,venus_session_role_t role,void *mapping,size_t bytes,uint32_t flag),(session,role,mapping,bytes,flag))
StatusWrapper(venus_channel_init,(venus_channel_t *channel,venus_session_t *session,int fd,const _Atomic uint32_t *cancel),(channel,session,fd,cancel))
venus_ring_status_t __real_venus_worker_create(venus_worker_t *,const char *,int,int);
venus_ring_status_t __wrap_venus_worker_create(venus_worker_t *worker,const char *path,int mapping,int stream)
{
    if (fail("venus_worker_create")) return RingCorrupt;
    venus_ring_status_t status=__real_venus_worker_create(worker,path,mapping,stream);
    if (status==RingOk && fail("budget_first")) clock_once=1;
    return status;
}
StatusWrapper(venus_channel_deadline,(venus_channel_t *channel,uint32_t budget),(channel,budget))
StatusWrapper(venus_channel_handshake,(venus_channel_t *channel),(channel))
venus_ring_status_t __real_venus_rpc_init(venus_rpc_t *,venus_channel_t *,void *,size_t);
venus_ring_status_t __wrap_venus_rpc_init(venus_rpc_t *rpc,venus_channel_t *channel,void *scratch,size_t bytes)
{
    if (fail("venus_rpc_init")) return RingCorrupt;
    venus_ring_status_t status=__real_venus_rpc_init(rpc,channel,scratch,bytes);
    if (status==RingOk && fail("budget_capture")) clock_once=1;
    return status;
}
venus_ring_status_t __real_venus_rpc_exchange(venus_rpc_t *,const venus_request_t *,const void *,size_t,venus_request_t *,void *,size_t,uint32_t);
venus_ring_status_t __wrap_venus_rpc_exchange(venus_rpc_t *rpc,const venus_request_t *request,const void *input,size_t length,venus_request_t *response,void *output,size_t capacity,uint32_t budget)
{
    if (fail("venus_rpc_exchange")) return RingCorrupt;
    venus_ring_status_t status=__real_venus_rpc_exchange(rpc,request,input,length,response,output,capacity,budget);
    if (status==RingOk && fail("cap_status")) response->status=RequestInvalid;
    if (status==RingOk && fail("cap_extent")) response->payload_bytes=0;
    return status;
}
StatusWrapper(venus_capabilities_decode,(venus_capabilities_t *output,const void *bytes,size_t length),(output,bytes,length))
StatusWrapper(venus_capabilities_compatible,(const venus_capabilities_t *capabilities),(capabilities))
venus_ring_status_t __real_venus_guest_init(venus_guest_t *,venus_rpc_t *,uint32_t);
venus_ring_status_t __wrap_venus_guest_init(venus_guest_t *guest,venus_rpc_t *rpc,uint32_t timeout)
{
    if (fail("venus_guest_init")) return RingCorrupt;
    venus_ring_status_t status=__real_venus_guest_init(guest,rpc,timeout);
    if (status==RingOk && fail("cap_changed")) ((uint8_t *)&guest->capabilities)[0]^=1;
    return status;
}
StatusWrapper(venus_tcp_server_negotiate,(venus_tcp_server_t *server,venus_guest_t *guest,const uint8_t *capabilities,const _Atomic uint32_t *cancel),(server,guest,capabilities,cancel))
StatusWrapper(venus_tcp_server_ack_retired,(venus_tcp_server_t *server,const _Atomic uint32_t *cancel),(server,cancel))
StatusWrapper(venus_tcp_socket_listen,(venus_tcp_socket_t *listener,uint32_t port,uint32_t *actual),(listener,port,actual))
StatusWrapper(venus_tcp_socket_accept,(venus_tcp_socket_t *listener,venus_tcp_socket_t *accepted,uint64_t deadline,const _Atomic uint32_t *cancel),(listener,accepted,deadline,cancel))
StatusWrapper(venus_tcp_server_authenticate,(venus_tcp_server_t *server,venus_tcp_socket_t *accepted,const uint8_t *token,uint32_t timeout,const _Atomic uint32_t *cancel),(server,accepted,token,timeout,cancel))
venus_ring_status_t __real_venus_tcp_server_step(venus_tcp_server_t *,venus_guest_t *,const _Atomic uint32_t *);
venus_ring_status_t __wrap_venus_tcp_server_step(venus_tcp_server_t *server,venus_guest_t *guest,const _Atomic uint32_t *cancel)
{
    if (fail("forward_cancel")) raise(SIGTERM);
    if (fail("venus_tcp_server_step")) return RingCorrupt;
    return __real_venus_tcp_server_step(server,guest,cancel);
}
venus_ring_status_t __real_venus_worker_poll(venus_worker_t *);
venus_ring_status_t __wrap_venus_worker_poll(venus_worker_t *worker)
{
    if (fail("venus_worker_poll")) return RingCorrupt;
    if (fail("lost_reaping_destroy")) { destroy_lost_test=1; return RingCorrupt; }
    venus_ring_status_t status=__real_venus_worker_poll(worker);
    if (status==RingClosed && fail("lost_reaping_proof")) {
        lost_reaping_test=1;worker->exit_status=-1; /* Actual child already reaped; failure-only observation loss. */
        return RingCorrupt;
    }
    return status;
}
venus_ring_status_t __real_venus_worker_destroy(venus_worker_t *,uint32_t);
venus_ring_status_t __wrap_venus_worker_destroy(venus_worker_t *worker,uint32_t timeout)
{
    if (fail("venus_worker_destroy")) return RingCorrupt;
    venus_ring_status_t status=__real_venus_worker_destroy(worker,timeout);
    if (status==RingOk && destroy_lost_test) {
        destroy_lost_test=0;lost_reaping_test=1;
        worker->exited=1;worker->exit_status=-1;
        return RingCorrupt;
    }
    return status;
}
#undef StatusWrapper
int __real_nanosleep(const struct timespec *,struct timespec *);
int __wrap_nanosleep(const struct timespec *delay,struct timespec *remaining)
{
#ifdef TcpControllerCoverage
    if (lost_reaping_test) {
        extern void __gcov_dump(void);
        __gcov_dump(); /* Persist actual retention coverage before supervised failure termination. */
        lost_reaping_test=0;
    }
#endif
    return __real_nanosleep(delay,remaining);
}
static unsigned descriptors(void)
{
    DIR *directory=opendir("/proc/self/fd"); assert(directory); unsigned count=0;
    while (readdir(directory)) count++;
    assert(closedir(directory)==0); return count;
}
static void helper_tests(void)
{
    unsigned baseline=descriptors();
    char directory[]="/tmp/waddle_tcp_controller_XXXXXX"; assert(mkdtemp(directory));
    char path[512]; assert(snprintf(path,sizeof path,"%s/private",directory)>0);
    venus_tcp_config_t config;
    assert(private_config(path,&config)==RingClosed);
    assert(private_config(directory,&config)==RingInvalid);
    cancel_controller(SIGTERM);assert(atomic_load(&controller_cancel)==1);atomic_store(&controller_cancel,0);
    const char Config[]="{\"version\":1,\"host\":\"127.0.0.1\",\"port\":12345,\"token\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"exchange_timeout_ms\":1000,\"icd_path\":\"C:\\\\fixture.dll\"}";
    assert(private_result(path,Config,sizeof Config-1)==RingOk);
    assert(private_result(path,"x",1)==RingClosed); /* Existing output is never overwritten. */
    assert(private_config(path,&config)==RingOk);
    fault_name="read_interrupt";fault_seen=0; assert(private_config(path,&config)==RingOk);
    fault_name="read";fault_seen=0; assert(private_config(path,&config)==RingClosed);
    fault_name="fstat";fault_seen=0; assert(private_config(path,&config)==RingInvalid);
    fault_name="owner";fault_seen=0; assert(private_config(path,&config)==RingInvalid);
    fault_name="close";fault_seen=0; assert(private_config(path,&config)==RingClosed);
    fault_name=NULL;assert(chmod(path,0644)==0); assert(private_config(path,&config)==RingInvalid); assert(chmod(path,0600)==0);
    char link_path[512]; assert(snprintf(link_path,sizeof link_path,"%s/link",directory)>0);
    assert(link(path,link_path)==0); assert(private_config(path,&config)==RingInvalid);assert(unlink(link_path)==0);
    assert(symlink(path,link_path)==0);assert(private_config(link_path,&config)==RingClosed);assert(unlink(link_path)==0);
    int fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0); assert(close(fd)==0);assert(private_config(path,&config)==RingInvalid);
    fd=open(path,O_WRONLY);assert(fd>=0);assert(ftruncate(fd,VenusTcpMaxConfigBytes+1)==0);assert(close(fd)==0);assert(private_config(path,&config)==RingInvalid);assert(unlink(path)==0);
    const char *Faults[]={"write_interrupt","write","write_zero","fsync","close"};
    for (size_t index=0;index<sizeof Faults/sizeof *Faults;index++) {
        fault_name=Faults[index];fault_seen=0;
        assert(private_result(path,"abc",3)==(index==0 ? RingOk : RingClosed));
        fault_name=NULL;assert(unlink(path)==0);
    }
    assert(timer_until(venus_tcp_now_ms())==RingTimeout);
    assert(remaining_budget(venus_tcp_now_ms())==0);
    assert(remaining_budget(venus_tcp_now_ms()+120000)==60000);
    fault_name="clock";fault_seen=0;assert(timer_until(UINT64_MAX)==RingClosed);
    fault_seen=0;assert(remaining_budget(UINT64_MAX)==0);
    fault_name=NULL;fd=-1;assert(release_fd(&fd)==0);
    assert(rmdir(directory)==0);assert(descriptors()==baseline);
    puts("TCP controller bounded helpers PASS: actual descriptor baseline exact");
}
/** @brief Run actual controller with optional one-shot native fault; test-only environment.
 * @param[in] argc/argv Borrowed actual CLI paths or --helpers. @return Actual controller
 * exit code. Faults manufacture failures only; all real acquisitions/owners remain real.
 */
int main(int argc,char **argv)
{
    if (argc==3 && !strcmp(argv[1],"--client")) {
        venus_tcp_config_t config={0}; venus_tcp_client_t client={0};
        venus_ring_status_t status=private_config(argv[2],&config);
        if (status==RingOk) status=venus_tcp_client_init(&client,&config,NULL);
        venus_tcp_scrub(&config,sizeof config);
        /* Independent literal pinned command137 preceded by command178 reply-stream binding.
         * Actual CPU receiver replies are required; this creates no Vulkan GPU object. */
        static const uint8_t Command[52]={178,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
            1,0,0,0,0,0,0,0,0,0,0,0,32,0,0,0,0,0,0,0,
            137,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0};
        static const uint8_t Reply[16]={137,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0};
        for (unsigned iteration=0;iteration<8 && status==RingOk;iteration++) {
            venus_request_t request={.kind=RequestSubmit,.payload_bytes=sizeof Command},response={0};
            status=venus_tcp_client_exchange(&client,&request,Command,sizeof Command,&response,NULL,0);
            if (status!=RingOk || !response.argument_zero) { status=RingCorrupt; break; }
            request=(venus_request_t){.kind=RequestPoll};
            uint64_t deadline=venus_tcp_now_ms()+5000;
            do {
                status=venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0);
                if (status==RingAgain) { struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,NULL); }
            } while (status==RingAgain && venus_tcp_now_ms()<deadline);
            if (status!=RingOk) break;
            uint8_t reply[32]={0};request=(venus_request_t){.kind=RequestReply,.argument_one=sizeof reply};
            status=venus_tcp_client_exchange(&client,&request,NULL,0,&response,reply,sizeof reply);
            if (status==RingOk && (response.payload_bytes!=sizeof reply || memcmp(reply,Reply,sizeof Reply) || !reply[18])) status=RingCorrupt;
        }
        if (status==RingOk) status=venus_tcp_client_retire(&client,NULL);
        venus_tcp_client_free(&client);
        return status==RingOk ? 0 : 1;
    }
    fault_name=getenv("WADDLE_TCP_TEST_FAULT");
    const char *call=getenv("WADDLE_TCP_TEST_CALL");if (call) fault_call=(unsigned)strtoul(call,NULL,10);
    if (argc==2 && !strcmp(argv[1],"--helpers")) { helper_tests(); return 0; }
    unsigned baseline=descriptors();
    int result=controller_main(argc,argv);
    assert(descriptors()==baseline);
    int status=0;assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    return result;
}
