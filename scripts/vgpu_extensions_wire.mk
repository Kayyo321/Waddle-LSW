# Actual extension count/fill codecs; generated pinned headers are read-only.
ZIG ?= zig
VgpuExtensionsSource = src/vgpu/venus_extensions_wire.zig
VgpuExtensionsIncludes = -Iinclude -Isubmodules/venus_protocol/include
VgpuExtensionsWarnings = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g
VgpuExtensionsGuestIncludes = $(VgpuExtensionsIncludes) -Itests/vgpu/encoder -Ibuild/venus_protocol -Isubmodules/venus_protocol/tests
VgpuExtensionsRendererIncludes = $(VgpuExtensionsIncludes) -Itests/vgpu/encoder -Ibuild/venus_renderer_protocol -Isubmodules/venus_protocol/include/vulkan
VgpuExtensionsGuestHeaders = tests/vgpu/encoder/vn_cs.h $(wildcard build/venus_protocol/*.h)
VgpuExtensionsRendererHeaders = tests/vgpu/encoder/vkr_cs.h $(wildcard build/venus_renderer_protocol/*.h)
VgpuExtensionsNativeOracles = build/extensions_wire/request.o build/extensions_wire/reply.o
VgpuExtensionsWindowsOracles = build/extensions_wire/request_windows.obj build/extensions_wire/reply_windows.obj

build/extensions_wire/request.o: tests/vgpu/extensions_wire_oracle.c $(VgpuExtensionsGuestHeaders)
	mkdir -p build/extensions_wire
	$(CC) $(VgpuExtensionsWarnings) $(VgpuExtensionsGuestIncludes) -c $< -o $@

build/extensions_wire/reply.o: tests/vgpu/extensions_reply_oracle.c $(VgpuExtensionsRendererHeaders)
	mkdir -p build/extensions_wire
	$(CC) $(VgpuExtensionsWarnings) $(VgpuExtensionsRendererIncludes) -c $< -o $@

build/extensions_wire/request_windows.obj: tests/vgpu/extensions_wire_oracle.c $(VgpuExtensionsGuestHeaders)
	mkdir -p build/extensions_wire
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuExtensionsWarnings) $(VgpuExtensionsGuestIncludes) -c $< -o $@

build/extensions_wire/reply_windows.obj: tests/vgpu/extensions_reply_oracle.c $(VgpuExtensionsRendererHeaders)
	mkdir -p build/extensions_wire
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuExtensionsWarnings) $(VgpuExtensionsRendererIncludes) -c $< -o $@

.PHONY: vgpu-extensions-wire-test vgpu-extensions-wire-sanitizers vgpu-extensions-wire-coverage vgpu-extensions-wire-windows
vgpu-extensions-wire-test: $(VgpuExtensionsNativeOracles)
	$(ZIG) test $(VgpuExtensionsSource) $(VgpuExtensionsIncludes) -lc $(VgpuExtensionsNativeOracles)
	$(ZIG) test $(VgpuExtensionsSource) $(VgpuExtensionsIncludes) -lc -O ReleaseSafe $(VgpuExtensionsNativeOracles)

vgpu-extensions-wire-sanitizers: scripts/extensions_wire_sanitizers.py scripts/icd_owned_sanitizers.py scripts/icd_dependency_sanitizers.py
	python3 -B scripts/extensions_wire_sanitizers.py build/extensions_wire_sanitizers

vgpu-extensions-wire-coverage: tests/av/coverage.py $(VgpuExtensionsSource)
	python3 -B tests/av/coverage.py venus_extensions_wire

vgpu-extensions-wire-windows: $(VgpuExtensionsWindowsOracles)
	$(ZIG) test $(VgpuExtensionsSource) $(VgpuExtensionsIncludes) -lc -target x86_64-windows-gnu -O ReleaseSafe $(VgpuExtensionsWindowsOracles) --test-no-exec -femit-bin=build/extensions_wire/units.exe
