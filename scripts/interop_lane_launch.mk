# Launch lane (TODO #29): `waddle open` request parsing (#29a) and the activation
# token store (#29e). Owns only the targets below; scripts/interop_lanes.mk
# aggregates them into interop-test, interop-sanitizers and interop-coverage.

InteropOpenRequestSanitize = -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fno-omit-frame-pointer -fsanitize=address,leak,undefined -fno-sanitize-recover=undefined

# waddle open client (#29b, #29d): its tests link the C pieces it calls.
OpenCommandObjects = build/interop_codec.o build/daemon_config.o build/daemon_fs.o build/path_filesystem.o build/path_rules.o build/arguments.o
OpenCommandFlags = -Iinclude -Isrc/daemon -Isrc/common

build/daemon_fs.o: src/daemon/daemon_fs.c src/daemon/daemon_fs.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/path_filesystem.o: src/common/path_filesystem.c src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/arguments.o: src/common/arguments.c src/common/path_rules.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# End-to-end `waddle open` against the built binary and a fake session.sock. Frames for the
# fake server are encoded by the production codec through open_frame_dump.
build/open_frame_dump: tests/interop/open_frame_dump.c build/interop_codec.o | build
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $< build/interop_codec.o -lc -o $@

.PHONY: interop-open-e2e-test interop-open-e2e-slow interop-open-command-test interop-open-command-coverage interop-open-request-test interop-open-request-abi-test interop-open-request-sanitizers interop-open-request-coverage interop-activation-store-test interop-activation-store-abi-test interop-activation-store-sanitizers interop-activation-store-coverage

build/interop_open_request.o: src/interop/open_request.zig src/interop/open_request.h src/interop/pe_header.zig src/interop/pe_header.h | build
	$(ZIG) build-obj $< -Iinclude -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/open_request_abi_test: tests/interop/open_request_abi.c src/interop/open_request.h src/common/arguments.c build/interop_open_request.o build/path_rules.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop $< src/common/arguments.c build/interop_open_request.o build/path_rules.o $(LDFLAGS) -o $@

interop-open-request-abi-test: build/open_request_abi_test
	./build/open_request_abi_test

# The Zig suite imports pe_header.zig, whose own test links the PE C fixture.
interop-open-request-test: interop-open-request-abi-test build/interop_pe_header_c_fixture.o
	$(ZIG) test src/interop/open_request.zig build/interop_pe_header_c_fixture.o -Iinclude -lc

build/open_request_abi_asan: tests/interop/open_request_abi.c src/interop/open_request.h src/common/arguments.c build/interop_open_request.o build/path_rules.o | build
	$(CC) $(CPPFLAGS) $(InteropOpenRequestSanitize) -Isrc/interop $< src/common/arguments.c build/interop_open_request.o build/path_rules.o -fsanitize=address,leak,undefined -o $@

# Zig code is not instrumented here; Zig ownership is proven by std.testing.allocator.
interop-open-request-sanitizers: build/open_request_abi_asan
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/open_request_abi_asan

# Line and taken-branch gate (>= 90 percent) through the shared coverage driver.
interop-open-request-coverage:
	python3 tests/av/coverage.py open_request

# Activation token store (#29e): pure Zig module with no imports beyond std, so
# the Zig suite links on its own and the C consumer needs only the Zig object.
build/interop_activation_store.o: src/interop/activation_store.zig src/interop/activation_store.h | build
	$(ZIG) build-obj $< -Iinclude -Isrc/interop -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=$@

build/activation_store_abi_test: tests/interop/activation_store_abi.c src/interop/activation_store.h build/interop_activation_store.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -Isrc/interop $< build/interop_activation_store.o $(LDFLAGS) -o $@

interop-activation-store-abi-test: build/activation_store_abi_test
	./build/activation_store_abi_test

interop-activation-store-test: interop-activation-store-abi-test
	$(ZIG) test src/interop/activation_store.zig -Iinclude -lc

build/activation_store_abi_asan: tests/interop/activation_store_abi.c src/interop/activation_store.h build/interop_activation_store.o | build
	$(CC) $(CPPFLAGS) $(InteropOpenRequestSanitize) -Isrc/interop $< build/interop_activation_store.o -fsanitize=address,leak,undefined -o $@

interop-activation-store-sanitizers: build/activation_store_abi_asan
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/activation_store_abi_asan

interop-activation-store-coverage:
	python3 tests/av/coverage.py activation_store

# Client orchestration: Zig suite linked with the C objects it calls and the PE fixture.
interop-open-command-test: build/interop_pe_header_c_fixture.o $(OpenCommandObjects)
	$(ZIG) test src/interop/open_command.zig $(OpenCommandObjects) build/interop_pe_header_c_fixture.o $(OpenCommandFlags) -lc

interop-open-command-coverage: $(OpenCommandObjects)
	python3 tests/av/coverage.py open_command

InteropLaneTests += interop-open-request-test interop-activation-store-test interop-open-command-test

# Session stop client (IMPL_DESC 3.12.5). The shared session.sock primitives are tested with it.
SessionStopCommandObjects = build/interop_codec.o
SessionStopCommandFlags = -Iinclude -Isrc/daemon -Isrc/common

interop-session-stop-test: $(SessionStopCommandObjects)
	$(ZIG) test src/interop/session_stop_command.zig $(SessionStopCommandObjects) $(SessionStopCommandFlags) -lc

interop-session-stop-coverage: $(SessionStopCommandObjects)
	python3 tests/av/coverage.py session_stop_command

interop-session-shared-coverage: $(SessionStopCommandObjects)
	python3 tests/av/coverage.py local_session_client

# Real session handler (harness) and the fake server; the abnormal cases use the fake server.
.PHONY: interop-session-stop-e2e-test
interop-session-stop-e2e-test: build/waddle build/open_frame_dump build/interop_session_local_harness
	WADDLE_TEST_SESSION_HARNESS=build/interop_session_local_harness python3 tests/integration/session_stop_e2e.py

InteropLaneTests += interop-session-stop-test interop-session-stop-e2e-test
InteropLaneCoverage += interop-session-stop-coverage interop-session-shared-coverage
InteropLaneSanitizers += interop-open-request-sanitizers interop-activation-store-sanitizers
InteropLaneCoverage += interop-open-request-coverage interop-activation-store-coverage interop-open-command-coverage

# Hardware-free; the cold-start and 120 s cases use temporary directories and fake helpers.
interop-open-e2e-test: build/waddle build/open_frame_dump
	python3 tests/integration/open_e2e.py

# Same suite plus the real 120 s cold-start deadline (about four minutes).
interop-open-e2e-slow: build/waddle build/open_frame_dump
	WADDLE_OPEN_E2E_SLOW=1 python3 tests/integration/open_e2e.py

InteropLaneTests += interop-open-e2e-test

# Session command socket (TODO #7 socket side, TODO #29 29c/29f): the handler is tested with its
# source included; header decoding and payload validation are the production codec. Contract:
# impl/seamless-interop/claude_45_percent/session_local_handler_contract.md.
SessionLocalCFlags = -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -UNDEBUG -O1 -g -Iinclude -Isrc/interop -Isrc/daemon -Isrc/common
SessionLocalSanitizeFlags = -fsanitize=address,leak,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# Fault injection for the handler suite: each wrapped libc call forwards unless the test arms it.
SessionLocalWrapFlags = -Wl,--wrap=socket -Wl,--wrap=bind -Wl,--wrap=listen -Wl,--wrap=mkdir -Wl,--wrap=lstat -Wl,--wrap=unlink -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=accept4 -Wl,--wrap=getsockopt -Wl,--wrap=epoll_create1 -Wl,--wrap=epoll_ctl -Wl,--wrap=epoll_wait

build/session_local_handler_test: tests/interop/session_local_handler_test.c src/interop/session_local_handler.c src/interop/session_local_handler.h build/interop_codec.o build/interop_activation_store.o | build
	$(CC) $(SessionLocalCFlags) $< build/interop_codec.o build/interop_activation_store.o $(SessionLocalWrapFlags) $(LDFLAGS) -o $@

.PHONY: interop-session-local-test
interop-session-local-test: build/session_local_handler_test
	./build/session_local_handler_test

InteropLaneTests += interop-session-local-test

# Harness (TODO #7 end to end): the production control runtime with this handler, a fake daemon, and a
# fake guest (tests/interop/session_local_harness.c). Its vsock socket is a socketpair through --wrap.
build/session_local_handler.o: src/interop/session_local_handler.c src/interop/session_local_handler.h | build
	$(CC) $(SessionLocalCFlags) -c $< -o $@

# The harness is the real supervised command (waddle_session_main, compiled with WaddleSessionNoMain)
# plus a fake daemon on the real socket path and a fake guest behind the VSOCK wrappers.
SessionHarnessSources = src/interop/session_main.c src/interop/session_app.c src/interop/session_supervisor.c src/interop/session_handlers_builtin.c src/interop/session_local_handler.c
build/interop_session_local_harness: tests/interop/session_local_harness.c $(SessionHarnessSources) build/interop_activation_store.o $(InteropHostControlSources) $(InteropHostControlObjects) | build
	$(CC) $(SessionLocalCFlags) -DWaddleSessionNoMain '-DWaddleSessionAgentVersion="1.2.3"' -Isubmodules/looking_glass/module -pthread tests/interop/session_local_harness.c $(SessionHarnessSources) build/interop_activation_store.o $(InteropHostControlSources) $(InteropHostControlObjects) -Wl,--wrap=socket -Wl,--wrap=connect $(LDFLAGS) -o $@

# The built `waddle open` against the real session command handler (harness mode of open_e2e.py).
# Cases that need a deliberately misbehaving server stay in interop-open-e2e-test (fake server).
.PHONY: interop-session-local-e2e-test
interop-session-local-e2e-test: build/waddle build/open_frame_dump build/interop_session_local_harness
	WADDLE_TEST_SESSION_HARNESS=build/interop_session_local_harness python3 tests/integration/open_e2e.py

InteropLaneTests += interop-session-local-e2e-test

# The handler suite under AddressSanitizer, LeakSanitizer, and UBSan (every allocation is caller-owned
# storage, so leak detection covers the descriptors and the socket file the tests create).
build/session_local_handler_asan: tests/interop/session_local_handler_test.c src/interop/session_local_handler.c src/interop/session_local_handler.h build/interop_codec.o build/interop_activation_store.o | build
	$(CC) $(SessionLocalCFlags) $(SessionLocalSanitizeFlags) $< build/interop_codec.o build/interop_activation_store.o $(SessionLocalWrapFlags) $(LDFLAGS) -o $@

.PHONY: interop-session-local-sanitizers interop-session-local-coverage
interop-session-local-sanitizers: build/session_local_handler_asan
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/session_local_handler_asan

interop-session-local-coverage:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/interop/session_local_coverage.py

InteropLaneSanitizers += interop-session-local-sanitizers
InteropLaneCoverage += interop-session-local-coverage
