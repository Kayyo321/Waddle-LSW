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
InteropWaylandWindowsTestHeaders = tests/interop/host_wayland_windows_fake.h tests/interop/host_wayland_windows_fixtures.h tests/interop/host_wayland_windows_stress.h tests/interop/host_wayland_windows_defensive.h
build/host_wayland_windows_test: tests/interop/host_wayland_windows_test.c $(InteropWaylandWindowsTestHeaders) src/interop/host_wayland_windows.c src/interop/host_wayland_windows.h $(InteropWaylandWindowsObjects) $(InteropWaylandWindowsProtocolHeaders) $(InteropWaylandWindowsProtocol) | build
	$(CC) $(InteropWaylandWindowsFlags) $(shell pkg-config --cflags wayland-client) tests/interop/host_wayland_windows_test.c $(InteropWaylandWindowsProtocol) $(InteropWaylandWindowsObjects) -fsanitize=address,leak,undefined $(shell pkg-config --libs wayland-client) $(InteropWaylandWindowsWraps) -o $@
.PHONY: interop-host-wayland-windows-test
interop-host-wayland-windows-test: build/host_wayland_windows_test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/host_wayland_windows_test
InteropLaneTests += interop-host-wayland-windows-test
InteropLaneSanitizers += interop-host-wayland-windows-test

# Line and branch coverage gate (>= 90 percent each, no exclusions) for host_wayland_windows.c. The script builds
# its own instrumented copy of the test unit under build/coverage/interop/host_wayland_windows/.
.PHONY: interop-host-wayland-windows-coverage
interop-host-wayland-windows-coverage: build/host_window_map.o build/host_popup_positioner.o build/interop_codec.o build/xdg_shell_protocol.c build/xdg_decoration_protocol.c build/xdg_shell_client.h build/xdg_decoration_client.h
	python3 tests/interop/host_wayland_windows_coverage.py
InteropLaneCoverage += interop-host-wayland-windows-coverage

# Host Wayland core (TODO #11g, #13): display connection, registry globals, seats, the poll loop and teardown. The
# test drives a recording Wayland double through --wrap; see impl/seamless-interop/claude_45_percent/host_wayland_core_contract.md.
InteropHostCoreWraps = -Wl,--wrap=wl_proxy_marshal_flags,--wrap=wl_proxy_get_version,--wrap=wl_proxy_add_listener,--wrap=wl_proxy_destroy,--wrap=wl_display_connect,--wrap=wl_display_disconnect,--wrap=wl_display_get_fd,--wrap=wl_display_get_error,--wrap=wl_display_dispatch_pending,--wrap=wl_display_prepare_read,--wrap=wl_display_read_events,--wrap=wl_display_flush,--wrap=wl_display_roundtrip
build/host_wayland_core_test: tests/interop/host_wayland_core_test.c tests/interop/host_wayland_core_fake.h src/interop/host_wayland_core.c src/interop/host_wayland_core.h build/xdg_shell_protocol.c build/xdg_decoration_protocol.c build/xdg_shell_client.h build/xdg_decoration_client.h | build
	$(CC) $(InteropWaylandWindowsFlags) $(shell pkg-config --cflags wayland-client) tests/interop/host_wayland_core_test.c build/xdg_shell_protocol.c build/xdg_decoration_protocol.c -fsanitize=address,leak,undefined $(shell pkg-config --libs wayland-client) $(InteropHostCoreWraps) -o $@
.PHONY: interop-host-wayland-core-test interop-host-wayland-core-coverage
interop-host-wayland-core-test: build/host_wayland_core_test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/host_wayland_core_test
InteropLaneTests += interop-host-wayland-core-test
InteropLaneSanitizers += interop-host-wayland-core-test
interop-host-wayland-core-coverage: build/xdg_shell_protocol.c build/xdg_decoration_protocol.c build/xdg_shell_client.h build/xdg_decoration_client.h
	python3 tests/interop/host_wayland_core_coverage.py
InteropLaneCoverage += interop-host-wayland-core-coverage

# Session windows handler (TODO #11g live wiring, #12 move, resize and state requests, #13 wire window path). The test
# links the handler, the core and the windows layer with the combined recording double, the codec, the window-map and
# positioner objects, and the window-state reducer object of interop_lane_coordinator.mk.
InteropSessionWindowsFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer -Iinclude -Isrc/interop -Isrc/daemon -Ibuild
InteropSessionWindowsWraps = -Wl,--wrap=wl_proxy_marshal_flags,--wrap=wl_proxy_get_version,--wrap=wl_proxy_add_listener,--wrap=wl_proxy_destroy,--wrap=wl_display_connect,--wrap=wl_display_disconnect,--wrap=wl_display_get_fd,--wrap=wl_display_get_error,--wrap=wl_display_dispatch_pending,--wrap=wl_display_prepare_read,--wrap=wl_display_read_events,--wrap=wl_display_flush,--wrap=wl_display_roundtrip,--wrap=interop_host_feature_send
InteropSessionWindowsObjects = build/host_window_map.o build/host_popup_positioner.o build/interop_codec.o build/window_state_abi/window_state_abi.o
build/session_windows_handler_test: tests/interop/session_windows_handler_test.c tests/interop/session_windows_handler_fake.h src/interop/session_windows_handler.c src/interop/session_windows_handler.h src/interop/host_wayland_core.c src/interop/host_wayland_core.h src/interop/host_wayland_windows.c src/interop/host_wayland_windows.h build/xdg_shell_protocol.c build/xdg_decoration_protocol.c build/xdg_shell_client.h build/xdg_decoration_client.h $(InteropSessionWindowsObjects) | build
	$(CC) $(InteropSessionWindowsFlags) $(shell pkg-config --cflags wayland-client) tests/interop/session_windows_handler_test.c src/interop/session_windows_handler.c src/interop/host_wayland_core.c src/interop/host_wayland_windows.c build/xdg_shell_protocol.c build/xdg_decoration_protocol.c $(InteropSessionWindowsObjects) $(shell pkg-config --libs wayland-client) $(InteropSessionWindowsWraps) -o $@
.PHONY: interop-session-windows-handler-test interop-session-windows-handler-coverage
interop-session-windows-handler-test: build/session_windows_handler_test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/session_windows_handler_test
InteropLaneTests += interop-session-windows-handler-test
InteropLaneSanitizers += interop-session-windows-handler-test
interop-session-windows-handler-coverage: $(InteropSessionWindowsObjects) build/xdg_shell_protocol.c build/xdg_decoration_protocol.c build/xdg_shell_client.h build/xdg_decoration_client.h
	python3 tests/interop/session_windows_handler_coverage.py
InteropLaneCoverage += interop-session-windows-handler-coverage

# Control-runtime integration of the session windows handler: the real interop_host_control_run with an in-process guest
# peer (tests/interop/session_windows_handler_control_test.c). The control runtime's own sources and objects are linked
# as session_control_host's test links them; its socket, poll and mapping calls are wrapped.
InteropSessionWindowsControlSources = src/interop/session_control_host.c src/interop/session_host.c src/interop/linux_session_owner.c src/interop/linux_socket_owner.c src/interop/interop_mapping_linux.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c src/daemon/daemon_device.c
InteropSessionWindowsControlObjects = build/interop_control_peer_abi.o build/daemon_wire.o build/device_storage.o build/daemon_config.o build/path_rules.o build/interop_layout_abi.o
InteropSessionWindowsControlWraps = -Wl,--wrap=wl_proxy_marshal_flags,--wrap=wl_proxy_get_version,--wrap=wl_proxy_add_listener,--wrap=wl_proxy_destroy,--wrap=wl_display_connect,--wrap=wl_display_disconnect,--wrap=wl_display_get_fd,--wrap=wl_display_get_error,--wrap=wl_display_dispatch_pending,--wrap=wl_display_prepare_read,--wrap=wl_display_read_events,--wrap=wl_display_flush,--wrap=wl_display_roundtrip,--wrap=socket,--wrap=connect,--wrap=poll,--wrap=getsockopt,--wrap=getpeername,--wrap=send,--wrap=recv
build/session_windows_handler_control_test: tests/interop/session_windows_handler_control_test.c tests/interop/session_windows_handler_fake.h src/interop/session_windows_handler.c src/interop/session_windows_handler.h src/interop/host_wayland_core.c src/interop/host_wayland_windows.c $(InteropSessionWindowsControlSources) build/xdg_shell_protocol.c build/xdg_decoration_protocol.c build/xdg_shell_client.h build/xdg_decoration_client.h $(InteropSessionWindowsObjects) $(InteropSessionWindowsControlObjects) | build
	$(CC) $(InteropSessionWindowsFlags) -D_GNU_SOURCE $(shell pkg-config --cflags wayland-client) tests/interop/session_windows_handler_control_test.c src/interop/session_windows_handler.c src/interop/host_wayland_core.c src/interop/host_wayland_windows.c $(InteropSessionWindowsControlSources) build/xdg_shell_protocol.c build/xdg_decoration_protocol.c $(InteropSessionWindowsObjects) $(InteropSessionWindowsControlObjects) $(shell pkg-config --libs wayland-client) $(InteropSessionWindowsControlWraps) -o $@
.PHONY: interop-session-windows-control-test
interop-session-windows-control-test: build/session_windows_handler_control_test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/session_windows_handler_control_test
InteropLaneTests += interop-session-windows-control-test
InteropLaneSanitizers += interop-session-windows-control-test
