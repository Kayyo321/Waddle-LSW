# Exact-target FLS exhaustion certificate

This qualification helper proves FLS index saturation and retirement of its own
temporary indices on one admitted Windows target. It contributes **+0%** until
native calibration, the final full-DLL reload run, independent review and CI pass.
Portable tests and cross-compilation provide no native acceptance.

Admission uses the exact System32 image hashes, module paths and environment token
specified by [the TLS certificate](OPENCL_SLOT_CERTIFICATE.md), with AMD64 PEB
version **10.0.26300**. The DLL file versions remain 26100 and do not identify the
PEB build. The trusted native runner must independently bind image hashes,
CodeView/PDB identities and actual OS build before and after execution. The token
alone is not authentication; unsupported targets fail closed.

## Layout and ownership

The exact admitted ntdll's `RtlpFlsContext` is at module RVA `0x1ceeb0`. Its eight
group pointers start at context `+8`; group `g` has `16 << g` records and occupies
`8 + 24 * capacity` bytes. The first DWORD is the occupied count, **including
reserved index 0**. Record `j` has its marker at group `+0x10 + 24*j`, mapping to
public index `(16 << g) + j - 16`. The complete domain is 0–4079; only 1–4079 may
be returned to this probe.

All reads use bounded own-process `ReadProcessMemory`, with checked arithmetic,
canonical aligned ranges and nonoverlapping groups. Raw pointers are never
unchecked C dereferences. Invalid counts, transient `-2` markers, short reads and
malformed graphs reject. Baseline group 0 and its reserved `UINT64_MAX` marker
must already exist; the helper performs no initialization warmup.

The sole quiescent caller records each unique, baseline-free returned index in
fixed storage. Error 8 qualifies only when all eight groups and all 4080 markers
are occupied, counters agree, preexisting markers remain exact, and every new
marker is `UINT64_MAX` for one recorded owner. Early OOM or error 8 alone never
passes. The helper frees one owned index, proves the exact hole, reacquires that
same index, and requires another immediate error 8 with the exact full state.

Every known temporary owner is reverse-freed on subsequent exit paths. Final
markers and counts must equal the baseline. Newly allocated empty groups may
remain as OS metadata, with their before/after graphs recorded; no public owner
is waived. Existing groups and newly observed full-state groups cannot move.
Ambiguous returned IDs are never borrowed-freed. Failed retirement remains fatal,
and the caller checks that flag independently of the diagnostic status before
any candidate unloading or reuse.

One certificate occupies 181,472 bytes. The native four-case control and the
serial reload orchestrator use exclusive fixed storage to avoid large stack
allocations. This proves index availability and probe cleanup, not arbitrary
FLS callback/value cleanup or candidate heap ownership. Heap and first-load
qualification remain separate requirements.

## Gates and recorded portable evidence

The Make targets are:

- `vgpu-opencl-fls-test`: actual helper under portable API doubles.
- `vgpu-opencl-fls-sanitizers`: AddressSanitizer, LeakSanitizer and UBSan, with leak
  detection and fatal error handling enabled.
- `vgpu-opencl-fls-coverage`: normal and optimized Python gates, at least 90% of
  actual helper lines and directional branch outcomes, with no exclusions.
- `vgpu-opencl-fls-native-build`: strict Windows standalone control cross-build;
  it does not execute Windows or confer qualification.

The Software vGPU OpenCL admission job runs the three portable gates. The full
OpenCL fixture explicitly depends on the FLS include so incremental builds
rebuild it when the certificate changes. Existing TLS and reload gates remain
required.

Astra's frozen implementation receipt is
`/tmp/waddle_fls_certificate/ready/receipt.json`, SHA256
`1c3ebc7f8ff601281c89c3ab6b2c1408f53de8271d36b5760d9f92181d461de8`.
Its 270 cases perform 7,756,634 checks, including duplicate/foreign tokens,
malformed graphs, early errors, failed reads and frees, and a moved group whose
markers and counts remain identical. O0/O2, UBSan, both coverage modes and strict
Windows cross-builds pass. All six FLS functions execute: **105/105 lines and
158/158 branch outcomes**. The shared TLS helper has its own complete gate.

The worker's combined sanitizer run remains **FAILED (environment)** because
LSan explicitly rejects ptrace execution; its exact binary and failure log are
preserved for the root rerun with leak detection enabled. Native FLS control
execution is **NOT RUN in this portable receipt**. Required native controls are
full saturation, rejection of forced early error 8 with cleanup, an actual held
FLS index reducing available count by exactly one, and explicit release restoring
the original count and markers. They emit all five raw snapshots and returned
IDs. Final 1100-cycle full-DLL qualification and all previous failed evidence
retain their separate requirements and original meanings.

## Root native calibration and final-source controls

Root independently verified all114 frozen FLS source/evidence bindings and57
build-unit bindings. The exact frozen sanitizer binary and the integrated
source both pass ASan/LSan/UBSan with leak detection enabled; all270cases and
7,756,634checks complete with zero owned indices. The integrated Make gates
also pass, including full105/105line,158/158directional-branch coverage and
strict Windows cross-build. No helper function or branch is excluded.

Private GCC14Win32 standalone build binds280 preprocessing inputs before and
after compilation in
`build/qualification_feature_20261021/fls_certificate_root/fls_private_receipt.json`.
The executable SHA256 is
`0c978a78fe9f800342619583b65503de0d397765898247606d652999076c30c2`.
Actual root native run `fls_certificate_native_ad56487c` passes all four measured
certificates in one native process, plus two separate unsupported-admission
process controls. Available public slots are4078→4077→4078 while one real index
is held/released. The early-error8 control acquires/frees2indices and rejects
incomplete saturation. Full/hole/refill occupancy is4080/4079/4080; every final
marker/count vector equals its own entry. All three processes naturally exit0,
with no forced cleanup or survivors and stable admitted OS hashes/build.

Independent offline review rehashes all280 compiler inputs and reconstructs
every unique token, marker, counter and identity transition from raw records.
Its report is preserved in
`fls_certificate_root/independent_native_review/review.json`, SHA256
`308b70cc36ad29025a467455e2362e2ecb5f26b9a4cd89c122cf914b17639177`.
It finds no defect and grants only certificate-control scope.

The fresh complete all-mode OpenCL fixture binds300inputs across two separately
captured translation units. Its SHA256 is
`4979abb2d17f9e068a98e6049fc22fd1e4513d0674ef537744b4fe00bc0abc90`;
build receipt `opencl_cancel_fixture_1bd5b1a9/fixture_build_receipt.json`.
Native `opencl_reload_controls_native_979c16cb` passes both exact TLS/FLS
retained-index controls and ten malformed-command exit2 regressions, with no
candidate DLL/GPU loaded. The prior Windows command-length failure occurs
before launch and is preserved with no-child proof in `05735b84`; revised
transport compresses only the inline script and changes no execution policy.

Full1100reload/3300context stress, simultaneous Linux GPU work, first-load
candidate heap verification, final review and exact final CI remain required.
This certificate/control unit alone increases no feature progress.
