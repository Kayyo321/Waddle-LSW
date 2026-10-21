/** @file native_csr_fake.h @brief Minimal Win32 boundary for CSR heap controls. */
#ifndef WADDLE_NATIVE_CSR_FAKE_H
#define WADDLE_NATIVE_CSR_FAKE_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef uint16_t WORD;
typedef wchar_t WCHAR;
typedef size_t SIZE_T;
typedef void *HANDLE;
typedef void *HMODULE;
typedef struct process_heap_entry_t {
    void *lpData;
    DWORD cbData;
    unsigned char cbOverhead;
    unsigned char iRegionIndex;
    WORD wFlags;
} PROCESS_HEAP_ENTRY;
#define MAX_PATH 260
#define ERROR_SUCCESS 0u
#define ERROR_NO_MORE_ITEMS 259u
#define ERROR_ENVVAR_NOT_FOUND 203u
#define ERROR_ACCESS_DENIED 5u
#define PROCESS_HEAP_ENTRY_BUSY 0x0004u
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
DWORD GetEnvironmentVariableA(const char *name, char *buffer, DWORD capacity);
void SetLastError(DWORD error);
DWORD GetLastError(void);
UINT GetSystemDirectoryW(WCHAR *buffer, UINT capacity);
HMODULE GetModuleHandleW(const WCHAR *name);
DWORD GetModuleFileNameW(HMODULE module, WCHAR *buffer, DWORD capacity);
int _wcsicmp(const WCHAR *left, const WCHAR *right);
DWORD GetProcessHeaps(DWORD capacity, HANDLE *heaps);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
int ReadProcessMemory(HANDLE process, const void *address, void *buffer, SIZE_T bytes, SIZE_T *read);
int HeapLock(HANDLE heap);
int HeapUnlock(HANDLE heap);
int HeapWalk(HANDLE heap, PROCESS_HEAP_ENTRY *entry);
int audit_stage(const WCHAR *directory, const char *stage);
#endif
