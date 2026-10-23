/** @file tcp_bootstrap_sdk.h @brief Test-only Windows SDK ABI boundary, never a platform replacement.
 * Native Windows symbols retain SDK spelling. All declarations borrow arguments;
 * acquisition/release ownership and faults are implemented by the matching fixture.
 * The fixture is single-threaded and uses actual Linux descriptors and heap owners.
 */
#ifndef WaddleTcpBootstrapSdkTests
#define WaddleTcpBootstrapSdkTests
#include <stdint.h>
#include <stddef.h>
#include <strings.h>
typedef uint32_t DWORD;
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *PSID;
typedef void *PSECURITY_DESCRIPTOR;
typedef void (*FARPROC)(void);
typedef struct { PSID Sid; DWORD Attributes; } SID_AND_ATTRIBUTES;
typedef struct { SID_AND_ATTRIBUTES User; } TOKEN_USER;
typedef struct { uint8_t AceType,AceFlags; uint16_t AceSize; } ACE_HEADER;
typedef struct { ACE_HEADER Header; DWORD Mask; DWORD SidStart; uint8_t rest[12]; } ACCESS_ALLOWED_ACE;
typedef struct { uint8_t revision,pad; uint16_t bytes,AceCount,pad2; } ACL,*PACL;
typedef struct { DWORD dwFileAttributes,unused[7],nFileSizeHigh,nFileSizeLow,nNumberOfLinks; } BY_HANDLE_FILE_INFORMATION;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_SUCCESS 0
#define SE_FILE_OBJECT 1
#define OWNER_SECURITY_INFORMATION 1
#define DACL_SECURITY_INFORMATION 4
#define TOKEN_QUERY 8
#define TokenUser 1
#define ACCESS_ALLOWED_ACE_TYPE 0
#define INHERITED_ACE 16
#define GENERIC_READ 0x80000000u
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define FILE_TYPE_DISK 1
#define FILE_ATTRIBUTE_DIRECTORY 16
#define FILE_ATTRIBUTE_REPARSE_POINT 1024
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 256
#define LOAD_LIBRARY_SEARCH_SYSTEM32 2048
#define _stricmp strcasecmp
HANDLE CreateFileA(const char *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
int CloseHandle(HANDLE);
DWORD GetFileType(HANDLE);
int GetFileInformationByHandle(HANDLE,BY_HANDLE_FILE_INFORMATION *);
int ReadFile(HANDLE,void *,DWORD,DWORD *,void *);
DWORD GetSecurityInfo(HANDLE,int,DWORD,PSID *,PSID *,PACL *,PACL *,PSECURITY_DESCRIPTOR *);
int OpenProcessToken(HANDLE,DWORD,HANDLE *);
HANDLE GetCurrentProcess(void);
int GetTokenInformation(HANDLE,int,void *,DWORD,DWORD *);
int IsValidSid(PSID);
int EqualSid(PSID,PSID);
int GetAce(PACL,DWORD,void **);
DWORD GetLengthSid(PSID);
void *LocalFree(void *);
HMODULE LoadLibraryExA(const char *,HANDLE,DWORD);
int FreeLibrary(HMODULE);
FARPROC GetProcAddress(HMODULE,const char *);
DWORD GetFullPathNameA(const char *,DWORD,char *,char **);
DWORD GetModuleFileNameA(HMODULE,char *,DWORD);
#endif
