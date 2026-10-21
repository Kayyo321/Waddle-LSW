# Passive diagnostic fixtures: portable API doubles and Windows cross-build only.
VgpuGuiDiagFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -Itests/vgpu
VgpuGuiDiagSources = tests/vgpu/tcp_gpu_gui_diag_host.c
VgpuGuiDiagHeaders = tests/vgpu/tcp_gpu_gui_diag_windows.inc tests/vgpu/win32_fake/gui_diag_fake.h

build/gui-diag: | build
	mkdir -p $@

build/gui-diag/host: $(VgpuGuiDiagSources) $(VgpuGuiDiagHeaders) | build/gui-diag
	$(CC) $(VgpuGuiDiagFlags) -O2 $(VgpuGuiDiagSources) -o $@

build/gui-diag/owners: tests/vgpu/gui_owner_snapshot_test.c tests/vgpu/m2_owned_fixture_test.c tests/vgpu/tcp_gpu_m2_owned_windows.inc $(VgpuM2OwnerHeaders) tests/vgpu/m2_owner_ledger.c | build/gui-diag
	$(CC) $(VgpuGuiDiagFlags) -O2 $< tests/vgpu/m2_owner_ledger.c -o $@

build/vgpu_gui_diag_native.exe: tests/vgpu/tcp_gpu_gui_diag_windows.c tests/vgpu/tcp_gpu_gui_diag_windows.inc | build
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuGuiDiagFlags) $< -luser32 -o $@

# The integrated native fixture must rebuild after diagnostic include changes.
build/vgpu_tcp_gpu_windows_draft.exe: tests/vgpu/tcp_gpu_gui_diag_windows.inc

.PHONY: vgpu-gui-diag-test vgpu-gui-diag-sanitizers vgpu-gui-diag-native-build vgpu-gui-diag-coverage
vgpu-gui-diag-test: build/gui-diag/host build/gui-diag/owners
	./build/gui-diag/host
	./build/gui-diag/owners

vgpu-gui-diag-sanitizers: | build/gui-diag
	$(CC) $(VgpuGuiDiagFlags) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuGuiDiagSources) -o build/gui-diag/host_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/gui-diag/host_sanitized
	$(CC) $(VgpuGuiDiagFlags) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/gui_owner_snapshot_test.c tests/vgpu/m2_owner_ledger.c -o build/gui-diag/owners_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/gui-diag/owners_sanitized

vgpu-gui-diag-native-build: build/vgpu_gui_diag_native.exe build/vgpu_tcp_gpu_windows_draft.exe

vgpu-gui-diag-coverage:
	python3 -B tests/vgpu/gui_diag_coverage.py
