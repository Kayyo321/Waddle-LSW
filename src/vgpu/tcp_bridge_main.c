/** @file tcp_bridge_main.c @brief Private Linux TCP controller for an actual isolated Venus receiver. */
#include "waddle/venus_tcp.h"
#include "waddle/venus_guest.h"
#include "waddle/venus_worker.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Signal cancellation requires a lock-free integer");
static _Atomic uint32_t controller_cancel;
static void cancel_controller(int signal_number)
{ (void)signal_number; atomic_store_explicit(&controller_cancel,1,memory_order_release); }
static venus_ring_status_t timer_until(uint64_t deadline)
{
    uint64_t now=venus_tcp_now_ms();
    if (!now) return RingClosed;
    if (now>=deadline) return RingTimeout;
    uint64_t remaining=deadline-now;
    struct itimerval timer={.it_value={.tv_sec=(time_t)(remaining/1000),.tv_usec=(suseconds_t)(remaining%1000)*1000}};
    return setitimer(ITIMER_REAL,&timer,NULL) ? RingClosed : RingOk;
}
static venus_ring_status_t private_config(const char *path,venus_tcp_config_t *config)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return RingClosed;
    struct stat information;
    uint8_t bytes[VenusTcpMaxConfigBytes+1]; size_t received=0;
    venus_ring_status_t status=RingInvalid;
    if (fstat(fd,&information) || !S_ISREG(information.st_mode) || information.st_uid!=geteuid() ||
        (information.st_mode&0777)!=0600 || information.st_nlink!=1 ||
        information.st_size<1 || information.st_size>VenusTcpMaxConfigBytes) goto cleanup;
    while (received<sizeof bytes) {
        ssize_t count=read(fd,bytes+received,sizeof bytes-received);
        if (count<0 && errno==EINTR) continue;
        if (count<0) { status=RingClosed; goto cleanup; }
        if (!count) break;
        received+=(size_t)count;
    }
    status=venus_tcp_config_decode(config,bytes,received);
cleanup:
    venus_tcp_scrub(bytes,sizeof bytes);
    if (close(fd)) status=RingClosed;
    return status;
}
static venus_ring_status_t private_result(const char *path,const char *bytes,size_t length)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0) return RingClosed;
    venus_ring_status_t status=RingOk; size_t sent=0;
    while (sent<length) {
        ssize_t count=write(fd,bytes+sent,length-sent);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) { status=RingClosed; break; }
        sent+=(size_t)count;
    }
    if (fsync(fd)) status=RingClosed;
    if (close(fd)) status=RingClosed;
    return status;
}
static uint32_t remaining_budget(uint64_t deadline)
{
    uint64_t now=venus_tcp_now_ms();
    if (!now || now>=deadline) return 0;
    uint64_t remaining=deadline-now;
    return remaining>60000 ? 60000 : (uint32_t)remaining;
}
static int release_fd(int *fd)
{
    if (*fd<0) return 0;
    int owned=*fd; *fd=-1;
    return close(owned);
}
static _Noreturn void retain_lost_reaping_proof(void)
{
    fputs("TCP controller retains views after lost worker reaping proof\n",stderr);
    for (;;) { struct timespec pause={.tv_sec=1}; (void)nanosleep(&pause,NULL); }
}
static int retire_worker(venus_worker_t *worker,venus_session_t *session,
    venus_channel_t *channel,int orderly,int *exit_status)
{
    if (!worker->process_id) return 0;
    int forced=0;
    if (channel->initialized && session->state==SessionReady) {
        if (venus_channel_deadline(channel,5000)==RingOk)
            (void)venus_channel_stop(channel,orderly ? StopDisconnect : StopProtocol);
    }
    /* Closing lifecycle flags retires access, but keeps every borrowed view alive. */
    venus_session_close(session,orderly ? StopDisconnect : StopProtocol);
    uint64_t deadline=venus_tcp_now_ms()+5000;
    for (;;) {
        venus_ring_status_t status=venus_worker_poll(worker);
        if (status==RingClosed) { *exit_status=worker->exit_status; break; }
        if (status!=RingAgain || !remaining_budget(deadline)) { forced=1; break; }
        struct timespec pause={.tv_nsec=1000000}; (void)nanosleep(&pause,NULL);
    }
    /* ECHILD invalidates the exclusive waiter contract and exact group proof. */
    if (worker->exited && worker->exit_status<0) retain_lost_reaping_proof();
    /* Never release mapping/channel or fake retirement when reaping is incomplete. */
    while (venus_worker_destroy(worker,5000)!=RingOk) {
        if (worker->exited && worker->exit_status<0) retain_lost_reaping_proof();
        forced=1;
        fputs("TCP controller retains unreaped worker ownership\n",stderr);
        struct timespec pause={.tv_sec=1}; (void)nanosleep(&pause,NULL);
    }
    return forced || (orderly && (*exit_status<0 || !WIFEXITED(*exit_status) || WEXITSTATUS(*exit_status)!=0));
}
/** @brief Serve one authenticated actual receiver then prove retirement before Ack.
 * @param[in] argc Exactly five including program name. @param[in] argv Borrowed
 * paths only: private config, absolute trusted immutable worker, fresh ready/result files.
 * @return Zero only for normal actual receiver retirement and Ack; one failed launch;
 * two malformed CLI. No heap/threads/secret output. Owns and deterministically releases
 * sockets, memfd/mmap, lifecycle borrows and exact worker; retains process on unreaped failure.
 */
int main(int argc,char **argv)
{
    if (argc!=5) { fputs("Usage: waddle_vgpu_tcp_bridge private_config absolute_worker new_ready new_result\n",stderr); return 2; }
    int result=1,orderly=0,retired=1,exit_status=-1,mapping_fd=-1,streams[2]={-1,-1};
    pid_t initial_pid=0; uint64_t session_id=0; uint32_t port=0;
    void *mapping=MAP_FAILED; char executable[PATH_MAX],report[256];
    uint8_t scratch[131072],capabilities[VenusCapabilitiesBytes];
    venus_tcp_config_t config={0}; venus_tcp_socket_t listener={0},accepted={0}; venus_tcp_server_t server={0};
    venus_worker_t worker={0}; venus_session_t session={0}; venus_channel_t channel={0}; venus_rpc_t rpc={0}; venus_guest_t guest={0};
    venus_ring_status_t status=RingInvalid;
    const char *stage="configuration";
    uint64_t started=venus_tcp_now_ms();
    if (!started || started>UINT64_MAX-180000) { status=RingClosed; goto cleanup; }
    uint64_t process_deadline=started+180000;
    struct sigaction action={.sa_handler=cancel_controller}; sigemptyset(&action.sa_mask);
    struct sigaction child_action={.sa_handler=SIG_DFL}; sigemptyset(&child_action.sa_mask);
    if (sigaction(SIGCHLD,&child_action,NULL) || sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL) || sigaction(SIGALRM,&action,NULL)) { status=RingClosed; goto cleanup; }
    status=timer_until(process_deadline); if (status!=RingOk) goto cleanup;
    status=private_config(argv[1],&config); if (status!=RingOk) goto cleanup;
    if (argv[2][0]!='/' || !realpath(argv[2],executable)) { status=RingInvalid; goto cleanup; }
    stage="listener"; status=venus_tcp_socket_listen(&listener,config.port,&port); if (status!=RingOk) goto cleanup;
    int length=snprintf(report,sizeof report,"port=%u\n",port);
    if (length<=0 || (size_t)length>=sizeof report) { status=RingClosed; goto cleanup; }
    status=private_result(argv[3],report,(size_t)length); if (status!=RingOk) goto cleanup;
    status=venus_tcp_socket_accept(&listener,&accepted,started+60000,&controller_cancel); if (status!=RingOk) goto cleanup;
    venus_tcp_socket_close(&listener);
    stage="authentication"; status=venus_tcp_server_authenticate(&server,&accepted,config.token,config.exchange_timeout_ms,&controller_cancel);
    venus_tcp_scrub(config.token,sizeof config.token); if (status!=RingOk) goto cleanup;
    session_id=server.session;
    status=timer_until(server.handshake_deadline); if (status!=RingOk) goto cleanup;
    stage="actual worker";
    mapping_fd=memfd_create("tcp-venus-receiver",MFD_CLOEXEC);
    if (mapping_fd<0 || ftruncate(mapping_fd,4096)) { status=RingClosed; goto cleanup; }
    mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,mapping_fd,0);
    if (mapping==MAP_FAILED) { status=RingClosed; goto cleanup; }
    status=venus_region_init(mapping,4096,64); if (status!=RingOk) goto cleanup;
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0,streams)) { status=RingClosed; goto cleanup; }
    status=venus_session_init(&session,SessionGuest,mapping,4096,0); if (status!=RingOk) goto cleanup;
    status=venus_channel_init(&channel,&session,streams[0],&controller_cancel); if (status!=RingOk) goto cleanup;
    status=venus_worker_create(&worker,executable,mapping_fd,streams[1]); if (status!=RingOk) goto cleanup;
    initial_pid=worker.process_id; retired=0;
    if (release_fd(&streams[1])) { status=RingClosed; goto cleanup; }
    uint32_t budget=remaining_budget(server.handshake_deadline);
    if (!budget) { status=RingTimeout; goto cleanup; }
    status=venus_channel_deadline(&channel,budget); if (status!=RingOk) goto cleanup;
    status=venus_channel_handshake(&channel); if (status!=RingOk) goto cleanup;
    status=venus_rpc_init(&rpc,&channel,scratch,sizeof scratch); if (status!=RingOk) goto cleanup;
    stage="actual capset capture";
    budget=remaining_budget(server.handshake_deadline); if (!budget) { status=RingTimeout; goto cleanup; }
    venus_request_t request={.kind=RequestCapabilities},response;
    status=venus_rpc_exchange(&rpc,&request,NULL,0,&response,capabilities,sizeof capabilities,budget);
    if (status!=RingOk) goto cleanup;
    if (response.status!=RequestSuccess || response.payload_bytes!=sizeof capabilities) { status=RingCorrupt; goto cleanup; }
    venus_capabilities_t actual;
    status=venus_capabilities_decode(&actual,capabilities,sizeof capabilities); if (status!=RingOk) goto cleanup;
    status=venus_capabilities_compatible(&actual); if (status!=RingOk) goto cleanup;
    stage="actual guest negotiation";
    status=venus_guest_init(&guest,&rpc,config.exchange_timeout_ms); if (status!=RingOk) goto cleanup;
    if (memcmp(&actual,&guest.capabilities,sizeof actual)) { status=RingCorrupt; goto cleanup; }
    status=venus_tcp_server_negotiate(&server,&guest,capabilities,&controller_cancel); if (status!=RingOk) goto cleanup;
    status=timer_until(process_deadline); if (status!=RingOk) goto cleanup;
    stage="actual forwarding";
    while ((status=venus_tcp_server_step(&server,&guest,&controller_cancel))==RingOk) {}
    orderly=status==RingClosed && server.eof && server.socket.received_eof;
    if (orderly) status=RingOk;
cleanup:
    if (retire_worker(&worker,&session,&channel,orderly,&exit_status)) { result=1; if (status==RingOk) status=RingCorrupt; }
    retired=1;
    venus_guest_free(&guest); venus_rpc_free(&rpc); venus_channel_free(&channel); venus_region_detach(&session.region);
    if (mapping!=MAP_FAILED && munmap(mapping,4096)) { status=RingClosed; }
    if (release_fd(&streams[0])) status=RingClosed;
    if (release_fd(&streams[1])) status=RingClosed;
    if (release_fd(&mapping_fd)) status=RingClosed;
    if (orderly && status==RingOk) { stage="retirement Ack"; status=venus_tcp_server_ack_retired(&server,&controller_cancel); }
    if (status==RingOk && orderly) result=0;
    venus_tcp_server_free(&server); venus_tcp_socket_close(&accepted); venus_tcp_socket_close(&listener);
    venus_tcp_scrub(&config,sizeof config); venus_tcp_scrub(scratch,sizeof scratch); venus_tcp_scrub(capabilities,sizeof capabilities);
    struct itimerval stopped={0}; if (setitimer(ITIMER_REAL,&stopped,NULL)) { status=RingClosed; result=1; }
    int report_bytes=snprintf(report,sizeof report,"session=%016" PRIx64 "\nworker_pid=%jd\nworker_retired=%d\nstatus=%d\nworker_exit_status=%d\n",session_id,(intmax_t)initial_pid,retired,(int)status,exit_status);
    if (report_bytes<=0 || (size_t)report_bytes>=sizeof report || private_result(argv[4],report,(size_t)report_bytes)!=RingOk) result=1;
    if (result) fprintf(stderr,"TCP controller failed at %s (status%d)\n",stage,(int)status);
    return result;
}
