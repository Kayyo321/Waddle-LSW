# Allocation-free Venus transport; no renderer dependencies for these checks.
.PHONY: vgpu-test vgpu-sanitizers

build/venus_bounds.o: src/vgpu/venus_bounds.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_ring_test: tests/vgpu/ring.c src/vgpu/venus_ring.c src/vgpu/venus_bounds.h include/waddle/venus_ring.h build/venus_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/ring.c src/vgpu/venus_ring.c build/venus_bounds.o $(LDFLAGS) -o $@

build/vgpu_stress_test: tests/vgpu/stress.c src/vgpu/venus_ring.c src/vgpu/venus_bounds.h include/waddle/venus_ring.h build/venus_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/stress.c src/vgpu/venus_ring.c build/venus_bounds.o $(LDFLAGS) -pthread -o $@

vgpu-test: build/vgpu_ring_test build/vgpu_stress_test build/vgpu_region_test build/vgpu_mapping_test build/vgpu_wait_test build/vgpu_session_test build/vgpu_channel_test
	./build/vgpu_ring_test
	./build/vgpu_stress_test
	./build/vgpu_region_test
	./build/vgpu_mapping_test
	./build/vgpu_wait_test
	./build/vgpu_session_test
	./build/vgpu_channel_test
	$(ZIG) test src/vgpu/venus_control.zig
	$(ZIG) test src/vgpu/venus_bounds.zig

vgpu-sanitizers: build/venus_bounds.o build/venus_control.o
	$(CC) $(CPPFLAGS) -Isrc/vgpu -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/ring.c src/vgpu/venus_ring.c build/venus_bounds.o -o build/vgpu_ring_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_ring_sanitized
	$(ZIG) test src/vgpu/venus_bounds.zig
	$(CC) $(CPPFLAGS) -Isrc/vgpu -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/stress.c src/vgpu/venus_ring.c build/venus_bounds.o -pthread -o build/vgpu_stress_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_stress_sanitized
	$(CC) $(CPPFLAGS) -Isrc/vgpu -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/region.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds.o -o build/vgpu_region_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_region_sanitized
	$(CC) $(CPPFLAGS) -Isrc/vgpu -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/mapping.c src/vgpu/venus_mapping_linux.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds.o $(VgpuMappingWrapFlags) -o build/vgpu_mapping_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_mapping_sanitized
	$(CC) $(CPPFLAGS) -Isrc/vgpu -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/wait.c src/vgpu/venus_wait.c src/vgpu/venus_ring.c build/venus_bounds.o -o build/vgpu_wait_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_wait_sanitized
	$(CC) $(CPPFLAGS) -Isrc/vgpu -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/session.c src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control.o build/venus_bounds.o -o build/vgpu_session_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_session_sanitized
	$(CC) $(CPPFLAGS) -Isrc/vgpu $(VgpuReceiverSanitizers) tests/vgpu/channel.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c build/venus_bounds.o build/venus_control.o -pthread $(VgpuChannelWrapFlags) -o build/vgpu_channel_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_channel_sanitized
	$(ZIG) test src/vgpu/venus_control.zig

.PHONY: vgpu-coverage vgpu-windows
vgpu-coverage: build/venus_bounds.o build/venus_control.o
	python3 tests/vgpu/coverage.py
	python3 tests/vgpu/coverage.py region
	python3 tests/vgpu/coverage.py mapping
	python3 tests/vgpu/coverage.py wait
	python3 tests/vgpu/coverage.py session
	python3 tests/vgpu/channel_coverage.py
	python3 tests/av/coverage.py venus_control
	python3 tests/av/coverage.py venus_bounds

build/venus_bounds_windows.lib: src/vgpu/venus_bounds.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_ring_test.exe: tests/vgpu/ring.c src/vgpu/venus_ring.c include/waddle/venus_ring.h src/vgpu/venus_bounds.h build/venus_bounds_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/ring.c src/vgpu/venus_ring.c build/venus_bounds_windows.lib -o $@

vgpu-windows: build/vgpu_ring_test.exe

# Renderer is pinned as a submodule; configure must not fetch anything.
VgpuMappingWrapFlags = -Wl,--wrap=memfd_create,--wrap=ftruncate,--wrap=fcntl,--wrap=mmap,--wrap=venus_region_init,--wrap=venus_region_attach

VgpuRendererDirectory = submodules/virglrenderer
VgpuRendererBuildDirectory = build/vendor/virglrenderer

$(VgpuRendererBuildDirectory)/build.ninja: $(VgpuRendererDirectory)/meson.build $(VgpuRendererDirectory)/meson_options.txt | build
	meson setup $(VgpuRendererBuildDirectory) $(VgpuRendererDirectory) --wrap-mode=nodownload -Dvenus=true -Dplatforms=[] -Dtests=false -Dtracing=none

.PHONY: vgpu-renderer
vgpu-renderer: $(VgpuRendererBuildDirectory)/build.ninja
	meson compile -C $(VgpuRendererBuildDirectory) -j 4

build/vgpu_region_test: tests/vgpu/region.c src/vgpu/venus_region.c src/vgpu/venus_ring.c include/waddle/venus_region.h build/venus_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/region.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds.o $(LDFLAGS) -o $@


build/vgpu_region_test.exe: tests/vgpu/region.c src/vgpu/venus_region.c src/vgpu/venus_ring.c include/waddle/venus_region.h build/venus_bounds_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/region.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds_windows.lib -o $@

vgpu-windows: build/vgpu_region_test.exe

build/vgpu_mapping_test: tests/vgpu/mapping.c src/vgpu/venus_mapping_linux.c src/vgpu/venus_region.c src/vgpu/venus_ring.c include/waddle/venus_mapping.h build/venus_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/mapping.c src/vgpu/venus_mapping_linux.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds.o $(LDFLAGS) $(VgpuMappingWrapFlags) -o $@

build/vgpu_mapping_test.exe: tests/vgpu/mapping_windows.c src/vgpu/venus_mapping_windows.c src/vgpu/venus_driver.h src/vgpu/venus_region.c src/vgpu/venus_ring.c include/waddle/venus_mapping.h build/venus_bounds_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu -Isubmodules/looking_glass/vendor/ivshmem tests/vgpu/mapping_windows.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds_windows.lib -o $@

vgpu-windows: build/vgpu_mapping_test.exe

build/vgpu_wait_test: tests/vgpu/wait.c src/vgpu/venus_wait.c src/vgpu/venus_ring.c include/waddle/venus_wait.h build/venus_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/wait.c src/vgpu/venus_wait.c src/vgpu/venus_ring.c build/venus_bounds.o $(LDFLAGS) -o $@

build/vgpu_wait_test.exe: tests/vgpu/wait.c src/vgpu/venus_wait.c src/vgpu/venus_ring.c include/waddle/venus_wait.h build/venus_bounds_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/wait.c src/vgpu/venus_wait.c src/vgpu/venus_ring.c build/venus_bounds_windows.lib -o $@

vgpu-windows: build/vgpu_wait_test.exe

# Bounded lifecycle codec is independent of the ring metadata codec.
build/venus_control.o: src/vgpu/venus_control.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_session_test: tests/vgpu/session.c src/vgpu/venus_session.c include/waddle/venus_session.h src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control.o build/venus_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/session.c src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control.o build/venus_bounds.o $(LDFLAGS) -o $@

build/venus_control_windows.lib: src/vgpu/venus_control.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_session_test.exe: tests/vgpu/session.c src/vgpu/venus_session.c include/waddle/venus_session.h src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control_windows.lib build/venus_bounds_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/session.c src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control_windows.lib build/venus_bounds_windows.lib -o $@

vgpu-windows: build/vgpu_session_test.exe

# Real public renderer dispatch plus a separately injected boundary ownership test.
VgpuReceiverIncludes = -Isrc/vgpu -I$(VgpuRendererDirectory)/src -I$(VgpuRendererBuildDirectory)/src
VgpuReceiverLibraries = -L$(VgpuRendererBuildDirectory)/src -Wl,-rpath,'$$ORIGIN/vendor/virglrenderer/src' -lvirglrenderer
VgpuReceiverSanitizers = -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer

build/venus_receiver_bounds.o: src/vgpu/venus_receiver_bounds.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_receiver_owner_test: tests/vgpu/receiver_owner.c src/vgpu/venus_receiver.c src/vgpu/venus_receiver_bounds.h include/waddle/venus_receiver.h build/venus_receiver_bounds.o | $(VgpuRendererBuildDirectory)/build.ninja
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/receiver_owner.c build/venus_receiver_bounds.o $(LDFLAGS) -pthread -o $@

build/vgpu_receiver_test: tests/vgpu/receiver.c src/vgpu/venus_receiver.c src/vgpu/venus_receiver_bounds.h include/waddle/venus_receiver.h build/venus_receiver_bounds.o | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/receiver.c src/vgpu/venus_receiver.c build/venus_receiver_bounds.o $(LDFLAGS) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-receiver-test vgpu-receiver-sanitizers vgpu-receiver-coverage
vgpu-receiver-test: build/vgpu_receiver_owner_test build/vgpu_receiver_test
	./build/vgpu_receiver_owner_test
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_receiver_test
	$(ZIG) test src/vgpu/venus_receiver_bounds.zig

vgpu-receiver-sanitizers: build/venus_receiver_bounds.o vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverIncludes) $(VgpuReceiverSanitizers) tests/vgpu/receiver_owner.c build/venus_receiver_bounds.o -pthread -o build/vgpu_receiver_owner_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_receiver_owner_sanitized
	$(CC) $(CPPFLAGS) $(VgpuReceiverIncludes) $(VgpuReceiverSanitizers) tests/vgpu/receiver.c src/vgpu/venus_receiver.c build/venus_receiver_bounds.o $(VgpuReceiverLibraries) -o build/vgpu_receiver_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_receiver_sanitized
	$(ZIG) test src/vgpu/venus_receiver_bounds.zig

vgpu-receiver-coverage: build/venus_receiver_bounds.o | $(VgpuRendererBuildDirectory)/build.ninja
	python3 tests/vgpu/coverage.py receiver
	python3 tests/av/coverage.py venus_receiver_bounds

VgpuChannelSources = src/vgpu/venus_channel.c src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c src/vgpu/venus_wait.c
VgpuChannelWrapFlags = -Wl,--wrap=clock_gettime,--wrap=poll,--wrap=recv,--wrap=send

build/vgpu_channel_test: tests/vgpu/channel.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_stream.h include/waddle/venus_channel.h build/venus_bounds.o build/venus_control.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/channel.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c build/venus_bounds.o build/venus_control.o $(LDFLAGS) -pthread $(VgpuChannelWrapFlags) -o $@

build/vgpu_channel_test.exe: tests/vgpu/channel_windows.c $(VgpuChannelSources) src/vgpu/venus_stream_windows.c src/vgpu/venus_stream.h include/waddle/venus_channel.h build/venus_bounds_windows.lib build/venus_control_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/channel_windows.c $(VgpuChannelSources) src/vgpu/venus_stream_windows.c build/venus_bounds_windows.lib build/venus_control_windows.lib -o $@

vgpu-windows: build/vgpu_channel_test.exe

VgpuRuntimeSources = src/vgpu/venus_rpc.c src/vgpu/venus_dispatch.c
VgpuRuntimeFixtureSources = $(VgpuRuntimeSources) src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c src/vgpu/venus_wait.c

build/vgpu_integration_test: build/venus_capabilities.o include/waddle/venus_capabilities.h build/vgpu_service_fixture build/waddle_vgpu_worker tests/vgpu/integration.c build/venus_capabilities.o src/vgpu/venus_worker.c $(VgpuRuntimeSources) $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c build/venus_bounds.o build/venus_control.o build/venus_request.o | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/integration.c build/venus_capabilities.o src/vgpu/venus_worker.c $(VgpuRuntimeSources) $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c build/venus_bounds.o build/venus_control.o build/venus_request.o $(LDFLAGS) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-integration vgpu-integration-sanitizers
vgpu-integration: build/vgpu_integration_test
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_integration_test

vgpu-integration-sanitizers: build/venus_capabilities.o build/vgpu_service_fixture_sanitized build/waddle_vgpu_worker_sanitized build/venus_bounds.o build/venus_control.o build/venus_request.o vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverIncludes) $(VgpuReceiverSanitizers) tests/vgpu/integration.c build/venus_capabilities.o src/vgpu/venus_worker.c $(VgpuRuntimeSources) $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c build/venus_bounds.o build/venus_control.o build/venus_request.o $(VgpuReceiverLibraries) -o build/vgpu_integration_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_SERVICE_FIXTURE=build/vgpu_service_fixture_sanitized WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_integration_sanitized

# Request codec reuses and includes the exported Zig resource-bound helpers.
# Link this object alone (not also venus_receiver_bounds.o) in dispatch binaries.
build/venus_request.o: src/vgpu/venus_request.zig src/vgpu/venus_receiver_bounds.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_request_test: tests/vgpu/request.c include/waddle/venus_request.h build/venus_request.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/vgpu/request.c build/venus_request.o $(LDFLAGS) -o $@

.PHONY: vgpu-request-test vgpu-request-sanitizers vgpu-request-coverage
vgpu-request-test: build/vgpu_request_test
	./build/vgpu_request_test
	$(ZIG) test src/vgpu/venus_request.zig

vgpu-request-sanitizers: build/venus_request.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/request.c build/venus_request.o -o build/vgpu_request_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_request_sanitized
	$(ZIG) test src/vgpu/venus_request.zig

vgpu-request-coverage:
	python3 tests/av/coverage.py venus_request

build/venus_request_windows.lib: src/vgpu/venus_request.zig src/vgpu/venus_receiver_bounds.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_request_test.exe: tests/vgpu/request.c include/waddle/venus_request.h build/venus_request_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/request.c build/venus_request_windows.lib -o $@

vgpu-windows: build/vgpu_request_test.exe

build/vgpu_runtime_test: tests/vgpu/runtime.c $(VgpuRuntimeFixtureSources) include/waddle/venus_rpc.h include/waddle/venus_dispatch.h build/venus_request.o build/venus_capabilities.o build/venus_bounds.o build/venus_control.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/runtime.c $(VgpuRuntimeFixtureSources) build/venus_request.o build/venus_capabilities.o build/venus_bounds.o build/venus_control.o -o $@

.PHONY: vgpu-runtime-test vgpu-runtime-sanitizers vgpu-runtime-coverage
vgpu-runtime-test: build/vgpu_runtime_test
	./build/vgpu_runtime_test

vgpu-runtime-sanitizers: build/venus_request.o build/venus_bounds.o build/venus_control.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/runtime.c $(VgpuRuntimeFixtureSources) build/venus_request.o build/venus_capabilities.o build/venus_bounds.o build/venus_control.o -o build/vgpu_runtime_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_runtime_sanitized

build/vgpu_runtime_test.exe: tests/vgpu/runtime.c $(VgpuRuntimeFixtureSources) include/waddle/venus_rpc.h include/waddle/venus_dispatch.h build/venus_request_windows.lib build/venus_capabilities_windows.lib build/venus_bounds_windows.lib build/venus_control_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/runtime.c $(VgpuRuntimeFixtureSources) build/venus_request_windows.lib build/venus_capabilities_windows.lib build/venus_bounds_windows.lib build/venus_control_windows.lib -o $@

vgpu-windows: build/vgpu_runtime_test.exe

vgpu-runtime-coverage: build/venus_request.o build/venus_bounds.o build/venus_control.o
	python3 tests/vgpu/runtime_coverage.py

# Header changes must rebuild every consumer, including the integrated renderer.
build/vgpu_integration_test build/vgpu_runtime_test build/vgpu_runtime_test.exe: src/vgpu/venus_rpc_internal.h include/waddle/venus_rpc.h include/waddle/venus_dispatch.h include/waddle/venus_request.h include/waddle/venus_receiver.h

build/vgpu_worker_test: tests/vgpu/worker.c src/vgpu/venus_worker.c include/waddle/venus_worker.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/worker.c -o $@

.PHONY: vgpu-worker-test vgpu-worker-sanitizers vgpu-worker-coverage
vgpu-worker-test: build/vgpu_worker_test
	./build/vgpu_worker_test

vgpu-worker-sanitizers:
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/worker.c -o build/vgpu_worker_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_worker_sanitized

vgpu-worker-coverage: build/venus_bounds.o
	python3 tests/vgpu/coverage.py worker


VgpuServiceSources = src/vgpu/venus_service.c src/vgpu/venus_export.c src/vgpu/venus_frame_linux.c $(VgpuRuntimeSources) $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c
VgpuServiceObjects = build/venus_frame.o build/venus_bounds.o build/venus_control.o build/venus_request.o build/venus_capabilities.o
build/vgpu_service_fixture: tests/vgpu/service_child.c $(VgpuServiceSources) include/waddle/venus_service.h $(VgpuServiceObjects) | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/service_child.c $(VgpuServiceSources) $(VgpuServiceObjects) $(LDFLAGS) $(VgpuReceiverLibraries) -o $@

build/waddle_vgpu_worker: src/vgpu/worker_main.c $(VgpuServiceSources) include/waddle/venus_service.h $(VgpuServiceObjects) | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) src/vgpu/worker_main.c $(VgpuServiceSources) $(VgpuServiceObjects) $(LDFLAGS) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-service
vgpu-service: build/waddle_vgpu_worker

VgpuServiceFixtureSources = src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c src/vgpu/venus_wait.c src/vgpu/venus_rpc.c
build/vgpu_service_owner_test: tests/vgpu/service_owner.c src/vgpu/venus_service.c include/waddle/venus_service.h $(VgpuServiceFixtureSources) $(VgpuServiceObjects)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/service_owner.c $(VgpuServiceFixtureSources) $(VgpuServiceObjects) -o $@

build/vgpu_service_fixture_sanitized: tests/vgpu/service_child.c $(VgpuServiceSources) include/waddle/venus_service.h $(VgpuServiceObjects) | vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) $(VgpuReceiverIncludes) tests/vgpu/service_child.c $(VgpuServiceSources) $(VgpuServiceObjects) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-service-test vgpu-service-sanitizers vgpu-service-coverage
vgpu-service-test: build/vgpu_service_owner_test build/waddle_vgpu_worker
	./build/vgpu_service_owner_test

vgpu-service-sanitizers: $(VgpuServiceObjects)
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/service_owner.c $(VgpuServiceFixtureSources) $(VgpuServiceObjects) -o build/vgpu_service_owner_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_service_owner_sanitized

vgpu-service-coverage: $(VgpuServiceObjects)
	python3 tests/vgpu/coverage.py service

# Service consumers share receiver, transport and framing ABI headers.
build/vgpu_service_fixture build/vgpu_service_fixture_sanitized build/waddle_vgpu_worker build/vgpu_service_owner_test: include/waddle/venus_worker.h include/waddle/venus_receiver.h include/waddle/venus_request.h include/waddle/venus_rpc.h include/waddle/venus_dispatch.h include/waddle/venus_channel.h include/waddle/venus_session.h include/waddle/venus_region.h include/waddle/venus_ring.h src/vgpu/venus_rpc_internal.h src/vgpu/venus_receiver_bounds.h src/vgpu/venus_stream.h

build/waddle_vgpu_worker_sanitized: src/vgpu/worker_main.c $(VgpuServiceSources) $(VgpuServiceObjects) | vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) $(VgpuReceiverIncludes) src/vgpu/worker_main.c $(VgpuServiceSources) $(VgpuServiceObjects) $(VgpuReceiverLibraries) -o $@

build/waddle_vgpu_worker_sanitized: include/waddle/venus_service.h include/waddle/venus_worker.h include/waddle/venus_receiver.h include/waddle/venus_request.h include/waddle/venus_rpc.h include/waddle/venus_dispatch.h include/waddle/venus_channel.h include/waddle/venus_session.h include/waddle/venus_region.h include/waddle/venus_ring.h src/vgpu/venus_rpc_internal.h src/vgpu/venus_receiver_bounds.h src/vgpu/venus_stream.h

build/venus_gpu_fixture.o: tests/vgpu/gpu_queue.zig include/waddle/venus_request.h include/waddle/venus_ring.h include/waddle/venus_values.h | build
	$(ZIG) build-obj $< -Iinclude -Isubmodules/venus_protocol/include -O ReleaseSafe -fPIC -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_gpu_queue_test: tests/vgpu/gpu_queue.c src/vgpu/venus_receiver.c include/waddle/venus_receiver.h build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/gpu_queue.c src/vgpu/venus_receiver.c build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-gpu-test vgpu-gpu-hardware vgpu-gpu-sanitizers
vgpu-gpu-test: build/vgpu_gpu_queue_test
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/build/vendor/virglrenderer/server/virgl_render_server" ./build/vgpu_gpu_queue_test

vgpu-gpu-hardware: build/vgpu_gpu_queue_test
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/build/vendor/virglrenderer/server/virgl_render_server" ./build/vgpu_gpu_queue_test --require-hardware

vgpu-gpu-sanitizers: build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) $(VgpuReceiverIncludes) tests/vgpu/gpu_queue.c src/vgpu/venus_receiver.c build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o $(VgpuReceiverLibraries) -o build/vgpu_gpu_queue_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 RENDER_SERVER_EXEC_PATH="$(CURDIR)/build/vendor/virglrenderer/server/virgl_render_server" ./build/vgpu_gpu_queue_sanitized

build/vgpu_gpu_receiver.o: src/vgpu/venus_receiver.c include/waddle/venus_receiver.h src/vgpu/venus_receiver_bounds.h | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) -c src/vgpu/venus_receiver.c -o $@

.PHONY: vgpu-gpu-unit
vgpu-gpu-unit: build/venus_values.o build/vgpu_gpu_receiver.o build/venus_receiver_bounds.o
	$(ZIG) test tests/vgpu/gpu_queue.zig -Iinclude -Isubmodules/venus_protocol/include build/venus_values.o build/vgpu_gpu_receiver.o build/venus_receiver_bounds.o -lc -Lbuild/vendor/virglrenderer/src -lvirglrenderer -rpath "$(CURDIR)/build/vendor/virglrenderer/src"

# Linux C ABI objects use libc; freestanding weak getauxval must not interpose.
VgpuLibcRuntimeObjects = build/venus_bounds.o build/venus_control.o build/venus_request.o build/venus_capabilities.o
build/vgpu_libc_runtime_test: tests/vgpu/libc_boundary.c $(VgpuLibcRuntimeObjects) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/vgpu/libc_boundary.c $(VgpuLibcRuntimeObjects) -o $@

build/vgpu_libc_receiver_test: tests/vgpu/libc_boundary.c build/venus_receiver_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/vgpu/libc_boundary.c build/venus_receiver_bounds.o -o $@

.PHONY: vgpu-libc-test vgpu-libc-sanitizers
vgpu-libc-test: build/vgpu_libc_runtime_test build/vgpu_libc_receiver_test
	./build/vgpu_libc_runtime_test
	./build/vgpu_libc_receiver_test

vgpu-libc-sanitizers: $(VgpuLibcRuntimeObjects) build/venus_receiver_bounds.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/libc_boundary.c $(VgpuLibcRuntimeObjects) -o build/vgpu_libc_runtime_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_libc_runtime_sanitized
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/libc_boundary.c build/venus_receiver_bounds.o -o build/vgpu_libc_receiver_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_libc_receiver_sanitized

build/vgpu_gpu_queue_test build/vgpu_gpu_receiver.o: src/vgpu/venus_receiver_bounds.h include/waddle/venus_ring.h

build/venus_capabilities.o: src/vgpu/venus_capabilities.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_capabilities_test: tests/vgpu/capabilities.c include/waddle/venus_capabilities.h build/venus_capabilities.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/vgpu/capabilities.c build/venus_capabilities.o -o $@

.PHONY: vgpu-capabilities-test vgpu-capabilities-sanitizers vgpu-capabilities-coverage
vgpu-capabilities-test: build/vgpu_capabilities_test
	./build/vgpu_capabilities_test
	$(ZIG) test src/vgpu/venus_capabilities.zig

vgpu-capabilities-sanitizers: build/venus_capabilities.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/capabilities.c build/venus_capabilities.o -o build/vgpu_capabilities_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_capabilities_sanitized
	$(ZIG) test src/vgpu/venus_capabilities.zig

vgpu-capabilities-coverage:
	python3 tests/av/coverage.py venus_capabilities

build/venus_capabilities_windows.lib: src/vgpu/venus_capabilities.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@

build/vgpu_capabilities_test.exe: tests/vgpu/capabilities.c include/waddle/venus_capabilities.h build/venus_capabilities_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/capabilities.c build/venus_capabilities_windows.lib -o $@

vgpu-windows: build/vgpu_capabilities_test.exe

build/vgpu_capabilities_test build/vgpu_capabilities_test.exe: include/waddle/venus_ring.h

# Bounded process-per-context routing; no renderer singleton in the controller.
build/vgpu_context_test: tests/vgpu/context.c src/vgpu/venus_context.c include/waddle/venus_context.h build/venus_bounds.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/context.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds.o -o $@

.PHONY: vgpu-context-test vgpu-context-sanitizers vgpu-context-coverage
vgpu-context-test: build/vgpu_context_test
	./build/vgpu_context_test

vgpu-context-sanitizers: build/venus_bounds.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/context.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_bounds.o -o build/vgpu_context_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_context_sanitized

vgpu-context-coverage: build/venus_bounds.o
	python3 tests/vgpu/coverage.py context


VgpuContextSources = src/vgpu/venus_guest.c src/vgpu/venus_context.c src/vgpu/venus_worker.c src/vgpu/venus_rpc.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c
VgpuContextObjects = build/venus_bounds.o build/venus_control.o build/venus_request.o build/venus_capabilities.o
build/vgpu_contexts_integration: tests/vgpu/contexts_integration.c $(VgpuContextSources) $(VgpuContextObjects) build/waddle_vgpu_worker include/waddle/venus_context.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/contexts_integration.c $(VgpuContextSources) $(VgpuContextObjects) -o $@

.PHONY: vgpu-context-integration vgpu-context-integration-sanitizers
vgpu-context-integration: build/vgpu_contexts_integration
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/build/vendor/virglrenderer/server/virgl_render_server" ./build/vgpu_contexts_integration

vgpu-context-integration-sanitizers: $(VgpuContextObjects) build/waddle_vgpu_worker_sanitized
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/contexts_integration.c $(VgpuContextSources) $(VgpuContextObjects) -o build/vgpu_contexts_integration_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/build/vendor/virglrenderer/server/virgl_render_server" ./build/vgpu_contexts_integration_sanitized

# Standalone sanitizer/coverage invocations need the dispatch negotiation codec.
vgpu-runtime-sanitizers vgpu-runtime-coverage: build/venus_capabilities.o

# Rebuild context consumers when any borrowed lifetime or framing contract changes.
build/vgpu_context_test build/vgpu_contexts_integration: include/waddle/venus_worker.h include/waddle/venus_region.h include/waddle/venus_ring.h
build/vgpu_contexts_integration: include/waddle/venus_rpc.h include/waddle/venus_channel.h include/waddle/venus_session.h include/waddle/venus_request.h include/waddle/venus_capabilities.h include/waddle/venus_receiver.h

# Image metadata and native-endian Wayland feedback parsing stay in Zig.
build/venus_dmabuf.o: src/vgpu/venus_dmabuf.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/venus_dmabuf_windows.lib: src/vgpu/venus_dmabuf.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_dmabuf_test: tests/vgpu/dmabuf.c include/waddle/venus_dmabuf.h include/waddle/venus_ring.h build/venus_dmabuf.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/vgpu/dmabuf.c build/venus_dmabuf.o $(LDFLAGS) -o $@

build/vgpu_dmabuf_test.exe: tests/vgpu/dmabuf.c include/waddle/venus_dmabuf.h include/waddle/venus_ring.h build/venus_dmabuf_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/dmabuf.c build/venus_dmabuf_windows.lib -o $@

.PHONY: vgpu-dmabuf-test vgpu-dmabuf-sanitizers vgpu-dmabuf-coverage
vgpu-dmabuf-test: build/vgpu_dmabuf_test
	./build/vgpu_dmabuf_test
	$(ZIG) test src/vgpu/venus_dmabuf.zig

vgpu-dmabuf-sanitizers: build/venus_dmabuf.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/dmabuf.c build/venus_dmabuf.o -o build/vgpu_dmabuf_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_dmabuf_sanitized
	$(ZIG) test src/vgpu/venus_dmabuf.zig

vgpu-dmabuf-coverage:
	python3 tests/av/coverage.py venus_dmabuf

vgpu-windows: build/vgpu_dmabuf_test.exe

# The user-mode frontend borrows the existing ready guest transport.
VgpuGuestHeaders = include/waddle/venus_guest.h include/waddle/venus_capabilities.h include/waddle/venus_rpc.h include/waddle/venus_channel.h include/waddle/venus_session.h include/waddle/venus_request.h
build/vgpu_guest_test: tests/vgpu/guest.c src/vgpu/venus_guest.c $(VgpuGuestHeaders) build/venus_capabilities.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/vgpu/guest.c src/vgpu/venus_guest.c build/venus_capabilities.o $(LDFLAGS) -o $@

build/vgpu_guest_test.exe: tests/vgpu/guest.c src/vgpu/venus_guest.c $(VgpuGuestHeaders) build/venus_capabilities_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/guest.c src/vgpu/venus_guest.c build/venus_capabilities_windows.lib -o $@

.PHONY: vgpu-guest-test vgpu-guest-sanitizers vgpu-guest-coverage
vgpu-guest-test: build/vgpu_guest_test
	./build/vgpu_guest_test

vgpu-guest-sanitizers: build/venus_capabilities.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/guest.c src/vgpu/venus_guest.c build/venus_capabilities.o -o build/vgpu_guest_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_guest_sanitized

vgpu-guest-coverage: build/venus_capabilities.o build/venus_bounds.o
	python3 tests/vgpu/coverage.py guest

vgpu-windows: build/vgpu_guest_test.exe

build/vgpu_contexts_integration: $(VgpuGuestHeaders)

# Presenters borrow the host window manager's already-role-bound surface.
VgpuPresentHeaders = include/waddle/venus_present.h include/waddle/venus_dmabuf.h include/waddle/venus_ring.h
build/venus_present.o: src/vgpu/venus_present.c $(VgpuPresentHeaders) build/linux_dmabuf_client.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ibuild $(shell pkg-config --cflags wayland-client) -c $< -o $@

build/vgpu_present_test: tests/vgpu/present.c src/vgpu/venus_present.c $(VgpuPresentHeaders) build/linux_dmabuf_client.h build/venus_dmabuf.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ibuild -Isrc/vgpu tests/vgpu/present.c build/venus_dmabuf.o -o $@

.PHONY: vgpu-present-test vgpu-present-sanitizers vgpu-present-coverage
vgpu-present-test: build/vgpu_present_test build/venus_present.o
	./build/vgpu_present_test

vgpu-present-sanitizers: build/linux_dmabuf_client.h build/venus_dmabuf.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Ibuild -Isrc/vgpu tests/vgpu/present.c build/venus_dmabuf.o -o build/vgpu_present_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_present_sanitized

vgpu-present-coverage: build/linux_dmabuf_client.h build/venus_dmabuf.o build/venus_bounds.o
	python3 tests/vgpu/coverage.py present

# Portable userland WDDM model; this DLL does not register a kernel miniport.
VgpuWddmHeaders = include/waddle/venus_wddm.h $(VgpuGuestHeaders) src/vgpu/venus_receiver_bounds.h
VgpuWddmWindowsSources = src/vgpu/venus_wddm.c src/vgpu/venus_guest.c src/vgpu/venus_rpc.c $(VgpuChannelSources) src/vgpu/venus_stream_windows.c
VgpuWddmWindowsObjects = build/venus_bounds_windows.lib build/venus_control_windows.lib build/venus_request_windows.lib build/venus_capabilities_windows.lib
build/vgpu_wddm_test: tests/vgpu/wddm.c src/vgpu/venus_wddm.c $(VgpuWddmHeaders) build/venus_receiver_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/wddm.c src/vgpu/venus_wddm.c build/venus_receiver_bounds.o -o $@

build/vgpu_wddm_test.exe: tests/vgpu/wddm.c src/vgpu/venus_wddm.c $(VgpuWddmHeaders) build/venus_request_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/wddm.c src/vgpu/venus_wddm.c build/venus_request_windows.lib -o $@

build/waddle_userland_adapter.dll: src/vgpu/venus_wddm.def $(VgpuWddmWindowsSources) $(VgpuWddmHeaders) $(VgpuWddmWindowsObjects) | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu -shared src/vgpu/venus_wddm.def $(VgpuWddmWindowsSources) $(VgpuWddmWindowsObjects) -o $@

.PHONY: vgpu-wddm-test vgpu-wddm-sanitizers vgpu-wddm-coverage
vgpu-wddm-test: build/vgpu_wddm_test
	./build/vgpu_wddm_test

vgpu-wddm-sanitizers: build/venus_receiver_bounds.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/wddm.c src/vgpu/venus_wddm.c build/venus_receiver_bounds.o -o build/vgpu_wddm_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_wddm_sanitized

vgpu-wddm-coverage: build/venus_receiver_bounds.o
	python3 tests/vgpu/coverage.py wddm

vgpu-windows: build/vgpu_wddm_test.exe build/waddle_userland_adapter.dll

build/vgpu_wddm_dll_test.exe: tests/vgpu/wddm_dll.c $(VgpuWddmHeaders) build/waddle_userland_adapter.dll | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/wddm_dll.c -o $@

vgpu-windows: build/vgpu_wddm_dll_test.exe

# Presentation metadata is portable; FD ownership is Linux/controller-only.
VgpuFrameHeaders = include/waddle/venus_frame.h include/waddle/venus_dmabuf.h include/waddle/venus_ring.h
build/venus_frame.o: src/vgpu/venus_frame.zig src/vgpu/venus_dmabuf.zig $(VgpuFrameHeaders) | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/venus_frame_windows.lib: src/vgpu/venus_frame.zig src/vgpu/venus_dmabuf.zig $(VgpuFrameHeaders) | build
	$(ZIG) build-lib $< -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_frame_test: tests/vgpu/frame.c src/vgpu/venus_frame_linux.c $(VgpuFrameHeaders) build/venus_frame.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/frame.c build/venus_frame.o -o $@

build/vgpu_frame_codec_test.exe: tests/vgpu/frame_codec.c $(VgpuFrameHeaders) build/venus_frame_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/frame_codec.c build/venus_frame_windows.lib -o $@

.PHONY: vgpu-frame-test vgpu-frame-sanitizers vgpu-frame-coverage
vgpu-frame-test: build/vgpu_frame_test
	./build/vgpu_frame_test
	$(ZIG) test src/vgpu/venus_frame.zig -Iinclude -lc

vgpu-frame-sanitizers: build/venus_frame.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/frame.c build/venus_frame.o -o build/vgpu_frame_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_frame_sanitized
	$(ZIG) test src/vgpu/venus_frame.zig -Iinclude -lc

vgpu-frame-coverage: build/venus_frame.o build/venus_bounds.o
	python3 tests/vgpu/coverage.py frame_linux
	python3 tests/av/coverage.py venus_frame

vgpu-windows: build/vgpu_frame_codec_test.exe

# Actual client/server protocol fixture: mock allocation, native Wayland FD transfer.
build/linux_dmabuf_server.h: /usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml | build
	wayland-scanner server-header $< $@

VgpuPresentIntegrationSources = tests/vgpu/present_integration.c src/vgpu/venus_surface.c src/vgpu/venus_present.c src/vgpu/venus_frame_linux.c build/linux_dmabuf_protocol.c
VgpuPresentIntegrationHeaders = include/waddle/venus_surface.h $(VgpuPresentHeaders) $(VgpuFrameHeaders) build/linux_dmabuf_client.h build/linux_dmabuf_server.h
VgpuPresentIntegrationFlags = -Ibuild $(shell pkg-config --cflags wayland-client wayland-server)
VgpuPresentIntegrationLibraries = $(shell pkg-config --libs wayland-client wayland-server) -pthread
build/vgpu_present_integration: $(VgpuPresentIntegrationSources) $(VgpuPresentIntegrationHeaders) build/venus_frame.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuPresentIntegrationFlags) $(VgpuPresentIntegrationSources) build/venus_frame.o $(LDFLAGS) $(VgpuPresentIntegrationLibraries) -o $@

.PHONY: vgpu-present-integration vgpu-present-integration-sanitizers
vgpu-present-integration: build/vgpu_present_integration
	./build/vgpu_present_integration

vgpu-present-integration-sanitizers: $(VgpuPresentIntegrationSources) $(VgpuPresentIntegrationHeaders) build/venus_frame.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) $(VgpuPresentIntegrationFlags) $(VgpuPresentIntegrationSources) build/venus_frame.o $(VgpuPresentIntegrationLibraries) -o build/vgpu_present_integration_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_present_integration_sanitized

# Generator and Vulkan declarations are pinned together to the receiver schema.
VgpuProtocolPython ?= /usr/bin/python3
VgpuProtocolIncludes = -Isubmodules/venus_protocol/tests -Ibuild/venus_protocol -Isubmodules/venus_protocol/include -Isubmodules/venus_protocol/include/vulkan
.PHONY: vgpu-protocol vgpu-protocol-test
vgpu-protocol:
	$(VgpuProtocolPython) scripts/venus_protocol.py

vgpu-protocol-test: vgpu-protocol
	$(CC) -std=c11 $(VgpuProtocolIncludes) -c submodules/venus_protocol/tests/driver.c -o build/venus_protocol_driver.o
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 $(VgpuProtocolIncludes) -c submodules/venus_protocol/tests/driver.c -o build/venus_protocol_driver_windows.o

# One event-thread worker binding retains pending FDs and compositor frame identities.
build/vgpu_surface_test: tests/vgpu/surface.c src/vgpu/venus_surface.c include/waddle/venus_surface.h $(VgpuPresentHeaders) $(VgpuFrameHeaders) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/surface.c -o $@

.PHONY: vgpu-surface-test vgpu-surface-sanitizers vgpu-surface-coverage
vgpu-surface-test: build/vgpu_surface_test
	./build/vgpu_surface_test

vgpu-surface-sanitizers:
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/surface.c -o build/vgpu_surface_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_surface_sanitized

vgpu-surface-coverage: build/venus_bounds.o
	python3 tests/vgpu/coverage.py surface

# Hardware image clear/export and real protocol presentation to the mock compositor.
VgpuImageIntegrationSources = $(VgpuPresentIntegrationSources) src/vgpu/venus_receiver.c
build/vgpu_image_integration: $(VgpuImageIntegrationSources) $(VgpuPresentIntegrationHeaders) build/venus_frame.o build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) -DVgpuHardwareImage $(VgpuReceiverIncludes) $(VgpuPresentIntegrationFlags) $(VgpuImageIntegrationSources) build/venus_frame.o build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o $(VgpuPresentIntegrationLibraries) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-image-hardware vgpu-image-hardware-sanitizers
vgpu-image-hardware: build/vgpu_image_integration
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/build/vendor/virglrenderer/server/virgl_render_server" ./build/vgpu_image_integration --require-hardware

vgpu-image-hardware-sanitizers: $(VgpuImageIntegrationSources) $(VgpuPresentIntegrationHeaders) build/venus_frame.o build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -DVgpuHardwareImage $(VgpuReceiverIncludes) $(VgpuPresentIntegrationFlags) $(VgpuImageIntegrationSources) build/venus_frame.o build/venus_gpu_fixture.o build/venus_values.o build/venus_receiver_bounds.o $(VgpuPresentIntegrationLibraries) $(VgpuReceiverLibraries) -o build/vgpu_image_integration_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 RENDER_SERVER_EXEC_PATH="$(CURDIR)/build/vendor/virglrenderer/server/virgl_render_server" ./build/vgpu_image_integration_sanitized --require-hardware

VgpuExportHeaders = include/waddle/venus_export.h include/waddle/venus_receiver.h $(VgpuFrameHeaders)
build/vgpu_export_test: tests/vgpu/export.c src/vgpu/venus_export.c $(VgpuExportHeaders) build/venus_frame.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/export.c build/venus_frame.o -o $@

.PHONY: vgpu-export-test vgpu-export-sanitizers vgpu-export-coverage
vgpu-export-test: build/vgpu_export_test
	./build/vgpu_export_test

vgpu-export-sanitizers: build/venus_frame.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu tests/vgpu/export.c build/venus_frame.o -o build/vgpu_export_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_export_sanitized

vgpu-export-coverage: build/venus_frame.o build/venus_bounds.o
	python3 tests/vgpu/coverage.py export

# Presented service consumers share native frame/lease ABI changes.
build/vgpu_service_fixture build/vgpu_service_fixture_sanitized build/waddle_vgpu_worker build/waddle_vgpu_worker_sanitized build/vgpu_service_owner_test: include/waddle/venus_export.h $(VgpuFrameHeaders)

VgpuPresentedWorkerSources = tests/vgpu/worker_presented.c src/vgpu/venus_worker.c src/vgpu/venus_guest.c src/vgpu/venus_frame_linux.c src/vgpu/venus_rpc.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c
VgpuPresentedWorkerObjects = build/venus_command.o build/venus_frame.o build/venus_bounds.o build/venus_control.o build/venus_request.o build/venus_capabilities.o
build/vgpu_presented_worker_test: $(VgpuPresentedWorkerSources) $(VgpuGuestHeaders) include/waddle/venus_worker.h $(VgpuFrameHeaders) $(VgpuPresentedWorkerObjects) build/waddle_vgpu_worker
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu $(VgpuPresentedWorkerSources) $(VgpuPresentedWorkerObjects) -o $@

.PHONY: vgpu-presented-worker-test vgpu-presented-worker-sanitizers
vgpu-presented-worker-test: build/vgpu_presented_worker_test
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_test

vgpu-presented-worker-sanitizers: $(VgpuPresentedWorkerObjects) build/waddle_vgpu_worker_sanitized
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -Isrc/vgpu $(VgpuPresentedWorkerSources) $(VgpuPresentedWorkerObjects) -o build/vgpu_presented_worker_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_presented_worker_sanitized

VgpuRemoteImageSources = $(VgpuPresentIntegrationSources) src/vgpu/venus_guest.c src/vgpu/venus_worker.c src/vgpu/venus_rpc.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c
VgpuRemoteImageObjects = build/venus_gpu_fixture.o build/venus_values.o $(VgpuPresentedWorkerObjects)
build/vgpu_remote_image_integration: $(VgpuRemoteImageSources) $(VgpuPresentIntegrationHeaders) $(VgpuGuestHeaders) include/waddle/venus_worker.h $(VgpuRemoteImageObjects) build/waddle_vgpu_worker | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) -DVgpuRemoteImage $(VgpuReceiverIncludes) $(VgpuPresentIntegrationFlags) $(VgpuRemoteImageSources) $(VgpuRemoteImageObjects) $(VgpuPresentIntegrationLibraries) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-image-remote-hardware vgpu-image-remote-sanitizers
vgpu-image-remote-hardware: build/vgpu_remote_image_integration
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_remote_image_integration --require-hardware

vgpu-image-remote-sanitizers: $(VgpuRemoteImageSources) $(VgpuRemoteImageObjects) build/waddle_vgpu_worker_sanitized | vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) -DVgpuRemoteImage $(VgpuReceiverIncludes) $(VgpuPresentIntegrationFlags) $(VgpuRemoteImageSources) $(VgpuRemoteImageObjects) $(VgpuPresentIntegrationLibraries) $(VgpuReceiverLibraries) -o build/vgpu_remote_image_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 WADDLE_PRODUCTION_WORKER=build/waddle_vgpu_worker_sanitized RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_remote_image_sanitized --require-hardware

# Private staging owner has no Vulkan loader or native renderer dependency.
build/venus_command.o: src/vgpu/venus_command.zig include/waddle/venus_command.h include/waddle/venus_request.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_command_test: tests/vgpu/command.c build/venus_command.o include/waddle/venus_command.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/venus_command.o -o $@

.PHONY: vgpu-command-test vgpu-command-sanitizers vgpu-command-coverage
vgpu-command-test: build/vgpu_command_test
	./build/vgpu_command_test
	$(ZIG) test src/vgpu/venus_command.zig -Iinclude -lc

vgpu-command-sanitizers: build/venus_command.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/command.c build/venus_command.o -o build/vgpu_command_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_command_sanitized

vgpu-command-coverage:
	python3 tests/av/coverage.py venus_command

build/venus_command_windows.lib: src/vgpu/venus_command.zig include/waddle/venus_command.h include/waddle/venus_request.h | build
	$(ZIG) build-lib $< -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_command_test.exe: tests/vgpu/command.c build/venus_command_windows.lib
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $< build/venus_command_windows.lib -o $@

vgpu-windows: build/vgpu_command_test.exe

# Caller-owned loader/object records; no native Vulkan library linkage.
build/venus_objects.o: src/vgpu/venus_objects.zig include/waddle/venus_objects.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_objects_test: tests/vgpu/objects.c build/venus_objects.o include/waddle/venus_objects.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/venus_objects.o -o $@

.PHONY: vgpu-objects-test vgpu-objects-sanitizers vgpu-objects-coverage
vgpu-objects-test: build/vgpu_objects_test
	./build/vgpu_objects_test
	$(ZIG) test src/vgpu/venus_objects.zig -Iinclude -lc

vgpu-objects-sanitizers: build/venus_objects.o
	$(CC) $(CPPFLAGS) $(VgpuReceiverSanitizers) tests/vgpu/objects.c build/venus_objects.o -o build/vgpu_objects_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_objects_sanitized

vgpu-objects-coverage:
	python3 tests/av/coverage.py venus_objects

build/venus_objects_windows.lib: src/vgpu/venus_objects.zig include/waddle/venus_objects.h | build
	$(ZIG) build-lib $< -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_objects_test.exe: tests/vgpu/objects.c build/venus_objects_windows.lib
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $< build/venus_objects_windows.lib -o $@

vgpu-windows: build/vgpu_objects_test.exe

# Fixed Vulkan replies use the immutable schema and an independent C encoder oracle.
VgpuValuesIncludes = -Iinclude -Isubmodules/venus_protocol/include
VgpuValuesOracleIncludes = -Itests/vgpu/encoder -Ibuild/venus_renderer_protocol $(VgpuValuesIncludes) -Isubmodules/venus_protocol/include/vulkan
VgpuValuesHeaders = include/waddle/venus_values.h tests/vgpu/encoder/vkr_cs.h
build/venus_values.o: src/vgpu/venus_values.zig $(VgpuValuesHeaders) | build
	$(ZIG) build-obj $< $(VgpuValuesIncludes) -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/venus_values_oracle.o: tests/vgpu/values.c $(VgpuValuesHeaders) | build vgpu-protocol
	$(CC) $(CFLAGS) $(VgpuValuesOracleIncludes) -DVgpuValuesOracle -c $< -o $@

build/vgpu_values_test: tests/vgpu/values.c build/venus_values.o build/venus_values_oracle.o
	$(CC) $(CFLAGS) $(VgpuValuesOracleIncludes) $< build/venus_values.o -o $@

.PHONY: vgpu-values-test vgpu-values-sanitizers vgpu-values-coverage
vgpu-values-test: build/vgpu_values_test
	./build/vgpu_values_test
	$(ZIG) test src/vgpu/venus_values.zig $(VgpuValuesIncludes) -lc build/venus_values_oracle.o

vgpu-values-sanitizers: build/venus_values.o build/venus_values_oracle.o
	$(CC) $(VgpuReceiverSanitizers) $(VgpuValuesOracleIncludes) tests/vgpu/values.c build/venus_values.o -o build/vgpu_values_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_values_sanitized

vgpu-values-coverage: build/venus_values_oracle.o
	python3 tests/av/coverage.py venus_values

build/venus_values_windows.lib: src/vgpu/venus_values.zig $(VgpuValuesHeaders) | build
	$(ZIG) build-lib $< $(VgpuValuesIncludes) -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_values_test.exe: tests/vgpu/values.c build/venus_values_windows.lib | vgpu-protocol
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror $(VgpuValuesOracleIncludes) $< build/venus_values_windows.lib -o $@

vgpu-windows: build/vgpu_values_test.exe
