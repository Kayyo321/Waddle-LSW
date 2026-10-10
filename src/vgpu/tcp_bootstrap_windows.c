/** @file tcp_bootstrap_windows.c @brief Explicit Windows TCP startup; no project loader-entry work. */
#include "waddle/venus_tcp.h"
#include "waddle/venus_command.h"
#ifdef TcpBootstrapSdkTests
#include "../../tests/vgpu/tcp_bootstrap_sdk.h"
#else
#include <windows.h>
#include <aclapi.h>
#endif
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(void *)==8,"TCP bootstrap supports the documented Windows x64 ABI");
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Owned cancellation requires lock-free integer storage");
/** @brief Resolved additive ICD bind ABI; borrowed immutable160-byte capset during call.
 * Callback/context stay borrowed until unbind/abandon; ICD mutex serializes application calls.
 * Returns existing ICD Ring status, allocates nothing, never owns caller transport storage.
 */
typedef venus_ring_status_t (*bootstrap_exchange_until_t)(void *,const venus_request_t *,const void *,size_t,venus_request_t *,void *,size_t,uint64_t);
/** @brief Borrowed monotonic clock; zero reports a clock failure. */
typedef uint64_t (*bootstrap_clock_t)(void *);
/** @brief Bounded query binding; callback/context borrowed through unbind. */
typedef venus_ring_status_t (*icd_bind_t)(bootstrap_exchange_until_t,bootstrap_clock_t,void *,const venus_capabilities_t *,uint32_t);
/** @brief Resolved quiescent ICD release ABI; no arguments/storage; Ok or Again while objects live. */
typedef venus_ring_status_t (*icd_unbind_t)(void);
/** @brief Resolved trusted receiver-retired abandonment ABI; no storage/return; application quiesced. */
typedef void (*icd_abandon_t)(void);
/** @brief Private process singleton; lifecycle APIs exclusive, callback serialized by ICD mutex.
 * Owns fixed client storage and one LoadLibraryEx reference until release; never copied.
 * Session and resolved pointers remain live through failed retirement/module-release retries.
 */
typedef struct bootstrap_t {
    venus_tcp_client_t client; /**< Private sequential socket/staging owner; lifetime until retirement. */
    HMODULE module; /**< Sole owned exact ICD module reference; NULL only when empty. */
    icd_bind_t bind; /**< Borrowed function in retained module, NULL before successful resolution. */
    icd_unbind_t unbind; /**< Borrowed function; usable only while module retained. */
    icd_abandon_t abandon; /**< Borrowed function; trusted exact retirement precondition. */
    uint64_t session; /**< Copied nonzero admitted identity, preserved until module release succeeds. */
    uint32_t starting; /**< Bind probes may use the authenticated client before publication. */
    uint32_t bound; /**< This owner installed the callback; incumbent bindings are never abandoned. */
    uint32_t retired; /**< Matching Ack or trusted exact-session assertion has proved retirement. */
    HANDLE pending_config; /**< Failed file release retained until stop retries native closure. */
    HANDLE pending_token; /**< Failed process-token closure retained with one sole owner. */
    PSECURITY_DESCRIPTOR pending_descriptor; /**< Failed LocalFree retained OS allocation, no borrowed pointers. */
    _Atomic uint32_t cancel; /**< Stable owned flag borrowed only by active socket callbacks. */
} bootstrap_t;
static bootstrap_t bootstrap;
static int pending_native(void)
{
    return bootstrap.pending_config || bootstrap.pending_token || bootstrap.pending_descriptor;
}
static venus_ring_status_t close_native(HANDLE *handle)
{
    if (!*handle) return RingOk;
    if (!CloseHandle(*handle)) return RingClosed;
    *handle=NULL;
    return RingOk;
}
static venus_ring_status_t release_pending(void)
{
    venus_ring_status_t status=RingOk;
    if (close_native(&bootstrap.pending_token)!=RingOk) status=RingClosed;
    if (bootstrap.pending_descriptor) {
        if (LocalFree(bootstrap.pending_descriptor)) status=RingClosed;
        else bootstrap.pending_descriptor=NULL;
    }
    if (close_native(&bootstrap.pending_config)!=RingOk) status=RingClosed;
    return status;
}
static venus_ring_status_t private_owner(HANDLE file)
{
    HANDLE token=NULL; PSECURITY_DESCRIPTOR descriptor=NULL; PSID owner_sid=NULL; PACL dacl=NULL;
    _Alignas(TOKEN_USER) uint8_t token_bytes[1024]; DWORD token_length=0;
    venus_ring_status_t status=RingInvalid;
    if (GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        &owner_sid,NULL,&dacl,NULL,&descriptor)!=ERROR_SUCCESS) { status=RingClosed; goto cleanup; }
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) ||
        !GetTokenInformation(token,TokenUser,token_bytes,sizeof token_bytes,&token_length)) { status=RingClosed; goto cleanup; }
    if (token_length<sizeof(TOKEN_USER) || token_length>sizeof token_bytes || !owner_sid || !dacl || !dacl->AceCount) goto cleanup;
    TOKEN_USER user={0}; memcpy(&user,token_bytes,sizeof user);
    if (!user.User.Sid || !IsValidSid(user.User.Sid) || !IsValidSid(owner_sid) || !EqualSid(owner_sid,user.User.Sid)) goto cleanup;
    for (DWORD index=0;index<dacl->AceCount;index++) {
        void *entry=NULL;
        if (!GetAce(dacl,index,&entry) || !entry) goto cleanup;
        ACE_HEADER *header=entry;
        if (header->AceType!=ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags&INHERITED_ACE) ||
            header->AceSize<offsetof(ACCESS_ALLOWED_ACE,SidStart)+8) goto cleanup;
        ACCESS_ALLOWED_ACE *ace=entry;
        PSID granted=&ace->SidStart;
        if (!IsValidSid(granted) || GetLengthSid(granted)>header->AceSize-offsetof(ACCESS_ALLOWED_ACE,SidStart) ||
            !EqualSid(granted,user.User.Sid)) goto cleanup;
    }
    status=RingOk;
cleanup:
    venus_tcp_scrub(token_bytes,sizeof token_bytes);
    if (close_native(&token)!=RingOk) { bootstrap.pending_token=token; status=RingClosed; }
    if (descriptor && LocalFree(descriptor)) { bootstrap.pending_descriptor=descriptor; status=RingClosed; }
    return status;
}
static venus_ring_status_t load_config(const char *path,venus_tcp_config_t *config)
{
    HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file==INVALID_HANDLE_VALUE) return RingClosed;
    BY_HANDLE_FILE_INFORMATION information={0}; uint8_t bytes[VenusTcpMaxConfigBytes+1]; size_t received=0;
    venus_ring_status_t status=RingInvalid;
    if (GetFileType(file)!=FILE_TYPE_DISK || !GetFileInformationByHandle(file,&information)) { status=RingClosed; goto cleanup; }
    if ((information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
        information.nNumberOfLinks!=1 || information.nFileSizeHigh ||
        !information.nFileSizeLow || information.nFileSizeLow>VenusTcpMaxConfigBytes) goto cleanup;
    status=private_owner(file); if (status!=RingOk) goto cleanup;
    while (received<sizeof bytes) {
        DWORD count=0;
        if (!ReadFile(file,bytes+received,(DWORD)(sizeof bytes-received),&count,NULL)) { status=RingClosed; goto cleanup; }
        if (!count) break;
        received+=count;
    }
    if (received!=information.nFileSizeLow) { status=RingCorrupt; goto cleanup; }
    status=venus_tcp_config_decode(config,bytes,received);
cleanup:
    venus_tcp_scrub(bytes,sizeof bytes);
    if (close_native(&file)!=RingOk) { bootstrap.pending_config=file; status=RingClosed; }
    return status;
}
static int exact_module(HMODULE module,const char *path)
{
    char expected[VenusTcpMaxWindowsPathBytes],loaded[VenusTcpMaxWindowsPathBytes],actual[VenusTcpMaxWindowsPathBytes];
    DWORD length=GetFullPathNameA(path,sizeof expected,expected,NULL);
    if (!length || length>=sizeof expected) return 0;
    length=GetModuleFileNameA(module,loaded,sizeof loaded);
    if (!length || length>=sizeof loaded) return 0;
    length=GetFullPathNameA(loaded,sizeof actual,actual,NULL);
    return length && length<sizeof actual && !_stricmp(actual,expected);
}
static venus_ring_status_t callback(void *context,const venus_request_t *request,const void *input,size_t length,
    venus_request_t *response,void *output,size_t capacity,uint64_t deadline_ms)
{
    if (context!=&bootstrap || (!bootstrap.bound && !bootstrap.starting)) return RingClosed;
    return venus_tcp_client_exchange_until_cancel(&bootstrap.client,request,input,length,response,output,capacity,deadline_ms,&bootstrap.cancel);
}
static uint64_t clock_ms(void *context)
{
    if(context!=&bootstrap || (!bootstrap.bound && !bootstrap.starting))return 0;
    return venus_tcp_now_ms();
}
static venus_ring_status_t release_module(void)
{
    venus_ring_status_t status=release_pending(); if (status!=RingOk) return status;
    venus_tcp_client_free(&bootstrap.client);
    if (bootstrap.module && !FreeLibrary(bootstrap.module)) return RingClosed;
    bootstrap.module=NULL; bootstrap.bind=NULL; bootstrap.unbind=NULL; bootstrap.abandon=NULL;
    bootstrap.session=0; bootstrap.starting=0; bootstrap.bound=0; bootstrap.retired=0;
    atomic_store_explicit(&bootstrap.cancel,0,memory_order_release);
    return RingOk;
}
/** @brief Header documents bounded arguments, every owner/lifetime, retained errors and threads. */
venus_ring_status_t venus_tcp_bootstrap_start(const char *path,size_t bytes)
{
    if (bootstrap.module || pending_native()) return RingAgain;
    venus_ring_status_t status=venus_tcp_windows_path_validate(path,bytes);
    if (status!=RingOk) return RingInvalid;
    venus_tcp_config_t config={0};
    status=load_config(path,&config); if (status!=RingOk) goto failure;
    bootstrap.module=LoadLibraryExA(config.icd_path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bootstrap.module) { status=RingClosed; goto failure; }
    if (!exact_module(bootstrap.module,config.icd_path)) { status=RingInvalid; goto failure; }
    FARPROC procedure=GetProcAddress(bootstrap.module,"venus_icd_bind_timed");
    _Static_assert(sizeof procedure==sizeof bootstrap.bind,"Resolved x64 function representation");
    memcpy(&bootstrap.bind,&procedure,sizeof procedure);
    procedure=GetProcAddress(bootstrap.module,"venus_icd_unbind");memcpy(&bootstrap.unbind,&procedure,sizeof procedure);
    procedure=GetProcAddress(bootstrap.module,"venus_icd_abandon");memcpy(&bootstrap.abandon,&procedure,sizeof procedure);
    if (!bootstrap.bind || !bootstrap.unbind || !bootstrap.abandon) { status=RingInvalid; goto failure; }
    status=venus_tcp_client_init(&bootstrap.client,&config,&bootstrap.cancel);
    venus_tcp_scrub(&config,sizeof config); if (status!=RingOk) goto failure;
    bootstrap.session=bootstrap.client.session;
    bootstrap.starting=1;
    status=bootstrap.bind(callback,clock_ms,&bootstrap,&bootstrap.client.capabilities,524288);
    bootstrap.starting=0;
    if (status!=RingOk) goto failure;
    bootstrap.bound=1;
    return RingOk;
failure:
    venus_tcp_scrub(&config,sizeof config);
    if (pending_native()) return status==RingOk ? RingClosed : status;
    if (bootstrap.session && !bootstrap.retired) {
        venus_ring_status_t retirement=venus_tcp_client_retire(&bootstrap.client,&bootstrap.cancel);
        if (retirement!=RingOk) return retirement;
        bootstrap.retired=1;
    }
    venus_ring_status_t released=release_module();
    return released==RingOk ? status : released;
}
/** @brief Header requires application/callback quiescence and defines retained retry phases. */
venus_ring_status_t venus_tcp_bootstrap_stop(void)
{
    venus_ring_status_t pending=release_pending(); if (pending!=RingOk) return pending;
    if (!bootstrap.module) return RingOk;
    if (bootstrap.bound) {
        venus_ring_status_t status=bootstrap.unbind(); if (status!=RingOk) return status;
        bootstrap.bound=0;
    }
    if (bootstrap.session && !bootstrap.retired) {
        venus_ring_status_t status=venus_tcp_client_retire(&bootstrap.client,&bootstrap.cancel);
        if (status!=RingOk) return status;
        bootstrap.retired=1;
    }
    return release_module();
}
/** @brief Header defines sole-lifecycle-thread copied identity; no native work or ownership transfer. */
uint64_t venus_tcp_bootstrap_session(void) { return bootstrap.session; }
/** @brief Header requires trusted exact receiver retirement before this matching-session assertion. */
venus_ring_status_t venus_tcp_bootstrap_abandon(uint64_t retired_session)
{
    if (!retired_session || retired_session!=bootstrap.session || !bootstrap.module) return RingInvalid;
    if (bootstrap.bound) { bootstrap.abandon(); bootstrap.bound=0; }
    bootstrap.retired=1;
    return release_module();
}
