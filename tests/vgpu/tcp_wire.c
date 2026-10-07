/** @file tcp_wire.c @brief Native ABI guards and independent TCP packet regression. */
#include "waddle/venus_tcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/** @brief Independent fixture encoder; borrowed output, exact extent, no allocations. */
int venus_tcp_wire_oracle(unsigned kind, uint8_t *output, size_t capacity);
static const char Config[] = "{\"version\":1,\"port\":1,\"exchange_timeout_ms\":1,\"host\":\"127.0.0.1\",\"token\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"icd_path\":\"C:\\\\driver.dll\"}";
static void packet_tests(void)
{
    uint8_t wire[226], oracle[224];
    venus_tcp_client_hello_t client = {0}, decoded_client;
    venus_tcp_server_hello_t server = {0}, decoded_server;
    uint64_t identity = 0;
    for (unsigned index=0; index<32; ++index) client.token[index]=(uint8_t)(index*7+3);
    for (unsigned index=0; index<16; ++index) client.nonce[index]=(uint8_t)(index*11+5);
    server.session=UINT64_C(0x1122334455667788);
    memcpy(server.nonce, client.nonce, sizeof client.nonce);
    for (unsigned index=0; index<160; ++index) server.capabilities[index]=(uint8_t)(index*13+9);
    memset(wire, 0xa5, sizeof wire);
    assert(venus_tcp_client_hello_encode(&client, wire+1, 128)==RingOk);
    assert(venus_tcp_wire_oracle(1, oracle, 128));
    assert(!memcmp(wire+1, oracle, 128) && wire[0]==0xa5 && wire[129]==0xa5);
    assert(venus_tcp_client_hello_decode(&decoded_client, wire+1, 128)==RingOk);
    assert(!memcmp(&client, &decoded_client, sizeof client));
    for (size_t extent=0; extent<128; ++extent) assert(venus_tcp_client_hello_decode(&decoded_client, wire+1, extent)==RingCorrupt);
    assert(venus_tcp_server_hello_encode(&server, wire+1, 224)==RingOk);
    assert(venus_tcp_wire_oracle(2, oracle, 224));
    assert(!memcmp(wire+1, oracle, 224) && wire[0]==0xa5 && wire[225]==0xa5);
    assert(venus_tcp_server_hello_decode(&decoded_server, wire+1, 224)==RingOk);
    assert(!memcmp(&server, &decoded_server, sizeof server));
    for (size_t extent=0; extent<224; ++extent) assert(venus_tcp_server_hello_decode(&decoded_server, wire+1, extent)==RingCorrupt);
    assert(venus_tcp_ack_encode(server.session, wire+1, 32)==RingOk);
    assert(venus_tcp_wire_oracle(3, oracle, 32));
    assert(!memcmp(wire+1, oracle, 32));
    assert(venus_tcp_ack_decode(&identity, wire+1, 32)==RingOk && identity==server.session);
    for (size_t extent=0; extent<32; ++extent) assert(venus_tcp_ack_decode(&identity, wire+1, extent)==RingCorrupt && !identity);
    assert(venus_tcp_profile_encode(wire+1, 160)==RingOk);
    assert(venus_tcp_wire_oracle(4, oracle, 160) && !memcmp(wire+1, oracle, 160));
    assert(venus_tcp_profile_validate(wire+1, 160)==RingOk);
    wire[153]^=1;
    assert(venus_tcp_profile_validate(wire+1, 160)==RingCorrupt);
    assert(venus_tcp_token_equal(client.token, client.token));
    for (unsigned index=0; index<32; ++index) {
        memcpy(decoded_client.token, client.token, 32);
        decoded_client.token[index]^=1;
        assert(!venus_tcp_token_equal(client.token, decoded_client.token));
    }
    assert(!venus_tcp_token_equal(NULL, client.token));
}
static void config_tests(void)
{
    venus_tcp_config_t config;
    assert(venus_tcp_config_decode(&config, Config, sizeof Config-1)==RingOk);
    assert(config.version==1 && config.port==1 && config.exchange_timeout_ms==1);
    assert(!strcmp(config.host, "127.0.0.1") && !strcmp(config.icd_path,"C:\\driver.dll"));
    for (size_t extent=0; extent<sizeof Config-1; ++extent) {
        memset(&config, 0xa5, sizeof config);
        assert(venus_tcp_config_decode(&config, Config, extent)==RingCorrupt);
        for (size_t index=0; index<sizeof config; ++index) assert(((uint8_t *)&config)[index]==0);
    }
    assert(venus_tcp_config_decode(NULL, Config, sizeof Config-1)==RingInvalid);
}
static void path_tests(void)
{
    const char *Valid[]={"C:\\config.json","c:\\x","\\\\server\\share\\config.json"};
    const char *Invalid[]={"","relative","C:relative","C:/file","\\\\server","\\\\server\\","\\\\server\\\\share"};
    for (size_t index=0;index<sizeof Valid/sizeof *Valid;index++) assert(venus_tcp_windows_path_validate(Valid[index],strlen(Valid[index])+1)==RingOk);
    for (size_t index=0;index<sizeof Invalid/sizeof *Invalid;index++) assert(venus_tcp_windows_path_validate(Invalid[index],strlen(Invalid[index])+1)==RingCorrupt);
    assert(venus_tcp_windows_path_validate(NULL,1)==RingInvalid);
    assert(venus_tcp_windows_path_validate(Valid[0],0)==RingInvalid);
    assert(venus_tcp_windows_path_validate(Valid[0],1025)==RingLimit);
    uint8_t bytes[1026];memset(bytes,0xa5,sizeof bytes);bytes[1]='C';bytes[2]=':';bytes[3]='\\';
    memset(bytes+4,'x',1020);bytes[1024]=0;
    assert(venus_tcp_windows_path_validate(bytes+1,1024)==RingOk);
    for (size_t extent=1;extent<1024;extent++) assert(venus_tcp_windows_path_validate(bytes+1,extent)==RingCorrupt);
    assert(bytes[0]==0xa5 && bytes[1025]==0xa5);
    bytes[4]=0;assert(venus_tcp_windows_path_validate(bytes+1,1024)==RingCorrupt);
    bytes[4]=255;assert(venus_tcp_windows_path_validate(bytes+1,1024)==RingCorrupt);
}
static void envelope_tests(void)
{
    venus_request_t request={.kind=RequestReply,.sequence=1,.argument_one=4096};
    venus_request_t response={.kind=RequestReply,.direction=1,.sequence=1,.payload_bytes=4096};
    assert(venus_tcp_request_limit(&request)==RingOk);
    assert(venus_tcp_response_limit(&request, &response)==RingOk);
    request.argument_one++;
    assert(venus_tcp_request_limit(&request)==RingLimit);
    assert(venus_tcp_response_limit(&request, &response)==RingCorrupt);
    request.kind=RequestPresent;
    assert(venus_tcp_request_limit(&request)==RingInvalid);
    response.sequence++;
    assert(venus_tcp_response_limit(&request, &response)==RingCorrupt);
}
/** @brief Execute all native ABI regressions; no heap or retained resources. @return Zero success. */
int main(void)
{
    for (unsigned iteration=0; iteration<32; ++iteration) { packet_tests(); config_tests(); envelope_tests(); path_tests(); }
    puts("TCP literal packets, strict config and native bounds: PASS (32 cycles)");
    return 0;
}
