/** @file tcp_bootstrap_sdk.c @brief Native resource-owner faults; synthetic transport admission.
 * SDK shim owns real Linux file descriptors, token heap cells, security descriptors
 * and module heap references. Failed destructors retain those actual owners until
 * retry. Client/ICD admission here is synthetic and proves no remote retirement/GPU.
 */
#define TcpBootstrapSdkTests
#include "tcp_bootstrap_sdk.h"
#include "waddle/venus_tcp.h"
#include "waddle/venus_command.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
static const char *fault;
static unsigned fault_call=1,fault_seen,allocations,module_releases,retirement_calls;
static venus_ring_status_t bind_status=RingOk,unbind_status=RingOk,retire_status=RingOk,init_status=RingOk;
static unsigned abandoned,fail_module_release_once;
static const char ConfigPath[]="C:\\private.json";
static const char ModulePath[]="C:\\fixture.dll";
static char native_path[256];
static int hit(const char *name) { return fault && !strcmp(fault,name) && ++fault_seen==fault_call; }
static void *acquire(size_t size) { void *owner=calloc(1,size);assert(owner);allocations++;return owner; }
static void release(void *owner) { assert(owner && allocations);allocations--;free(owner); }
/** Test token/file handle discriminator; actual file descriptor remains owned. */
typedef struct handle_t { int fd,token; } handle_t;
/** Test SDK allocation packs borrowed SID/DACL/ACE within one owned descriptor. */
typedef struct security_t { uint32_t owner[4]; ACL acl; ACCESS_ALLOWED_ACE ace; } security_t;
static uint32_t current_sid[4]={1,2,3,4};
#include "../../src/vgpu/tcp_bootstrap_windows.c"
static bootstrap_exchange_until_t bound_callback;
static void *bound_context;
HANDLE CreateFileA(const char *path,DWORD access,DWORD sharing,void *security,DWORD disposition,DWORD flags,HANDLE template)
{
    assert(!strcmp(path,ConfigPath) && access==GENERIC_READ && sharing==FILE_SHARE_READ && !security && disposition==OPEN_EXISTING && flags==FILE_FLAG_OPEN_REPARSE_POINT && !template);
    if(hit("create"))return INVALID_HANDLE_VALUE;
    int fd=open(native_path,O_RDONLY);assert(fd>=0);handle_t *handle=acquire(sizeof *handle);handle->fd=fd;return handle;
}
int CloseHandle(HANDLE owner)
{
    handle_t *handle=owner;assert(handle);
    if(hit(handle->token ? "close_token" : "close_file"))return 0;
    if(!handle->token)assert(close(handle->fd)==0);
    release(handle);return 1;
}
DWORD GetFileType(HANDLE file) { assert(file);return hit("type") ? 0 : FILE_TYPE_DISK; }
int GetFileInformationByHandle(HANDLE owner,BY_HANDLE_FILE_INFORMATION *out)
{
    handle_t *handle=owner;struct stat information;assert(fstat(handle->fd,&information)==0);
    if(hit("information"))return 0;
    out->nNumberOfLinks=hit("links") ? 2 : 1;out->nFileSizeLow=(DWORD)information.st_size;
    if(hit("directory"))out->dwFileAttributes=FILE_ATTRIBUTE_DIRECTORY;
    if(hit("reparse"))out->dwFileAttributes=FILE_ATTRIBUTE_REPARSE_POINT;
    if(hit("high_size"))out->nFileSizeHigh=1;
    if(hit("size_mismatch"))out->nFileSizeLow++;
    return 1;
}
int ReadFile(HANDLE owner,void *output,DWORD capacity,DWORD *count,void *overlapped)
{
    handle_t *handle=owner;assert(!overlapped);
    if(hit("read"))return 0;
    ssize_t received=read(handle->fd,output,capacity);assert(received>=0);*count=(DWORD)received;return 1;
}
DWORD GetSecurityInfo(HANDLE file,int type,DWORD flags,PSID *owner,PSID *group,PACL *dacl,PACL *sacl,PSECURITY_DESCRIPTOR *descriptor)
{
    assert(file && type==SE_FILE_OBJECT && flags==(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION) && !group && !sacl);
    if(hit("security"))return 1;
    security_t *value=acquire(sizeof *value);memcpy(value->owner,current_sid,sizeof current_sid);
    value->acl.AceCount=1;value->ace.Header.AceSize=sizeof value->ace;memcpy(&value->ace.SidStart,current_sid,sizeof current_sid);
    if(hit("owner_mismatch"))value->owner[1]++;
    if(hit("sid_mismatch"))value->ace.rest[0]++;
    if(hit("empty_acl"))value->acl.AceCount=0;
    if(hit("ace_type"))value->ace.Header.AceType=1;
    if(hit("ace_inherited"))value->ace.Header.AceFlags=INHERITED_ACE;
    if(hit("ace_short"))value->ace.Header.AceSize=8;
    if(hit("sid_extent"))value->ace.Header.AceSize=16;
    *owner=hit("owner_null") ? NULL : value->owner;*dacl=hit("dacl_null") ? NULL : &value->acl;*descriptor=value;return ERROR_SUCCESS;
}
int OpenProcessToken(HANDLE process,DWORD access,HANDLE *out)
{
    assert(process==GetCurrentProcess() && access==TOKEN_QUERY);
    if(hit("token_open"))return 0;
    handle_t *token=acquire(sizeof *token);token->token=1;*out=token;return 1;
}
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-2; }
int GetTokenInformation(HANDLE token,int type,void *bytes,DWORD capacity,DWORD *length)
{
    assert(token && type==TokenUser && capacity==1024);
    if(hit("token_info"))return 0;
    TOKEN_USER user={.User={.Sid=hit("user_null") ? NULL : current_sid}};memcpy(bytes,&user,sizeof user);
    *length=hit("token_short") ? 0 : (hit("token_long") ? 1025 : sizeof user);return 1;
}
int IsValidSid(PSID sid) { assert(sid);return !hit("sid_invalid"); }
int EqualSid(PSID left,PSID right) { assert(left && right);return !memcmp(left,right,sizeof current_sid); }
int GetAce(PACL acl,DWORD index,void **entry)
{
    assert(acl && index==0);
    if(hit("ace_read"))return 0;
    security_t *value=(security_t *)((char *)acl - offsetof(security_t,acl));*entry=hit("ace_null") ? NULL : &value->ace;return 1;
}
DWORD GetLengthSid(PSID sid) { assert(sid);return sizeof current_sid; }
void *LocalFree(void *owner) { if(hit("local_free"))return owner;release(owner);return NULL; }
HMODULE LoadLibraryExA(const char *path,HANDLE file,DWORD flags)
{
    assert(!strcmp(path,ModulePath) && !file && flags==(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32));
    return hit("load") ? NULL : acquire(1);
}
int FreeLibrary(HMODULE module) { assert(module);if(fail_module_release_once){fail_module_release_once=0;return 0;}if(hit("free_module"))return 0;release(module);module_releases++;return 1; }
static venus_ring_status_t bind_icd(bootstrap_exchange_until_t callback_value,bootstrap_clock_t clock_value,void *context,const venus_capabilities_t *capabilities,uint32_t reply_bytes)
{
    assert(callback_value && clock_value && context && capabilities->wire_format_version==42 && reply_bytes==524288);
    assert(clock_value(context)>0);
    venus_request_t request={.kind=RequestPoll},response={0};
    assert(callback_value(context,&request,NULL,0,&response,NULL,0,2000)==RingAgain);
    if(bind_status==RingOk){bound_callback=callback_value;bound_context=context;}
    return bind_status;
}
static venus_ring_status_t unbind_icd(void) { if(unbind_status==RingOk){bound_callback=NULL;bound_context=NULL;}return unbind_status; }
static void abandon_icd(void) { abandoned++;bound_callback=NULL;bound_context=NULL; }
FARPROC GetProcAddress(HMODULE module,const char *name)
{
    assert(module);FARPROC output=NULL;
    if(hit("export"))return NULL;
    if(!strcmp(name,"venus_icd_bind_timed")){icd_bind_t value=bind_icd;memcpy(&output,&value,sizeof output);}
    else if(!strcmp(name,"venus_icd_unbind")){icd_unbind_t value=unbind_icd;memcpy(&output,&value,sizeof output);}
    else {assert(!strcmp(name,"venus_icd_abandon"));icd_abandon_t value=abandon_icd;memcpy(&output,&value,sizeof output);}
    return output;
}
DWORD GetFullPathNameA(const char *path,DWORD capacity,char *output,char **part)
{
    assert(capacity==VenusTcpMaxWindowsPathBytes && !part);
    if(hit("full_zero"))return 0;
    if(hit("full_long"))return capacity;
    size_t bytes=strlen(path);assert(bytes<capacity);memcpy(output,path,bytes+1);return (DWORD)bytes;
}
DWORD GetModuleFileNameA(HMODULE module,char *output,DWORD capacity)
{
    assert(module && capacity==VenusTcpMaxWindowsPathBytes);
    if(hit("module_zero"))return 0;
    if(hit("module_long"))return capacity;
    const char *path=hit("module_mismatch") ? "C:\\other.dll" : ModulePath;strcpy(output,path);return (DWORD)strlen(path);
}
venus_ring_status_t venus_tcp_client_init(venus_tcp_client_t *client,const venus_tcp_config_t *config,const _Atomic uint32_t *cancel)
{
    assert(config->port==12345 && cancel);if(init_status!=RingOk)return init_status;
    client->session=123;client->capabilities.wire_format_version=42;return RingOk;
}
void venus_tcp_client_free(venus_tcp_client_t *client) { memset(client,0,sizeof *client); }
venus_ring_status_t venus_tcp_client_retire(venus_tcp_client_t *client,const _Atomic uint32_t *cancel)
{ assert(client->session==123 && cancel);retirement_calls++;return retire_status; }
venus_ring_status_t venus_tcp_client_exchange_until_cancel(venus_tcp_client_t *client,const venus_request_t *request,const void *input,size_t length,venus_request_t *response,void *output,size_t capacity,uint64_t deadline_ms,const _Atomic uint32_t *cancel)
{ assert(deadline_ms==2000);assert(client->session==123 && request && !input && !length && response && !output && !capacity && cancel);return RingAgain; }
static unsigned descriptors(void)
{
    DIR *directory=opendir("/proc/self/fd");assert(directory);unsigned count=0;while(readdir(directory))count++;assert(!closedir(directory));return count;
}
static void select_fault(const char *name,unsigned call) { fault=name;fault_call=call;fault_seen=0; }
static void reset(void)
{
    select_fault(NULL,1);bind_status=unbind_status=retire_status=init_status=RingOk;
    assert(venus_tcp_bootstrap_stop()==RingOk);assert(!allocations && !bootstrap.module && !pending_native() && !bound_callback && !bound_context);
}
static void write_config(const char *bytes,size_t length)
{ int fd=open(native_path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(write(fd,bytes,length)==(ssize_t)length);assert(!close(fd)); }
int main(void)
{
    char directory[]="/tmp/waddle_bootstrap_sdk_XXXXXX";assert(mkdtemp(directory));assert(snprintf(native_path,sizeof native_path,"%s/config",directory)>0);
    const char Config[]="{\"version\":1,\"host\":\"127.0.0.1\",\"port\":12345,\"token\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"exchange_timeout_ms\":1000,\"icd_path\":\"C:\\\\fixture.dll\"}";
    write_config(Config,sizeof Config-1);unsigned baseline=descriptors();
    assert(venus_tcp_bootstrap_start(NULL,0)==RingInvalid);assert(venus_tcp_bootstrap_abandon(0)==RingInvalid);assert(venus_tcp_bootstrap_stop()==RingOk);
    const char *Faults[]={"create","type","information","links","directory","reparse","high_size","size_mismatch","read","security","owner_mismatch","sid_mismatch","empty_acl","ace_type","ace_inherited","ace_short","sid_extent","owner_null","dacl_null","token_open","token_info","user_null","token_short","token_long","sid_invalid","ace_read","ace_null","load","full_zero","full_long","module_zero","module_long","module_mismatch","export"};
    for(size_t index=0;index<sizeof Faults/sizeof *Faults;index++) {
        select_fault(Faults[index],1);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)!=RingOk);assert(!venus_tcp_bootstrap_session());reset();assert(descriptors()==baseline);
    }
    for(unsigned call=2;call<=3;call++){select_fault("export",call);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingInvalid);reset();}
    for(unsigned call=2;call<=3;call++){select_fault("sid_invalid",call);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingInvalid);reset();}
    select_fault("full_zero",2);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingInvalid);reset();
    select_fault("full_long",2);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingInvalid);reset();
    write_config("",0);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingInvalid);
    char oversized[VenusTcpMaxConfigBytes+1]={0};write_config(oversized,sizeof oversized);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingInvalid);
    write_config("x",1);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)!=RingOk);write_config(Config,sizeof Config-1);
    const char *ReleaseFaults[]={"close_token","local_free","close_file"};
    for(size_t index=0;index<3;index++){
        select_fault(ReleaseFaults[index],1);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingClosed);assert(pending_native() && allocations);assert(!venus_tcp_bootstrap_session());assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingAgain);
        select_fault(ReleaseFaults[index],1);assert(venus_tcp_bootstrap_stop()==RingClosed);assert(pending_native() && allocations);reset();assert(descriptors()==baseline);
    }
    init_status=RingClosed;assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingClosed);reset();
    bind_status=RingAgain;assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingAgain);assert(!venus_tcp_bootstrap_session());reset();
    bind_status=RingAgain;retire_status=RingCorrupt;assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingCorrupt);assert(venus_tcp_bootstrap_session()==123 && allocations==1 && !bootstrap.bound);assert(venus_tcp_bootstrap_abandon(124)==RingInvalid);assert(venus_tcp_bootstrap_abandon(123)==RingOk);reset();
    assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingOk);assert(venus_tcp_bootstrap_session()==123 && bound_callback && bound_context);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingAgain);
    venus_request_t request={.kind=RequestPoll},response={0};assert(callback(NULL,&request,NULL,0,&response,NULL,0,2000)==RingClosed);assert(bound_callback(bound_context,&request,NULL,0,&response,NULL,0,2000)==RingAgain);
    unbind_status=RingAgain;assert(venus_tcp_bootstrap_stop()==RingAgain);assert(bootstrap.bound && venus_tcp_bootstrap_session()==123);unbind_status=RingOk;
    retire_status=RingCorrupt;assert(venus_tcp_bootstrap_stop()==RingCorrupt);assert(!bootstrap.bound && bootstrap.module && venus_tcp_bootstrap_session()==123);assert(callback(&bootstrap,&request,NULL,0,&response,NULL,0,2000)==RingClosed);
    assert(venus_tcp_bootstrap_abandon(0)==RingInvalid);assert(venus_tcp_bootstrap_abandon(124)==RingInvalid);assert(venus_tcp_bootstrap_abandon(123)==RingOk);reset();
    assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingOk);assert(venus_tcp_bootstrap_abandon(123)==RingOk);assert(abandoned==1);reset();
    assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingOk);select_fault("free_module",1);unsigned old=retirement_calls;assert(venus_tcp_bootstrap_stop()==RingClosed);assert(allocations==1 && bootstrap.retired && venus_tcp_bootstrap_session()==123);assert(retirement_calls==old+1);reset();assert(retirement_calls==old+1);
    select_fault("export",1);fail_module_release_once=1;assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingClosed);assert(!venus_tcp_bootstrap_session() && bootstrap.module && allocations==1);assert(venus_tcp_bootstrap_start(ConfigPath,sizeof ConfigPath)==RingAgain);reset();
    assert(module_releases);assert(!unlink(native_path));assert(!rmdir(directory));assert(descriptors()==baseline);
    puts("TCP bootstrap SDK ownership: all actual descriptors/heap owners released; synthetic client/ICD admission only");return 0;
}
