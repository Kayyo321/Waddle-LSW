CC ?= cc
ZIG ?= zig
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
COMMON = src/protocol.c src/arguments.c build/path_rules.o

.PHONY: all test clean zig-test test-sanitizers demo windows windows-test

all: build/waddle build/waddle-mock-guest

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

zig-test: build/path_rules.o
	$(ZIG) test tests/test_cli.zig -Iinclude -Isrc src/protocol.c src/arguments.c build/path_rules.o -lc
	$(ZIG) test src/path_rules.zig
	$(ZIG) test src/guest_codec.zig

test: all build/unit build/integration zig-test
	./build/unit
	./build/integration

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
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -lc -fno-compiler-rt -femit-bin=$@

build/waddle-guest-exec.exe: $(GuestSources) $(GuestHeaders) build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) $(GuestSources) build/guest_codec.lib -lws2_32 -o $@

windows: build/waddle-guest-exec.exe

build/windows_guest_test.exe: tests/windows_guest.c src/guest_codec.h build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) tests/windows_guest.c build/guest_codec.lib -lws2_32 -municode -o $@

windows-test: windows build/windows_guest_test.exe
	./build/windows_guest_test.exe
