/** @file venus_tcp_server.c @brief Token admission and bounded real receiver forwarding. */
#include "waddle/venus_tcp.h"
#include "waddle/venus_guest.h"
#include <string.h>
static int overlaps(const void *first,size_t first_bytes,const void *second,size_t second_bytes)
{
    uintptr_t a=(uintptr_t)first,b=(uintptr_t)second;
    if (first_bytes>UINTPTR_MAX-a || second_bytes>UINTPTR_MAX-b) return 1;
    return a<b+second_bytes && b<a+first_bytes;
}
static venus_ring_status_t deadline_after(uint32_t timeout_ms,uint64_t *deadline)
{
    uint64_t now=venus_tcp_now_ms();
    if (!now || now>UINT64_MAX-timeout_ms) return RingClosed;
    *deadline=now+timeout_ms;
    return RingOk;
}
static venus_ring_status_t operation_status(uint32_t status)
{
    static const venus_ring_status_t Statuses[]={RingOk,RingAgain,RingInvalid,RingCorrupt,
        RingClosed,RingCancelled,RingTimeout,RingLimit};
    return status<sizeof Statuses/sizeof *Statuses ? Statuses[status] : RingCorrupt;
}
static venus_ring_status_t poison(venus_tcp_server_t *server,venus_ring_status_t status)
{
    venus_tcp_socket_close(&server->socket);
    server->lost=status;
    return status;
}
static venus_ring_status_t receive_exact(venus_tcp_socket_t *socket,void *bytes,size_t length,
    uint64_t deadline,const _Atomic uint32_t *cancel)
{
    size_t received=0;
    venus_ring_status_t status=venus_tcp_socket_receive(socket,bytes,length,&received,deadline,cancel);
    return status==RingClosed && received ? RingCorrupt : status;
}
void venus_tcp_server_free(venus_tcp_server_t *server)
{
    if (!server) return;
    venus_tcp_socket_close(&server->socket);
    venus_tcp_scrub(server,sizeof *server);
}
venus_ring_status_t venus_tcp_server_authenticate(venus_tcp_server_t *server,
    venus_tcp_socket_t *accepted,const uint8_t *token,uint32_t timeout_ms,
    const _Atomic uint32_t *cancel)
{
    if (!server || !accepted || !token || server->socket.initialized || server->session ||
        !accepted->initialized || !timeout_ms || timeout_ms>60000 ||
        overlaps(server,sizeof *server,accepted,sizeof *accepted) ||
        overlaps(server,sizeof *server,token,32) || overlaps(accepted,sizeof *accepted,token,32)) return RingInvalid;
    venus_tcp_server_t staged={0}; staged.socket=*accepted;
    memset(accepted,0,sizeof *accepted);
    uint8_t bytes[VenusTcpClientHelloBytes];
    venus_tcp_client_hello_t hello={0};
    venus_ring_status_t status=deadline_after(5000,&staged.handshake_deadline);
    if (status!=RingOk) goto cleanup;
    status=receive_exact(&staged.socket,bytes,sizeof bytes,staged.handshake_deadline,cancel);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_client_hello_decode(&hello,bytes,sizeof bytes);
    if (status!=RingOk) goto cleanup;
    if (!venus_tcp_token_equal(hello.token,token)) { status=RingCorrupt; goto cleanup; }
    status=venus_tcp_random(&staged.session,sizeof staged.session);
    if (status!=RingOk) goto cleanup;
    if (!staged.session) { status=RingClosed; goto cleanup; }
    memcpy(staged.nonce,hello.nonce,sizeof staged.nonce);
    staged.next_sequence=1; staged.timeout_ms=timeout_ms;
    *server=staged;
    memset(&staged.socket,0,sizeof staged.socket);
cleanup:
    venus_tcp_server_free(&staged);
    venus_tcp_scrub(&hello,sizeof hello); venus_tcp_scrub(bytes,sizeof bytes);
    return status;
}
venus_ring_status_t venus_tcp_server_negotiate(venus_tcp_server_t *server,
    const venus_guest_t *guest,const uint8_t *capabilities,const _Atomic uint32_t *cancel)
{
    if (!server || !guest || !capabilities) return RingInvalid;
    if (server->lost!=RingOk) return server->lost;
    if (!server->socket.initialized || !server->session || server->ready ||
        !guest->rpc || guest->lost!=RingOk || overlaps(server,sizeof *server,guest,sizeof *guest) ||
        overlaps(server,sizeof *server,capabilities,VenusCapabilitiesBytes)) return RingInvalid;
    venus_capabilities_t actual;
    venus_ring_status_t status=venus_capabilities_decode(&actual,capabilities,VenusCapabilitiesBytes);
    if (status!=RingOk || venus_capabilities_compatible(&actual)!=RingOk ||
        memcmp(&actual,&guest->capabilities,sizeof actual)) return poison(server,RingCorrupt);
    venus_tcp_server_hello_t hello={.session=server->session};
    memcpy(hello.nonce,server->nonce,sizeof hello.nonce);
    memcpy(hello.capabilities,capabilities,sizeof hello.capabilities);
    uint8_t bytes[VenusTcpServerHelloBytes];
    status=venus_tcp_server_hello_encode(&hello,bytes,sizeof bytes);
    if (status!=RingOk) return poison(server,status);
    status=venus_tcp_socket_send(&server->socket,bytes,sizeof bytes,server->handshake_deadline,cancel);
    if (status!=RingOk) return poison(server,status);
    status=receive_exact(&server->socket,bytes,VenusCapabilitiesBytes,server->handshake_deadline,cancel);
    if (status!=RingOk) return poison(server,status);
    status=venus_tcp_profile_validate(bytes,VenusCapabilitiesBytes);
    if (status!=RingOk) return poison(server,status);
    status=venus_tcp_ack_encode(server->session,bytes,VenusTcpAckBytes);
    if (status!=RingOk) return poison(server,status);
    status=venus_tcp_socket_send(&server->socket,bytes,VenusTcpAckBytes,server->handshake_deadline,cancel);
    if (status!=RingOk) return poison(server,status);
    server->ready=1;
    return RingOk;
}
venus_ring_status_t venus_tcp_server_step(venus_tcp_server_t *server,
    venus_guest_t *guest,const _Atomic uint32_t *cancel)
{
    if (!server || !guest) return RingInvalid;
    if (server->lost!=RingOk) return server->lost;
    if (!server->ready || !server->socket.initialized || !guest->rpc ||
        guest->lost!=RingOk || guest->timeout_ms<server->timeout_ms ||
        !guest->rpc->channel || (cancel && guest->rpc->channel->cancel!=cancel)) return RingInvalid;
    uint8_t header[VenusRequestHeaderBytes];
    uint64_t deadline=0;
    venus_ring_status_t status=deadline_after(server->timeout_ms,&deadline);
    if (status!=RingOk) return poison(server,status);
    size_t received=0;
    status=venus_tcp_socket_receive(&server->socket,header,sizeof header,&received,deadline,cancel);
    if (status==RingClosed && !received && server->socket.received_eof) { server->eof=1; server->lost=RingClosed; return RingClosed; }
    if (status!=RingOk) return poison(server,status==RingClosed && received ? RingCorrupt : status);
    venus_request_t request={0},response={0};
    status=venus_request_decode(&request,header,sizeof header);
    if (status!=RingOk || venus_tcp_request_limit(&request)!=RingOk ||
        request.sequence!=server->next_sequence || request.sequence==UINT64_MAX) return poison(server,RingCorrupt);
    status=receive_exact(&server->socket,server->tx,request.payload_bytes,deadline,cancel);
    if (status!=RingOk) return poison(server,status);
    uint64_t now=venus_tcp_now_ms();
    if (!now) return poison(server,RingClosed);
    if (now>=deadline) return poison(server,RingTimeout);
    server->last_request=request;
    ++server->forwarded_requests;
    if (request.kind==RequestRead) ++server->forwarded_reads;
    if (request.kind==RequestSubmit && request.payload_bytes>=44 &&
        server->tx[0]==178 && server->tx[1]==0 && server->tx[2]==0 && server->tx[3]==0)
        server->last_opcode=(uint32_t)server->tx[36] | (uint32_t)server->tx[37]<<8 |
            (uint32_t)server->tx[38]<<16 | (uint32_t)server->tx[39]<<24;
    uint64_t outer_sequence=request.sequence; request.sequence=0;
    size_t capacity=(request.kind==RequestReply || request.kind==RequestRead) ? (size_t)request.argument_one : 0;
    status=venus_guest_exchange_timeout(guest,&request,server->tx,request.payload_bytes,
        &response,server->rx,capacity,(uint32_t)(deadline-now));
    /* An unacknowledged frontend failure has no actual response to forward. */
    if (!response.kind || response.direction!=1 || !response.sequence) return poison(server,status==RingOk ? RingCorrupt : status);
    if (status!=operation_status(response.status)) return poison(server,RingCorrupt);
    response.sequence=outer_sequence; request.sequence=outer_sequence;
    if (venus_tcp_response_limit(&request,&response)!=RingOk ||
        venus_request_encode(&response,header,sizeof header)!=RingOk) return poison(server,RingCorrupt);
    venus_ring_status_t sent=venus_tcp_socket_send(&server->socket,header,sizeof header,deadline,cancel);
    if (sent!=RingOk) return poison(server,sent);
    sent=venus_tcp_socket_send(&server->socket,server->rx,response.payload_bytes,deadline,cancel);
    if (sent!=RingOk) return poison(server,sent);
    ++server->next_sequence;
    if (status==RingCorrupt || status==RingClosed || status==RingCancelled || status==RingTimeout) return poison(server,status);
    return RingOk;
}
venus_ring_status_t venus_tcp_server_ack_retired(venus_tcp_server_t *server,
    const _Atomic uint32_t *cancel)
{
    if (!server || !server->eof || !server->socket.initialized || !server->session) return RingInvalid;
    uint8_t bytes[VenusTcpAckBytes];
    venus_ring_status_t status=venus_tcp_ack_encode(server->session,bytes,sizeof bytes);
    if (status!=RingOk) return poison(server,status);
    uint64_t deadline=0;
    status=deadline_after(server->timeout_ms,&deadline);
    if (status!=RingOk) return poison(server,status);
    status=venus_tcp_socket_send(&server->socket,bytes,sizeof bytes,deadline,cancel);
    venus_tcp_socket_close(&server->socket);
    if (status!=RingOk) server->lost=status;
    return status;
}
