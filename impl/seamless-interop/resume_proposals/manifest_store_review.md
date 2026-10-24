# Unaccepted manifest store draft

The adjacent manifest_store_draft.zig is a recoverable implementation proposal,
not a production module. It received no journal/artifact progress credit and is
not wired into builds or commands. Preserve the frozen schema1 public manifest;
its private journal format can still change before any consumer ships.

To resume isolated tests, copy this exact draft to src/interop/manifest_store.zig
(the relative manifest.zig import requires that package location), then run:
`zig test src/interop/manifest_store.zig -lc -O Debug -femit-bin=/tmp/manifest_store_tests`
with task-owned /tmp caches. Do not overwrite an existing implementation.
The archived exact source passed33 tests including imported codec tests under
std.testing.allocator failure sweeps; saved tests.log is evidence only of that.

Accepted review corrections inside this draft: full record/action reconciliation,
unchanged-record disk checks, forced-edit backups independent of expected-current
hash, bounded enumeration with failed-append ownership cleanup, explicit recovery
error after rollback failure, persisted root/parent/state directory identities,
held-state-fd publication/removal/temp cleanup and swap regressions. Tests include
Remove apply/rollback, both menu roots, incomplete/corrupt retained backups and
committed-state conflicts. These corrections do not qualify the whole engine.

Required before promotion:

- Close the leaf race: the current artifact helper hashes a leaf and later uses
  ordinary rename/unlink. Another editor can replace/create the leaf between
  those calls, causing unknown bytes to be destroyed. A held parent fd prevents
  directory redirection but does not compare-and-swap the leaf. Retain the
  displaced entry with atomic exchange/quarantine and no-replace operations;
  journal every recoverable namespace state and fail closed on unknown data.
- Cleanup must verify retained sibling identity/content before unlinking it;
  deterministic names alone do not establish ownership. Unsupported atomic
  rename flags must never fall back to unsafe check-then-overwrite behavior.
- Review commit-marker/error and cleanup ordering again after redesign. Every
  durable boundary needs crash and actual syscall-failure coverage; the current
  durable-boundary hook is not an actual EIO/ENOSPC injector.
- Meet unchanged90% production line/branch coverage for the complete engine.
  An earlier27-test snapshot measured90.36% lines but58.47% branches. That is a
  failing result, not acceptance of the later33-test source. Fork children call
  _exit and bypass the recorder destructor; improve measurement without changing
  crash semantics or deleting sites before using their execution as evidence.
- Implement caller XDG/desktop root resolution, commands and all producers, plus
  native recovery/removal acceptance. This explicit-roots draft does none of
  those and does not support system/guest/MIME policy mutations.

The native rename contract is documented in the Linux man-pages project:
https://man7.org/linux/man-pages/man2/rename.2.html . RENAME_EXCHANGE preserves
both displaced names; RENAME_NOREPLACE refuses an existing destination. Neither
is a leaf CAS or prevents continued writes through an already-open fd. The
[separate proposal](manifest_leaf_recovery_proposal.md) records a possible bounded
redesign and its limits; it is not approved implementation or completion credit.
