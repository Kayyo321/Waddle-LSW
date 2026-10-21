/** @file tcp_gpu_wsi_lifetime_windows.inc @brief Actual native WSI resize/loss lifetimes.
 * Included after tcp_gpu_present_windows.inc. Uses its real Vulkan triangle renderer
 * and independent GPU readback; no synthetic WSI, SDK, or GPU behavior is substituted.
 */

/** @brief Report a native WSI lifetime failure at its exact operation stage.
 * @param[in] test_name Static case label; borrowed for this call.
 * @param[in] stage Static operation label; borrowed for this call.
 * @return One, for direct propagation by the fixture.
 * @note Sole fixture thread; no ownership or allocation.
 */
static int wsi_lifetime_failure(const char *test_name, const char *stage)
{
    fprintf(stderr, "Native WSI lifetime case failed: %s: %s\n", test_name, stage);
    fflush(stderr);
    return 1;
}

/** @brief Set and verify a popup's exact native client extent.
 * @param[in] window Live HWND borrowed from the caller.
 * @param[in] width,height Requested client dimensions in pixels.
 * @return Zero only when SetWindowPos succeeds and GetClientRect confirms both axes.
 * @note Caller retains HWND. The popup has no non-client frame. No GDI owner acquired.
 */
static int wsi_lifetime_set_extent(HWND window, int width, int height)
{
    RECT client = {0};
    if (!window || !IsWindow(window) ||
        !SetWindowPos(window, HWND_TOPMOST, 100, 100, width, height,
            SWP_SHOWWINDOW | SWP_NOACTIVATE) ||
        !GetClientRect(window, &client)) return 1;
    return client.right - client.left != width || client.bottom - client.top != height;
}

/** @brief Compare every native client RGB pixel with the independent GPU BGRA readback.
 * @param[in] window Live popup HWND borrowed from the caller.
 * @param[in] pixels Borrowed 64x64x4 output from swapchain_triangle_render.
 * @return Zero only after all 4096 RGB pixels match and the acquired DC is released.
 * @note Caller completes presentation first. Every successful GetDC receives one
 * ReleaseDC attempt, including pixel mismatch and DwmFlush failure paths.
 */
static int wsi_lifetime_compare_pixels(HWND window, const unsigned char *pixels)
{
    HDC dc = GetDC(window);
    int result = 0;
    if (!dc) return 1;
    if (DwmFlush() != S_OK) result = 1;
    for (unsigned y = 0; !result && y < 64; y++) {
        for (unsigned x = 0; x < 64; x++) {
            const unsigned char *pixel = pixels + ((size_t)y * 64 + x) * 4;
            const COLORREF expected = RGB(pixel[2], pixel[1], pixel[0]);
            const COLORREF actual = GetPixel(dc, (int)x, (int)y);
            if (pixel[3] != 255 || actual == CLR_INVALID || actual != expected) {
                fprintf(stderr, "Native WSI lifetime pixel (%u,%u): got=%08lx expected=%08lx alpha=%u\n",
                    x, y, (unsigned long)actual, (unsigned long)expected, pixel[3]);
                result = 1;
                break;
            }
        }
    }
    if (!ReleaseDC(window, dc)) result = 1;
    return result;
}

/** @brief Fill the exact valid 64x64 BGRA two-image WSI request.
 * @param[in] surface Live borrowed VkSurfaceKHR owned by the enclosing probe.
 * @param[in] old_chain Optional live same-surface chain; borrowed and never destroyed here.
 * @return A by-value SDK request with no chained pointer or transferred ownership.
 */
static VkSwapchainCreateInfoKHR wsi_lifetime_chain_info(VkSurfaceKHR surface,
    VkSwapchainKHR old_chain)
{
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = 2,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {64, 64},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old_chain
    };
    return info;
}

/** @brief Fetch exactly two distinct real images from a live native swapchain.
 * @param[in] device Live borrowed VkDevice.
 * @param[in] chain Live borrowed VkSwapchainKHR.
 * @param[in] images Loaded Vulkan image enumeration function.
 * @param[out] output Exclusive two-element caller storage, written only on success.
 * @return Zero when Vulkan reports exactly two nonnull distinct borrowed images.
 */
static int wsi_lifetime_get_two_images(VkDevice device, VkSwapchainKHR chain,
    PFN_vkGetSwapchainImagesKHR images, VkImage output[2])
{
    uint32_t count = 0;
    if (images(device, chain, &count, NULL) != VK_SUCCESS || count != 2) return 1;
    VkImage values[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t capacity = 2;
    if (images(device, chain, &capacity, values) != VK_SUCCESS || capacity != 2 ||
        values[0] == VK_NULL_HANDLE || values[1] == VK_NULL_HANDLE || values[0] == values[1]) return 1;
    output[0] = values[0];
    output[1] = values[1];
    return 0;
}

/** @brief Submit a real wait-semaphore presentation and require success at both levels.
 * @param[in] queue Live borrowed queue.
 * @param[in] chain Live borrowed chain, including a chain retired by replacement.
 * @param[in] rendered Live binary semaphore signaled by completed triangle rendering.
 * @param[in] index Acquired image index belonging to chain.
 * @param[in] present Loaded Vulkan queue-presentation function.
 * @return Zero only when both queue and per-chain result are VK_SUCCESS.
 * @note The queue consumes rendered. Caller waits for device idle before releasing owners.
 */
static int wsi_lifetime_present(VkQueue queue, VkSwapchainKHR chain, VkSemaphore rendered,
    uint32_t index, PFN_vkQueuePresentKHR present)
{
    VkResult chain_result = VK_ERROR_UNKNOWN;
    const VkPresentInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered,
        .swapchainCount = 1,
        .pSwapchains = &chain,
        .pImageIndices = &index,
        .pResults = &chain_result
    };
    const VkResult queue_result = present(queue, &info);
    return queue_result != VK_SUCCESS || chain_result != VK_SUCCESS;
}

/** @brief Exercise real HWND extent, swapchain retirement, acquired-image and loss behavior.
 * @param[in] instance,physical,device,queue,family Live borrowed Vulkan owners.
 * @param[in] memory Live borrowed physical memory properties used by the GPU readback helper.
 * @param[in] lookup,device_proc Live borrowed Vulkan dispatchers.
 * @return Zero only after every requested actual WSI transition, exact image comparison,
 * balanced HWND/surface/swapchain/semaphore/DC teardown, and stable GDI/USER counts.
 * @note Sole native fixture thread. Owns each HWND, Vulkan surface, swapchain, semaphore,
 * temporary image-index arrays, and short-lived DC. Triangle rendering owns and frees its
 * Vulkan temporaries and returns only after fence and queue completion. A failed case runs
 * the same idempotent owner cleanup; no HWND generation-reuse claim is made.
 */
static int native_wsi_lifetime_probe(VkInstance instance, VkPhysicalDevice physical,
    VkDevice device, VkQueue queue, uint32_t family,
    const VkPhysicalDeviceMemoryProperties *memory,
    PFN_vkGetInstanceProcAddr lookup, PFN_vkGetDeviceProcAddr device_proc)
{
    enum { Client64 = 64, Client80 = 80, ReadbackBytes = Client64 * Client64 * 4 };
    PFN_vkCreateWin32SurfaceKHR create_surface =
        (PFN_vkCreateWin32SurfaceKHR)lookup(instance, "vkCreateWin32SurfaceKHR");
    PFN_vkDestroySurfaceKHR destroy_surface =
        (PFN_vkDestroySurfaceKHR)lookup(instance, "vkDestroySurfaceKHR");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surface_support =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)lookup(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)lookup(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkCreateSwapchainKHR create_swapchain =
        (PFN_vkCreateSwapchainKHR)compute_proc(device, device_proc, "vkCreateSwapchainKHR");
    PFN_vkDestroySwapchainKHR destroy_swapchain =
        (PFN_vkDestroySwapchainKHR)compute_proc(device, device_proc, "vkDestroySwapchainKHR");
    PFN_vkGetSwapchainImagesKHR get_images =
        (PFN_vkGetSwapchainImagesKHR)compute_proc(device, device_proc, "vkGetSwapchainImagesKHR");
    PFN_vkAcquireNextImageKHR acquire =
        (PFN_vkAcquireNextImageKHR)compute_proc(device, device_proc, "vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR present =
        (PFN_vkQueuePresentKHR)compute_proc(device, device_proc, "vkQueuePresentKHR");
    PFN_vkCreateSemaphore create_semaphore =
        (PFN_vkCreateSemaphore)compute_proc(device, device_proc, "vkCreateSemaphore");
    PFN_vkDestroySemaphore destroy_semaphore =
        (PFN_vkDestroySemaphore)compute_proc(device, device_proc, "vkDestroySemaphore");
    PFN_vkDeviceWaitIdle wait_idle =
        (PFN_vkDeviceWaitIdle)compute_proc(device, device_proc, "vkDeviceWaitIdle");
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    DWORD gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    DWORD user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    HWND window = NULL;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    unsigned char readback[ReadbackBytes];
    int result = 1;
    const char *stage = "Vulkan dispatch acquisition";

    if (!create_surface || !destroy_surface || !surface_support || !capabilities ||
        !create_swapchain || !destroy_swapchain || !get_images || !acquire || !present ||
        !create_semaphore || !destroy_semaphore || !wait_idle) goto cleanup;
    stage = "64x64 popup creation and exact client verification";
    window = CreateWindowExA(0, "STATIC", "Waddle WSI lifetime", WS_POPUP | WS_VISIBLE,
        100, 100, Client64, Client64, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!window || wsi_lifetime_set_extent(window, Client64, Client64)) goto cleanup;
    stage = "Win32 Vulkan surface creation and queue support";
    const VkWin32SurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleA(NULL),
        .hwnd = window
    };
    VkBool32 supported = VK_FALSE;
    if (create_surface(instance, &surface_info, NULL, &surface) != VK_SUCCESS ||
        surface == VK_NULL_HANDLE || surface_support(physical, family, surface, &supported) != VK_SUCCESS ||
        !supported) goto cleanup;
    stage = "binary semaphore acquisition";
    if (create_semaphore(device, &semaphore_info, NULL, &acquired) != VK_SUCCESS ||
        acquired == VK_NULL_HANDLE ||
        create_semaphore(device, &semaphore_info, NULL, &rendered) != VK_SUCCESS ||
        rendered == VK_NULL_HANDLE) goto cleanup;

    /* A failed extent-mismatch replacement retires its valid old chain, while one
     * image acquired before that failure remains presentable and owns its backing. */
    stage = "initial 64x64 chain creation";
    VkSwapchainCreateInfoKHR chain_info = wsi_lifetime_chain_info(surface, VK_NULL_HANDLE);
    if (create_swapchain(device, &chain_info, NULL, &chain) != VK_SUCCESS ||
        chain == VK_NULL_HANDLE) goto cleanup;
    VkImage chain_images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    stage = "initial chain image enumeration";
    if (wsi_lifetime_get_two_images(device, chain, get_images, chain_images)) goto cleanup;
    uint32_t acquired_index = UINT32_MAX;
    stage = "initial chain acquire";
    if (acquire(device, chain, UINT64_C(1000000000), acquired, VK_NULL_HANDLE,
            &acquired_index) != VK_SUCCESS || acquired_index >= 2) goto cleanup;
    stage = "actual 64x64 GPU triangle and independent readback";
    if (swapchain_triangle_render(device, queue, family, memory, device_proc,
            chain_images[acquired_index], acquired, rendered, readback)) goto cleanup;
    stage = "valid failed 80x64 replacement with unchanged null output";
    chain_info = wsi_lifetime_chain_info(surface, chain);
    chain_info.imageExtent.width = Client80;
    replacement = VK_NULL_HANDLE;
    const VkResult replacement_status = create_swapchain(device, &chain_info, NULL, &replacement);
    if (replacement_status != VK_ERROR_OUT_OF_DATE_KHR || replacement != VK_NULL_HANDLE) goto cleanup;
    stage = "retired old-chain acquire result and untouched image index";
    uint32_t unchanged_index = UINT32_C(0x5a5a5a5a);
    if (acquire(device, chain, 0, acquired, VK_NULL_HANDLE, &unchanged_index) !=
            VK_ERROR_OUT_OF_DATE_KHR || unchanged_index != UINT32_C(0x5a5a5a5a)) goto cleanup;
    stage = "previously acquired retired image presentation";
    if (wsi_lifetime_present(queue, chain, rendered, acquired_index, present) ||
        wait_idle(device) != VK_SUCCESS) goto cleanup;
    stage = "retired-image presentation pixel-for-pixel verification";
    if (wsi_lifetime_compare_pixels(window, readback)) goto cleanup;
    printf("Native WSI lifetime case=failed-replacement-retained-acquired-image PASS\n");
    fflush(stdout);
    stage = "initial chain destruction after presentation completion";
    if (wait_idle(device) != VK_SUCCESS) goto cleanup;
    destroy_swapchain(device, chain, NULL);
    chain = VK_NULL_HANDLE;

    /* Zero-area is a valid native surface state but invalidates this chain.
     * Restore cannot clear the sticky out-of-date generation; replacement is explicit. */
    stage = "fresh 64x64 chain creation for zero-area recovery";
    chain_info = wsi_lifetime_chain_info(surface, VK_NULL_HANDLE);
    if (create_swapchain(device, &chain_info, NULL, &chain) != VK_SUCCESS ||
        chain == VK_NULL_HANDLE) goto cleanup;
    stage = "popup transition to zero client extent";
    if (wsi_lifetime_set_extent(window, 0, 0)) goto cleanup;
    stage = "native zero-area client rectangle verification";
    RECT zero_client = {0};
    if (!IsWindow(window) || !GetClientRect(window, &zero_client) ||
        zero_client.right != zero_client.left || zero_client.bottom != zero_client.top) goto cleanup;
    stage = "paired zero-extent surface capabilities";
    VkSurfaceCapabilitiesKHR caps = {0};
    if (capabilities(physical, surface, &caps) != VK_SUCCESS ||
        caps.currentExtent.width != 0 || caps.currentExtent.height != 0 ||
        caps.minImageExtent.width != 0 || caps.minImageExtent.height != 0 ||
        caps.maxImageExtent.width != 0 || caps.maxImageExtent.height != 0) goto cleanup;
    stage = "zero-area out-of-date acquire and unchanged output index";
    unchanged_index = UINT32_C(0x5a5a5a5a);
    if (acquire(device, chain, 0, acquired, VK_NULL_HANDLE, &unchanged_index) !=
            VK_ERROR_OUT_OF_DATE_KHR ||
        unchanged_index != UINT32_C(0x5a5a5a5a)) goto cleanup;
    stage = "restore exact 64x64 client extent";
    if (wsi_lifetime_set_extent(window, Client64, Client64)) goto cleanup;
    stage = "restored old chain remains sticky out-of-date";
    unchanged_index = UINT32_C(0x5a5a5a5a);
    if (acquire(device, chain, 0, acquired, VK_NULL_HANDLE, &unchanged_index) !=
            VK_ERROR_OUT_OF_DATE_KHR ||
        unchanged_index != UINT32_C(0x5a5a5a5a)) goto cleanup;
    stage = "fresh 64x64 replacement after zero-area invalidation";
    chain_info = wsi_lifetime_chain_info(surface, chain);
    if (create_swapchain(device, &chain_info, NULL, &replacement) != VK_SUCCESS ||
        replacement == VK_NULL_HANDLE || wait_idle(device) != VK_SUCCESS) goto cleanup;
    destroy_swapchain(device, chain, NULL);
    chain = replacement;
    replacement = VK_NULL_HANDLE;
    stage = "replacement 64x64 chain acquire";
    acquired_index = UINT32_MAX;
    if (acquire(device, chain, UINT64_C(1000000000), acquired, VK_NULL_HANDLE,
            &acquired_index) != VK_SUCCESS || acquired_index >= 2) goto cleanup;
    stage = "restored chain actual GPU rendering";
    if (wsi_lifetime_get_two_images(device, chain, get_images, chain_images) ||
        swapchain_triangle_render(device, queue, family, memory, device_proc,
            chain_images[acquired_index], acquired, rendered, readback)) goto cleanup;
    stage = "restored chain real presentation";
    if (wsi_lifetime_present(queue, chain, rendered, acquired_index, present) ||
        wait_idle(device) != VK_SUCCESS) goto cleanup;
    stage = "restored chain exact client/GPU pixel comparison";
    if (wsi_lifetime_compare_pixels(window, readback)) goto cleanup;
    printf("Native WSI lifetime case=zero-area-sticky-out-of-date-replacement-and-restore PASS\n");
    fflush(stdout);

    /* A nonzero resize invalidates acquire on the old size. A valid replacement
     * is created at the new current extent; resizing back creates another generation. */
    stage = "popup resize to exact 80x64 client extent";
    if (wsi_lifetime_set_extent(window, Client80, Client64)) goto cleanup;
    stage = "80x64 surface capability snapshot";
    memset(&caps, 0, sizeof caps);
    if (capabilities(physical, surface, &caps) != VK_SUCCESS ||
        caps.currentExtent.width != Client80 || caps.currentExtent.height != Client64 ||
        caps.minImageExtent.width != Client80 || caps.minImageExtent.height != Client64 ||
        caps.maxImageExtent.width != Client80 || caps.maxImageExtent.height != Client64) goto cleanup;
    stage = "old 64x64 acquire after resize and unchanged image index";
    unchanged_index = UINT32_C(0x5a5a5a5a);
    if (acquire(device, chain, 0, acquired, VK_NULL_HANDLE, &unchanged_index) !=
            VK_ERROR_OUT_OF_DATE_KHR || unchanged_index != UINT32_C(0x5a5a5a5a)) goto cleanup;
    stage = "80x64 replacement creation";
    chain_info = wsi_lifetime_chain_info(surface, chain);
    chain_info.imageExtent.width = Client80;
    chain_info.imageExtent.height = Client64;
    if (create_swapchain(device, &chain_info, NULL, &replacement) != VK_SUCCESS ||
        replacement == VK_NULL_HANDLE) goto cleanup;
    if (wait_idle(device) != VK_SUCCESS) goto cleanup;
    destroy_swapchain(device, chain, NULL);
    chain = replacement;
    replacement = VK_NULL_HANDLE;
    stage = "popup resize back to exact 64x64 client extent";
    if (wsi_lifetime_set_extent(window, Client64, Client64)) goto cleanup;
    stage = "64x64 replacement from the 80x64 chain";
    chain_info = wsi_lifetime_chain_info(surface, chain);
    if (create_swapchain(device, &chain_info, NULL, &replacement) != VK_SUCCESS ||
        replacement == VK_NULL_HANDLE) goto cleanup;
    if (wait_idle(device) != VK_SUCCESS) goto cleanup;
    destroy_swapchain(device, chain, NULL);
    chain = replacement;
    replacement = VK_NULL_HANDLE;
    stage = "resized-back replacement acquire and GPU render";
    acquired_index = UINT32_MAX;
    if (acquire(device, chain, UINT64_C(1000000000), acquired, VK_NULL_HANDLE,
            &acquired_index) != VK_SUCCESS || acquired_index >= 2 ||
        wsi_lifetime_get_two_images(device, chain, get_images, chain_images) ||
        swapchain_triangle_render(device, queue, family, memory, device_proc,
            chain_images[acquired_index], acquired, rendered, readback)) goto cleanup;
    stage = "resized-back replacement exact native presentation";
    if (wsi_lifetime_present(queue, chain, rendered, acquired_index, present) ||
        wait_idle(device) != VK_SUCCESS ||
        wsi_lifetime_compare_pixels(window, readback)) goto cleanup;
    printf("Native WSI lifetime case=resize-capabilities-out-of-date-and-replacements PASS\n");
    fflush(stdout);
    stage = "resize generations explicit chain teardown";
    if (wait_idle(device) != VK_SUCCESS) goto cleanup;
    destroy_swapchain(device, chain, NULL);
    chain = VK_NULL_HANDLE;
    destroy_surface(instance, surface, NULL);
    surface = VK_NULL_HANDLE;
    if (!DestroyWindow(window)) goto cleanup;
    window = NULL;

    /* Native destruction is observed as sticky SURFACE_LOST. A valid old-chain
     * replacement attempt reports surface loss after retiring that old chain. */
    stage = "fresh native window for observed-loss case";
    window = CreateWindowExA(0, "STATIC", "Waddle WSI loss", WS_POPUP | WS_VISIBLE,
        100, 100, Client64, Client64, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!window || wsi_lifetime_set_extent(window, Client64, Client64)) goto cleanup;
    stage = "loss-case Win32 Vulkan surface creation";
    const VkWin32SurfaceCreateInfoKHR loss_surface_info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleA(NULL),
        .hwnd = window
    };
    surface = VK_NULL_HANDLE;
    if (create_surface(instance, &loss_surface_info, NULL, &surface) != VK_SUCCESS ||
        surface == VK_NULL_HANDLE) goto cleanup;
    stage = "loss-case valid 64x64 chain creation";
    chain_info = wsi_lifetime_chain_info(surface, VK_NULL_HANDLE);
    if (create_swapchain(device, &chain_info, NULL, &chain) != VK_SUCCESS ||
        chain == VK_NULL_HANDLE) goto cleanup;
    stage = "native HWND destruction before querying surface";
    /* A failed DestroyWindow keeps the owner so cleanup retries it. After success the owner
     * is cleared at once, so cleanup can never destroy a stale or reused HWND value; the old
     * value survives only in destroyed_window for the existing invalidity checks below. */
    const HWND destroyed_window = window;
    if (!DestroyWindow(destroyed_window)) goto cleanup;
    window = NULL;
    RECT lost_client = {0};
    if (IsWindow(destroyed_window) || GetClientRect(destroyed_window, &lost_client)) goto cleanup;
    stage = "lost-surface acquire result and unchanged output index";
    unchanged_index = UINT32_C(0x5a5a5a5a);
    if (acquire(device, chain, 0, acquired, VK_NULL_HANDLE, &unchanged_index) !=
            VK_ERROR_SURFACE_LOST_KHR || unchanged_index != UINT32_C(0x5a5a5a5a)) goto cleanup;
    stage = "valid replacement after HWND destruction retires old chain and loses surface";
    chain_info = wsi_lifetime_chain_info(surface, chain);
    replacement = VK_NULL_HANDLE;
    if (create_swapchain(device, &chain_info, NULL, &replacement) !=
            VK_ERROR_SURFACE_LOST_KHR || replacement != VK_NULL_HANDLE) goto cleanup;
    stage = "retired old chain takes precedence after observed surface loss";
    unchanged_index = UINT32_C(0x5a5a5a5a);
    if (acquire(device, chain, 0, acquired, VK_NULL_HANDLE, &unchanged_index) !=
            VK_ERROR_OUT_OF_DATE_KHR || unchanged_index != UINT32_C(0x5a5a5a5a)) goto cleanup;
    printf("Native WSI lifetime case=destroyed-HWND-surface-loss-and-retirement-precedence PASS\n");
    fflush(stdout);
    stage = "loss-case explicit chain and surface teardown";
    if (wait_idle(device) != VK_SUCCESS) goto cleanup;
    destroy_swapchain(device, chain, NULL);
    chain = VK_NULL_HANDLE;
    destroy_surface(instance, surface, NULL);
    surface = VK_NULL_HANDLE;
    result = 0;

cleanup:
    if (result) (void)wsi_lifetime_failure("native HWND lifetime", stage);
    if (wait_idle) (void)wait_idle(device);
    if (replacement != VK_NULL_HANDLE && destroy_swapchain) {
        destroy_swapchain(device, replacement, NULL);
        replacement = VK_NULL_HANDLE;
    }
    if (chain != VK_NULL_HANDLE && destroy_swapchain) {
        destroy_swapchain(device, chain, NULL);
        chain = VK_NULL_HANDLE;
    }
    if (surface != VK_NULL_HANDLE && destroy_surface) {
        destroy_surface(instance, surface, NULL);
        surface = VK_NULL_HANDLE;
    }
    if (rendered != VK_NULL_HANDLE && destroy_semaphore) {
        destroy_semaphore(device, rendered, NULL);
        rendered = VK_NULL_HANDLE;
    }
    if (acquired != VK_NULL_HANDLE && destroy_semaphore) {
        destroy_semaphore(device, acquired, NULL);
        acquired = VK_NULL_HANDLE;
    }
    if (window) {
        if (!DestroyWindow(window)) result = 1;
        window = NULL;
    }
    const DWORD gdi_after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user_after = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (gdi_after != gdi_before || user_after != user_before) {
        fprintf(stderr, "Native WSI lifetime retained GDI/USER owner: GDI=%lu->%lu USER=%lu->%lu\n",
            (unsigned long)gdi_before, (unsigned long)gdi_after,
            (unsigned long)user_before, (unsigned long)user_after);
        result = 1;
    }
    return result;
}
