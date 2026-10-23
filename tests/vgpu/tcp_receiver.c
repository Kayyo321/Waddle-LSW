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
    PeerReadFull,PeerReadFullPartial,PeerReadFullOversized,PeerSendFailure,PeerPayloadFailure,PeerRetireFailure,PeerClockExchange,PeerClockRetire,
    PeerUntilNormal,PeerUntilRead,PeerUntilDeadline,PeerUntilCap,PeerUntilExpired,
    PeerUntilPartialHeader,PeerUntilPartialPayload,PeerUntilReuse,PeerUntilCancel,
    PeerUntilClock,PeerUntilOverflow,PeerUntilPostClock,PeerUntilPostCancel,PeerUntilPostLate,PeerUntilPostBackwards } peer_mode_t;
typedef struct peer_t { venus_tcp_socket_t listener; peer_mode_t mode; uint32_t port; } peer_t;
#ifdef TcpPeerFaultTests
/* Per-thread one-shot native errors: peer OS operations never consume client faults. */
static _Thread_local unsigned send_failure,send_count,clock_failure,random_failure,shutdown_failure;
static _Thread_local unsigned completion_fault,completion_bytes;
static _Thread_local _Atomic uint32_t *completion_cancel;
static _Thread_local uint64_t completion_deadline,clock_override;
ssize_t __real_recv(int fd,void *bytes,size_t length,int flags);
ssize_t __wrap_recv(int fd,void *bytes,size_t length,int flags)
{
    ssize_t count=__real_recv(fd,bytes,length,flags);
    if (count>0 && completion_bytes) {
        assert((size_t)count<=completion_bytes);
        completion_bytes-=(unsigned)count;
        if (!completion_bytes) {
            if (completion_fault==2) atomic_store_explicit(completion_cancel,1,memory_order_release);
        }
    }
    return count;
}
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
    if (clock_override) {
        uint64_t value=clock_override;clock_override=0;
        now->tv_sec=(time_t)(value/1000);now->tv_nsec=(long)(value%1000)*1000000;
        return 0;
    }
    if (completion_fault && !completion_bytes) {
        unsigned fault=completion_fault;completion_fault=0;
        if (fault==1) {errno=EIO;return -1;}
        if (fault==3 || fault==4) {
            uint64_t value=fault==3 ? completion_deadline : 1;
            now->tv_sec=(time_t)(value/1000);now->tv_nsec=(long)(value%1000)*1000000;
            return 0;
        }
    }
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
    if (peer->mode==PeerCancel || peer->mode==PeerDeadline || (peer->mode>=PeerSendFailure && peer->mode<=PeerClockRetire) ||
        peer->mode==PeerUntilDeadline || peer->mode==PeerUntilCap) { sleep_ms(100); goto cleanup; }
    if (peer->mode==PeerUntilExpired || peer->mode==PeerUntilCancel ||
        peer->mode==PeerUntilClock || peer->mode==PeerUntilOverflow) {
        size_t received=0;
        assert(venus_tcp_socket_receive(&socket,bytes,64,&received,venus_tcp_now_ms()+1000,NULL)==RingClosed &&
            !received && socket.received_eof);
        goto cleanup;
    }
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
    if (peer->mode==PeerReadFullOversized) response.payload_bytes=VenusTcpMaxReplyBytes+1;
    assert(venus_request_encode(&response,bytes,64)==RingOk);
    if (peer->mode==PeerResponseBad) bytes[56]=1;
    if (peer->mode==PeerUntilPartialHeader) {
        send_bytes(&socket,bytes,8);sleep_ms(200);goto cleanup;
    }
    if (peer->mode==PeerUntilReuse) sleep_ms(40);
    send_bytes(&socket,bytes,peer->mode==PeerResponsePartial ? 3 : 64);
    if (peer->mode==PeerResponsePayloadPartial) { send_bytes(&socket,payload,3); goto cleanup; }
    if (peer->mode==PeerUntilPartialPayload) {
        send_bytes(&socket,(const uint8_t[]){11,12,13},3);sleep_ms(200);goto cleanup;
    }
    if (peer->mode==PeerReadFullPartial) {
        for (size_t index=0;index<VenusTcpMaxReplyBytes-1;++index) payload[index]=(uint8_t)(index+11);
        send_bytes(&socket,payload,VenusTcpMaxReplyBytes-1);goto cleanup;
    }
    if (response.payload_bytes && peer->mode!=PeerResponseTooLong && peer->mode!=PeerReadFullOversized) {
        for (size_t index=0;index<response.payload_bytes;++index) payload[index]=(uint8_t)(index+11);
        send_bytes(&socket,payload,response.payload_bytes);
    }
    if (peer->mode==PeerReadFullOversized || (peer->mode>=PeerResponsePartial && peer->mode<=PeerResponseTooLong)) goto cleanup;
    if (peer->mode>=PeerResponseCorrupt && peer->mode<=PeerResponseTimeout) goto cleanup;
    if (peer->mode==PeerMaxSequence || peer->mode>=PeerUntilPostClock) goto cleanup;
    if (peer->mode==PeerUntilReuse) {
        receive_bytes(&socket,bytes,64);
        assert(venus_request_decode(&request,bytes,64)==RingOk && request.sequence==2 && !request.payload_bytes);
        response=(venus_request_t){.kind=request.kind,.sequence=2,.direction=1};
        assert(venus_request_encode(&response,bytes,64)==RingOk);
        sleep_ms(100);
        venus_ring_status_t sent=venus_tcp_socket_send(&socket,bytes,64,venus_tcp_now_ms()+1000,NULL);
        assert(sent==RingOk || sent==RingClosed);goto cleanup;
    }
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
static void cycle(peer_mode_t mode,int timed)
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
    venus_request_t request={.kind=RequestPoll},response={0}; uint8_t output[VenusTcpMaxReplyBytes]; memset(output,0xa5,sizeof output);
    if (mode==PeerLocalInvalid) {
        const venus_tcp_client_t before=client;
        assert(venus_tcp_client_exchange_until(&client,&request,NULL,0,&response,NULL,0,0)==RingInvalid);
        assert(!memcmp(&client,&before,sizeof client));
        _Atomic uint32_t cancelled=1;
        assert(venus_tcp_client_exchange_until_cancel(&client,&request,NULL,0,&response,NULL,0,0,&cancelled)==RingInvalid);
        assert(!memcmp(&client,&before,sizeof client));
        assert(venus_tcp_client_exchange_until(&client,&request,NULL,0,&request,NULL,0,1)==RingInvalid);
        assert(!memcmp(&client,&before,sizeof client));
        /* Invalid encoded fields beat cancellation/expired time without touching
         * the native clock, socket, staging or sequence; the peer still retires. */
        for (unsigned field=0;field<3;++field) {
            venus_request_t malformed=request;
            if (!field) malformed.flags=1;
            else if (field==1) malformed.status=RequestAgain;
            else malformed.resource_id=1;
            memset(&response,0xa5,sizeof response);
#ifdef TcpPeerFaultTests
            clock_failure=1;
            unsigned sent=send_count;
#endif
            assert(venus_tcp_client_exchange_until_cancel(&client,&malformed,NULL,0,
                &response,NULL,0,1,&cancelled)==RingInvalid);
            assert(!memcmp(&client,&before,sizeof client));
            const venus_request_t Zero={0};assert(!memcmp(&response,&Zero,sizeof Zero));
#ifdef TcpPeerFaultTests
            assert(clock_failure==1 && send_count==sent);
            clock_failure=0;
#endif
        }
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
        // First over each operation-specific quota rejects without socket/staging changes.
        request=(venus_request_t){.kind=RequestRead,.resource_id=2,.argument_one=VenusTcpMaxReplyBytes+1};
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,output,VenusTcpMaxReplyBytes+1)==RingInvalid);
        request=(venus_request_t){.kind=RequestReply,.argument_zero=1,.argument_one=VenusTcpMaxCommandReplyBytes+1};
        assert(venus_tcp_client_exchange(&client,&request,NULL,0,&response,output,VenusTcpMaxCommandReplyBytes+1)==RingInvalid);
        request=(venus_request_t){.kind=RequestWrite,.resource_id=2,.argument_one=VenusTcpMaxCommandReplyBytes+1,.payload_bytes=VenusTcpMaxCommandReplyBytes+1};
        assert(venus_tcp_client_exchange(&client,&request,output,VenusTcpMaxCommandReplyBytes+1,&response,NULL,0)==RingInvalid);
        assert(!memcmp(&client,&before,sizeof client));
        for (size_t index=0;index<sizeof output;++index) assert(output[index]==0xa5);
        assert(client.next_sequence==1 && client.lost==RingOk); goto retirement;
    }
    if (mode==PeerClose) { venus_tcp_client_free(&client); goto cleanup; }
    if (mode==PeerRead || mode==PeerResponsePayloadPartial || mode==PeerResponseTooLong || mode==PeerUntilRead || mode==PeerUntilPartialPayload) request=(venus_request_t){.kind=RequestRead,.resource_id=2,.argument_one=8};
    if (mode>=PeerReadFull && mode<=PeerReadFullOversized) request=(venus_request_t){.kind=RequestRead,.resource_id=2,.argument_one=VenusTcpMaxReplyBytes};
    if (mode==PeerWriteAlias || mode==PeerPayloadFailure) request=(venus_request_t){.kind=RequestWrite,.resource_id=2,.argument_one=8,.payload_bytes=8};
    if (mode==PeerMaxSequence) client.next_sequence=UINT64_MAX-1;
    if (mode==PeerDeadline || mode==PeerUntilCap) client.timeout_ms=20;
#ifdef TcpPeerFaultTests
    if (mode==PeerSendFailure) send_failure=send_count+1;
    if (mode==PeerPayloadFailure) send_failure=send_count+2;
    if (mode==PeerClockExchange) clock_failure=1;
    if (mode==PeerRetireFailure || mode==PeerClockRetire) goto retirement;
#endif
    _Atomic uint32_t cancel=mode==PeerCancel || mode==PeerUntilCancel;
    uint8_t input[8]={1,2,3,4,5,6,7,8};
    uint64_t until=venus_tcp_now_ms()+1000;
    if (mode==PeerUntilDeadline) until=venus_tcp_now_ms()+20;
    if (mode==PeerUntilPartialHeader || mode==PeerUntilPartialPayload || mode==PeerUntilReuse) until=venus_tcp_now_ms()+100;
    if (mode==PeerUntilExpired) until=venus_tcp_now_ms()-1;
#ifdef TcpPeerFaultTests
    if (mode==PeerUntilClock) clock_failure=1;
    if (mode==PeerUntilOverflow) clock_override=UINT64_MAX;
    if (mode>=PeerUntilPostClock) {
        completion_fault=(unsigned)(mode-PeerUntilPostClock)+1;
        completion_bytes=64;completion_cancel=&cancel;completion_deadline=until;
    }
#endif
    status=mode==PeerUntilNormal ? venus_tcp_client_exchange_until(&client,&request,NULL,0,&response,NULL,0,until) :
        mode>=PeerUntilNormal || timed ? venus_tcp_client_exchange_until_cancel(&client,&request,NULL,0,&response,
            request.kind==RequestRead ? output : NULL,request.kind==RequestRead ? (size_t)request.argument_one : 0,until,&cancel) :
        venus_tcp_client_exchange_cancel(&client,&request,request.payload_bytes ? input : NULL,request.payload_bytes,&response,
            request.kind==RequestRead ? output : NULL,request.kind==RequestRead ? (size_t)request.argument_one : 0,&cancel);
    if (mode==PeerUntilReuse) {
        assert(status==RingOk && client.next_sequence==2 && client.timeout_ms==1000);
        status=venus_tcp_client_exchange_until(&client,&request,NULL,0,&response,NULL,0,until);
    }
#ifdef TcpPeerFaultTests
    if (mode==PeerUntilPostCancel) completion_fault=0;
    if (mode==PeerUntilClock) assert(!clock_failure);
    if (mode==PeerUntilOverflow) assert(!clock_override);
    if (mode>=PeerUntilPostClock) assert(!completion_bytes && !completion_fault);
#endif
    static const venus_ring_status_t Statuses[]={RingAgain,RingInvalid,RingLimit,RingCorrupt,RingClosed,RingCancelled,RingTimeout};
    venus_ring_status_t expected=mode==PeerReadFullPartial || mode==PeerReadFullOversized ? RingCorrupt : mode==PeerReadFull ? RingOk : mode==PeerUntilPostClock || mode==PeerUntilPostBackwards ||
        mode==PeerUntilClock || mode==PeerUntilOverflow ? RingClosed :
        mode==PeerUntilPostCancel || mode==PeerUntilCancel ? RingCancelled : mode==PeerUntilPostLate ? RingTimeout :
        mode==PeerUntilNormal || mode==PeerUntilRead ? RingOk : mode>=PeerUntilDeadline ? RingTimeout :
        mode>=PeerSendFailure ? RingClosed : mode==PeerCancel ? RingCancelled : mode==PeerDeadline ? RingTimeout :
        mode>=PeerResponsePartial && mode<=PeerResponseTooLong ? RingCorrupt :
        mode>=PeerResponseAgain && mode<=PeerResponseTimeout ? Statuses[mode-PeerResponseAgain] : RingOk;
    assert(status==expected);
    if (mode==PeerRead || mode==PeerUntilRead || mode==PeerReadFull) {
        size_t amount=(size_t)request.argument_one;
        for (size_t index=0;index<amount;++index) assert(output[index]==(uint8_t)(index+11));
        for (size_t index=amount;index<sizeof output;++index) assert(output[index]==0xa5);
        assert(response.sequence==1 && response.payload_bytes==amount && client.next_sequence==2);
    } else for (size_t index=0;index<sizeof output;++index) assert(output[index]==0xa5);
    if (mode==PeerReadFullPartial || mode==PeerReadFullOversized) {
        const venus_request_t Zero={0};assert(!memcmp(&response,&Zero,sizeof Zero));
        assert(client.next_sequence==1);
    }
    if (expected==RingCorrupt || expected==RingClosed || expected==RingCancelled || expected==RingTimeout) {
        if (mode>=PeerUntilNormal) {
            assert(client.next_sequence==(mode==PeerUntilReuse ? 2u : 1u));
            const venus_request_t Zero={0};assert(!memcmp(&response,&Zero,sizeof Zero));
            assert(client.timeout_ms==(mode==PeerUntilCap ? 20u : 1000u));
        }
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
    assert(status==(mode>=PeerRetireFailure && mode<=PeerClockRetire ? RingClosed : mode>=PeerRetireBad && mode<=PeerRetirePartial ? RingCorrupt : RingOk));
    assert(!client.socket.initialized && client.session);
cleanup:
#ifdef TcpPeerFaultTests
    completion_cancel=NULL;completion_deadline=0;
    assert(!completion_fault && !completion_bytes && !clock_override);
#endif
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
    invalid(); cycle(PeerNormal,0); sleep_ms(1000); unsigned baseline=resource_count();
    for (unsigned iteration=0;iteration<8;++iteration) {
        for (peer_mode_t mode=PeerNormal;mode<=PeerLocalInvalid;++mode) cycle(mode,0);
        for (peer_mode_t mode=PeerReadFull;mode<=PeerReadFullOversized;++mode) cycle(mode,0);
        for (peer_mode_t mode=PeerUntilNormal;mode<=PeerUntilCancel;++mode) cycle(mode,0);
        for (peer_mode_t mode=PeerResponseAgain;mode<=PeerResponseTimeout;++mode) cycle(mode,1);
#ifdef TcpPeerFaultTests
        for (peer_mode_t mode=PeerSendFailure;mode<=PeerClockRetire;++mode) cycle(mode,0);
        for (peer_mode_t mode=PeerUntilClock;mode<=PeerUntilPostBackwards;++mode) cycle(mode,0);
#endif
        uint64_t deadline=venus_tcp_now_ms()+1000;
        while (resource_count()!=baseline && venus_tcp_now_ms()<deadline) sleep_ms(1);
        assert(resource_count()==baseline);
    }
    puts("TCP client real peers: PASS (8 cycles of every peer case; synthetic capset, no GPU credit)"); return 0;
}
