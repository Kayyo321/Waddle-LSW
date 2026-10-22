/** @file opencl_os_preinit_fake.h @brief Test-only Win32 declarations for OS pre-initialization.
 * Functions are implemented by tests/vgpu/opencl_os_preinit_test.c. No Windows module,
 * Winsock, DXGI or native execution occurs.
 */
#ifndef WaddleOpenclOsPreinitFakeH
#define WaddleOpenclOsPreinitFakeH
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef struct fake_module_t *HMODULE;
typedef void (*FARPROC)(void);
typedef struct { uint32_t Data1; uint16_t Data2, Data3; unsigned char Data4[8]; } GUID;
#define WINAPI
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800u
static HMODULE LoadLibraryExW(const wchar_t *, HANDLE, DWORD);
static HMODULE LoadLibraryW(const wchar_t *);
static HMODULE GetModuleHandleW(const wchar_t *);
static FARPROC GetProcAddress(HMODULE, const char *);
static BOOL FreeLibrary(HMODULE);
static unsigned GetSystemDirectoryW(wchar_t *, unsigned);
static DWORD GetModuleFileNameW(HMODULE, wchar_t *, DWORD);
static int _wcsicmp(const wchar_t *, const wchar_t *);
#endif
