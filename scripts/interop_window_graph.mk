# Standalone window-graph gates (TODO #10 slice, impl/seamless-interop).
#
# Run from the worktree root (the directory that holds src/, include/ and tests/):
#   make -f scripts/interop_window_graph.mk window-graph-help
#   make -f scripts/interop_window_graph.mk window-graph-all
#
# Not included by GNUmakefile or scripts/interop.mk. Variables use ?= so the command
# line or the environment can override them. This file's own outputs and the Zig caches go
# under $(WgBuild), so nothing is written to ~/.cache/zig or the source tree. Exception:
# tests/av/coverage.py writes its run directories to build/coverage/interop/<mode>/, which is
# hard-coded in that tracked script and cannot be redirected without editing it.
#
# Inputs that do not exist yet are referenced through $(wildcard ...), so a missing input
# does not stop make from planning (dry runs keep working). The recipe that needs the
# input fails with the tool's own error when it runs.
#
# This file never assigns .DEFAULT_GOAL. window-graph-help is the first rule only so that a
# standalone run prints the target list. A makefile that includes this file should define
# its own default goal before the include.

# ---- Tools -----------------------------------------------------------------------------
CC ?= cc
ZIG ?= zig
NM ?= nm
# Coverage compiler and gcov must be the same GCC: gcov reads the .gcno written by that compiler.
WgCovCC ?= gcc
WgCovGcov ?= gcov
WgMingwCC ?= x86_64-w64-mingw32-gcc
WgWinZigCC ?= $(ZIG) cc -target x86_64-windows-gnu

# ---- Output tree -----------------------------------------------------------------------
WgBuild ?= build/window_graph
WgSanDir := $(WgBuild)/san
WgCovDir := $(WgBuild)/coverage
WgCovObs := $(WgCovDir)/observer
WgCovPy := $(WgBuild)/coverage_window_graph.py

# ---- Flags -----------------------------------------------------------------------------
WgCStd ?= -std=c11 -Wall -Wextra -Wpedantic -Werror
WgCFlags ?= $(WgCStd) -UNDEBUG
WgCThreads ?= -pthread
WgSanFlags ?= -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer
WgCovFlags ?= -O0 -g --coverage -fprofile-update=atomic -fprofile-abs-path
WgZigFlags ?= -Iinclude -lc
# zig predefines _WIN32_WINNT=0x0603 on the command line, so the Windows defines are given
# on the command line too (a source #define would be -Wmacro-redefined under -Werror).
# GetDpiForWindow needs WINVER >= 0x0605, so WINVER is set to match _WIN32_WINNT.
WgWinDefs ?= -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -UWINVER -DWINVER=0x0A00 -DUNICODE -D_UNICODE
WgWinLibs ?= -ldwmapi
WgCovMinPercent ?= 90
# Observer allocation-failure injection (Linux, GNU-compatible linker only; never passed to the Windows
# builds): the test defines __wrap_calloc, which fails exactly the next calloc when armed. The Def part
# enables the test body, the Ld part makes every calloc call of the linked objects reach the wrapper.
# Override both to empty to build the test without the NoMemory case (it then prints a skip line).
WgObsWrapDef ?= -DWgObsWrapCalloc=1
WgObsWrapLd ?= -Wl,--wrap=calloc
WgObsWrap = $(WgObsWrapDef) $(WgObsWrapLd)

# Sanitizer runtime options. Leak detection is always on and is never overridden here.
WgSanEnv = ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

# ---- Environment for every recipe ------------------------------------------------------
export PYTHONDONTWRITEBYTECODE := 1
export ZIG_LOCAL_CACHE_DIR := $(abspath $(WgBuild))/zig-local
export ZIG_GLOBAL_CACHE_DIR := $(abspath $(WgBuild))/zig-global
export PATH := $(HOME)/.local/bin:$(PATH)

# ---- Sources (some may not exist yet; see the header note) -----------------------------
WgZigCore := src/interop/window_graph.zig
WgZigAbi := src/interop/window_graph_abi.zig
WgHdrApi := include/waddle/interop_window_graph.h
WgObsSrc := src/interop/window_observer_windows.c
WgObsHdr := src/interop/window_observer_windows.h
WgTestAbi := tests/interop/window_graph.c
WgTestObs := tests/interop/window_observer_windows.c
WgTestDbl := tests/interop/window_observer_api_double.h
# The exact set of global text symbols of the ABI object: the 21 functions of include/waddle/interop_window_graph.h.
WgAbiSymbols := interop_window_graph_check_action interop_window_graph_classify interop_window_graph_complete_retire \
	interop_window_graph_create interop_window_graph_destroy interop_window_graph_drain interop_window_graph_find_live \
	interop_window_graph_get_stats interop_window_graph_invalidate_all interop_window_graph_list_retiring \
	interop_window_graph_next_event interop_window_graph_note_destroyed interop_window_graph_observe \
	interop_window_graph_refresh_abort interop_window_graph_refresh_begin interop_window_graph_refresh_commit \
	interop_window_graph_refresh_keep interop_window_graph_refresh_offer interop_window_graph_refresh_unseen \
	interop_window_graph_stamp_high_water interop_window_graph_take_capacity_diagnostic

.PHONY: window-graph-help window-graph-test window-graph-symbols window-graph-sanitizers window-graph-coverage window-graph-windows window-graph-all window-graph-clean

# Print the target list. First rule in the file, so it is the default goal for standalone runs.
window-graph-help:
	@echo "Window-graph gates. Run from the worktree root. Outputs go under $(WgBuild)."
	@echo "  window-graph-test        Zig unit tests (Debug and ReleaseSafe), ABI object, C ABI test, observer double test (executes)"
	@echo "  window-graph-symbols     nm check: the ABI object exports exactly the 21 interop_window_graph_* functions (T symbols)"
	@echo "  window-graph-sanitizers  C ABI and observer tests under ASan+LSan+UBSan (executes)"
	@echo "  window-graph-coverage    Zig line/branch via tests/av/coverage.py; gcov line/branch for observer portable C (executes)"
	@echo "  window-graph-windows     Windows cross-compile and link checks only (NOT executed)"
	@echo "  window-graph-all         Runs window-graph-test, -symbols, -sanitizers, -coverage, -windows"
	@echo "  window-graph-clean       Removes $(WgBuild) only"
	@echo "  window-graph-help        Prints this list"

# ---- Output directories (order-only prerequisites) --------------------------------------
$(WgBuild) $(WgSanDir) $(WgCovDir):
	mkdir -p $@

# Zig unit tests, the ReleaseSafe ABI object, and both C consumers (built, then run).
window-graph-test: $(WgBuild)/window_graph_abi.o $(WgBuild)/window_graph_c_test $(WgBuild)/window_observer_test
	$(ZIG) test $(WgZigCore) $(WgZigFlags)
	$(ZIG) test $(WgZigAbi) $(WgZigFlags)
	$(ZIG) test $(WgZigCore) $(WgZigFlags) -O ReleaseSafe
	$(ZIG) test $(WgZigAbi) $(WgZigFlags) -O ReleaseSafe
	$(WgBuild)/window_graph_c_test
	$(WgBuild)/window_observer_test

# Symbol check: the global text symbols (nm type T) of the ABI object are exactly WgAbiSymbols (21 names) and
# exactly the functions the public header declares. Weak compiler-rt symbols (type W) are not T and are ignored.
window-graph-symbols: $(WgBuild)/window_graph_abi.o
	$(NM) -g --defined-only $(WgBuild)/window_graph_abi.o | awk '$$2=="T" {print $$3}' | LC_ALL=C sort > $(WgBuild)/abi_symbols_actual.txt
	printf '%s\n' $(WgAbiSymbols) | LC_ALL=C sort > $(WgBuild)/abi_symbols_expected.txt
	grep -o 'interop_window_graph_[a-z_]*(' $(WgHdrApi) | tr -d '(' | LC_ALL=C sort -u > $(WgBuild)/abi_symbols_header.txt
	test "$$(wc -l < $(WgBuild)/abi_symbols_expected.txt)" -eq 21
	diff -u $(WgBuild)/abi_symbols_expected.txt $(WgBuild)/abi_symbols_actual.txt
	diff -u $(WgBuild)/abi_symbols_expected.txt $(WgBuild)/abi_symbols_header.txt
	@echo "window-graph-symbols: 21 exported T symbols, equal to the header declarations."

# ReleaseSafe, position-independent object of the Zig C ABI. Every C consumer links this one object.
$(WgBuild)/window_graph_abi.o: $(wildcard $(WgZigCore) $(WgZigAbi) $(WgHdrApi)) | $(WgBuild)
	$(ZIG) build-obj $(WgZigAbi) -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

# C ABI consumer test, linked with the ABI object. -UNDEBUG keeps assert() active.
$(WgBuild)/window_graph_c_test: $(wildcard $(WgTestAbi) $(WgHdrApi)) $(WgBuild)/window_graph_abi.o | $(WgBuild)
	$(CC) $(WgCFlags) $(WgCThreads) -Iinclude $(WgTestAbi) $(WgBuild)/window_graph_abi.o -o $@

# Observer lifecycle tests driven through the API double. The REAL graph object is linked in.
$(WgBuild)/window_observer_test: $(wildcard $(WgObsSrc) $(WgObsHdr) $(WgTestObs) $(WgTestDbl)) $(WgBuild)/window_graph_abi.o | $(WgBuild)
	$(CC) $(WgCFlags) $(WgCThreads) $(WgObsWrap) -Isrc/interop -Iinclude $(WgObsSrc) $(WgTestObs) $(WgBuild)/window_graph_abi.o -o $@

# Sanitizer rebuilds of the same two C tests into $(WgSanDir). The Zig object is not instrumented.
window-graph-sanitizers: $(WgBuild)/window_graph_abi.o | $(WgSanDir)
	$(CC) $(WgCFlags) $(WgSanFlags) $(WgCThreads) -Iinclude $(WgTestAbi) $(WgBuild)/window_graph_abi.o -o $(WgSanDir)/window_graph_c_test
	$(CC) $(WgCFlags) $(WgSanFlags) $(WgCThreads) $(WgObsWrap) -Isrc/interop -Iinclude $(WgObsSrc) $(WgTestObs) $(WgBuild)/window_graph_abi.o -o $(WgSanDir)/window_observer_test
	$(WgSanEnv) $(WgSanDir)/window_graph_c_test
	$(WgSanEnv) $(WgSanDir)/window_observer_test

# (a) Zig modules: a sed copy of tests/av/coverage.py that adds the two window_graph modes to
#     InteropModes. The tracked script is never edited. The sed must change exactly one line.
# (b) C: gcov line and branch coverage of the portable observer code. The _WIN32 section is
#     preprocessed out on Linux, so gcov never counts it. The threshold is WgCovMinPercent.
window-graph-coverage: $(WgBuild)/window_graph_abi.o | $(WgBuild) $(WgCovDir)
	sed "s/^InteropModes = (/InteropModes = ('window_graph', 'window_graph_abi', /" tests/av/coverage.py > $(WgCovPy).tmp
	test "$$(diff tests/av/coverage.py $(WgCovPy).tmp | grep -c '^[<>]')" -eq 2
	grep -q "^InteropModes = ('window_graph', 'window_graph_abi', " $(WgCovPy).tmp
	mv $(WgCovPy).tmp $(WgCovPy)
	PYTHONPATH=tests/av python3 $(WgCovPy) window_graph
	PYTHONPATH=tests/av python3 $(WgCovPy) window_graph_abi
	rm -rf $(WgCovObs) && mkdir -p $(WgCovObs)
	$(WgCovCC) $(WgCFlags) $(WgCovFlags) -Isrc/interop -Iinclude -c $(WgObsSrc) -o $(WgCovObs)/window_observer_windows.o
	$(CC) $(WgCFlags) $(WgCThreads) $(WgObsWrapDef) -Isrc/interop -Iinclude -c $(WgTestObs) -o $(WgCovObs)/window_observer_test.o
	$(WgCovCC) $(WgCovFlags) $(WgCThreads) $(WgObsWrapLd) $(WgCovObs)/window_observer_test.o $(WgCovObs)/window_observer_windows.o $(WgBuild)/window_graph_abi.o -o $(WgCovObs)/window_observer_test
	$(WgCovObs)/window_observer_test
	cd $(WgCovObs) && $(WgCovGcov) --json-format -b -c window_observer_windows.gcno > /dev/null
	python3 -c "import gzip,json,sys; r=json.loads(gzip.decompress(open(sys.argv[1],'rb').read())); fs=[x for x in r['files'] if x['file'].endswith('src/interop/window_observer_windows.c')]; assert len(fs)==1, 'expected exactly one gcov record for src/interop/window_observer_windows.c'; L=fs[0]['lines']; B=[b for l in L for b in l.get('branches',[])]; lc=sum(1 for l in L if l['count']>0); bc=sum(1 for b in B if b['count']>0); lt=len(L); bt=len(B); m=int(sys.argv[2]); print('line %d/%d branch %d/%d' % (lc,lt,bc,bt)); sys.exit(0 if lt>0 and bt>0 and 100*lc>=m*lt and 100*bc>=m*bt else 1)" $(WgCovObs)/window_observer_windows.gcov.json.gz $(WgCovMinPercent)

# Cross-compile and link checks only. Nothing here is executed (no Windows runtime is available).
window-graph-windows: $(WgBuild)/window_graph_c_test.exe $(WgBuild)/window_observer_test.exe $(WgBuild)/observer_native_zig.o $(WgBuild)/observer_native_mingw.o
	@echo "window-graph-windows: cross-compiled and linked only; the Windows binaries were NOT executed."

# Static Zig library of the ABI for Windows (x86_64-windows-gnu, no compiler-rt, libc linked).
$(WgBuild)/window_graph_abi_windows.lib: $(wildcard $(WgZigCore) $(WgZigAbi) $(WgHdrApi)) | $(WgBuild)
	$(ZIG) build-lib $(WgZigAbi) -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=$@

$(WgBuild)/window_graph_c_test.exe: $(wildcard $(WgTestAbi) $(WgHdrApi)) $(WgBuild)/window_graph_abi_windows.lib | $(WgBuild)
	$(WgWinZigCC) $(WgCFlags) -Iinclude $(WgTestAbi) $(WgBuild)/window_graph_abi_windows.lib -o $@

# The observer double test compiles the native _WIN32 section too, so WinDefs and -ldwmapi apply here.
$(WgBuild)/window_observer_test.exe: $(wildcard $(WgObsSrc) $(WgObsHdr) $(WgTestObs) $(WgTestDbl)) $(WgBuild)/window_graph_abi_windows.lib | $(WgBuild)
	$(WgWinZigCC) $(WgCFlags) $(WgWinDefs) -Isrc/interop -Iinclude $(WgObsSrc) $(WgTestObs) $(WgBuild)/window_graph_abi_windows.lib $(WgWinLibs) -o $@

# Native observer object only (-c, no link), once per Windows toolchain.
$(WgBuild)/observer_native_zig.o: $(wildcard $(WgObsSrc) $(WgObsHdr)) | $(WgBuild)
	$(WgWinZigCC) $(WgCFlags) $(WgWinDefs) -Isrc/interop -Iinclude -c $(WgObsSrc) -o $@

$(WgBuild)/observer_native_mingw.o: $(wildcard $(WgObsSrc) $(WgObsHdr)) | $(WgBuild)
	$(WgMingwCC) $(WgCFlags) $(WgWinDefs) -Isrc/interop -Iinclude -c $(WgObsSrc) -o $@

# Every gate in order (test, symbols, sanitizers, coverage, windows). Under -j the independent gates may interleave.
window-graph-all: window-graph-test window-graph-symbols window-graph-sanitizers window-graph-coverage window-graph-windows

# Removes the build tree only. Refuses an empty, "/", "." or ".." WgBuild so an override cannot widen the delete.
window-graph-clean:
	@case '$(WgBuild)' in ''|/|.|..) echo "window-graph-clean: refusing to remove '$(WgBuild)'" >&2; exit 1 ;; esac
	rm -rf -- '$(WgBuild)'
