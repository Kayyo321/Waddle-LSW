/** @file icd.c @brief Native experimental ICD dispatch/lifecycle/loss fixture. */
#include "waddle/venus_icd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef VgpuIcdLoader
#include <dlfcn.h>
#include <stdlib.h>
#endif
/** @brief Test-only immutable renderer encoder oracle, borrows output for the call.
 * @param[in] kind Pinned query3/6/8.
 * @param[out] bytes Nonnull output[capacity], exclusive to test.
 * @param[in] capacity Actual accessible extent, at least4096.
 * @return Encoded byte extent, zero invalid local input.
 * @note Allocation-free, synchronous; defined in independent values fixture.
 */
size_t venus_values_test_encode(uint32_t kind, void *bytes, size_t capacity);
/** @brief External Vulkan loader ABI alias, no storage or ownership. */
extern VkResult
negotiate_external(uint32_t *version) __asm__("vk_icdNegotiateLoaderICDInterfaceVersion");
/** @brief External Vulkan loader ABI alias; pointers are static/borrowed. */
extern PFN_vkVoidFunction lookup_external(VkInstance instance,
                                          const char *name) __asm__("vk_icdGetInstanceProcAddr");
/** @brief Test backend owns only its private reply bytes; no native Vulkan implementation. */
typedef struct fixture_t {
    unsigned char reply[4096];
    uint32_t submissions;
    uint32_t enumerations;
    uint32_t command;
    uint32_t fail_command;
    uint32_t corrupt_command;
    uint32_t poll_again;
    uint32_t reply_again;
    int32_t create_result;
    int32_t enumerate_result;
    int32_t transport_failure;
    uint32_t device_count;
    unsigned fail_fill;
} fixture_t;
static uint32_t read_u32(const void *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}
static uint64_t read_u64(const void *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}
static void put_u32(void *bytes, uint32_t value) { memcpy(bytes, &value, sizeof(value)); }
static void put_u64(void *bytes, uint64_t value) { memcpy(bytes, &value, sizeof(value)); }
static venus_ring_status_t exchange(void *context, const venus_request_t *request,
                                    const void *input, size_t length, venus_request_t *response,
                                    void *output, size_t capacity) {
    fixture_t *fixture = context;
    *response = (venus_request_t){.kind = request->kind, .direction = 1};
    if (request->kind == RequestSubmit) {
        assert(input && length >= 44 && !output && !capacity);
        const unsigned char *bytes = (const unsigned char *)input + 36;
        fixture->command = read_u32(bytes);
        if (fixture->command == fixture->fail_command ||
            (fixture->command == 2 && fixture->fail_fill && read_u32(bytes + 24)))
            return fixture->transport_failure;
        fixture->submissions++;
        response->argument_zero = fixture->submissions;
        memset(fixture->reply, 0, sizeof(fixture->reply));
        put_u32(fixture->reply, fixture->command);
        if (fixture->command == 0) {
            put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
            put_u64(fixture->reply + 8, 1);
            put_u64(fixture->reply + 16, read_u64((const unsigned char *)input + length - 8));
        } else if (fixture->command == 2) {
            fixture->enumerations++;
            uint32_t count = read_u32(bytes + 24);
            put_u32(fixture->reply + 4, (uint32_t)fixture->enumerate_result);
            put_u64(fixture->reply + 8, 1);
            if (!fixture->enumerate_result) {
                put_u32(fixture->reply + 16, fixture->device_count);
                if (count) {
                    assert(count == 2 && length == 88);
                    put_u64(fixture->reply + 20, 2);
                    memcpy(fixture->reply + 28, bytes + 36, 16);
                }
            }
        } else if (fixture->command != 1) {
            assert(
                venus_values_test_encode(fixture->command, fixture->reply, sizeof(fixture->reply)));
            if ((fixture->command == 7 && read_u32(bytes + 24) == 0) ||
                (fixture->command == 33 && read_u32(bytes + 44) == 0))
                put_u64(fixture->reply + 16, 0);
        }
        if (fixture->command == fixture->corrupt_command) {
            if (fixture->command == 0)
                fixture->reply[16] ^= 1;
            else if (fixture->command == 2)
                fixture->reply[8] = 0;
            else if (fixture->command == 3 || fixture->command == 6 || fixture->command == 8)
                fixture->reply[4] = 0;
            else
                fixture->reply[0] ^= 1;
        }
    } else if (request->kind == RequestPoll) {
        assert(!input && !length && !output && !capacity);
        if (fixture->poll_again) {
            fixture->poll_again--;
            return RingAgain;
        }
    } else {
        assert(request->kind == RequestReply && !input && !length && output &&
               capacity == sizeof(fixture->reply));
        if (fixture->reply_again) {
            fixture->reply_again--;
            return RingAgain;
        }
        memcpy(output, fixture->reply, capacity);
        response->payload_bytes = (uint32_t)capacity;
    }
    return RingOk;
}
static fixture_t fresh(void) {
    return (fixture_t){.fail_command = UINT32_MAX,
                       .corrupt_command = UINT32_MAX,
                       .transport_failure = RingClosed,
                       .device_count = 2};
}
static PFN_vkCreateInstance create_function(void) {
    return (PFN_vkCreateInstance)lookup_external(NULL, "vkCreateInstance");
}
static VkInstance create(void) {
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance = NULL;
    assert(create_function()(&info, NULL, &instance) == VK_SUCCESS && instance);
    return instance;
}
static void destroy(VkInstance instance) {
    PFN_vkDestroyInstance function =
        (PFN_vkDestroyInstance)lookup_external(instance, "vkDestroyInstance");
    assert(function);
    function(instance, NULL);
}
static void routing(void) {
    uint32_t version = 9;
    assert(negotiate_external(&version) == VK_SUCCESS && version == 5);
    for (version = 2; version <= 5; version++) {
        uint32_t input = version;
        assert(venus_icd_negotiate_loader(&input) == VK_SUCCESS && input == version);
    }
    version = 1;
    assert(negotiate_external(&version) == VK_ERROR_INITIALIZATION_FAILED && version == 1);
    assert(negotiate_external(NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(!lookup_external(NULL, NULL));
    char long_name[256];
    memset(long_name, 'a', sizeof(long_name));
    assert(!lookup_external(NULL, long_name));
    assert(!lookup_external(NULL, "vkDestroyInstance"));
    assert(!lookup_external(NULL, "vkCreateDevice"));
    assert(!lookup_external((VkInstance)(uintptr_t)1, "vkCreateInstance"));
    PFN_vkEnumerateInstanceVersion enumerate =
        (PFN_vkEnumerateInstanceVersion)lookup_external(NULL, "vkEnumerateInstanceVersion");
    assert(enumerate && enumerate(&version) == VK_SUCCESS && version == VK_API_VERSION_1_0);
    assert(enumerate(NULL) == VK_ERROR_INITIALIZATION_FAILED);
    PFN_vkEnumerateInstanceExtensionProperties extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)lookup_external(
            NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t count = 99;
    assert(extensions(NULL, &count, NULL) == VK_SUCCESS && !count);
    assert(extensions("missing", &count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);
    assert(extensions(NULL, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
}
static void healthy(fixture_t *fixture) {
    assert(venus_icd_bind(exchange, fixture) == RingOk);
    assert(venus_icd_bind(exchange, fixture) == RingInvalid);
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        VkInstance instance = create();
        assert(venus_icd_unbind() == RingAgain);
        assert(!lookup_external(instance, "vkUnknown"));
        assert(lookup_external(instance, "vkCreateDevice"));
        assert(!venus_icd_get_physical_proc_addr(instance, "vkCreateInstance"));
        assert(venus_icd_get_physical_proc_addr(instance, "vkGetPhysicalDeviceProperties"));
        assert(!venus_icd_get_physical_proc_addr(NULL, "vkGetPhysicalDeviceProperties"));
        assert(!venus_icd_get_physical_proc_addr(instance, NULL));
        assert(!venus_icd_get_physical_proc_addr((VkInstance)(uintptr_t)1,
                                                 "vkGetPhysicalDeviceProperties"));
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 99;
        unsigned before = fixture->enumerations;
        assert(enumerate(instance, &count, NULL) == VK_SUCCESS && count == 2);
        VkPhysicalDevice devices[2] = {0};
        count = 1;
        assert(enumerate(instance, &count, devices) == VK_INCOMPLETE && count == 1);
        VkPhysicalDevice first = devices[0];
        count = 0;
        assert(enumerate(instance, &count, devices) == VK_INCOMPLETE && !count);
        count = 2;
        assert(enumerate(instance, &count, devices) == VK_SUCCESS && count == 2);
        assert(devices[0] == first && fixture->enumerations == before + 2);
        VkPhysicalDeviceProperties properties = {0};
        VkPhysicalDeviceFeatures features = {0};
        VkPhysicalDeviceMemoryProperties memory = {0};
        PFN_vkGetPhysicalDeviceProperties get_properties =
            (PFN_vkGetPhysicalDeviceProperties)lookup_external(instance,
                                                               "vkGetPhysicalDeviceProperties");
        PFN_vkGetPhysicalDeviceFeatures get_features =
            (PFN_vkGetPhysicalDeviceFeatures)lookup_external(instance,
                                                             "vkGetPhysicalDeviceFeatures");
        PFN_vkGetPhysicalDeviceMemoryProperties get_memory =
            (PFN_vkGetPhysicalDeviceMemoryProperties)lookup_external(
                instance, "vkGetPhysicalDeviceMemoryProperties");
        get_properties(devices[0], &properties);
        get_features(devices[0], &features);
        get_memory(devices[0], &memory);
        PFN_vkGetPhysicalDeviceFormatProperties format =
            (PFN_vkGetPhysicalDeviceFormatProperties)lookup_external(
                instance, "vkGetPhysicalDeviceFormatProperties");
        VkFormatProperties format_value = {0};
        format(devices[0], VK_FORMAT_R8G8B8A8_UNORM, &format_value);
        assert(format_value.optimalTilingFeatures == VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        format(devices[0], VK_FORMAT_R8G8B8A8_UNORM, NULL);
        format(NULL, VK_FORMAT_R8G8B8A8_UNORM, &format_value);
        format((VkPhysicalDevice)(uintptr_t)1, VK_FORMAT_R8G8B8A8_UNORM, &format_value);
        PFN_vkGetPhysicalDeviceImageFormatProperties image =
            (PFN_vkGetPhysicalDeviceImageFormatProperties)lookup_external(
                instance, "vkGetPhysicalDeviceImageFormatProperties");
        VkImageFormatProperties image_value = {0};
        assert(image(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                     VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                     &image_value) == VK_SUCCESS);
        assert(image_value.maxExtent.width == 4096 && image_value.maxResourceSize == 1048576);
        assert(image(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                     VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                     NULL) == VK_ERROR_INITIALIZATION_FAILED);
        PFN_vkGetPhysicalDeviceQueueFamilyProperties queues =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)lookup_external(
                instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        count = 0;
        queues(devices[0], &count, NULL);
        assert(count == 1);
        VkQueueFamilyProperties queue = {0};
        queues(devices[0], &count, &queue);
        assert(count == 1 && queue.queueCount == 1 && queue.timestampValidBits == 64);
        queues(devices[0], NULL, NULL);
        count = 0;
        queues(devices[0], &count, &queue);
        assert(count == 0);
        PFN_vkGetPhysicalDeviceSparseImageFormatProperties sparse =
            (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)lookup_external(
                instance, "vkGetPhysicalDeviceSparseImageFormatProperties");
        count = 0;
        sparse(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, NULL);
        assert(count == 1);
        VkSparseImageFormatProperties sparse_value = {0};
        sparse(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, &sparse_value);
        assert(count == 1 && sparse_value.imageGranularity.width == 64);
        sparse(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, NULL, NULL);
        PFN_vkEnumerateDeviceExtensionProperties device_extensions =
            (PFN_vkEnumerateDeviceExtensionProperties)lookup_external(
                instance, "vkEnumerateDeviceExtensionProperties");
        assert(device_extensions(devices[0], NULL, &count, NULL) == VK_SUCCESS && count == 0);
        assert(device_extensions(NULL, NULL, &count, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(device_extensions((VkPhysicalDevice)(uintptr_t)1, NULL, &count, NULL) ==
               VK_ERROR_INITIALIZATION_FAILED);
        PFN_vkCreateDevice device_create =
            (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        VkDevice device_handle = (VkDevice)(uintptr_t)1;
        assert(device_create(devices[0], NULL, NULL, &device_handle) ==
                   VK_ERROR_FEATURE_NOT_PRESENT &&
               !device_handle);
        assert(properties.vendorID == 42 && features.robustBufferAccess && memory.memoryTypeCount);
        get_properties((VkPhysicalDevice)(uintptr_t)1, &properties);
        get_properties(NULL, &properties);
        get_properties(devices[0], NULL);
        get_features(devices[0], NULL);
        get_features(NULL, &features);
        get_memory(devices[0], NULL);
        get_memory(NULL, &memory);
        assert(enumerate(NULL, &count, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(enumerate((VkInstance)(uintptr_t)1, &count, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(enumerate(instance, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        PFN_vkGetDeviceProcAddr device =
            (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        assert(device && !device(NULL, "vkCreateBuffer"));
        PFN_vkDestroyInstance retire =
            (PFN_vkDestroyInstance)lookup_external(instance, "vkDestroyInstance");
        retire(NULL, NULL);
        retire((VkInstance)(uintptr_t)1, NULL);
        destroy(instance);
        assert(!lookup_external(instance, "vkDestroyInstance"));
        retire(instance, NULL);
    }
    assert(venus_icd_unbind() == RingOk);
}
#ifdef _WIN32
static DWORD WINAPI stress_thread(void *argument) {
#else
static void *stress_thread(void *argument) {
#endif
    (void)argument;
    for (unsigned iteration = 0; iteration < 32; iteration++) {
        VkInstance instance = create();
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        assert(enumerate(instance, &count, NULL) == VK_SUCCESS && count == 2);
        destroy(instance);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void concurrent(void) {
    fixture_t fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
#ifdef _WIN32
    HANDLE threads[4];
    for (unsigned index = 0; index < 4; index++) {
        threads[index] = CreateThread(NULL, 0, stress_thread, NULL, 0, NULL);
        assert(threads[index]);
    }
    for (unsigned index = 0; index < 4; index++) {
        assert(WaitForSingleObject(threads[index], 30000) == WAIT_OBJECT_0);
        DWORD result;
        assert(GetExitCodeThread(threads[index], &result) && result == 0);
        assert(CloseHandle(threads[index]));
    }
#else
    pthread_t threads[4];
    for (unsigned index = 0; index < 4; index++)
        assert(pthread_create(&threads[index], NULL, stress_thread, NULL) == 0);
    for (unsigned index = 0; index < 4; index++)
        assert(pthread_join(threads[index], NULL) == 0);
#endif
    assert(venus_icd_unbind() == RingOk);
}
static void failures(void) {
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance = (VkInstance)(uintptr_t)1;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED &&
           !instance);
    assert(create_function()(&info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    fixture_t fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    info.flags = 1;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED &&
           !instance);
    info.flags = 0;
    assert(create_function()(NULL, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    fixture.create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_OUT_OF_HOST_MEMORY && !instance);
    assert(venus_icd_unbind() == RingOk);
    const uint32_t Commands[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 33};
    for (unsigned item = 0; item < sizeof(Commands) / sizeof(*Commands); item++) {
        uint32_t command = Commands[item];
        fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        if (command == 0) {
            fixture.corrupt_command = 0;
            assert(create_function()(&info, NULL, &instance) == VK_ERROR_DEVICE_LOST && !instance);
        } else {
            instance = create();
            fixture.corrupt_command = command;
            if (command == 1)
                destroy(instance);
            else {
                PFN_vkEnumeratePhysicalDevices enumerate =
                    (PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
                                                                    "vkEnumeratePhysicalDevices");
                uint32_t count = 2;
                VkPhysicalDevice devices[2];
                if (command == 2)
                    assert(enumerate(instance, &count, devices) == VK_ERROR_DEVICE_LOST);
                else {
                    assert(enumerate(instance, &count, devices) == VK_SUCCESS);
                    if (command == 3) {
                        VkPhysicalDeviceFeatures output = {0};
                        ((PFN_vkGetPhysicalDeviceFeatures)lookup_external(
                            instance, "vkGetPhysicalDeviceFeatures"))(devices[0], &output);
                        assert(!output.robustBufferAccess);
                    } else if (command == 6) {
                        VkPhysicalDeviceProperties output = {0};
                        ((PFN_vkGetPhysicalDeviceProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceProperties"))(devices[0], &output);
                        assert(!output.vendorID);
                    } else if (command == 4) {
                        VkFormatProperties output = {0};
                        ((PFN_vkGetPhysicalDeviceFormatProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceFormatProperties"))(
                            devices[0], VK_FORMAT_R8G8B8A8_UNORM, &output);
                        assert(!output.optimalTilingFeatures);
                    } else if (command == 5) {
                        VkImageFormatProperties output = {0};
                        assert(((PFN_vkGetPhysicalDeviceImageFormatProperties)lookup_external(
                                   instance, "vkGetPhysicalDeviceImageFormatProperties"))(
                                   devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                   VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                                   &output) == VK_ERROR_DEVICE_LOST);
                    } else if (command == 7) {
                        VkQueueFamilyProperties output = {0};
                        count = 1;
                        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceQueueFamilyProperties"))(devices[0],
                                                                                   &count, &output);
                        assert(!output.queueCount);
                    } else if (command == 33) {
                        VkSparseImageFormatProperties output = {0};
                        count = 1;
                        ((PFN_vkGetPhysicalDeviceSparseImageFormatProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceSparseImageFormatProperties"))(
                            devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_TILING_OPTIMAL, &count, &output);
                        assert(!output.aspectMask);
                    } else {
                        VkPhysicalDeviceMemoryProperties output = {0};
                        ((PFN_vkGetPhysicalDeviceMemoryProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceMemoryProperties"))(devices[0], &output);
                        assert(!output.memoryTypeCount);
                    }
                }
            }
            uint32_t number = 99;
            assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
                       instance, "vkEnumeratePhysicalDevices"))(instance, &number, NULL) ==
                   VK_ERROR_DEVICE_LOST);
            VkInstance second = NULL;
            assert(create_function()(&info, NULL, &second) == VK_ERROR_DEVICE_LOST && !second);
        }
        assert(venus_icd_unbind() == RingAgain);
        venus_icd_abandon(); /* Fake receiver owns no live native Vulkan work. */
        assert(venus_icd_unbind() == RingOk);
    }
    fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    VkBaseInStructure link = {.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO};
    info.pNext = &link;
    instance = NULL;
    assert(create_function()(&info, NULL, &instance) == VK_SUCCESS);
    destroy(instance);
    link.pNext = &link;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    link.pNext = NULL;
    link.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    info.pNext = NULL;
    assert(venus_icd_unbind() == RingOk);
    fixture = fresh();
    fixture.device_count = 0;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    instance = create();
    uint32_t none = 99;
    assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
               instance, "vkEnumeratePhysicalDevices"))(instance, &none, NULL) == VK_SUCCESS &&
           none == 0);
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
    fixture = fresh();
    fixture.fail_fill = 1;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    instance = create();
    none = 0;
    assert(
        ((PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices"))(
            instance, &none, NULL) == VK_ERROR_DEVICE_LOST);
    venus_icd_abandon();
    fixture = fresh();
    fixture.poll_again = 1001;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_DEVICE_LOST);
    venus_icd_abandon();
    fixture = fresh();
    fixture.fail_command = 0;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_DEVICE_LOST);
    venus_icd_abandon();
    fixture = fresh();
    fixture.enumerate_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    instance = create();
    uint32_t count = 0;
    assert(
        ((PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices"))(
            instance, &count, NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
    fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    VkInstance instances[16];
    for (unsigned index = 0; index < 16; index++)
        instances[index] = create();
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_OUT_OF_HOST_MEMORY && !instance);
    for (unsigned index = 0; index < 16; index++)
        destroy(instances[index]);
    assert(venus_icd_unbind() == RingOk);
}
#ifdef VgpuIcdLoader
/** @brief Dynamic binding function borrows callback/context until unbind. */
typedef venus_ring_status_t (*binding_t)(venus_command_exchange_t, void *);
/** @brief Dynamic unbinding function owns no storage. */
typedef venus_ring_status_t (*unbinding_t)(void);
static void loader_fixture(void) {
    void *library = dlopen("build/libwaddle_vulkan_experimental.so", RTLD_NOW | RTLD_LOCAL);
    void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(library && loader);
    binding_t bind = NULL;
    unbinding_t unbind = NULL;
    PFN_vkGetInstanceProcAddr lookup = NULL;
    void *address = dlsym(library, "venus_icd_bind");
    memcpy(&bind, &address, sizeof(bind));
    address = dlsym(library, "venus_icd_unbind");
    memcpy(&unbind, &address, sizeof(unbind));
    address = dlsym(loader, "vkGetInstanceProcAddr");
    memcpy(&lookup, &address, sizeof(lookup));
    assert(bind && unbind && lookup);
    const char *previous = getenv("VK_DRIVER_FILES");
    char *saved = previous ? strdup(previous) : NULL;
    assert(!previous || saved);
    assert(setenv("VK_DRIVER_FILES", "build/waddle_vulkan_experimental.json", 1) == 0);
    fixture_t fixture = fresh();
    assert(bind(exchange, &fixture) == RingOk);
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)lookup(NULL, "vkCreateInstance");
    assert(create);
    for (unsigned iteration = 0; iteration < 8; iteration++) {
        VkInstance instance = NULL;
        VkResult result = create(&info, NULL, &instance);
        if (result != VK_SUCCESS)
            fprintf(stderr, "loader create failed: %d\n", result);
        assert(result == VK_SUCCESS && instance);
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)lookup(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        assert(enumerate && enumerate(instance, &count, NULL) == VK_SUCCESS && count == 2);
        VkPhysicalDevice devices[2];
        assert(enumerate(instance, &count, devices) == VK_SUCCESS && count == 2);
        PFN_vkGetPhysicalDeviceProperties properties =
            (PFN_vkGetPhysicalDeviceProperties)lookup(instance, "vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties value = {0};
        assert(properties);
        properties(devices[0], &value);
        assert(value.vendorID == 42);
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)lookup(instance, "vkDestroyInstance");
        assert(destroy);
        destroy(instance, NULL);
    }
    assert(unbind() == RingOk);
    assert(saved ? setenv("VK_DRIVER_FILES", saved, 1) == 0 : unsetenv("VK_DRIVER_FILES") == 0);
    free(saved);
    saved = NULL;
    assert(dlclose(loader) == 0);
    assert(dlclose(library) == 0);
    puts("System Vulkan loader: manifest discovery, dispatch and eight instance lifecycles passed");
}
#endif
int main(void) {
    assert(venus_icd_unbind() == RingOk);
    assert(venus_icd_bind(NULL, NULL) == RingInvalid);
    routing();
    fixture_t fixture = fresh();
    fixture.poll_again = 2;
    fixture.reply_again = 2;
    healthy(&fixture);
    concurrent();
    failures();
    venus_icd_abandon();
#ifdef VgpuIcdLoader
    loader_fixture();
#endif
    puts("ICD native instance dispatch: 128 cycles, exact loader aliases and sticky loss passed");
    return 0;
}
