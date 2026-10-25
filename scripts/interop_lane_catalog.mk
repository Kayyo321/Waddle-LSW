# Host catalog runtime lane (TODO #27 wire/runtime and watch/enrichment portable halves, TODO #28
# runtime/association). Contract: impl/seamless-interop/claude_45_percent/catalog_runtime_contract.md.
# Included by GNUmakefile through scripts/interop_lane_*.mk. This file owns only the targets below.
# build/interop_codec.o is defined by scripts/interop.mk and is consumed here, not redefined.

ZIG ?= zig
InteropCatalogCFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer -Iinclude -Isrc/interop -Isrc/daemon
InteropCatalogSanEnv = ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
InteropCatalogMarkers = tests/av/coverage_modes/catalog_runtime

build/catalog_runtime.o: src/interop/catalog_runtime.zig src/interop/catalog_runtime.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/session_catalog_handler_test: tests/interop/session_catalog_handler.c src/interop/session_catalog_handler.c src/interop/session_catalog_handler.h build/catalog_runtime.o build/interop_codec.o | build
	$(CC) $(InteropCatalogCFlags) tests/interop/session_catalog_handler.c src/interop/session_catalog_handler.c build/catalog_runtime.o build/interop_codec.o -lc -o $@

.PHONY: interop-catalog-test interop-catalog-sanitizers interop-catalog-coverage

# Zig built-in tests for the runtime (std.testing.allocator leak checks, allocation-failure sweeps),
# then the C handler fixture, which runs under ASan, LSan and UBSan with fresh rooted directories.
interop-catalog-test: build/session_catalog_handler_test
	$(ZIG) test src/interop/catalog_runtime.zig -Iinclude -lc
	rm -rf build/catalog-fixture && mkdir -p build/catalog-fixture
	$(InteropCatalogSanEnv) ./build/session_catalog_handler_test build/catalog-fixture

interop-catalog-sanitizers: build/session_catalog_handler_test
	rm -rf build/catalog-fixture-sanitized && mkdir -p build/catalog-fixture-sanitized
	$(InteropCatalogSanEnv) ./build/session_catalog_handler_test build/catalog-fixture-sanitized

# Line and taken-arc gates (>= 90 percent each, no exclusions): the Zig runtime through the
# instrumented LLVM coverage runner, and the C handler through gcov.
$(InteropCatalogMarkers):
	mkdir -p tests/av/coverage_modes && touch $@
interop-catalog-coverage: $(InteropCatalogMarkers)
	python3 tests/av/coverage.py catalog_runtime
	python3 tests/interop/session_catalog_handler_coverage.py

InteropLaneTests += interop-catalog-test
InteropLaneSanitizers += interop-catalog-sanitizers
InteropLaneCoverage += interop-catalog-coverage
