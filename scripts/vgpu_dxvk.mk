# Cross-compilation is distinct from native execution/compatibility acceptance.
build/vgpu_dxvk_integration.exe: tests/vgpu/dxvk_integration.c tests/vgpu/dxvk_heap_audit_windows.inc tests/vgpu/dxvk_fault_windows.inc tests/vgpu/loader_environment_windows.h tests/vgpu/loader_environment_windows.c | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $< tests/vgpu/loader_environment_windows.c -municode -luser32 -lgdi32 -ldwmapi -ldxguid -o $@

.PHONY: vgpu-dxvk-build
vgpu-dxvk-build: build/vgpu_dxvk_integration.exe

vgpu-windows: build/vgpu_dxvk_integration.exe

# Verify pinned upstream sources; the tracked owner patch is applied only to build copies.
.PHONY: vgpu-dxvk-pin vgpu-dxvk-client-build
vgpu-dxvk-pin:
	sh scripts/verify_dxvk.sh

# Every client artifact uses the same tracked owner patch and hash receipt.
# The upstream submodule and prior pristine/private builds remain untouched.
vgpu-dxvk-client-build: vgpu-dxvk-pin scripts/build_dxvk_client.py patches/dxvk/setupapi_device_info_owner.patch | build
	python3 scripts/build_dxvk_client.py

# Private immutable loader crossbuild: native discovery remains a separate gate.
.PHONY: vgpu-native-loader-win64
vgpu-native-loader-win64: submodules/vulkan_loader/CMakeLists.txt scripts/vulkan_loader.cmake scripts/vulkan_loader_win64.cmake
	cmake -S submodules/vulkan_loader -B build/vendor/vulkan_loader_win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(CURDIR)/scripts/vulkan_loader_win64.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_VULKAN_LOADER_INCLUDE="$(CURDIR)/scripts/vulkan_loader.cmake" -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF -DLOADER_CODEGEN=OFF -DBUILD_WERROR=ON -DUSE_GAS=ON
	cmake --build build/vendor/vulkan_loader_win64 --parallel 4
	test -s build/vendor/vulkan_loader_win64/loader/vulkan-1.dll
	x86_64-w64-mingw32-objdump -f build/vendor/vulkan_loader_win64/loader/vulkan-1.dll | grep 'file format pei-x86-64'
	x86_64-w64-mingw32-objdump -p build/vendor/vulkan_loader_win64/loader/vulkan-1.dll | grep 'DLL Name:'

# OpenCL (TODO #5): pinned clvk + clspv/LLVM cross-built for Windows. Not part of the
# default vgpu-windows aggregate because the LLVM build is long; cross-compilation is
# not execution. Submodules: clvk (nested), llvm_project, clspv_spirv_{headers,tools}.
.PHONY: vgpu-opencl-pin vgpu-opencl-build
vgpu-opencl-pin:
	sh scripts/verify_clvk.sh

vgpu-opencl-build: vgpu-opencl-pin scripts/build_clvk_windows.py scripts/clvk/OpenCL.def patches/clvk/0001-explicit-quiescent-windows-shutdown.patch include/waddle/clvk.h | build
	python3 scripts/build_clvk_windows.py

# Native execution requires a separately owned authenticated host receiver.
.PHONY: vgpu-opencl-test vgpu-opencl-fixture
vgpu-opencl-test:
	python3 -B tests/vgpu/clvk_build.py

build/vgpu_opencl_integration.exe: tests/vgpu/opencl_integration.c tests/vgpu/dxvk_heap_audit_windows.inc tests/vgpu/loader_environment_windows.h tests/vgpu/loader_environment_windows.c include/waddle/venus_tcp.h include/waddle/clvk.h submodules/clvk/external/OpenCL-Headers/CL/cl.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/vgpu -Isubmodules/clvk/external/OpenCL-Headers $< tests/vgpu/loader_environment_windows.c -municode -o $@

vgpu-opencl-fixture: build/vgpu_opencl_integration.exe

# Actual guard implementation under a mock-only Linux Win32 boundary. This does
# not replace the native Windows environment round-trip target below.
.PHONY: vgpu-loader-environment-test vgpu-loader-environment-sanitizers vgpu-loader-environment-coverage
vgpu-loader-environment-test: | build
	$(CC) $(CFLAGS) -DWaddleLoaderEnvironmentTest -DWaddleLoaderEnvironmentFakeWindows -Itests/vgpu/loader_environment_fake -Itests/vgpu tests/vgpu/loader_environment_windows.c -o build/vgpu_loader_environment_mock
	./build/vgpu_loader_environment_mock

vgpu-loader-environment-sanitizers: | build
	$(CC) $(CFLAGS) -O1 -fsanitize=address,leak,undefined -fno-omit-frame-pointer -DWaddleLoaderEnvironmentTest -DWaddleLoaderEnvironmentFakeWindows -Itests/vgpu/loader_environment_fake -Itests/vgpu tests/vgpu/loader_environment_windows.c -o build/vgpu_loader_environment_mock_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/vgpu_loader_environment_mock_sanitized

build/vgpu_loader_environment_test.exe: tests/vgpu/loader_environment_windows.c tests/vgpu/loader_environment_windows.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -DWaddleLoaderEnvironmentTest -Itests/vgpu $< -o $@

vgpu-windows: build/vgpu_loader_environment_test.exe

vgpu-loader-environment-coverage: | build
	python3 -B scripts/loader_environment_coverage.py build/loader_environment_coverage
