/** @file opencl_reload_fake.h @brief Test-only Win32 declarations, no vendor code.
 * Functions are implemented by the host fixture. Kernel handles are represented
 * by owned pthread/event test objects; no Windows/native qualification occurs.
 */
#ifndef WaddleOpenclReloadFakeH
#define WaddleOpenclReloadFakeH
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HMODULE;
typedef struct { DWORD dwLowDateTime,dwHighDateTime; } FILETIME;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define INFINITE UINT32_MAX
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define TLS_OUT_OF_INDEXES UINT32_MAX
#define FLS_OUT_OF_INDEXES UINT32_MAX
#define ERROR_SUCCESS 0
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_NOT_ENOUGH_MEMORY 8
static void SetLastError(DWORD);
static DWORD GetLastError(void);
static BOOL CloseHandle(HANDLE);
static DWORD GetCurrentThreadId(void);
static BOOL SetEvent(HANDLE);
static DWORD WaitForSingleObject(HANDLE,DWORD);
static ULONGLONG GetTickCount64(void);
static BOOL GetExitCodeThread(HANDLE,DWORD *);
static HANDLE CreateEventW(void *,BOOL,BOOL,const wchar_t *);
static HANDLE CreateThread(void *,size_t,DWORD (*)(void *),void *,DWORD,DWORD *);
static DWORD TlsAlloc(void);
static BOOL TlsFree(DWORD);
static DWORD FlsAlloc(void *);
static BOOL FlsFree(DWORD);
static HANDLE GetCurrentProcess(void);
static BOOL GetProcessHandleCount(HANDLE,DWORD *);
static DWORD GetModuleFileNameW(HMODULE,wchar_t *,DWORD);
static int _wcsicmp(const wchar_t *,const wchar_t *);
static BOOL GetProcessTimes(HANDLE,FILETIME *,FILETIME *,FILETIME *,FILETIME *);
static DWORD GetCurrentProcessId(void);
static HMODULE LoadLibraryW(const wchar_t *);
static BOOL FreeLibrary(HMODULE);
#endif
