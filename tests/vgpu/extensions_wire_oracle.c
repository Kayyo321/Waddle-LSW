/** @file extensions_wire_oracle.c @brief Pinned command14 guest request oracle. */
#include "vn_cs.h"
/** @brief Isolate the vendor compile-only submit stub; no runtime owner. */
#define vn_ring_submit_command venus_extensions_oracle_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_device.h"
#pragma GCC diagnostic pop
/** @brief Encode actual count/fill request through the pinned generator.
 * @param[in] physical_id Nonzero translated identity; never dereferenced.
 * @param[in] capacity Zero for count query,1..1024 for fill.
 * @param[out] bytes Nonnull exclusive output[44], borrowed synchronously.
 * @return Initialized44-byte prefix; no allocation or retained pointers.
 * @note Local native partial records own stack storage; disjoint calls thread safe.
 */
size_t venus_extensions_test_request(uint64_t physical_id, uint32_t capacity,
                                    unsigned char *bytes) {
    assert(physical_id && capacity<=1024 && bytes);
    VkExtensionProperties properties[1024];
    struct instance_encoder_t encoder={.bytes=bytes,.capacity=44};
    vn_encode_vkEnumerateDeviceExtensionProperties(&encoder,1,
        (VkPhysicalDevice)(uintptr_t)physical_id,NULL,&capacity,capacity?properties:NULL);
    return encoder.used;
}
#ifdef VgpuExtensionsOracleSelfTest
#include <stdio.h>
#include <stdlib.h>
/** @brief Independent renderer serializer; definition documents full ownership. */
extern size_t venus_extensions_test_reply(int32_t result,uint32_t count,
    const VkExtensionProperties *properties,unsigned char *bytes,size_t capacity);
/** @brief Verify literal boundaries and pinned native request/reply fields.
 * @return Zero on exact count/fill/header/entry/sentinel assertions.
 * @note Sole main thread; two heap allocations have unconditional matching frees.
 */
int main(void) {
    const size_t max_bytes=28+1024*268;
    unsigned char request[45];
    unsigned char *reply=malloc(max_bytes+1);
    VkExtensionProperties *properties=calloc(1024,sizeof *properties);
    if (!reply || !properties) {
        free(properties); properties=NULL;
        free(reply); reply=NULL;
        return 1;
    }
    for (uint32_t index=0;index<1024;index++) {
        int length=snprintf(properties[index].extensionName,sizeof properties[index].extensionName,
                            "VK_test_%04u",index);
        assert(length==12);
        properties[index].specVersion=UINT32_MAX-index;
    }
    for (uint32_t count=0;count<=1024;count=count?1024:1) {
        memset(request,0xa5,sizeof request);
        assert(venus_extensions_test_request(7,count,request)==44);
        assert(request[0]==14 && request[4]==1 && request[8]==7 && request[24]==1);
        uint32_t initial=0; uint64_t array_count=0;
        memcpy(&initial,request+32,4); memcpy(&array_count,request+36,8);
        assert(initial==count && array_count==count && request[44]==0xa5);
        memset(reply,0xa5,max_bytes+1);
        size_t used=venus_extensions_test_reply(0,count,count?properties:NULL,reply,max_bytes);
        assert(used==28+(size_t)count*268 && reply[used]==0xa5);
        assert(reply[0]==14 && reply[8]==1);
        uint32_t returned=0; memcpy(&returned,reply+16,4); assert(returned==count);
        if (count) {
            uint64_t name_bytes=0; memcpy(&name_bytes,reply+28,8); assert(name_bytes==256);
            assert(!memcmp(reply+36,"VK_test_0000",12));
            uint32_t version=0; memcpy(&version,reply+292,4); assert(version==UINT32_MAX);
        }
        if (count==1024) break;
    }
    free(properties); properties=NULL;
    free(reply); reply=NULL;
    return 0;
}
#endif
