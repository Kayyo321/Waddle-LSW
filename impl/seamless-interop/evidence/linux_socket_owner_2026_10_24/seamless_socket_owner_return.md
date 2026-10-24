# Linux socket-owner implementation return

Source and test ownership has been released to root. No further repository edits or test runs were made after release.

## Owned files and final hashes

- `src/interop/linux_socket_owner.c` — SHA-256 `46a66725ca8d08870161f6290c761df1defe6b0c4f3b5de420cd61d3e066ac54`
- `tests/interop/linux_socket_owner.c` — SHA-256 `c087747eeb23ceee24e9b5cfc1cf850558a2894bcadfbe1636a6997cd596615a`

## Verification

Exact command, from `/home/dev/Waddle-LSW`:

```sh
python3 tests/interop/linux_socket_owner_coverage.py
```

Exit status: `0`. The fixture reported `linux_socket_owner: all tests passed`. Production coverage was 97.33% lines (728/748) and 90.08% branches (545/605), meeting the unchanged gates. Evidence directory:

`/home/dev/Waddle-LSW/build/coverage/interop/linux_socket_owner/run-20261024T210431-b806ed2a`

The helper compiles strict C objects, links the exact production source through the private test TU with the real peer/codec objects, includes public guard cases, runs the executable, and verifies all input hashes stayed stable during the attempt.

## Correctness changes and focused regressions

- Peer phase is validated before an event is copied to caller-visible work or mapping-callback state is changed. An invalid-phase FeatureFrame from `next_event` now fails closed without publishing an actionable event. Invalid-phase ACK handling retains the byte count already accepted by `send`.
- The fixture covers invalid phases from feed, `next_event`, ACK, tick, state query, mapping result, and drain entry; transport-lost notification failures; timeout abort ordering against simultaneous `POLLERR`; carry/pending-frame step boundaries; MappingWait carry saturation and hangup; and repeated Draining event behavior.
- Added a bounded 128-iteration lifecycle stress using fresh real socketpairs for fragmented negotiation, Ping/Pong and feature dispatch, followed by teardown.
- Added a `would_block()` helper to express the platform errno rule. Where `EWOULDBLOCK == EAGAIN`, it checks that value once. Where the values differ, it checks either value. This preserves behavior on both cases while removing an impossible second short-circuit arc on this Linux build; no coverage threshold or source exclusion changed.
- Fixed constants in the owned test fixture to repository PascalCase naming and guarded `_GNU_SOURCE` definitions against command-line redefinition.

## Limits

The fault wrapper cases test deterministic syscall/peer-ABI failures and cleanup reporting; they are not evidence of kernel/device failures. The real transport checks use local AF_UNIX socketpairs only. No VM, virtiofsd backend, native runner, or end-to-end application acceptance was performed in this lane; root owns those integration and sanitizer gates.
