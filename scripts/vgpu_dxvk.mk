# Cross-compilation is distinct from native execution/compatibility acceptance.
build/vgpu_dxvk_integration.exe: tests/vgpu/dxvk_integration.c tests/vgpu/dxvk_heap_audit_windows.inc tests/vgpu/native_csr_heap_windows.inc tests/vgpu/dxvk_fault_windows.inc tests/vgpu/loader_environment_windows.h tests/vgpu/loader_environment_windows.c | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $< tests/vgpu/loader_environment_windows.c -municode -luser32 -lgdi32 -ldwmapi -ldxguid -o $@

.PHONY: vgpu-dxvk-build
vgpu-dxvk-build: build/vgpu_dxvk_integration.exe

vgpu-windows: build/vgpu_dxvk_integration.exe

# Verify pinned upstream sources; the tracked owner patch is applied only to build copies.
.PHONY: vgpu-dxvk-pin vgpu-dxvk-client-build
vgpu-dxvk-pin:
	sh scripts/verify_dxvk.sh

# Every client artifact uses the same tracked owner patch and hash receipt.
# The upstream submodule and prior pristine/private builds remain untouched.
vgpu-dxvk-client-build: vgpu-dxvk-pin scripts/build_dxvk_client.py patches/dxvk/setupapi_device_info_owner.patch | build
	python3 scripts/build_dxvk_client.py

# Private immutable loader crossbuild: native discovery remains a separate gate.
.PHONY: vgpu-native-loader-win64
# Windows loaders build from a private copy of the pin with the tracked patches applied
# (patches/vulkan_loader/, approved 2026-10-22); the submodule itself is never modified.
VgpuLoaderPatches = patches/vulkan_loader/0001-release-windows-initialization-module-references.patch
vgpu-native-loader-win64: submodules/vulkan_loader/CMakeLists.txt scripts/vulkan_loader.cmake scripts/vulkan_loader_win64.cmake scripts/vulkan_loader_private_source.py $(VgpuLoaderPatches)
	python3 -B scripts/vulkan_loader_private_source.py build/vendor/vulkan_loader_private
	cmake -S build/vendor/vulkan_loader_private -B build/vendor/vulkan_loader_win64_private -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(CURDIR)/scripts/vulkan_loader_win64.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_VULKAN_LOADER_INCLUDE="$(CURDIR)/scripts/vulkan_loader.cmake" -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF -DLOADER_CODEGEN=OFF -DBUILD_WERROR=ON -DUSE_GAS=ON
	cmake --build build/vendor/vulkan_loader_win64_private --parallel 4
	test -s build/vendor/vulkan_loader_win64_private/loader/vulkan-1.dll
	x86_64-w64-mingw32-objdump -f build/vendor/vulkan_loader_win64_private/loader/vulkan-1.dll | grep 'file format pei-x86-64'
	x86_64-w64-mingw32-objdump -p build/vendor/vulkan_loader_win64_private/loader/vulkan-1.dll | grep 'DLL Name:'

.PHONY: vgpu-loader-patch-test
vgpu-loader-patch-test:
	python3 -B tests/vgpu/vulkan_loader_patch_test.py
	python3 -O -B tests/vgpu/vulkan_loader_patch_test.py

# OpenCL (TODO #5): pinned clvk + clspv/LLVM cross-built for Windows. Not part of the
# default vgpu-windows aggregate because the LLVM build is long; cross-compilation is
# not execution. Submodules: clvk (nested), llvm_project, clspv_spirv_{headers,tools}.
.PHONY: vgpu-opencl-pin vgpu-opencl-build
# All compiler, provenance and independent policy selectors are explicit.
# See impl/software-vgpu-slicing/CLVK_RUNTIME_DRIVER_V2.md for the complete argv.
ClvkBuildArgs ?=
vgpu-opencl-pin:
	sh scripts/verify_clvk.sh

vgpu-opencl-build: vgpu-opencl-pin scripts/build_clvk_windows.py scripts/clvk_runtime_audit.py scripts/clvk/runtime_sections.ld scripts/clvk/OpenCL.def src/vgpu/clvk_runtime_tls.c src/vgpu/clvk_runtime_eh_pool.c patches/clvk/0001-explicit-quiescent-windows-shutdown.patch patches/clvk/0002-validate-one-dimensional-buffer-commands.patch patches/clvk/0003-release-failed-buffer-map-command.patch include/waddle/clvk.h | build
	$(if $(strip $(ClvkBuildArgs)),,$(error ClvkBuildArgs must name the reviewed compiler, provenance and policy selectors))
	python3 -B scripts/build_clvk_windows.py $(ClvkBuildArgs)

# Native execution requires a separately owned authenticated host receiver.
.PHONY: vgpu-opencl-test vgpu-opencl-fixture
vgpu-opencl-test:
	python3 -B tests/vgpu/clvk_build.py

build/vgpu_opencl_integration.exe: tests/vgpu/opencl_integration.c tests/vgpu/dxvk_heap_audit_windows.inc tests/vgpu/native_csr_heap_windows.inc tests/vgpu/loader_environment_windows.h tests/vgpu/loader_environment_windows.c include/waddle/venus_tcp.h include/waddle/clvk.h submodules/clvk/external/OpenCL-Headers/CL/cl.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/vgpu -Isubmodules/clvk/external/OpenCL-Headers $< tests/vgpu/loader_environment_windows.c -municode -o $@

vgpu-opencl-fixture: build/vgpu_opencl_integration.exe

# Actual guard implementation under a mock-only Linux Win32 boundary. This does
# not replace the native Windows environment round-trip target below.
.PHONY: vgpu-loader-environment-test vgpu-loader-environment-sanitizers vgpu-loader-environment-coverage
vgpu-loader-environment-test: | build
	$(CC) $(CFLAGS) -DWaddleLoaderEnvironmentTest -DWaddleLoaderEnvironmentFakeWindows -Itests/vgpu/loader_environment_fake -Itests/vgpu tests/vgpu/loader_environment_windows.c -o build/vgpu_loader_environment_mock
	./build/vgpu_loader_environment_mock

vgpu-loader-environment-sanitizers: | build
	$(CC) $(CFLAGS) -O1 -fsanitize=address,leak,undefined -fno-omit-frame-pointer -DWaddleLoaderEnvironmentTest -DWaddleLoaderEnvironmentFakeWindows -Itests/vgpu/loader_environment_fake -Itests/vgpu tests/vgpu/loader_environment_windows.c -o build/vgpu_loader_environment_mock_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/vgpu_loader_environment_mock_sanitized

build/vgpu_loader_environment_test.exe: tests/vgpu/loader_environment_windows.c tests/vgpu/loader_environment_windows.h | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -DWaddleLoaderEnvironmentTest -Itests/vgpu $< -o $@

vgpu-windows: build/vgpu_loader_environment_test.exe

vgpu-loader-environment-coverage: | build
	python3 -B scripts/loader_environment_coverage.py build/loader_environment_coverage

# Exact vendor API/helper bodies with CPU doubles; no native/GPU substitution.
.PHONY: vgpu-opencl-buffer-coverage
vgpu-opencl-buffer-coverage: | build
	python3 -B scripts/clvk_buffer_coverage.py build/clvk_buffer_coverage


# Vendor C++ runtime ownership regression. These targets only cross-compile;
# execute the C loader against this exact DLL in native Windows for acceptance.
# Compare the current POSIX runtime and candidate Win32 runtime; neither is
# accepted by compilation. Native handles, TLS slots and module retirement gate it.
.PHONY: vgpu-opencl-runtime-fixture
build/vgpu_clvk_runtime_posix.dll: tests/vgpu/clvk_runtime_windows.cpp | build
	x86_64-w64-mingw32-g++-posix -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -shared $< -o $@ -static-libgcc -static-libstdc++ -Wl,--whole-archive -Wl,-Bstatic -lwinpthread -Wl,--no-whole-archive -Wl,-Bdynamic

build/vgpu_clvk_runtime_win32.dll: tests/vgpu/clvk_runtime_windows.cpp | build
	x86_64-w64-mingw32-g++-win32 -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -shared -mthreads $< -o $@ -static-libgcc -static-libstdc++

build/vgpu_clvk_runtime_test.exe: tests/vgpu/clvk_runtime_windows.c tests/vgpu/dxvk_heap_audit_windows.inc | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror $< -o $@

build/vgpu_clvk_runtime_stress.exe: tests/vgpu/clvk_runtime_windows.c tests/vgpu/dxvk_heap_audit_windows.inc | build
	$(ZIG) cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -DWaddleRuntimeLifetimeCount=2048 $< -o $@

vgpu-opencl-runtime-fixture: build/vgpu_clvk_runtime_posix.dll build/vgpu_clvk_runtime_win32.dll build/vgpu_clvk_runtime_test.exe build/vgpu_clvk_runtime_stress.exe

# Real fixture compile boundaries only; these do not execute Windows code.
.PHONY: vgpu-opencl-runtime-probe-build-test
vgpu-opencl-runtime-probe-build-test:
	python3 -B tests/vgpu/clvk_runtime_probe_build_test.py
	python3 -O -B tests/vgpu/clvk_runtime_probe_build_test.py

# Actual Waddle runtime shim with Linux Win32 doubles. These gates do not
# qualify Windows loader/TEB behavior or the final linked OpenCL image.
.PHONY: vgpu-opencl-runtime-tls-test vgpu-opencl-runtime-tls-sanitizers vgpu-opencl-runtime-tls-tsan vgpu-opencl-runtime-tls-coverage
vgpu-opencl-runtime-tls-test: | build
	$(CC) $(CFLAGS) -pthread -DWaddleRuntimeTlsFakeWindows -Itests/vgpu tests/vgpu/clvk_runtime_tls_test.c -o build/vgpu_clvk_runtime_tls_test
	./build/vgpu_clvk_runtime_tls_test

vgpu-opencl-runtime-tls-sanitizers: | build
	$(CC) $(CFLAGS) -O1 -pthread -fsanitize=address,leak,undefined -fno-omit-frame-pointer -DWaddleRuntimeTlsFakeWindows -Itests/vgpu tests/vgpu/clvk_runtime_tls_test.c -o build/vgpu_clvk_runtime_tls_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/vgpu_clvk_runtime_tls_sanitized

vgpu-opencl-runtime-tls-tsan: | build
	$(CC) $(CFLAGS) -O1 -pthread -fsanitize=thread -fno-omit-frame-pointer -DWaddleRuntimeTlsFakeWindows -Itests/vgpu tests/vgpu/clvk_runtime_tls_test.c -o build/vgpu_clvk_runtime_tls_tsan
	TSAN_OPTIONS=halt_on_error=1 ./build/vgpu_clvk_runtime_tls_tsan

vgpu-opencl-runtime-tls-coverage: | build
	python3 -B tests/vgpu/clvk_runtime_tls_coverage_test.py
	python3 -O -B tests/vgpu/clvk_runtime_tls_coverage_test.py
	python3 -B scripts/clvk_runtime_tls_coverage.py

.PHONY: vgpu-opencl-runtime-eh-test vgpu-opencl-runtime-eh-sanitizers vgpu-opencl-runtime-eh-coverage
vgpu-opencl-runtime-eh-test: | build
	$(CC) $(CFLAGS) tests/vgpu/clvk_runtime_eh_pool_test.c -o build/vgpu_clvk_runtime_eh_test
	./build/vgpu_clvk_runtime_eh_test

vgpu-opencl-runtime-eh-sanitizers: | build
	$(CC) $(CFLAGS) -O1 -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/clvk_runtime_eh_pool_test.c -o build/vgpu_clvk_runtime_eh_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/vgpu_clvk_runtime_eh_sanitized

vgpu-opencl-runtime-eh-coverage: | build
	python3 -B tests/vgpu/clvk_runtime_tls_coverage_test.py
	python3 -B scripts/clvk_runtime_tls_coverage.py --eh-pool

# Real compiler/linker controls; explicit reviewed compiler selectors may be
# supplied through WADDLE_CLVK_CC/CXX. These never execute Windows binaries.
.PHONY: vgpu-opencl-runtime-link-test
vgpu-opencl-runtime-link-test:
	python3 -B tests/vgpu/clvk_runtime_link_test.py

# Qualification-only authenticated submit witness; never a production bridge flag.
OpenclGateSources = tests/vgpu/opencl_transport_gate_test.c tests/vgpu/opencl_transport_gate.c src/vgpu/venus_tcp_socket.c
OpenclGateWrap = -Wl,--wrap=fchmod -Wl,--wrap=fstat -Wl,--wrap=write -Wl,--wrap=close -Wl,--wrap=poll -Wl,--wrap=venus_tcp_now_ms
.PHONY: vgpu-opencl-transport-gate-test vgpu-opencl-transport-gate-sanitizers vgpu-opencl-transport-gate-coverage
vgpu-opencl-transport-gate-test: | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWaddleOpenclTransportGate -Itests/vgpu -pthread $(OpenclGateSources) $(OpenclGateWrap) -o build/vgpu_opencl_transport_gate_test
	./build/vgpu_opencl_transport_gate_test

vgpu-opencl-transport-gate-sanitizers: | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer -DWaddleOpenclTransportGate -Itests/vgpu -pthread $(OpenclGateSources) $(OpenclGateWrap) -o build/vgpu_opencl_transport_gate_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/vgpu_opencl_transport_gate_sanitized

vgpu-opencl-transport-gate-coverage: | build
	python3 -B tests/vgpu/opencl_transport_gate_coverage.py
	python3 -O -B tests/vgpu/opencl_transport_gate_coverage.py

.PHONY: vgpu-opencl-cancellation-test
vgpu-opencl-cancellation-test:
	python3 -B tests/vgpu/opencl_cancellation_test.py
	python3 -O -B tests/vgpu/opencl_cancellation_test.py
	python3 -B tests/vgpu/opencl_cancellation_order_test.py
	python3 -O -B tests/vgpu/opencl_cancellation_order_test.py

# Exact native heap-stage logic under portable Win32 doubles; native file-system
# behavior remains a separate Windows execution gate. No coverage exclusions.
.PHONY: vgpu-native-heap-stage-coverage
vgpu-native-heap-stage-coverage:
	python3 -B tests/vgpu/dxvk_heap_audit_coverage_test.py
	python3 -O -B tests/vgpu/dxvk_heap_audit_coverage_test.py
	python3 -B scripts/dxvk_heap_audit_coverage.py

# Approved OS-only first-use initialization before reload baselines (2026-10-22 amendment).
# Actual include under Win32 API doubles; native behavior is a separate census/run gate.
VgpuOpenclOsPreinitFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -Itests/vgpu
VgpuOpenclOsPreinitSources = tests/vgpu/opencl_os_preinit_test.c
VgpuOpenclOsPreinitHeaders = tests/vgpu/opencl_os_preinit_windows.inc tests/vgpu/win32_fake/opencl_os_preinit_fake.h
build/opencl-os-preinit: | build
	mkdir -p $@

build/opencl-os-preinit/host: $(VgpuOpenclOsPreinitSources) $(VgpuOpenclOsPreinitHeaders) | build/opencl-os-preinit
	$(CC) $(VgpuOpenclOsPreinitFlags) -O2 $< -o $@

build/vgpu_opencl_integration.exe: tests/vgpu/opencl_os_preinit_windows.inc

.PHONY: vgpu-opencl-os-preinit-test vgpu-opencl-os-preinit-sanitizers vgpu-opencl-os-preinit-coverage
vgpu-opencl-os-preinit-test: build/opencl-os-preinit/host
	./build/opencl-os-preinit/host

vgpu-opencl-os-preinit-sanitizers: | build/opencl-os-preinit
	$(CC) $(VgpuOpenclOsPreinitFlags) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuOpenclOsPreinitSources) -o build/opencl-os-preinit/host_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/opencl-os-preinit/host_sanitized

vgpu-opencl-os-preinit-coverage:
	python3 -B tests/vgpu/opencl_os_preinit_coverage.py

# Separate actual-DLL1100-reload stress. Native execution is root-supervised.
VgpuOpenclReloadFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread
VgpuOpenclReloadSources = tests/vgpu/opencl_reload_test.c
VgpuOpenclReloadHeaders = tests/vgpu/opencl_reload_windows.inc tests/vgpu/win32_fake/opencl_reload_fake.h
build/opencl-reload: | build
	mkdir -p $@

build/opencl-reload/host: $(VgpuOpenclReloadSources) $(VgpuOpenclReloadHeaders) | build/opencl-reload
	$(CC) $(VgpuOpenclReloadFlags) -O2 $< -o $@

build/vgpu_opencl_integration.exe: tests/vgpu/opencl_reload_windows.inc

.PHONY: vgpu-opencl-reload-test vgpu-opencl-reload-sanitizers vgpu-opencl-reload-coverage vgpu-opencl-reload-native-build vgpu-opencl-reload-verify
vgpu-opencl-reload-test: build/opencl-reload/host
	./build/opencl-reload/host
	python3 -B tests/vgpu/opencl_reload_output_test.py
	python3 -B -O tests/vgpu/opencl_reload_output_test.py

vgpu-opencl-reload-sanitizers: | build/opencl-reload
	$(CC) $(VgpuOpenclReloadFlags) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuOpenclReloadSources) -o build/opencl-reload/host_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/opencl-reload/host_sanitized

vgpu-opencl-reload-coverage:
	python3 -B tests/vgpu/opencl_reload_coverage.py

vgpu-opencl-reload-native-build: build/vgpu_opencl_integration.exe

# Required fresh frozen inputs; this gate confers lifecycle stress credit only.
vgpu-opencl-reload-verify:
	$(if $(strip $(VgpuOpenclReloadStdout)),,$(error VgpuOpenclReloadStdout is required))
	$(if $(strip $(VgpuOpenclReloadStderr)),,$(error VgpuOpenclReloadStderr is required))
	$(if $(strip $(VgpuOpenclReloadManifest)),,$(error VgpuOpenclReloadManifest is required))
	$(if $(strip $(VgpuOpenclReloadDll)),,$(error VgpuOpenclReloadDll is required))
	$(if $(strip $(VgpuOpenclReloadFixture)),,$(error VgpuOpenclReloadFixture is required))
	$(if $(strip $(VgpuOpenclReloadExit)),,$(error VgpuOpenclReloadExit must contain the actual native exit status))
	$(if $(strip $(VgpuOpenclReloadExpectedCandidateSha256)),,$(error VgpuOpenclReloadExpectedCandidateSha256 is required))
	$(if $(strip $(VgpuOpenclReloadExpectedFixtureSha256)),,$(error VgpuOpenclReloadExpectedFixtureSha256 is required))
	python3 -B scripts/opencl_reload_output.py --stdout "$(VgpuOpenclReloadStdout)" --stderr "$(VgpuOpenclReloadStderr)" --manifest "$(VgpuOpenclReloadManifest)" --dll "$(VgpuOpenclReloadDll)" --fixture "$(VgpuOpenclReloadFixture)" --exit-code "$(VgpuOpenclReloadExit)" --expected-candidate-sha256 "$(VgpuOpenclReloadExpectedCandidateSha256)" --expected-fixture-sha256 "$(VgpuOpenclReloadExpectedFixtureSha256)"

# Exact-target TLS certificate: portable API doubles and native calibration are separate.
VgpuOpenclSlotSources = tests/vgpu/opencl_slot_certificate_test.c
VgpuOpenclSlotHeaders = tests/vgpu/opencl_slot_certificate_windows.inc tests/vgpu/win32_fake/opencl_slot_certificate_fake.h
VgpuOpenclSlotFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror
build/vgpu_opencl_integration.exe: tests/vgpu/opencl_slot_certificate_windows.inc
build/opencl_slot: | build
	mkdir -p $@
build/opencl_slot/host: $(VgpuOpenclSlotSources) $(VgpuOpenclSlotHeaders) | build/opencl_slot
	$(CC) $(VgpuOpenclSlotFlags) -O2 $(VgpuOpenclSlotSources) -o $@
.PHONY: vgpu-opencl-slot-test vgpu-opencl-slot-sanitizers vgpu-opencl-slot-coverage vgpu-opencl-slot-native-build
vgpu-opencl-slot-test: build/opencl_slot/host
	./build/opencl_slot/host
vgpu-opencl-slot-sanitizers: | build/opencl_slot
	$(CC) $(VgpuOpenclSlotFlags) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuOpenclSlotSources) -o build/opencl_slot/sanitized
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/opencl_slot/sanitized
vgpu-opencl-slot-coverage:
	python3 -B tests/vgpu/opencl_slot_certificate_coverage.py
	python3 -O -B tests/vgpu/opencl_slot_certificate_coverage.py
build/vgpu_opencl_slot_certificate.exe: $(VgpuOpenclSlotSources) tests/vgpu/opencl_slot_certificate_windows.inc | build
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuOpenclSlotFlags) -O2 $(VgpuOpenclSlotSources) -o $@
vgpu-opencl-slot-native-build: build/vgpu_opencl_slot_certificate.exe


# Bounded CSR-port heap inventory under API doubles. Portable tests do not
# qualify native Windows heap behavior; the native target only cross-builds
# the exact-target calibration control.
VgpuNativeCsrFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -Itests/vgpu
VgpuNativeCsrSources = tests/vgpu/native_csr_heap_test.c
VgpuNativeCsrWindowsCC ?= x86_64-w64-mingw32-gcc-posix
VgpuNativeCsrHeaders = tests/vgpu/native_csr_heap_windows.inc tests/vgpu/win32_fake/native_csr_fake.h
.PHONY: vgpu-native-csr-heap-test vgpu-native-csr-heap-sanitizers vgpu-native-csr-heap-coverage vgpu-native-csr-heap-native-build
build/native-csr-heap: | build
	mkdir -p $@

build/native-csr-heap/host: $(VgpuNativeCsrSources) $(VgpuNativeCsrHeaders) | build/native-csr-heap
	$(CC) $(VgpuNativeCsrFlags) -O2 $(VgpuNativeCsrSources) -o $@

vgpu-native-csr-heap-test: build/native-csr-heap/host
	./build/native-csr-heap/host

vgpu-native-csr-heap-sanitizers: | build/native-csr-heap
	$(CC) $(VgpuNativeCsrFlags) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuNativeCsrSources) -o build/native-csr-heap/host_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/native-csr-heap/host_sanitized

vgpu-native-csr-heap-coverage:
	python3 -B scripts/native_csr_heap_coverage.py

# This only cross-compiles the exact-target control; root runs it under the
# supervised native Windows audit and makes no acceptance claim from the build.
build/vgpu_native_csr_heap_control.exe: $(VgpuNativeCsrSources) $(VgpuNativeCsrHeaders) | build
	$(VgpuNativeCsrWindowsCC) $(VgpuNativeCsrFlags) $< -o $@

vgpu-native-csr-heap-native-build: build/vgpu_native_csr_heap_control.exe

# Exact-target FLS certificate: portable gates do not confer native qualification.
VgpuOpenclFlsSources = tests/vgpu/opencl_fls_certificate_test.c
VgpuOpenclFlsHeaders = tests/vgpu/opencl_fls_certificate_windows.inc tests/vgpu/opencl_slot_certificate_windows.inc tests/vgpu/win32_fake/opencl_fls_certificate_fake.h tests/vgpu/win32_fake/opencl_slot_certificate_fake.h
VgpuOpenclFlsFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror
build/vgpu_opencl_integration.exe: tests/vgpu/opencl_fls_certificate_windows.inc
build/opencl_fls: | build
	mkdir -p $@
build/opencl_fls/host: $(VgpuOpenclFlsSources) $(VgpuOpenclFlsHeaders) | build/opencl_fls
	$(CC) $(VgpuOpenclFlsFlags) -O2 $(VgpuOpenclFlsSources) -o $@
.PHONY: vgpu-opencl-fls-test vgpu-opencl-fls-sanitizers vgpu-opencl-fls-coverage vgpu-opencl-fls-native-build
vgpu-opencl-fls-test: build/opencl_fls/host
	./build/opencl_fls/host
vgpu-opencl-fls-sanitizers: | build/opencl_fls
	$(CC) $(VgpuOpenclFlsFlags) -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer $(VgpuOpenclFlsSources) -o build/opencl_fls/sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/opencl_fls/sanitized
vgpu-opencl-fls-coverage:
	python3 -B tests/vgpu/opencl_fls_certificate_coverage.py
	python3 -O -B tests/vgpu/opencl_fls_certificate_coverage.py
build/vgpu_opencl_fls_certificate.exe: $(VgpuOpenclFlsSources) tests/vgpu/opencl_fls_certificate_windows.inc tests/vgpu/opencl_slot_certificate_windows.inc | build
	$(ZIG) cc -target x86_64-windows-gnu $(VgpuOpenclFlsFlags) -O2 $(VgpuOpenclFlsSources) -o $@
vgpu-opencl-fls-native-build: build/vgpu_opencl_fls_certificate.exe
