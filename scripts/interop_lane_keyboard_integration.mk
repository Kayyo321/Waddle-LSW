# Keyboard integration lane (TODO #15, contract section 12): the host keyboard handler on the real
# session runtime path. Included by GNUmakefile through scripts/interop_lane_*.mk. This fragment owns
# only the targets below. It does not edit scripts/interop_lane_keyboard.mk.
#
# The binary includes tests/interop/session_control_host.c (the vsock socketpair seam, in-process daemon
# and wraps, the same way tests/interop/session_app.c does) and runs session_app_run_detailed with the
# keyboard handler. The handler source is compiled into the binary; the codec and reducer objects come from
# the keyboard lane and the session lane. Wraps are the session tests' own list.

KeyboardIntegrationSanitizerFlags = $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g \
	-fsanitize=address,leak,undefined -fno-omit-frame-pointer -Iinclude -Isrc/interop -Isrc/daemon -Isubmodules/looking_glass/module
KeyboardIntegrationSanitizerEnv = ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
KeyboardIntegrationWrapFlags = $(shell grep -ho '__wrap_[a-z_0-9]*' tests/interop/session_control_host.c | sort -u | sed 's/^__wrap_/-Wl,--wrap=/')
KeyboardIntegrationRuntimeSources = src/interop/session_host.c src/interop/linux_session_owner.c src/interop/linux_socket_owner.c src/interop/interop_mapping_linux.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c src/daemon/daemon_device.c
KeyboardIntegrationObjects = build/interop_control_peer_abi.o build/interop_codec.o build/daemon_wire.o build/device_storage.o build/daemon_config.o build/path_rules.o build/interop_layout_abi.o \
	build/interop_keyboard_lock.o build/interop_keyboard_layouts.o build/interop_keyboard_inhibit.o build/interop_keyboard_repeat_abi.o build/interop_keymap_names.o
KeyboardIntegrationHeaders = src/interop/session_control_host.h src/interop/session_app.h src/interop/session_handler.h src/interop/session_input_handler.h

build/session_input_integration_test: tests/interop/session_input_integration.c tests/interop/session_control_host.c tests/interop/session_control_host_private.c src/interop/session_app.c src/interop/session_input_handler.c $(KeyboardIntegrationHeaders) $(KeyboardIntegrationRuntimeSources) build/interop_control_peer_abi.o build/daemon_wire.o build/device_storage.o build/daemon_config.o build/path_rules.o build/interop_layout_abi.o build/interop_codec.o build/interop_keyboard_lock.o build/interop_keyboard_layouts.o build/interop_keyboard_inhibit.o build/interop_keyboard_repeat_abi.o build/interop_keymap_names.o | build
	$(CC) $(KeyboardIntegrationSanitizerFlags) -ffunction-sections -fdata-sections tests/interop/session_input_integration.c tests/interop/session_control_host_private.c src/interop/session_app.c src/interop/session_input_handler.c $(KeyboardIntegrationRuntimeSources) build/interop_control_peer_abi.o build/interop_codec.o build/daemon_wire.o build/device_storage.o build/daemon_config.o build/path_rules.o build/interop_layout_abi.o build/interop_keyboard_lock.o build/interop_keyboard_layouts.o build/interop_keyboard_inhibit.o build/interop_keyboard_repeat_abi.o build/interop_keymap_names.o $(KeyboardIntegrationWrapFlags) $(LDFLAGS) -Wl,--gc-sections -pthread -o $@

.PHONY: interop-keyboard-integration-test
interop-keyboard-integration-test: build/session_input_integration_test
	$(KeyboardIntegrationSanitizerEnv) ./build/session_input_integration_test

InteropLaneTests += interop-keyboard-integration-test
InteropLaneSanitizers += interop-keyboard-integration-test
