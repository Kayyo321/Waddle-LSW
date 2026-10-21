# Portable production WSI lifetime tests. Native API doubles do not qualify Windows.
VgpuWsiLifecycleSources = tests/vgpu/win32_present_portable.c src/vgpu/venus_win32_present.c
VgpuWsiLifecycleHeaders = include/waddle/venus_win32_present.h tests/vgpu/win32_fake/windows.h tests/vgpu/win32_fake/dwmapi.h
VgpuWsiLifecycleIncludes = -Iinclude -Itests/vgpu/win32_fake

build/wsi-lifecycle: | build
	mkdir -p $@

build/wsi-lifecycle/native: $(VgpuWsiLifecycleSources) $(VgpuWsiLifecycleHeaders) | build/wsi-lifecycle
	$(CC) $(CFLAGS) -D_WIN32 $(VgpuWsiLifecycleIncludes) $(VgpuWsiLifecycleSources) -o $@

build/wsi-lifecycle/unsupported: $(VgpuWsiLifecycleSources) $(VgpuWsiLifecycleHeaders) | build/wsi-lifecycle
	$(CC) $(CFLAGS) $(VgpuWsiLifecycleIncludes) $(VgpuWsiLifecycleSources) -o $@

.PHONY: vgpu-wsi-lifecycle-test vgpu-wsi-lifecycle-sanitizers vgpu-wsi-lifecycle-coverage
vgpu-wsi-lifecycle-test: vgpu-wsi-test build/wsi-lifecycle/native build/wsi-lifecycle/unsupported
	./build/wsi-lifecycle/native
	./build/wsi-lifecycle/unsupported

vgpu-wsi-lifecycle-sanitizers: | build/wsi-lifecycle
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fno-omit-frame-pointer -fsanitize=address,leak,undefined -D_WIN32 $(VgpuWsiLifecycleIncludes) $(VgpuWsiLifecycleSources) -o build/wsi-lifecycle/sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/wsi-lifecycle/sanitized
	python3 tests/vgpu/state_sanitizers.py venus_wsi

vgpu-wsi-lifecycle-coverage: vgpu-protocol
	python3 tests/av/coverage.py venus_wsi
	python3 tests/vgpu/wsi_lifecycle_coverage.py

# This portable gate validates the final native-output verifier with malformed logs.
# Qualification must additionally run scripts/vgpu_m2_output.py on every final
# guest_status.json; these synthetic tests cannot accept a native run.
.PHONY: vgpu-m2-output-test
vgpu-m2-output-test:
	python3 -B tests/vgpu/m2_output_test.py -v

# Explicit qualification instrumentation; ordinary sink/DLL recipes are unchanged.
VgpuM2OwnerSources = tests/vgpu/m2_owner_ledger.c tests/vgpu/m2_ownership_probe.c
VgpuM2OwnerHeaders = tests/vgpu/m2_owner_ledger.h tests/vgpu/m2_ownership_probe.h
VgpuM2OwnerFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/vgpu
VgpuM2OwnerPortableFlags = $(VgpuM2OwnerFlags) -D_WIN32 -DWaddleM2OwnershipProbe -DWaddleM2OwnershipFaults -Itests/vgpu/win32_fake

build/wsi-lifecycle/m2_owner_ledger: tests/vgpu/m2_owner_ledger_test.c tests/vgpu/m2_owner_ledger.c $(VgpuM2OwnerHeaders) | build/wsi-lifecycle
	$(CC) $(VgpuM2OwnerFlags) -O2 tests/vgpu/m2_owner_ledger_test.c tests/vgpu/m2_owner_ledger.c -o $@

build/wsi-lifecycle/m2_ownership_portable: tests/vgpu/m2_ownership_portable.c $(VgpuM2OwnerSources) $(VgpuM2OwnerHeaders) $(VgpuWsiLifecycleHeaders) src/vgpu/venus_win32_present.c | build/wsi-lifecycle
	$(CC) $(VgpuM2OwnerPortableFlags) -O2 tests/vgpu/m2_ownership_portable.c $(VgpuM2OwnerSources) src/vgpu/venus_win32_present.c -o $@

.PHONY: vgpu-m2-ownership-test vgpu-m2-ownership-sanitizers vgpu-m2-ownership-coverage vgpu-m2-ownership-native-build
vgpu-m2-ownership-test: build/wsi-lifecycle/m2_owner_ledger build/wsi-lifecycle/m2_ownership_portable
	./build/wsi-lifecycle/m2_owner_ledger
	./build/wsi-lifecycle/m2_ownership_portable

vgpu-m2-ownership-sanitizers: | build/wsi-lifecycle
	$(CC) $(VgpuM2OwnerFlags) -O1 -g -fno-omit-frame-pointer -fsanitize=address,leak,undefined tests/vgpu/m2_owner_ledger_test.c tests/vgpu/m2_owner_ledger.c -o build/wsi-lifecycle/m2_owner_ledger_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/wsi-lifecycle/m2_owner_ledger_sanitized
	$(CC) $(VgpuM2OwnerPortableFlags) -O1 -g -fno-omit-frame-pointer -fsanitize=address,leak,undefined tests/vgpu/m2_ownership_portable.c $(VgpuM2OwnerSources) src/vgpu/venus_win32_present.c -o build/wsi-lifecycle/m2_ownership_portable_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/wsi-lifecycle/m2_ownership_portable_sanitized

vgpu-m2-ownership-coverage:
	python3 -B tests/vgpu/m2_ownership_coverage_test.py -v
	python3 -B tests/vgpu/m2_ownership_coverage.py

build/vgpu_m2_ownership_native.exe: tests/vgpu/m2_ownership_native.c $(VgpuM2OwnerSources) $(VgpuM2OwnerHeaders) include/waddle/venus_win32_present.h src/vgpu/venus_win32_present.c | build
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuM2OwnerFlags) -DWaddleM2OwnershipProbe -DWaddleM2OwnershipFaults tests/vgpu/m2_ownership_native.c $(VgpuM2OwnerSources) src/vgpu/venus_win32_present.c -luser32 -lgdi32 -ldwmapi -o $@

# This object deliberately omits WaddleM2OwnershipFaults; the qualified DLL will
# link it in a separate later rule, never the standalone fault-control executable.
build/wsi-lifecycle/m2_ownership_probe_windows.obj: tests/vgpu/m2_ownership_probe.c $(VgpuM2OwnerHeaders) | build/wsi-lifecycle
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuM2OwnerFlags) -DWaddleM2OwnershipProbeExport -c $< -o $@

vgpu-m2-ownership-native-build: build/vgpu_m2_ownership_native.exe build/wsi-lifecycle/m2_ownership_probe_windows.obj
	python3 -c 'import subprocess,sys; names={line.split()[-1] for line in subprocess.check_output(["nm","-g","--defined-only","build/wsi-lifecycle/m2_ownership_probe_windows.obj"],text=True).splitlines() if line.split()}; expected={"venus_m2_dc_acquire","venus_m2_dc_release","venus_m2_dc_snapshot"}; sys.exit(0 if names==expected else "Unexpected qualified observer definitions: "+repr(names));'
