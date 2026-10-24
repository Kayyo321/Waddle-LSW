# Contextual control-peer C error mapping proposal

## Finding and rule

The ABI currently uses a global `map_peer_error`/`map_result_error` for every exported operation. Its fallback is appropriately `InteropPeerInternalInvariant`, but specific tags such as `WrongPhase`, `WrongConnectionToken`, and `TimeWentBackwards` are globally mapped to recoverable C results. Several of those tags can also emerge from a nested helper after the wrapper/core has already validated the caller's token, time, role, and phase. Returning a retryable status for that internal disagreement violates §8.21's requirement that unexpected composed-state failures be `InternalInvariant`.

Use per-operation allowlists at each exported boundary. Prefer explicit wrapper preflight for the user-controlled conditions (token, time, applicable role/phase, guest zero epoch, `succeeded` range), then make catch mapping operation-specific. A helper's error tag is user-recoverable only when that operation can legitimately produce it after its public preconditions; every other tag maps to `InteropPeerInternalInvariant`. Keep all raw pointer/config/alias/range failures as `InvalidArgument` and preserve existing C results for valid caller mistakes.

## Per-operation public result policy

| Export | Recoverable results after raw pointer guards | Any other core/helper error |
|---|---|---|
| `create` | `InvalidArgument` for malformed config/token/version/capabilities; `OutOfMemory` for allocation failure | `InternalInvariant` (e.g. unexpected fixed reader/queue initialization failure) |
| `destroy` | `InvalidArgument` for invalid/overlapping owner slot; `Success` for live or NULL slot | No fallible peer helper is expected; any future helper failure is `InternalInvariant` |
| `get_state` | `InvalidArgument` for zero token/bad output; `WrongToken` | No peer helper; unexpected failure is `InternalInvariant` |
| `feed` | `InvalidArgument` for zero token, malformed ranges, peer/input alias; `WrongToken`; `WrongPhase` only when the live peer is already Draining/Closed; `Closed` only if the allocated peer is explicitly not live | Reader sticky failure/input-closed and unexpected state errors are `InternalInvariant` |
| `pending_bytes` | `InvalidArgument` for zero token/bad output; `WrongToken` | No peer helper; unexpected failure is `InternalInvariant` |
| `next_event` | `InvalidArgument` for zero token/bad ranges/alias; `WrongToken`; `TimeWentBackwards`; `WrongPhase` only for the core's explicit Closed-phase preflight; `Closed` only for a non-live allocated owner; `OutputTooSmall` for a valid copy-out that does not fit | Helper `WrongPhase` after phase preflight, helper token/time disagreement, `FrameNotReady` escaping the core's None conversion, reader failure, or other unexpected errors are `InternalInvariant` |
| `mapping_result` | `InvalidArgument` for raw args, zero token, succeeded outside {0,1}, Guest wrong layout, Guest zero epoch; `WrongToken`; `TimeWentBackwards`; `WrongPhase` only for the peer's explicit role/phase preflight; `Closed` for non-live owner; `MappingFailed` only for Guest `succeeded=0` | Host mismatch/false validation is a successful Draining event; helper `WrongPhase` after wrapper preflight, unexpected `InvalidSessionEpoch` on Host, ID exhaustion/internal state, or unrelated error tags are `InternalInvariant` |
| `tx_bytes` / `copy_tx` | `InvalidArgument` for raw args/aliases; `WrongToken`; `WrongPhase` only when the core is Closed; `Closed` for a non-live owner; `OutputTooSmall` for `copy_tx` | A queue-closed/inconsistent queue while owner is live, or any helper-only error is `InternalInvariant` |
| `ack_tx` | `InvalidArgument` for zero token/bad output; `WrongToken`; `TimeWentBackwards`; `WrongPhase` only for core Closed preflight; `Closed` for non-live owner; `NoQueuedFrame`; `InvalidWriteCount` | Helper `WrongPhase`/`WrongRequestId`, liveness or ID errors after successful outer preflight, metadata mismatch, and queue-consumption failure are `InternalInvariant` |
| `tick` | `InvalidArgument` for zero token/bad output; `WrongToken`; `TimeWentBackwards`; `Closed` for non-live owner | There is no public WrongPhase result: core terminal phases return successful terminal events. Helper `WrongPhase`, `SessionClosed`, role/token/time disagreement, or unexpected request-ID error is `InternalInvariant`; exhausted IDs normally become a successful Draining event |
| `begin_drain` | `InvalidArgument` for zero token/bad output; `WrongToken`; `TimeWentBackwards`; `Closed` for non-live owner | There is no public WrongPhase result: repeat/Closed calls return events. Helper `WrongPhase` or token/time disagreement is `InternalInvariant` |
| `transport_lost` | `InvalidArgument` for zero token/bad output; `WrongToken`; `Closed` for non-live owner | No helper/clock error is expected; any unexpected error is `InternalInvariant` |

The `Closed` statuses above retain the stable C enum mapping for `PeerClosed`; normal callers cannot call through a freed opaque pointer. Core `WrongPhase` for Closed is a distinct, valid operation-specific result where stated. Do not map `NoPendingFrame` globally to `WrongPhase`: the core converts an empty reader into a None event, so such an escaped tag is an invariant failure.

## Implementation approach

A private operation enum plus `map_error_for_operation(operation, err)` is sufficient, but operation-specific catch blocks are clearer where the allowed set differs. Do not use one global `map_result_error` at export sites. For each stateful method, either:

1. Preflight wrapper-owned caller mistakes before invoking the core and return their C status directly; then map only operation-specific post-preflight errors, or
2. Preserve the pre-call `role`/`connection_phase`/`live` snapshot and use it to distinguish core preflight `WrongPhase` from a helper-originated `WrongPhase`.

Preflight must be read-only and follow the existing order: pointer/range/alias checks first; then token and monotonic time; then API-specific caller arguments/role/phase; only then invoke the core. `next_event` must retain the core's pending-frame-aware OutputTooSmall preflight so no reader, time, or liveness state is consumed to size output.

## Focused tests

- Keep C ABI tests for every documented public status: invalid config/argument, OOM, stale token, backward time, WrongPhase for the specific Closed/Draining operation, short output, invalid write count, empty queue, Guest MappingFailed, and normal successful Draining/Closed events.
- Add private test-only scalar mismatch cases that make the core preflight valid but a helper phase/token/time inconsistent, and assert `InternalInvariant` plus unchanged C outputs. Never alter owning pointers, queue lengths, or allocation metadata.
- Ensure Host validation mismatch remains `Success` + Draining, while Guest false initialization remains `MappingFailed` and unchanged.
- Assert that `tick`, `begin_drain`, and `transport_lost` do not expose `WrongPhase` for normal terminal callbacks.
- Retain ASan/LSan/UBSan, allocator-failure, and existing coverage requirements; do not add coverage exclusions or production noinline branches.

This is an error-classification refinement only. It adds no protocol behavior, feature admission, native lifecycle, or detach proof.
