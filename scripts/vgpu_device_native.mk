# Pure owned native device preflight; generated pinned headers remain read-only.
# Include after vgpu_device_wire.mk to share its strict, assertion-active oracles.
VgpuDeviceNativeSource = src/vgpu/venus_device_native.zig

.PHONY: vgpu-device-native-test vgpu-device-native-sanitizers vgpu-device-native-coverage vgpu-device-native-windows
vgpu-device-native-test: $(VgpuDeviceWireNativeOracles)
	$(ZIG) test $(VgpuDeviceNativeSource) $(VgpuDeviceWireIncludes) -lc $(VgpuDeviceWireNativeOracles)
	$(ZIG) test $(VgpuDeviceNativeSource) $(VgpuDeviceWireIncludes) -lc -O ReleaseSafe $(VgpuDeviceWireNativeOracles)

vgpu-device-native-sanitizers: scripts/device_native_sanitizers.py scripts/icd_owned_sanitizers.py scripts/icd_dependency_sanitizers.py vgpu-owned-sanitizer-inventory-test
	python3 -B scripts/device_native_sanitizers.py build/device_native_owned_safety

vgpu-device-native-coverage: tests/av/coverage.py $(VgpuDeviceNativeSource)
	python3 -B tests/av/coverage.py venus_device_native

vgpu-device-native-windows: $(VgpuDeviceWireWindowsOracles)
	mkdir -p build/device_native
	$(ZIG) test $(VgpuDeviceNativeSource) $(VgpuDeviceWireIncludes) -lc -target x86_64-windows-gnu -O ReleaseSafe $(VgpuDeviceWireWindowsOracles) --test-no-exec -femit-bin=build/device_native/units.exe
