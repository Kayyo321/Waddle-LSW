/** @file tcp_receiver_server.c @brief Actual socket/server owner units; explicit synthetic frontend, no GPU/worker credit. */
#include "waddle/venus_tcp.h"
#include "waddle/venus_guest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#endif
/** @brief Borrowed public synthetic byte oracle; no ownership or allocation. */
int venus_tcp_wire_oracle(unsigned kind,uint8_t *output,size_t capacity);
typedef enum server_mode_t { Normal,TokenBad,HelloBad,HelloPartial,ProfileBad,ProfilePartial,
    CapsBad,CapsUnsupported,CapsMismatch,HeaderPartial,HeaderBad,SequenceBad,SequenceMax,
    CommandTooLong,PayloadPartial,ReadFull,WriteFull,CommandOpcode,ReplyAgain,ReplyInvalid,ReplyLimit,
    ReplyCorrupt,ReplyClosed,ReplyCancelled,ReplyTimeout,GuestNoResponse,GuestFalseSuccess,
    GuestBadResponse,GuestStatusMismatch,GuestNoDirection,GuestNoSequence,GuestOversized,StepCancel,StepTimeout,LocalInvalid,
    MaxLast,AuthRandomFail,AuthClockFail,HelloSendFail,AckSendFail,HeaderSendFail,
    PayloadSendFail,StepClockFail,RetireSendFail,RetireClockFail,BeforeRpcClockFail,
    BeforeRpcTimeout,StepReceiveFail } server_mode_t;
typedef struct fixture_t { venus_tcp_socket_t listener; uint32_t port; server_mode_t mode; unsigned calls; } fixture_t;
static _Thread_local fixture_t *active_fixture;
#ifdef TcpPeerFaultTests
static _Thread_local unsigned clock_failure,random_failure,send_failure,send_count,before_rpc_clock,before_rpc_delay,receive_failure;
static void sleep_ms(unsigned milliseconds);
ssize_t __real_getrandom(void *bytes,size_t length,unsigned flags);
ssize_t __wrap_getrandom(void *bytes,size_t length,unsigned flags)
{ if (random_failure) { random_failure=0; errno=EIO; return -1; } return __real_getrandom(bytes,length,flags); }
int __real_clock_gettime(clockid_t id,struct timespec *now);
int __wrap_clock_gettime(clockid_t id,struct timespec *now)
{ if (clock_failure && !--clock_failure) { errno=EIO; return -1; } return __real_clock_gettime(id,now); }
ssize_t __real_recv(int fd,void *bytes,size_t length,int flags);
ssize_t __wrap_recv(int fd,void *bytes,size_t length,int flags)
{
    if (receive_failure) { receive_failure=0; errno=EIO; return -1; }
    ssize_t count=__real_recv(fd,bytes,length,flags);
    if (count==64 && before_rpc_clock) { before_rpc_clock=0; clock_failure=1; }
    if (count==64 && before_rpc_delay) { before_rpc_delay=0; sleep_ms(5); }
    return count;
}
ssize_t __real_send(int fd,const void *bytes,size_t length,int flags);
ssize_t __wrap_send(int fd,const void *bytes,size_t length,int flags)
{ if (++send_count==send_failure) { send_failure=0; errno=EIO; return -1; } return __real_send(fd,bytes,length,flags); }
#endif
static unsigned resource_count(void)
{
#ifdef _WIN32
    DWORD count=0; assert(GetProcessHandleCount(GetCurrentProcess(),&count)); return count;
#else
    DIR *directory=opendir("/proc/self/fd"); assert(directory); unsigned count=0;
    struct dirent *entry; while ((entry=readdir(directory))) if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    assert(!closedir(directory)); return count;
#endif
}
static void sleep_ms(unsigned milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay={.tv_sec=milliseconds/1000,.tv_nsec=(long)(milliseconds%1000)*1000000}; nanosleep(&delay,NULL);
#endif
}
static const uint32_t WireStatuses[]={RequestAgain,RequestInvalid,RequestLimit,RequestCorrupt,RequestClosed,RequestCancelled,RequestTimeout};
static const venus_ring_status_t RingStatuses[]={RingAgain,RingInvalid,RingLimit,RingCorrupt,RingClosed,RingCancelled,RingTimeout};
/** @brief Explicit mock frontend boundary for TCP unit faults, never linked into production.
 * @param[in,out] guest Borrowed synthetic zero-owner fixture. @param[in] request Borrowed sequence-zero actual decoded frame.
 * @param[in] input Borrowed immutable bytes[length]. @param[in] length Exact declared bytes.
 * @param[out] response Owned unit envelope. @param[out] output Owned unit output[capacity].
 * @param[in] capacity Actual output extent. @param[in] timeout_ms Actual remaining budget.
 * @return Deliberate unit result; no GPU, RPC, allocation or retained input. Sole fixture thread.
 */
venus_ring_status_t venus_guest_exchange_timeout(venus_guest_t *guest,const venus_request_t *request,
    const void *input,size_t length,venus_request_t *response,void *output,size_t capacity,uint32_t timeout_ms)
{
    fixture_t *fixture=active_fixture; assert(fixture && guest && !request->sequence && timeout_ms && timeout_ms<=guest->timeout_ms);
    assert(request->payload_bytes==length); ++fixture->calls;
    *response=(venus_request_t){.kind=request->kind,.direction=1,.sequence=99};
    server_mode_t mode=fixture->mode;
    if (mode==GuestNoResponse || mode==GuestFalseSuccess) { memset(response,0,sizeof *response); return mode==GuestFalseSuccess ? RingOk : RingTimeout; }
    if (mode>=ReplyAgain && mode<=ReplyTimeout) { response->status=WireStatuses[mode-ReplyAgain]; return RingStatuses[mode-ReplyAgain]; }
    if (mode==GuestStatusMismatch) return RingInvalid;
    if (mode==GuestBadResponse) { response->kind=RequestCapabilities; return RingOk; }
    if (mode==GuestNoDirection) { response->direction=0; return RingOk; }
    if (mode==GuestNoSequence) { response->sequence=0; return RingOk; }
    if (mode==GuestOversized) { response->payload_bytes=4097; return RingOk; }
    if (request->kind==RequestSubmit) { assert(length==44 && ((const uint8_t *)input)[0]==178 && ((const uint8_t *)input)[36]==173); response->argument_zero=99; }
    if (request->kind==RequestWrite) { assert(length==4096); for (size_t index=0;index<length;++index) assert(((const uint8_t *)input)[index]==(uint8_t)(index*13+7)); }
    if (request->kind==RequestRead) { assert(capacity==4096); response->payload_bytes=4096; for (size_t index=0;index<capacity;++index) ((uint8_t *)output)[index]=(uint8_t)(index*17+9); }
    return RingOk;
}
static void receive_bytes(venus_tcp_socket_t *socket,void *bytes,size_t length)
{ size_t received=0; assert(venus_tcp_socket_receive(socket,bytes,length,&received,venus_tcp_now_ms()+1000,NULL)==RingOk && received==length); }
static void send_bytes(venus_tcp_socket_t *socket,const void *bytes,size_t length)
{ assert(venus_tcp_socket_send(socket,bytes,length,venus_tcp_now_ms()+1000,NULL)==RingOk); }
#ifdef _WIN32
static DWORD WINAPI server_run(LPVOID argument)
#else
static void *server_run(void *argument)
#endif
{
    fixture_t *fixture=argument; active_fixture=fixture; server_mode_t mode=fixture->mode;
    venus_tcp_socket_t accepted={0}; venus_tcp_server_t server={0}; uint8_t token[32]={0},capabilities[160];
    assert(venus_tcp_socket_accept(&fixture->listener,&accepted,venus_tcp_now_ms()+1000,NULL)==RingOk); venus_tcp_socket_close(&fixture->listener);
#ifdef TcpPeerFaultTests
    send_count=0;
    if (mode==AuthRandomFail) random_failure=1;
    if (mode==AuthClockFail) clock_failure=1;
#endif
    venus_ring_status_t status=venus_tcp_server_authenticate(&server,&accepted,token,1000,NULL);
    if (mode==TokenBad || mode==HelloBad || mode==HelloPartial || mode==AuthRandomFail || mode==AuthClockFail) {
        assert(status==(mode>=AuthRandomFail ? RingClosed : RingCorrupt) && !accepted.initialized && !server.socket.initialized && !server.session); goto cleanup;
    }
    assert(status==RingOk && !accepted.initialized && server.session && server.next_sequence==1);
    venus_channel_t channel={0}; venus_rpc_t rpc={.channel=&channel}; venus_guest_t guest={.rpc=&rpc,.timeout_ms=1000};
    assert(venus_tcp_wire_oracle(4,capabilities,sizeof capabilities)); assert(venus_capabilities_decode(&guest.capabilities,capabilities,sizeof capabilities)==RingOk);
    if (mode==CapsBad) capabilities[16]=2;
    if (mode==CapsUnsupported) capabilities[0]=2;
    if (mode==CapsMismatch) guest.capabilities.allow_vk_wait_syncs=1;
#ifdef TcpPeerFaultTests
    if (mode==HelloSendFail) send_failure=1;
    if (mode==AckSendFail) send_failure=2;
#endif
    status=venus_tcp_server_negotiate(&server,&guest,capabilities,NULL);
    if (mode==ProfileBad || mode==ProfilePartial || (mode>=CapsBad && mode<=CapsMismatch) || mode==HelloSendFail || mode==AckSendFail) {
        assert(status==(mode==HelloSendFail || mode==AckSendFail ? RingClosed : RingCorrupt)); assert(!server.socket.initialized); goto cleanup;
    }
    assert(status==RingOk && server.ready);
    if (mode==LocalInvalid) {
        assert(venus_tcp_server_step(NULL,&guest,NULL)==RingInvalid); assert(venus_tcp_server_step(&server,NULL,NULL)==RingInvalid);
        guest.timeout_ms=999; assert(venus_tcp_server_step(&server,&guest,NULL)==RingInvalid); guest.timeout_ms=1000;
        guest.lost=RingTimeout; assert(venus_tcp_server_step(&server,&guest,NULL)==RingInvalid); guest.lost=RingOk;
        guest.rpc=NULL; assert(venus_tcp_server_step(&server,&guest,NULL)==RingInvalid); guest.rpc=&rpc;
        rpc.channel=NULL; assert(venus_tcp_server_step(&server,&guest,NULL)==RingInvalid); rpc.channel=&channel;
        _Atomic uint32_t cancel=0; assert(venus_tcp_server_step(&server,&guest,&cancel)==RingInvalid);
        server.ready=0; assert(venus_tcp_server_step(&server,&guest,NULL)==RingInvalid); server.ready=1;
        server.socket.initialized=0; assert(venus_tcp_server_step(&server,&guest,NULL)==RingInvalid); server.socket.initialized=1;
        assert(venus_tcp_server_negotiate(&server,&guest,capabilities,NULL)==RingInvalid);
    }
    if (mode==MaxLast) server.next_sequence=UINT64_MAX-1;
    if (mode==StepTimeout) server.timeout_ms=20;
    _Atomic uint32_t cancelled=mode==StepCancel; channel.cancel=&cancelled;
#ifdef TcpPeerFaultTests
    if (mode==StepClockFail) clock_failure=1;
    if (mode==StepReceiveFail) receive_failure=1;
    if (mode==HeaderSendFail) send_failure=3;
    if (mode==PayloadSendFail) send_failure=4;
    if (mode==BeforeRpcClockFail) before_rpc_clock=1;
    if (mode==BeforeRpcTimeout) { server.timeout_ms=1; before_rpc_delay=1; }
#endif
    if (mode==LocalInvalid || mode==RetireSendFail || mode==RetireClockFail) goto orderly;
    status=venus_tcp_server_step(&server,&guest,&cancelled);
    venus_ring_status_t expected=RingOk;
    if ((mode>=HeaderPartial && mode<=PayloadPartial) || (mode>=GuestFalseSuccess && mode<=GuestOversized)) expected=RingCorrupt;
    if (mode==GuestNoResponse || mode==StepTimeout || mode==BeforeRpcTimeout) expected=RingTimeout;
    if (mode==StepCancel) expected=RingCancelled;
    if (mode==HeaderSendFail || mode==PayloadSendFail || mode==StepClockFail || mode==BeforeRpcClockFail || mode==StepReceiveFail) expected=RingClosed;
    if (mode>=ReplyCorrupt && mode<=ReplyTimeout) expected=RingStatuses[mode-ReplyAgain];
    if (status!=expected) { fprintf(stderr,"Server mode%d actual%d expected%d\n",mode,status,expected); fflush(stderr); }
    assert(status==expected);
    if (expected!=RingOk) { if (server.socket.initialized || server.lost!=expected) { fprintf(stderr,"Unexpected retained mode%d live%u lost%d eof%u\n",mode,server.socket.initialized,server.lost,server.eof); fflush(stderr); } assert(!server.socket.initialized && server.lost==expected); assert(venus_tcp_server_step(&server,&guest,NULL)==expected); goto cleanup; }
    assert(server.next_sequence==(mode==MaxLast ? UINT64_MAX : 2));
    assert(server.forwarded_requests==1 && server.last_request.sequence==(mode==MaxLast ? UINT64_MAX-1 : 1));
    assert(server.forwarded_reads==(mode==ReadFull ? 1u : 0u));
    assert(server.last_opcode==(mode==CommandOpcode ? 173u : 0u));
    assert(server.last_request.kind==(mode==ReadFull ? RequestRead : mode==WriteFull ? RequestWrite : mode==CommandOpcode ? RequestSubmit : RequestPoll));
orderly:
    assert(venus_tcp_server_step(&server,&guest,NULL)==RingClosed && server.eof && server.socket.initialized);
    /* Standalone units acquired no receiver worker; clear all synthetic borrows before framing-only Ack. */
    guest.rpc=NULL; rpc.channel=NULL;
#ifdef TcpPeerFaultTests
    if (mode==RetireSendFail) send_failure=send_count+1;
    if (mode==RetireClockFail) clock_failure=1;
#endif
    status=venus_tcp_server_ack_retired(&server,NULL);
    assert(status==(mode==RetireSendFail || mode==RetireClockFail ? RingClosed : RingOk)); assert(!server.socket.initialized);
cleanup:
    venus_tcp_socket_close(&accepted); venus_tcp_server_free(&server); venus_tcp_server_free(&server);
    for (size_t index=0;index<sizeof server;++index) assert(!((const uint8_t *)&server)[index]);
    active_fixture=NULL;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void cycle(server_mode_t mode)
{
    fixture_t fixture={.mode=mode}; assert(venus_tcp_socket_listen(&fixture.listener,0,&fixture.port)==RingOk);
#ifdef _WIN32
    HANDLE thread=CreateThread(NULL,0,server_run,&fixture,0,NULL); assert(thread);
#else
    pthread_t thread; assert(!pthread_create(&thread,NULL,server_run,&fixture));
#endif
    venus_tcp_socket_t socket={0}; assert(venus_tcp_socket_connect(&socket,"127.0.0.1",fixture.port,venus_tcp_now_ms()+1000,NULL)==RingOk);
    venus_tcp_client_hello_t hello={0}; uint8_t bytes[VenusTcpMaxCommandBytes],capabilities[160];
    if (mode==TokenBad) hello.token[31]=1;
    assert(venus_tcp_client_hello_encode(&hello,bytes,128)==RingOk);
    if (mode==HelloBad) bytes[127]=1;
    if (mode==AuthClockFail) goto cleanup;
    send_bytes(&socket,bytes,mode==HelloPartial ? 3 : 128);
    if (mode==TokenBad || mode==HelloBad || mode==HelloPartial || mode==AuthRandomFail || (mode>=CapsBad && mode<=CapsMismatch) || mode==HelloSendFail) goto cleanup;
    receive_bytes(&socket,bytes,224); venus_tcp_server_hello_t host; assert(venus_tcp_server_hello_decode(&host,bytes,224)==RingOk && host.session && !memcmp(host.nonce,hello.nonce,16));
    assert(venus_tcp_wire_oracle(4,capabilities,160)); assert(!memcmp(host.capabilities,capabilities,160));
    assert(venus_tcp_profile_encode(bytes,160)==RingOk);
    if (mode==ProfileBad) bytes[159]=1;
    send_bytes(&socket,bytes,mode==ProfilePartial ? 13 : 160);
    if (mode==ProfileBad || mode==ProfilePartial || mode==AckSendFail) goto cleanup;
    receive_bytes(&socket,bytes,32); uint64_t session=0; assert(venus_tcp_ack_decode(&session,bytes,32)==RingOk && session==host.session);
    if (mode==LocalInvalid || mode==RetireSendFail || mode==RetireClockFail) goto retire;
    if (mode==StepCancel || mode==StepTimeout || mode==StepClockFail || mode==StepReceiveFail) { sleep_ms(50); goto cleanup; }
    venus_request_t request={.kind=RequestPoll,.sequence=mode==MaxLast ? UINT64_MAX-1 : 1};
    if (mode==ReadFull || mode==PayloadSendFail) request=(venus_request_t){.kind=RequestRead,.sequence=1,.resource_id=2,.argument_one=4096};
    if (mode==WriteFull || mode==PayloadPartial) request=(venus_request_t){.kind=RequestWrite,.sequence=1,.resource_id=2,.argument_one=4096,.payload_bytes=4096};
    if (mode==CommandOpcode) request=(venus_request_t){.kind=RequestSubmit,.sequence=1,.payload_bytes=44};
    if (mode==CommandTooLong) request=(venus_request_t){.kind=RequestSubmit,.sequence=1,.payload_bytes=VenusTcpMaxCommandBytes+4};
    if (mode==SequenceBad) request.sequence=2;
    if (mode==SequenceMax) request.sequence=UINT64_MAX;
    assert(venus_request_encode(&request,bytes,64)==RingOk);
    if (mode==HeaderBad) bytes[63]=1;
    send_bytes(&socket,bytes,mode==HeaderPartial ? 5 : 64);
    if (mode>=HeaderPartial && mode<=CommandTooLong) goto cleanup;
    if (request.payload_bytes) { for (size_t index=0;index<request.payload_bytes;++index) bytes[index]=(uint8_t)(index*13+7); if (mode==CommandOpcode) { memset(bytes,0,44); bytes[0]=178; bytes[36]=173; } send_bytes(&socket,bytes,mode==PayloadPartial ? 15 : request.payload_bytes); }
    if (mode==PayloadPartial || (mode>=GuestNoResponse && mode<=GuestOversized) || mode==HeaderSendFail || mode==BeforeRpcClockFail || mode==BeforeRpcTimeout) goto cleanup;
    receive_bytes(&socket,bytes,64); venus_request_t response; assert(venus_request_decode(&response,bytes,64)==RingOk && response.sequence==request.sequence && response.kind==request.kind);
    assert(response.status==(mode>=ReplyAgain && mode<=ReplyTimeout ? WireStatuses[mode-ReplyAgain] : RequestSuccess));
    if (mode==PayloadSendFail) goto cleanup;
    if (mode==ReadFull) { receive_bytes(&socket,bytes,4096); for (size_t index=0;index<4096;++index) assert(bytes[index]==(uint8_t)(index*17+9)); }
    if (mode>=ReplyCorrupt && mode<=ReplyTimeout) goto cleanup;
retire:
    assert(venus_tcp_socket_shutdown_write(&socket)==RingOk);
    if (mode==RetireSendFail || mode==RetireClockFail) goto cleanup;
    receive_bytes(&socket,bytes,32); assert(venus_tcp_ack_decode(&session,bytes,32)==RingOk && session==host.session);
cleanup:
    venus_tcp_socket_close(&socket);
#ifdef _WIN32
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); assert(CloseHandle(thread));
#else
    assert(!pthread_join(thread,NULL));
#endif
    assert(fixture.calls<=1);
}
static void invalid(void)
{
    venus_tcp_server_t server={0}; venus_tcp_socket_t accepted={0}; uint8_t token[32]={0},capabilities[160]={0}; venus_guest_t guest={0};
    assert(venus_tcp_server_authenticate(NULL,&accepted,token,1000,NULL)==RingInvalid);
    assert(venus_tcp_server_authenticate(&server,NULL,token,1000,NULL)==RingInvalid);
    assert(venus_tcp_server_authenticate(&server,&accepted,NULL,1000,NULL)==RingInvalid);
    assert(venus_tcp_server_authenticate(&server,&accepted,token,1000,NULL)==RingInvalid);
    accepted.initialized=1;
    assert(venus_tcp_server_authenticate(&server,&accepted,token,0,NULL)==RingInvalid);
    assert(venus_tcp_server_authenticate(&server,&accepted,token,60001,NULL)==RingInvalid);
    server.socket.initialized=1; assert(venus_tcp_server_authenticate(&server,&accepted,token,1000,NULL)==RingInvalid); server.socket.initialized=0;
    server.session=1; assert(venus_tcp_server_authenticate(&server,&accepted,token,1000,NULL)==RingInvalid); server.session=0;
    assert(venus_tcp_server_authenticate(&server,&server.socket,token,1000,NULL)==RingInvalid);
    assert(venus_tcp_server_authenticate(&server,&accepted,server.tx,1000,NULL)==RingInvalid);
    assert(venus_tcp_server_authenticate(&server,&accepted,(const void *)&accepted,1000,NULL)==RingInvalid);
    accepted.initialized=0;
    assert(venus_tcp_server_negotiate(NULL,&guest,capabilities,NULL)==RingInvalid);
    assert(venus_tcp_server_negotiate(&server,NULL,capabilities,NULL)==RingInvalid);
    assert(venus_tcp_server_negotiate(&server,&guest,NULL,NULL)==RingInvalid);
    assert(venus_tcp_server_negotiate(&server,&guest,capabilities,NULL)==RingInvalid);
    server.lost=RingTimeout; assert(venus_tcp_server_negotiate(&server,&guest,capabilities,NULL)==RingTimeout); server.lost=RingOk;
    assert(venus_tcp_server_ack_retired(NULL,NULL)==RingInvalid);
    assert(venus_tcp_server_ack_retired(&server,NULL)==RingInvalid);
    server.eof=1; assert(venus_tcp_server_ack_retired(&server,NULL)==RingInvalid);
    server.socket.initialized=1; assert(venus_tcp_server_ack_retired(&server,NULL)==RingInvalid); server.socket.initialized=0;
    server.socket.initialized=1; server.session=0;
    assert(venus_tcp_server_negotiate(&server,&guest,capabilities,NULL)==RingInvalid);
    server.session=1;
    assert(venus_tcp_server_negotiate(&server,&guest,capabilities,NULL)==RingInvalid);
    venus_rpc_t rpc={0}; guest.rpc=&rpc; guest.lost=RingTimeout;
    assert(venus_tcp_server_negotiate(&server,&guest,capabilities,NULL)==RingInvalid);
    guest.lost=RingOk;
    assert(venus_tcp_server_negotiate(&server,&guest,server.tx,NULL)==RingInvalid);
    server.socket.initialized=0;
    venus_tcp_server_free(NULL);
}
/** @brief Run standalone synthetic frontend/server ownership units, zero on pass; no actual GPU claims. */
int main(void)
{
    invalid(); cycle(Normal); sleep_ms(1000); unsigned baseline=resource_count();
    for (unsigned iteration=0;iteration<8;++iteration) {
        for (server_mode_t mode=Normal;mode<=MaxLast;++mode) cycle(mode);
#ifdef TcpPeerFaultTests
        for (server_mode_t mode=AuthRandomFail;mode<=StepReceiveFail;++mode) cycle(mode);
#endif
        uint64_t deadline=venus_tcp_now_ms()+1000; while (resource_count()!=baseline && venus_tcp_now_ms()<deadline) sleep_ms(1);
        assert(resource_count()==baseline);
    }
    puts("TCP server real peers: PASS (8 cycles; synthetic frontend, no GPU/worker credit)"); return 0;
}
