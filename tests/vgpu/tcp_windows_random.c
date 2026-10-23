/** @file tcp_windows_random.c @brief Native CNG failure cleanup with real provider handles. */
#include "waddle/venus_tcp.h"
#include <assert.h>
#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
static int fail_open, fail_generate, fail_close, opened, generated, closed;
static NTSTATUS WINAPI open_rng(BCRYPT_ALG_HANDLE *algorithm,LPCWSTR name,LPCWSTR provider,ULONG flags)
{
    if (fail_open) return (NTSTATUS)0xc0000001u;
    NTSTATUS status=BCryptOpenAlgorithmProvider(algorithm,name,provider,flags);
    if (!status) ++opened;
    return status;
}
static NTSTATUS WINAPI generate_rng(BCRYPT_ALG_HANDLE algorithm,PUCHAR bytes,ULONG length,ULONG flags)
{
    ++generated;
    if (fail_generate) return (NTSTATUS)0xc0000001u;
    return BCryptGenRandom(algorithm,bytes,length,flags);
}
static NTSTATUS WINAPI close_rng(BCRYPT_ALG_HANDLE algorithm,ULONG flags)
{
    /* Even simulated close-status failure retires the real provider first;
     * this proves reported-error handling without leaking a test-owned handle. */
    NTSTATUS status=BCryptCloseAlgorithmProvider(algorithm,flags);
    assert(!status); ++closed;
    return fail_close ? (NTSTATUS)0xc0000001u : status;
}
#define BCryptOpenAlgorithmProvider open_rng
#define BCryptGenRandom generate_rng
#define BCryptCloseAlgorithmProvider close_rng
/* Private test-only source inclusion substitutes exactly the three CNG calls;
 * sockets, ownership and native provider operations remain actual production code. */
#include "../../src/vgpu/venus_tcp_socket.c"
static DWORD handle_count(void)
{
    DWORD count=0; assert(GetProcessHandleCount(GetCurrentProcess(),&count)); return count;
}
/** @brief Execute native success/error CNG ownership checks. @return Zero success; no retained provider. */
int main(void)
{
    uint8_t bytes[32];
    assert(venus_tcp_random(bytes,sizeof bytes)==RingOk);
    DWORD baseline=handle_count();
    for (unsigned iteration=0;iteration<32;++iteration) for (unsigned mode=0;mode<4;++mode) {
        fail_open=mode==1; fail_generate=mode==2; fail_close=mode==3;
        opened=generated=closed=0;
        memset(bytes,0xa5,sizeof bytes);
        assert(venus_tcp_random(bytes,sizeof bytes)==(mode ? RingClosed : RingOk));
        assert(opened==(mode!=1) && generated==(mode!=1) && closed==(mode!=1));
        if (mode==1 || mode==2) for (size_t index=0;index<sizeof bytes;++index) assert(bytes[index]==0xa5);
        venus_tcp_scrub(bytes,sizeof bytes);
        for (size_t index=0;index<sizeof bytes;++index) assert(!bytes[index]);
        assert(handle_count()==baseline);
    }
    opened=generated=closed=0; memset(bytes,0xa5,sizeof bytes);
    assert(venus_tcp_random(NULL,1)==RingInvalid);
    assert(venus_tcp_random(bytes,0)==RingInvalid);
    assert(venus_tcp_random(bytes,33)==RingInvalid);
    assert(!opened && !generated && !closed);
    for (size_t index=0;index<sizeof bytes;++index) assert(bytes[index]==0xa5);
    assert(handle_count()==baseline);
    puts("Native CNG real provider success/error ownership: PASS (128 cycles)");
    return 0;
}
