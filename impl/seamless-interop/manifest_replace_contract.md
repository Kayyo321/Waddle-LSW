# Manifest replacement, backup, and restore extension contract

## Status and boundary

This contract describes the additive replacement extension in
`src/interop/manifest_store.zig`. It preserves the frozen schema-1 codec and
the absent-only `commit` behavior. The replacement journal and API milestone
is qualified by the source-bound evidence in
`manifest_replace_evidence_2026_10_25/qualification.json`. Command integration,
all artifact producers and comprehensive process-crash/native/full-removal
qualification remain open under TODO #33. The full acceptance matrix below
also specifies those later qualification requirements; it is not a claim that
every listed process-termination or errno combination has executed.

`store_t.replace`, `store_t.restore`, `store_t.inspect_record`, and
`store_t.list_backups` are additive methods on the accepted
store. The original `store_t.commit` still accepts one absent artifact and
returns `UnsupportedReplacement` for existing managed records and target
files. Root-resolution, journal, staging, recovery, and filesystem-operation
helpers remain private to the shared store.

The base store remains the only owner of the exclusive `manifest.lock`, root
descriptors, path traversal, transaction journal format, crash recovery, and
filesystem syscalls. The extension must use those same locked descriptors and
recovery path. It must not open a second independent store lock or create a
second journal namespace.

## Public extension surface

The methods share the store lock, rooted descriptor traversal, journal parser,
transaction namespace, and filesystem-operation seam with `commit`:

```zig
pub const artifact_identity_t = struct {
    device: u64,
    inode: u64,
    mode: u32,
    sha256: [32]u8,
};

pub const record_snapshot_t = struct {
    manifest_sha256: [32]u8,
    target: artifact_identity_t,
};

pub const replace_request_t = struct {
    record_id: []const u8,
    expected_manifest_sha256: [32]u8,
    expected_target: artifact_identity_t,
    artifact_bytes: []const u8,
    artifact_mode: u9,
    candidate_manifest: []const u8,
    backup_modified: bool,
};

pub const backup_ref_t = struct {
    transaction_id: [32]u8,
    record_id_sha256: [32]u8,
    sha256: [32]u8,
    artifact_mode: u32,
    device: u64,
    inode: u64,
};

pub const backup_list_t = struct {
    allocator: std.mem.Allocator,
    items: []backup_ref_t,
    pub fn deinit(self: *backup_list_t) void;
};

pub const replace_receipt_t = struct {
    target: artifact_identity_t,
    backup: ?backup_ref_t,
};

pub const restore_request_t = struct {
    record_id: []const u8,
    expected_manifest_sha256: [32]u8,
    expected_target: artifact_identity_t,
    backup: backup_ref_t,
    candidate_manifest: []const u8,
};

pub const restore_receipt_t = struct {
    target: artifact_identity_t,
};

pub fn replace(self: *store_t, request: replace_request_t) !replace_receipt_t;
pub fn restore(self: *store_t, request: restore_request_t) !restore_receipt_t;
pub fn inspect_record(self: *store_t, record_id: []const u8) !record_snapshot_t;
pub fn list_backups(self: *store_t, allocator: std.mem.Allocator, record_id: []const u8) !backup_list_t;
```

Request slices are borrowed only for the call. The store retains no
caller memory. Receipts contain bounded identity metadata and a stable backup
reference, never artifact bytes or arbitrary paths. Backup lookup is derived
only from the validated transaction id and digest of the validated record id;
callers cannot supply a path.

`inspect_record` returns the current manifest digest and exact opened target
device, inode, mode, and content digest by value while the store lock is held.
It is a snapshot rather than a reservation; replacement and restore recheck
both facts under the lock. `list_backups` makes one bounded pass over the
backup root, accepts at most `MaxBackups` directory entries, validates every
encountered entry, and returns references for the requested record digest
only. Every entry, including an interrupted staging entry, counts against the
256-entry scan bound. A staging entry is never treated as a complete backup;
its presence fails inventory closed with `RecoveryRequired` and is preserved.
Any malformed entry, I/O failure, or allocation failure returns an error and
no partial result. A successful `backup_list_t` owns its `items`
allocation through its recorded allocator and must be released with `deinit`.

Before creating a backup, the store validates the complete existing backup
inventory under the same lock. It refuses creation with `RecoveryRequired` if
the inventory is malformed, contains a pending entry, or already has 256
published entries. Thus no 257th backup can make later inventory unreadable.
The store preserves all existing entries on refusal. Pending entries are
included in the bound and fail closed; they are never silently removed or
treated as available quota.

`replace` is authorized only for one record already present in the old manifest
and with the same frozen record identity in the candidate manifest. The
candidate may change that record’s digest, and no other record or immutable
record field may change. The candidate schema, duplicate IDs, path policy,
record kind, root binding, and exact serialized bytes are validated by the
existing codec/store rules. The request’s manifest digest and target identity
must match the exact currently held objects. A target that is absent, a
symlink, non-regular, foreign-owned, multiply linked, or of an unsupported
mode is a conflict; `backup_modified` does not authorize any of those states.

If the current artifact hash matches the old manifest record, replacement may
proceed without a force backup. If the bytes differ from the old manifest
digest, replacement is refused unless `backup_modified` is true. When force
replacement is authorized, the exact pre-replacement bytes and permission bits
are preserved. Backup payload files are private mode `0600`, current
state-owner owned, single-link regular files below
`backups/<transaction-id>/<sha256(record-id)>`. Every transaction directory
also contains exactly one `metadata.bin`; unknown child names and extra files
are rejected. Creation writes and fsyncs both files in a private
`.pending-<transaction-id>` directory, then atomically publishes that directory
under the final transaction id with no-replace semantics and fsyncs
`backups/`. Pending directories are not valid backups. Any failure before
publication preserves the pending entry and fails inventory closed for
bounded operator inspection; cleanup never guesses which partially written
entries are safe to remove. Pending entries are never silently counted or
garbage-collected. A fully published backup is retained after every later
replacement failure, including a parent fsync error after the rename. The
directory is current-user owned, exact mode `0700`, and has the normal two-link
directory identity.
An exact no-op request whose candidate manifest bytes, artifact bytes, and
artifact mode all match the current state returns the current target identity
and no backup without creating a journal or changing the filesystem. If the
candidate manifest bytes are identical but the requested artifact mode differs,
the store returns `UnsupportedReplacement`; changing bytes while retaining
that manifest fails the normal manifest digest check. These rules prevent a
replacement journal from having identical old and new manifest images.

The store retains backups after successful replacement, rollback, and remove;
normal cleanup never deletes a backup. Restore is an explicit new transaction.
It validates the backup reference and exact
backup identity/hash/mode, then replaces only a target matching the caller’s
expected currently managed identity. A modified or foreign target is never
overwritten by restore.

## Filesystem and identity rules

All operations remain rooted in the store’s already-open, allowlisted root
directory descriptors. Every parent component is opened relative to its held
parent with no-follow directory semantics. Final artifacts and backups are
opened with no-follow semantics and must be regular, owned by the effective
user, and have exactly one hard link. Private state directories remain exact
mode `0700`; manifests, journal files, transaction payloads, and backups remain
exact mode `0600` unless the artifact’s validated mode is `0644` or `0755`.
Setuid, setgid, and sticky bits are rejected; masking them off is not enough to
make a privileged artifact look like an allowed `0644` or `0755` file.
Ownership and mode checks use `fstat` on opened descriptors, not only pathname
metadata. SHA-256, device, inode, and mode are captured from the opened object.

Linux does not provide a general compare-and-swap for a directory entry.
`renameat2(RENAME_EXCHANGE)` can preserve both directory entries atomically,
but it cannot compare the displaced inode against a prior `stat` result. The
implementation must not claim linearizable compare-and-swap against arbitrary
same-UID writers. It must use only operations with explicit no-clobber or
exchange semantics and retain the displaced entry until it has been checked.
If the filesystem does not support the required operation, fail closed without
falling back to check-then-rename. If the displaced object or any recovery
entry differs from the journal’s expected identity, leave all extant entries
and evidence in place and return a recovery/conflict error.

The store lock serializes Waddle writers only. An unrelated process can still
race pathnames or mutate an opened inode. This contract guarantees that the
store never intentionally deletes or overwrites a file whose identity it
cannot establish; it does not claim protection against an active hostile
same-UID writer holding writable descriptors. Directory and inode identity
must be rechecked at every available mutation boundary, and an ambiguous race
must retain evidence rather than report success.

## Journal and durable transaction protocol

The replacement extension uses the existing bounded journal parser and
recovery protocol; it does not introduce a separate journal namespace. New
replacement transactions use private journal version 2. Version 1 absent-only
journals remain recoverable unchanged. The reader rejects every unknown
version without mutating evidence. Version 2 records bind the transaction id,
phase (`prepared` or `committed`), record id, allowlisted root role, relative
path, old and new artifact hashes/modes/device/inodes, old and new manifest
hashes/device/inodes, backup reference and backup identity, and every
transaction-owned sibling temporary name. A present prior manifest is bound by
its exact device/inode as well as its digest/mode. No absolute path from journal input
is trusted. All JSON, strings, record counts, actions, and bytes remain bounded
by explicit constants no larger than the existing store limits unless separately
reviewed.

Before durable PREPARED:

1. Recover all older journals while holding the existing exclusive lock.
2. Parse and validate both manifests, exact record delta, path authorization,
   prior manifest digest, target identity, expected old artifact state, and
   backup policy. No target mutation is allowed during preflight.
   During journal-manifest validation, version-2 recovery reads the bounded
   old-artifact payload only long enough to hash it and compare that digest to
   the journal's recorded identity, then releases the payload before parsing
   the old manifest or reading backup bytes. The validated digest is retained
   as a fixed 32-byte value for the old-record digest check. The parser does
   not retain a second copy of the payload alongside manifest or backup
   buffers.
3. Create transaction and backup directories with no-follow checks and exact
   private ownership/mode. Stage exact old bytes for any retained backup, exact
   new artifact bytes, exact old manifest bytes, and exact candidate manifest
   bytes. Validate file type, owner, link count, mode, hash, size, device, and
   inode for every staged object.
4. Fsync every staged file and each containing directory. Write the complete
   bounded PREPARED journal via a transaction-specific temporary file; fsync
   it, atomically publish it without clobbering unrelated state, then fsync the
   transactions directory.

After PREPARED, every state transition is idempotent and recoverable:

1. Apply the artifact replacement with `renameat2(RENAME_EXCHANGE)`. This
   leaves the exact prior inode at the deterministic transaction sibling name
   and the staged new inode at the target name. Fsync the artifact parent.
   Open and verify the displaced file’s exact
   identity/hash/mode against the preflight state. On mismatch, retain both
   names and the journal; do not delete either object or proceed to manifest
   publication.
2. Publish the candidate manifest last. If `manifest.json` exists, exchange it
   with the verified same-directory staging file using
   `renameat2(RENAME_EXCHANGE)`. The displaced manifest stays at the staging
   name until its device, inode, mode, and digest match the preflight/journal
   identity. On any mismatch, preserve both names and all transaction evidence.
   If no manifest exists, use `RENAME_NOREPLACE`. Fsync the manifest directory.
3. Atomically change the journal phase to COMMITTED, fsync the new journal and
   transactions directory, then verify the new artifact and manifest hashes,
   modes, and inode identities.
4. Remove only verified transaction-owned staging files and journal-bound
   displaced entries. Retained backup payloads remain. Fsync changed parent
   directories before removing the transaction directory/journal and fsyncing
   the transactions directory.

PREPARED recovery validates the full journal, old/new manifests, backup, staged
payloads, target, manifest, and deterministic sibling entries before any
mutation. It rolls back in reverse publication order. An old target is
restored by exchanging the journal-bound displaced sibling inode back into the
target name; its device/inode/hash/mode must match the prepared identity. The
private byte backup is retained evidence and does not authorize overwriting an
unknown namespace state.
The exact prior manifest bytes are restored only when the current manifest
matches the transaction’s staged new identity; an originally absent manifest
may be removed only if the journal proves that this transaction created it.
Any unknown third state, mismatching inode, missing required backup, malformed
journal, unknown field, unsupported version, symlink, or non-regular object
returns `RecoveryRequired` and preserves evidence and all foreign objects.

COMMITTED recovery verifies that the new manifest and artifact have the exact
recorded identity, digest, and mode. It does not roll back committed data. It
may remove only verified staging and displaced temporary objects; it never
removes retained backup data. A mismatch fails closed and retains the journal,
backup, and any sibling entry. Recovery is idempotent and may be retried after
the cause of an I/O failure is corrected.

The replacement journal is version 2 and adds exact prior artifact device and
inode, the prior manifest device and inode when known, plus an optional
retained-backup digest/mode/device/inode. Older version-1 journals may omit
prior-manifest identity and are recovered using their existing digest/mode
checks. Version 1
absent-add journals keep their original null prior-artifact fields and are
validated and recovered by the same reader. The reader accepts exactly
versions 1 and 2; other versions are `RecoveryRequired` before mutation. A
version-2 journal without a backup is valid only when all backup fields are
absent/zero. A backup reference is validated from the transaction id and
record-id digest; backup bytes must match both its own journal digest and the
recorded prior artifact digest.

Before reading or mutating a backup, the store opens `backups/` and the
transaction-id child without following symlinks. The child must be a current
user-owned directory with exact mode `0700`; the backup payload must be a
current-user-owned single-link regular file with exact mode `0600`, recorded
device/inode, and expected SHA-256. The backup’s `artifact_mode` field records
the source artifact mode independently from the backup file’s actual `0600`
mode. `metadata.bin` is a fixed 124-byte binary version-1 record: `WDBK`
magic, little-endian version, 32-byte lowercase-hex transaction id, 32-byte
record-id SHA-256, 32-byte payload SHA-256, little-endian original mode,
device, and inode. All bytes are required and the version is exact; no path or
caller-selected name is encoded. Metadata is a current-user-owned single-link
regular file with exact mode `0600`. It is fsynced with the payload and backup
directory before PREPARED publication and remains independently available
after the transaction journal is removed. Inventory cross-checks its values
against the exact payload inode and content digest, and requires exactly the
metadata file plus that payload in the transaction directory.

`restore` validates the durable metadata and payload against the supplied
reference, candidate record digest, current manifest, and current target
identity, then calls the same journaled replacement path. It does not consume
or delete the backup.

## Restore and backup lifecycle

Backups are immutable user recovery artifacts. Their names are derived from a
random transaction id and lowercase SHA-256 of the record id; the journal
contains the referenced id and backup object identity, not a caller path.
Backups record exact bytes and original permission mode. The backup directory
and payload are fsynced before PREPARED can become durable. Backup creation
failure leaves the target and current manifest untouched. A failed replacement
after PREPARED rolls the target and manifest back but retains the backup.
`remove` does not remove backups.

Restore requires a validated backup reference, the expected current manifest
digest, and expected current target identity. It validates the backup as an
owned single-link regular file with exact identity/hash/mode before staging.
It then performs a normal journaled replacement whose PREPARED rollback returns
to the current state and whose COMMITTED state publishes the restored bytes
and candidate manifest. The referenced backup remains unchanged and retained.
Once the payload and independent metadata have been fsynced and the staging
directory atomically published, any later replacement error retains the backup
even if no journal or receipt was published. The caller discovers that
reference through `list_backups` after the store has recovered or reopened;
ordinary cleanup never removes it.
There is no automatic “restore latest” behavior, no globbing, and no deletion
of unreferenced or foreign backup entries.

## Error and ownership behavior

Public operations serialize on the store’s existing owner thread and lock.
They borrow request slices only through the call and return receipts by value.
No handle, parsed document, allocator buffer, or file descriptor escapes except
the documented bounded receipt. Every allocation and opened descriptor has a
single deterministic owner and cleanup path. Allocation failures before
PREPARED leave targets and manifest untouched. Allocation after PREPARED must
leave a fully parseable durable journal and sufficient recovery evidence; where
possible, preallocate recovery state before writing PREPARED. No cleanup path
may unlink an object until descriptor-based type, owner, link count, mode,
hash, device, and inode checks authorize that exact object.

Errors must distinguish at least invalid request/schema, manifest conflict,
artifact conflict, unsupported atomic filesystem operation, I/O failure,
allocation failure, and `RecoveryRequired`. The first operational error remains
primary if rollback or cleanup also fails; the journal/evidence remains the
source of retry. No API reports success until the committed journal is durable,
the published artifact and manifest are verified, and the operation’s required
directory fsyncs have succeeded.

## Acceptance criteria

Full feature closure requires all criteria below on the real store and
filesystem seam. The accepted journal/API milestone uses actual rooted-file
replacement, retained backups, restart/restore, PREPARED/COMMITTED checkpoint
faults, transient syscall retries, legacy v1 recovery, adversarial identities
and allocation-failure sweeps. Child termination at every boundary and the
integrated command/removal matrix remain separate, unaccepted work. Passing
isolated helper tests does not count as store acceptance.

### Positive real-filesystem cases

- Replace an unmodified existing managed regular file and update only its
  record digest plus allowed metadata; verify exact new bytes, mode, hash,
  manifest bytes, and inode change.
- Force-replace a modified managed file; verify backup bytes exactly equal the
  pre-replacement bytes, backup mode is `0600`, recorded original mode is exact,
  backup is owned/single-link, and it persists after success and restart.
- Restore an explicitly named retained backup; verify exact bytes and original
  artifact mode return, the current manifest digest changes as requested, and
  the backup remains byte-identical and available.
- Run recover twice after completed replacement, restore, and cleanup; second
  recovery is a no-op with all final bytes and retained backups unchanged.
- Verify old absent-only API behavior and old journal recovery remain intact.

### Refusal and adversarial namespace cases

- Reject wrong expected manifest digest, wrong target digest/mode/device/inode,
  stale record id, altered immutable record fields, multiple changed records,
  duplicate targets, unsupported kind/root, malformed/newer schema, oversized
  journal or payload, invalid mode, invalid transaction id, and unknown journal
  field before target mutation.
- Preserve and refuse symlink roots, symlink parents, symlink target, FIFO,
  device, socket, directory, foreign-owned file, hard link (`nlink != 1`),
  wrong-owner backup, wrong-mode backup, and backup path/identity mismatch.
- Replace the target or manifest between preflight and each atomic mutation
  seam. Verify foreign bytes and all evidence remain; result is conflict or
  `RecoveryRequired`, never false success or deletion of the replacement.
- Inject unexpected files at deterministic sibling, transaction, and backup
  names. Verify no overwrite, no unlink, no traversal, and retained journal.
- Reject unsupported `RENAME_EXCHANGE` / `RENAME_NOREPLACE` without a weaker
  rename fallback.
- Refuse restore if current manifest or target is modified, absent contrary to
  request, or replaced by a different inode, even if bytes happen to match.
- Never garbage-collect a backup merely because its record was removed or the
  original transaction was committed.

### Crash recovery matrix

For each replacement and restore, terminate a child at every durable boundary:
transaction creation, backup write/fsync, old/new payload write/fsync, staged
manifest write/fsync, sibling-temp write/fsync, PREPARED journal write/fsync/
rename/directory-fsync, artifact namespace operation, artifact-parent fsync,
manifest publication, manifest-directory fsync, COMMITTED journal replacement
and directory fsync, every cleanup unlink/rmdir, and final transactions-dir
fsync. Reopen the real store and assert:

- PREPARED recovery results in the exact old artifact and old manifest, with
  required backup retained and no transaction-owned debris after successful
  cleanup; or
- COMMITTED recovery results in exact new artifact and new manifest, with
  required backup retained and no transaction-owned debris after successful
  cleanup; or
- if any observed object is foreign/ambiguous, recovery returns
  `RecoveryRequired` and retains journal, backup, and all unverified namespace
  entries byte-for-byte.

Run recovery again after every successful recovery and after clearing each
injected transient fault to prove retry/idempotence.

### Fault injection and allocator testing

Use real temporary directory trees and a narrow syscall dispatch seam whose
normal functions perform real operations. Inject each open/openat/fstat/fstatat,
read/write, mkdir, chmod, fsync, flock, rename/no-replace/exchange, unlink, and
close failure at every call index; include `EINTR`, short/zero transfer,
`ENOSPC`, `EDQUOT`, `EIO`, `EACCES`, and unsupported-flag errors where
applicable. Assert the operation never returns success after an incomplete
durable boundary, no foreign object is removed, and journal/backups are either
fully absent before PREPARED or sufficient and retained after PREPARED.

Run `std.testing.checkAllAllocationFailures` over request validation, schema
parse, journal encode/decode, backup staging, replacement staging, recovery,
and restore. At every injected failure assert zero leaked allocator bytes,
descriptors, lock ownership, or transaction-owned namespace changes that lack
durable recovery evidence. Ensure allocator OOM preserves caller outputs and
request memory unchanged.

### Coverage and sanitizer gate

Run the repository’s `tests/av/coverage.py` gate through its public seam. The
actual production replacement and recovery implementation, not test-only
copies, must reach at least 90% source lines and 90% taken arcs with no source
exclusions. This Zig 0.13 module's memory-ownership gate is
`std.testing.allocator` with allocation-failure coverage and zero leaked
bytes; do not claim Zig AddressSanitizer or LeakSanitizer coverage from that
gate. C consumers and adapters have their own ASan/LSan/UBSan requirements.
Keep coverage artifacts and raw commands/results in a persistent run directory
so a reviewer can rerun and inspect them. Native filesystem acceptance is
required; mocked journal or fabricated inode success cannot substitute.

## Integration boundary

The CLI/session owner calls `recover` under the existing store lock, then
invokes the replacement or restore method with an exact candidate manifest and
expected identities. Manifest replacement uses `RENAME_EXCHANGE` after
verifying the current manifest's bytes, mode, device, and inode; the displaced
object remains at the transaction-specific temporary name until its identity
and digest are checked. This gives atomic namespace exchange against competing
cooperating store users. The store lock does not serialize unrelated writers;
unexpected target or staging identities fail closed and retain recovery
evidence. Version-2 replacement journals require the old manifest's nonzero
device and inode identity. If rollback still needs that old manifest but its
exact displaced inode is missing from the staging name, version-2 recovery
returns `RecoveryRequired`; it never manufactures a replacement inode whose
identity is absent from the durable journal.

Legacy version-1 absent-only journals predate old-manifest inode fields and
used ordinary rename publication. A PREPARED version-1 transaction may have
published its new manifest without leaving the displaced old manifest at the
transaction-specific temp name. Recovery first validates the journal,
candidate manifest, old-manifest bytes, record delta, rooted artifact mapping,
and every current artifact and manifest state. Only when the current manifest
is the exact journal-owned new manifest and the transaction's `old-manifest`
bytes hash to the journal's recorded old-manifest digest may recovery recreate
the old-manifest temp from those authenticated transaction bytes. It stages
that temp before removing or exchanging any artifact. On a later failure, a
retry checks the recreated temp by mode and digest and reuses it. This
compatibility path applies only to version 1 with absent old-manifest identity
fields; a version-2 missing or mismatched inode always fails closed with
evidence retained. The engine does not decide which artifacts should be repaired,
generate catalog entries, invoke privileged operations, or interpret `--force`
outside the explicitly passed per-record backup authorization.
