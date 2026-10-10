# Portable production WSI lifetime tests. Native API doubles do not qualify Windows.
VgpuWsiLifecycleSources = tests/vgpu/win32_present_portable.c src/vgpu/venus_win32_present.c
VgpuWsiLifecycleHeaders = include/waddle/venus_win32_present.h tests/vgpu/win32_fake/windows.h tests/vgpu/win32_fake/dwmapi.h
VgpuWsiLifecycleIncludes = -Iinclude -Itests/vgpu/win32_fake

build/wsi-lifecycle: | build
	mkdir -p $@

build/wsi-lifecycle/native: $(VgpuWsiLifecycleSources) $(VgpuWsiLifecycleHeaders) | build/wsi-lifecycle
	$(CC) $(CFLAGS) -D_WIN32 $(VgpuWsiLifecycleIncludes) $(VgpuWsiLifecycleSources) -o $@

build/wsi-lifecycle/unsupported: $(VgpuWsiLifecycleSources) $(VgpuWsiLifecycleHeaders) | build/wsi-lifecycle
	$(CC) $(CFLAGS) $(VgpuWsiLifecycleIncludes) $(VgpuWsiLifecycleSources) -o $@

.PHONY: vgpu-wsi-lifecycle-test vgpu-wsi-lifecycle-sanitizers vgpu-wsi-lifecycle-coverage
vgpu-wsi-lifecycle-test: vgpu-wsi-test build/wsi-lifecycle/native build/wsi-lifecycle/unsupported
	./build/wsi-lifecycle/native
	./build/wsi-lifecycle/unsupported

vgpu-wsi-lifecycle-sanitizers: | build/wsi-lifecycle
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fno-omit-frame-pointer -fsanitize=address,leak,undefined -D_WIN32 $(VgpuWsiLifecycleIncludes) $(VgpuWsiLifecycleSources) -o build/wsi-lifecycle/sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/wsi-lifecycle/sanitized
	python3 tests/vgpu/state_sanitizers.py venus_wsi

vgpu-wsi-lifecycle-coverage: vgpu-protocol
	python3 tests/av/coverage.py venus_wsi
	python3 tests/vgpu/wsi_lifecycle_coverage.py
