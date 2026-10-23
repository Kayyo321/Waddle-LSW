/** @file icd_dll.c @brief Load actual Windows ICD DLL and verify exact exports/lifetimes. */
#include "waddle/venus_icd.h"
#include <assert.h>
#include <windows.h>
/** @brief Borrowed static loader negotiation function signature, no storage. */
typedef VkResult (*negotiate_t)(uint32_t *);
/** @brief Borrowed static instance procedure lookup signature, no storage. */
typedef PFN_vkVoidFunction (*lookup_t)(VkInstance, const char *);
/** @brief Empty-binding teardown signature, no ownership transfer. */
typedef venus_ring_status_t (*unbind_t)(void);
int main(void) {
    HMODULE library = LoadLibraryW(L"build/waddle_vulkan_experimental.dll");
    assert(library);
    const char *const Symbols[] = {"venus_icd_bind",
                                   "venus_icd_unbind",
                                   "venus_icd_abandon",
                                   "venus_icd_negotiate_loader",
                                   "venus_icd_get_instance_proc_addr",
                                   "venus_icd_get_physical_proc_addr",
                                   "vk_icdNegotiateLoaderICDInterfaceVersion",
                                   "vk_icdGetInstanceProcAddr",
                                   "vk_icdGetPhysicalDeviceProcAddr"};
    for (unsigned index = 0; index < sizeof(Symbols) / sizeof(*Symbols); index++)
        assert(GetProcAddress(library, Symbols[index]));
    assert(!GetProcAddress(library, "venus_command_start"));
    assert(!GetProcAddress(library, "venus_objects_init"));
    assert(!GetProcAddress(library, "DriverEntry"));
    negotiate_t negotiate =
        (negotiate_t)GetProcAddress(library, "vk_icdNegotiateLoaderICDInterfaceVersion");
    lookup_t lookup = (lookup_t)GetProcAddress(library, "vk_icdGetInstanceProcAddr");
    unbind_t unbind = (unbind_t)GetProcAddress(library, "venus_icd_unbind");
    uint32_t version = 9;
    assert(negotiate(&version) == VK_SUCCESS && version == 5);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)lookup(NULL, "vkCreateInstance");
    assert(create && !lookup(NULL, "vkDestroyInstance"));
    VkInstance instance = NULL;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(create(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED && !instance);
    assert(unbind() == RingOk);
    assert(FreeLibrary(library));
    return 0;
}
