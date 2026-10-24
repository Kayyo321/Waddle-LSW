# Seamless-interop targets (impl/seamless-interop). Host dependencies come from
# base system packages; Wayland protocol XML is pinned by TODO #7.

.PHONY: interop-test
# Hardware-free gates for the seamless-interop feature.
interop-test: interop-codec-test interop-layout-test interop-heap-test interop-manifest-test interop-layout-abi-test interop-native-atomic-test interop-qemu-build-test interop-vendor-copy-test interop-canonical-path-test interop-slot-runtime-test interop-device-smoke-cleanup-test interop-slot-runtime-abi-test interop-mapping-init-test interop-stream-reader-test
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
# Bounds-checked Zig/testing-allocator suites plus C consumers under ASan/LSan/UBSan.
# Native C mapping adapters remain a separate acceptance subtask.
interop-sanitizers: interop-layout-test interop-heap-test interop-manifest-test interop-slot-runtime-test interop-stream-reader-test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 $(MAKE) -B interop-codec-test interop-layout-abi-test interop-native-atomic-test interop-canonical-path-test interop-slot-runtime-abi-test interop-mapping-init-test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"
# >= 90% production line and branch coverage for each implemented interop module.
interop-coverage: build/interop_codec.o
	python3 tests/av/coverage.py interop_codec
	python3 tests/av/coverage.py interop_layout
	python3 tests/av/coverage.py interop_heap
	python3 tests/av/coverage.py interop_slot_runtime
	python3 tests/av/coverage.py interop_slot_runtime_abi
	python3 tests/av/coverage.py stream_reader
	python3 tests/av/coverage.py manifest
	python3 tests/av/coverage.py interop_layout_abi
	python3 tests/interop/native_atomic_coverage.py
	python3 tests/interop/canonical_coverage.py
	python3 tests/interop/mapping_init_coverage.py
# Windows link check of the same C ABI test (executed by guest_windows CI).
build/interop_codec_abi_test.exe: tests/interop/codec_abi.c build/interop_codec_windows.lib include/waddle/interop_memory.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/interop/codec_abi.c build/interop_codec_windows.lib -o $@

.PHONY: interop-layout-test interop-layout-coverage

# Bounds-checked snapshot/geometry primitives; native mapping ownership is separate.
interop-layout-test:
	$(ZIG) test src/interop/interop_layout.zig -Iinclude -lc
interop-layout-coverage:
	python3 tests/av/coverage.py interop_layout

.PHONY: interop-heap-test interop-heap-coverage
# Pure reservations; capture pins and live mapping ownership remain separate.
interop-heap-test:
	$(ZIG) test src/interop/interop_heap.zig -lc
interop-heap-coverage:
	python3 tests/av/coverage.py interop_heap

.PHONY: interop-manifest-test interop-manifest-coverage
# Owned JSON parsing; testing allocator checks every allocation failure path.
interop-manifest-test:
	$(ZIG) test src/interop/manifest.zig -lc
interop-manifest-coverage:
	python3 tests/av/coverage.py manifest
	python3 tests/av/coverage.py interop_layout_abi

# Native Windows unit executables; cross-build is distinct from actual execution.
build/interop_layout_test.exe: src/interop/interop_layout.zig | build
	$(ZIG) test $< -Iinclude -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@
build/interop_heap_test.exe: src/interop/interop_heap.zig src/interop/interop_layout.zig | build
	$(ZIG) test $< -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@
build/interop_manifest_test.exe: src/interop/manifest.zig | build
	$(ZIG) test $< -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

.PHONY: interop-layout-abi-test
# Private snapshot C boundary; live mapping atomics remain separate.
build/interop_layout_abi.o: src/interop/interop_layout_abi.zig src/interop/interop_layout.zig src/interop/interop_layout.h | build
	$(ZIG) build-obj $< -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/interop_layout_abi_test: tests/interop/layout_abi.c build/interop_layout_abi.o src/interop/interop_layout.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop $< build/interop_layout_abi.o $(LDFLAGS) -o $@
interop-layout-abi-test: build/interop_layout_abi_test
	$(ZIG) test src/interop/interop_layout_abi.zig -Isrc/interop -lc
	./build/interop_layout_abi_test
build/interop_layout_abi_windows.lib: src/interop/interop_layout_abi.zig src/interop/interop_layout.zig src/interop/interop_layout.h | build
	$(ZIG) build-lib $< -Isrc/interop -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/interop_layout_abi_test.exe: tests/interop/layout_abi.c build/interop_layout_abi_windows.lib src/interop/interop_layout.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc/interop $< build/interop_layout_abi_windows.lib -o $@

.PHONY: interop-native-atomic-test interop-qemu-build-test interop-vendor-copy-test interop-canonical-path-test
build/interop_native_atomic_test: src/interop/interop_native_atomic.c src/interop/interop_native_atomic.h include/waddle/interop_memory.h tests/interop/native_atomic.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/interop src/interop/interop_native_atomic.c tests/interop/native_atomic.c $(LDFLAGS) -pthread -o $@
interop-native-atomic-test: build/interop_native_atomic_test
	./build/interop_native_atomic_test
build/interop_native_atomic_test.exe: src/interop/interop_native_atomic.c src/interop/interop_native_atomic.h include/waddle/interop_memory.h tests/interop/native_atomic.c | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc/interop -Iinclude src/interop/interop_native_atomic.c tests/interop/native_atomic.c -o $@
interop-qemu-build-test:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/qemu_build.py
interop-vendor-copy-test:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/vendor_copy.py

.PHONY: interop-canonical-path-test
build/interop_canonical_paths: tests/interop/canonical_paths.c src/common/arguments.c src/common/path_filesystem.c src/common/path_rules.h build/path_rules.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/common/arguments.c src/common/path_filesystem.c build/path_rules.o $(LDFLAGS) -Wl,--wrap=malloc,--wrap=strdup,--wrap=strndup,--wrap=realpath,--wrap=open,--wrap=fstatat,--wrap=openat,--wrap=lstat -o $@
interop-canonical-path-test: build/interop_canonical_paths
	./build/interop_canonical_paths

.PHONY: interop-slot-runtime-test interop-slot-runtime-coverage
# Fixed bookkeeping only; live mapping/capture integration remains separate.
interop-slot-runtime-test:
	$(ZIG) test src/interop/interop_slot_runtime.zig -lc
interop-slot-runtime-coverage:
	python3 tests/av/coverage.py interop_slot_runtime
build/interop_slot_runtime_test.exe: src/interop/interop_slot_runtime.zig src/interop/interop_heap.zig src/interop/interop_layout.zig | build
	$(ZIG) test $< -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

.PHONY: interop-device-smoke-cleanup-test
# Fault-only fixture: no VM, device, ACL or process signaling occurs.
interop-device-smoke-cleanup-test:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/device_smoke_cleanup.py

.PHONY: interop-slot-runtime-abi-test
build/interop_slot_runtime_abi.o: src/interop/interop_slot_runtime_abi.zig src/interop/interop_slot_runtime.zig src/interop/interop_slot_runtime.h src/interop/interop_heap.zig src/interop/interop_layout.zig | build
	$(ZIG) build-obj $< -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/interop_slot_runtime_abi_test: tests/interop/slot_runtime_abi.c build/interop_slot_runtime_abi.o src/interop/interop_slot_runtime.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop $< build/interop_slot_runtime_abi.o $(LDFLAGS) -o $@
interop-slot-runtime-abi-test: build/interop_slot_runtime_abi_test
	$(ZIG) test src/interop/interop_slot_runtime_abi.zig -Isrc/interop -lc
	./build/interop_slot_runtime_abi_test
build/interop_slot_runtime_abi_windows.lib: src/interop/interop_slot_runtime_abi.zig src/interop/interop_slot_runtime.zig src/interop/interop_slot_runtime.h src/interop/interop_heap.zig src/interop/interop_layout.zig | build
	$(ZIG) build-lib $< -Isrc/interop -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@
build/interop_slot_runtime_abi_test.exe: tests/interop/slot_runtime_abi.c build/interop_slot_runtime_abi_windows.lib src/interop/interop_slot_runtime.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Isrc/interop $< build/interop_slot_runtime_abi_windows.lib -o $@
build/interop_slot_runtime_abi_unit_test.exe: src/interop/interop_slot_runtime_abi.zig src/interop/interop_slot_runtime.zig src/interop/interop_slot_runtime.h src/interop/interop_heap.zig src/interop/interop_layout.zig | build
	$(ZIG) test $< -Isrc/interop -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

.PHONY: interop-mapping-init-test
build/interop_mapping_init_test: src/interop/interop_mapping_init.c src/interop/interop_mapping_init.h tests/interop/mapping_init.c include/waddle/interop_memory.h include/waddle/interop_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/interop src/interop/interop_mapping_init.c tests/interop/mapping_init.c $(LDFLAGS) -o $@
interop-mapping-init-test: build/interop_mapping_init_test
	./build/interop_mapping_init_test
build/interop_mapping_init_test.exe: src/interop/interop_mapping_init.c src/interop/interop_mapping_init.h tests/interop/mapping_init.c include/waddle/interop_memory.h include/waddle/interop_protocol.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/interop src/interop/interop_mapping_init.c tests/interop/mapping_init.c -o $@

.PHONY: interop-stream-reader-test
# Parser bounds/testing-allocator proof; no socket or mapping ownership.
interop-stream-reader-test: build/interop_codec.o
	$(ZIG) test src/interop/stream_reader.zig -Iinclude build/interop_codec.o -lc
build/interop_stream_reader_test.exe: src/interop/stream_reader.zig build/interop_codec_windows.lib include/waddle/interop_protocol.h | build
	$(ZIG) test $< -Iinclude build/interop_codec_windows.lib -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@
