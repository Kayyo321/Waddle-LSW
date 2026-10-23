#ifndef WaddleLoaderEnvironmentFakeWindowsH
#define WaddleLoaderEnvironmentFakeWindowsH

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#define WINAPI
typedef uint32_t DWORD;
typedef int BOOL;
typedef wchar_t WCHAR;
typedef const WCHAR *LPCWSTR;
typedef WCHAR *LPWSTR;

#define TRUE 1
#define FALSE 0
#define ERROR_SUCCESS 0u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ENVVAR_NOT_FOUND 203u

static DWORD fake_last_error;

static inline DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR value, DWORD capacity)
{
    (void)name;
    if (value && capacity) value[0] = L'\0';
    fake_last_error = ERROR_ENVVAR_NOT_FOUND;
    return 0;
}

static inline BOOL WINAPI SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value)
{
    (void)name;
    (void)value;
    fake_last_error = ERROR_ACCESS_DENIED;
    return FALSE;
}

static inline DWORD WINAPI GetLastError(void)
{
    return fake_last_error;
}

static inline void WINAPI SetLastError(DWORD value)
{
    fake_last_error = value;
}

#endif
