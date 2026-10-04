CC ?= cc
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
COMMON = src/protocol.c src/arguments.c
.PHONY: all test clean
all: build/waddle build/waddle-mock-guest
build:
	mkdir -p $@
build/unit: tests/unit.c $(COMMON) include/waddle/cli_protocol.h src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit.c $(COMMON) $(LDFLAGS) -o $@
build/waddle: src/host.c $(COMMON) include/waddle/cli_protocol.h src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/host.c $(COMMON) $(LDFLAGS) -o $@
build/waddle-mock-guest: src/mock_guest.c $(COMMON) include/waddle/cli_protocol.h src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/mock_guest.c $(COMMON) $(LDFLAGS) -lutil -o $@
test: all build/unit build/integration
	./build/unit
	./build/integration
clean:
	rm -rf build
build/integration: tests/integration.c $(COMMON) include/waddle/cli_protocol.h src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/integration.c $(COMMON) $(LDFLAGS) -lutil -o $@
