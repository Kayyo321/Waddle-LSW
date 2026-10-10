# AV platform targets. OS dependencies come from base system packages; driver
# declarations come only from the pinned Looking Glass submodule.
WaylandProtocolsDir ?= /usr/share/wayland-protocols
AvZigSources = src/av/av_audio.zig src/av/av_codec.zig src/av/av_layout.zig
AvHostFlags = -Isrc/av -Ibuild $(shell pkg-config --cflags wayland-client libpipewire-0.3)
AvDriverFlags = -Isubmodules/looking_glass/module
AvWindowsFlags = $(WindowsFlags) -Isrc/av -Isubmodules/looking_glass/vendor/ivshmem
AvWindowsSources = src/av/av_lease.c src/av/av_identity.c src/av/av_deploy.c src/av/av_guest_setup.c src/av/av_guids.c src/av/av_windows.c src/av/av_input.c src/av/av_capture.c src/av/av_wasapi.c src/av/av_ivshmem.c src/av/av_video.c
AvWindowsLibraries = -luser32 -ldwmapi -ld3d11 -ldxgi -lmmdevapi -lavrt -lole32 -luuid -lsetupapi

build/av_audio.o: src/av/av_audio.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/av_codec.o: src/av/av_codec.zig include/waddle/av_protocol.h | build
	$(ZIG) build-obj $< -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/av_layout.o: src/av/av_layout.zig | build
	$(ZIG) build-obj $< -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/av_transport_test: tests/av/transport.c src/av/av_video.c src/av/av_dmabuf.c build/av_audio.o build/av_layout.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) $^ $(LDFLAGS) -pthread -o $@

.PHONY: av-test av-sanitizers av-windows
av-test: build/av_input_transport_test build/av_input_test build/av_transport_test build/av_environment_test build/av_peer_test build/av_setup_test build/av_wayland_state_test build/av_deploy_test build/av_gpu_test
	./build/av_input_transport_test
	./build/av_platform_requests_test
	./build/av_identity_test
	cd tests/av/legacy && sha256sum --check SHA256SUMS
	./build/av_lifecycle_transport_test
	./build/av_input_test
	./build/av_lease_test
	./build/av_windows_lease_test
	cd tests/av/v2 && sha256sum --check SHA256SUMS
	./build/av_gpu_test
	python3 tests/av/package.py
	$(ZIG) test src/cli/av_commands.zig -Isrc/cli
	./build/av_deploy_test
	./build/av_wayland_state_test
	./build/av_setup_test
	./build/av_environment_test
	./build/av_peer_test
	./build/av_transport_test
	$(ZIG) test src/av/av_audio.zig
	$(ZIG) test src/av/av_codec.zig -Iinclude
	$(ZIG) test src/av/av_layout.zig -lc
av-sanitizers:
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 $(MAKE) -B av-test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"

.PHONY: av-coverage
av-coverage: build/av_audio.o build/av_layout.o build/av_codec.o build/av_v2_codec.o
	python3 tests/av/input_coverage.py
	python3 tests/av/lease_coverage.py
	python3 tests/av/identity_coverage.py
	python3 tests/av/coverage.py av_audio
	python3 tests/av/coverage.py av_codec
	python3 tests/av/coverage.py av_layout
	python3 tests/av/coverage.py av_video

build/xdg_shell_client.h: $(WaylandProtocolsDir)/stable/xdg-shell/xdg-shell.xml | build
	wayland-scanner client-header $< $@
build/xdg_shell_protocol.c: $(WaylandProtocolsDir)/stable/xdg-shell/xdg-shell.xml | build
	wayland-scanner private-code $< $@
build/linux_dmabuf_client.h: $(WaylandProtocolsDir)/stable/linux-dmabuf/linux-dmabuf-v1.xml | build
	wayland-scanner client-header $< $@
build/linux_dmabuf_protocol.c: $(WaylandProtocolsDir)/stable/linux-dmabuf/linux-dmabuf-v1.xml | build
	wayland-scanner private-code $< $@

build/av_environment_test: tests/av/environment.c src/av/av_environment.c src/av/av_gpu.c build/av_codec.o build/av_layout.o build/daemon_config.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) $^ $(LDFLAGS) -o $@

build/av_environment.o: src/av/av_environment.c src/av/av_environment.h src/av/av_layout.h src/daemon/daemon_config.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) -c $< -o $@

build/vendor/av/module/kvmfr.c: submodules/looking_glass/module/kvmfr.c submodules/looking_glass/module/kvmfr.h submodules/looking_glass/module/Makefile scripts/patches/kvmfr_capacity.patch | build/vendor
	mkdir -p build/vendor/av/module
	cp submodules/looking_glass/module/kvmfr.c submodules/looking_glass/module/kvmfr.h submodules/looking_glass/module/Makefile build/vendor/av/module/
	cp submodules/looking_glass/LICENSE build/vendor/av/module/LICENSE
	git apply --directory=build/vendor/av/module scripts/patches/kvmfr_capacity.patch
build/waddle-av-setup: src/av/av_gpu.c src/av/av_setup.c src/av/av_layout.h src/av/av_kvmfr.h build/av_codec.o build/vendor/av/module/kvmfr.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) src/av/av_setup.c src/av/av_gpu.c build/av_codec.o $(LDFLAGS) -o $@
.PHONY: av-module
av-module: build/waddle-av-setup
	./build/waddle-av-setup --build-module

build/av_peer_test: tests/av/peer.c src/av/av_peer.c build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $^ $(LDFLAGS) -o $@

build/av_audio_windows.lib: src/av/av_audio.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/av_codec_windows.lib: src/av/av_codec.zig include/waddle/av_protocol.h | build
	$(ZIG) build-lib $< -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/av_layout_windows.lib: src/av/av_layout.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@
build/waddle-guest-av.exe: src/av/av_guest.c src/av/av_peer.c $(AvWindowsSources) build/av_audio_windows.lib build/av_codec_windows.lib build/av_layout_windows.lib | build
	$(ZIG) cc $(AvWindowsFlags) $^ $(AvWindowsLibraries) -lws2_32 -o $@
av-windows: build/waddle-guest-av.exe

AvHostSources = src/av/av_lease.c src/av/av_identity.c src/av/av_input.c src/av/av_host.c src/av/av_peer.c src/av/av_video.c src/av/av_wayland.c src/av/av_pipewire.c src/av/av_dmabuf.c
build/waddle-av-host: $(AvHostSources) build/xdg_shell_client.h build/linux_dmabuf_client.h build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_audio.o build/av_codec.o build/av_layout.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-pedantic $(AvHostFlags) $(AvDriverFlags) $(AvHostSources) build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_audio.o build/av_codec.o build/av_layout.o $(LDFLAGS) $(shell pkg-config --libs wayland-client libpipewire-0.3) -o $@
.PHONY: av
av: build/waddle-av-host build/waddle-av-setup av-windows

build/av_windows_test.exe: tests/av/windows.c $(AvWindowsSources) build/av_audio_windows.lib build/av_codec_windows.lib build/av_layout_windows.lib | build
	$(ZIG) cc $(AvWindowsFlags) $^ $(AvWindowsLibraries) -lgdi32 -lwinmm -o $@

# Explicit native driver gate: fails if setup has not provisioned KVMFR.
build/av_kvmfr_test: tests/av/kvmfr.c src/av/av_environment.c src/av/av_dmabuf.c build/av_layout.o build/daemon_config.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) $^ $(LDFLAGS) -o $@
.PHONY: av-native-kvmfr-test
av-native-kvmfr-test: build/av_kvmfr_test
	./build/av_kvmfr_test

# The signed IVSHMEM package is supplied by the same immutable B7 dependency.
# Only ignored build outputs contain extracted third-party binaries/licenses.
build/vendor/av/ivshmem/ivshmem.inf: | build/vendor
	mkdir -p build/vendor/av/download build/vendor/av/ivshmem
	curl --fail --location --proto '=https' --tlsv1.2 https://looking-glass.io/artifact/B7/host -o build/vendor/av/download/looking_glass_b7.zip
	echo 'c2415a5a0c405f1d6aa936986bdd4b806c50574b4521747e113c3be2be047b1b  build/vendor/av/download/looking_glass_b7.zip' | sha256sum --check
	7z e -y -obuild/vendor/av/download build/vendor/av/download/looking_glass_b7.zip looking-glass-host-setup.exe
	7z e -y -obuild/vendor/av/ivshmem build/vendor/av/download/looking-glass-host-setup.exe ivshmem.inf ivshmem.sys ivshmem.cat LICENSE.txt
.PHONY: av-drivers
av-drivers: build/vendor/av/ivshmem/ivshmem.inf

build/av_platform_test: tests/av/platform.c tests/av/platform_requests.h src/av/av_lease.c src/av/av_identity.c src/av/av_input.c src/av/av_wayland.c src/av/av_pipewire.c src/av/av_video.c src/av/av_dmabuf.c build/xdg_shell_client.h build/linux_dmabuf_client.h build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_audio.o build/av_codec.o build/av_layout.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-pedantic $(AvHostFlags) $(AvDriverFlags) tests/av/platform.c src/av/av_lease.c src/av/av_identity.c src/av/av_input.c src/av/av_wayland.c src/av/av_pipewire.c src/av/av_video.c src/av/av_dmabuf.c build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_audio.o build/av_codec.o build/av_layout.o $(LDFLAGS) $(shell pkg-config --libs wayland-client libpipewire-0.3) -o $@
.PHONY: av-native-platform-test
av-native-platform-test: build/av_platform_test
	./build/av_platform_test

# The SDK projection DLL is built on native Windows by scripts/build_wgc.cmd.
# Linux packaging consumes the same-run CI artifact; never omit the DLL.
build/av_wgc.dll: | build
	@echo 'AV distribution needs the matching Windows SDK-built build/av_wgc.dll (scripts/build_wgc.cmd or complete CI bundle)' >&2
	@exit 1

.PHONY: av-distribution
av-distribution: build/waddle build/waddled av windows av-drivers build/av_wgc.dll build/av_windows_test.exe
	sh scripts/package_av.sh

# Fake syscall wrappers ensure privileged reuse tests never touch real devices.
build/av_setup_test.o: src/av/av_setup.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) -Dmain=av_setup_entry -c $< -o $@
build/av_setup_test: tests/av/setup.c src/av/av_gpu.c build/av_setup_test.o build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $^ $(LDFLAGS) -Wl,--wrap=geteuid,--wrap=open,--wrap=ioctl,--wrap=lseek,--wrap=close,--wrap=fork,--wrap=waitpid -o $@

build/av_wayland_state_test: tests/av/wayland_state.c src/av/av_lease.c src/av/av_identity.c src/av/av_input.c src/av/av_wayland.c src/av/av_video.c src/av/av_dmabuf.c build/xdg_shell_client.h build/linux_dmabuf_client.h build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_codec.o build/av_audio.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-pedantic $(AvHostFlags) $(AvDriverFlags) tests/av/wayland_state.c src/av/av_lease.c src/av/av_identity.c src/av/av_input.c src/av/av_video.c src/av/av_dmabuf.c build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_codec.o build/av_audio.o $(LDFLAGS) $(shell pkg-config --libs wayland-client) -Wl,--wrap=wl_proxy_marshal_flags,--wrap=wl_proxy_get_version,--wrap=wl_proxy_add_listener,--wrap=wl_display_flush,--wrap=wl_proxy_destroy,--wrap=clock_gettime,--wrap=wl_display_dispatch_pending,--wrap=wl_display_disconnect -o $@

build/av_deploy_test: tests/av/deploy.c src/av/av_deploy.c src/av/av_deploy.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av tests/av/deploy.c src/av/av_deploy.c $(LDFLAGS) -o $@

build/av_gpu.o: src/av/av_gpu.c src/av/av_gpu.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av -c $< -o $@
build/av_gpu_test: tests/av/gpu.c src/av/av_gpu.c build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $^ $(LDFLAGS) -o $@

# Hardware-independent bounded input state machine.
build/av_input_test: tests/av/input.c src/av/av_input.c build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $^ $(LDFLAGS) -o $@

build/av_input_transport_test: tests/av/input_transport.c src/av/av_input.c src/av/av_peer.c build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $^ $(LDFLAGS) -o $@

.PHONY: av-input-test
av-input-test: build/av_input_test build/av_input_transport_test build/av_wayland_state_test
	./build/av_platform_requests_test
	./build/av_identity_test
	cd tests/av/legacy && sha256sum --check SHA256SUMS
	./build/av_lifecycle_transport_test
	./build/av_input_test
	./build/av_lease_test
	./build/av_windows_lease_test
	cd tests/av/v2 && sha256sum --check SHA256SUMS
	./build/av_input_transport_test
	./build/av_wayland_state_test
	$(ZIG) test src/av/av_codec.zig -Iinclude

# The same portable gate is used before native HWND mutations and in transport tests.
build/av_identity_test: tests/av/identity.c src/av/av_identity.c build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $^ $(LDFLAGS) -o $@
av-test av-input-test: build/av_identity_test

build/av_legacy_codec.o: tests/av/legacy/av_codec.zig tests/av/legacy/av_protocol.h | build
	$(ZIG) build-obj $< -Itests/av/legacy -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/av_lifecycle_transport_test: tests/av/lifecycle_transport.c tests/av/legacy_fixture.c tests/av/legacy/av_peer.c tests/av/legacy/av_peer.h tests/av/legacy/av_protocol.h src/av/av_identity.c src/av/av_peer.c build/av_codec.o build/av_legacy_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av tests/av/lifecycle_transport.c tests/av/legacy_fixture.c src/av/av_identity.c src/av/av_peer.c build/av_codec.o build/av_legacy_codec.o $(LDFLAGS) -o $@
av-test av-input-test: build/av_lifecycle_transport_test

build/av_guest_quiescence_test.exe: tests/av/guest_quiescence.c src/av/av_guest.c src/av/av_peer.c $(AvWindowsSources) build/av_audio_windows.lib build/av_codec_windows.lib build/av_layout_windows.lib | build
	$(ZIG) cc $(AvWindowsFlags) tests/av/guest_quiescence.c src/av/av_peer.c $(AvWindowsSources) build/av_audio_windows.lib build/av_codec_windows.lib build/av_layout_windows.lib $(AvWindowsLibraries) -lws2_32 -o $@

# Exercise the real synthetic native-platform request callback without a compositor.
build/av_platform_requests_test: tests/av/platform_requests.c tests/av/platform_requests.h build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av tests/av/platform_requests.c build/av_codec.o $(LDFLAGS) -o $@
av-test av-input-test: build/av_platform_requests_test

# Frozen pre-lease codec: no runtime binary links this compatibility fixture.
build/av_v2_codec.o: tests/av/v2/av_codec.zig tests/av/v2/av_protocol.h | build
	$(ZIG) build-obj $< -Itests/av/v2 -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@
build/av_lease_test: tests/av/lease.c src/av/av_lease.c src/av/av_input.c src/av/av_identity.c src/av/av_peer.c build/av_codec.o build/av_v2_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $^ $(LDFLAGS) -o $@
av-test av-input-test: build/av_lease_test

# Production Win32 adapter with deterministic API doubles; no physical desktop.
build/av_windows_lease_test: tests/av/windows_lease.c src/av/av_windows.c src/av/av_windows.h src/av/av_lease.c src/av/av_input.c src/av/av_identity.c build/av_codec.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av tests/av/windows_lease.c src/av/av_lease.c src/av/av_input.c src/av/av_identity.c build/av_codec.o $(LDFLAGS) -o $@
build/av_windows_lease_test.exe: tests/av/windows_lease.c src/av/av_windows.c src/av/av_windows.h src/av/av_lease.c src/av/av_input.c src/av/av_identity.c build/av_codec_windows.lib | build
	$(ZIG) cc $(AvWindowsFlags) tests/av/windows_lease.c src/av/av_lease.c src/av/av_input.c src/av/av_identity.c build/av_codec_windows.lib -o $@
av-test av-input-test: build/av_windows_lease_test
