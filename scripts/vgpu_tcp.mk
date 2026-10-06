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
