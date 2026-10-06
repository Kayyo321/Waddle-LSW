# Selective milestones retain the full default stress suite and common device
# baseline; unknown workload names fail before creating any native resources.
build/vgpu_presented_worker_sanitized: $(VgpuPresentedWorkerSources) $(VgpuPresentedWorkerObjects) build/waddle_vgpu_worker_sanitized
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu -Isubmodules/venus_protocol/include $(VgpuPresentedWorkerSources) $(VgpuPresentedWorkerObjects) -o $@

build/vgpu_loader_worker_sanitized: $(VgpuPresentedWorkerSources) $(VgpuPresentedWorkerObjects) build/waddle_vgpu_worker_sanitized build/libwaddle_vulkan_experimental.so build/waddle_vulkan_experimental.json
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -DVgpuIcdLoader -Isrc/vgpu -Isubmodules/venus_protocol/include $(VgpuPresentedWorkerSources) $(VgpuPresentedWorkerObjects) -ldl -o $@

.PHONY: vgpu-image-worker-test vgpu-image-worker-sanitizers
vgpu-image-worker-test: build/vgpu_presented_worker_test build/vgpu_loader_worker_test vgpu-native-loader
	WADDLE_TEST_WORKLOAD=image RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_test
	WADDLE_TEST_WORKLOAD=image WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader/loader/libvulkan.so.1" RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_test

vgpu-image-worker-sanitizers: build/vgpu_presented_worker_sanitized build/vgpu_loader_worker_sanitized vgpu-native-loader-sanitizers
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_WORKLOAD=image WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_WORKLOAD=image WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader_sanitized/loader/libvulkan.so.1" WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_sanitized
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_LOADER_FAILURE=1 WADDLE_TEST_WORKLOAD=image WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader_sanitized/loader/libvulkan.so.1" WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_sanitized; status=$$?; test $$status -eq 1

# ComputeShader is owned source-derived fixture data; changing it rebuilds callers.
build/vgpu_presented_worker_test build/vgpu_loader_worker_test build/vgpu_presented_worker_sanitized build/vgpu_loader_worker_sanitized: tests/vgpu/shaders/compute_shader.h

.PHONY: vgpu-compute-worker-test vgpu-compute-worker-sanitizers
vgpu-compute-worker-test: build/vgpu_presented_worker_test build/vgpu_loader_worker_test vgpu-native-loader
	WADDLE_TEST_WORKLOAD=compute RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_test
	WADDLE_TEST_WORKLOAD=compute WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader/loader/libvulkan.so.1" RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_test

vgpu-compute-worker-sanitizers: build/vgpu_presented_worker_sanitized build/vgpu_loader_worker_sanitized vgpu-native-loader-sanitizers
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_WORKLOAD=compute WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_WORKLOAD=compute WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader_sanitized/loader/libvulkan.so.1" WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_sanitized
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_LOADER_FAILURE=1 WADDLE_TEST_WORKLOAD=compute WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader_sanitized/loader/libvulkan.so.1" WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_sanitized; status=$$?; test $$status -eq 1

build/vgpu_presented_worker_test build/vgpu_loader_worker_test build/vgpu_presented_worker_sanitized build/vgpu_loader_worker_sanitized: tests/vgpu/shaders/compute_push_shader.h

.PHONY: vgpu-compute-push-worker-test vgpu-compute-push-worker-sanitizers
vgpu-compute-push-worker-test: build/vgpu_presented_worker_test build/vgpu_loader_worker_test vgpu-native-loader
	WADDLE_TEST_WORKLOAD=compute_push RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_test
	WADDLE_TEST_WORKLOAD=compute_push WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader/loader/libvulkan.so.1" RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_test

vgpu-compute-push-worker-sanitizers: build/vgpu_presented_worker_sanitized build/vgpu_loader_worker_sanitized vgpu-native-loader-sanitizers
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_WORKLOAD=compute_push WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_WORKLOAD=compute_push WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader_sanitized/loader/libvulkan.so.1" WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_sanitized
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_TEST_LOADER_FAILURE=1 WADDLE_TEST_WORKLOAD=compute_push WADDLE_TEST_VULKAN_LOADER="$(CURDIR)/build/vendor/vulkan_loader_sanitized/loader/libvulkan.so.1" WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_loader_worker_sanitized; status=$$?; test $$status -eq 1
