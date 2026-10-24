# Host buffer registry review

## Result

I found no concrete internal transition, stale-ticket reuse, ticket-exhaustion,
quarantine-clearing, or error-output-preservation defect in
`src/interop/host_buffer_registry.zig` against frozen §8.19. Admission validates
before writing; issued tickets monotonically advance through `UINT64_MAX` and
then remain exhausted; retiring a record does not rewind the cursor; callback
methods require the exact current epoch/ticket and phase; quarantine retains
records while preventing new admission; error paths do not mutate the registry.
`begin_import`'s whole-value stack copy is absent in the current source; no
correctness issue from it applies. I did not build or run tests, as requested.

## Native-boundary precondition to preserve

Tickets are checked as `(registry.session_epoch, ticket)`, while each fresh
registry starts at ticket 1. The API cannot distinguish two registry values
initialized with the same epoch. If an adapter reinitializes/replaces a live
registry with the same epoch while an old callback can still run, then after a
new admission reissues ticket 1, that stale callback can call (for example)
`buffer_released(epoch, 1)` and advance the new record. This violates §8.19's
explicit preconditions (“caller cannot reinit a live registry as cleanup” and
ticket scoped to the exact registry instance/epoch), so it is not an internal
bug under compliant use. The native owner must enforce one canonical mutable
registry per unique authorized mapping epoch, keep callback contexts bound to
that owner, and quiesce/destroy every callback before retiring or replacing the
registry. Do not use an epoch reused across registry instances to recover from
transport loss.

The registry's `mark_attached` intentionally trusts the adapter's documented
serialized callback/listener/attach precondition; this module cannot verify a
Wayland event. `complete_never_attached` similarly trusts the explicit
callbacks-quiesced assertion. Those are boundary obligations, not proof
produced by the pure registry. Its zero-record count is not a host-drained or
safe-unmap proof, as the contract and API docs correctly state.
