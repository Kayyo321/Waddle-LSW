# Launch lane (TODO #29): `waddle open` request parsing (#29a) and the activation
# token store (#29e). Owns only the targets below; scripts/interop_lanes.mk
# aggregates them into interop-test, interop-sanitizers and interop-coverage.

InteropOpenRequestSanitize = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fno-omit-frame-pointer -fsanitize=address,leak,undefined -fno-sanitize-recover=undefined

.PHONY: interop-open-request-test interop-open-request-abi-test interop-open-request-sanitizers interop-open-request-coverage

build/interop_open_request.o: src/interop/open_request.zig src/interop/open_request.h src/interop/pe_header.zig src/interop/pe_header.h | build
	$(ZIG) build-obj $< -Iinclude -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/open_request_abi_test: tests/interop/open_request_abi.c src/interop/open_request.h src/common/arguments.c build/interop_open_request.o build/path_rules.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop $< src/common/arguments.c build/interop_open_request.o build/path_rules.o $(LDFLAGS) -o $@

interop-open-request-abi-test: build/open_request_abi_test
	./build/open_request_abi_test

# The Zig suite imports pe_header.zig, whose own test links the PE C fixture.
interop-open-request-test: interop-open-request-abi-test build/interop_pe_header_c_fixture.o
	$(ZIG) test src/interop/open_request.zig build/interop_pe_header_c_fixture.o -Iinclude -lc

build/open_request_abi_asan: tests/interop/open_request_abi.c src/interop/open_request.h src/common/arguments.c build/interop_open_request.o build/path_rules.o | build
	$(CC) $(CPPFLAGS) $(InteropOpenRequestSanitize) -Isrc/interop $< src/common/arguments.c build/interop_open_request.o build/path_rules.o -fsanitize=address,leak,undefined -o $@

# Zig code is not instrumented here; Zig ownership is proven by std.testing.allocator.
interop-open-request-sanitizers: build/open_request_abi_asan
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/open_request_abi_asan

# Line and taken-branch gate (>= 90 percent) through the shared coverage driver.
interop-open-request-coverage:
	python3 tests/av/coverage.py open_request

InteropLaneTests += interop-open-request-test
InteropLaneSanitizers += interop-open-request-sanitizers
InteropLaneCoverage += interop-open-request-coverage
