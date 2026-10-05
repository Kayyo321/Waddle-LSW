CC ?= cc
ZIG ?= zig
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc -Isrc/common -Isrc/cli -Isrc/daemon -Isrc/guest -Isrc/mock
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
COMMON = src/common/protocol.c src/common/arguments.c build/path_rules.o

.PHONY: all test clean zig-test test-sanitizers demo windows windows-test coverage

all: build/waddle build/waddle-mock-guest build/waddled

build:
	mkdir -p $@

build/path_rules.o: src/common/path_rules.zig | build
	$(ZIG) build-obj src/common/path_rules.zig -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/unit: tests/unit/unit.c $(COMMON) include/waddle/cli_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/unit.c $(COMMON) $(LDFLAGS) -o $@

build/waddle: src/cli/host.c src/cli/session.c src/cli/terminal.c src/daemon/daemon_client.c src/daemon/daemon_protocol.c build/daemon_config.o src/cli/session.h src/cli/terminal.h src/daemon/daemon_client.h $(COMMON) include/waddle/cli_protocol.h include/waddle/daemon_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/cli/host.c src/cli/session.c src/cli/terminal.c src/daemon/daemon_client.c src/daemon/daemon_protocol.c build/daemon_config.o $(COMMON) $(LDFLAGS) -o $@

build/waddle-mock-guest: src/mock/mock_guest.c src/mock/mock_process.c src/mock/mock_process.h $(COMMON) include/waddle/cli_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/mock/mock_guest.c src/mock/mock_process.c $(COMMON) $(LDFLAGS) -lutil -o $@

build/integration: tests/integration/integration.c $(COMMON) include/waddle/cli_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/integration/integration.c $(COMMON) $(LDFLAGS) -lutil -o $@

build/terminal.o: src/cli/terminal.c src/cli/terminal.h src/common/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/cli/terminal.c -o $@

build/session.o: src/cli/session.c src/cli/session.h src/cli/terminal.h src/common/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/cli/session.c -o $@

build/mock_process.o: src/mock/mock_process.c src/mock/mock_process.h src/common/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/mock/mock_process.c -o $@

build/daemon_config.o: src/daemon/daemon_config.zig src/daemon/daemon_config.h include/waddle/daemon_protocol.h | build
	$(ZIG) build-obj src/daemon/daemon_config.zig -Iinclude -Isrc/daemon -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/test_daemon_config: tests/daemon/test_daemon_config.c build/daemon_config.o src/daemon/daemon_config.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_config.c build/daemon_config.o $(LDFLAGS) -o $@

zig-test: build/path_rules.o
	$(ZIG) test tests/unit/test_cli.zig -Iinclude -Isrc/common -Isrc/cli src/common/protocol.c src/common/arguments.c build/path_rules.o -lc
	$(ZIG) test src/common/path_rules.zig
	$(ZIG) test src/guest/guest_codec.zig
	$(ZIG) test src/daemon/daemon_config.zig -Iinclude -Isrc/daemon -lc

build/test_daemon_protocol: tests/daemon/test_daemon_protocol.c src/daemon/daemon_protocol.c include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_protocol.c src/daemon/daemon_protocol.c $(LDFLAGS) -o $@

build/test_daemon_qemu: tests/daemon/test_daemon_qemu.c src/daemon/daemon_qemu.c src/daemon/daemon_qmp.c build/daemon_config.o src/daemon/daemon_qemu.h src/daemon/daemon_qmp.h src/daemon/daemon_config.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_qemu.c src/daemon/daemon_qemu.c src/daemon/daemon_qmp.c build/daemon_config.o $(LDFLAGS) -lpthread -o $@

build/test_daemon_fs: tests/daemon/test_daemon_fs.c src/daemon/daemon_fs.c src/common/arguments.c build/daemon_config.o build/path_rules.o src/daemon/daemon_fs.h src/daemon/daemon_config.h src/common/path_rules.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_fs.c src/daemon/daemon_fs.c src/common/arguments.c build/daemon_config.o build/path_rules.o $(LDFLAGS) -o $@

DAEMON_COMMON = src/daemon/daemon_protocol.c src/daemon/daemon_state.c src/daemon/daemon_server.c src/daemon/daemon_qemu.c src/daemon/daemon_qmp.c src/daemon/daemon_fs.c src/common/arguments.c build/daemon_config.o build/path_rules.o

build/waddled: src/daemon/daemon_main.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/daemon/daemon_main.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

build/test_daemon_server: tests/daemon/test_daemon_server.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_server.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

build/test_daemon_client: tests/daemon/test_daemon_client.c src/daemon/daemon_client.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_client.c src/daemon/daemon_client.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

build/test_auto_terminal: tests/daemon/test_auto_terminal.c src/daemon/daemon_client.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_auto_terminal.c src/daemon/daemon_client.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

test: all build/unit build/integration build/test_daemon_protocol build/test_daemon_config build/test_daemon_qemu build/test_daemon_fs build/test_daemon_server build/test_daemon_client build/test_auto_terminal zig-test
	./build/unit
	./build/integration
	./build/test_daemon_protocol
	./build/test_daemon_config
	./build/test_daemon_qemu
	./build/test_daemon_fs
	./build/test_daemon_server
	./build/test_daemon_client
	./build/test_auto_terminal
	sh tests/integration/path_options.sh
	sh tests/acceptance/demo_fs.sh

test-sanitizers: clean
	$(MAKE) test \
		CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,leak,undefined"

demo: all
	sh tests/integration/demo.sh

clean:
	rm -rf build

# Windows 10 1809 system declarations; C owns native APIs, Zig owns wire parsing.
WindowsFlags = -target x86_64-windows-gnu -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006 -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -Isrc/common -Isrc/guest
GuestSources = src/guest/guest_listener.c src/guest/guest_session.c src/guest/guest_wire.c src/guest/guest_input.c src/guest/guest_output.c src/guest/guest_process.c src/guest/guest_environment.c
GuestHeaders = src/guest/guest_session.h src/guest/guest_wire.h src/guest/guest_pump.h src/guest/guest_process.h src/guest/guest_environment.h src/guest/guest_codec.h

build/guest_codec.lib: src/guest/guest_codec.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@

build/waddle-guest-exec.exe: $(GuestSources) $(GuestHeaders) build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) $(GuestSources) build/guest_codec.lib -lws2_32 -o $@

windows: build/waddle-guest-exec.exe

build/windows_guest_test.exe: tests/windows/windows_guest.c src/guest/guest_codec.h build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) tests/windows/windows_guest.c build/guest_codec.lib -lws2_32 -municode -o $@

windows-test: windows build/windows_guest_test.exe
	./build/windows_guest_test.exe

coverage: build/path_rules.o
	sh tests/coverage.sh

# Explicit real VM gate; never silently substituted with the mock transport.
build/vm_terminal: tests/acceptance/vm_terminal.c | build
	$(CC) $(CFLAGS) $< $(LDFLAGS) -lutil -o $@

.PHONY: vm-test
vm-test: build/waddle build/windows_guest_test.exe build/vm_terminal
	bash tests/acceptance/vm_acceptance.sh
