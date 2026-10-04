CC ?= cc
ZIG ?= zig
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
COMMON = src/protocol.c src/arguments.c build/path_rules.o

.PHONY: all test clean zig-test test-sanitizers demo

all: build/waddle build/waddle-mock-guest

build:
	mkdir -p $@

build/path_rules.o: src/path_rules.zig | build
	$(ZIG) build-obj src/path_rules.zig -O ReleaseSafe -fPIC -fcompiler-rt -femit-bin=$@

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
