# Seamless-interop targets (impl/seamless-interop). Host dependencies come from
# base system packages; Wayland protocol XML is pinned by TODO #7.

.PHONY: interop-test
# Hardware-free gates for the seamless-interop feature.
interop-test: interop-desktop-artifacts-test interop-icon-ownership-test interop-window-state-test interop-keyboard-repeat-test interop-mimeapps-test interop-app-catalog-test interop-png-writer-test interop-pe-header-test interop-host-control-test interop-keyboard-scan-test interop-windows-session-unit-test interop-manifest-store-test interop-windows-socket-test interop-host-session-test interop-mapping-linux-test interop-native-runner-test interop-codec-test interop-layout-test interop-heap-test interop-manifest-test interop-layout-abi-test interop-native-atomic-test interop-qemu-build-test interop-vendor-copy-test interop-canonical-path-test interop-slot-runtime-test interop-device-smoke-cleanup-test interop-slot-runtime-abi-test interop-mapping-init-test interop-stream-reader-test interop-stream-send-queue-test interop-session-liveness-test interop-session-handshake-test interop-agent-version-test interop-request-ids-test interop-host-buffer-registry-test interop-control-peer-test interop-control-peer-abi-test interop-mapping-windows-test interop-linux-socket-test interop-linux-session-test
	bash tests/interop/verify_m0.sh

build/interop_codec.o: src/interop/interop_codec.zig src/interop/agent_version.zig include/waddle/interop_protocol.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/interop_codec_windows.lib: src/interop/interop_codec.zig src/interop/agent_version.zig include/waddle/interop_protocol.h | build
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
interop-sanitizers: interop-desktop-artifacts-test interop-icon-ownership-test interop-window-state-test interop-keyboard-repeat-test interop-mimeapps-test interop-pe-header-test interop-manifest-store-test interop-layout-test interop-heap-test interop-manifest-test interop-slot-runtime-test interop-stream-reader-test interop-stream-send-queue-test interop-session-liveness-test interop-session-handshake-test interop-agent-version-test interop-request-ids-test interop-host-buffer-registry-test interop-control-peer-test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 $(MAKE) -B interop-pe-header-abi-test interop-codec-test interop-layout-abi-test interop-native-atomic-test interop-canonical-path-test interop-slot-runtime-abi-test interop-mapping-init-test interop-mapping-windows-test interop-control-peer-abi-test interop-linux-socket-test interop-linux-session-test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"
# >= 90% production line and branch coverage for each implemented interop module.
interop-coverage: build/interop_codec.o build/interop_control_peer_abi.o build/interop_layout_abi.o build/daemon_wire.o build/device_storage.o build/daemon_config.o build/path_rules.o
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/windows_socket_owner_coverage.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/session_control_windows_coverage.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/session_host_coverage.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/mapping_linux_coverage.py
	python3 tests/av/coverage.py interop_codec
	python3 tests/av/coverage.py interop_layout
	python3 tests/av/coverage.py interop_heap
	python3 tests/av/coverage.py interop_slot_runtime
	python3 tests/av/coverage.py interop_slot_runtime_abi
	python3 tests/av/coverage.py stream_reader
	python3 tests/av/coverage.py stream_send_queue
	python3 tests/av/coverage.py session_liveness
	python3 tests/av/coverage.py session_handshake
	python3 tests/av/coverage.py agent_version
	python3 tests/av/coverage.py request_ids
	python3 tests/av/coverage.py host_buffer_registry
	python3 tests/av/coverage.py control_peer
	python3 tests/av/coverage.py control_peer_abi
	python3 tests/av/coverage.py manifest
	python3 tests/av/coverage.py manifest_store
	python3 tests/av/coverage.py desktop_artifacts
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/app_catalog_coverage.py
	python3 tests/av/coverage.py app_catalog_validation
	python3 tests/av/coverage.py window_state
	python3 tests/av/coverage.py keyboard_repeat
	python3 tests/av/coverage.py mimeapps
	python3 tests/av/coverage.py pe_header
	python3 tests/av/coverage.py interop_layout_abi
	python3 tests/interop/native_atomic_coverage.py
	python3 tests/interop/canonical_coverage.py
	python3 tests/interop/mapping_init_coverage.py
	python3 tests/interop/mapping_windows_coverage.py
	python3 tests/interop/linux_socket_owner_coverage.py
	python3 tests/interop/linux_session_owner_coverage.py
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

.PHONY: interop-stream-send-queue-test
# Owning frame FIFO; testing allocator covers OOM and teardown paths.
interop-stream-send-queue-test: build/interop_codec.o
	$(ZIG) test src/interop/stream_send_queue.zig -Iinclude build/interop_codec.o -lc
build/interop_stream_send_queue_test.exe: src/interop/stream_send_queue.zig build/interop_codec_windows.lib include/waddle/interop_protocol.h | build
	$(ZIG) test $< -Iinclude build/interop_codec_windows.lib -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

.PHONY: interop-session-liveness-test
# Ready-only value machine; no socket, request allocator or mapping ownership.
interop-session-liveness-test:
	$(ZIG) test src/interop/session_liveness.zig -Iinclude -lc
build/interop_session_liveness_test.exe: src/interop/session_liveness.zig include/waddle/interop_protocol.h | build
	$(ZIG) test $< -Iinclude -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

.PHONY: interop-session-handshake-test
# Typed negotiation only; external owner validates and drains live mappings.
interop-session-handshake-test:
	$(ZIG) test src/interop/session_handshake.zig -Iinclude -lc
build/interop_session_handshake_test.exe: src/interop/session_handshake.zig include/waddle/interop_protocol.h | build
	$(ZIG) test $< -Iinclude -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

.PHONY: interop-agent-version-test
# Complete bounded SemVer2 grammar; caller supplies borrowed input.
interop-agent-version-test:
	$(ZIG) test src/interop/agent_version.zig -lc
build/interop_agent_version_test.exe: src/interop/agent_version.zig | build
	$(ZIG) test $< -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

.PHONY: interop-request-ids-test
# Common fixed request namespace; response/resource lifetimes remain external.
interop-request-ids-test:
	$(ZIG) test src/interop/request_ids.zig -lc
build/interop_request_ids_test.exe: src/interop/request_ids.zig | build
	$(ZIG) test $< -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@

# Same source against narrow Linux doubles and actual Windows SDK declarations.
InteropMappingWindowsFixtureFlags = -DInteropMappingWindowsTestApi='"mapping_windows_api_double.h"' -Isrc/interop -Itests/interop -Itests/interop/win32_shim -Isubmodules/looking_glass/vendor/ivshmem
InteropMappingWindowsDependencies = src/interop/interop_mapping_windows.c src/interop/interop_mapping_windows.h src/interop/interop_ivshmem_driver.h tests/interop/mapping_windows_api_double.h tests/interop/win32_shim/initguid.h submodules/looking_glass/vendor/ivshmem/ivshmem.h
.PHONY: interop-mapping-windows-test
build/interop_mapping_windows_test: tests/interop/mapping_windows.c $(InteropMappingWindowsDependencies) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(InteropMappingWindowsFixtureFlags) $< $(LDFLAGS) -o $@
interop-mapping-windows-test: build/interop_mapping_windows_test
	./build/interop_mapping_windows_test
build/interop_mapping_windows_sdk.o: src/interop/interop_mapping_windows.c $(InteropMappingWindowsDependencies) | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/interop -Isubmodules/looking_glass/vendor/ivshmem -c $< -o $@
build/interop_mapping_windows_test.exe: tests/interop/mapping_windows.c $(InteropMappingWindowsDependencies) | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Iinclude -Isrc/interop -Isubmodules/looking_glass/vendor/ivshmem $< -o $@

.PHONY: interop-host-buffer-registry-test
# Pure exact-ticket ownership ledger; actual compositor callbacks remain external.
interop-host-buffer-registry-test:
	$(ZIG) test src/interop/host_buffer_registry.zig -lc
	$(ZIG) test --dep registry -Mroot=tests/interop/host_buffer_registry.zig -Mregistry=src/interop/host_buffer_registry.zig -lc
build/interop_host_buffer_registry_test.exe: src/interop/host_buffer_registry.zig src/interop/interop_layout.zig | build
	$(ZIG) test $< -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=$@
build/interop_host_buffer_registry_adversarial_test.exe: tests/interop/host_buffer_registry.zig src/interop/host_buffer_registry.zig src/interop/interop_layout.zig | build
	$(ZIG) test -target x86_64-windows-gnu --dep registry -Mroot=tests/interop/host_buffer_registry.zig -Mregistry=src/interop/host_buffer_registry.zig -lc --test-no-exec -femit-bin=$@

.PHONY: interop-control-peer-test
InteropControlPeerDependencies = src/interop/control_peer.zig src/interop/stream_reader.zig src/interop/stream_send_queue.zig src/interop/session_handshake.zig src/interop/session_liveness.zig src/interop/request_ids.zig src/interop/agent_version.zig include/waddle/interop_protocol.h
# Connected pure control composition, tested through independent named-module peers.
interop-control-peer-test: build/interop_codec.o
	$(ZIG) test build/interop_codec.o -lc -Iinclude --dep peer -Mroot=tests/interop/control_peer.zig -Iinclude -Mpeer=src/interop/control_peer.zig
build/interop_control_peer_test.exe: tests/interop/control_peer.zig $(InteropControlPeerDependencies) build/interop_codec_windows.lib | build
	$(ZIG) test build/interop_codec_windows.lib -lc -target x86_64-windows-gnu -Iinclude --dep peer -Mroot=tests/interop/control_peer.zig -Iinclude -Mpeer=src/interop/control_peer.zig --test-no-exec -femit-bin=$@

.PHONY: interop-control-peer-abi-test
# Pure opaque owner; C clients always receive copied TX/RX storage.
build/interop_control_peer_abi.o: src/interop/control_peer_abi.zig include/waddle/interop_peer.h $(InteropControlPeerDependencies) | build
	$(ZIG) build-obj $< -Iinclude -lc -O ReleaseSafe -fPIC -fcompiler-rt -femit-bin=$@
# This non-sanitized test-driver object is linked into Zig's bounds-checked unit suite.
# The independent C executable below is separately rebuilt under ASan/LSan/UBSan.
build/interop_control_peer_abi_fixture.o: tests/interop/control_peer_abi.c include/waddle/interop_peer.h include/waddle/interop_protocol.h | build
	$(CC) -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Dmain=interop_peer_c_fixture_main -Iinclude -c $< -o $@
build/interop_control_peer_abi_c_test: tests/interop/control_peer_abi.c build/interop_control_peer_abi.o build/interop_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< build/interop_control_peer_abi.o build/interop_codec.o $(LDFLAGS) -o $@
interop-control-peer-abi-test: build/interop_control_peer_abi_c_test build/interop_control_peer_abi_fixture.o
	./build/interop_control_peer_abi_c_test
	$(ZIG) test src/interop/control_peer_abi.zig -Iinclude -lc build/interop_codec.o build/interop_control_peer_abi_fixture.o
build/interop_control_peer_abi_windows.lib: src/interop/control_peer_abi.zig include/waddle/interop_peer.h $(InteropControlPeerDependencies) | build
	$(ZIG) build-lib $< -Iinclude -lc -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/interop_control_peer_abi_c_test.exe: tests/interop/control_peer_abi.c build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Iinclude $< build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib -o $@
build/interop_control_peer_abi_fixture_windows.o: tests/interop/control_peer_abi.c include/waddle/interop_peer.h include/waddle/interop_protocol.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Dmain=interop_peer_c_fixture_main -Iinclude -c $< -o $@
build/interop_control_peer_abi_unit_test.exe: src/interop/control_peer_abi.zig include/waddle/interop_peer.h $(InteropControlPeerDependencies) build/interop_control_peer_abi_fixture_windows.o build/interop_codec_windows.lib | build
	$(ZIG) test $< -Iinclude -lc -target x86_64-windows-gnu build/interop_control_peer_abi_fixture_windows.o build/interop_codec_windows.lib --test-no-exec -femit-bin=$@

# Connected Linux byte ownership only; no VSOCK admission or native release proof.
# The private fixture includes the unchanged C source; build the ordinary object
# separately and verify its exact eight exports to keep the release path checked.
InteropLinuxSocketDependencies = src/interop/linux_socket_owner.c include/waddle/interop_linux_socket.h include/waddle/interop_peer.h include/waddle/interop_protocol.h
InteropLinuxSocketFixtures = tests/interop/linux_socket_owner.c tests/interop/linux_socket_owner_guards.c tests/interop/linux_socket_owner_private.c
InteropLinuxSocketWrapFlags = -Wl,--wrap=calloc -Wl,--wrap=close -Wl,--wrap=fcntl -Wl,--wrap=getsockopt -Wl,--wrap=interop_peer_ack_tx -Wl,--wrap=interop_peer_begin_drain -Wl,--wrap=interop_peer_copy_tx -Wl,--wrap=interop_peer_create -Wl,--wrap=interop_peer_destroy -Wl,--wrap=interop_peer_enqueue_feature -Wl,--wrap=interop_peer_feed -Wl,--wrap=interop_peer_get_state -Wl,--wrap=interop_peer_mapping_result -Wl,--wrap=interop_peer_next_event -Wl,--wrap=interop_peer_pending_bytes -Wl,--wrap=interop_peer_tick -Wl,--wrap=interop_peer_transport_lost -Wl,--wrap=interop_peer_tx_bytes -Wl,--wrap=recv -Wl,--wrap=send
.PHONY: interop-linux-socket-test
build/interop_linux_socket_owner.o: $(InteropLinuxSocketDependencies) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c $< -o $@
build/interop_linux_socket_test: $(InteropLinuxSocketFixtures) $(InteropLinuxSocketDependencies) build/interop_control_peer_abi.o build/interop_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(InteropLinuxSocketFixtures) build/interop_control_peer_abi.o build/interop_codec.o $(LDFLAGS) $(InteropLinuxSocketWrapFlags) -pthread -o $@
interop-linux-socket-test: build/interop_linux_socket_owner.o build/interop_linux_socket_test
	python3 tests/interop/linux_socket_owner_abi.py build/interop_linux_socket_owner.o
	./build/interop_linux_socket_test

# Per-device lease/one-attempt admission mechanics; not installed runtime acceptance.
LinuxSessionOwnerWrapFlags = -Wl,--wrap=socket,--wrap=connect,--wrap=poll,--wrap=getsockopt,--wrap=getpeername,--wrap=clock_gettime,--wrap=calloc,--wrap=close,--wrap=fstatat,--wrap=fstat,--wrap=flock,--wrap=interop_linux_socket_create,--wrap=interop_linux_socket_destroy
build/interop_linux_session_owner_test: tests/interop/linux_session_owner.c tests/interop/linux_session_owner_private.c src/interop/linux_session_owner.c src/interop/linux_session_owner.h src/interop/linux_socket_owner.c src/daemon/daemon_device.c build/interop_control_peer_abi.o build/interop_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -ffunction-sections -fdata-sections tests/interop/linux_session_owner.c tests/interop/linux_session_owner_private.c src/interop/linux_socket_owner.c src/daemon/daemon_device.c build/interop_control_peer_abi.o build/interop_codec.o $(LDFLAGS) $(LinuxSessionOwnerWrapFlags) -Wl,--gc-sections -pthread -o $@
.PHONY: interop-linux-session-test
interop-linux-session-test: build/interop_linux_session_owner_test
	./build/interop_linux_session_owner_test

# Fault-test the isolated runner without launching QEMU, swtpm or any guest.
.PHONY: interop-native-runner-test
interop-native-runner-test:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/interop -p native_guest_runner_test.py
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/interop -p fresh_native_guest_test.py
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/interop -p fresh_native_install_test.py

# Borrowed Linux backing descriptor -> immutable-header validation -> retryable local cleanup.
InteropMappingLinuxSources = tests/interop/mapping_linux.c tests/interop/mapping_linux_private.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c
InteropMappingLinuxDependencies = src/interop/interop_mapping_linux.c src/interop/interop_mapping_linux.h src/interop/interop_layout.h src/interop/interop_native_atomic.h src/av/av_kvmfr.h submodules/looking_glass/module/kvmfr.h
InteropMappingLinuxWrapFlags = -Wl,--wrap=calloc,--wrap=close,--wrap=fcntl,--wrap=fstat,--wrap=ioctl,--wrap=mmap,--wrap=munmap,--wrap=interop_atomic_validate_mapping,--wrap=interop_layout_validate_shm_header
build/interop_mapping_linux.o: src/interop/interop_mapping_linux.c $(InteropMappingLinuxDependencies) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isubmodules/looking_glass/module -pthread -c $< -o $@
build/interop_mapping_linux_test: $(InteropMappingLinuxSources) $(InteropMappingLinuxDependencies) build/interop_layout_abi.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop -Isubmodules/looking_glass/module $(InteropMappingLinuxSources) build/interop_layout_abi.o $(LDFLAGS) $(InteropMappingLinuxWrapFlags) -pthread -o $@
.PHONY: interop-mapping-linux-test interop-mapping-linux-coverage interop-mapping-linux-sanitizers
interop-mapping-linux-test: build/interop_mapping_linux.o build/interop_mapping_linux_test
	./build/interop_mapping_linux_test
interop-mapping-linux-coverage: build/interop_layout_abi.o
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/mapping_linux_coverage.py
interop-mapping-linux-sanitizers:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -W tests/interop/mapping_linux.c interop-mapping-linux-test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"

# Production daemon readiness + singleton + one-attempt VSOCK orchestration.
InteropHostSessionObjects = build/interop_control_peer_abi.o build/interop_codec.o build/daemon_wire.o build/device_storage.o build/daemon_config.o build/path_rules.o
InteropHostSessionDependencies = src/interop/session_host.c src/interop/session_host.h src/interop/linux_session_owner.c src/interop/linux_session_owner.h src/interop/linux_socket_owner.c src/daemon/daemon_device.c src/daemon/daemon_wire.h include/waddle/daemon_protocol.h
InteropHostSessionFixtures = tests/interop/session_host.c tests/interop/session_host_private.c
InteropHostSessionWrapFlags = -Wl,--wrap=daemon_wire_decode_status_response,--wrap=daemon_wire_decode_ready_response,--wrap=interop_linux_session_acquire,--wrap=interop_linux_session_release,--wrap=socket,--wrap=connect,--wrap=getsockopt,--wrap=getpeername,--wrap=send,--wrap=recv,--wrap=poll,--wrap=clock_gettime,--wrap=interop_linux_session_connect_peer,--wrap=interop_linux_session_destroy_peer
build/interop_host_session.o: src/interop/session_host.c src/interop/session_host.h src/interop/linux_session_owner.h src/daemon/daemon_wire.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c $< -o $@
build/interop_host_session_test: $(InteropHostSessionFixtures) $(InteropHostSessionDependencies) $(InteropHostSessionObjects) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -ffunction-sections -fdata-sections $(InteropHostSessionFixtures) src/interop/linux_session_owner.c src/interop/linux_socket_owner.c src/daemon/daemon_device.c $(InteropHostSessionObjects) $(LDFLAGS) $(InteropHostSessionWrapFlags) -Wl,--gc-sections -pthread -o $@
.PHONY: interop-host-session-test interop-host-session-coverage interop-host-session-sanitizers
interop-host-session-test: build/interop_host_session.o build/interop_host_session_test
	./build/interop_host_session_test
interop-host-session-coverage: $(InteropHostSessionObjects)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/windows_socket_owner_coverage.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/session_host_coverage.py
interop-host-session-sanitizers:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -W tests/interop/session_host.c interop-host-session-test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"


# Connected WinSock byte ownership: Linux syscall double and native SDK loopback.
InteropWindowsSocketFixtures = tests/interop/windows_socket_owner.c tests/interop/windows_socket_owner_private.c tests/interop/windows_socket_owner_guards.c tests/interop/windows_socket_owner_api_double.c
InteropWindowsSocketDependencies = src/interop/windows_socket_owner.c src/interop/windows_socket_owner.h tests/interop/windows_socket_owner_api_double.h
InteropWindowsSocketWrapFlags = -Wl,--wrap=calloc -Wl,--wrap=close -Wl,--wrap=fcntl -Wl,--wrap=getsockopt -Wl,--wrap=interop_peer_ack_tx -Wl,--wrap=interop_peer_begin_drain -Wl,--wrap=interop_peer_copy_tx -Wl,--wrap=interop_peer_create -Wl,--wrap=interop_peer_destroy -Wl,--wrap=interop_peer_enqueue_feature -Wl,--wrap=interop_peer_feed -Wl,--wrap=interop_peer_get_state -Wl,--wrap=interop_peer_mapping_result -Wl,--wrap=interop_peer_next_event -Wl,--wrap=interop_peer_pending_bytes -Wl,--wrap=interop_peer_tick -Wl,--wrap=interop_peer_transport_lost -Wl,--wrap=interop_peer_tx_bytes -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=shutdown
build/interop_windows_socket_test: $(InteropWindowsSocketFixtures) $(InteropWindowsSocketDependencies) build/interop_control_peer_abi.o build/interop_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DInteropWindowsSocketTestApi -UNDEBUG -Itests/interop -Isrc/interop $(InteropWindowsSocketFixtures) build/interop_control_peer_abi.o build/interop_codec.o $(LDFLAGS) $(InteropWindowsSocketWrapFlags) -pthread -o $@
build/interop_windows_socket_sdk.o: src/interop/windows_socket_owner.c src/interop/windows_socket_owner.h include/waddle/interop_peer.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Iinclude -Isrc/interop -c $< -o $@
build/interop_windows_socket_native_test.exe: tests/interop/windows_socket_owner_windows.c build/interop_windows_socket_sdk.o build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Iinclude -Isrc/interop $< build/interop_windows_socket_sdk.o build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib -lws2_32 -o $@
.PHONY: interop-windows-socket-test interop-windows-socket-coverage
interop-windows-socket-test: build/interop_windows_socket_test
	./build/interop_windows_socket_test
interop-windows-socket-coverage: build/interop_control_peer_abi.o build/interop_codec.o
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/windows_socket_owner_coverage.py


# Real rooted filesystem transactions; Zig testing allocator checks ownership.
.PHONY: interop-manifest-store-test
interop-manifest-store-test:
	$(ZIG) test src/interop/manifest_store.zig -lc
	$(ZIG) test -lc --dep manifest_store -Mroot=tests/interop/manifest_store.zig -Mmanifest_store=src/interop/manifest_store.zig
	$(ZIG) test -lc --dep manifest_store -Mroot=tests/interop/manifest_replace.zig -Mmanifest_store=src/interop/manifest_store.zig


# Production Windows controller; trusted freshness/detach provider remains external.
InteropGuestControlDependencies = src/interop/session_control_windows.c src/interop/session_control_windows.h src/interop/windows_socket_owner.h src/interop/interop_mapping_windows.h src/interop/interop_layout.h src/interop/interop_mapping_init.h src/interop/interop_native_atomic.h
InteropGuestControlUnitSources = tests/interop/session_control_windows_private.c tests/interop/session_control_windows_api_double.c src/interop/session_control_windows.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c
build/interop_guest_session_unit_test: $(InteropGuestControlUnitSources) $(InteropGuestControlDependencies) tests/interop/session_control_windows_api_double.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -DInteropGuestSessionTestApi -DInteropWindowsSocketTestApi -Itests/interop -Isrc/interop $(InteropGuestControlUnitSources) $(LDFLAGS) -o $@
.PHONY: interop-windows-session-unit-test
interop-windows-session-unit-test: build/interop_guest_session_unit_test
	./build/interop_guest_session_unit_test
build/interop_guest_session_sdk.o: src/interop/session_control_windows.c $(InteropGuestControlDependencies) | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/interop -c $< -o $@
build/interop_guest_session_integration_test.exe: tests/interop/session_control_windows_integration.c tests/interop/session_control_windows_mapping_seam.c $(InteropGuestControlDependencies) src/interop/windows_socket_owner.c src/interop/interop_mapping_windows.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib build/interop_layout_abi_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Iinclude -Isrc/interop -Isubmodules/looking_glass/module -Isubmodules/looking_glass/vendor/ivshmem tests/interop/session_control_windows_integration.c tests/interop/session_control_windows_mapping_seam.c src/interop/windows_socket_owner.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib build/interop_layout_abi_windows.lib -lws2_32 -lsetupapi -lbcrypt -o $@
build/waddle-session-guest.exe: src/interop/session_guest_main.c $(InteropGuestControlDependencies) build/interop_guest_session_sdk.o build/interop_windows_socket_sdk.o build/interop_mapping_windows_sdk.o src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib build/interop_layout_abi_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/interop $< build/interop_guest_session_sdk.o build/interop_windows_socket_sdk.o build/interop_mapping_windows_sdk.o src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c build/interop_control_peer_abi_windows.lib build/interop_codec_windows.lib build/interop_layout_abi_windows.lib -lws2_32 -lsetupapi -lbcrypt -o $@


# Full typed keyboard table plus legacy state/bounds and leak-enabled C gate.
.PHONY: interop-keyboard-scan-test
interop-keyboard-scan-test: build/av_codec.o
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/keyboard_scan_coverage.py

# Actual bounded host session entry; advertises no application capabilities.
InteropHostControlSources = src/interop/session_control_host.c src/interop/session_host.c src/interop/linux_session_owner.c src/interop/linux_socket_owner.c src/interop/interop_mapping_linux.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c src/daemon/daemon_device.c
InteropHostControlObjects = $(InteropHostSessionObjects) build/interop_layout_abi.o
InteropHostControlHeaders = include/waddle/interop_linux_socket.h include/waddle/interop_peer.h include/waddle/interop_protocol.h include/waddle/interop_memory.h src/interop/session_control_host.h src/interop/interop_mapping_linux.h src/interop/interop_mapping_init.h src/interop/interop_native_atomic.h $(InteropHostSessionDependencies)
build/waddle-session: src/interop/session_main.c $(InteropHostControlSources) $(InteropHostControlHeaders) $(InteropHostControlObjects) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/interop -Isubmodules/looking_glass/module src/interop/session_main.c $(InteropHostControlSources) $(InteropHostControlObjects) $(LDFLAGS) -pthread -o $@

all: build/waddle-session
.PHONY: interop-host-control-test
interop-host-control-test: build/waddle-session $(InteropHostControlObjects)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/session_control_host_coverage.py

# Bounded PE classifier; malformed bytes route GUI, file I/O remains caller-owned.
.PHONY: interop-pe-header-test interop-pe-header-abi-test
build/interop_pe_header.o: src/interop/pe_header.zig src/interop/pe_header.h | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/interop_pe_header_c_fixture.o: tests/interop/pe_header.c src/interop/pe_header.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Dmain=interop_pe_c_fixture_main -Isrc/interop -c $< -o $@
build/interop_pe_header_abi_test: tests/interop/pe_header.c src/interop/pe_header.h build/interop_pe_header.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop $< build/interop_pe_header.o $(LDFLAGS) -o $@
interop-pe-header-abi-test: build/interop_pe_header_abi_test
	./build/interop_pe_header_abi_test
interop-pe-header-test: interop-pe-header-abi-test build/interop_pe_header_c_fixture.o
	$(ZIG) test src/interop/pe_header.zig build/interop_pe_header_c_fixture.o -lc
build/interop_pe_header_windows.lib: src/interop/pe_header.zig src/interop/pe_header.h | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/interop_pe_header_abi_test.exe: tests/interop/pe_header.c src/interop/pe_header.h build/interop_pe_header_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Isrc/interop $< build/interop_pe_header_windows.lib -o $@

# Frozen icon sizes, independent PNG/zlib checks and leak-enabled C qualification.
.PHONY: interop-png-writer-test
interop-png-writer-test:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/png_writer_coverage.py

# Native SDK/profile test is separate from portable PNG acceptance.
WindowsComponentCC ?= x86_64-w64-mingw32-gcc
WindowsComponentFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -D_WIN32_WINNT=0x0601 -Isrc/interop -O0 -g
build/native_components: | build
	mkdir -p $@
build/native_components/icon_fixture.o: tests/interop/icon_windows.c tests/interop/icon_windows_faults.inc src/interop/icon_windows.c src/interop/icon_windows.h src/interop/png_writer.h | build/native_components
	$(WindowsComponentCC) $(WindowsComponentFlags) -DInteropNativeCoverageProfile --coverage -fprofile-update=atomic -fprofile-abs-path -c $< -o $@
build/native_components/png_writer.o: src/interop/png_writer.c src/interop/png_writer.h | build/native_components
	$(WindowsComponentCC) $(WindowsComponentFlags) -c $< -o $@
build/native_components/icon_windows_profile.exe: build/native_components/icon_fixture.o build/native_components/png_writer.o
	$(WindowsComponentCC) -static --coverage $^ -lshell32 -lole32 -luuid -lgdi32 -lcomctl32 -o $@


# Catalog source core and native adapter are qualified separately.
.PHONY: interop-app-catalog-test
build/interop_app_catalog_validation.o: src/interop/app_catalog_validation.zig src/interop/app_catalog_validation.h | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/interop_app_catalog_test: tests/interop/app_catalog.c src/interop/app_catalog.c src/interop/app_catalog.h src/interop/app_catalog_validation.h build/interop_app_catalog_validation.o build/interop_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop tests/interop/app_catalog.c src/interop/app_catalog.c build/interop_app_catalog_validation.o build/interop_codec.o $(LDFLAGS) -o $@
interop-app-catalog-test: build/interop_app_catalog_test
	$(ZIG) test src/interop/app_catalog_validation.zig
	./build/interop_app_catalog_test
build/interop_app_catalog_validation_windows.lib: src/interop/app_catalog_validation.zig src/interop/app_catalog_validation.h | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/native_components/catalog_adapter.o: tests/interop/app_catalog_windows_faults.c src/interop/app_catalog_windows.c src/interop/app_catalog_windows_private.h src/interop/app_catalog.h | build/native_components
	$(WindowsComponentCC) $(WindowsComponentFlags) --coverage -fprofile-update=atomic -fprofile-abs-path -Iinclude -c $< -o $@
build/native_components/catalog_core.o: src/interop/app_catalog.c src/interop/app_catalog.h src/interop/app_catalog_validation.h | build/native_components
	$(WindowsComponentCC) $(WindowsComponentFlags) -Iinclude -c $< -o $@
build/native_components/catalog_fixture.o: tests/interop/app_catalog_windows.c src/interop/app_catalog.h src/interop/app_catalog_windows_private.h | build/native_components
	$(WindowsComponentCC) $(WindowsComponentFlags) -Iinclude -c $< -o $@
build/native_components/app_catalog_windows_profile.exe: build/native_components/catalog_adapter.o build/native_components/catalog_core.o build/native_components/catalog_fixture.o build/interop_app_catalog_validation_windows.lib build/interop_codec_windows.lib
	$(WindowsComponentCC) -static --coverage $^ -lshell32 -lole32 -luuid -lpropsys -loleaut32 -lntdll -o $@

.PHONY: interop-mimeapps-test
interop-mimeapps-test:
	$(ZIG) test src/interop/mimeapps.zig -lc

.PHONY: interop-keyboard-repeat-test
interop-keyboard-repeat-test:
	$(ZIG) test src/interop/keyboard_repeat.zig

# Fixed-size owner-thread state/action reducer; imported ABI headers are frozen.
.PHONY: interop-window-state-test
interop-window-state-test:
	$(ZIG) test src/interop/window_state.zig -Iinclude -lc

# Shared image allocator owner under real Linux memory/leak sanitizers.
# Native Shell/GDI qualification remains in the Windows component workflow.
build/interop_icon_ownership_sanitized_test: tests/interop/icon_windows_ownership.c src/interop/icon_windows.c src/interop/icon_windows.h | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer -no-pie tests/interop/icon_windows_ownership.c src/interop/icon_windows.c -o $@
.PHONY: interop-icon-ownership-test
interop-icon-ownership-test: build/interop_icon_ownership_sanitized_test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/interop_icon_ownership_sanitized_test

# The Zig producer uses the testing allocator/OOM sweeps. Its real GLib C
# consumer is compiled by the fixture with ASan/LSan/UBSan and detect_leaks=1.
.PHONY: interop-desktop-artifacts-test
interop-desktop-artifacts-test:
	$(ZIG) test -lc -O ReleaseSafe src/interop/desktop_artifacts.zig
