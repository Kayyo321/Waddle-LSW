CC ?= cc
ZIG ?= zig
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc -Isrc/common -Isrc/cli -Isrc/daemon -Isrc/guest -Isrc/mock
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
COMMON = src/common/protocol.c src/common/arguments.c build/path_rules.o

.PHONY: all cli test clean zig-test test-sanitizers demo windows windows-test coverage qemu-vendor qemu-clean

# Bundled Vendor Dependencies
QEMU_DIR = submodules/qemu
QEMU_BUILD_DIR = $(QEMU_DIR)/build
QEMU_SYSTEM_X86 = $(QEMU_BUILD_DIR)/qemu-system-x86_64

all: cli av-distribution

# CLI regressions do not require a Windows SDK-produced capture DLL.
cli: build/waddle build/waddle-mock-guest build/waddled qemu-vendor

build:
	mkdir -p $@

build/vendor: | build
	mkdir -p $@

$(QEMU_BUILD_DIR)/config-host.mak: $(QEMU_DIR)/configure
	cd $(QEMU_DIR) && env -u CFLAGS -u CPPFLAGS -u LDFLAGS ./configure --target-list=x86_64-softmmu --enable-kvm --disable-docs --disable-gtk --disable-sdl --disable-vnc

$(QEMU_SYSTEM_X86): $(QEMU_BUILD_DIR)/config-host.mak
	ninja -C $(QEMU_BUILD_DIR) qemu-system-x86_64

build/vendor/qemu-system-x86_64: | build/vendor
	@if [ -x /usr/bin/qemu-system-x86_64 ]; then \
		cp -f /usr/bin/qemu-system-x86_64 $@; \
	elif which qemu-system-x86_64 >/dev/null 2>&1; then \
		cp -f "$$(which qemu-system-x86_64)" $@; \
	elif [ -f $(QEMU_SYSTEM_X86) ]; then \
		cp -f $(QEMU_SYSTEM_X86) $@; \
	elif [ -d $(QEMU_DIR) ]; then \
		$(MAKE) $(QEMU_SYSTEM_X86) && cp -f $(QEMU_SYSTEM_X86) $@; \
	fi
	@[ -f $@ ] && chmod +x $@ || true

build/vendor/virtiofsd: | build/vendor
	@if [ -x /usr/libexec/virtiofsd ]; then \
		cp -f /usr/libexec/virtiofsd $@; \
	elif [ -x /usr/lib/qemu/virtiofsd ]; then \
		cp -f /usr/lib/qemu/virtiofsd $@; \
	elif which virtiofsd >/dev/null 2>&1; then \
		cp -f "$$(which virtiofsd)" $@; \
	fi
	@[ -f $@ ] && chmod +x $@ || true

build/vendor/pc-bios: | build/vendor
	@if [ -d $(QEMU_DIR)/pc-bios ]; then \
		rm -rf $@ && cp -r $(QEMU_DIR)/pc-bios $@; \
	fi

qemu-vendor: build/vendor/qemu-system-x86_64 build/vendor/virtiofsd build/vendor/pc-bios

qemu-clean:
	rm -rf $(QEMU_BUILD_DIR)

build/path_rules.o: src/common/path_rules.zig | build
	$(ZIG) build-obj src/common/path_rules.zig -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/unit: tests/unit/unit.c $(COMMON) include/waddle/cli_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/unit.c $(COMMON) $(LDFLAGS) -o $@

build/device_storage.o: src/daemon/device_storage.zig src/daemon/daemon_device.h src/daemon/daemon_config.h | build
	$(ZIG) build-obj $< -D_GNU_SOURCE -Iinclude -Isrc/daemon -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/device_commands.o: src/cli/device_commands.zig src/cli/device_commands.h src/daemon/daemon_device.h | build
	$(ZIG) build-obj $< -Iinclude -Isrc/daemon -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/av_commands.o: src/cli/av_commands.zig src/cli/av_commands.h | build
	$(ZIG) build-obj $< -Isrc/cli -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/waddle: src/av/av_deploy.c src/av/av_deploy.h build/av_commands.o src/cli/av_managed.inc build/device_commands.o src/cli/host.c src/cli/session.c src/cli/terminal.c src/daemon/daemon_client.c src/daemon/daemon_device.c build/device_storage.o src/daemon/daemon_protocol.c build/daemon_config.o src/cli/session.h src/cli/terminal.h src/daemon/daemon_client.h src/daemon/daemon_device.h $(COMMON) include/waddle/cli_protocol.h include/waddle/daemon_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/av/av_deploy.c src/cli/host.c src/cli/session.c src/cli/terminal.c src/daemon/daemon_client.c src/daemon/daemon_device.c build/device_storage.o src/daemon/daemon_protocol.c build/daemon_config.o build/device_commands.o build/av_commands.o $(COMMON) $(LDFLAGS) -o $@

build/waddle-mock-guest: src/mock/mock_guest.c src/mock/mock_process.c src/mock/mock_process.h $(COMMON) include/waddle/cli_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/mock/mock_guest.c src/mock/mock_process.c $(COMMON) $(LDFLAGS) -lutil -o $@

build/integration: tests/integration/integration.c $(COMMON) include/waddle/cli_protocol.h src/common/common.h src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/integration/integration.c $(COMMON) $(LDFLAGS) -lutil -o $@

build/terminal.o: src/cli/terminal.c src/cli/terminal.h src/common/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/cli/terminal.c -o $@

build/session.o: src/cli/session.c src/cli/session.h src/cli/terminal.h src/common/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/cli/session.c -o $@

build/mock_process.o: src/mock/mock_process.c src/mock/mock_process.h src/common/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/mock/mock_process.c -o $@

build/daemon_config.o: src/daemon/daemon_config.zig src/daemon/daemon_config.h include/waddle/daemon_protocol.h | build
	$(ZIG) build-obj src/daemon/daemon_config.zig -Iinclude -Isrc/daemon -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/test_daemon_config: tests/daemon/test_daemon_config.c build/daemon_config.o src/daemon/daemon_config.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_config.c build/daemon_config.o $(LDFLAGS) -o $@

zig-test: build/path_rules.o build/device_storage.o
	$(ZIG) test tests/unit/test_cli.zig -Iinclude -Isrc/common -Isrc/cli src/common/protocol.c src/common/arguments.c build/path_rules.o -lc
	$(ZIG) test src/common/path_rules.zig
	$(ZIG) test src/guest/guest_codec.zig
	$(ZIG) test src/daemon/daemon_config.zig -Iinclude -Isrc/daemon -lc
	$(ZIG) test src/daemon/device_storage.zig -D_GNU_SOURCE -Iinclude -Isrc/daemon src/daemon/daemon_device.c build/daemon_config.o -lc
	$(ZIG) test src/cli/device_commands.zig -D_GNU_SOURCE -Iinclude -Isrc/daemon src/daemon/daemon_device.c src/daemon/daemon_client.c src/daemon/daemon_protocol.c build/device_storage.o build/daemon_config.o -lc

build/test_daemon_protocol: tests/daemon/test_daemon_protocol.c src/daemon/daemon_protocol.c include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_protocol.c src/daemon/daemon_protocol.c $(LDFLAGS) -o $@

build/test_daemon_qemu: tests/daemon/test_daemon_qemu.c src/daemon/daemon_qemu.c src/daemon/daemon_qmp.c build/daemon_config.o src/daemon/daemon_qemu.h src/daemon/daemon_qmp.h src/daemon/daemon_config.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_qemu.c src/daemon/daemon_qemu.c src/daemon/daemon_qmp.c build/daemon_config.o $(LDFLAGS) -lpthread -o $@

build/test_daemon_fs: tests/daemon/test_daemon_fs.c src/daemon/daemon_fs.c src/common/arguments.c build/daemon_config.o build/path_rules.o src/daemon/daemon_fs.h src/daemon/daemon_config.h src/common/path_rules.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_fs.c src/daemon/daemon_fs.c src/common/arguments.c build/daemon_config.o build/path_rules.o $(LDFLAGS) -o $@

build/test_daemon_device: tests/daemon/test_daemon_device.c src/daemon/daemon_device.c build/device_storage.o build/daemon_config.o src/daemon/daemon_device.h src/daemon/daemon_config.h include/waddle/daemon_protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_device.c src/daemon/daemon_device.c build/device_storage.o build/daemon_config.o $(LDFLAGS) -o $@

DAEMON_COMMON = build/av_gpu.o build/av_codec.o build/av_environment.o build/av_layout.o src/daemon/daemon_protocol.c src/daemon/daemon_state.c src/daemon/daemon_server.c src/daemon/daemon_qemu.c src/daemon/daemon_qmp.c src/daemon/daemon_fs.c src/daemon/daemon_device.c src/common/arguments.c build/device_storage.o build/daemon_config.o build/path_rules.o

build/waddled: src/daemon/daemon_main.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/daemon/daemon_main.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

build/test_daemon_server: tests/daemon/test_daemon_server.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_server.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

build/test_daemon_client: tests/daemon/test_daemon_client.c src/daemon/daemon_client.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_daemon_client.c src/daemon/daemon_client.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

build/test_auto_terminal: tests/daemon/test_auto_terminal.c src/daemon/daemon_client.c $(DAEMON_COMMON) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/daemon/test_auto_terminal.c src/daemon/daemon_client.c $(DAEMON_COMMON) $(LDFLAGS) -lpthread -o $@

test: cli build/unit build/integration build/test_daemon_protocol build/test_daemon_config build/test_daemon_qemu build/test_daemon_fs build/test_daemon_device build/test_daemon_server build/test_daemon_client build/test_auto_terminal zig-test
	./build/unit
	./build/integration
	./build/test_daemon_protocol
	./build/test_daemon_config
	./build/test_daemon_qemu
	./build/test_daemon_fs
	./build/test_daemon_device
	./build/test_daemon_server
	./build/test_daemon_client
	./build/test_auto_terminal
	python3 tests/integration/device_commands.py
	python3 tests/integration/device_storage.py
	python3 tests/integration/device_faults.py
	sh tests/integration/path_options.sh
	sh tests/acceptance/demo_fs.sh

test-sanitizers: clean
	$(MAKE) test \
		CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,leak,undefined"

demo: cli
	sh tests/integration/demo.sh

clean:
	rm -rf build

# Windows 10 1809 system declarations; C owns native APIs, Zig owns wire parsing.
WindowsFlags = -target x86_64-windows-gnu -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006 -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -Isrc/common -Isrc/guest
GuestSources = src/guest/guest_listener.c src/guest/guest_session.c src/guest/guest_wire.c src/guest/guest_input.c src/guest/guest_output.c src/guest/guest_process.c src/guest/guest_environment.c
GuestHeaders = src/guest/guest_session.h src/guest/guest_wire.h src/guest/guest_pump.h src/guest/guest_process.h src/guest/guest_environment.h src/guest/guest_codec.h

build/guest_codec.lib: src/guest/guest_codec.zig | build
	$(ZIG) build-lib $< -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -femit-bin=$@

build/waddle-guest-exec.exe: $(GuestSources) $(GuestHeaders) build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) $(GuestSources) build/guest_codec.lib -lws2_32 -o $@

windows: build/waddle-guest-exec.exe

build/windows_guest_test.exe: tests/windows/windows_guest.c src/guest/guest_codec.h build/guest_codec.lib | build
	$(ZIG) cc $(WindowsFlags) tests/windows/windows_guest.c build/guest_codec.lib -lws2_32 -municode -o $@

windows-test: windows build/windows_guest_test.exe
	./build/windows_guest_test.exe

coverage: cli build/test_daemon_device build/path_rules.o
	sh tests/coverage.sh
	python3 tests/device_coverage.py
	python3 tests/device_branches.py
	python3 tests/device_branches.py cli

# Explicit real VM gate; never silently substituted with the mock transport.
build/vm_terminal: tests/acceptance/vm_terminal.c | build
	$(CC) $(CFLAGS) $< $(LDFLAGS) -lutil -o $@

.PHONY: vm-test
vm-test: build/waddle build/windows_guest_test.exe build/vm_terminal
	bash tests/acceptance/vm_acceptance.sh

.PHONY: device-stress
device-stress: build/waddle
	python3 tests/integration/device_stress.py

include scripts/av.mk

include scripts/mesa_cpu_cache.mk
include scripts/vgpu.mk
include scripts/vgpu_dxvk.mk
include scripts/vgpu_workloads.mk
