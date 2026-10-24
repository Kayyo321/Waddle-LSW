> Unapproved design proposal preserved for lead review. No tracker credit or frozen contract change.

# #33 Manifest journal and recovery design proposal

## Scope and boundary

Implement only the durable store, exclusive lock, journal validation, recovery, and crash/failure tests. Keep `src/interop/manifest.zig` a pure schema-1 parser/validator. Do not add `waddle integration` commands, catalog producers, session startup, guest writes, desktop-entry generation, or host-policy generation in this slice. A later caller supplies a fully serialized candidate manifest plus a bounded set of already-authorized artifact changes; this store remains the final filesystem safety boundary and revalidates each target.

The store uses the existing codec before any mutation. It rejects a newer/unknown schema, malformed JSON, duplicate IDs, and records that fail path-policy checks. `parse` remains validation rather than authorization. No guest paths are actionable on the host.

## Proposed files and API

Add `src/interop/manifest_store.zig`; put private helpers and its unit/fault tests in the same Zig module. Do not change the existing manifest codec API in this slice. The store accepts candidate manifest bytes, validates them with `manifest.parse`, and publishes those exact bytes. A later caller/serializer can produce canonical bytes independently.

Proposed public Zig API:

```zig
pub const scope_t = enum { user, system };
pub const store_t = struct {
    // Owns allocator-backed root paths and open directory/lock descriptors.
    // Must be closed exactly once; not copyable by convention.
    pub fn open(allocator: std.mem.Allocator, scope: scope_t) !store_t;
    pub fn recover(self: *store_t) !void; // requires held exclusive lock
    pub fn commit(self: *store_t, request: commit_request_t) !void;
    pub fn close(self: *store_t) void;
};
pub const commit_request_t = struct {
    expected_manifest_sha256: ?[32]u8, // null means manifest must be absent
    manifest_bytes: []const u8,
    actions: []const artifact_action_t,
};
pub const artifact_action_t = struct {
    record_id: []const u8,
    operation: enum { write, remove },
    target: authorized_target_t, // root enum + validated relative path, never arbitrary fd/path
    expected_current_sha256: ?[32]u8, // null means target must be absent
    bytes: ?[]const u8, // required for write, absent for remove; borrowed through commit only
    mode: u32,
    backup_modified: bool,
};
```

`authorized_target_t` is private to the module or constructible only through a validating function that takes the parsed record and scope. It carries an allowlisted root enum and a relative path; it cannot carry an arbitrary absolute path. The caller owns all request slices and retains them only for the duration of `commit`; the store copies data that must survive the call into transaction-owned files. `store_t` owns its allocator-backed paths, root descriptors, lock descriptor, and any temporary arena. `close` unlocks/closes descriptors and frees owned allocations; it is idempotent. `recover` and `commit` are serialized by the same lock held from `open` until `close`.

The implementation should use Zig/POSIX `openat`-style dirfd operations through the existing C import conventions. Do not add a C wrapper until the actual C caller integration is owned; if needed later, mirror the established Zig-exported C ABI pattern used by device storage rather than duplicating filesystem logic in C.

## Store roots, lock, and path safety

User state root is `$XDG_STATE_HOME/waddle/integration`, falling back to `$HOME/.local/state/waddle/integration` only when `XDG_STATE_HOME` is unset. System state is exactly `/var/lib/waddle/integration` and requires effective uid 0. Never fall through from system to user state. The store directory, `transactions`, `backups`, manifest, journals, and lock are opened without following symlinks; lock and metadata files are regular files. User-owned state directories/files must be owned by the effective user and private (`0700` directories, `0600` metadata/lock); system state must be root-owned and not writable by group/other. Acquire `manifest.lock` with nonblocking exclusive `flock`; busy means return a distinct busy error without reading, deleting, or rewriting journals. Keep the lock across recovery and the whole commit. The lock order remains session lock → device AV command lock → manifest lock; commands that start/stop the session must release the manifest lock before doing so.

For action paths, validate record-kind/scope/root mapping before touching the filesystem, reject NUL, empty, `.`, `..`, absolute-relative confusion, oversized components, and disallowed filename characters. Walk each root and parent component by dirfd with `O_DIRECTORY|O_NOFOLLOW`; reject symlink parents and a symlink/non-regular final target. Verify the final path is the allowlisted root for that record kind, not merely a prefix match. Important frozen examples include desktop entries under `$XDG_DATA_HOME/applications`, icons under `$XDG_DATA_HOME/icons/hicolor/<size>x<size>/apps`, shortcuts under the validated desktop directory, and menu files under `$XDG_CONFIG_HOME/menus/applications-merged`. Root-only/system policy artifacts are a separate scope and must never be reachable from a user store. Guest record kinds are rejected by this host store. A future policy layer must enumerate the remaining frozen kind-to-root mappings before enabling those kinds.

A target's current state is checked immediately before the transaction and again immediately before replacement/removal. It must match the caller's expected content hash (or be absent). If a selected file was user-modified, preserve it and return a conflict unless `backup_modified` corresponds to explicit force authorization from the caller. For forced replacement/removal, copy exact prior bytes and mode into `backups/<transaction-id>/<record-id-hash>` with mode 0600, fsync the file and backup directories, and only then permit mutation. Backups are retained after commit and rollback; ordinary remove never deletes them.

## Journal format and durable transaction sequence

Use a strict bounded JSON journal (schema version 1) at `transactions/<32-lowercase-hex-id>.json`. It contains only: transaction id, phase (`prepared` or `committed`), old manifest presence/hash and backup reference, new manifest hash and staged-file reference, action count, and for each action the record id, allowlisted root enum, relative path, operation, expected-before state, desired-after hash/mode, exact backup reference when present, and transaction-owned sibling-temp basename. No arbitrary absolute path is trusted from journal input. Bound the number of journals to 32, actions to 4096, journal size to 4 MiB, strings to codec limits, reject duplicate JSON keys/IDs, reject unknown fields/version/phase, and validate every referenced file digest and path before recovery mutates anything.

Transaction order:

1. Caller holds the exclusive lock. Run recovery first; refuse a new commit if any journal cannot be safely recovered.
2. Parse/validate the candidate manifest and check its `expected_manifest_sha256` against the current manifest (or require absence). Validate every action against both old/new records and the scope policy. Open all target parents safely and capture stable identity/hash/mode for each target. Reject duplicate targets and ambiguous action ordering.
3. Create a random transaction id and private transaction directory. Stage the exact new manifest, any replacement payloads, and byte-exact prior-state backups. For target directories on other filesystems, create transaction-specific sibling temp files using exclusive create/no-follow, write desired bytes and fsync them, then fsync each containing directory. Fsync every staged/backup file and all transaction/backup directories. No target is changed yet.
4. Write the `prepared` journal through a transaction-specific temporary file: write complete bounded JSON, fsync it, rename it to the final journal name, and fsync `transactions/`. Once this durable boundary exists, every rollback byte and every staged replacement is already durable.
5. Apply actions one by one with same-directory atomic renames/unlinks, verifying the precondition again and fsyncing each changed parent directory. A remove first preserves prior bytes in the durable backup; recovery can restore by writing a sibling temp from that backup and atomically renaming it into place. Keep action application idempotent by recognizing exact before/after hashes and transaction-owned temp names; unknown third-party state is a recovery conflict, never overwritten.
6. Atomically rename the staged manifest over `manifest.json` last, then fsync the manifest directory. This is the externally visible commit point.
7. Atomically replace the journal phase with `committed` using temp-write/fsync/rename/fsync-directory. Only after this durable phase may cleanup begin.
8. On committed cleanup, remove transaction-owned sibling temps and the transaction staging directory without following links; retain backups. Fsync affected directories, unlink the journal, then fsync `transactions/`.

Use the device-storage implementation's strict JSON parsing, no-follow traversal, transaction id validation, fsync discipline, atomic journal replacement, and idempotent prepared/committed recovery as the closest local pattern. Do not reuse device-storage's domain-specific move journal directly; manifest transactions span the manifest directory and artifact roots and need content hashes plus retained byte backups.

## Recovery semantics

`store.open` acquires the lock; the caller must call `recover` before a new mutation. Recovery enumerates only direct regular `.json` journal files under `transactions`, rejects symlink/non-regular entries, and applies the bound of 32. Validate the complete journal and all referenced staged/backup bytes before changing any target.

For `prepared`, undo actions in reverse order, restoring each prior regular file from its durable backup (or removing a transaction-created file) with a same-directory temp and atomic rename; restore the old manifest bytes or remove it if originally absent. Each undo is idempotent. If any target now contains unrelated bytes, has become a symlink/non-regular file, or parent identity differs, stop with `RecoveryRequired`, preserve the journal and all backups, and do not claim success. After rollback, remove only transaction-owned staged/temp files, fsync, then unlink/fsync the journal.

For `committed`, verify the manifest and every desired action still match the journal's after-hashes before cleanup. Do not roll back committed data. If verification fails, preserve journal/backups and report a conflict for repair. Cleanup is idempotent and removes only the exact transaction-owned names recorded in the journal. Unknown schema, malformed journal, digest mismatch, missing backup, or path escape fails closed and prevents new commits.

A crash after manifest replacement but before durable `committed` phase is treated as prepared and rolled back, leaving old manifest and old artifacts. A crash after committed phase is treated as committed and only cleaned up. Therefore callers see a consistent old or new manifest/artifact set after recovery, never a claimed partial success.

## Error and allocation ownership

Use one arena per `open`/`recover` call or bounded transaction parse; no parsed journal/manifest slices escape the arena lifetime. The caller owns request slices. The store owns lock/root descriptors and all copied/staged bytes. All errors before the prepared journal may remove only transaction-owned files and leave targets untouched. Errors after prepared must synchronously attempt recovery while still holding the lock; if recovery fails, preserve the journal and return a recovery-required error. Never clear lock ownership while a child process exists; this engine does not spawn subprocesses.

Use `std.testing.allocator` and `std.testing.checkAllAllocationFailures` for journal parse, action validation/materialization, and each allocation-bearing prepare/commit path. Ensure defers close every opened fd and arena on all allocator failures. Allocator errors must occur before target mutation where possible; after the prepared journal, allocation-free rollback is preferred by preallocating all recovery paths/records before the journal becomes durable. If recovery requires allocation, journal data remains authoritative and retryable after allocation becomes available.

## Required tests

Add `test "..."` coverage in `src/interop/manifest_store.zig` using isolated `std.testing.tmpDir` roots and private fake XDG state. No shared build, host installation, VM, GPU, or privileged `/var/lib` writes.

- Lock lifecycle: second process/thread gets busy without altering journal/manifest; lock released on close and reacquirable. Verify lock path is regular and symlink/FIFO is rejected.
- Scope and path policy: user cannot target system roots; guest kinds fail; reject traversal, absolute paths outside allowed root, symlink parent/final target, non-regular final target, duplicate action target, invalid owner/mode, path-prefix confusion.
- Codec/preflight: malformed/duplicate/newer-schema manifest, stale expected old hash, changed target between preflight and apply, file conflict, force backup exact byte+mode and retained backup; backup write/fsync failure leaves original untouched.
- Journal validation: malformed JSON, duplicate keys/IDs, unsupported schema/phase, missing/changed staged file, missing/changed backup, hash mismatch, too many journals/actions, temp-name escape, unknown fields all fail closed and retain journal.
- Crash-at-each-durable-boundary child tests: transaction directory, staged manifest/payload fsyncs, backup fsync, prepared-journal file fsync/rename/directory fsync, each artifact rename and parent fsync, manifest rename and directory fsync, committed-journal replacement/fsync, cleanup/unlink/fsync. After each child exits, reopen and recover; assert old state for prepared crashes and new state for committed crashes, then run recovery twice to prove idempotence.
- Fault injection: write, fsync, rename, unlink, openat, and directory fsync failures at each indexed call (ENOSPC, EDQUOT, EIO, EACCES). Assert no false success, no foreign-file deletion, and either a complete old/new state or a retained recoverable journal.
- Allocation failures: all failure points in codec and journal decode/validation/prepare/commit; assert no leaked allocation, no leaked fd, no changed target before prepared, and an intact journal after any post-prepared failure.
- Races: concurrent commits serialize; an external replacement between preflight and rename causes conflict; symlink swap or parent inode replacement cannot redirect an action. Do not test through a real session or system-wide integration tree.

## Integration handoff

The session/CLI owner wires `open → recover → commit → close` into both `waddle integration` startup and `waddle-session` startup under the documented lock order. Installation commits host base artifacts and manifest, releases the manifest lock, then starts the session. Catalog synchronization is a separate commit. The store does not spawn/start/stop the session and does not implement producers. Guest manifest operations remain a separate Windows-side transaction and must not be represented as host filesystem success.
