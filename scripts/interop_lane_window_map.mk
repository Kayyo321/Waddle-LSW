# Host window mapping and popup positioner lane (TODO #11a and #11c; contracts in
# impl/seamless-interop/claude_45_percent/host_window_map_contract.md and
# host_popup_positioner_contract.md). Included by GNUmakefile through scripts/interop_lane_*.mk.
# This file owns only the targets below and registers them in the lane aggregates.

InteropWindowMapCFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer
InteropWindowMapLdFlags = -fsanitize=address,leak,undefined
InteropWindowMapSanEnv = ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
InteropWindowMapMarkers = tests/av/coverage_modes/host_window_map tests/av/coverage_modes/host_popup_positioner

build/host_window_map.o: src/interop/host_window_map.zig src/interop/host_window_map.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/host_popup_positioner.o: src/interop/host_popup_positioner.zig src/interop/host_popup_positioner.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/host_window_map_abi_test: tests/interop/host_window_map_abi.c build/host_window_map.o src/interop/host_window_map.h | build
	$(CC) $(InteropWindowMapCFlags) -Iinclude -Isrc/interop $< build/host_window_map.o $(InteropWindowMapLdFlags) -o $@
build/host_popup_positioner_abi_test: tests/interop/host_popup_positioner_abi.c build/host_popup_positioner.o src/interop/host_popup_positioner.h | build
	$(CC) $(InteropWindowMapCFlags) -Iinclude -Isrc/interop $< build/host_popup_positioner.o $(InteropWindowMapLdFlags) -o $@

.PHONY: interop-host-window-map-test interop-host-window-map-coverage

# Zig built-in tests for both modules, then the C ABI consumers under ASan, LSan and UBSan.
interop-host-window-map-test: build/host_window_map_abi_test build/host_popup_positioner_abi_test
	$(ZIG) test src/interop/host_window_map.zig -Iinclude -lc
	$(ZIG) test src/interop/host_popup_positioner.zig -Iinclude -lc
	$(InteropWindowMapSanEnv) ./build/host_window_map_abi_test
	$(InteropWindowMapSanEnv) ./build/host_popup_positioner_abi_test

# Coverage gates (>= 90% production line and taken-branch coverage) for both modules. The marker
# files register each module with tests/av/coverage.py; they are created here if missing.
$(InteropWindowMapMarkers):
	mkdir -p tests/av/coverage_modes && touch $@
interop-host-window-map-coverage: $(InteropWindowMapMarkers)
	python3 tests/av/coverage.py host_window_map
	python3 tests/av/coverage.py host_popup_positioner

InteropLaneTests += interop-host-window-map-test
InteropLaneSanitizers += interop-host-window-map-test
InteropLaneCoverage += interop-host-window-map-coverage

# Host Wayland realization of interop windows (TODO #11b, #11d, #11e, #11f). The test includes the module
# source; it links the Zig window table, positioner and codec objects. The xdg protocol headers and code are
# the av.mk generated files.
InteropWaylandWindowsFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer -Iinclude -Isrc/interop -Ibuild
InteropWaylandWindowsObjects = build/host_window_map.o build/host_popup_positioner.o build/interop_codec.o
InteropWaylandWindowsProtocol = build/xdg_shell_protocol.c build/xdg_decoration_protocol.c
InteropWaylandWindowsProtocolHeaders = build/xdg_shell_client.h build/xdg_decoration_client.h
InteropWaylandWindowsWraps = -Wl,--wrap=wl_proxy_marshal_flags,--wrap=wl_proxy_add_listener,--wrap=wl_proxy_destroy,--wrap=wl_proxy_get_version
build/host_wayland_windows_test: tests/interop/host_wayland_windows_test.c tests/interop/host_wayland_windows_fake.h src/interop/host_wayland_windows.c src/interop/host_wayland_windows.h $(InteropWaylandWindowsObjects) $(InteropWaylandWindowsProtocolHeaders) $(InteropWaylandWindowsProtocol) | build
	$(CC) $(InteropWaylandWindowsFlags) $(shell pkg-config --cflags wayland-client) tests/interop/host_wayland_windows_test.c $(InteropWaylandWindowsProtocol) $(InteropWaylandWindowsObjects) -fsanitize=address,leak,undefined $(shell pkg-config --libs wayland-client) $(InteropWaylandWindowsWraps) -o $@
.PHONY: interop-host-wayland-windows-test
interop-host-wayland-windows-test: build/host_wayland_windows_test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/host_wayland_windows_test
InteropLaneTests += interop-host-wayland-windows-test
InteropLaneSanitizers += interop-host-wayland-windows-test
