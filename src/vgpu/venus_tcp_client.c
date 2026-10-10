/** @file venus_tcp_client.c @brief Authenticated sequential actual receiver callback. */
#include "waddle/venus_tcp.h"
#include <string.h>
static int overlaps(const void *first,size_t first_bytes,const void *second,size_t second_bytes)
{
    if (!first_bytes || !second_bytes) return 0;
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
static venus_ring_status_t poison(venus_tcp_client_t *client,venus_ring_status_t status)
{
    venus_tcp_socket_close(&client->socket);
    client->lost=status;
    return status;
}
static venus_ring_status_t operation_status(uint32_t status)
{
    static const venus_ring_status_t Statuses[] = {RingOk,RingAgain,RingInvalid,RingCorrupt,
                                                  RingClosed,RingCancelled,RingTimeout,RingLimit};
    return status<sizeof Statuses/sizeof *Statuses ? Statuses[status] : RingCorrupt;
}
static venus_ring_status_t receive_exact(venus_tcp_socket_t *socket,void *bytes,size_t length,
    uint64_t deadline,const _Atomic uint32_t *cancel)
{
    size_t received=0;
    venus_ring_status_t status=venus_tcp_socket_receive(socket,bytes,length,&received,deadline,cancel);
    return status==RingClosed && received ? RingCorrupt : status;
}
void venus_tcp_client_free(venus_tcp_client_t *client)
{
    if (!client) return;
    venus_tcp_socket_close(&client->socket);
    venus_tcp_scrub(client,sizeof *client);
}
venus_ring_status_t venus_tcp_client_init(venus_tcp_client_t *client,
    const venus_tcp_config_t *config,const _Atomic uint32_t *cancel)
{
    if (!client || !config || client->socket.initialized || client->session ||
        client->next_sequence || client->timeout_ms || config->version!=1 ||
        !config->exchange_timeout_ms || config->exchange_timeout_ms>60000 ||
        !memchr(config->host,0,sizeof config->host) || overlaps(client,sizeof *client,config,sizeof *config)) return RingInvalid;
    venus_tcp_client_t staged={0};
    venus_tcp_client_hello_t hello={0};
    venus_tcp_server_hello_t host={0};
    uint8_t bytes[VenusTcpServerHelloBytes];
    uint64_t deadline=0,ack=0;
    venus_ring_status_t status=deadline_after(5000,&deadline);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_random(hello.nonce,sizeof hello.nonce);
    if (status!=RingOk) goto cleanup;
    memcpy(hello.token,config->token,sizeof hello.token);
    status=venus_tcp_socket_connect(&staged.socket,config->host,config->port,deadline,cancel);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_client_hello_encode(&hello,bytes,VenusTcpClientHelloBytes);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_socket_send(&staged.socket,bytes,VenusTcpClientHelloBytes,deadline,cancel);
    if (status!=RingOk) goto cleanup;
    status=receive_exact(&staged.socket,bytes,sizeof bytes,deadline,cancel);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_server_hello_decode(&host,bytes,sizeof bytes);
    if (status!=RingOk) goto cleanup;
    if (memcmp(host.nonce,hello.nonce,sizeof hello.nonce)) { status=RingCorrupt; goto cleanup; }
    status=venus_capabilities_decode(&staged.capabilities,host.capabilities,sizeof host.capabilities);
    if (status!=RingOk) goto cleanup;
    status=venus_capabilities_compatible(&staged.capabilities);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_profile_encode(bytes,VenusCapabilitiesBytes);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_socket_send(&staged.socket,bytes,VenusCapabilitiesBytes,deadline,cancel);
    if (status!=RingOk) goto cleanup;
    status=receive_exact(&staged.socket,bytes,VenusTcpAckBytes,deadline,cancel);
    if (status!=RingOk) goto cleanup;
    status=venus_tcp_ack_decode(&ack,bytes,VenusTcpAckBytes);
    if (status!=RingOk) goto cleanup;
    if (ack!=host.session) { status=RingCorrupt; goto cleanup; }
    staged.session=host.session; staged.next_sequence=1;
    staged.timeout_ms=config->exchange_timeout_ms;
    *client=staged;
    memset(&staged.socket,0,sizeof staged.socket); /* Move the sole native owner. */
cleanup:
    venus_tcp_client_free(&staged);
    venus_tcp_scrub(&hello,sizeof hello); venus_tcp_scrub(&host,sizeof host);
    venus_tcp_scrub(bytes,sizeof bytes);
    return status;
}
static venus_ring_status_t exchange_common(venus_tcp_client_t *client,
    const venus_request_t *request,const void *input,size_t length,
    venus_request_t *response,void *output,size_t capacity,uint64_t deadline_ms,
    int absolute,const _Atomic uint32_t *cancel)
{
    if (!response) return RingInvalid;
    if ((client && (overlaps(response,sizeof *response,client,sizeof *client) ||
        (request && overlaps(request,sizeof *request,client,sizeof *client)) ||
        overlaps(input,length,client,sizeof *client) || overlaps(output,capacity,client,sizeof *client))) ||
        (request && (overlaps(request,sizeof *request,response,sizeof *response) ||
        overlaps(input,length,request,sizeof *request) || overlaps(output,capacity,request,sizeof *request))) ||
        overlaps(input,length,response,sizeof *response) || overlaps(output,capacity,response,sizeof *response)) return RingInvalid;
    memset(response,0,sizeof *response);
    if (!client || !request || (!input && length) || (!output && capacity) ||
        length>VenusTcpMaxCommandBytes || capacity>VenusTcpMaxReplyBytes) return RingInvalid;
    if (client->lost!=RingOk) return client->lost;
    if (!client->socket.initialized || !client->session || !client->timeout_ms) return RingInvalid;
    if (absolute && !deadline_ms) return RingInvalid;
    if (!client->next_sequence || client->next_sequence==UINT64_MAX) return poison(client,RingLimit);
    if (request->sequence || request->payload_bytes!=length || venus_tcp_request_limit(request)!=RingOk) return RingInvalid;
    size_t expected=(request->kind==RequestReply || request->kind==RequestRead) ? (size_t)request->argument_one : 0;
    if (capacity!=expected) return RingInvalid;
    venus_request_t framed=*request,decoded={0}; framed.sequence=client->next_sequence;
    uint8_t header[VenusRequestHeaderBytes];
    if (venus_request_encode(&framed,header,sizeof header)!=RingOk) return RingInvalid;
    uint64_t deadline=0,started=0;
    venus_ring_status_t status=RingOk;
    if (absolute) {
        if (cancel && atomic_load_explicit(cancel,memory_order_acquire))
            return poison(client,RingCancelled);
        started=venus_tcp_now_ms();
        if (!started || started>UINT64_MAX-client->timeout_ms)
            return poison(client,RingClosed);
        deadline=started+client->timeout_ms;
        if (deadline_ms<deadline) deadline=deadline_ms;
        if (started>=deadline) return poison(client,RingTimeout);
    }
    if (length) memcpy(client->tx,input,length);
    if (!absolute) {
        status=deadline_after(client->timeout_ms,&deadline);
        if (status!=RingOk) return poison(client,status);
    }
    status=venus_tcp_socket_send(&client->socket,header,sizeof header,deadline,cancel);
    if (status!=RingOk) return poison(client,status);
    status=venus_tcp_socket_send(&client->socket,client->tx,length,deadline,cancel);
    if (status!=RingOk) return poison(client,status);
    status=receive_exact(&client->socket,header,sizeof header,deadline,cancel);
    if (status!=RingOk) return poison(client,status);
    status=venus_request_decode(&decoded,header,sizeof header);
    if (status!=RingOk || venus_tcp_response_limit(&framed,&decoded)!=RingOk)
        return poison(client,RingCorrupt);
    if (decoded.payload_bytes>capacity) return poison(client,RingCorrupt);
    status=receive_exact(&client->socket,client->rx,decoded.payload_bytes,deadline,cancel);
    if (status!=RingOk) return poison(client,status);
    status=operation_status(decoded.status);
    if (absolute && status!=RingCorrupt && status!=RingClosed &&
        status!=RingCancelled && status!=RingTimeout) {
        if (cancel && atomic_load_explicit(cancel,memory_order_acquire))
            return poison(client,RingCancelled);
        const uint64_t completed=venus_tcp_now_ms();
        if (!completed || completed<started) return poison(client,RingClosed);
        if (completed>=deadline) return poison(client,RingTimeout);
    }
    ++client->next_sequence;
    if (status==RingCorrupt || status==RingClosed || status==RingCancelled || status==RingTimeout)
        return poison(client,status);
    if (decoded.payload_bytes) memcpy(output,client->rx,decoded.payload_bytes);
    *response=decoded;
    return status;
}
venus_ring_status_t venus_tcp_client_exchange_cancel(venus_tcp_client_t *client,
    const venus_request_t *request,const void *input,size_t length,
    venus_request_t *response,void *output,size_t capacity,const _Atomic uint32_t *cancel)
{
    return exchange_common(client,request,input,length,response,output,capacity,0,0,cancel);
}
venus_ring_status_t venus_tcp_client_exchange_until_cancel(venus_tcp_client_t *client,
    const venus_request_t *request,const void *input,size_t length,
    venus_request_t *response,void *output,size_t capacity,uint64_t deadline_ms,
    const _Atomic uint32_t *cancel)
{
    return exchange_common(client,request,input,length,response,output,capacity,deadline_ms,1,cancel);
}
venus_ring_status_t venus_tcp_client_exchange_until(void *context,
    const venus_request_t *request,const void *input,size_t length,
    venus_request_t *response,void *output,size_t capacity,uint64_t deadline_ms)
{
    return venus_tcp_client_exchange_until_cancel(context,request,input,length,response,output,capacity,deadline_ms,NULL);
}
venus_ring_status_t venus_tcp_client_exchange(void *context,const venus_request_t *request,
    const void *input,size_t length,venus_request_t *response,void *output,size_t capacity)
{
    return venus_tcp_client_exchange_cancel(context,request,input,length,response,output,capacity,NULL);
}
venus_ring_status_t venus_tcp_client_retire(venus_tcp_client_t *client,const _Atomic uint32_t *cancel)
{
    if (!client) return RingInvalid;
    if (client->lost!=RingOk) return client->lost;
    if (!client->socket.initialized || !client->session) return RingInvalid;
    uint64_t deadline=0,ack=0;
    uint8_t bytes[VenusTcpAckBytes];
    venus_ring_status_t status=deadline_after(client->timeout_ms,&deadline);
    if (status!=RingOk) return poison(client,status);
    status=venus_tcp_socket_shutdown_write(&client->socket);
    if (status!=RingOk) return poison(client,status);
    status=receive_exact(&client->socket,bytes,sizeof bytes,deadline,cancel);
    if (status!=RingOk) return poison(client,status);
    status=venus_tcp_ack_decode(&ack,bytes,sizeof bytes);
    if (status!=RingOk || ack!=client->session) return poison(client,RingCorrupt);
    venus_tcp_socket_close(&client->socket);
    client->lost=RingClosed;
    return RingOk;
}
