# Cross-compilation is distinct from native execution/compatibility acceptance.
build/vgpu_dxvk_integration.exe: tests/vgpu/dxvk_integration.c | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror $< -municode -luser32 -ldxguid -o $@

.PHONY: vgpu-dxvk-build
vgpu-dxvk-build: build/vgpu_dxvk_integration.exe

vgpu-windows: build/vgpu_dxvk_integration.exe

# Build the unchanged pinned black-box client; native runtime acceptance is separate.
.PHONY: vgpu-dxvk-pin vgpu-dxvk-client-build
vgpu-dxvk-pin:
	sh scripts/verify_dxvk.sh

vgpu-dxvk-client-build: vgpu-dxvk-pin | build
	@if test -f build/dxvk_win64/meson-private/coredata.dat; then \
		meson configure build/dxvk_win64 -Dbuildtype=release -Denable_dxgi=true -Denable_d3d11=true -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false; \
	else \
		meson setup build/dxvk_win64 submodules/dxvk --cross-file submodules/dxvk/build-win64.txt --wrap-mode=nodownload --buildtype=release -Denable_dxgi=true -Denable_d3d11=true -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false; \
	fi
	ninja -C build/dxvk_win64 -j2
	test -s build/dxvk_win64/src/dxgi/dxgi.dll
	test -s build/dxvk_win64/src/d3d11/d3d11.dll

# Private immutable loader crossbuild: native discovery remains a separate gate.
.PHONY: vgpu-native-loader-win64
vgpu-native-loader-win64: submodules/vulkan_loader/CMakeLists.txt scripts/vulkan_loader.cmake scripts/vulkan_loader_win64.cmake
	cmake -S submodules/vulkan_loader -B build/vendor/vulkan_loader_win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(CURDIR)/scripts/vulkan_loader_win64.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_VULKAN_LOADER_INCLUDE="$(CURDIR)/scripts/vulkan_loader.cmake" -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF -DLOADER_CODEGEN=OFF -DBUILD_WERROR=ON -DUSE_GAS=ON
	cmake --build build/vendor/vulkan_loader_win64 --parallel 4
	test -s build/vendor/vulkan_loader_win64/loader/vulkan-1.dll
	x86_64-w64-mingw32-objdump -f build/vendor/vulkan_loader_win64/loader/vulkan-1.dll | grep 'file format pei-x86-64'
	x86_64-w64-mingw32-objdump -p build/vendor/vulkan_loader_win64/loader/vulkan-1.dll | grep 'DLL Name:'
