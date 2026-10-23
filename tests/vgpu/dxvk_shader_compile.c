/** @file dxvk_shader_compile.c @brief Reproduce the acceptance DXBC with the system SDK.
 * This one-shot tool owns files, compiler module and blobs through cleanup.
 * Known SDK per-thread metadata from repeated DLL reloads is documented in
 * the acceptance evidence; this tool does not claim SDK heap qualification.
 */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
/** @brief Borrowed SDK export; returned blobs transfer one caller-owned reference. */
typedef HRESULT (WINAPI *compile_shader_t)(const void *,SIZE_T,const char *,const D3D_SHADER_MACRO *,ID3DInclude *,const char *,const char *,UINT,UINT,ID3DBlob **,ID3DBlob **);
/** @brief Compile the unchanged acceptance HLSL once and publish exact DXBC.
 * @param[in] argc Must equal three.
 * @param[in] argv Borrowed nonnull paths: source HLSL, fresh output DXBC.
 * @return Zero on complete output and balanced project ownership; one otherwise.
 * @note Sole process thread; source bounded to 4096 bytes, DXBC to 65536.
 * Caller records signed compiler/source/output SHA provenance separately.
 */
int wmain(int argc,wchar_t **argv) {
    if(argc!=3)return 1;
    char source[4096];HANDLE input=INVALID_HANDLE_VALUE,output=INVALID_HANDLE_VALUE;
    HMODULE compiler=NULL;ID3DBlob *code=NULL,*errors=NULL;int result=1;
    input=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    LARGE_INTEGER length;DWORD source_bytes=0;
    if(input==INVALID_HANDLE_VALUE || !GetFileSizeEx(input,&length) || length.QuadPart<=0 || length.QuadPart>(LONGLONG)sizeof source)goto cleanup;
    if(!ReadFile(input,source,(DWORD)length.QuadPart,&source_bytes,NULL) || source_bytes!=(DWORD)length.QuadPart)goto cleanup;
    compiler=LoadLibraryExW(L"d3dcompiler_47.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!compiler)goto cleanup;
    FARPROC procedure=GetProcAddress(compiler,"D3DCompile");compile_shader_t compile=NULL;
    _Static_assert(sizeof compile==sizeof procedure,"Windows function representation");memcpy(&compile,&procedure,sizeof compile);if(!compile)goto cleanup;
    HRESULT status=compile(source,source_bytes,"acceptance_compute",NULL,NULL,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
    if(FAILED(status) || !code)goto cleanup;
    SIZE_T code_bytes=ID3D10Blob_GetBufferSize(code);const void *data=ID3D10Blob_GetBufferPointer(code);
    if(!data || code_bytes<4 || code_bytes>65536 || memcmp(data,"DXBC",4))goto cleanup;
    output=CreateFileW(argv[2],GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);if(output==INVALID_HANDLE_VALUE)goto cleanup;
    DWORD written=0;if(!WriteFile(output,data,(DWORD)code_bytes,&written,NULL) || written!=(DWORD)code_bytes || !FlushFileBuffers(output))goto cleanup;
    printf("Acceptance DXBC compiled: target=cs_5_0 flags=0x%08x source_bytes=%lu code_bytes=%llu\n",D3DCOMPILE_ENABLE_STRICTNESS,(unsigned long)source_bytes,(unsigned long long)code_bytes);result=0;
cleanup:
    if(errors)ID3D10Blob_Release(errors);
    if(code)ID3D10Blob_Release(code);
    if(output!=INVALID_HANDLE_VALUE && !CloseHandle(output))result=1;
    if(input!=INVALID_HANDLE_VALUE && !CloseHandle(input))result=1;
    if(compiler && !FreeLibrary(compiler))result=1;
    return result;
}
