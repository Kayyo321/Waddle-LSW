# Private clvk one-dimensional buffer admission review

Status: explicitly approved by the user in this session, implemented as tracked
private-copy patches0002/0003; final DLL/native/heap qualification is pending.
The approved quiescent-shutdown patch remains separate. No submodule or pin changed.

## Reproduction and actual failure

The first new-DLL native run `build/windows11_vm/dxvk_gpu_51ad9650` loads the
published receipt from `clvk_pe_cross_build.log`, selects NVIDIA RTX5080, and
executes exact integer add, local-barrier reduction, atomic increment and
advertised maximum-group kernels. It then fails the unchanged invalid-range
assertion. Native clvk logs show `clEnqueueReadBuffer(offset=4096,size=1)` on a
4096-byte logical buffer enqueues a host copy and returns success. It must reject
that range. No complete OpenCL lifetime receives acceptance credit. All logs,
source/DLL receipt, actual session retirement and failure owners are retained.

Pinned `submodules/clvk/src/api.cpp` read/write paths validate object identity and
host flags but do not check pointer, nonzero length or logical buffer range.
CopyBuffer also lacks range admission; MapBuffer uses `offset + size`, which can
wrap. A successful valid kernel workload does not establish malformed-input safety.

## Approved private-copy change

Add a separate tracked patch, applied only to the driver's private clvk copy,
with its digest included in build/publication admission. Keep upstream gitlinks
and working trees untouched. Scope is the four one-dimensional buffer APIs used
by the native fixture: ReadBuffer, WriteBuffer, CopyBuffer and MapBuffer.

Validate queue and memory objects before accessing their contexts. Require queue,
all buffers and wait-list events to share the same context. Preserve documented
invalid-object/event-list/context/host-access return categories; no command,
event, mapping, retain or GPU operation may occur on rejected input. Read/write
require a nonnull host pointer and nonzero length. All four APIs use subtraction
bounds: `offset <= logical_size && size != 0 && size <= logical_size - offset`.
Copy validates both logical spans and rejects overlapping regions in the same
buffer with CL_MEM_COPY_OVERLAP. Existing map flag/host access checks remain.
No rectangular/image support, new kernel capability, shutdown relaxation, manual
vendor frees or broader conformance claim is included.

## Required evidence

Compile the actual patched API bodies with controlled queue/buffer/event and
command-allocation boundaries under ASan/LSan/UBSan. Cover exact-end valid ranges,
zero length, offset past end, size past end, SIZE_MAX wraparound, null host pointer,
wrong-context buffer/events, invalid identity/list, host access flags and copy
alias overlap. Verify rejected calls do not allocate/enqueue/retain or alter
output-event sentinels. The previous implementation must fail the range regression.

Rebuild the full pinned private DLL and pass exact PE/export/import admission.
Repeat native valid outputs and every invalid-range assertion against the actual
DLL, then all eight context lifetimes/concurrent callers, explicit global shutdown,
module retirement, native heap checks and simultaneous host GPU workload. Keep
all existing thresholds, expected error codes, assertions and deadlines.


## Additional ownership defect within the approved map path

Independent source review confirms `cvk_enqueue_map_buffer` allocates its command
before `build`. A native map failure makes build return before queue ownership,
but the pinned helper returns CL_MAP_FAILURE without deleting that command.
The command owns queue/buffer holders and its event. The failure precedes mapping
retain/count acquisition, so deleting the command releases exactly its acquired
owners and does not unmap a nonexistent mapping. Patch0003 adds that destruction
only to this pre-enqueue failure; enqueue failures already destroy their commands.
The helper also serves buffer-backed image mapping, without adding image support.

The sanitizer fixture now extracts13 exact API/helper definitions from a newly
patched private source copy, including the actual map helper. Only CLVK objects,
command construction/build and enqueue execution are controlled doubles; the
C++ spelling at this boundary follows the pinned vendor ABI. Forced build failure
requires CL_MAP_FAILURE, null mapping, unchanged output event, one destructor,
zero enqueue and restored queue/buffer/event-owner counters. The original pinned
API fails the zero-size range assertion. Removing only the new delete fails the
ownership assertion and LeakSanitizer reports the expected command leak.

`python3 -B tests/vgpu/clvk_build.py` passes all19 tests with actual-body
ASan/LSan/UBSan execution (`clvk_buffer_map_admission_final.log`). The full
patch0001/0002 DLL cross-build passed exact121-export/import admission in
`clvk_buffer_cross_build.log`; it is not the final lifetime-corrected artifact.
The fresh all-three-patch build is separately retained in
`clvk_buffer_map_final_cross_build.log`. Neither CPU doubles nor DLL compilation
establish native GPU execution or zero native owned heap leaks.


Private buffer memory admission has its own exact production coverage gate:
`make vgpu-opencl-buffer-coverage`, also required by the OpenCL admission CI job.
The first measurement was152/161 lines (94.41%) and165/212 directional outcomes
(77.83%);97.17% merely reached a condition and did not meet the branch gate.
Added cases exercise host map access restrictions, nullable error outputs,
allocated invalid object identity/type, nonfirst foreign events, independent
logical/backing-span boundaries and valid controls without changing production.
The final gate counts161/161 lines (100%) and200/212 directional outcomes
(94.34%), including both untaken compiler throw edges with no exclusions.
All13 definitions execute and have100% individual line coverage. The generated
header contains exact freshly patched API/helper bytes; object/command boundaries
remain CPU doubles. Immutable input/tool hashes, raw gcov and commands are retained
under `build/clvk_buffer_coverage/run-20261018T010722656693Z-201377`.
All19 ASan/LSan/UBSan admission tests pass again in
`clvk_buffer_coverage_sanitizers.log`. Initial lower coverage and script diagnostics
are preserved; this is not actual Windows/DLL/GPU or native heap proof.
