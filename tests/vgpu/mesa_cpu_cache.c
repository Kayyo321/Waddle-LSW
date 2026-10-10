/** @file mesa_cpu_cache.c @brief Ordinary Vulkan enumeration and real driver-unload ownership regression. */
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
/** @brief Fixed independently repeated instance/library ownership cycles. */
#define TeardownCycles 3u
/** @brief Exclusive borrowed Vulkan dispatch and owned native teardown handles.
 * @note Single fixture thread; dispatch expires when library closes. No retained allocations.
 */
typedef struct vulkan_fixture_t {
    void *library; /**< Owned nullable dlopen handle; released last. */
    VkInstance instance; /**< Owned nullable instance; destroyed before library. */
    PFN_vkCreateInstance create; /**< Borrowed static library export. */
    PFN_vkDestroyInstance destroy; /**< Borrowed static library export. */
    PFN_vkEnumeratePhysicalDevices enumerate; /**< Borrowed static library export. */
} vulkan_fixture_t;
/** @brief Resolve one ELF loader export without an object/function pointer cast.
 * @param[in] library Nonnull owned live library, borrowed for call.
 * @param[in] name Nonnull static accessible terminated symbol name.
 * @param[out] output Nonnull private function-pointer storage[bytes], unchanged on failure.
 * @param[in] bytes Exact native function-pointer extent, equal to an ELF object pointer.
 * @return Zero on success, one on missing export or unsupported pointer ABI.
 * @note Allocation-free fixture code, no pointer retention, single thread.
 */
static int load_proc(void *library, const char *name, void *output, size_t bytes) {
    if (!library || !name || !output || bytes != sizeof(void *)) return 1;
    dlerror();
    void *symbol = dlsym(library, name);
    const char *error = dlerror();
    if (error || !symbol) {
        fprintf(stderr, "Vulkan loader export %s unavailable: %s\n", name, error ? error : "null symbol");
        return 1;
    }
    memcpy(output, &symbol, bytes);
    return 0;
}
/** @brief Retire all native ownership, including partial initialization failures.
 * @param[in,out] fixture Nonnull exclusive initialized fixture, scrubbed before return.
 * @return Zero on successful/idempotent cleanup, one if dlclose reports failure.
 * @note No allocations; instance destroyed first; freed handles and borrowed dispatch NULLed.
 * Single thread; Vulkan implementation owns its internal instance allocations.
 */
static int fixture_free(vulkan_fixture_t *fixture) {
    int result = 0;
    if (fixture->instance) {
        fixture->destroy(fixture->instance, NULL);
        fixture->instance = VK_NULL_HANDLE;
    }
    fixture->create = NULL;
    fixture->destroy = NULL;
    fixture->enumerate = NULL;
    if (fixture->library) {
        if (dlclose(fixture->library)) {
            fprintf(stderr, "Vulkan loader close failed: %s\n", dlerror());
            result = 1;
        }
        fixture->library = NULL;
    }
    return result;
}
/** @brief Confirm render-node namespace presence without trusting an API error alone.
 * @return Zero for absent /dev/dri or no renderD* entry; one for any matching
 * entry; minus one for directory open/read/close error other than absent path.
 * @note No parameters; single fixture thread. Own nullable DIR allocation only
 * within this call, closed on every acquired-directory path and NULLed after close.
 */
static int render_node_presence(void) {
    DIR *directory = opendir("/dev/dri");
    if (!directory) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "Render-node directory open failed: %s\n", strerror(errno));
        return -1;
    }
    int result = 0;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno) {
                fprintf(stderr, "Render-node directory read failed: %s\n", strerror(errno));
                result = -1;
            }
            break;
        }
        if (!strncmp(entry->d_name, "renderD", 7)) {
            result = 1;
            break;
        }
    }
    if (closedir(directory)) {
        fprintf(stderr, "Render-node directory close failed: %s\n", strerror(errno));
        result = -1;
    }
    directory = NULL;
    return result;
}
/** @brief Classify normal enumeration or exact confirmed no-GPU error.
 * @param[in] status Native enumeration result. @param[in] count Initialized count.
 * @param[in] render_nodes Zero confirms absence; one presence; minus one namespace error.
 * @param[in] require_device Nonzero requires successful positive device count.
 * @return One for accepted enumeration, zero for any invalid result. Default mode
 * also accepts INIT_FAILED/count0/confirmed-absence; required mode never does.
 * @note Pure, allocation-free, thread-safe. No API error or directory failure suppressed.
 */
static int enumeration_valid(VkResult status, uint32_t count, int render_nodes, int require_device) {
    if (require_device) return status == VK_SUCCESS && count > 0;
    return status == VK_SUCCESS ||
        (status == VK_ERROR_INITIALIZATION_FAILED && count == 0 && render_nodes == 0);
}
/** @brief Run one ordinary instance enumeration/destruction and verify actual driver unload.
 * @param[in] driver_path Nonnull accessible absolute driver path, borrowed for call.
 * @param[in] require_device Nonzero requires positive successful enumeration.
 * @return Zero for exact successful acquisition/teardown, one for any loader/API/unload failure.
 * @note All errors retire acquired ownership, including temporary directory storage;
 * no GPU selection occurs here: caller supplies the manifest through normal loader configuration.
 */
static int run_cycle(const char *driver_path, int require_device) {
    vulkan_fixture_t fixture = {0};
    int result = 1;
    fixture.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!fixture.library) {
        fprintf(stderr, "Vulkan loader open failed: %s\n", dlerror());
        goto done;
    }
    if (load_proc(fixture.library, "vkCreateInstance", &fixture.create, sizeof(fixture.create)) ||
        load_proc(fixture.library, "vkDestroyInstance", &fixture.destroy, sizeof(fixture.destroy)) ||
        load_proc(fixture.library, "vkEnumeratePhysicalDevices", &fixture.enumerate, sizeof(fixture.enumerate)))
        goto done;
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application};
    VkResult status = fixture.create(&info, NULL, &fixture.instance);
    if (status != VK_SUCCESS || !fixture.instance) {
        fprintf(stderr, "Vulkan instance creation failed: %d\n", status);
        goto done;
    }
    uint32_t count = 0;
    status = fixture.enumerate(fixture.instance, &count, NULL);
    const int render_nodes = !require_device && status == VK_ERROR_INITIALIZATION_FAILED && count == 0
        ? render_node_presence() : -1;
    if (!enumeration_valid(status, count, render_nodes, require_device)) {
        fprintf(stderr, "Vulkan physical enumeration failed: result=%d count=%u require_device=%d\n",
            status, count, require_device);
        goto done;
    }
    void *participating = dlopen(driver_path, RTLD_NOW | RTLD_NOLOAD);
    if (!participating) {
        fprintf(stderr, "Intended Mesa driver was not resident after physical enumeration\n");
        goto done;
    }
    const int probe_status = dlclose(participating);
    participating = NULL;
    if (probe_status) {
        fprintf(stderr, "Participating driver probe close failed: %s\n", dlerror());
        goto done;
    }
    printf("Mesa physical enumeration: result=%d count=%u require_device=%d\n",
        status, count, require_device);
    /* The native loader returns INIT_FAILED when all ICDs report zero devices.
     * Confirm namespace absence before accepting that legitimate headless path;
     * enumeration still probes the same RADV CPU topology initialization. */
    result = 0;
done:
    if (fixture_free(&fixture)) result = 1;
    if (!result) {
        void *resident = dlopen(driver_path, RTLD_NOW | RTLD_NOLOAD);
        if (resident) {
            fprintf(stderr, "Mesa driver remained resident after ordinary Vulkan teardown\n");
            if (dlclose(resident)) fprintf(stderr, "Resident probe close failed: %s\n", dlerror());
            resident = NULL;
            result = 1;
        }
    }
    return result;
}
/** @brief Execute three independent real driver retirement cycles.
 * @param[in] argc Two or three. @param[in] argv Nonnull borrowed terminated strings;
 * argv[1] is the absolute patched driver path selected by the caller's Vulkan manifest.
 * Optional argv[2] is exactly --require-device; other options fail before acquisition.
 * @return Zero only when every Vulkan call and actual unload succeeds, one otherwise.
 * @note Single thread; temporary directory allocation closed before return.
 * ASan/LSan enforce fixture and vendor allocation cleanup.
 */
int main(int argc, char **argv) {
    if ((argc != 2 && argc != 3) || !argv[1] || argv[1][0] != '/' ||
        (argc == 3 && (!argv[2] || strcmp(argv[2], "--require-device")))) {
        fprintf(stderr, "Usage: mesa_cpu_cache ABSOLUTE_DRIVER_LIBRARY [--require-device]\n");
        return 1;
    }
    for (unsigned cycle = 0; cycle < TeardownCycles; cycle++)
        if (run_cycle(argv[1], argc == 3)) return 1;
    puts("Mesa CPU cache: three ordinary Vulkan teardown cycles and actual driver unload passed");
    return 0;
}
