CC ?= cc
ZIG ?= zig
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
COMMON = src/protocol.c src/arguments.c build/path_rules.o

.PHONY: all test clean zig-test test-sanitizers demo windows windows-test coverage

all: build/waddle build/waddle-mock-guest build/waddled

build:
	mkdir -p $@

build/path_rules.o: src/path_rules.zig | build
	$(ZIG) build-obj src/path_rules.zig -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/unit: tests/unit.c $(COMMON) include/waddle/cli_protocol.h src/common.h src/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit.c $(COMMON) $(LDFLAGS) -o $@

build/waddle: src/host.c src/session.c src/terminal.c src/session.h src/terminal.h $(COMMON) include/waddle/cli_protocol.h src/common.h src/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/host.c src/session.c src/terminal.c $(COMMON) $(LDFLAGS) -o $@

build/waddle-mock-guest: src/mock_guest.c src/mock_process.c src/mock_process.h $(COMMON) include/waddle/cli_protocol.h src/common.h src/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/mock_guest.c src/mock_process.c $(COMMON) $(LDFLAGS) -lutil -o $@

build/integration: tests/integration.c $(COMMON) include/waddle/cli_protocol.h src/common.h src/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/integration.c $(COMMON) $(LDFLAGS) -lutil -o $@

build/terminal.o: src/terminal.c src/terminal.h src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/terminal.c -o $@

build/session.o: src/session.c src/session.h src/terminal.h src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/session.c -o $@

build/mock_process.o: src/mock_process.c src/mock_process.h src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/mock_process.c -o $@

build/daemon_config.o: src/daemon_config.zig src/daemon_config.h include/waddle/daemon_protocol.h | build
	$(ZIG) build-obj src/daemon_config.zig -Iinclude -Isrc -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/test_daemon_config: tests/test_daemon_config.c build/daemon_config.o src/daemon_config.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_daemon_config.c build/daemon_config.o $(LDFLAGS) -o $@

zig-test: build/path_rules.o
	$(ZIG) test tests/test_cli.zig -Iinclude -Isrc src/protocol.c src/arguments.c build/path_rules.o -lc
	$(ZIG) test src/path_rules.zig
	$(ZIG) test src/guest_codec.zig
	$(ZIG) test src/daemon_config.zig -Iinclude -Isrc -lc

build/test_daemon_protocol: tests/test_daemon_protocol.c src/daemon_protocol.c include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_daemon_protocol.c src/daemon_protocol.c $(LDFLAGS) -o $@

build/test_daemon_qemu: tests/test_daemon_qemu.c src/daemon_qemu.c src/daemon_qmp.c build/daemon_config.o src/daemon_qemu.h src/daemon_qmp.h src/daemon_config.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_daemon_qemu.c src/daemon_qemu.c src/daemon_qmp.c build/daemon_config.o $(LDFLAGS) -lpthread -o $@

build/test_daemon_fs: tests/test_daemon_fs.c src/daemon_fs.c src/arguments.c build/daemon_config.o build/path_rules.o src/daemon_fs.h src/daemon_config.h src/path_rules.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_daemon_fs.c src/daemon_fs.c src/arguments.c build/daemon_config.o build/path_rules.o $(LDFLAGS) -o $@

DAEMON_COMMON = src/daemon_protocol.c src/daemon_state.c src/daemon_server.c src/daemon_qemu.c src/daemon_qmp.c src/daemon_fs.c src/arguments.c build/daemon_config.o build/path_rules.o

build/waddled: src/daemon_main.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/daemon_main.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

build/test_daemon_server: tests/test_daemon_server.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_daemon_server.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

test: all build/unit build/integration build/test_daemon_protocol build/test_daemon_config build/test_daemon_qemu build/test_daemon_fs build/test_daemon_server zig-test
	./build/unit
	./build/integration
	./build/test_daemon_protocol
	./build/test_daemon_config
	./build/test_daemon_qemu
	./build/test_daemon_fs
	./build/test_daemon_server
	sh tests/path_options.sh

test-sanitizers: clean
	$(MAKE) test \
		CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,leak,undefined"

demo: all
	sh tests/demo.sh

clean:
	rm -rf build

# Windows 10 1809 system declarations; C owns native APIs, Zig owns wire parsing.
WindowsFlags = -target x86_64-windows-gnu -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006 -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc
GuestSources = src/guest_listener.c src/guest_session.c src/guest_wire.c src/guest_input.c src/guest_output.c src/guest_process.c src/guest_environment.c
GuestHeaders = src/guest_session.h src/guest_wire.h src/guest_pump.h src/guest_process.h src/guest_environment.h src/guest_codec.h

build/guest_codec.lib: src/guest_codec.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@

build/waddle-guest-exec.exe: $(GuestSources) $(GuestHeaders) build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) $(GuestSources) build/guest_codec.lib -lws2_32 -o $@

windows: build/waddle-guest-exec.exe

build/windows_guest_test.exe: tests/windows_guest.c src/guest_codec.h build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) tests/windows_guest.c build/guest_codec.lib -lws2_32 -municode -o $@

windows-test: windows build/windows_guest_test.exe
	./build/windows_guest_test.exe

coverage: build/path_rules.o
	sh tests/coverage.sh

# Explicit real VM gate; never silently substituted with the mock transport.
build/vm_terminal: tests/vm_terminal.c | build
	$(CC) $(CFLAGS) $< $(LDFLAGS) -lutil -o $@

.PHONY: vm-test
vm-test: build/waddle build/windows_guest_test.exe build/vm_terminal
	bash tests/vm_acceptance.sh
