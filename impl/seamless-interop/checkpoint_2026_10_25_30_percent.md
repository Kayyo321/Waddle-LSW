# Seamless interop: 30% milestone checkpoint

The user requested autonomous parallel Luna development with root critical
review to reach30% of the existing overall tracker. Accepted completion is
**30.95%**, with the original34 task weights unchanged. The feature remains
In Progress, Time Ended remains TBD, and no PR/merge is eligible at this milestone.

## Recoverable Git state

Feature branch: `feat/seamless-interop`; merge target: `origin`. Code milestone
commits are `aac0e6e` (replacement/backup/restore) and `58eefbd` (desktop
materialization/rooted consumer). The preceding native catalog acceptance is
`91b6d29`. This checkpoint is documentation only and earns no additional credit.
All Luna ownership is released; no worker is authorized to continue edits.
`resume_work_orders.json` records the exact accepted slices and next open lanes.

## Accepted evidence

- #13 portable reducer50%: root170tests,100%lines/93.90%taken arcs.
- #27 native icon15%: fixed four-bitmap retry ownership at `eb01019`, actual
  Windows run40224183558,98.10%lines/90.47%taken arcs. Native catalog20%:
  actual `6318a3d` Windows run40225302430,100%lines/90.15%taken arcs.
  Existing portable snapshot/diff20% and PNG10% remain accepted.
- #33 now65%: schema15%, journal30%, rooted ownership/backup/restore API20%.
  Independent64module/imported plus3public tests; whole source99.79%lines/
  90.02%taken arcs. Retained backups survive restart; explicit restore preserves
  bytes/mode; v1/v2 recovery, identity conflicts, checkpoint/syscall/OOM retry
  tests pass. See `manifest_replace_evidence_2026_10_25/qualification.json`.
- #28 now60%: entries20%, menu/directory10%, supplied-icon data10%, real rooted
  publication/recovery consumer20%. Independent90tests;98.55%lines/90.03%taken
  arcs. Root corrected the menu Directory basename and pinned the cross-output
  relation. Real PREPARED rollback and COMMITTED recovery twice preserve prior
  records; desktop-file validation and actual GLib argv parsing pass. See
  `desktop_artifacts_evidence_2026_10_25/qualification.json`.

The Zig0.13 ownership proof uses its leak-detecting allocator and allocation
failure sweeps. The real GLib C consumer runs with ASan/LSan/UBSan and
`detect_leaks=1`. Windows GDI/COM providers have actual Windows functionality and
coverage evidence; Linux C shared ownership has its separate sanitizer gate.
No Zig or Windows-provider sanitizer execution is invented.

## CI and restart

At checkpoint creation, all four workflows for `6318a3d` are green: full
interop40225302507, native components40225302430, AV40225302518 and
graph40225302432. This checkpoint and the two accepted host commits are to be
pushed together; inspect the latest-head runs for their final status. Reproduce
the local changed-area gates with `make interop-manifest-store-test`,
`make interop-desktop-artifacts-test`, and coverage modes `manifest_store` and
`desktop_artifacts` in `tests/av/coverage.py`. Compiler cache, local sockets and
LSan require normal local execution rather than the restricted tool sandbox.
Do not weaken gates when sandbox access fails.

## Remaining work and machine state

Continue coordinator/capture/slot/wire integration (#10), session/Wayland/local
commands (#7/#11), native state adapters (#13), catalog watches/runtime (#27),
desktop association/mirroring/native shell (#28), and command/all-producer plus
comprehensive crash/native/full-removal qualification (#33). The tracker
records all other uncompleted tasks. The temporary rooted consumer does not
install into a live shell or launch a guest app. No hardware capability is
newly claimed from portable fixtures.

The fresh isolated installed Windows guest is ACPI stopped; QEMU and swtpm were
reaped. Original Win11, its TPM/firmware, and private preparation assets are
untouched. Never run `make clean`, `make test-sanitizers`, or a broad build-tree
cleanup: build contains large VM disks and retained evidence. Do not inspect or
publish credentials or answer-media contents. README.md was not modified.
