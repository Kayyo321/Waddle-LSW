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
- icd_memory: public ICD/native/device ownership; device integrationd4dc30b is
  committed. `build/device_integration_checkpoint.md`. Current read-only next
  assignment: exact large-query/timed-callback/actual-allocation design.
- image_pipeline: exact access/seam/physical helpers and safety makefile;
  `build/image_pipeline_checkpoint/HANDOFF.md`. Physical helper7486580 and exact
  named codec-count helper01d910c committed;14 embedded+6 linked sources. Current
  read-only Properties2 proposal. Historical physical runner remains pinned to
  d4dc30b/25-unit graph and rejects the new b06be83/28 graph; new physical proof
  needs a coherent separately frozen runtime, not reused historical acceptance.
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

Historical current20 physical acceptance7486580 is complete:9positive/144 GPU
lifetimes and6actual shared postflush failures,6process-owner tests, all353input
hashes/15logs exact, zeroCSan3. Completion90c62445..., same-make seam aabc76dd...
and recoverylog b09cbdb3... under build/icd_physical_owned_safety_20 and complete
seam20. Old run032548774271Z-1989791 remains incomplete/ZERO accepted cases.
Device integrationd4dc30b and this tool acceptance are attributed+0 in8d68f35.

Rootb06be83 adds the80-byte cursor owner/Reading phase and narrow ICD unbind
regression, not a large query. Command9D/RS/nativeD/RS/actualWin9D/9RS/native
all0; coverage93.59%73/78/100%90/90/22guards; genuine9defs/79accesses native/test
901/1054guards zeroCSan3/allocator leaks. Actual Windows receipt:
`build/windows11_vm/command_chunks_479502b2/provenance.json`, SHA bac21f81...
Current ICD117/native128/shared-loader0, coverage90.03%2095/2327 and98.98%
2433/2458/853guards separate. Fresh complete20 seam command28 at
run134222497263Z-120724/completion.json SHA3896e748... verifies414/416defs,
6617/7644guards,4686/4694actual accesses,117ICD+28 named codec units and147
artifact hashes. Helper01d910c enforces exact3/9/4/5/3/4 counts in both final
reports. Root durable freeze/evidence under build/root_todo3_continuation/command_chunks.

Wddm copied all29 oldruntime items,57 bridge source inputs and prior20 proof
receipts; its prepared12-case Windows matrix is isolated from new ABI builds.
Existing8 repaired Mesa manifests/2DSOs must remain read-only. Wddm holds exclusive
GPU/QMP keyboard lease. First actualWin triangle tcp_gpu_39c4c740 reached exact
RTX5080 pixels and natural controller/worker0/reaped SIDempty, but guest failed
native handle baseline97→157; NO acceptance/progress credit. All failed artifacts
retained. Diagnostic-only byte-identical-runtime rerun traces native handle
owners; original baseline assertion stays. Root CPUWinRM lease completed/released.

01d910c pushed without rewriting history; full new-head CI is pending. Prior
clean Linux/Windows37557528193 passed atc2faa4c, not new ABI acceptance. Linux
parent60min;180s proof children/256MiB text,300s physical cases and1s fences
unchanged. TODO3=55%, overall68.75%, #11=60%; no duplicate milestone credit.

## VM and real Windows acceptance

Existing Windows11Pro10.0.26300.9457 VM:80G qcow2, persistent UEFI vars.fd and TPM,
KVM4CPU8G. Existing disk/firmware/TPM recovered; VM is running/unlocked.
On a later interruption restart only those preserved artifacts. Never run provision.py: it resets firmware/disk and boots unattended media.
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

Contracts57dedad/34da0be and implementedb06be83 use one≤4096-byte read per poll
from ONE completed reply under unchanged TCPv1, not record pagination. Reading=4
owns a validated-byte cursor and accepted CPU fence until complete identity check;
partial take is NULL/zero/Again, retry keeps offset, terminal failure sticky.
Measured x64 ABI80/8:cursor48/fence56/ID64/state68/lost72; all consumers rebuild.
274460 bytes needs68chunks,16MiB4096; actual9units include maximum/canaries/faults.
ICD rx4096/service allocation65536 are unchanged. Capset160 does NOT report the
actual reply resource. Before full query, specify/verify trusted requested524288
power-of-two actual mapping, its configuration/compatibility/error boundaries.
Current1000poll/sleep loop is an iteration bound, not an elapsed deadline; next
large-query design must provide monotonic whole-query time and remaining timed
callback budgets before production code. Public extension cache/query, modern
API/features/Properties2/WSI and pinned DXVKdevice/swapchain remain pending.
