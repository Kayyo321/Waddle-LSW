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

# ---- Native window-state adapter (TODO #13, native adapters 25 percent) ------------------------
# src/interop/window_state_adapter_windows.c is the guest Win32 adapter. Its portable logic runs
# against tests/interop/window_state_adapter_double.h on every platform. The _WIN32 table is compiled
# only by the mingw cross build of the native smoke below, and is executed by Windows CI only.
#
# Targets:
#   make interop-window-state-adapter-test       double suite and reducer integration, plain and sanitized
#   make interop-window-state-adapter-coverage   gcov line and branch gate (90 percent) on the portable logic
#   make build/interop_window_state_adapter_native_test.exe
#                                                mingw cross build of the Windows smoke (not executed here)
#
# Run the Windows smoke on a Windows desktop as: .\build\interop_window_state_adapter_native_test.exe

WsaAdapterBuild := build/window_state_adapter
WsaAdapterSrc := src/interop/window_state_adapter_windows.c
WsaAdapterHdr := src/interop/window_state_adapter_windows.h
WsaAdapterDouble := tests/interop/window_state_adapter_double.h
WsaAdapterSuite := tests/interop/window_state_adapter_suite.c
WsaAdapterIntegration := tests/interop/window_state_adapter_integration.c
WsaAdapterNative := tests/interop/window_state_adapter_native_windows.c
WsaMingwCC ?= x86_64-w64-mingw32-gcc
WsaAdapterCFlags := -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -Iinclude -Isrc/interop -Itests/interop
WsaAdapterSanFlags := -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer
WsaAdapterWrap := -Wl,--wrap=calloc
WsaAdapterWinDefs := -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE
WsaAdapterSanEnv := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

InteropLaneTests += interop-window-state-adapter-test
InteropLaneCoverage += interop-window-state-adapter-coverage

$(WsaAdapterBuild):
	mkdir -p $@

.PHONY: interop-window-state-adapter-test interop-window-state-adapter-coverage

# Double suite and reducer integration, each plain and under ASan, LSan and UBSan with leak detection on.
# The integration links the ReleaseSafe Zig ABI object of the window-state ABI lane.
interop-window-state-adapter-test: $(WsAbiObj) $(WsaAdapterSrc) $(WsaAdapterHdr) $(WsaAdapterDouble) | $(WsaAdapterBuild)
	$(CC) $(WsaAdapterCFlags) $(WsaAdapterWrap) $(WsaAdapterSrc) $(WsaAdapterSuite) -o $(WsaAdapterBuild)/suite
	$(WsaAdapterBuild)/suite
	$(CC) $(WsaAdapterCFlags) $(WsaAdapterSanFlags) $(WsaAdapterWrap) $(WsaAdapterSrc) $(WsaAdapterSuite) -o $(WsaAdapterBuild)/suite_san
	$(WsaAdapterSanEnv) $(WsaAdapterBuild)/suite_san
	$(CC) $(WsaAdapterCFlags) $(WsaAdapterSrc) $(WsaAdapterIntegration) $(WsAbiObj) -o $(WsaAdapterBuild)/integration
	$(WsaAdapterBuild)/integration
	$(CC) $(WsaAdapterCFlags) $(WsaAdapterSanFlags) $(WsaAdapterSrc) $(WsaAdapterIntegration) $(WsAbiObj) -o $(WsaAdapterBuild)/integration_san
	$(WsaAdapterSanEnv) $(WsaAdapterBuild)/integration_san

# gcov line and branch gate on the portable logic, driven by the double suite (see the script header).
interop-window-state-adapter-coverage:
	python3 tests/interop/window_state_adapter_coverage.py

# Cross-built Windows smoke. Compiled with mingw, linked against user32; executed only by Windows CI.
build/interop_window_state_adapter_native_test.exe: $(WsaAdapterNative) $(WsaAdapterSrc) $(WsaAdapterHdr) src/interop/window_state.h
	mkdir -p build
	$(WsaMingwCC) $(WsaAdapterCFlags) $(WsaAdapterWinDefs) $(WsaAdapterNative) $(WsaAdapterSrc) -luser32 -o $@

# Window coordinator (TODO #10 C1-C3, TODO #13 guest half, impl/seamless-interop/claude_45_percent).
#   make interop-window-coordinator-test         suite plain (random long sequences included)
#   make interop-window-coordinator-sanitizers   suite under ASan, LSan and UBSan with detect_leaks=1
#   make interop-window-coordinator-coverage     gcov line and branch gate (90 percent)
#   make build/window_coordinator/window_coordinator_windows.o   mingw cross-build of the coordinator
#
# The suite links the real graph, slot-runtime, codec and reducer Zig ABI objects built here, the real
# observer and adapter, a capture fake and a fake wire. Seam failures are injected through the
# -Wl,--wrap symbols listed in tests/interop/window_coordinator_wraps.txt (see window_coordinator_faults.h).

WcBuild := build/window_coordinator
WcSrc := src/interop/window_coordinator.c
WcHdr := src/interop/window_coordinator.h
WcSuite := tests/interop/window_coordinator_suite.c
WcFaults := tests/interop/window_coordinator_faults.h
WcWrapList := tests/interop/window_coordinator_wraps.txt
WcCoverage := tests/interop/window_coordinator_coverage.py
WcObserverSrc := src/interop/window_observer_windows.c
WcAdapterSrc := src/interop/window_state_adapter_windows.c
WcLinkWraps := $(foreach s,$(shell grep -v '^\#' $(WcWrapList)),-Wl,--wrap=$(s))
WcCFlags := -std=c11 -Wall -Wextra -Wpedantic -Werror -Wno-unused-function -UNDEBUG -O0 -g -pthread -Iinclude -Isrc/interop -Itests/interop
WcSanFlags := -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer
WcSanEnv := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
WcMingwCC ?= x86_64-w64-mingw32-gcc
WcWinDefs := -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE
WcZigObjs := $(WcBuild)/graph.o $(WcBuild)/slot.o $(WcBuild)/wstate.o $(WcBuild)/codec.o

InteropLaneTests += interop-window-coordinator-test
InteropLaneSanitizers += interop-window-coordinator-sanitizers
InteropLaneCoverage += interop-window-coordinator-coverage

$(WcBuild):
	mkdir -p $@

# ReleaseSafe, position-independent objects of the Zig ABIs, built with the same flags as the coverage gate.
$(WcBuild)/codec.o: src/interop/interop_codec.zig include/waddle/interop_protocol.h | $(WcBuild)
	$(ZIG) build-obj src/interop/interop_codec.zig -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
$(WcBuild)/graph.o: src/interop/window_graph_abi.zig src/interop/window_graph.zig include/waddle/interop_window_graph.h | $(WcBuild)
	$(ZIG) build-obj src/interop/window_graph_abi.zig -Iinclude -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
$(WcBuild)/slot.o: src/interop/interop_slot_runtime_abi.zig src/interop/interop_slot_runtime.zig src/interop/interop_slot_runtime.h | $(WcBuild)
	$(ZIG) build-obj src/interop/interop_slot_runtime_abi.zig -Iinclude -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
$(WcBuild)/wstate.o: src/interop/window_state_abi.zig src/interop/window_state.zig src/interop/window_state.h | $(WcBuild)
	$(ZIG) build-obj src/interop/window_state_abi.zig -Iinclude -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

$(WcBuild)/suite: $(WcSuite) $(WcFaults) $(WcWrapList) $(WcSrc) $(WcHdr) $(WcObserverSrc) $(WcAdapterSrc) $(WcZigObjs) | $(WcBuild)
	$(CC) $(WcCFlags) $(WcSuite) $(WcSrc) $(WcObserverSrc) $(WcAdapterSrc) $(WcZigObjs) $(WcLinkWraps) -o $@

$(WcBuild)/suite_san: $(WcSuite) $(WcFaults) $(WcWrapList) $(WcSrc) $(WcHdr) $(WcObserverSrc) $(WcAdapterSrc) $(WcZigObjs) | $(WcBuild)
	$(CC) $(WcCFlags) $(WcSanFlags) $(WcSuite) $(WcSrc) $(WcObserverSrc) $(WcAdapterSrc) $(WcZigObjs) $(WcLinkWraps) -o $@

.PHONY: interop-window-coordinator-test interop-window-coordinator-sanitizers interop-window-coordinator-coverage interop-window-coordinator-windows

interop-window-coordinator-test: $(WcBuild)/suite
	$(WcBuild)/suite

interop-window-coordinator-sanitizers: $(WcBuild)/suite_san
	$(WcSanEnv) $(WcBuild)/suite_san

# gcov line and branch gate on window_coordinator.c, driven by the suite (see the script header).
interop-window-coordinator-coverage:
	python3 $(WcCoverage)

# mingw cross-build of the portable coordinator object; compiled and linted here, not executed.
$(WcBuild)/window_coordinator_windows.o: $(WcSrc) $(WcHdr) | $(WcBuild)
	$(WcMingwCC) -std=c11 -Wall -Wextra -Wpedantic -Werror $(WcWinDefs) -Iinclude -Isrc/interop -c $(WcSrc) -o $@

interop-window-coordinator-windows: $(WcBuild)/window_coordinator_windows.o
