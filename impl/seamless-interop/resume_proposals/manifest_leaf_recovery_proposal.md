# Leaf replacement recovery proposal

## Decision boundary

Linux does not expose a general compare-and-swap for a directory entry. A hash/stat check followed by `renameat()` or `unlinkat()` cannot prove that the checked inode is still at the name when the mutation occurs. `renameat2(RENAME_EXCHANGE)` atomically swaps two names and preserves both entries; `RENAME_NOREPLACE` atomically creates a name only if it is absent. Neither flag compares an entry's inode or contents. Exchange and no-replace support is filesystem-dependent, so unsupported operations must fail closed rather than fall back to check-then-rename.

The bounded implementation can guarantee that a concurrent replacement observed at the mutation boundary is retained and causes `RecoveryRequired`; it cannot promise a linearizable leaf CAS against a non-cooperating process that continues renaming or writing concurrently. That stronger promise is impossible with these syscalls. The store must not report success for an entry whose displaced object differs from the prepared expected object. If the frozen wording requires the public target name never to change transiently under arbitrary same-UID writers, implementation is blocked until the environment supplies enforced writer exclusion (a privileged broker or equivalent). An advisory flock only serializes Waddle processes.

## Bounded apply protocol

Keep the existing ordering: stage exact old/new bytes and metadata, fsync them and their directories, then durably publish the prepared journal before touching an artifact. Give each action one deterministic same-directory temporary name derived from transaction ID and action index. Retain that name until the journal reaches Committed; never delete a displaced entry just because apply returned an error.

For a write when the target existed at preparation:

1. Create and fsync the new temporary file in the held parent directory; retain the prepared old bytes/mode in the private transaction data.
2. Atomically `RENAME_EXCHANGE(temp, target)` using the held parent fd for both names, then fsync the parent directory. At this instant the target contains the staged new inode and the temporary name contains the inode actually displaced, including any replacement made after preflight.
3. Open the displaced temporary with `O_NOFOLLOW`, verify regular-file/owner/mode, and bounded-read/hash it. Accept the action only if it matches the prepared old hash and mode. Keep the displaced old entry at the temporary name until Committed cleanup.
4. On mismatch, do not unlink or overwrite the displaced entry. Attempt an exchange-back only when the current target is still the staged new inode; after the attempt, inspect both names. If any identity is unexpected or restoration cannot be confirmed, leave every surviving name and journal intact and return `RecoveryRequired`. Do not run prepared cleanup on this path.

For a write when the target was absent, use `RENAME_NOREPLACE(temp, target)`. `EEXIST` is a conflict; preserve any remaining temporary/journal state and do not replace the newly created target. This closes the absent-name race without pretending to compare contents.

For remove, atomically rename the target to the deterministic temporary name with `RENAME_NOREPLACE` on that destination, fsync the parent, then hash/mode-check the displaced entry. If it matches the prepared old object, the remove is applied and the old entry remains at the temporary name until Committed cleanup. If it differs, try to restore it with `RENAME_NOREPLACE(temp, target)`; if a concurrent creator now occupies target, preserve both entries and return `RecoveryRequired`.

All rename operations stay within one parent directory/filesystem. `ENOSYS`, `EINVAL` for an unsupported flag, or filesystem `EOPNOTSUPP` becomes a fail-closed unsupported-operation error before that action is considered applied. Never silently fall back to ordinary rename/unlink.

## Recovery interpretation

Recovery must inspect the deterministic temporary name for every journal action before rollback or cleanup. For a prepared replacement, the normal pre-apply state is `(target=old, temp=absent)`; the post-exchange state is `(target=new, temp=old)`. For prepared creation, a pre-rename staged file is `(target=absent, temp=new)`; post-create is `(target=new, temp=absent)`. For remove, pre-apply is `(target=old, temp=absent)` and post-rename is `(target=absent, temp=old)`. Validate all extant entries against the journal's bounded hashes/modes and expected staged data before changing either name. Any unknown pair or type is retained and returns `RecoveryRequired`.

Rollback of an applied replacement exchanges the verified target and retained old temporary; rollback of creation removes only a target verified as the staged new object; rollback of remove restores the retained old temporary with `RENAME_NOREPLACE`. Each namespace change is fsynced before advancing recovery. If the target has been independently recreated or either side no longer matches the journal, stop and preserve the journal and siblings.

Committed recovery verifies the manifest and all new targets, then permits cleanup only when each retained temporary matches the corresponding known old object (or is absent for a creation). Unknown temporary data blocks cleanup. Prepared cleanup must likewise verify that any temp is the staged new file or the exact displaced old file; it must never blindly unlink a deterministic name that could now refer to a third-party file.

Incomplete retained-backup payload/metadata pairs remain `RecoveryRequired` and are preserved as already specified. The exchange temporary is a second recovery location, not a substitute for the private force-edit backup.

## Required tests and limits

Use a syscall seam to replace/create the target immediately before exchange/no-replace and immediately after the first exchange. Verify the unexpected bytes remain at target or the deterministic retained sibling, the journal remains, and the result is `Conflict`/`RecoveryRequired`, never success or silent cleanup. Cover prepared and committed restart states for all three operation cases, rename/fsync failures at every boundary, unsupported flags, a target recreated before remove restoration, and repeated recover calls. The tests must assert directory contents and bytes, not just error codes.

This protocol preserves the displaced object at the atomic rename boundary and makes crashes inspectable. It does not lock out an external writer that already holds an fd and continues modifying the displaced inode, or one that repeatedly races recovery. Such cases must fail closed; guaranteeing an immutable snapshot against a hostile same-UID writer requires an enforced isolation boundary outside this store.
