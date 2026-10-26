# Manifest store milestone contract

This document records the bounded implementation in `src/interop/manifest_store.zig`. It is an incremental single-action milestone under frozen manifest schema 1 and IMPL_DESC §8.9; it does not replace the normative feature description.

## Scope and state

The store accepts preopened Data, Config, and Desktop directory descriptors and borrowed canonical absolute spellings. It walks each spelling from `/` with `O_DIRECTORY|O_NOFOLLOW`, binds the final device/inode to the supplied descriptor, rejects duplicate kinds and overlapping roots, and rejects roots writable by group or other. The caller is responsible for choosing the correct existing export root. State directory and `transactions/` must be current-user-owned exact-mode `0700`; `manifest.lock`, staged manifests, and journals must be current-user-owned exact-mode `0600`. The lock is `flock(LOCK_EX|LOCK_NB)` and remains held until deinit. This lock coordinates cooperating Waddle processes only.

Supported artifact paths are selected from the frozen record kind and configured root: desktop/handler entries under Data `applications/`, icons under Data `icons/hicolor/<size>/apps/<leaf>` (the XDG-correct form desktop_artifacts.zig emits; new commits and replacements accept only this form). Records already present in a manifest with the earlier `hicolor/<size>/apps/<leaf>` form are still inspected, removed, and recovered, and recovery accepts both forms, so a legacy record never wedges; no new commit writes it, data menus under `desktop-directories/`, config menus under `menus/applications-merged/`, autostart under Config `autostart/`, and one-component desktop shortcuts under Desktop. The guest-facing kinds and unsupported host policy kinds fail before filesystem mutation. Every target parent component is opened descriptor-relative with `O_DIRECTORY|O_NOFOLLOW`; target files must be current-user-owned regular files with one hard link.

## Transaction semantics

This milestone supports adding exactly one absent artifact. Replacement of an existing record/target, even with `backup_modified=true`, returns `UnsupportedReplacement` before transaction staging. There is no backup/restore API in this milestone. New artifact publication uses Linux `renameat2(RENAME_NOREPLACE)` after checking expected absence. It does not overwrite an unexpected target.

Before mutation the store parses the candidate manifest with the frozen codec, validates its digest and target binding, parses the current manifest if present, and verifies the complete old-to-candidate record delta: only the requested target may be added. The bounded private journal uses strict JSON, exact schema version 1 and transaction-id binding, duplicate/unknown-field rejection, 4 MiB limit, and a maximum of 32 transaction directories. It records exact staged manifest hashes, artifact hash/mode, phase, allowlisted root role, record id, and relative path. No absolute journal path is trusted.

The store stages the exact new artifact and candidate manifest in private transaction files, then creates deterministic same-parent artifact and manifest temporary files and fsyncs each file and containing directory. Their device/inode, mode, and SHA-256 facts are recorded before `prepared` is durably written. It publishes the artifact with no-clobber semantics, publishes/fsyncs the manifest last, durably switches the journal to `committed`, verifies the published hashes, and cleans transaction files. This sequence is not global multi-file atomicity. A failure after `prepared` leaves the store recoverable; every new commit first retries recovery.

Recovery fully validates journal bounds/version/id/phase, staged file modes and hashes, old and candidate manifests, complete manifest delta, and candidate record-to-root/path binding before changing host files. `prepared` rolls back in reverse publication order only after preflighting both live artifact and manifest: the artifact is unlinked only if device/inode, mode, and SHA-256 match the staged new file, and the manifest is restored or removed only if its staged identity and hash authorize it. Same-content files with a different inode are conflicts. `committed` verifies artifact and manifest device/inode, mode, and SHA-256 before cleanup. `committed` verifies new artifact and manifest hashes and only cleans transaction state. Any third state, malformed journal, missing required evidence, or mismatch fails closed as `RecoveryRequired` and retains evidence. A journal-less transaction directory fails closed; only an empty directory produced after the cleanup transition is automatically removed. `manifest.lock` serializes Waddle writers but cannot prevent unrelated same-user processes from racing pathnames.

## Ownership and error contract

`store_t.init` borrows roots only during the call, duplicates their descriptors close-on-exec, and copies their spellings with the supplied allocator. The store owns these copies until idempotent `deinit`; the allocator must outlive the store. `request_t` slices are borrowed for `commit` only. `Busy` means the lock is already held; `UnsupportedArtifactKind`, `InvalidScope`, `InvalidPath`, `ManifestMismatch`, `ArtifactConflict`, and `UnsupportedReplacement` reject before target mutation. `RecoveryRequired` means evidence cannot authorize automatic rollback/cleanup. Native filesystem errors are mapped to bounded Zig errors; `OutOfMemory` remains distinguishable and failure paths release allocator-owned data.

## Verification

The module's embedded tests run with:

```sh
ZIG_LOCAL_CACHE_DIR=/tmp/waddle_manifest_local \
ZIG_GLOBAL_CACHE_DIR=/tmp/waddle_manifest_global \
zig test -lc src/interop/manifest_store.zig
```

They exercise real temporary directories, lock contention, symlink-root rejection, transaction recovery at each injected durable checkpoint, POSIX syscall failures during both PREPARED rollback and COMMITTED verification/cleanup, actual short reads/writes and EINTR retry behavior, ownership helper checks against genuine files with mode/link/type/hash/inode mismatches and injected read/unlink errors, and allocator failures at successive allocations until the first allocation index that permits completion. The private syscall seam performs all normal operations against the real filesystem and only injects a selected error or caps one real transfer length. The public API bounds smoke test is in `tests/interop/manifest_store.zig`. Linux `renameat2(RENAME_NOREPLACE)` is required.
