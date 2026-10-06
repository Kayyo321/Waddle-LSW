# Cross-compilation is distinct from native execution/compatibility acceptance.
build/vgpu_dxvk_integration.exe: tests/vgpu/dxvk_integration.c | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror $< -municode -luser32 -ldxguid -o $@

.PHONY: vgpu-dxvk-build
vgpu-dxvk-build: build/vgpu_dxvk_integration.exe

vgpu-windows: build/vgpu_dxvk_integration.exe
