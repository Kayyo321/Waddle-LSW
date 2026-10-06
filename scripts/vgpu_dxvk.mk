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
