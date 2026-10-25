/** @file pe_header.c @brief Independent bounded PE C ABI consumer. */
#include "pe_header.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void make_image(uint8_t bytes[InteropPePrefixBytes], uint16_t magic, uint16_t subsystem) {
    memset(bytes, 0, InteropPePrefixBytes);
    bytes[0] = 'M'; bytes[1] = 'Z'; bytes[0x3c] = 0x80;
    bytes[0x80] = 'P'; bytes[0x81] = 'E';
    bytes[0x94] = 0xf0;
    bytes[0x98] = (uint8_t)magic; bytes[0x99] = (uint8_t)(magic >> 8);
    bytes[0xdc] = (uint8_t)subsystem; bytes[0xdd] = (uint8_t)(subsystem >> 8);
}

/** @brief Exercise the exact public ABI without executing any input. */
int main(void) {
    _Alignas(uint32_t) uint8_t bytes[InteropPePrefixBytes];
    uint32_t kind = 77;
    for (unsigned format = 0; format < 2; format++) {
        make_image(bytes, format == 0 ? 0x10b : 0x20b, 3);
        assert(interop_pe_classify_prefix(bytes, sizeof(bytes), &kind) == InteropPeSuccess);
        assert(kind == InteropPeConsole);
        for (size_t length = 0; length < 0xde; length++) {
            assert(interop_pe_classify_prefix(bytes, length, &kind) == InteropPeSuccess);
            assert(kind == InteropPeGui);
        }
        assert(interop_pe_classify_prefix(bytes, 0xde, &kind) == InteropPeSuccess);
        assert(kind == InteropPeConsole);
        bytes[0xdc] = 2;
        assert(interop_pe_classify_prefix(bytes, sizeof(bytes), &kind) == InteropPeSuccess);
        assert(kind == InteropPeGui);
        bytes[0xdc] = 0xff;
        assert(interop_pe_classify_prefix(bytes, sizeof(bytes), &kind) == InteropPeSuccess);
        assert(kind == InteropPeGui);
    }
    kind = 77;
    assert(interop_pe_classify_prefix(NULL, 1, &kind) == InteropPeInvalidArgument);
    assert(interop_pe_classify_prefix(bytes, sizeof(bytes) + 1, &kind) == InteropPeInvalidArgument);
    assert(interop_pe_classify_prefix(bytes, sizeof(bytes), NULL) == InteropPeInvalidArgument);
    assert(interop_pe_classify_prefix(bytes, sizeof(bytes), (uint32_t *)(void *)bytes) == InteropPeInvalidArgument);
    assert(interop_pe_classify_prefix((const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1), 8, &kind) == InteropPeInvalidArgument);
    assert(interop_pe_classify_prefix(bytes, sizeof(bytes), (uint32_t *)(uintptr_t)(UINTPTR_MAX - 3)) == InteropPeInvalidArgument);
    assert(kind == 77);
    assert(interop_pe_classify_prefix(NULL, 0, &kind) == InteropPeSuccess && kind == InteropPeGui);
    puts("PE C ABI: bounded GUI/console classification passed");
    return 0;
}
