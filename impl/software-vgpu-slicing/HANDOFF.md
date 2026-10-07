# TODO #3 continuation handoff — 2026-10-07

## Objective and rules

Continue TODO#3 through verified100%, including real Windows GPU execution and
pinned DXVK2.7.1 device/swapchain acceptance. Read README.md, CONTRIBUTING.md,
PROJECT.md and AGENTS.md, then this file, IMPL_DESC.md, TRACKER.md and the original
guide at `/home/dev/.codex/attachments/7dbe1ccb-ee0f-4398-b1e0-bdb47ba308a2/Pasted text.txt`.
README stays untouched. Preserve every commit, dirty file, VM and test artifact;
atomic detailed commits,≥90% protocol/memory coverage and zero prescribed-checker
leaks remain required. Compress documentation without rewriting Git history.

Branch `feature/software-vgpu-slicing`; target `origin`. TODO#3=55%, overall68.75%,
#10=100%, #11=60%. TODO#5 OpenCL remains outside this request. Original milestone
accounting forbids duplicate standalone-adapter credit. Do not merge the feature
while its other tasks remain incomplete. Public ICD still Vulkan1.0, empty
extensions/all optional feature flags false; codecs/mocks do not justify lifting
that ceiling. DXVK requires faithful modern API/feature/query/runtime/WSI work.

## Roles and durable checkpoints

Resume or recreate exactly `icd_memory`, `image_pipeline`, `wddm_dxvk`. Existing
agents can be absent from list_agents yet still resumable with followup_task.
All share this checkout and Git index. Hold `/tmp/waddle_git_index.lock` across
staging AND commit, require initially empty index and exact owned staged paths.
Do not stage other roles' changes. `/tmp` vanished during the latest interruption;
use durable ignored build checkpoints. Root helper:
`build/root_todo3_continuation/commit_owned.py`.

- Root: docs/tracker/workflow/GNU and standalone device/extension gates;
  `build/root_todo3_continuation/checkpoint.md`.
- icd_memory: pending public ICD/C fixture integration, native/device codecs and
  narrowly negotiated coverage/make wiring; `build/device_integration_checkpoint.md`.
  Pending source hashes7f78e3ce.../16cafa22..., worker fixturea68383c9... unchanged.
- image_pipeline: exact owned access/seam/physical helpers and safety makefile;
  `build/image_pipeline_checkpoint/HANDOFF.md` and recovery_freeze.json. Pending
  physical helper/make changes, candidate5927068b...; exact14 embedded+6 linked
  sources. Root GNU/vgpu.mk frozen after331c68b.
- wddm_dxvk: TCP/Windows bootstrap, TCP contracts/tests and pending
  `tests/vgpu/tcp_gpu_windows.c`. Private supervisors remain in
  `build/windows11_vm/tcp_gpu_actual.py` and tcp_gpu_owned_session.py; prepared
  normal/safety snapshots tcp_gpu_39c4c740/c9f3939f have checkpoint.json.
  Narrow future tracked acceptance/session helper lease granted after real gates.

## Verified checkpoint and immediate sequence

Device request integration validates before reservation, encodes only owned
values and publishes enabled state only after exact ACK.117 Debug units/native
128 cycles/mapping/loader fixtures pass; production branch90.03%2095/2327 and
line98.98%2433/2458. Actual Windows117/static128/DLL/medium pinned-loader8lifetimes
plus128dispatch all exit0, exact6deployedSHA:
`build/windows11_vm/device_integration_8289e5d9`. These use a mock frontend.
Four normal and four C-ASan/LSan/UBSan direct/shared triangle/compute RTX5080 paths
pass. Their ICD/codecs are ReleaseSafe; full owned instrumentation is separate.
Frozen29source/runtime originals and copies:
`build/device_integration_runtime_checkpoint/provenance.json`.

Complete20 native/test seam passes with414/416linked definitions,
6617/7605original guards and4672/4680actual final accesses;117ICD+25nested allocator
units and6native codec suites are clean. Unique immutable completion receipts
under build/icd_complete_seam_safety_20; historical18 artifacts retained.
Interrupted physical run032548774271Z-1989791 reached its first positive case
but records ZERO accepted cases. It is incomplete, never aggregate acceptance.
All353 retained inputs still match after recovery. Image owns an exclusive
GPU/source/header/runtime/make/helper freeze while a fresh same-make20seam plus
full15physical matrix runs:9positive cases/144lifetimes and6actual shared-only
postflush failures,6real process-owner tests, exact session/reap/hash proof.

Next: image finishes that whole gate and commits its physical helper/make;
icd_memory commits the exact ICD/C fixture integration after release. All such
commits remain+0 until required guide milestones are audited. GPU then goes to
wddm for actual authenticated Windows→Linux triangle/compute and failure paths,
using copied coherent artifacts and natural whole-owned-session retirement.
Root must not mutate current sources/runtime until those copy/freeze boundaries.
Current-head complete CI remains required: prior clean Linux/Windows run37557528193
passed atc2faa4c; newer local commits are not covered by it. Linux parent budget60min;
180s proof children/256MiB text,300s physical case and1s fence deadlines unchanged.

## VM and real Windows acceptance

Existing Windows11Pro10.0.26300.9457 VM:80G qcow2, persistent UEFI vars.fd and TPM,
KVM4CPU8G. Interruption stopped qemu/swtpm; restart EXISTING disk/firmware/TPM only.
Never run provision.py: it resets firmware/disk and boots unattended media.
Reuse QMP qmp.sock/VNC19 and WinRM127.0.0.1:55985; artifact HTTP55986 serves only
repair_media. Credentials JSON600/private directories700; tokens only encrypted
WinRM/stdin or private owner-DACL storage, never argv/public HTTP/logs/commits.
Keyboard/QMP and GPU leases are explicit; preserve guest settings/drivers.

Supervisor must prove exact authenticated session, observed worker PID/group,
controller wait/reap and entire dedicated owned SID empty before retirement
receipt. Use exact starttime/pidfds and subreaper ownership. Positive acceptance
requires natural whole-graph exit and actual GPU byte/pixel comparisons. Finite
owned-only TERM/KILL cancellation is failure evidence, never success/leak credit.
Lost proof retains callbacks/modules. Released bootstrap/loader/ICD must be absent.
Physical hypervisor/full KMD tests remain delegated as previously documented;
real userland ICD/GPU/DXVK acceptance is still required.

## Next bounded reply prerequisite

Contracts57dedad/34da0be specify cooperative4KiB byte-range reads of ONE completed
reply under unchanged TCPv1, not record pagination. Future CommandReading=4 and
owned cursor preserve partial-data privacy/retries/sticky loss. Measured x64 owner
would be80/8 (old72):cursor48/fence56/ID64/state68/lost72; earlier88assumption was
rejected by independent C assertions. Root's9unit `/tmp` prototype passed before
interruption but was lost; reconstruct/reverify durably before integration.
Root has only the future narrow ICD unbind Reading guard lease after source
commit+image15+Windowscopiedfreeze; all header/library consumers must rebuild.
274460-byte extension reply requires68chunks. Current service allocation65536 is
NOT reported in capset160. Explicit trusted actual524288 allocation/mapping proof
is a separate prerequisite. Current1000poll/sleep loop is an iteration bound,
not a whole elapsed deadline; future large-query integration must first specify
monotonic deadline and remaining callback budget. No large query or advertisement
is currently implemented.
