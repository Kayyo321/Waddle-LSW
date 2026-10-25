# Seamless-interop session lane: host feature runtime, Linux socket owner and control peer.
# Included by GNUmakefile through scripts/interop_lane_*.mk. Owns only the targets below.
# The control peer and socket owner gates run from the worktree root; the host runtime
# gate builds the fixture and runs its ASan/LSan/UBSan integration as part of its script.

.PHONY: interop-session-lane-test interop-session-lane-coverage
interop-session-lane-test: interop-host-control-test interop-host-session-test interop-linux-socket-test interop-control-peer-test interop-control-peer-abi-test
interop-session-lane-coverage:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/linux_socket_owner_coverage.py
	python3 tests/av/coverage.py control_peer
	python3 tests/av/coverage.py control_peer_abi

InteropLaneTests += interop-session-lane-test
InteropLaneCoverage += interop-session-lane-coverage

# Application runtime and reconnect supervisor. The binary links the control-host fixture's in-process
# daemon, VSOCK socketpair seam and guest peer, and drives the real runtime through session_app.
SessionAppRuntimeSources = src/interop/session_host.c src/interop/linux_session_owner.c src/interop/linux_socket_owner.c src/interop/interop_mapping_linux.c src/interop/interop_mapping_init.c src/interop/interop_native_atomic.c src/daemon/daemon_device.c
SessionAppObjects = build/interop_control_peer_abi.o build/interop_codec.o build/daemon_wire.o build/device_storage.o build/daemon_config.o build/path_rules.o build/interop_layout_abi.o
SessionAppWrapFlags = $(shell grep -ho '__wrap_[a-z_0-9]*' tests/interop/session_control_host.c | sort -u | sed 's/^__wrap_/-Wl,--wrap=/')
SessionAppHeaders = src/interop/session_control_host.h src/interop/session_app.h src/interop/session_supervisor.h src/interop/session_handler.h
build/session_app_test: tests/interop/session_app.c tests/interop/session_supervisor.c tests/interop/session_control_host.c tests/interop/session_control_host_private.c src/interop/session_app.c src/interop/session_supervisor.c src/interop/session_handlers_builtin.c $(SessionAppHeaders) $(SessionAppRuntimeSources) $(SessionAppObjects) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -ffunction-sections -fdata-sections -Isrc/interop -Isubmodules/looking_glass/module tests/interop/session_app.c tests/interop/session_supervisor.c tests/interop/session_control_host_private.c src/interop/session_app.c src/interop/session_supervisor.c src/interop/session_handlers_builtin.c $(SessionAppRuntimeSources) $(SessionAppObjects) $(LDFLAGS) $(SessionAppWrapFlags) -Wl,--gc-sections -pthread -o $@
.PHONY: interop-session-app-test
interop-session-app-test: build/session_app_test
	./build/session_app_test

# Fast fixture gate for development: the same control-host runner as interop-host-control-test,
# without the coverage and sanitizer phases of its script.
SessionHostFixtureSources = tests/interop/session_control_host.c tests/interop/session_control_host_private.c tests/interop/session_main_private.c
build/session_control_host_fixture: $(SessionHostFixtureSources) $(SessionAppHeaders) $(SessionAppRuntimeSources) $(SessionAppObjects) src/interop/session_main.c src/interop/session_control_host.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -ffunction-sections -fdata-sections -Isrc/interop -Isubmodules/looking_glass/module $(SessionHostFixtureSources) $(SessionAppRuntimeSources) $(SessionAppObjects) $(LDFLAGS) $(SessionAppWrapFlags) -Wl,--gc-sections -pthread -o $@
.PHONY: interop-session-host-fixture
interop-session-host-fixture: build/session_control_host_fixture
	./build/session_control_host_fixture
