# Cross-compilation is distinct from native execution/compatibility acceptance.
build/vgpu_dxvk_integration.exe: tests/vgpu/dxvk_integration.c tests/vgpu/dxvk_heap_audit_windows.inc tests/vgpu/dxvk_fault_windows.inc | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $< -municode -luser32 -lgdi32 -ldwmapi -ldxguid -o $@

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
