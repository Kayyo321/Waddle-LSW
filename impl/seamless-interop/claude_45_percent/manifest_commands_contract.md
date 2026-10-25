# Manifest command engine contract (TODO #33a, part 1)

## Scope

This contract covers three additional operations on the rooted store in
`src/interop/manifest_store.zig`, which the future `waddle integration remove`,
`repair`, and `migrate` commands will call:

- `store_t.status(allocator)`: read-only classification of every record.
- `store_t.remove(request)`: journaled removal of one recorded artifact.
- `store_t.migrate_record(request)`: journaled rewrite of manifest-level
  metadata for one record.

Out of scope: `repair` (recreation and `--force` replacement), the CLI
(`src/cli/integration_commands.zig`), producers beyond the desktop-entry and
menu kinds, guest and system kinds, and native qualification. These remain
open under TODO #33b. The store owns the exclusive `manifest.lock`, root
descriptors, no-follow traversal, and every filesystem mutation. Commands never
write manifest or artifact paths directly.

## Public types

```zig
pub const record_state_t = enum { Ok, Missing, Modified, Foreign };
pub const record_status_t = struct { record_id: []u8, state: record_state_t };
pub const status_list_t = struct { allocator: std.mem.Allocator, items: []record_status_t; pub fn deinit(*status_list_t) void };

pub const remove_request_t = struct { record_id: []const u8, candidate_manifest: []const u8 };
pub const remove_outcome_t = enum { Removed, KeptModified, AlreadyMissing, KeptForeign };
pub const prior_copy_t = union(enum) { None, Text: bounded_text_t, U32Value: u32 };
pub const bounded_text_t = struct { bytes: [manifest.MaxManifestStringBytes]u8, len: usize };
pub const remove_receipt_t = struct { outcome: remove_outcome_t, record_found: bool, prior: prior_copy_t };

pub const migrate_request_t = struct { record_id: []const u8, candidate_manifest: []const u8 };
pub const migrate_receipt_t = struct { changed: bool };
```

Every receipt and list is by value or caller-owned. No returned value borrows
parsed manifest memory, and `prior_copy_t.Text` stores its bytes inline.

## Record classification

`classify_target` and `resolve_for_record` (shared by `status` and `remove`)
apply these rules, evaluated in order:

| Condition | State | Effect |
|---|---|---|
| Record path or kind outside authorized roots (`InvalidPath`, `InvalidScope`, `UnsupportedArtifactKind`) | Foreign | none |
| Parent or leaf missing (`FileNotFound`, `ENOENT`) | Missing | none |
| Symlink, hardlink (`nlink != 1`), directory, foreign owner, setuid/setgid/sticky bits, or non-directory parent (`AccessDenied`) | Foreign | none |
| Owned regular file with mode other than 0644 or 0755 | Foreign | none |
| Owned regular file larger than `MaxArtifactBytes` (`FileTooLarge`) | Modified | none |
| Owned regular file, allowed mode, SHA-256 differs from record | Modified | none |
| Owned regular file, allowed mode, SHA-256 equals record | Ok | eligible for removal |

Other I/O and allocation errors propagate; they never become a state.

## `status`

- Runs the same recovery as `inspect_record` first. With no pending journal it
  performs no filesystem writes; the manifest and artifacts are only read.
- Returns one entry per record in manifest order; a missing manifest yields an
  empty list. Newer schemas, malformed JSON, duplicate or overlapping paths
  fail before classification with `UnsupportedSchema`, `InvalidManifest`, or
  `ManifestMismatch`.
- The result is a snapshot under the held lock, not a reservation.

## `remove`

Request: `record_id` and `candidate_manifest`, the complete current manifest
with exactly that record deleted (its root `updated` may differ). The caller
builds the candidate from a manifest it has read; the store parses it on every
call and verifies the record delta only when a removal would proceed.

| Outcome | Condition | Manifest | Artifact | `record_found` |
|---|---|---|---|---|
| `Removed` | state Ok, candidate equals current minus the record | record deleted | unlinked | true |
| `KeptModified` | state Modified | unchanged | unchanged | true |
| `KeptForeign` | state Foreign | unchanged | unchanged | true |
| `AlreadyMissing` | state Missing (record kept) | unchanged | unchanged | true |
| `AlreadyMissing` | record or manifest absent | unchanged | unchanged | false |

Decision recorded here: IMPL_DESC 3.12.4 says remove "reports and keeps" items
it does not delete. A record whose artifact is missing is therefore kept and
reported, not dropped. `repair` (not in this milestone) is the path that
recreates it. Repeated calls after a removal return `AlreadyMissing` with
`record_found = false`.

`prior` is returned for every outcome in which the record existed. The store
never restores prior values; restoration belongs to the producer.

Errors before the PREPARED journal leave the manifest and artifact unchanged:
`Closed`, `InvalidArgument` (empty or oversized candidate, empty or invalid
record id), `InvalidManifest`, `UnsupportedSchema`, `ManifestMismatch` (candidate
keeps the record, drops another record, changes a remaining record, or
overlaps paths), `ArtifactConflict`, `RecoveryRequired`, `AccessDenied`,
`OutOfMemory`, and filesystem errors.

## `migrate_record`

Request: `record_id` and `candidate_manifest`. Requirements:

- The named record must exist in the current manifest (`FileNotFound` otherwise,
  including when no manifest exists).
- The candidate must keep every record equal by schema fields (`id`, `kind`,
  `path`, `sha256`, `device`, `app_key`, `created_by`, `prior`, `mime`) with the
  same count. Only the root-level `waddle_version` and `updated` may change.
  Any other difference fails with `ManifestMismatch`.
- A newer schema is refused by the codec with `UnsupportedSchema` before any
  mutation.
- The artifact is never opened for writing, renamed, or unlinked, and its state
  is not a precondition. Its inode is unchanged by a successful migration.

Decision recorded here: schema 1 has no per-record migration or migrated field.
Introducing one would change the frozen schema (IMPL_DESC 3.12.2), so none is
added. The rewrite is the root metadata the codec exposes. A candidate
byte-identical to the current manifest returns `changed = false` with no
journal and no write.

## Journal version 3

Used by `remove` and `migrate_record`. Stored as `transactions/<txid>/journal.json`
(mode 0600) with the same directory, cleanup, and bound rules as versions 1
and 2. Strict parsing rejects unknown and duplicate fields.

| Field | Remove | Migrate |
|---|---|---|
| `version` | 3 | 3 |
| `transaction_id` | equals directory name | equals directory name |
| `phase` | `prepared` or `committed` | `prepared` or `committed` |
| `operation` | `remove` | `migrate` |
| `record_id` | required, valid | required, valid |
| `root`, `relative_path` | required; root must be configured | must be null |
| `old_artifact_hash`, `old_artifact_mode`, `old_artifact_device`, `old_artifact_inode` | required; mode 0644 or 0755; device and inode nonzero | must be null, mode 0 |
| `old_manifest_hash`, `old_manifest_device`, `old_manifest_inode` | required | required |
| `new_manifest_hash`, `new_manifest_device`, `new_manifest_inode` | required; hash differs from old | required; hash differs from old |

Recovery dispatches on the top-level `version` before the version 1/2 parser,
so those code paths are unchanged.

## Transaction phases

Both operations use one engine (`run_manifest_transaction`):

1. Create `transactions/<txid>`; write `new-manifest` and `old-manifest` (0600).
2. Stage the candidate at `.manifest-<txid>.tmp` in the state directory.
3. Write and fsync the PREPARED journal.
4. PREPARED apply: for removal, the artifact must still have its recorded
   identity (`ArtifactConflict` otherwise). The candidate is published with
   `RENAME_EXCHANGE`, which leaves the old manifest at the temp name. The
   artifact is not touched.
5. Durably switch the journal to `committed`.
6. COMMITTED apply: verify the live manifest is the candidate; verify the
   displaced old manifest at the temp name (absent is accepted after an earlier
   cleanup). For removal, unlink the artifact only if its identity is still the
   recorded one; an already absent artifact is a completed unlink. Fsync the
   journal, transaction directory, state directory, and root, and the artifact's
   parent for removal.
7. Delete the displaced manifest, then the transaction tree.

The artifact is unlinked only after the candidate manifest is durable. A crash
at any point therefore leaves either the old state (PREPARED rollback) or the
new state (COMMITTED roll-forward).

## Recovery rules

`recover` (also run at the start of every operation and by `init`) validates
each version-3 journal before any host mutation:

- Header: the fields above, with `transaction_id` equal to the directory name.
- Staged manifests: their SHA-256 values match the journal, the old manifest
  parses and passes global path checks, and the candidate satisfies the
  operation rule (remove: exactly the record deleted, others equal; migrate:
  every record equal). For removal, the record resolves through the journal's
  root and relative path, and its digest equals the journal's artifact hash.

Any failure in this validation is `RecoveryRequired` and leaves all evidence in
place.

Then by phase:

- PREPARED: removal requires the artifact identity (`ArtifactConflict` if
  absent or changed). If the candidate is live, the displaced old manifest must
  be at the temp name with its exact identity (`RecoveryRequired` if absent,
  `ArtifactConflict` if different) and is exchanged back. If the old manifest is
  live, nothing is published. Any other live manifest is `ArtifactConflict`.
  Finally the staged candidate temp is removed.
- COMMITTED: the live manifest must be the candidate (`RecoveryRequired`
  otherwise). The displaced temp is verified before anything is unlinked. A
  removal retires the artifact only if its identity matches or it is absent.
  Then the transaction is cleaned up.

A journal-less transaction directory still fails closed (existing policy). A
second `recover` after success is a no-op.

## Threading and ownership

Owner-thread serialized, as for the rest of `store_t`. Requests are borrowed
for the call. Results are caller-owned or by-value; `status_list_t.deinit` is
idempotent.

## Errors

Common to all three operations: `Closed`, `InvalidArgument`, `InvalidManifest`,
`UnsupportedSchema`, `ManifestMismatch`, `ArtifactConflict`,
`RecoveryRequired`, `AccessDenied`, `OutOfMemory`, and bounded filesystem
errors (`IoError`, `NoSpaceLeft`, `Busy`, and others). Fault-injected `fstat`
failures in the existing `read_manifest` and `open_private_dir` paths surface
as `AccessDenied`; this predates this contract and is unchanged.

## Verification (this commit set)

Module tests (`make interop-manifest-store-test`, `zig test src/interop/manifest_store.zig -lc`):
83 in-file tests in `src/interop/manifest_store.zig` (the run also includes the
12 codec tests of the imported `manifest.zig`, 95 in total), including:

- every classification class and manifest-level rejection for `status`;
- every outcome and receipt for `remove`, including text and numeric priors;
- candidate mismatch, malformed, oversized, and overlapping inputs for both
  operations, each rejected before mutation;
- checkpoint crash injection at every reachable boundary for remove and
  migrate, with convergence and a second `recover` as a no-op;
- selected POSIX I/O failures in forward and recovery paths, retried to convergence;
- allocation-failure sweeps for `status`, `remove`, `migrate_record`, and remove
  recovery, asserting zero leaked bytes with `std.testing.FailingAllocator`;
- journal field mutation for version 3 (22 cases), staged manifest and displaced
  manifest tampering, artifact tampering, root-kind mismatch, and digest-consistent
  but semantically invalid evidence.

Public API suite (`make interop-manifest-commands-test`, lane target in
`interop-test`): status, migrate, and remove through exported declarations only.

Whole-module coverage (`python3 tests/av/coverage.py manifest_store`, gate
>= 90% line and branch): **91.06% branch (1925/2114), 99.72% line (1774/1779)**
after the migrate commit. The gate counts the whole file because the file has
no `// Test-only fixtures.` marker; test helper functions are therefore counted.
Test helpers avoid assertions and error-only branches (fixtures use `catch @panic`
and return values; assertions live in test bodies).

Toolchain note: the instrumented coverage build compiles the emitted IR at
clang's default -O0. In `rollback_prepared_v3`, LLVM placed a 16-byte-aligned
digest slot at an 8-mod-16 offset, causing a general-protection fault. Moving the
manifest identity check into `classify_v3_manifest` removed that frame layout.
Any future change that re-introduces the fault should keep 32-byte digest copies
out of the same frame. No sanitizer run is claimed for the Zig suite.

## Not claimed

- Command integration (`src/cli/integration_commands.zig`): not implemented.
- `repair` and forced replacement of modified artifacts: not implemented here.
- Native (Linux desktop or Windows guest) qualification: not run.
- Guest and system record kinds: rejected by the existing resolver
  (`UnsupportedArtifactKind` maps to Foreign).
