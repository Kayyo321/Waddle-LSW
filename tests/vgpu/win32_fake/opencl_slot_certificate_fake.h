/** @file opencl_slot_certificate_fake.h
 * @brief Test-only API surface; fixed synthetic memory, no native execution.
 */
#ifndef WaddleOpenclSlotCertificateFakeH
#define WaddleOpenclSlotCertificateFakeH
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef size_t SIZE_T;
typedef void *HANDLE;
typedef void *HMODULE;
typedef int BOOL;
#define ERROR_SUCCESS 0
#define ERROR_NOT_ENOUGH_MEMORY 8
#define TLS_OUT_OF_INDEXES UINT32_MAX
static void SetLastError(DWORD);
static DWORD GetLastError(void);
static DWORD TlsAlloc(void);
static BOOL TlsFree(DWORD);
static void *NtCurrentTeb(void);
static HANDLE GetCurrentProcess(void);
static BOOL ReadProcessMemory(HANDLE,const void *,void *,SIZE_T,SIZE_T *);
static DWORD GetEnvironmentVariableA(const char *,char *,DWORD);
static UINT GetSystemDirectoryW(wchar_t *,UINT);
static HMODULE GetModuleHandleW(const wchar_t *);
static DWORD GetModuleFileNameW(HMODULE,wchar_t *,DWORD);
static int _wcsicmp(const wchar_t *,const wchar_t *);
#endif
