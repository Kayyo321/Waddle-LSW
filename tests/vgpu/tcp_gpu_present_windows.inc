/** @file tcp_gpu_present_windows.inc @brief Actual native Vulkan swapchain pixels and owners.
 * Shared dispatcher/math helpers and immutable original offscreen probes are supplied
 * by tcp_gpu_windows.c. All additions below are sole-thread native fixture functions.
 */
/** @brief Render a real red triangle on a borrowed BGRA swapchain image.
 * @param[in] device/queue/family/memory/dispatcher Live borrowed Vulkan owners.
 * @param[in] borrowed_image Swapchain image, never destroyed by this helper.
 * @param[in] acquired/rendered Borrowed binary semaphores: consume/signal on actual GPU.
 * @param[out] readback_output Borrowed16384-byte exact GPU BGRA image, valid on success.
 * @return Zero after fence/queue completion, invalidate and actual red/blue samples.
 * @note Sole fixture thread. Local allocations and commands freed on every path;
 * caller retains all borrowed owners until matching GPU completion/retirement.
 */
static int swapchain_triangle_render(VkDevice device, VkQueue queue, uint32_t family,
    const VkPhysicalDeviceMemoryProperties *supported_memory, PFN_vkGetDeviceProcAddr device_proc, VkImage borrowed_image, VkSemaphore acquired, VkSemaphore rendered, unsigned char *readback_output) {
    PFN_vkCreateImageView create_image_view = (PFN_vkCreateImageView)compute_proc(device, device_proc, "vkCreateImageView");
    if (!create_image_view) return 1;
    PFN_vkDestroyImageView destroy_image_view = (PFN_vkDestroyImageView)compute_proc(device, device_proc, "vkDestroyImageView");
    if (!destroy_image_view) return 1;
    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)compute_proc(device, device_proc, "vkCreateBuffer");
    if (!create_buffer) return 1;
    PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)compute_proc(device, device_proc, "vkDestroyBuffer");
    if (!destroy_buffer) return 1;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = (PFN_vkGetBufferMemoryRequirements)compute_proc(device, device_proc, "vkGetBufferMemoryRequirements");
    if (!get_buffer_memory_requirements) return 1;
    PFN_vkBindBufferMemory bind_buffer_memory = (PFN_vkBindBufferMemory)compute_proc(device, device_proc, "vkBindBufferMemory");
    if (!bind_buffer_memory) return 1;
    PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)compute_proc(device, device_proc, "vkAllocateMemory");
    if (!allocate_memory) return 1;
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)compute_proc(device, device_proc, "vkFreeMemory");
    if (!free_memory) return 1;
    PFN_vkMapMemory map_memory = (PFN_vkMapMemory)compute_proc(device, device_proc, "vkMapMemory");
    if (!map_memory) return 1;
    PFN_vkUnmapMemory unmap_memory = (PFN_vkUnmapMemory)compute_proc(device, device_proc, "vkUnmapMemory");
    if (!unmap_memory) return 1;
    PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = (PFN_vkFlushMappedMemoryRanges)compute_proc(device, device_proc, "vkFlushMappedMemoryRanges");
    if (!flush_mapped_memory_ranges) return 1;
    PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = (PFN_vkInvalidateMappedMemoryRanges)compute_proc(device, device_proc, "vkInvalidateMappedMemoryRanges");
    if (!invalidate_mapped_memory_ranges) return 1;
    PFN_vkCreateShaderModule create_shader_module = (PFN_vkCreateShaderModule)compute_proc(device, device_proc, "vkCreateShaderModule");
    if (!create_shader_module) return 1;
    PFN_vkDestroyShaderModule destroy_shader_module = (PFN_vkDestroyShaderModule)compute_proc(device, device_proc, "vkDestroyShaderModule");
    if (!destroy_shader_module) return 1;
    PFN_vkCreatePipelineLayout create_pipeline_layout = (PFN_vkCreatePipelineLayout)compute_proc(device, device_proc, "vkCreatePipelineLayout");
    if (!create_pipeline_layout) return 1;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = (PFN_vkDestroyPipelineLayout)compute_proc(device, device_proc, "vkDestroyPipelineLayout");
    if (!destroy_pipeline_layout) return 1;
    PFN_vkCreateRenderPass create_render_pass = (PFN_vkCreateRenderPass)compute_proc(device, device_proc, "vkCreateRenderPass");
    if (!create_render_pass) return 1;
    PFN_vkDestroyRenderPass destroy_render_pass = (PFN_vkDestroyRenderPass)compute_proc(device, device_proc, "vkDestroyRenderPass");
    if (!destroy_render_pass) return 1;
    PFN_vkCreateFramebuffer create_framebuffer = (PFN_vkCreateFramebuffer)compute_proc(device, device_proc, "vkCreateFramebuffer");
    if (!create_framebuffer) return 1;
    PFN_vkDestroyFramebuffer destroy_framebuffer = (PFN_vkDestroyFramebuffer)compute_proc(device, device_proc, "vkDestroyFramebuffer");
    if (!destroy_framebuffer) return 1;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = (PFN_vkCreateGraphicsPipelines)compute_proc(device, device_proc, "vkCreateGraphicsPipelines");
    if (!create_graphics_pipelines) return 1;
    PFN_vkDestroyPipeline destroy_pipeline = (PFN_vkDestroyPipeline)compute_proc(device, device_proc, "vkDestroyPipeline");
    if (!destroy_pipeline) return 1;
    PFN_vkCreateCommandPool create_command_pool = (PFN_vkCreateCommandPool)compute_proc(device, device_proc, "vkCreateCommandPool");
    if (!create_command_pool) return 1;
    PFN_vkDestroyCommandPool destroy_command_pool = (PFN_vkDestroyCommandPool)compute_proc(device, device_proc, "vkDestroyCommandPool");
    if (!destroy_command_pool) return 1;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = (PFN_vkAllocateCommandBuffers)compute_proc(device, device_proc, "vkAllocateCommandBuffers");
    if (!allocate_command_buffers) return 1;
    PFN_vkBeginCommandBuffer begin_command_buffer = (PFN_vkBeginCommandBuffer)compute_proc(device, device_proc, "vkBeginCommandBuffer");
    if (!begin_command_buffer) return 1;
    PFN_vkEndCommandBuffer end_command_buffer = (PFN_vkEndCommandBuffer)compute_proc(device, device_proc, "vkEndCommandBuffer");
    if (!end_command_buffer) return 1;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = (PFN_vkCmdBindPipeline)compute_proc(device, device_proc, "vkCmdBindPipeline");
    if (!cmd_bind_pipeline) return 1;
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass = (PFN_vkCmdBeginRenderPass)compute_proc(device, device_proc, "vkCmdBeginRenderPass");
    if (!cmd_begin_render_pass) return 1;
    PFN_vkCmdEndRenderPass cmd_end_render_pass = (PFN_vkCmdEndRenderPass)compute_proc(device, device_proc, "vkCmdEndRenderPass");
    if (!cmd_end_render_pass) return 1;
    PFN_vkCmdDraw cmd_draw = (PFN_vkCmdDraw)compute_proc(device, device_proc, "vkCmdDraw");
    if (!cmd_draw) return 1;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = (PFN_vkCmdPipelineBarrier)compute_proc(device, device_proc, "vkCmdPipelineBarrier");
    if (!cmd_pipeline_barrier) return 1;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = (PFN_vkCmdCopyImageToBuffer)compute_proc(device, device_proc, "vkCmdCopyImageToBuffer");
    if (!cmd_copy_image_to_buffer) return 1;
    PFN_vkCreateFence create_fence = (PFN_vkCreateFence)compute_proc(device, device_proc, "vkCreateFence");
    if (!create_fence) return 1;
    PFN_vkDestroyFence destroy_fence = (PFN_vkDestroyFence)compute_proc(device, device_proc, "vkDestroyFence");
    if (!destroy_fence) return 1;
    PFN_vkQueueSubmit queue_submit = (PFN_vkQueueSubmit)compute_proc(device, device_proc, "vkQueueSubmit");
    if (!queue_submit) return 1;
    PFN_vkWaitForFences wait_for_fences = (PFN_vkWaitForFences)compute_proc(device, device_proc, "vkWaitForFences");
    if (!wait_for_fences) return 1;
    PFN_vkQueueWaitIdle queue_wait_idle = (PFN_vkQueueWaitIdle)compute_proc(device, device_proc, "vkQueueWaitIdle");
    if (!queue_wait_idle) return 1;
    int result = 1, submitted = 0;
    const char *stage = "image acquisition";
    VkImage image = borrowed_image;
    VkDeviceMemory buffer_memory = NULL;
    VkImageView view = NULL;
    VkBuffer buffer = NULL;
    void *mapped = NULL;
    VkShaderModule vertex = NULL, fragment = NULL;
    VkPipelineLayout pipeline_layout = NULL;
    VkRenderPass render_pass = NULL;
    VkFramebuffer framebuffer = NULL;
    VkPipeline pipeline = NULL;
    VkCommandPool command_pool = NULL;
    VkFence fence = NULL;
    const VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
    if (create_image_view(device, &view_info, NULL, &view) != VK_SUCCESS || !view) goto cleanup;
    stage = "noncoherent readback buffer acquisition";
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16384, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (create_buffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS || !buffer) goto cleanup;
    VkMemoryRequirements buffer_requirements = {0};
    get_buffer_memory_requirements(device, buffer, &buffer_requirements);
    uint32_t buffer_type = VK_MAX_MEMORY_TYPES;
    for (uint32_t index = 0; index < supported_memory->memoryTypeCount; index++) {
        const VkMemoryPropertyFlags flags = supported_memory->memoryTypes[index].propertyFlags;
        if ((buffer_requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            buffer_type = index; break;
        }
    }
    if (buffer_requirements.size < 16384 || buffer_type == VK_MAX_MEMORY_TYPES) goto cleanup;
    const VkMemoryAllocateInfo buffer_allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_requirements.size, .memoryTypeIndex = buffer_type};
    if (allocate_memory(device, &buffer_allocation, NULL, &buffer_memory) != VK_SUCCESS || !buffer_memory ||
        bind_buffer_memory(device, buffer, buffer_memory, 0) != VK_SUCCESS ||
        map_memory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) goto cleanup;
    memset(mapped, 0xa5, 16384);
    const VkMappedMemoryRange mapped_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = buffer_memory, .size = VK_WHOLE_SIZE};
    stage = "triangle mapped poison and flush";
    if (flush_mapped_memory_ranges(device, 1, &mapped_range) != VK_SUCCESS) goto cleanup;
#ifdef VgpuIcdLoader
    if (getenv("WADDLE_TEST_LOADER_FAILURE")) goto cleanup;
#endif
    stage = "shader and render pass acquisition";
    const VkShaderModuleCreateInfo vertex_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(TriangleVertexShader), .pCode = TriangleVertexShader};
    const VkShaderModuleCreateInfo fragment_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(TriangleFragmentShader), .pCode = TriangleFragmentShader};
    if (create_shader_module(device, &vertex_info, NULL, &vertex) != VK_SUCCESS || !vertex ||
        create_shader_module(device, &fragment_info, NULL, &fragment) != VK_SUCCESS || !fragment) goto cleanup;
    const VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (create_pipeline_layout(device, &layout_info, NULL, &pipeline_layout) != VK_SUCCESS || !pipeline_layout) goto cleanup;
    const VkAttachmentDescription attachment = {.format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkAttachmentReference color = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    const VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    if (create_render_pass(device, &pass_info, NULL, &render_pass) != VK_SUCCESS || !render_pass) goto cleanup;
    const VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass, .attachmentCount = 1, .pAttachments = &view, .width = 64, .height = 64, .layers = 1};
    if (create_framebuffer(device, &framebuffer_info, NULL, &framebuffer) != VK_SUCCESS || !framebuffer) goto cleanup;
    stage = "graphics pipeline acquisition";
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment, .pName = "main"}};
    const VkPipelineVertexInputStateCreateInfo vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkViewport viewport = {.width = 64, .height = 64, .maxDepth = 1};
    const VkRect2D scissor = {.extent = {64, 64}};
    const VkPipelineViewportStateCreateInfo viewport_state = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const VkPipelineRasterizationStateCreateInfo rasterization = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    const VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state, .pRasterizationState = &rasterization, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = pipeline_layout, .renderPass = render_pass, .basePipelineIndex = -1};
    if (create_graphics_pipelines(device, NULL, 1, &pipeline_info, NULL, &pipeline) != VK_SUCCESS || !pipeline) goto cleanup;
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    if (create_command_pool(device, &pool_info, NULL, &command_pool) != VK_SUCCESS || !command_pool) goto cleanup;
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command = NULL;
    if (allocate_command_buffers(device, &command_info, &command) != VK_SUCCESS || !command) goto cleanup;
    const VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (begin_command_buffer(command, &begin_info) != VK_SUCCESS) goto cleanup;
    stage = "triangle command recording";
    const VkClearValue clear = {.color = {.float32 = {0, 0, 1, 1}}};
    const VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass, .framebuffer = framebuffer, .renderArea = {.extent = {64, 64}},
        .clearValueCount = 1, .pClearValues = &clear};
    cmd_bind_pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    cmd_begin_render_pass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    cmd_draw(command, 3, 1, 0, 0);
    cmd_end_render_pass(command);
    const VkImageMemoryBarrier image_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
    cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_barrier);
    const VkBufferImageCopy region = {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .imageExtent = {64, 64, 1}};
    cmd_copy_image_to_buffer(command, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    const VkBufferMemoryBarrier buffer_barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .size = 16384};
    cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &buffer_barrier, 0, NULL);
    const VkImageMemoryBarrier present_barrier = {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
        .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&present_barrier);
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (end_command_buffer(command) != VK_SUCCESS ||
        create_fence(device, &fence_info, NULL, &fence) != VK_SUCCESS || !fence) goto cleanup;
    const VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acquired,
        .pWaitDstStageMask=&wait_stage,.signalSemaphoreCount=1,.pSignalSemaphores=&rendered,.commandBufferCount=1,.pCommandBuffers=&command};
    stage = "triangle submission";
    if (queue_submit(queue, 1, &submit, fence) != VK_SUCCESS) goto cleanup;
    submitted = 1;
    if (wait_for_fences(device, 1, &fence, VK_TRUE, UINT64_C(1000000000)) != VK_SUCCESS ||
        queue_wait_idle(queue) != VK_SUCCESS) goto cleanup;
    submitted = 0;
    stage = "triangle mapped RGBA comparison";
    if (invalidate_mapped_memory_ranges(device, 1, &mapped_range) != VK_SUCCESS) goto cleanup;
    const unsigned char *pixels = mapped;
    const size_t sample_offsets[2] = {(32 * 64 + 32) * 4, 0};
    const unsigned char expected[2][4] = {{0, 0, 255, 255}, {255, 0, 0, 255}};
    for (size_t sample = 0; sample < 2; sample++) {
        if (memcmp(pixels + sample_offsets[sample], expected[sample], 4)) {
            fprintf(stderr, "ICD triangle sample %zu: got [%u,%u,%u,%u], expected [%u,%u,%u,%u]\n", sample,
                pixels[sample_offsets[sample]], pixels[sample_offsets[sample] + 1],
                pixels[sample_offsets[sample] + 2], pixels[sample_offsets[sample] + 3],
                expected[sample][0], expected[sample][1], expected[sample][2], expected[sample][3]);
            goto cleanup;
        }
    }
    memcpy(readback_output,mapped,16384);
    result = 0;
cleanup:
    if (result) fprintf(stderr, "ICD triangle acceptance failed: %s\n", stage);
    if (submitted) (void)queue_wait_idle(queue);
    if (fence) { destroy_fence(device, fence, NULL); fence = NULL; }
    if (command_pool) { destroy_command_pool(device, command_pool, NULL); command_pool = NULL; }
    if (pipeline) { destroy_pipeline(device, pipeline, NULL); pipeline = NULL; }
    if (framebuffer) { destroy_framebuffer(device, framebuffer, NULL); framebuffer = NULL; }
    if (render_pass) { destroy_render_pass(device, render_pass, NULL); render_pass = NULL; }
    if (pipeline_layout) { destroy_pipeline_layout(device, pipeline_layout, NULL); pipeline_layout = NULL; }
    if (fragment) { destroy_shader_module(device, fragment, NULL); fragment = NULL; }
    if (vertex) { destroy_shader_module(device, vertex, NULL); vertex = NULL; }
    if (mapped) { unmap_memory(device, buffer_memory); mapped = NULL; }
    if (buffer) { destroy_buffer(device, buffer, NULL); buffer = NULL; }
    if (buffer_memory) { free_memory(device, buffer_memory, NULL); buffer_memory = NULL; }
    if (view) { destroy_image_view(device, view, NULL); view = NULL; }
    image = NULL; /* Borrowed swapchain image is released only by destroy_swapchain. */
    return result;
}

/** @brief Initialize independent Win32 window/DC dependencies before native baselines.
 * @return Zero only after all temporary native owners are released.
 * @note Sole fixture thread; no Vulkan/project-owned object created or retained.
 */
static int initialize_presentation_windows(void)
{
    DWORD gdi_before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),user_before=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    HWND window=CreateWindowExA(0,"STATIC","Waddle presentation initialization",WS_POPUP|WS_VISIBLE,100,100,64,64,NULL,NULL,GetModuleHandleA(NULL),NULL);
    if(!window)return 1;
    (void)ShowWindow(window,SW_SHOW);(void)UpdateWindow(window);
    if(DwmFlush()!=S_OK){(void)DestroyWindow(window);return 1;}
    HDC dc=GetDC(window);int result=!dc;
    if(dc)(void)GetPixel(dc,0,0);
    if(dc && !ReleaseDC(window,dc))result=1;
    if(!DestroyWindow(window))result=1;
    printf("Independent visible Win32 initialization released HWND/DC: GDI=%lu->%lu USER=%lu->%lu\n",(unsigned long)gdi_before,(unsigned long)GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),(unsigned long)user_before,(unsigned long)GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS));fflush(stdout);return result;
}
/** @brief Execute actual native surface/swapchain/render/present/recreation lifecycle.
 * @param[in] instance/physical/device/queue/family/memory/dispatchers Live borrowed owners.
 * @param[in] smoke Nonzero selects one frame/mode; zero selects three frames/mode.
 * @return Zero after2swapchain modes with actual acquires/render/presents, every4096
 * client RGB pixel equals independently invalidated GPU BGRA readback and teardown.
 * @note Sole desktop fixture thread. Function owns HWND/surface/chains/semaphores/DC.
 * Matching queue/device completion precedes resource release; caller preserves host
 * session on transport loss through existing trusted retirement receipt handling.
 */
static int native_swapchain_probe(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,
    const VkPhysicalDeviceMemoryProperties *memory,PFN_vkGetInstanceProcAddr lookup,PFN_vkGetDeviceProcAddr device_proc,int smoke)
{
    PFN_vkCreateWin32SurfaceKHR create_surface=(PFN_vkCreateWin32SurfaceKHR)lookup(instance,"vkCreateWin32SurfaceKHR");
    PFN_vkDestroySurfaceKHR destroy_surface=(PFN_vkDestroySurfaceKHR)lookup(instance,"vkDestroySurfaceKHR");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surface_support=(PFN_vkGetPhysicalDeviceSurfaceSupportKHR)lookup(instance,"vkGetPhysicalDeviceSurfaceSupportKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities=(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)lookup(instance,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkCreateSwapchainKHR create_swapchain=(PFN_vkCreateSwapchainKHR)compute_proc(device,device_proc,"vkCreateSwapchainKHR");
    PFN_vkDestroySwapchainKHR destroy_swapchain=(PFN_vkDestroySwapchainKHR)compute_proc(device,device_proc,"vkDestroySwapchainKHR");
    PFN_vkGetSwapchainImagesKHR images=(PFN_vkGetSwapchainImagesKHR)compute_proc(device,device_proc,"vkGetSwapchainImagesKHR");
    PFN_vkAcquireNextImageKHR acquire=(PFN_vkAcquireNextImageKHR)compute_proc(device,device_proc,"vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR present=(PFN_vkQueuePresentKHR)compute_proc(device,device_proc,"vkQueuePresentKHR");
    PFN_vkCreateSemaphore create_semaphore=(PFN_vkCreateSemaphore)compute_proc(device,device_proc,"vkCreateSemaphore");
    PFN_vkDestroySemaphore destroy_semaphore=(PFN_vkDestroySemaphore)compute_proc(device,device_proc,"vkDestroySemaphore");
    PFN_vkDeviceWaitIdle idle=(PFN_vkDeviceWaitIdle)compute_proc(device,device_proc,"vkDeviceWaitIdle");
    if(!create_surface || !destroy_surface || !surface_support || !capabilities || !create_swapchain || !destroy_swapchain || !images || !acquire || !present || !create_semaphore || !destroy_semaphore || !idle)return 1;
    DWORD gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    HWND window=NULL;HDC dc=NULL;VkSurfaceKHR surface=NULL;VkSwapchainKHR chain=NULL,old_chain=NULL;VkSemaphore acquired=NULL,rendered=NULL;
    int result=1;const char *stage="native window acquisition";
    window=CreateWindowExA(0,"STATIC","Waddle actual Vulkan presentation",WS_POPUP|WS_VISIBLE,100,100,64,64,NULL,NULL,GetModuleHandleA(NULL),NULL);
    if(!window || !SetWindowPos(window,HWND_TOPMOST,100,100,64,64,SWP_SHOWWINDOW))goto cleanup;
    (void)ShowWindow(window,SW_SHOW);(void)UpdateWindow(window);
    const VkWin32SurfaceCreateInfoKHR surface_info={.sType=VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,.hinstance=GetModuleHandleA(NULL),.hwnd=window};
    stage="actual Win32 surface ownership";if(create_surface(instance,&surface_info,NULL,&surface)!=VK_SUCCESS || !surface)goto cleanup;
    VkBool32 supported=VK_FALSE;if(surface_support(physical,family,surface,&supported)!=VK_SUCCESS || !supported)goto cleanup;
    VkSurfaceCapabilitiesKHR caps={0};if(capabilities(physical,surface,&caps)!=VK_SUCCESS || caps.currentExtent.width!=64 || caps.currentExtent.height!=64 || caps.minImageCount>2 || caps.maxImageCount<2)goto cleanup;
    const VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if(create_semaphore(device,&sem_info,NULL,&acquired)!=VK_SUCCESS || !acquired || create_semaphore(device,&sem_info,NULL,&rendered)!=VK_SUCCESS || !rendered)goto cleanup;
    unsigned char readback[16384];
    for(unsigned mode=0;mode<2;mode++) {
        const VkSwapchainCreateInfoKHR chain_info={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surface,.minImageCount=2,
            .imageFormat=VK_FORMAT_B8G8R8A8_UNORM,.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,.imageExtent={64,64},.imageArrayLayers=1,
            .imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
            .preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode=mode ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE,.oldSwapchain=old_chain};
        stage="real bound swapchain image acquisition/recreation";if(create_swapchain(device,&chain_info,NULL,&chain)!=VK_SUCCESS || !chain)goto cleanup;
        if(old_chain){destroy_swapchain(device,old_chain,NULL);old_chain=NULL;}
        uint32_t image_count=0;if(images(device,chain,&image_count,NULL)!=VK_SUCCESS || image_count!=2)goto cleanup;
        VkImage image_handles[3]={0};uint32_t capacity=3;if(images(device,chain,&capacity,image_handles)!=VK_SUCCESS || capacity!=image_count || !image_handles[0] || !image_handles[1] || image_handles[0]==image_handles[1])goto cleanup;
        for(unsigned frame=0;frame<(smoke ? 1u : 3u);frame++) {
            uint32_t index=UINT32_MAX;stage="real acquire binary semaphore signal";
            if(acquire(device,chain,UINT64_C(1000000000),acquired,NULL,&index)!=VK_SUCCESS || index>=image_count)goto cleanup;
            stage="real GPU swapchain triangle and complete BGRA readback";
            if(swapchain_triangle_render(device,queue,family,memory,device_proc,image_handles[index],acquired,rendered,readback))goto cleanup;
            VkResult chain_result=VK_ERROR_UNKNOWN;const VkPresentInfoKHR present_info={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&rendered,.swapchainCount=1,.pSwapchains=&chain,.pImageIndices=&index,.pResults=&chain_result};
            stage="actual swapchain presentation";if(present(queue,&present_info)!=VK_SUCCESS || chain_result!=VK_SUCCESS || idle(device)!=VK_SUCCESS || DwmFlush()!=S_OK)goto cleanup;
            dc=GetDC(window);if(!dc)goto cleanup;
            stage="all4096 actual client RGB pixels match GPU BGRA image";
            for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
                const unsigned char *pixel=readback+(y*64+x)*4;
                COLORREF expected=RGB(pixel[2],pixel[1],pixel[0]),actual=GetPixel(dc,(int)x,(int)y);
                if(pixel[3]!=255 || actual!=expected){fprintf(stderr,"Native Vulkan present pixel(%u,%u): actual=%08lx expected=%08lx alpha=%u\n",x,y,(unsigned long)actual,(unsigned long)expected,pixel[3]);goto cleanup;}
            }
            if(!ReleaseDC(window,dc))goto cleanup;dc=NULL;
            printf("Native Vulkan actual swapchain mode=%s frame=%u exact4096RGB/BGRA PASS\n",mode ? "IMMEDIATE" : "FIFO",frame+1);fflush(stdout);
        }
        old_chain=chain;chain=NULL;
    }
    result=0;
cleanup:
    if(result)fprintf(stderr,"Native Vulkan swapchain failed: %s\n",stage);
    (void)idle(device);
    if(dc){if(!ReleaseDC(window,dc))result=1;dc=NULL;}
    if(chain){destroy_swapchain(device,chain,NULL);chain=NULL;}
    if(old_chain){destroy_swapchain(device,old_chain,NULL);old_chain=NULL;}
    if(rendered){destroy_semaphore(device,rendered,NULL);rendered=NULL;}
    if(acquired){destroy_semaphore(device,acquired,NULL);acquired=NULL;}
    if(surface){destroy_surface(instance,surface,NULL);surface=NULL;}
    if(window){if(!DestroyWindow(window))result=1;window=NULL;}
    DWORD gdi_after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),user_after=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    if(gdi_after!=gdi || user_after!=user){fprintf(stderr,"Native Vulkan swapchain retained GDI/USER owner: gdi=%lu->%lu user=%lu->%lu\n",(unsigned long)gdi,(unsigned long)gdi_after,(unsigned long)user,(unsigned long)user_after);result=1;}
    return result;
}
