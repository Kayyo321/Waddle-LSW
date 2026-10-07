/** @file tcp_bootstrap_windows.c @brief Real Windows ACL/module/TCP bootstrap cycles.
 * Peer capability and retirement records are deliberately synthetic. This verifies
 * native SDK ownership plus actual ICD capability binding, never physical GPU work.
 */
#include "waddle/venus_tcp.h"
#include <winsock2.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sddl.h>
/** @brief Independent immutable pinned capability oracle; caller output borrowed for call. */
int venus_tcp_wire_oracle(unsigned kind,uint8_t *output,size_t capacity);
/** @brief Synthetic retirement variant; neither value claims a real renderer. */
typedef enum peer_mode_t { PeerNormal,PeerRetireMismatch } peer_mode_t;
/** @brief Sole test peer owns listener until moved/closed by its native thread. */
typedef struct peer_t { venus_tcp_socket_t listener;peer_mode_t mode;uint32_t port; } peer_t;
static unsigned resource_count(void) { DWORD count=0;assert(GetProcessHandleCount(GetCurrentProcess(),&count));return count; }
static void sleep_ms(unsigned milliseconds) { Sleep(milliseconds); }
static void send_bytes(venus_tcp_socket_t *socket_value,const void *bytes,size_t length)
{ assert(venus_tcp_socket_send(socket_value,bytes,length,venus_tcp_now_ms()+1000,NULL)==RingOk); }
static void receive_bytes(venus_tcp_socket_t *socket_value,void *bytes,size_t length)
{ size_t received=0;assert(venus_tcp_socket_receive(socket_value,bytes,length,&received,venus_tcp_now_ms()+1000,NULL)==RingOk && received==length); }
/** @brief Actual DLL additive function ABI; borrowed while reference retained. */
typedef venus_ring_status_t (*bootstrap_start_t)(const char *,size_t);
/** @brief Actual quiescent release ABI, no caller storage; errors retain owner. */
typedef venus_ring_status_t (*bootstrap_stop_t)(void);
/** @brief Sole lifecycle caller copied identity query ABI; no allocation. */
typedef uint64_t (*bootstrap_session_t)(void);
/** @brief Exact trusted test-peer retirement identity release ABI. */
typedef venus_ring_status_t (*bootstrap_abandon_t)(uint64_t);
static void private_config(const char *path,const char *module,uint32_t port,int public_acl)
{
    HANDLE token=NULL;assert(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token));
    _Alignas(TOKEN_USER) uint8_t bytes[1024];DWORD length=0;
    assert(GetTokenInformation(token,TokenUser,bytes,sizeof bytes,&length));TOKEN_USER user={0};memcpy(&user,bytes,sizeof user);
    char *sid=NULL;assert(ConvertSidToStringSidA(user.User.Sid,&sid));
    char acl[1024];assert(snprintf(acl,sizeof acl,"O:%sD:P(A;;FA;;;%s)%s",sid,sid,public_acl ? "(A;;GR;;;WD)" : "")>0);
    PSECURITY_DESCRIPTOR descriptor=NULL;assert(ConvertStringSecurityDescriptorToSecurityDescriptorA(acl,SDDL_REVISION_1,&descriptor,NULL));
    SECURITY_ATTRIBUTES attributes={.nLength=sizeof attributes,.lpSecurityDescriptor=descriptor};
    assert(DeleteFileA(path) || GetLastError()==ERROR_FILE_NOT_FOUND);
    HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,0,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);assert(file!=INVALID_HANDLE_VALUE);
    char escaped[VenusTcpMaxWindowsPathBytes*2];size_t position=0;
    for(size_t index=0;module[index];index++){if(module[index]=='\\')escaped[position++]='\\';escaped[position++]=module[index];}escaped[position]=0;
    char config[4096];int count=snprintf(config,sizeof config,"{\"version\":1,\"host\":\"127.0.0.1\",\"port\":%u,\"token\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"exchange_timeout_ms\":1000,\"icd_path\":\"%s\"}",port,escaped);assert(count>0 && (size_t)count<sizeof config);
    DWORD written=0;assert(WriteFile(file,config,(DWORD)count,&written,NULL) && written==(DWORD)count);assert(CloseHandle(file));
    assert(!LocalFree(descriptor) && !LocalFree(sid));assert(CloseHandle(token));venus_tcp_scrub(config,sizeof config);venus_tcp_scrub(bytes,sizeof bytes);
}
/** @brief Native socket peer owns no GPU/context; matching or deliberately lost synthetic Ack. */
static DWORD WINAPI bootstrap_peer_run(LPVOID argument)
{
    peer_t *peer=argument;venus_tcp_socket_t socket_value={0};
    assert(venus_tcp_socket_accept(&peer->listener,&socket_value,venus_tcp_now_ms()+5000,NULL)==RingOk);venus_tcp_socket_close(&peer->listener);
    uint8_t bytes[VenusTcpServerHelloBytes];receive_bytes(&socket_value,bytes,VenusTcpClientHelloBytes);
    venus_tcp_client_hello_t client;assert(venus_tcp_client_hello_decode(&client,bytes,VenusTcpClientHelloBytes)==RingOk);
    venus_tcp_server_hello_t hello={.session=0x1122334455667788ull};memcpy(hello.nonce,client.nonce,16);assert(venus_tcp_wire_oracle(4,hello.capabilities,VenusCapabilitiesBytes));
    assert(venus_tcp_server_hello_encode(&hello,bytes,sizeof bytes)==RingOk);send_bytes(&socket_value,bytes,sizeof bytes);
    receive_bytes(&socket_value,bytes,VenusCapabilitiesBytes);assert(venus_tcp_profile_validate(bytes,VenusCapabilitiesBytes)==RingOk);
    assert(venus_tcp_ack_encode(hello.session,bytes,VenusTcpAckBytes)==RingOk);send_bytes(&socket_value,bytes,VenusTcpAckBytes);
    size_t received=0;
    for (;;) {
        venus_ring_status_t status=venus_tcp_socket_receive(&socket_value,bytes,VenusRequestHeaderBytes,&received,venus_tcp_now_ms()+5000,NULL);
        if(status==RingClosed){assert(!received);break;}
        assert(status==RingOk && received==VenusRequestHeaderBytes);
        venus_request_t request={0};assert(venus_request_decode(&request,bytes,VenusRequestHeaderBytes)==RingOk);
        assert(request.kind==RequestReply && request.argument_one==1 && (request.argument_zero==524287 || request.argument_zero==524288));
        venus_request_t response={.kind=RequestReply,.direction=1,.sequence=request.sequence,
            .status=request.argument_zero==524287 ? RequestSuccess : RequestInvalid,
            .payload_bytes=request.argument_zero==524287 ? 1 : 0};
        assert(venus_request_encode(&response,bytes,VenusRequestHeaderBytes)==RingOk);send_bytes(&socket_value,bytes,VenusRequestHeaderBytes);
        if(response.payload_bytes){uint8_t zero=0;send_bytes(&socket_value,&zero,1);}
    }
    assert(venus_tcp_ack_encode(hello.session+(peer->mode==PeerRetireMismatch),bytes,VenusTcpAckBytes)==RingOk);send_bytes(&socket_value,bytes,VenusTcpAckBytes);
    venus_tcp_socket_close(&socket_value);venus_tcp_scrub(&client,sizeof client);return 0;
}
/** @brief Initialize independently traced process lifetime GUI dependencies.
 * @note Sole test thread. Each module reference is acquired from System32 and
 * released here. No project DLL is loaded. USER32 ETW and GDI desktop/IO cache
 * handles belong to Windows process initialization; the exact baseline is taken
 * afterward and must hold across every project lifetime and final DLL release.
 */
static void warm_system_gui(void)
{
    HMODULE user32=LoadLibraryExA("user32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE gdi32=LoadLibraryExA("gdi32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dwmapi=LoadLibraryExA("dwmapi.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(user32 && gdi32 && dwmapi);
    assert(FreeLibrary(dwmapi) && FreeLibrary(gdi32) && FreeLibrary(user32));
}
static void warm_client(void)
{
    peer_t peer={0};assert(venus_tcp_socket_listen(&peer.listener,0,&peer.port)==RingOk);
    HANDLE thread=CreateThread(NULL,0,bootstrap_peer_run,&peer,0,NULL);assert(thread);
    venus_tcp_config_t config={.version=1,.port=peer.port,.exchange_timeout_ms=1000,.host="127.0.0.1",.icd_path="C:\\fixture.dll"};venus_tcp_client_t client={0};
    assert(venus_tcp_client_init(&client,&config,NULL)==RingOk);assert(venus_tcp_client_retire(&client,NULL)==RingOk);venus_tcp_client_free(&client);
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);assert(CloseHandle(thread));
    assert(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)==INVALID_SOCKET && WSAGetLastError()==WSANOTINITIALISED);
}
static void native_baseline(unsigned baseline)
{
    for(unsigned attempt=0;attempt<100;attempt++){if(resource_count()==baseline)return;sleep_ms(10);}
    fprintf(stderr,"Bootstrap native handles baseline=%u actual=%u\n",baseline,resource_count());
    assert(resource_count()==baseline);
}
/** @brief Run exact absolute bootstrap DLL + ICD paths, no global loader changes.
 * @param[in] argc/argv Three immutable absolute paths: DLL, ICD, fresh config.
 * @return Zero after every native owner/thread is released; assertions fail closed.
 * @note Sole lifecycle thread; test-only synthetic peer owns Windows thread/socket.
 */
int main(int argc,char **argv)
{
    assert(argc==4);HMODULE library=LoadLibraryExA(argv[1],NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);assert(library);
    bootstrap_start_t start=NULL;bootstrap_stop_t stop=NULL;bootstrap_session_t session=NULL;bootstrap_abandon_t abandon=NULL;
    FARPROC proc=GetProcAddress(library,"venus_tcp_bootstrap_start");memcpy(&start,&proc,sizeof proc);
    proc=GetProcAddress(library,"venus_tcp_bootstrap_stop");memcpy(&stop,&proc,sizeof proc);
    proc=GetProcAddress(library,"venus_tcp_bootstrap_session");memcpy(&session,&proc,sizeof proc);
    proc=GetProcAddress(library,"venus_tcp_bootstrap_abandon");memcpy(&abandon,&proc,sizeof proc);assert(start && stop && session && abandon);
    assert(start(NULL,0)==RingInvalid && stop()==RingOk && !session() && abandon(0)==RingInvalid);
    /* Warm cryptographic/socket runtime before taking an exact native baseline. */
    uint8_t random[16];assert(venus_tcp_random(random,sizeof random)==RingOk);venus_tcp_scrub(random,sizeof random);
    venus_tcp_socket_t warm={0};uint32_t warm_port=0;assert(venus_tcp_socket_listen(&warm,0,&warm_port)==RingOk);venus_tcp_socket_close(&warm);
    /* One standalone complete client cycle initializes the system mswsock
     * process IO completion port, independently traced to native connect. */
    warm_client();
    /* Initialize SDK RPC/thread-pool and this retained DLL's C runtime before
     * the exact repeated-owner baseline. Diagnostic snapshots separately record
     * one-time SDK ALPC/thread-pool objects and eight ntmarta.dll semaphores acquired during the initial SDK owner query. */
    private_config(argv[3],argv[2],12345,1);assert(start(argv[3],strlen(argv[3])+1)==RingInvalid);assert(!session() && stop()==RingOk);
    warm_system_gui();assert(!GetModuleHandleA(argv[2]));
    sleep_ms(1000);unsigned baseline=resource_count();
    for(unsigned cycle=0;cycle<24;cycle++) {
        private_config(argv[3],argv[2],12345,1);assert(start(argv[3],strlen(argv[3])+1)==RingInvalid);assert(!session() && stop()==RingOk);native_baseline(baseline);
    }
    for(unsigned cycle=0;cycle<24;cycle++) {
        peer_t peer={.mode=cycle%2 ? PeerRetireMismatch : PeerNormal};assert(venus_tcp_socket_listen(&peer.listener,0,&peer.port)==RingOk);
        HANDLE thread=CreateThread(NULL,0,bootstrap_peer_run,&peer,0,NULL);assert(thread);
        private_config(argv[3],argv[2],peer.port,0);
        venus_ring_status_t status=start(argv[3],strlen(argv[3])+1);assert(status==RingOk);uint64_t identity=session();assert(identity==0x1122334455667788ull);
        assert(GetModuleHandleA(argv[2]));assert(start(argv[3],strlen(argv[3])+1)==RingAgain);assert(abandon(identity+1)==RingInvalid && session()==identity);
        if(peer.mode==PeerRetireMismatch) {
            assert(stop()==RingCorrupt && session()==identity && GetModuleHandleA(argv[2]));
            assert(abandon(identity+1)==RingInvalid && session()==identity);
            assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
            /* The joined test peer has no receiver/GPU object; this is only a
             * synthetic unit supervisor assertion, no real host proof. */
            assert(abandon(identity)==RingOk && !session());
        } else assert(stop()==RingOk && !session());
        assert(stop()==RingOk);
        assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);assert(CloseHandle(thread));assert(!GetModuleHandleA(argv[2]));assert(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)==INVALID_SOCKET && WSAGetLastError()==WSANOTINITIALISED);native_baseline(baseline);
    }
    assert(DeleteFileA(argv[3]));assert(FreeLibrary(library));
    assert(!GetModuleHandleA(argv[1]));native_baseline(baseline);
    fprintf(stderr,"Bootstrap quiescent handles=%u afterDLLrelease=%u\n",baseline,resource_count());
    puts("Windows bootstrap: private SID ACL rejection,24 real TCP/ICD binding/module cycles (12matching+12lost synthetic retirement Ack), exact native handle baseline PASS (synthetic peer; no GPU)");return 0;
}
