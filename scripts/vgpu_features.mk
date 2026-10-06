# Native feature-chain adaptation: no receiver, capability advertisement or heap ownership.
VgpuFeaturesNativeSource = src/vgpu/venus_features_native.zig
VgpuFeaturesOracleIncludes = $(VgpuInstanceOracleIncludes) $(VgpuValuesOracleIncludes)
VgpuFeaturesOracleNames = features_query features_reply render_wire
VgpuFeaturesNativeOracles = $(addprefix build/venus_,$(addsuffix _oracle.o,$(VgpuFeaturesOracleNames)))
VgpuFeaturesSanitizedOracles = $(addprefix build/features_native_,$(addsuffix _sanitized.o,$(VgpuFeaturesOracleNames)))
VgpuFeaturesWindowsOracles = $(addprefix build/features_native_,$(addsuffix _windows.obj,$(VgpuFeaturesOracleNames)))
VgpuFeaturesSanitizerDirectory = $(dir $(shell $(CC) -print-file-name=libasan.so))

build/features_native_%_sanitized.o: tests/vgpu/%_oracle.c tests/vgpu/features_oracle.h tests/vgpu/encoder/vn_cs.h tests/vgpu/encoder/vkr_cs.h | vgpu-protocol
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuFeaturesOracleIncludes) -c $< -o $@

build/features_native_%_windows.obj: tests/vgpu/%_oracle.c tests/vgpu/features_oracle.h tests/vgpu/encoder/vn_cs.h tests/vgpu/encoder/vkr_cs.h | vgpu-protocol
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror $(VgpuFeaturesOracleIncludes) -c $< -o $@

.PHONY: vgpu-features-native-test vgpu-features-native-sanitizers vgpu-features-native-windows vgpu-features-native-coverage
vgpu-features-native-test: $(VgpuFeaturesNativeOracles)
	$(ZIG) test $(VgpuFeaturesNativeSource) $(VgpuInstanceWireIncludes) -lc $(VgpuFeaturesNativeOracles)

vgpu-features-native-sanitizers: $(VgpuFeaturesSanitizedOracles)
	$(ZIG) test $(VgpuFeaturesNativeSource) $(VgpuInstanceWireIncludes) -lc $(VgpuFeaturesSanitizedOracles) -L$(VgpuFeaturesSanitizerDirectory) -lasan -lubsan --test-no-exec -femit-bin=build/venus_features_native_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/venus_features_native_sanitized

vgpu-features-native-windows: $(VgpuFeaturesWindowsOracles)
	$(ZIG) test $(VgpuFeaturesNativeSource) $(VgpuInstanceWireIncludes) -target x86_64-windows-gnu -lc $(VgpuFeaturesWindowsOracles) --test-no-exec -femit-bin=build/venus_features_native_test.exe

vgpu-features-native-coverage: $(VgpuFeaturesNativeOracles)
	python3 tests/av/coverage.py venus_features_native

vgpu-icd-test: vgpu-features-native-test
vgpu-icd-sanitizers: vgpu-features-native-sanitizers
vgpu-icd-coverage: vgpu-features-native-coverage
vgpu-windows: vgpu-features-native-windows
