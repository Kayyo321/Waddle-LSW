# Absent-artifact transaction qualification

Root independently ran the exact source through the existing ReleaseSafe LLVM
coverage gate: 43 embedded tests, 912/1013 taken conditional/switch arcs (90.03%)
and860/862 source line sites (99.77%). Metadata, raw hit pairs, source hash,
toolchain and missed sites are retained. The established recorder excludes165
compiler panic/stack guards; no new source or production error path exclusion.
The independent public Zig API smoke test also passes. Zig std.testing.allocator
and allocation-failure tests verify tracked allocation cleanup; no claim of C
ASan instrumentation for this Zig0.13 module is made.

Real temporary filesystem transactions cover strict schema/delta/roots, modes,
symlinks/hardlinks, locks, staged identity/hash ownership, PREPARED reverse rollback,
COMMITTED cleanup, fsync failures, short transfer/EINTR, OOM and adversarial
same-content foreign-inode replacement. Every conflicting state is retained.
The contract documents cooperative lock limits and exactly-one-absent-artifact
scope. Existing-target replacement, backups, restore, commands and producers
are unsupported and remain open.

Accept15% of TODO #33 journal30% for this absent-add transaction/recovery path,
and10% of artifact API20% for rooted ownership/no-clobber publication. Remaining
journal15% and artifact10% require replacement/backup/restore. Parent #33 rises
from15% to40%, adding0.75% overall (total20.55%). Full-removal/crash/native15%
remains open even though these bounded crash cases pass.
