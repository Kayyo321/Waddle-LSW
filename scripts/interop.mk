# Seamless-interop targets (impl/seamless-interop). Host dependencies come from
# base system packages; Wayland protocol XML is pinned by TODO #7.

.PHONY: interop-test
# Hardware-free gates for the seamless-interop feature.
interop-test: interop-codec-test interop-layout-test
	bash tests/interop/verify_m0.sh

build/interop_codec.o: src/interop/interop_codec.zig include/waddle/interop_protocol.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/interop_codec_windows.lib: src/interop/interop_codec.zig include/waddle/interop_protocol.h | build
	$(ZIG) build-lib $< -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/interop_codec_abi_test: tests/interop/codec_abi.c build/interop_codec.o include/waddle/interop_memory.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG tests/interop/codec_abi.c build/interop_codec.o $(LDFLAGS) -o $@

.PHONY: interop-codec-test interop-sanitizers interop-coverage
# Zig unit/fuzz tests plus the C ABI round trip.
interop-codec-test: build/interop_codec_abi_test
	$(ZIG) test src/interop/interop_codec.zig -Iinclude -lc
	./build/interop_codec_abi_test
# C consumers under ASan/LSan/UBSan; zero leaks required.
interop-sanitizers:
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 $(MAKE) -B interop-codec-test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"
# >= 90% line and branch coverage of the interop codec.
interop-coverage:
	python3 tests/av/coverage.py interop_codec
	python3 tests/av/coverage.py interop_layout
# Windows link check of the same C ABI test (executed by guest_windows CI).
build/interop_codec_abi_test.exe: tests/interop/codec_abi.c build/interop_codec_windows.lib include/waddle/interop_memory.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/interop/codec_abi.c build/interop_codec_windows.lib -o $@

.PHONY: interop-layout-test interop-layout-coverage

# Bounds-checked snapshot/geometry primitives; native mapping ownership is separate.
interop-layout-test:
	$(ZIG) test src/interop/interop_layout.zig -Iinclude -lc
interop-layout-coverage:
	python3 tests/av/coverage.py interop_layout
