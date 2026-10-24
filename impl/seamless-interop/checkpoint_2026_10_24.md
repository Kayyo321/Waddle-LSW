# Recoverable checkpoint: 2026-10-24 03:22 UTC

Stopped at the user's explicit request. No implementation work or background
monitor is scheduled. Resume the same feature goal: accurate tracker to100%,
parallel Luna implementation, lead adversarial review, then PR integration into
branch `origin`. This checkpoint earns only the explicitly recorded foundation
credit; the feature remains incomplete.

## Repository and accepted state

- Workspace: `/home/dev/Waddle-LSW`.
- Feature branch: `feat/seamless-interop`; remote: `origin`.
- Target branch: `origin` (last observed `origin/origin` at89671de).
- Last implementation commit: eea6671. The containing documentation commit is
  the resume point; it also records successful native CPU qualification.
- Tracker: **14.00%**; #5=50%, #8=65%, #9=50%, #33=15%.
  All remaining task percentages and acceptance allocations are authoritative
  in [TRACKER.md](TRACKER.md). No completion timestamp, PR or merge is claimed.
- Frozen protocolv2/layoutv3/SMW3/manifest-schema1 stay unchanged.
- User decisions persist: include ViGEmBus; use this Linux machine and Waddle
  devices for native testing. The driver has not been installed.
- README is untouched. Every completed code change is committed separately.

Accepted commits since the previous checkpoint4079560:

| Commit | Result |
|---|---|
| 6cbc90e | Resumed contracts, exclusive work orders and acceptance allocation |
| ef39989 | Four pure layout C ABI wrappers and Linux/Windows consumers |
| 6f1ce16 | Checked native descriptor/heartbeat atomics and CPU stress/coverage |
| 84fc54b | Bounded zero-to-eight drive-tagged QEMU mount arrays |
| ffd08d4 | Explicit readonly backend policy and recoverable process teardown |
| 56ceca5 | Actual pinned QEMU source build with PipeWire/Slirp/libusb |
| 51f3923 | Exact wide-switch coverage recorder; sparse masks within512KiB |
| 571c884 | Verifiable system/pinned vendor executable provenance |
| eea6671 | All-mount supervisor, retry/rollback, QEMU descriptor and IPC fixes |

The containing checkpoint commit accepts C ABI5% and native atomic5% of #5
only after hosted Linux and actual Windows execution. Every other accepted
subtask has its attribution in the tracker.

## Verification and durable evidence

[Foundation receipt](evidence/foundation_qualification_2026_10_24.json) binds
saved logs and successful CI. [Pinned-build receipt](evidence/qemu_source_build_2026_10_24.json)
binds helper/configuration/binary identities and host startup checks.

- Hosted interop run40057377210 succeeded at51f3923, both Linux and Windows.
  The Windows step actually executed all six codec/layout/heap/manifest/C ABI/
  native atomic executables; this is CPU qualification, not guest integration.
- Hosted Native AV run40057377234 also succeeded at51f3923.
- The later packaging/supervisor commits pass focused host checks but need the
  new pushed-head workflows checked on resume. Do not attribute old CI to them.
- Full `make test` passed before the final focused descriptor/IPC fixes.
  Fresh current-source server/lifecycle fixtures and rebuilt daemon pass after
  those fixes. The updated server fixture links
  `-Wl,--wrap=daemon_state_kill_subsystem` for fault injection.
- `make interop-test interop-sanitizers interop-coverage` passed. Production
  branch/line: codec97.23/96.65, layout98.44/98.57, heap97.92/100,
  manifest92.75/100, layout C ABI100/97.37, native atomic93.46/96.81 percent.
- Ten recorder tests pass with ASan/LSan/UBSan and leak detection enabled,
  including255-edge concurrent recording and the unchanged76,512-site case
  (382,560/524,288 bytes). CI's previously failing wide switch is now accepted.
- Eighteen pinned-build helper tests and fourteen vendor packaging tests pass.
- FS failure, QEMU lifecycle, multi-mount and server fixtures pass with
  ASan/LSan/UBSan and `detect_leaks=1` under host access. Pure Zig suites use
  their testing allocator/ReleaseSafe checks.
- Actual pinned QEMU9.2.4 b2995af... builds out of tree. Diskless paused QMP
  configuration checks pass for silent/PipeWire backends, HDA, xHCI and
  user+socket-guestfwd. No actual audio stream, USB realization or traffic proof.

Saved logs are in [evidence/checkpoint_2026_10_24](evidence/checkpoint_2026_10_24).
No hidden failed experiment has been converted into tracker acceptance.

## Native runner and preserved artifacts

All task-owned QEMU, virtiofsd and waddled processes were checked stopped before
this checkpoint. No Luna worker remains active. The `/dev/vhost-vsock` ACL is
restored exactly to root:kvm rw/rw with no dev entry. The user's original
37GB Windows disk and existing devices are preserved; no Windows VM was booted
or reprovisioned in this resumed phase.

Host access exposes `/dev/kvm` and `/dev/vhost-vsock`; the restricted executor
hides devices and disallows Unix sockets/LSan. Use reviewed host escalation for
native/sanitizer runs rather than suppressing leak detection. UID1001(dev) is
not in kvm. Existing session ACL permits KVM; vhost-vsock needs a temporary
user ACL with an exact saved restore in `finally` if testing as dev. Do not
permanently change groups or device permissions to reproduce a test.

The disposable device is isolated under `/tmp/wd-interop-kqyqdae2`:

- XDG config/state/runtime roots: its `config`, `state`, `run` directories.
  HOME was not overridden. It does not touch the user's real device registry.
- Name `interop-smoke`, empty64MiB qcow2,512MiB RAM, one CPU, CID32100,
  no networking/audio, private D: read-write and E: read-only test directories.
- First QEMU attempt failed with vhost-vsock EACCES. After the temporary ACL,
  both backend children started and QMP showed both drive-derived mount IDs.
- The harness then failed because `waddle kill` intentionally shuts down the
  supervisor too; checking that old daemon's FD directory afterward is invalid.
  Cleanup ran, children/daemon stopped and ACL restored. This is an **incomplete
  smoke**, not accepted native lifecycle/guest qualification.
- A preserved [draft harness](resume_proposals/native_device_smoke_draft.py)
  and saved incomplete logs allow exact review. Fix its metric before reuse:
  either verify all processes exit after CLI kill, or use daemon RPC directly
  if measuring repeated VM cycles within one supervisor. The real-FD regression
  already covers the supervisor descriptor leak in an automated test.
- Tomorrow inspect the root if it survives; create a fresh private root/device
  if absent. Never delete or replace an existing user's device to run this test.

Other artifacts that must not be cleaned:

- `build/windows11_vm/windows.qcow2`, `vars.fd`, related images/evidence.
  `credentials.json` is a secret SDK binding: never print/cat it.
- `build/qemu-seamless-source/`: validated out-of-tree build and stamp. Source
  gitlink remains unchanged. `build/vendor/qemu-system-x86_64` currently contains
  the pinned build and `qemu_identity.json`; virtiofsd1.13.2 has its identity.
- Existing dev1/dev2 placeholders are unrelated and untouched.

**Do not run `make clean` or `make test-sanitizers` in this checkout.** The latter
cleans build output and risks deleting the preserved VM. Use isolated outputs
or a disposable checkout for any required clean build.

## Exact next steps

1. Read this checkpoint, tracker and IMPL_DESC; inspect `git status` and last
   commit. Check final pushed-head CI before changing anything. Verify runner
   process ownership afresh; do not infer that an old VM or runner is available.
2. Read `/home/dev/.agents/skills/workforce/SKILL.md`, existing work orders and
   validated phase allocation in `resume_work_orders.json`. User requested
   Luna/high workers and lead review. Reuse completed children or create bounded
   Luna workers with `fork_turns=none`; no recursive workers. Lead owns shared
   headers/build/workflow/docs/Git/native runner. Update exclusive file ownership
   before edits. The router/GPU/advisor were not invoked in this phase.
3. Review the three saved, **unapproved** next-slice proposals before assigning
   implementation. They are not frozen contracts and earn no credit:
   [slot pins/growth](resume_proposals/slot_runtime.md),
   [manifest journal](resume_proposals/manifest_journal.md),
   [canonical paths](resume_proposals/canonical_paths.md).
   Resolve the listed lifecycle/sequence/drain and symlink policy choices in
   IMPL_DESC first. Do not confuse legacy `src/av` with the new v3 session.
4. Finish the disposable native multi-mount smoke with correct daemon shutdown
   expectations. A blank disk can prove startup/QMP/backend attach only. Guest
   drive binding and readonly write denial require a booted guest with the
   appropriate agent/drivers and remain open. Source audio/USB/CUPS runtime
   acceptance also remains open.
5. Continue #5 pins/growth plus actual host/guest integration, #33 journal/lock/
   recovery engine, and #9 canonical/symlink paths in separate Luna lanes after
   lead-reviewed contracts. Plan remaining session skeletons #6/#7 around these
   dependencies. Keep #34 and every native matrix row honest; no mock/native-
   unrun substitution.
6. Commit atomic reviewed increments, update attribution immediately, and only
   open/merge the feature PR once every task genuinely reaches100% and required
   verification passes, per repository rules.

Safe focused commands from the repository (host access for sockets/LSan):

```sh
ZIG_GLOBAL_CACHE_DIR=/tmp/seamless_resume_zig_cache PYTHONDONTWRITEBYTECODE=1 make interop-test interop-sanitizers interop-coverage
PYTHONDONTWRITEBYTECODE=1 WADDLE_RECORDER_SANITIZERS=1 python3 -m unittest discover -s tests/av -p coverage_recorder_test.py -v
ZIG_GLOBAL_CACHE_DIR=/tmp/seamless_resume_zig_cache PYTHONDONTWRITEBYTECODE=1 make test
env -u PYTHONPATH -u PYTHONHOME PYTHONDONTWRITEBYTECODE=1 make qemu-source
make qemu-vendor-verify
```

Do not repeat all passed checks solely to fill time; rerun gates when relevant
source changes, a failure or unresolved concern justifies them. Checkpoint
contains no pending approval question and requires no new permission to resume
the already-authorized feature work.
