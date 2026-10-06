# Allocation-free Venus transport; no renderer dependencies for these checks.
.PHONY: vgpu-test vgpu-sanitizers

build/venus_bounds.o: src/vgpu/venus_bounds.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -femit-bin=$@

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
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@

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
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -femit-bin=$@

build/vgpu_session_test: tests/vgpu/session.c src/vgpu/venus_session.c include/waddle/venus_session.h src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control.o build/venus_bounds.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/vgpu tests/vgpu/session.c src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control.o build/venus_bounds.o $(LDFLAGS) -o $@

build/venus_control_windows.lib: src/vgpu/venus_control.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@

build/vgpu_session_test.exe: tests/vgpu/session.c src/vgpu/venus_session.c include/waddle/venus_session.h src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control_windows.lib build/venus_bounds_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/vgpu tests/vgpu/session.c src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c build/venus_control_windows.lib build/venus_bounds_windows.lib -o $@

vgpu-windows: build/vgpu_session_test.exe

# Real public renderer dispatch plus a separately injected boundary ownership test.
VgpuReceiverIncludes = -Isrc/vgpu -I$(VgpuRendererDirectory)/src -I$(VgpuRendererBuildDirectory)/src
VgpuReceiverLibraries = -L$(VgpuRendererBuildDirectory)/src -Wl,-rpath,'$$ORIGIN/vendor/virglrenderer/src' -lvirglrenderer
VgpuReceiverSanitizers = -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer

build/venus_receiver_bounds.o: src/vgpu/venus_receiver_bounds.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -femit-bin=$@

build/vgpu_receiver_owner_test: tests/vgpu/receiver_owner.c src/vgpu/venus_receiver.c src/vgpu/venus_receiver_bounds.h include/waddle/venus_receiver.h build/venus_receiver_bounds.o | $(VgpuRendererBuildDirectory)/build.ninja
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/receiver_owner.c build/venus_receiver_bounds.o $(LDFLAGS) -o $@

build/vgpu_receiver_test: tests/vgpu/receiver.c src/vgpu/venus_receiver.c src/vgpu/venus_receiver_bounds.h include/waddle/venus_receiver.h build/venus_receiver_bounds.o | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/receiver.c src/vgpu/venus_receiver.c build/venus_receiver_bounds.o $(LDFLAGS) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-receiver-test vgpu-receiver-sanitizers vgpu-receiver-coverage
vgpu-receiver-test: build/vgpu_receiver_owner_test build/vgpu_receiver_test
	./build/vgpu_receiver_owner_test
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_receiver_test
	$(ZIG) test src/vgpu/venus_receiver_bounds.zig

vgpu-receiver-sanitizers: build/venus_receiver_bounds.o vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverIncludes) $(VgpuReceiverSanitizers) tests/vgpu/receiver_owner.c build/venus_receiver_bounds.o -o build/vgpu_receiver_owner_sanitized
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

build/vgpu_integration_test: tests/vgpu/integration.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c build/venus_bounds.o build/venus_control.o build/venus_receiver_bounds.o | vgpu-renderer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuReceiverIncludes) tests/vgpu/integration.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c build/venus_bounds.o build/venus_control.o build/venus_receiver_bounds.o $(LDFLAGS) $(VgpuReceiverLibraries) -o $@

.PHONY: vgpu-integration vgpu-integration-sanitizers
vgpu-integration: build/vgpu_integration_test
	RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_integration_test

vgpu-integration-sanitizers: build/venus_bounds.o build/venus_control.o build/venus_receiver_bounds.o vgpu-renderer
	$(CC) $(CPPFLAGS) $(VgpuReceiverIncludes) $(VgpuReceiverSanitizers) tests/vgpu/integration.c $(VgpuChannelSources) src/vgpu/venus_stream_linux.c src/vgpu/venus_receiver.c build/venus_bounds.o build/venus_control.o build/venus_receiver_bounds.o $(VgpuReceiverLibraries) -o build/vgpu_integration_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 RENDER_SERVER_EXEC_PATH="$(CURDIR)/$(VgpuRendererBuildDirectory)/server/virgl_render_server" ./build/vgpu_integration_sanitized

# Request codec reuses and includes the exported Zig resource-bound helpers.
# Link this object alone (not also venus_receiver_bounds.o) in dispatch binaries.
build/venus_request.o: src/vgpu/venus_request.zig src/vgpu/venus_receiver_bounds.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -femit-bin=$@

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
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@

build/vgpu_request_test.exe: tests/vgpu/request.c include/waddle/venus_request.h build/venus_request_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/request.c build/venus_request_windows.lib -o $@

vgpu-windows: build/vgpu_request_test.exe
