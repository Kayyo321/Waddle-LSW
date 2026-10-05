# AV platform targets. OS dependencies come from base system packages; driver
# declarations come only from the pinned Looking Glass submodule.
AvZigSources = src/av/av_audio.zig src/av/av_codec.zig src/av/av_layout.zig
AvHostFlags = -Isrc/av -Ibuild $(shell pkg-config --cflags wayland-client libpipewire-0.3)
AvDriverFlags = -Isubmodules/looking_glass/module
AvWindowsFlags = $(WindowsFlags) -Isrc/av -Isubmodules/looking_glass/vendor/ivshmem
AvWindowsSources = src/av/av_guids.c src/av/av_windows.c src/av/av_capture.c src/av/av_wasapi.c src/av/av_ivshmem.c src/av/av_video.c
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
av-test: build/av_transport_test build/av_environment_test build/av_peer_test
	./build/av_environment_test
	./build/av_peer_test
	./build/av_transport_test
	$(ZIG) test src/av/av_audio.zig
	$(ZIG) test src/av/av_codec.zig -Iinclude
	$(ZIG) test src/av/av_layout.zig
av-sanitizers:
	$(MAKE) -B build/av_transport_test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/av_transport_test
	$(ZIG) test src/av/av_audio.zig
	$(ZIG) test src/av/av_codec.zig -Iinclude
	$(ZIG) test src/av/av_layout.zig

build/xdg_shell_client.h: /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml | build
	wayland-scanner client-header $< $@
build/xdg_shell_protocol.c: /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml | build
	wayland-scanner private-code $< $@
build/linux_dmabuf_client.h: /usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml | build
	wayland-scanner client-header $< $@
build/linux_dmabuf_protocol.c: /usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml | build
	wayland-scanner private-code $< $@

build/av_environment_test: tests/av/environment.c src/av/av_environment.c build/av_layout.o build/daemon_config.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) $^ $(LDFLAGS) -o $@

build/av_environment.o: src/av/av_environment.c src/av/av_environment.h src/av/av_layout.h src/daemon/daemon_config.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) -c $< -o $@

build/vendor/av/module/kvmfr.c: submodules/looking_glass/module/kvmfr.c submodules/looking_glass/module/kvmfr.h submodules/looking_glass/module/Makefile | build/vendor
	mkdir -p build/vendor/av/module
	cp submodules/looking_glass/module/kvmfr.c submodules/looking_glass/module/kvmfr.h submodules/looking_glass/module/Makefile build/vendor/av/module/
	cp submodules/looking_glass/LICENSE build/vendor/av/module/LICENSE
build/waddle-av-setup: src/av/av_setup.c src/av/av_layout.h build/vendor/av/module/kvmfr.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/av $(AvDriverFlags) $< $(LDFLAGS) -o $@
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

AvHostSources = src/av/av_host.c src/av/av_peer.c src/av/av_video.c src/av/av_wayland.c src/av/av_pipewire.c src/av/av_dmabuf.c
build/waddle-av-host: $(AvHostSources) build/xdg_shell_client.h build/linux_dmabuf_client.h build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_audio.o build/av_codec.o build/av_layout.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-pedantic $(AvHostFlags) $(AvDriverFlags) $(AvHostSources) build/xdg_shell_protocol.c build/linux_dmabuf_protocol.c build/av_audio.o build/av_codec.o build/av_layout.o $(LDFLAGS) $(shell pkg-config --libs wayland-client libpipewire-0.3) -o $@
.PHONY: av
av: build/waddle-av-host build/waddle-av-setup av-windows
