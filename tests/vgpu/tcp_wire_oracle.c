/** @file tcp_wire_oracle.c @brief Independent literal TCP hello byte oracle. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/** @brief Encode fixed fixture packets without production codecs or native casts.
 * @param[in] kind One client, two server, three acknowledgment, four profile.
 * @param[out] output Nonnull owned bytes[capacity], unchanged on invalid extent.
 * @param[in] capacity Exact128/224/32/160. @return One success, zero invalid.
 * No allocation/retention; thread-safe on disjoint outputs. Fixture tokens are public synthetic data.
 */
int venus_tcp_wire_oracle(unsigned kind, uint8_t *output, size_t capacity)
{
    static const uint8_t header[16] = {'W','D','T','C','P','0','0','1',1,0,0,0,0,0,0,0};
    static const uint8_t identity[8] = {0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11};
    size_t length = kind == 1 ? 128 : kind == 2 ? 224 : kind == 3 ? 32 : kind == 4 ? 160 : 0;
    if (!output || !length || capacity != length) return 0;
    memset(output, 0, length);
    if (kind == 4) {
        output[0]=1; output[4]=0x33; output[5]=0x41; output[6]=0x40;
        output[8]=1; output[12]=3; output[16]=1; output[20]=1;
        output[68]=3; output[152]=1;
        return 1;
    }
    memcpy(output, header, sizeof header);
    output[12]=(uint8_t)length;
    if (kind == 1) {
        for (unsigned index=0; index<32; ++index) output[16+index]=(uint8_t)(index*7+3);
        for (unsigned index=0; index<16; ++index) output[48+index]=(uint8_t)(index*11+5);
    } else {
        memcpy(output+16, identity, sizeof identity);
        if (kind == 2) {
            for (unsigned index=0; index<16; ++index) output[24+index]=(uint8_t)(index*11+5);
            for (unsigned index=0; index<160; ++index) output[64+index]=(uint8_t)(index*13+9);
        }
    }
    return 1;
}
