# Presented Worker Native WSI Link Input

## Scope and failure

The Linux software-vGPU CI gate runs `make vgpu-presented-worker-test
vgpu-presented-worker-sanitizers`. The production ICD object now imports
`venus_win32_present_pixels_exact` from its WSI implementation. The worker
acceptance executable linked the ICD without the production native sink object,
so the executable failed to link before it could spawn its independent worker.

## Architecture and exact correction

Add the existing `build/venus_win32_present.o` to the shared
`VgpuPresentedWorkerObjects` list in `scripts/vgpu.mk`. Its existing rule tracks
`src/vgpu/venus_win32_present.c` and `include/waddle/venus_win32_present.h` and
compiles the same production native boundary used by `VgpuIcdObjects`.
`tests/vgpu/worker_presented.c` has no mock Win32 sink to update. No synthetic
implementation is introduced. The direct worker executable, loader worker
executable, sanitizer versions, named image/compute/triangle workloads, and
remote-image executable all inherit the same required link input.

## ABI, memory and ownership

No ABI or wire-format change occurs. The linked object defines the extent,
scaled-pixel and exact-pixel functions using their existing public C signatures.
The native sink borrows window and pixel storage and retains no resources. On
Linux it preserves the documented unsupported return behavior. On Windows the
existing implementation owns a DC only within each synchronous call and checks
exact client extent before and after acquiring that DC. This correction neither
changes that implementation nor establishes Windows presentation acceptance.

## Execution, concurrency and errors

Make builds the native object through its existing prerequisites and passes it
once to each affected executable link. The independent production worker,
protocol negotiation, frame-FD channel isolation, invalid-frame rejection,
unknown-release shutdown, descriptor-baseline checks, 5-second channel/guest
limits and 300-second test watchdog remain unchanged. No test is skipped and no
sanitizer, deadline or assertion is relaxed. A link, worker spawn, renderer,
protocol or ownership failure continues to fail its existing gate.

## Verification criteria

1. Reproduce the prior unresolved native-sink reference with the original link
   inputs, then link the actual test executable with the corrected shared list.
2. Inspect every affected make recipe for exactly one native-sink object and
   verify executable symbols have no unresolved or duplicate native sink.
3. Run the exact CI command with an actual built renderer/worker when available.
   Record prerequisite blockers separately from executable-link success; do not
   turn missing renderer or Vulkan execution into a pass.
4. Preserve ASan/LSan/UBSan options and exercise the production sink's existing
   portable native-boundary tests. Request independent review before committing.

## Boundaries

No README, dependencies, production ABI, tests, thresholds, hardware acceptance
claims or Windows runtime acceptance claims change. Verification receipts belong
in `TRACKER.md`; source-only or link-only evidence never substitutes for the
independent production-worker execution proof.
