# Window-state reducer C ABI gates (TODO #13 prerequisite, impl/seamless-interop/claude_45_percent).
#
# Included by GNUmakefile through scripts/interop_lanes.mk. Run from the worktree root:
#   make interop-window-state-abi-test
#   make interop-window-state-abi-sanitizers
#   make interop-window-state-abi-coverage
#
# Outputs go under build/window_state_abi/. The C consumer test links the single ReleaseSafe
# object of src/interop/window_state_abi.zig, which is not instrumented. The sanitizers therefore
# cover the C side and the libc allocations that the Zig side makes through std.heap.c_allocator.
# Zig ownership is proven by std.testing.allocator inside `zig test`, not by a sanitizer.

WsAbiBuild := build/window_state_abi
WsAbiZig := src/interop/window_state_abi.zig
WsAbiCore := src/interop/window_state.zig
WsAbiHdr := src/interop/window_state.h
WsAbiTest := tests/interop/window_state_abi.c
WsAbiObj := $(WsAbiBuild)/window_state_abi.o
WsAbiCFlags := -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Iinclude -Isrc/interop
WsAbiSanFlags := -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer
WsAbiSanEnv := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

InteropLaneTests += interop-window-state-abi-test
InteropLaneSanitizers += interop-window-state-abi-sanitizers
InteropLaneCoverage += interop-window-state-abi-coverage

$(WsAbiBuild):
	mkdir -p $@

# ReleaseSafe, position-independent object of the Zig C ABI. Both C builds link this one object.
$(WsAbiObj): $(WsAbiZig) $(WsAbiCore) $(WsAbiHdr) | $(WsAbiBuild)
	$(ZIG) build-obj $(WsAbiZig) -Iinclude -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

.PHONY: interop-window-state-abi-test interop-window-state-abi-sanitizers interop-window-state-abi-coverage

# Zig unit tests (std.testing.allocator, FailingAllocator sweeps, random-sequence oracle), then the
# C consumer test built without sanitizers.
interop-window-state-abi-test: $(WsAbiObj)
	$(ZIG) test $(WsAbiZig) -Iinclude -Isrc/interop -lc
	$(CC) $(WsAbiCFlags) $(WsAbiTest) $(WsAbiObj) -o $(WsAbiBuild)/window_state_abi_test
	$(WsAbiBuild)/window_state_abi_test

# The C consumer test under ASan, LSan and UBSan with leak detection enabled.
interop-window-state-abi-sanitizers: $(WsAbiObj)
	$(CC) $(WsAbiCFlags) $(WsAbiSanFlags) $(WsAbiTest) $(WsAbiObj) -o $(WsAbiBuild)/window_state_abi_san
	$(WsAbiSanEnv) $(WsAbiBuild)/window_state_abi_san

# Zig line and taken-branch coverage of window_state_abi.zig through the repository recorder.
# The recorder compiles the module with -Iinclude only; see the report for the required patch.
interop-window-state-abi-coverage:
	python3 tests/av/coverage.py window_state_abi
