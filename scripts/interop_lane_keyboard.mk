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
build/interop_keyboard_inhibit.o: src/interop/keyboard_inhibit.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -femit-bin=$@
build/interop_keyboard_inhibit_abi_test: tests/interop/keyboard_inhibit_abi.c build/interop_keyboard_inhibit.o src/interop/keyboard_inhibit.h | build
	$(CC) $(KeyboardLaneSanitizerFlags) $< build/interop_keyboard_inhibit.o $(LDFLAGS) -o $@

.PHONY: interop-keyboard-lanes-sanitizers
interop-keyboard-lanes-sanitizers: build/interop_keyboard_lock_abi_test build/interop_keyboard_layouts_abi_test build/interop_keyboard_inhibit_abi_test build/interop_keyboard_repeat_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_lock_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_layouts_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_inhibit_abi_test
	$(KeyboardLaneSanitizerEnv) ./build/interop_keyboard_repeat_abi_test

.PHONY: interop-keyboard-lanes-test
interop-keyboard-lanes-test: interop-keyboard-lanes-sanitizers
	$(ZIG) test src/interop/keyboard_lock.zig
	$(ZIG) test src/interop/keyboard_layouts.zig
	$(ZIG) test src/interop/keyboard_inhibit.zig
	$(ZIG) test src/interop/keyboard_repeat.zig
	$(ZIG) test src/interop/keyboard_repeat_abi.zig

.PHONY: interop-keyboard-lanes-coverage
interop-keyboard-lanes-coverage:
	python3 tests/av/coverage.py keyboard_lock
	python3 tests/av/coverage.py keyboard_layouts
	python3 tests/av/coverage.py keyboard_inhibit
	python3 tests/av/coverage.py keyboard_repeat
	python3 tests/av/coverage.py keyboard_repeat_abi

InteropLaneTests += interop-keyboard-lanes-test
InteropLaneSanitizers += interop-keyboard-lanes-sanitizers
InteropLaneCoverage += interop-keyboard-lanes-coverage
