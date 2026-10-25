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
