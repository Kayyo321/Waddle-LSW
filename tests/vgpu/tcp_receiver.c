/** @file tcp_receiver.c @brief Real TCP peer framing tests; synthetic capset/guest only, no GPU credit. */
#include "waddle/venus_tcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#endif
/** @brief Public synthetic profile oracle; output borrowed for call, no allocation. */
int venus_tcp_wire_oracle(unsigned kind,uint8_t *output,size_t capacity);
typedef enum peer_mode_t { PeerNormal,PeerHelloPartial,PeerHelloBad,PeerNonceBad,
    PeerCapsBad,PeerCapsUnsupported,PeerAckPartial,PeerAckBad,PeerAckMismatch,
    PeerResponsePartial,PeerResponseBad,PeerResponseSequence,PeerResponsePayloadPartial,
    PeerResponseTooLong,PeerResponseAgain,PeerResponseInvalid,PeerResponseLimit,
    PeerResponseCorrupt,PeerResponseClosed,PeerResponseCancelled,PeerResponseTimeout,
    PeerRetireBad,PeerRetireMismatch,PeerRetirePartial,PeerRead,PeerWriteAlias,
    PeerMaxSequence,PeerCancel,PeerDeadline,PeerClose,PeerLocalInvalid,
    PeerSendFailure,PeerPayloadFailure,PeerRetireFailure,PeerClockExchange,PeerClockRetire } peer_mode_t;
typedef struct peer_t { venus_tcp_socket_t listener; peer_mode_t mode; uint32_t port; } peer_t;
#ifdef TcpPeerFaultTests
/* Per-thread one-shot native errors: peer OS operations never consume client faults. */
static _Thread_local unsigned send_failure,send_count,clock_failure,random_failure,shutdown_failure;
ssize_t __real_send(int fd,const void *bytes,size_t length,int flags);
ssize_t __wrap_send(int fd,const void *bytes,size_t length,int flags)
{
    ++send_count;
    if (send_failure && send_count==send_failure) { send_failure=0; errno=EIO; return -1; }
    return __real_send(fd,bytes,length,flags);
}
ssize_t __real_getrandom(void *bytes,size_t length,unsigned flags);
ssize_t __wrap_getrandom(void *bytes,size_t length,unsigned flags)
{
    if (random_failure) { random_failure=0; errno=EIO; return -1; }
    return __real_getrandom(bytes,length,flags);
}
int __real_clock_gettime(clockid_t id,struct timespec *now);
int __wrap_clock_gettime(clockid_t id,struct timespec *now)
{
    if (clock_failure) { clock_failure=0; errno=EIO; return -1; }
    return __real_clock_gettime(id,now);
}
int __real_shutdown(int fd,int direction);
int __wrap_shutdown(int fd,int direction)
{
    if (shutdown_failure) { shutdown_failure=0; errno=EIO; return -1; }
    return __real_shutdown(fd,direction);
}
#endif
static unsigned resource_count(void)
{
#ifdef _WIN32
    DWORD count=0; assert(GetProcessHandleCount(GetCurrentProcess(),&count)); return count;
#else
    DIR *directory=opendir("/proc/self/fd"); assert(directory); unsigned count=0;
    struct dirent *entry; while ((entry=readdir(directory)))
        if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    assert(!closedir(directory)); return count;
#endif
}
static void sleep_ms(unsigned milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay={.tv_sec=milliseconds/1000,.tv_nsec=(long)(milliseconds%1000)*1000000};
    nanosleep(&delay,NULL);
#endif
}
static void send_bytes(venus_tcp_socket_t *socket,const void *bytes,size_t length)
{ assert(venus_tcp_socket_send(socket,bytes,length,venus_tcp_now_ms()+1000,NULL)==RingOk); }
static void receive_bytes(venus_tcp_socket_t *socket,void *bytes,size_t length)
{ size_t received=0; assert(venus_tcp_socket_receive(socket,bytes,length,&received,venus_tcp_now_ms()+1000,NULL)==RingOk && received==length); }
#ifdef _WIN32
static DWORD WINAPI peer_run(LPVOID argument)
#else
static void *peer_run(void *argument)
#endif
{
    peer_t *peer=argument; venus_tcp_socket_t socket={0};
    assert(venus_tcp_socket_accept(&peer->listener,&socket,venus_tcp_now_ms()+1000,NULL)==RingOk);
    venus_tcp_socket_close(&peer->listener);
    uint8_t bytes[VenusTcpServerHelloBytes]; receive_bytes(&socket,bytes,VenusTcpClientHelloBytes);
    venus_tcp_client_hello_t client_hello; assert(venus_tcp_client_hello_decode(&client_hello,bytes,128)==RingOk);
    venus_tcp_server_hello_t hello={.session=0x1122334455667788};
    memcpy(hello.nonce,client_hello.nonce,16); assert(venus_tcp_wire_oracle(4,hello.capabilities,160));
    if (peer->mode==PeerNonceBad) hello.nonce[15]^=1;
    if (peer->mode==PeerCapsBad) hello.capabilities[16]=2;
    if (peer->mode==PeerCapsUnsupported) hello.capabilities[0]=2;
    assert(venus_tcp_server_hello_encode(&hello,bytes,224)==RingOk);
    if (peer->mode==PeerHelloBad) bytes[40]=1;
    send_bytes(&socket,bytes,peer->mode==PeerHelloPartial ? 100 : 224);
    if (peer->mode>=PeerHelloPartial && peer->mode<=PeerCapsUnsupported) goto cleanup;
    receive_bytes(&socket,bytes,160); assert(venus_tcp_profile_validate(bytes,160)==RingOk);
    assert(venus_tcp_ack_encode(hello.session+(peer->mode==PeerAckMismatch),bytes,32)==RingOk);
    if (peer->mode==PeerAckBad) bytes[24]=1;
    send_bytes(&socket,bytes,peer->mode==PeerAckPartial ? 4 : 32);
    if (peer->mode>=PeerAckPartial && peer->mode<=PeerAckMismatch) goto cleanup;
    if (peer->mode==PeerCancel || peer->mode==PeerDeadline || peer->mode>=PeerSendFailure) { sleep_ms(100); goto cleanup; }
    if (peer->mode==PeerClose || peer->mode==PeerLocalInvalid) goto retire;
    receive_bytes(&socket,bytes,64); venus_request_t request;
    assert(venus_request_decode(&request,bytes,64)==RingOk);
    assert(request.sequence==(peer->mode==PeerMaxSequence ? UINT64_MAX-1 : 1));
    uint8_t payload[VenusTcpMaxCommandBytes];
    receive_bytes(&socket,payload,request.payload_bytes);
    if (peer->mode==PeerWriteAlias) for (size_t index=0;index<8;++index) assert(payload[index]==index+1);
    venus_request_t response={.kind=request.kind,.sequence=request.sequence,.direction=1};
    if (request.kind==RequestRead) response.payload_bytes=(uint32_t)request.argument_one;
    if (peer->mode>=PeerResponseAgain && peer->mode<=PeerResponseTimeout) {
        static const uint32_t Statuses[]={RequestAgain,RequestInvalid,RequestLimit,RequestCorrupt,RequestClosed,RequestCancelled,RequestTimeout};
        response.status=Statuses[peer->mode-PeerResponseAgain];
    }
    if (peer->mode==PeerResponseSequence) ++response.sequence;
    if (peer->mode==PeerResponseTooLong) response.payload_bytes=9;
    assert(venus_request_encode(&response,bytes,64)==RingOk);
    if (peer->mode==PeerResponseBad) bytes[56]=1;
    send_bytes(&socket,bytes,peer->mode==PeerResponsePartial ? 3 : 64);
    if (peer->mode==PeerResponsePayloadPartial) { send_bytes(&socket,payload,3); goto cleanup; }
    if (response.payload_bytes && peer->mode!=PeerResponseTooLong) {
        for (size_t index=0;index<response.payload_bytes;++index) payload[index]=(uint8_t)(index+11);
        send_bytes(&socket,payload,response.payload_bytes);
    }
    if (peer->mode>=PeerResponsePartial && peer->mode<=PeerResponseTooLong) goto cleanup;
    if (peer->mode>=PeerResponseCorrupt && peer->mode<=PeerResponseTimeout) goto cleanup;
    if (peer->mode==PeerMaxSequence) goto cleanup;
retire:
    { size_t received=0; assert(venus_tcp_socket_receive(&socket,bytes,64,&received,venus_tcp_now_ms()+1000,NULL)==RingClosed && !received); }
    assert(venus_tcp_ack_encode(hello.session+(peer->mode==PeerRetireMismatch),bytes,32)==RingOk);
    if (peer->mode==PeerRetireBad) bytes[24]=1;
    send_bytes(&socket,bytes,peer->mode==PeerRetirePartial ? 2 : 32);
cleanup:
    venus_tcp_socket_close(&socket); venus_tcp_scrub(&client_hello,sizeof client_hello);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void zero_owner(const venus_tcp_client_t *client)
{ const uint8_t *bytes=(const void *)client; for (size_t index=0;index<sizeof *client;++index) assert(!bytes[index]); }
static void cycle(peer_mode_t mode)
{
#ifdef TcpPeerFaultTests
    send_count=0;
#endif
    peer_t peer={.mode=mode}; assert(venus_tcp_socket_listen(&peer.listener,0,&peer.port)==RingOk);
#ifdef _WIN32
    HANDLE thread=CreateThread(NULL,0,peer_run,&peer,0,NULL); assert(thread);
#else
    pthread_t thread; assert(!pthread_create(&thread,NULL,peer_run,&peer));
#endif
    venus_tcp_config_t config={.version=1,.port=peer.port,.exchange_timeout_ms=1000,.host="127.0.0.1",.icd_path="C:\\fixture.dll"};
    venus_tcp_client_t client={0};
    venus_ring_status_t status=venus_tcp_client_init(&client,&config,NULL);
    if (mode>=PeerHelloPartial && mode<=PeerAckMismatch) {
        assert(status==(mode==PeerCapsUnsupported ? RingInvalid : RingCorrupt)); zero_owner(&client); goto cleanup;
    }
    assert(status==RingOk && client.session && client.next_sequence==1 && client.timeout_ms==1000);
    assert(client.capabilities.wire_format_version==1 && client.capabilities.vk_xml_version==VenusPinnedXmlVersion);
    venus_request_t request={.kind=RequestPoll},response={0}; uint8_t output[8]; memset(output,0xa5,8);
    if (mode==PeerLocalInvalid) {
        assert(venus_tcp_client_exchange(NULL,&request,NULL,0,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,NULL,NULL,0,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,NULL,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,1,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,output,1,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,1)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,output,VenusTcpMaxReplyBytes+1)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,output,VenusTcpMaxCommandBytes+1,&response,NULL,0)==RingInvalid);
        request.sequence=1; assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid); request.sequence=0;
        request.kind=RequestPresent; assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid); request.kind=RequestPoll;
        request.flags=1; assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid); request.flags=0;
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,output,8)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&request,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,&request,1,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,&request,1)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,&response,1,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,&response,1)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,client.tx,1,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,client.rx,1)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,(const void *)client.tx,NULL,0,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,(void *)client.rx,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,(void *)(UINTPTR_MAX-1),4,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,(void *)(UINTPTR_MAX-1),4)==RingInvalid);
        uint64_t session=client.session; client.session=0;
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid);
        assert(venus_tcp_client_retire(&client,NULL)==RingInvalid); client.session=session;
        client.timeout_ms=0; assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid); client.timeout_ms=1000;
        client.socket.initialized=0; assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid); client.socket.initialized=1;
        request.payload_bytes=1; assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid); request.payload_bytes=0;
        assert(client.next_sequence==1 && client.lost==RingOk); goto retirement;
    }
    if (mode==PeerClose) { venus_tcp_client_free(&client); goto cleanup; }
    if (mode==PeerRead || mode==PeerResponsePayloadPartial || mode==PeerResponseTooLong) request=(venus_request_t){.kind=RequestRead,.resource_id=2,.argument_one=8};
    if (mode==PeerWriteAlias || mode==PeerPayloadFailure) request=(venus_request_t){.kind=RequestWrite,.resource_id=2,.argument_one=8,.payload_bytes=8};
    if (mode==PeerMaxSequence) client.next_sequence=UINT64_MAX-1;
    if (mode==PeerDeadline) client.timeout_ms=20;
#ifdef TcpPeerFaultTests
    if (mode==PeerSendFailure) send_failure=send_count+1;
    if (mode==PeerPayloadFailure) send_failure=send_count+2;
    if (mode==PeerClockExchange) clock_failure=1;
    if (mode==PeerRetireFailure || mode==PeerClockRetire) goto retirement;
#endif
    _Atomic uint32_t cancel=mode==PeerCancel;
    uint8_t input[8]={1,2,3,4,5,6,7,8};
    status=venus_tcp_client_exchange_cancel(&client,&request,request.payload_bytes ? input : NULL,request.payload_bytes,&response,
        request.kind==RequestRead ? output : NULL,request.kind==RequestRead ? 8 : 0,&cancel);
    static const venus_ring_status_t Statuses[]={RingAgain,RingInvalid,RingLimit,RingCorrupt,RingClosed,RingCancelled,RingTimeout};
    venus_ring_status_t expected=mode>=PeerSendFailure ? RingClosed : mode==PeerCancel ? RingCancelled : mode==PeerDeadline ? RingTimeout :
        mode>=PeerResponsePartial && mode<=PeerResponseTooLong ? RingCorrupt :
        mode>=PeerResponseAgain && mode<=PeerResponseTimeout ? Statuses[mode-PeerResponseAgain] : RingOk;
    assert(status==expected);
    if (mode==PeerRead) for (size_t index=0;index<8;++index) assert(output[index]==index+11);
    else for (size_t index=0;index<8;++index) assert(output[index]==0xa5);
    if (expected==RingCorrupt || expected==RingClosed || expected==RingCancelled || expected==RingTimeout) {
        assert(!client.socket.initialized && client.lost==expected);
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==expected);
        assert(venus_tcp_client_retire(&client,NULL)==expected); goto cleanup;
    }
    if (mode==PeerMaxSequence) { assert(client.next_sequence==UINT64_MAX); assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingLimit); goto cleanup; }
retirement:
#ifdef TcpPeerFaultTests
    if (mode==PeerRetireFailure) shutdown_failure=1;
    if (mode==PeerClockRetire) clock_failure=1;
#endif
    status=venus_tcp_client_retire(&client,NULL);
    assert(status==(mode>=PeerRetireFailure ? RingClosed : mode>=PeerRetireBad && mode<=PeerRetirePartial ? RingCorrupt : RingOk));
    assert(!client.socket.initialized && client.session);
cleanup:
    venus_tcp_client_free(&client); venus_tcp_client_free(&client); zero_owner(&client);
#ifdef _WIN32
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); assert(CloseHandle(thread));
#else
    assert(!pthread_join(thread,NULL));
#endif
}
static void invalid(void)
{
    venus_tcp_client_t client={0}; venus_tcp_config_t config={0}; venus_request_t request={.kind=RequestPoll},response;
    assert(venus_tcp_client_init(NULL,&config,NULL)==RingInvalid);
    assert(venus_tcp_client_init(&client,NULL,NULL)==RingInvalid);
    assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid);
    config.version=1; assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid);
    config.exchange_timeout_ms=60001; assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid);
    config.exchange_timeout_ms=1; memset(config.host,'x',sizeof config.host); assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid);
    memcpy(config.host,"127.0.0.1",10); assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid);
    client.session=1; assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid); client.session=0;
    client.next_sequence=1; assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid); client.next_sequence=0;
    client.timeout_ms=1; assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid); client.timeout_ms=0;
    client.socket.initialized=1; assert(venus_tcp_client_init(&client,&config,NULL)==RingInvalid); client.socket.initialized=0;
    assert(venus_tcp_client_retire(NULL,NULL)==RingInvalid); assert(venus_tcp_client_retire(&client,NULL)==RingInvalid);
    assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,NULL,0)==RingInvalid);
    venus_tcp_client_free(NULL);
#ifdef TcpPeerFaultTests
    config.port=1;
    clock_failure=1; assert(venus_tcp_client_init(&client,&config,NULL)==RingClosed && !clock_failure); zero_owner(&client);
    random_failure=1; assert(venus_tcp_client_init(&client,&config,NULL)==RingClosed && !random_failure); zero_owner(&client);
#endif
}
/** @brief Run synthetic-peer ownership/framing regressions; zero return means units pass only. */
int main(void)
{
    invalid(); cycle(PeerNormal); sleep_ms(1000); unsigned baseline=resource_count();
    for (unsigned iteration=0;iteration<8;++iteration) {
        for (peer_mode_t mode=PeerNormal;mode<=PeerLocalInvalid;++mode) cycle(mode);
#ifdef TcpPeerFaultTests
        for (peer_mode_t mode=PeerSendFailure;mode<=PeerClockRetire;++mode) cycle(mode);
#endif
        uint64_t deadline=venus_tcp_now_ms()+1000;
        while (resource_count()!=baseline && venus_tcp_now_ms()<deadline) sleep_ms(1);
        assert(resource_count()==baseline);
    }
    puts("TCP client real peers: PASS (8 cycles of every peer case; synthetic capset, no GPU credit)"); return 0;
}
