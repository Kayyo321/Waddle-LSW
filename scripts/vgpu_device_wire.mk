# Typed device-create codec gates. Generated pinned headers must already exist;
# standalone safety discovers them read-only and never runs a shared generator.
ZIG ?= zig
VgpuDeviceWireSource = src/vgpu/venus_device_wire.zig
VgpuDeviceWireIncludes = -Iinclude -Isubmodules/venus_protocol/include
VgpuDeviceWireOracleIncludes = $(VgpuDeviceWireIncludes) -Itests/vgpu/encoder -Ibuild/venus_protocol -Isubmodules/venus_protocol/tests
VgpuDeviceWireWarnings = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG
VgpuDeviceWireHeaderInputs = tests/vgpu/encoder/vn_cs.h $(wildcard build/venus_protocol/*.h)
VgpuDeviceWireNativeOracles = build/venus_device_wire_oracle.o build/device_wire/render_oracle.o
VgpuDeviceWireWindowsOracles = build/device_wire/device_oracle_windows.obj build/device_wire/render_oracle_windows.obj

build/venus_device_wire_oracle.o: tests/vgpu/device_wire_oracle.c $(VgpuDeviceWireHeaderInputs)
	mkdir -p build
	$(CC) $(VgpuDeviceWireWarnings) -O1 -g $(VgpuDeviceWireOracleIncludes) -c $< -o $@

build/device_wire/render_oracle.o: tests/vgpu/render_wire_oracle.c $(VgpuDeviceWireHeaderInputs)
	mkdir -p build/device_wire
	$(CC) $(VgpuDeviceWireWarnings) -O1 -g $(VgpuDeviceWireOracleIncludes) -c $< -o $@

build/device_wire/%_oracle_windows.obj: tests/vgpu/%_wire_oracle.c $(VgpuDeviceWireHeaderInputs)
	mkdir -p build/device_wire
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuDeviceWireWarnings) -O1 -g $(VgpuDeviceWireOracleIncludes) -c $< -o $@

.PHONY: vgpu-device-wire-test vgpu-device-wire-sanitizers vgpu-device-wire-coverage vgpu-device-wire-windows
vgpu-device-wire-test: $(VgpuDeviceWireNativeOracles)
	$(ZIG) test $(VgpuDeviceWireSource) $(VgpuDeviceWireIncludes) -lc $(VgpuDeviceWireNativeOracles)
	$(ZIG) test $(VgpuDeviceWireSource) $(VgpuDeviceWireIncludes) -lc -O ReleaseSafe $(VgpuDeviceWireNativeOracles)

vgpu-device-wire-sanitizers: scripts/device_wire_sanitizers.py scripts/icd_owned_sanitizers.py scripts/icd_dependency_sanitizers.py
	python3 -B scripts/device_wire_sanitizers.py build/device_wire_sanitizers

vgpu-device-wire-coverage: build/venus_device_wire_oracle.o build/venus_render_wire_oracle.o
	python3 tests/av/coverage.py venus_device_wire

vgpu-device-wire-windows: $(VgpuDeviceWireWindowsOracles)
	$(ZIG) test $(VgpuDeviceWireSource) $(VgpuDeviceWireIncludes) -lc -target x86_64-windows-gnu -O ReleaseSafe $(VgpuDeviceWireWindowsOracles) --test-no-exec -femit-bin=build/device_wire/units.exe
