# Authenticated test TCP codec is independent of renderer/shared ICD build outputs.
build/venus_tcp_wire_oracle.o: tests/vgpu/tcp_wire_oracle.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/venus_tcp_wire.o: src/vgpu/venus_tcp_wire.zig include/waddle/venus_tcp.h include/waddle/venus_request.h include/waddle/venus_ring.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_tcp_wire_test: tests/vgpu/tcp_wire.c include/waddle/venus_tcp.h build/venus_tcp_wire.o build/venus_tcp_wire_oracle.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/venus_tcp_wire.o build/venus_tcp_wire_oracle.o $(LDFLAGS) -o $@

.PHONY: vgpu-tcp-wire-test vgpu-tcp-wire-sanitizers vgpu-tcp-wire-coverage vgpu-tcp-wire-windows
vgpu-tcp-wire-test: build/vgpu_tcp_wire_test
	./build/vgpu_tcp_wire_test
	$(ZIG) test src/vgpu/venus_tcp_wire.zig -Iinclude build/venus_tcp_wire_oracle.o -lc -O ReleaseSafe

vgpu-tcp-wire-sanitizers: build/venus_tcp_wire.o
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/tcp_wire.c tests/vgpu/tcp_wire_oracle.c build/venus_tcp_wire.o -o build/vgpu_tcp_wire_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_tcp_wire_sanitized

vgpu-tcp-wire-coverage: build/venus_tcp_wire_oracle.o
	python3 tests/av/coverage.py venus_tcp_wire

build/venus_tcp_wire_windows.lib: src/vgpu/venus_tcp_wire.zig include/waddle/venus_tcp.h | build
	$(ZIG) build-lib $< -static -Iinclude -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_tcp_wire_test.exe: tests/vgpu/tcp_wire.c tests/vgpu/tcp_wire_oracle.c build/venus_tcp_wire_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/tcp_wire.c tests/vgpu/tcp_wire_oracle.c build/venus_tcp_wire_windows.lib -o $@

vgpu-tcp-wire-windows: build/vgpu_tcp_wire_test.exe

VgpuTcpSocketWrapFlags = -Wl,--wrap=socket,--wrap=fcntl,--wrap=bind,--wrap=listen,--wrap=getsockname,--wrap=connect,--wrap=getsockopt,--wrap=accept,--wrap=poll,--wrap=send,--wrap=recv,--wrap=getrandom,--wrap=clock_gettime,--wrap=shutdown

build/vgpu_tcp_transport_test: tests/vgpu/tcp_transport.c src/vgpu/venus_tcp_socket.c include/waddle/venus_tcp.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTcpFaultTests tests/vgpu/tcp_transport.c src/vgpu/venus_tcp_socket.c $(VgpuTcpSocketWrapFlags) -pthread -o $@

.PHONY: vgpu-tcp-transport-test vgpu-tcp-transport-sanitizers vgpu-tcp-transport-windows
vgpu-tcp-transport-test: build/vgpu_tcp_transport_test
	./build/vgpu_tcp_transport_test

vgpu-tcp-transport-sanitizers:
	$(CC) $(CPPFLAGS) -DTcpFaultTests -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/tcp_transport.c src/vgpu/venus_tcp_socket.c $(VgpuTcpSocketWrapFlags) -pthread -o build/vgpu_tcp_transport_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_tcp_transport_sanitized

build/vgpu_tcp_transport_test.exe: tests/vgpu/tcp_transport.c src/vgpu/venus_tcp_socket.c include/waddle/venus_tcp.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/tcp_transport.c src/vgpu/venus_tcp_socket.c -lws2_32 -lbcrypt -o $@

vgpu-tcp-transport-windows: build/vgpu_tcp_transport_test.exe

.PHONY: vgpu-tcp-transport-coverage
vgpu-tcp-transport-coverage:
	python3 tests/vgpu/tcp_transport_coverage.py

build/vgpu_tcp_windows_random_test.exe: tests/vgpu/tcp_windows_random.c src/vgpu/venus_tcp_socket.c include/waddle/venus_tcp.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/tcp_windows_random.c -lws2_32 -lbcrypt -o $@

vgpu-tcp-transport-windows: build/vgpu_tcp_windows_random_test.exe

# Peer units own their codec outputs; no shared ICD/worker/dependency rebuild lease.
VgpuTcpPeerObjects = build/tcp_peer_request.o build/tcp_peer_capabilities.o build/venus_tcp_wire.o
VgpuTcpPeerWrapFlags = -Wl,--wrap=send,--wrap=clock_gettime,--wrap=getrandom,--wrap=shutdown,--wrap=recv
VgpuTcpPeerSources = tests/vgpu/tcp_receiver.c tests/vgpu/tcp_wire_oracle.c src/vgpu/venus_tcp_client.c src/vgpu/venus_tcp_socket.c

build/tcp_peer_request.o: src/vgpu/venus_request.zig src/vgpu/venus_receiver_bounds.zig | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/tcp_peer_capabilities.o: src/vgpu/venus_capabilities.zig include/waddle/venus_capabilities.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/vgpu_tcp_receiver_test: $(VgpuTcpPeerSources) include/waddle/venus_tcp.h $(VgpuTcpPeerObjects) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTcpPeerFaultTests $(VgpuTcpPeerSources) $(VgpuTcpPeerObjects) $(VgpuTcpPeerWrapFlags) -pthread -o $@

.PHONY: vgpu-tcp-client-test vgpu-tcp-client-sanitizers vgpu-tcp-client-coverage vgpu-tcp-client-windows
vgpu-tcp-client-test: build/vgpu_tcp_receiver_test
	./build/vgpu_tcp_receiver_test

vgpu-tcp-client-sanitizers: $(VgpuTcpPeerObjects)
	$(CC) $(CPPFLAGS) -DTcpPeerFaultTests -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuTcpPeerSources) $(VgpuTcpPeerObjects) $(VgpuTcpPeerWrapFlags) -pthread -o build/vgpu_tcp_receiver_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_tcp_receiver_sanitized

vgpu-tcp-client-coverage:
	python3 tests/vgpu/tcp_receiver_coverage.py

build/tcp_peer_request_windows.lib: src/vgpu/venus_request.zig src/vgpu/venus_receiver_bounds.zig | build
	$(ZIG) build-lib $< -static -Iinclude -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/tcp_peer_capabilities_windows.lib: src/vgpu/venus_capabilities.zig include/waddle/venus_capabilities.h | build
	$(ZIG) build-lib $< -static -Iinclude -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

build/vgpu_tcp_receiver_test.exe: $(VgpuTcpPeerSources) include/waddle/venus_tcp.h build/tcp_peer_request_windows.lib build/tcp_peer_capabilities_windows.lib build/venus_tcp_wire_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $(VgpuTcpPeerSources) build/tcp_peer_request_windows.lib build/tcp_peer_capabilities_windows.lib build/venus_tcp_wire_windows.lib -lws2_32 -lbcrypt -o $@

vgpu-tcp-client-windows: build/vgpu_tcp_receiver_test.exe

VgpuTcpServerSources = tests/vgpu/tcp_receiver_server.c tests/vgpu/tcp_wire_oracle.c src/vgpu/venus_tcp_server.c src/vgpu/venus_tcp_socket.c
VgpuTcpServerWrapFlags = -Wl,--wrap=send,--wrap=recv,--wrap=clock_gettime,--wrap=getrandom

build/vgpu_tcp_server_test: $(VgpuTcpServerSources) include/waddle/venus_tcp.h $(VgpuTcpPeerObjects) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTcpPeerFaultTests $(VgpuTcpServerSources) $(VgpuTcpPeerObjects) $(VgpuTcpServerWrapFlags) -pthread -o $@

.PHONY: vgpu-tcp-server-test vgpu-tcp-server-sanitizers vgpu-tcp-server-coverage vgpu-tcp-server-windows
vgpu-tcp-server-test: build/vgpu_tcp_server_test
	./build/vgpu_tcp_server_test

vgpu-tcp-server-sanitizers: $(VgpuTcpPeerObjects)
	$(CC) $(CPPFLAGS) -DTcpPeerFaultTests -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuTcpServerSources) $(VgpuTcpPeerObjects) $(VgpuTcpServerWrapFlags) -pthread -o build/vgpu_tcp_server_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/vgpu_tcp_server_sanitized

vgpu-tcp-server-coverage:
	python3 tests/vgpu/tcp_receiver_coverage.py server

build/vgpu_tcp_server_test.exe: $(VgpuTcpServerSources) include/waddle/venus_tcp.h build/tcp_peer_request_windows.lib build/tcp_peer_capabilities_windows.lib build/venus_tcp_wire_windows.lib | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $(VgpuTcpServerSources) build/tcp_peer_request_windows.lib build/tcp_peer_capabilities_windows.lib build/venus_tcp_wire_windows.lib -lws2_32 -lbcrypt -o $@

vgpu-tcp-server-windows: build/vgpu_tcp_server_test.exe

# Controller owns private codec outputs and snapshots an already-built actual worker.
VgpuTcpBridgeSources = src/vgpu/tcp_bridge_main.c src/vgpu/venus_tcp_server.c src/vgpu/venus_tcp_socket.c src/vgpu/venus_guest.c src/vgpu/venus_rpc.c src/vgpu/venus_channel.c src/vgpu/venus_session.c src/vgpu/venus_region.c src/vgpu/venus_ring.c src/vgpu/venus_wait.c src/vgpu/venus_stream_linux.c src/vgpu/venus_worker.c
VgpuTcpBridgeObjects = build/tcp_bridge_bounds.o build/tcp_bridge_control.o $(VgpuTcpPeerObjects)

build/tcp_bridge_bounds.o: src/vgpu/venus_bounds.zig include/waddle/venus_region.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/tcp_bridge_control.o: src/vgpu/venus_control.zig include/waddle/venus_session.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/waddle_vgpu_tcp_bridge: $(VgpuTcpBridgeSources) $(VgpuTcpBridgeObjects) include/waddle/venus_tcp.h include/waddle/venus_worker.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VgpuTcpBridgeSources) $(VgpuTcpBridgeObjects) $(LDFLAGS) -o $@

.PHONY: vgpu-tcp-bridge-test vgpu-tcp-bridge-coverage vgpu-tcp-bridge-sanitizers
vgpu-tcp-bridge-test vgpu-tcp-bridge-coverage:
	python3 tests/vgpu/tcp_bridge_coverage.py

vgpu-tcp-bridge-sanitizers:
	python3 tests/vgpu/tcp_bridge_coverage.py sanitizers

# Explicit Windows bootstrap owner; no project DLL-entry I/O or shared ICD rebuild.
VgpuTcpBootstrapWindowsSources = src/vgpu/tcp_bootstrap_windows.c src/vgpu/venus_tcp_client.c src/vgpu/venus_tcp_socket.c
VgpuTcpBootstrapWindowsObjects = build/tcp_peer_request_windows.lib build/tcp_peer_capabilities_windows.lib build/venus_tcp_wire_windows.lib

build/waddle_tcp_bootstrap.dll: $(VgpuTcpBootstrapWindowsSources) src/vgpu/tcp_bootstrap_windows.def include/waddle/venus_tcp.h $(VgpuTcpBootstrapWindowsObjects) | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -shared $(VgpuTcpBootstrapWindowsSources) src/vgpu/tcp_bootstrap_windows.def $(VgpuTcpBootstrapWindowsObjects) -lws2_32 -lbcrypt -ladvapi32 -o $@

build/vgpu_tcp_bootstrap_test.exe: tests/vgpu/tcp_bootstrap_windows.c tests/vgpu/tcp_wire_oracle.c src/vgpu/venus_tcp_client.c src/vgpu/venus_tcp_socket.c include/waddle/venus_tcp.h $(VgpuTcpBootstrapWindowsObjects) | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/vgpu/tcp_bootstrap_windows.c tests/vgpu/tcp_wire_oracle.c src/vgpu/venus_tcp_client.c src/vgpu/venus_tcp_socket.c $(VgpuTcpBootstrapWindowsObjects) -lws2_32 -lbcrypt -ladvapi32 -o $@

.PHONY: vgpu-tcp-bootstrap-windows vgpu-tcp-bootstrap-coverage vgpu-tcp-bootstrap-sanitizers
vgpu-tcp-bootstrap-windows: build/waddle_tcp_bootstrap.dll build/vgpu_tcp_bootstrap_test.exe

vgpu-tcp-bootstrap-coverage:
	python3 tests/vgpu/tcp_bootstrap_sdk_coverage.py

vgpu-tcp-bootstrap-sanitizers:
	python3 tests/vgpu/tcp_bootstrap_sdk_coverage.py sanitizers

# Native real-GPU fixture is separate from synthetic TCP/bootstrap acceptance.
build/vgpu_tcp_gpu_windows_draft.exe: tests/vgpu/tcp_gpu_windows.c $(VgpuIcdHeaders) tests/vgpu/shaders/compute_shader.h tests/vgpu/shaders/compute_push_shader.h tests/vgpu/shaders/triangle_vertex_shader.h tests/vgpu/shaders/triangle_fragment_shader.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isubmodules/venus_protocol/include $< -luser32 -ldxguid -ladvapi32 -o $@
