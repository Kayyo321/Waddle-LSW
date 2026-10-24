# Seamless interop night checkpoint

Stopped at the user's request: 2026-10-24T23:51:22.656116+00:00. Branch `feat/seamless-interop`,
merge target `origin`. Implementation is saved through `0965410`; the final
checkpoint commit is this file's Git commit. All Luna ownership is released.
No agent, QEMU, swtpm, daemon or native acceptance run was left running by this
chat. No automation was scheduled. No PR or merge was performed.

## Exact accepted and pending state

- Tracker remains **18.00%** (#7=30%, #10=35%, #34=0%). Native, coordinator and
  actual session-entry requirements remain open. Do not mark100% or merge.
- `89ff058` saves O5 observer preservation, the final Failed-state mutation fix,
  native fixture migration and all local qualification. Full command:
  `make -j1 -f scripts/interop_window_graph.mk window-graph-all WgBuild=/tmp/seamless_o5_worker_gate`.
  Exit0: graph148 tests, imported ABI164, C ABI28, observer89 plus graph-fault
  fixture; exact22 graph exports; ASan/LSan/UBSan with detect_leaks=1; Zig and
  MinGW Windows cross-build/link. Exact observer production1188/1245 lines
  (95.42%),851/944 taken branch arcs (90.15%), no observer exclusions. Evidence:
  [qualification](o5_evidence_2026_10_24/qualification.json).
- Updated O5 Windows binaries were **not executed locally**. First resume step
  is inspecting CI for the final pushed checkpoint SHA, especially real Windows
  adapter smoke and the preedit/own-window checks. Local cross-build is not
  Windows runtime acceptance. New failures require repair and requalification.
- Pre-O5 source `e4d42de` has all three workflows green: interop40143405298,
  graph40143405306, Native AV40143405357, all Linux/Windows jobs successful.
  [Immutable metadata](ci_qualification_e4d42de.json). These runs do not qualify
  `89ff058` or the fresh guest prototype. Failed older CI40142666161 exposed
  fixture dependencies on swtpm/build paths; `e4d42de` fixed them, and34 runner
  tests pass in a source-only tree with PATH=/nonexistent.
- `0965410` saves the **prepare-only prototype**,15 passing unit tests and the
  real retained KeyError. Read [fresh_native_guest.md](fresh_native_guest.md)
  before using it. No fresh VM was launched. The test validator supplies a path
  field absent from the real QEMU provenance result. Fix the validated-path
  boundary and add a real-shape regression before retrying preparation.

## Resume order

1. Check clean worktree/current branch and latest GitHub runs. Review actual
   updated Windows execution, retain exact logs, and resolve failures. Award
   tracker credit only against the existing acceptance allocations and evidence;
   the O5 coordinator/installed-session gates are still separate.
2. In a bounded Luna lane repair the fresh preparation result-shape bug, rerun
   source-only unit tests and review the real preparation receipt. Do not dump
   any generated secret, answer XML or answer ISO. Keep its media and generated
   files private. Do not overwrite/reuse a failed run without a recovery plan.
3. Root must review a bounded install supervisor before launching the new guest:
   exact child handles, KVM/VSOCK descriptor ownership,30-minute install deadline,
   QMP screenshots and ACPI shutdown, explicit forced cleanup, ordinary dev UID,
   and retained failure state. No host exports/network/USB/WinRM are needed.
   Native install must demonstrate driver startup, port5242 and a real guest
   command; then use the interactive standard test user's listener for actual
   native fixtures. A prepared file or booted lock screen is not acceptance.
4. Freeze and implement the actual seamless coordinator/runtime. The legacy
   `src/av/av_guest.c` V1–V3 path is explicitly out of scope (IMPL_DESC §1.3).
   No `waddle-session` or `waddle-guest-session.exe` product entrypoint is wired
   yet. O5 alone must not be described as full runtime integration.

## Coordinator facts to preserve

Exactly256 ordered staged graph events; sole next_event consumer; freeze
admissions while staged work/Destroy barrier remains. Announce/Update require
exact generation checks, fresh native validation and a post-query Destroy
barrier before publication/capture. Stale suppression is not release proof.
The proposed fresh-facts API in `/tmp/seamless_graph_fresh_facts_contract.md`
is **not frozen or approved**: automatic rejection of unknown process creation
time would change protected/UAC capabilities, and suppressing an old Announce
then publishing Update violates lifecycle ordering. A blocked Announce with
admissions frozen also needs a complete progress policy. Do not implement the
proposal without resolving these issues. Existing graph event values do not
carry full identity, so check_action alone does not validate current native
facts. Real generation capture join and actual host slot/pin release accounting
are missing; never manufacture capture_stopped/pins_drained flags. Protocol2
has no host-drain receipt. Retain/quarantine old graph/backing absent proof.

## Retained native state and access

- Preserved Windows11 base: `build/windows11_vm/windows.qcow2` (about37GiB),
  `vars.fd`, `tpm/`. **Never run make clean, make test-sanitizers, git clean on
  build/, or the old provisioner.** Never read existing credentials.json,
  legacy seed/unattend files, or their answer ISO contents.
- The old private boot is `/tmp/waddle-native-guest-5gwbb6t4`; the latest VSOCK
  boot is `/tmp/waddle-native-guest-8_69xbse`. Both stopped; no base read lease
  remains. Latest evidence is committed under
  `native_vsock_evidence_2026_10_24/`. Windows reached lock screen; all93 port5242
  probes failed to connect. No guest command ran. ACPI shutdown was not forced;
  original base/UEFI/TPM unchanged. Do not infer driver absence from these resets.
- Elevated inspection shows usable `/dev/vsock` and `/dev/kvm` as dev.
  `/dev/vhost-vsock` exists (10:241) but ordinary open is EACCES. A fixed sudo
  helper can open just that device and authenticate an SCM_RIGHTS handoff; the
  runner/QEMU stay UID1001. No ACL/node/module/policy change was made. The exact
  helper and completed run are in the committed native evidence. Sandbox-only
  missing-node observations are not host facts. Root must retain descriptor
  ownership and pass only the validated duplicate to QEMU.
- Private registered profile: `/tmp/waddle_guestexec_device_20261024`. It was
  created but not booted by the managed daemon. Its isolated CID3 is not cleared
  for use. Normal dev1/dev2 profiles were not modified.
- Fresh preparation failure: `build/fresh_native_guests/fresh-0582e3b441114ef5980dbacb27af3a7c/`.
  It contains a private blank disk, vars/TPM directory, answer media, newly
  generated account secrets and non-secret failure manifest. Keep mode0700/0600;
  never stage/copy those secrets into Git. No QEMU/swtpm was started. Proposed
  CID52943 has not been used by this new guest; check live inventory before use.
- Cached WIM: `build/fresh_native_media_cache/install.wim`, extracted only from
  the pinned Server2025 evaluation ISO. Real image index2 `/IMAGE/NAME` is
  `Windows Server 2025 SERVERSTANDARD`; the human Desktop Experience label is
  a DisplayName, not the install selector. VirtIO media uses2k25/amd64 Viosock.
- Build/tests/evidence under `/tmp/seamless_o5_worker_gate` and
  `build/coverage/interop/window_observer/run-*` are retained. The interrupted
  pre-review full gate is not qualification; the final reviewed full gate is.

## Working rules

Keep atomic detailed commits and tracker attribution, README untouched. Use
Luna in parallel with disjoint file ownership and root's critical review.
Do not invoke the subagent skill. Never repeat the legacy seed credential read;
a worker accidentally encountered one credential-bearing line and stopped that
path without copying/reusing the value. Generated test credentials belong only
to the new disposable guest. All work is on the feature branch; merge only after
all tasks actually reach100% and exact CI/native gates pass.
