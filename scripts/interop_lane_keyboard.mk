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
build/interop_keyboard_layout.o: src/interop/keyboard_layout.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -femit-bin=$@
build/interop_keyboard_layout_abi_test: tests/interop/keyboard_layout_abi.c build/interop_keyboard_layout.o src/interop/keyboard_layout.h | build
	$(CC) $(KeyboardLaneSanitizerFlags) $< build/interop_keyboard_layout.o $(LDFLAGS) -o $@

.PHONY: interop-keyboard-lanes-sanitizers
interop-keyboard-lanes-sanitizers: build/interop_keyboard_lock_abi_test build/interop_keyboard_layout_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_lock_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_layout_abi_test

.PHONY: interop-keyboard-lanes-test
interop-keyboard-lanes-test: interop-keyboard-lanes-sanitizers
	$(ZIG) test src/interop/keyboard_lock.zig
	$(ZIG) test src/interop/keyboard_layout.zig

.PHONY: interop-keyboard-lanes-coverage
interop-keyboard-lanes-coverage:
	python3 tests/av/coverage.py keyboard_lock
	python3 tests/av/coverage.py keyboard_layout

InteropLaneTests += interop-keyboard-lanes-test
InteropLaneSanitizers += interop-keyboard-lanes-sanitizers
InteropLaneCoverage += interop-keyboard-lanes-coverage
