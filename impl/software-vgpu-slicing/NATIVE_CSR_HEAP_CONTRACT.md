# Native CSR heap qualification unit (scratch proposal)

- Repository HEAD: `7dfd51642d1ade16c48020ae422f6778ed2d02c9`; scratch sources are under `/tmp/waddle_native_csr_unit`. Repository tree was clean.
- Scope: bounded read-only `CsrPortHeap` inventory at existing audit baseline and each `unloaded_N` boundary, complementary to the full DPH walk.
- The proposed fixture patch adds one include and replaces only baseline/unloaded stage calls in the DXVK and ordinary OpenCL audit loops. OpenCL `--runtime-reload` retains its existing stage callback; unaudited execution never calls the helper.
- The helper binds loaded System32 `ntdll.dll` path and trusted runner digest, reads `CsrPortHeap` at image base + `0x1d5e68`, verifies unique active process-heap membership, then locks and fully walks with limits 256 heaps / 4096 walk entries / 1024 busy rows. Every failure after lock unlocks; all output and stage waits are outside the lock. Acceptance requires zero busy rows.
- The Windows-only control dynamically calls `CsrAllocateCaptureBuffer(0,12345)` and `CsrFreeCaptureBuffer`, requiring the exact pointer and 12380-byte heap row while held and a zero-row snapshot after free. It cross-compiles but has not run.

## Validation

- Plain and optimized API-double tests: 5 groups / 68 checks each, pass.
- Actual helper coverage: 100% lines (102/102), 93.55% directional branch outcomes (116/124), no exclusions.
- ASan/UBSan pass with leak detection disabled. Combined LSan is blocked by the environment's ptrace restriction; the raw failure is preserved.
- Strict x64 MinGW cross-compilation and fixture patch dry-run pass. No native execution or acceptance credit.

The exact contract, five source/patch hashes, command outputs, preserved sanitizer logs and target source hashes are in `receipt.json`.

## Root integration and native calibration

The proposed unit is integrated in the two audit-only fixture loops. Root ran
its exact source under ASan/LSan/UBSan with leak detection enabled:5 groups,
68 checks, zero diagnostics. Both Python modes of the actual-helper coverage
gate pass102/102lines and116/124directional branches (93.55%), all5functions,
no exclusions. Make repeats the portable/safety/coverage gates and separately
cross-builds the Windows control; CI now requires these portable gates.

Root strict private GCC14Win32 cross-build records211 preprocessing inputs
before and after compilation in
`build/qualification_feature_20261021/native_csr_root/native_csr_private_receipt.json`.
The native helper control `csr_helper_native_64e4b891` passes0, naturally,
with no remaining process and stable pre/post System32 ntdll/kernelbase hashes.
It observes the exact12380-byte CsrAllocateCaptureBuffer row, calls the paired
CsrFreeCaptureBuffer, then requires zero user-busy rows twice. Root independently
reconciles the records in `native_csr_root/native_root_review.json`.

The separate three-stage debugger control `pageheap_negative_5534258c`
corroborates HeapWalk against the CSR debugger table while that allocation is
held and after release. Heap metadata/free-capacity growth is recorded as such;
it is not a live user allocation. The debugger warning about
RtlpHeapInvalidBadAddress remains preserved. The complete DPH walk, API-heap
mapping, calibrated retained-owner controls and fresh full candidate run are
still required. This unit grants no candidate heap acceptance or progress.
