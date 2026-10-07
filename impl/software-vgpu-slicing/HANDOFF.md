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
  committed. `build/device_integration_checkpoint.md`. Channel4064193 has actualWinCPU acceptance; cappedabsoluteRPC source/tests
  in progress under0d6a58b, then separatelyadopted guest callback. Publiclargequery
  remains unleased; designunder build/large_extension_query_design/.
- image_pipeline: exact access/seam/physical helpers and safety makefile;
  `build/image_pipeline_checkpoint/HANDOFF.md`. Physical helper7486580 and exact
  named codec-count helper01d910c committed;14 embedded+6 linked sources. Current Properties2 stages: wire+2independentoracles; nativechain; private
  safetyhelper. Three namedCobjects include freshread-only renderoracle for full
  import tests, per8b69019. PublicICD/WSI/modernadvertisement remains unleased. Historical physical runner remains pinned to
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

01d910c pushed without rewriting history; cleanCI37631620496 Linux+Windows
success, immutable receipt6289b0a2... under root_todo3_continuation/ci_command28_complete.
This proves01d910c only; later source/realGPU/DXVK still pending. Prior
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

## Absolute transport and interrupted diagnostic checkpoint

Native channel4064193: local clock+1 origin, exact deadline with no renewal;
95.08%116/122 branches98.61%142/144 lines, stream97.62%41/42 and100%33/33;
strictnative/CSan3 zero, actualWin channel_until_882e15f8 native0/exact43c03 SHA
and balancedhandles. Separate cappedRPC work now owned by icd_memory.
Root owns only TCPclient/source/header prototypes/clienttest/wrapflag wiring;
final_frozen provenance d3dafbfb... under build/absolute_tcp_client_checkpoint,
EXE5e40dac6...,100%156/156 lines93.48%215/230 branches/zeroCSan3. ActualWinCPU
execution pending through wddm; no shared runtime/GPU build mutation.

Paused diagnostic tcp_gpu_diag_9871f859 failed shared output read/idle timing and
lost supervisor in-memory retirement proof. Exact failed Windows PID5748/image/
creation/SHA was force-terminated1464091718 under explicit authorization; no
acceptance, leak, abandonment or synthetic receipt. All artifacts stay. Before
next GPU launch require durable initial/intermediate/final SID/controller/starttime/
pidfd ownership journal, coherent nested finite budgets and owned cancellation
checks. Native shared-read FileStream must dispose and fail on read errors.
Independent system DXGI reproduces persistent OS handles but does not establish
exact actual GPU attribution or justify baseline inflation. Existing assertion stays.


## Implementation-first continuation: coherent mapping integrated

Preserve branch/history/VM/deployed snapshots and all prior artifacts. The user's
workflow override defers final sanitizer/coverage/stress until completed implementation
and basic integration. Root owns scatter protocol/build/qualification infrastructure;
`icd_memory` owns ICD/native fixtures/profiles/state; `image_pipeline` owns pure
rendering/image helpers; `wddm_dxvk` owns Windows fixtures and exclusive VM/GPU.
Hold `/tmp/waddle_git_index.lock` across the complete stage/commit operation.

Key commits: `83e5ebe` scatter backend, `11b8338` templates/library/mutable
ownership, `c839945` coherent mappings and cube views, `274af56` instance1.3,
`c6d3a89` required DXVK image-view usage. Guarded actual host-intersected API/feature
admission is currently undergoing owner checks. Root isolated draft resides under
`build/root_todo3_continuation/admission_compile`; never overwrite active core
from that snapshot. Regular/sanitized manifest ceilings1.3.0 preserve actual host policy.

Actual corrected coherent source93bed1d passed eight query/triangle lifetimes
`tcp_gpu_6eb88ac4` (native handles160 unchanged; allfour deployed hashes unchanged)
and four functional WSI frames/two lifetimes `tcp_gpu_fd643e37` (GDI7/USER5/native197
unchanged; final hashes pending). Controller/worker/native0, owned sessions empty,
no signals/observer errors. Failed noncoherent-only fixture `tcp_gpu_b5a68377`
remains preserved. Wddm reports GPU idle after WSI; recheck before any launch.

Native Windows TCP32-cycle `native_tcp_1a883c14` naturally exited0 after retained
initial observer timeout; no replacement/termination. DXVK fixture189ef2c now
requires exact4096 staged/client RGB pixels after Present plus64 HLSL compute words,
real device creation and teardown. Actual pinned DXVK2.7.1 remains pending; next
archive needs committed admission and matching backend/bootstrap/fixtures.

After basic integrated execution, run required final test/sanitizer/leak/coverage/
stress and real Windows/DXVK qualification. `vgpu-icd-sanitizers` reuses the current
integrated Debug/ASan hook runner with native C/fault checks, without the historical
fixed20-source117-unit LLVM reverse-proof audit. Old targets/artifacts remain.
New hook runner does not claim instrumentation of external link objects. No90%
coverage gate or ownership/acceptance assertion was weakened. Do not mark TODO #3
100% before final full48-frame WSI, timed-bootstrap leak and DXVK qualification pass.


### 2026-10-07 current qualification continuation

Same three owner agents remain active.516cf61 fixes actual DXVK timeline wait
panic with saved device namespace and nondispatchable semaphore lookup. Fresh
real pinned DXVK rerun is owned exclusively by wddm_dxvk. Runtime owner reorganizes
production declarations before the existing coverage test boundary so all new
APIs participate honestly in the unchanged90% core gate. Root scatter request,
bounds, receiver C coverage and sanitizer checks pass; root modern synchronization,
extra-object and requirements serializers coverage/sanitizers also pass. Root
logs are `build/root_todo3_continuation/*final*` and `*runtime*coverage.log`.
Image owner completes remaining helper/template/shader and render/instance gates,
then pure-module final sanitizers. No TODO#3 final acceptance or100% claim yet.
All Zig/C vGPU coverage attempts now have unique directories; historical flat
reports remain untouched. Shared-index lock and exclusive VM/GPU owner unchanged.


### 2026-10-07 integrated transport and qualification milestone

Keep the same branch, three owners, shared-index lock and exclusive Windows/GPU
coordinator. The trusted TCP hello is now version2; validated resource reads can
return65536 bytes while command replies and writes retain4096-byte limits.
Completion refreshes intersect allocation-owned conservative submitted resource
spans; initial maps still read actual complete mapped bytes. The controller owns
a262144-byte mapping containing two65536-byte payload rings, with consistent
allocation/attachment/unmap after proven worker retirement. Deadlines unchanged.

Actual frozen Windows DXVK c731d380 creates FL11/device/swapchain and completes
GetData, then throws during staging Map. Its controller/worker exit0 and owned
session is empty; this is a failed acceptance, with no pixel/compute/present
credit. New command reset/begin diagnostics de3920c and ring fix ed50df6 are
being deployed in a fresh immutable snapshot by wddm_dxvk. Preserve all failed
snapshots/logs. Timeline completion still cannot retire submitted ownership.

Whole-core coverage now includes every production declaration and honestly
fails its first complete run at55.28% branches/67.62% lines. Keep90% gates and
continue meaningful wrapper/state-machine tests; pure-module qualification
receipts are in build/image_pipeline_checkpoint/final_coverage_20261007.json
and final_sanitizers_20261007.json. Profiles/compute state full scopes pass.

The former cold bootstrap+9 handles are attributed to independent USER32/GDI
process initialization by exact external creation stacks and a system-only
LoadLibraryEx/FreeLibrary reproduction. Diagnostic24cycles retain exact111
system handles with project DLL absence; final fresh native bootstrap24 remains
pending. Final actual DXVK normal/sanitized, full48-frame WSI, controlled device
removal, external native DLL heap audit, core coverage and final sanitizer/stress
qualification remain open. TODO#3 remains55%; never substitute intermediate
functional progress for final qualification. README remains untouched.

### 2026-10-07 actual DXVK GPU readback milestone

Current branch/history/VM and all old artifacts remain preserved. Core production
at2e77d98 uses acknowledged native per-submission fences to retire GPU ownership,
never timeline-counter inference. Native command reuse and staging Map work.
Current821c0c7 includes276 D/RS/native128 passing ownership fixtures and optional
scalar descriptor-allocation diagnostics;6cb8be4 prints optional worker service
status. Shared-index lease and three exclusive source owners remain unchanged.

Actual eb202e00 returns4096 BGRA127,64,32,255 pixels for fractional0.5 blue.
Microsoft D3D11.3 section3.2.3.6 permits0.6 integer ULP, so127 and128 are both valid
at that midpoint. b60fef3 changes only clear input to32/255,64/255,128/255,1;
all4096 exact BGRA/RGB expectations, compute values, deadlines and cleanup stay.
Actual182afac7 passes4096 exact GPU pixels, then compute descriptor allocation77
ends in controller RingTimeout(-5), worker rawexit256. No compute/Present/teardown
credit. NativePID4296 remains alive and preserved after original supervisor210s
failure; signed Microsoft CDB non-invasive stacks show DXVK synchronization waiting
after its command thread exited. Host receiver/session already retired, so a new
isolated process can trace the next failure without resetting or killing oldstate.

wddm_dxvk owns the next immutable actual trace, with perprocess VIRGL_LOG_LEVEL=info,
unique upstream logs and existing scalar ICD/TCP diagnostics. Root/image/core keep
compilation and targeted functional tests moving. Whole-core90% and final expensive
sanitizer/stress/Windows/native heap acceptance stay deferred until basic compute,
presentation and teardown work. Do not mark TODO#3 complete prematurely.


### 2026-10-07 real DXVK compute and bounded presentation verification

Resume at971e58a on the existing branch. The last-opcode77 timeout was not an
allocation failure: shared live DXVK logs prove successful allocations followed
by unsupported VkBufferUsageFlags2CreateInfoKHR on a buffer view. 7c3f17e/3acd2cd
encode the bounded chain;86caaf6 validates effective texel usage against the fully
bound allocation and descriptor ownership. A subsequent actual Windows run found
a native stack overflow.79ac292 borrows existing mutex-owned registries rather
than copying them on the default DXVK thread stack. All282 D/RS/native128 and
Windows static checks pass; no new stack size or weakened ownership.

Actual db079fbb now passes4096 exact GPU pixels and64 exact compute words.
Present returned S_OK, but immediate client readback failed; shared DXVK logs
show no Present-path error.971e58a polls all4096 unchanged exact RGB comparisons
for at most the original10s to allow asynchronous DXVK presentation. WDDM owns
the fresh immutable actual run, all VM/GPU access and retained host receipts.
Image owner reviewed the readback/format/GDI path without a concrete defect.
Core owner prepares targeted private tests; expensive final gates remain held
until basic real presentation and teardown succeed. Root owns final bridge
sanitation, documentation and integrated gate coordination. Extra-object final
qualification must rerun for its changed production serializer.

Preserve old native PIDs4296/1404, VM/snapshots/artifacts and untracked files.
TODO#3 remains55%, overall68.75%; full90% core coverage, zero leaks, stress,
actual normal/sanitized DXVK, full WSI, device loss and native DLL heap audit
remain mandatory. Exclusive source ownership and shared-index locking unchanged.


### 2026-10-07 basic real Windows/DXVK passes; enter final qualification

Actual immutable81a46cb2 at e970799/snapshot20ed114b passes native0, real FL11
device/swapchain,4096 exact GPU BGRA pixels,64 exact compute words,4096 exact
client RGB pixels and normal COM/DLL teardown. Normal host resultstatus0, worker
exit0, retired1; no failure abandonment substitutes for this result.

The e591931e failed run confirmed image68/allocation69 alone blocked parent
teardown, all in-flight/idle/fence/ticket counts zero. e970799 adds explicit hidden
retired WSI image ownership and last-view-ACK retry, image55 before memory22 ACK.
Retired guest image and old-view descriptor/recorded-submit use are rejected.
9bc7de4 corrects effective variable count refund;0b8ecc8 proves pending ownership
through actual frontend submit/fence polling/queue idle without manual decrements.

Final qualification now runs: core required ICD tests/sanitizers/whole90% coverage/
Windows builds; image changed extra-object coverage/sanitizers; root fresh worker
and changed bridge sanitizer gate; WDDM sole VM/GPU owner for current normal and
sanitized DXVK, full48-frame WSI/eight lifetimes, native transport32/bootstrap24,
real device removal and external UMDH audit. Reuse valid unchanged receipts, fix
failures and rerun affected gates. TODO#3 now80% (implementation/basic acceptance),
overall75%; final20% remains open and Time Ended TBD. Preserve original branch,
old native PIDs, all snapshots/artifacts, untracked files and shared-index lease.
