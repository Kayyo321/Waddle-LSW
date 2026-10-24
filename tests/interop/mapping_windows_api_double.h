#ifndef WaddleMappingWindowsApiDoubleH
#define WaddleMappingWindowsApiDoubleH
/** @file mapping_windows_api_double.h
 * @brief Handwritten compile-time SDK double; no Windows/vendor source is copied.
 * SDK typedef/member/macro spellings intentionally match the compatibility
 * boundary. Only the Linux fixture uses these declarations; native fixtures use
 * the real SDK. No resource or memory is acquired by this header.
 */
#include <stddef.h>
#include <stdint.h>

typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint64_t UINT64;
typedef uint32_t DWORD;
typedef uint16_t WCHAR;
typedef uint8_t BOOLEAN;
typedef void *PVOID;
typedef void *LPVOID;
typedef void *HANDLE;
typedef void *HDEVINFO;
typedef int BOOL;
typedef struct mapping_windows_guid_t { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } GUID;

typedef struct mapping_windows_device_interface_data_t {
    DWORD cbSize;
    GUID InterfaceClassGuid;
    DWORD Flags;
    uintptr_t Reserved;
} SP_DEVICE_INTERFACE_DATA;

typedef struct mapping_windows_detail_data_t {
    DWORD cbSize;
    WCHAR DevicePath[1];
} SP_DEVICE_INTERFACE_DETAIL_DATA_W;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_SUCCESS 0u
#define ERROR_INVALID_FUNCTION 1u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_DATA 13u
#define ERROR_GEN_FAILURE 31u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_NO_MORE_ITEMS 259u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define OPEN_EXISTING 3u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define DIGCF_PRESENT 0x2u
#define DIGCF_DEVICEINTERFACE 0x10u
#define FILE_DEVICE_UNKNOWN 0x22u
#define METHOD_BUFFERED 0u
#define FILE_ANY_ACCESS 0u
#define CTL_CODE(device_type, function, method, access) \
    (((device_type) << 16) | ((access) << 14) | ((function) << 2) | (method))

#endif
