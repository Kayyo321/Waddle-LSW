# Linux socket-owner gate review

Read-only review of the frozen gate/build files. No actionable gate or fixture
defect found.

Reviewed SHA-256 hashes:

| File | SHA-256 |
|---|---|
| `scripts/interop.mk` | `164b459510fab4cb17b3dc6cd8cf967f2b731ca45456fe4c379d8a1288cb9046` |
| `tests/interop/linux_socket_owner_abi.py` | `8ec4c092384bc752a0568897ad6335c4f5045f686c12d61c54ed38d748ad0f17` |
| `tests/interop/linux_socket_owner_coverage.py` | `740cc9bae3493e2a4ce2df17719e70b158b9fde0a40fb6e0b93aa850d3a50443` |
| `src/interop/linux_socket_owner.c` | `46a66725ca8d08870161f6290c761df1defe6b0c4f3b5de420cd61d3e066ac54` |
| `include/waddle/interop_linux_socket.h` | `ac4b7b06087bd83bec26198726a34a6f04277d8c4907c7f96baba500d49bfc29` |
| `tests/interop/linux_socket_owner.c` | `c087747eeb23ceee24e9b5cfc1cf850558a2894bcadfbe1636a6997cd596615a` |
| `tests/interop/linux_socket_owner_guards.c` | `7f1ed8b3e2cd16f8e5408f55345455efe33eef34c28c2507d860193dc25d6293` |
| `tests/interop/linux_socket_owner_private.c` | `21bf0250d77c385c8d4dccd11f6b92087e22d8b27531b258f317437e5193e8e9` |

The ordinary-object rule compiles `src/interop/linux_socket_owner.c` without
including test seams; `linux_socket_owner_abi.py` checks both the header's
declared API set and the object's exact eight defined external symbols. The
runtime fixture instead includes the exact source once through the private
test translation unit, with static helpers exposed only inside that test TU.
The public guards and socketpair fixture link against that one test definition,
not a second production object. The coverage driver counts only the resolved
production source path, records input hashes/commands/wrapper names in a unique
attempt directory, and rechecks all recorded inputs after execution; it fails
below the unchanged 90% line or branch threshold.

The Make target dependencies include all three fixture files, the production
source/header and protocol headers, plus the existing peer-ABI and codec objects.
The coverage runner's direct include compile uses the same production source and
the same peer/codec objects; wrappers are derived from the hashed C fixture so
the fault seams cannot silently drift from the build command. The sanitizer
target forces rebuilds with `address,leak,undefined`, leak detection and abort
on sanitizer error; it runs the real socketpair fixture and does not disable
LSan. Linux-only POSIX socket tests are not pulled into the Windows cross-build
targets. The fixture uses AF_UNIX socketpairs and has explicit descriptor/owner
cleanup; it does not invoke VSOCK, devices, a VM, or native mapping operations.

Limits: this was source inspection only. I did not execute the Make targets,
coverage driver, sanitizer run, or Windows CI. Those remain lead-owned gates.
