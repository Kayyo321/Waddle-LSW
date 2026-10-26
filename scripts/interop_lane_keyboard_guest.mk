# Guest keyboard executor lane (seamless interop, IMPL_DESC 3.6.2, 3.6.3, 6.3). Owns only the
# interop-keyboard-guest-* targets below. Included by GNUmakefile through scripts/interop_lane_*.mk.
# The Linux test is built with ASan, LSan and UBSan and gated by the coverage script. The Windows
# smoke program is cross-built here and executed only on a Windows CI runner with an interactive desktop.

GuestInputSanitizerFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g \
	-fsanitize=address,leak,undefined -fno-omit-frame-pointer -Iinclude -Isrc/interop -Isrc/av
GuestInputSanitizerEnv = ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
GuestInputLinuxObjects = build/interop_keyboard_lock.o build/interop_keyboard_layouts.o
GuestInputWinObjects = build/guest_input_win_keyboard_lock.o build/guest_input_win_keyboard_layouts.o
GuestInputMingwCC ?= x86_64-w64-mingw32-gcc

# Linux test binary. av_input.c is compiled in only for the equivalence check; it needs the AV codec object.
build/guest_input_test: tests/interop/guest_input_test.c src/interop/guest_input.c src/interop/guest_input.h src/interop/guest_win32_compat.h src/av/av_input.c src/av/av_input.h build/av_codec.o $(GuestInputLinuxObjects) | build
	$(CC) $(GuestInputSanitizerFlags) tests/interop/guest_input_test.c src/interop/guest_input.c src/av/av_input.c build/av_codec.o $(GuestInputLinuxObjects) $(LDFLAGS) -o $@

.PHONY: interop-keyboard-guest-test
interop-keyboard-guest-test: build/guest_input_test
	$(GuestInputSanitizerEnv) ./build/guest_input_test
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/guest_input_coverage.py

# Windows objects for the cross-built smoke program: the same Zig modules the Linux build links, built for the Windows target.
build/guest_input_win_keyboard_lock.o: src/interop/keyboard_lock.zig | build
	$(ZIG) build-obj $< -target x86_64-windows-gnu -O ReleaseSafe -femit-bin=$@
build/guest_input_win_keyboard_layouts.o: src/interop/keyboard_layouts.zig | build
	$(ZIG) build-obj $< -target x86_64-windows-gnu -O ReleaseSafe -femit-bin=$@

# Native Windows smoke program. Built with zig cc for x86_64-windows-gnu; not executed on Linux.
build/interop_guest_input_native_test.exe: tests/interop/guest_input_native_test.c src/interop/guest_input.c src/interop/guest_input.h src/interop/guest_win32_compat.h src/interop/keyboard_lock.h src/interop/keyboard_layouts.h $(GuestInputWinObjects) | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/interop tests/interop/guest_input_native_test.c src/interop/guest_input.c $(GuestInputWinObjects) -luser32 -lkernel32 -o $@

# Standalone mingw compile of the executor (windows.h path) with -Werror. Compile only.
build/guest_input_mingw.o: src/interop/guest_input.c src/interop/guest_input.h src/interop/guest_win32_compat.h src/interop/keyboard_lock.h src/interop/keyboard_layouts.h | build
	$(GuestInputMingwCC) -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/interop -c $< -o $@

.PHONY: interop-keyboard-guest-native-cross
interop-keyboard-guest-native-cross: build/interop_guest_input_native_test.exe build/guest_input_mingw.o

InteropLaneTests += interop-keyboard-guest-test interop-keyboard-guest-native-cross
InteropLaneSanitizers += interop-keyboard-guest-test
