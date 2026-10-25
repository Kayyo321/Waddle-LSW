# Keyboard lane (TODO #15: lock sync, layout sync, shortcut inhibit). This fragment owns
# only the keyboard_* targets below. Pure Zig modules are tested with the built-in test
# runner; their C consumers are rebuilt with ASan, LSan and UBSan and run with leak detection.
KeyboardLaneSanitizerFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g \
	-fsanitize=address,leak,undefined -fno-omit-frame-pointer -Iinclude -Isrc/interop
KeyboardLaneSanitizerEnv = ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

build/interop_keyboard_lock.o: src/interop/keyboard_lock.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -femit-bin=$@
build/interop_keyboard_lock_abi_test: tests/interop/keyboard_lock_abi.c build/interop_keyboard_lock.o src/interop/keyboard_lock.h | build
	$(CC) $(KeyboardLaneSanitizerFlags) $< build/interop_keyboard_lock.o $(LDFLAGS) -o $@
build/interop_keyboard_layouts.o: src/interop/keyboard_layouts.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -femit-bin=$@
build/interop_keyboard_layouts_abi_test: tests/interop/keyboard_layouts_abi.c build/interop_keyboard_layouts.o src/interop/keyboard_layouts.h | build
	$(CC) $(KeyboardLaneSanitizerFlags) $< build/interop_keyboard_layouts.o $(LDFLAGS) -o $@
build/interop_keyboard_repeat_abi.o: src/interop/keyboard_repeat_abi.zig src/interop/keyboard_repeat.zig src/interop/keyboard_repeat_abi.h | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -femit-bin=$@
build/interop_keyboard_repeat_abi_test: tests/interop/keyboard_repeat_abi.c build/interop_keyboard_repeat_abi.o src/interop/keyboard_repeat_abi.h | build
	$(CC) $(KeyboardLaneSanitizerFlags) $< build/interop_keyboard_repeat_abi.o $(LDFLAGS) -o $@
build/interop_keyboard_repeat_policy_test: tests/interop/keyboard_repeat_policy.c build/interop_keyboard_repeat_abi.o src/av/av_input.c src/av/av_input.h build/av_codec.o | build
	$(CC) $(KeyboardLaneSanitizerFlags) -Isrc/av tests/interop/keyboard_repeat_policy.c src/av/av_input.c build/av_codec.o build/interop_keyboard_repeat_abi.o $(LDFLAGS) -o $@
build/interop_keyboard_inhibit.o: src/interop/keyboard_inhibit.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -femit-bin=$@
build/interop_keyboard_inhibit_abi_test: tests/interop/keyboard_inhibit_abi.c build/interop_keyboard_inhibit.o src/interop/keyboard_inhibit.h | build
	$(CC) $(KeyboardLaneSanitizerFlags) $< build/interop_keyboard_inhibit.o $(LDFLAGS) -o $@

# Host session keyboard handler (IMPL_DESC 3.6.2, 3.6.3). The handler is linked against the
# codec and reducer objects; the test wraps interop_host_feature_send and clock_gettime.
SessionInputSanitizerFlags = $(KeyboardLaneSanitizerFlags) -Isrc/daemon -Isubmodules/looking_glass/module
SessionInputObjects = build/session_input_handler.o build/interop_codec.o build/interop_keyboard_lock.o \
	build/interop_keyboard_layouts.o build/interop_keyboard_inhibit.o build/interop_keyboard_repeat_abi.o \
	build/interop_keymap_names.o
build/interop_keymap_names.o: src/interop/keymap_names.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -femit-bin=$@
build/session_input_handler.o: src/interop/session_input_handler.c src/interop/session_input_handler.h src/interop/session_handler.h src/interop/keymap_names.h build/interop_codec.o | build
	$(CC) $(SessionInputSanitizerFlags) -c $< -o $@
build/session_input_handler_test.o: tests/interop/session_input_handler_test.c src/interop/session_input_handler.h | build
	$(CC) $(SessionInputSanitizerFlags) -c $< -o $@
build/session_input_handler_test: build/session_input_handler_test.o $(SessionInputObjects) | build
	$(CC) $(SessionInputSanitizerFlags) build/session_input_handler_test.o $(SessionInputObjects) -Wl,--wrap=interop_host_feature_send,--wrap=clock_gettime $(LDFLAGS) -o $@

.PHONY: interop-keyboard-lanes-sanitizers
interop-keyboard-lanes-sanitizers: build/interop_keyboard_lock_abi_test build/interop_keyboard_layouts_abi_test build/interop_keyboard_inhibit_abi_test build/interop_keyboard_repeat_abi_test build/interop_keyboard_repeat_policy_test build/session_input_handler_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_lock_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_layouts_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_inhibit_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_repeat_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_repeat_policy_test
	$(KeyboardLaneSanitizerEnv) ./build/session_input_handler_test

.PHONY: interop-keyboard-lanes-test
interop-keyboard-lanes-test: interop-keyboard-lanes-sanitizers
	$(ZIG) test src/interop/keyboard_lock.zig
	$(ZIG) test src/interop/keyboard_layouts.zig
	$(ZIG) test src/interop/keyboard_inhibit.zig
	$(ZIG) test src/interop/keyboard_repeat.zig
	$(ZIG) test src/interop/keyboard_repeat_abi.zig
	$(ZIG) test src/interop/keymap_names.zig

.PHONY: interop-keyboard-lanes-coverage
interop-keyboard-lanes-coverage:
	python3 tests/av/coverage.py keyboard_lock
	python3 tests/av/coverage.py keyboard_layouts
	python3 tests/av/coverage.py keyboard_inhibit
	python3 tests/av/coverage.py keyboard_repeat
	python3 tests/av/coverage.py keyboard_repeat_abi
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/session_input_coverage.py

InteropLaneTests += interop-keyboard-lanes-test
InteropLaneSanitizers += interop-keyboard-lanes-sanitizers
InteropLaneCoverage += interop-keyboard-lanes-coverage
