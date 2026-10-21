# Exact-target TLS Exhaustion Certificate

The payload is four new test/support files plus one additive scratch patch for the existing reload support include. No tracked source, main/worktree file, Git state, native process, VM, GPU, network endpoint or historical evidence was modified. Root must review and integrate the patch, add the helper as a fixture build dependency, and run native calibration. FLS is unchanged and still rejects error8; this unit cannot qualify the full reload candidate.

The corrected evidence is `/tmp/waddle_tls_fls_contract_root_20261021.md`, SHA256 `8847b78d4f35d6a45944de3a5ca47a6508bbf1b3bfc3a6978397c617ae1c86f4`. Independent matching-PDB and exact-image disassembly dumps are preserved under evidence/. The superseded report's embedded-descriptor assumption is not used. PEB+0x78/+0x238 contain pointers to16-byte RTL_BITMAP descriptors. Their sizes must be64/1024 and buffers must identify PEB+0x80/+0x240. Inline words occupy8/128 bytes. TEB+0x60 supplies PEB; TEB+0x1780 supplies the expansion array pointer. NtCurrentTeb supplies only the current-thread root identity. All following process-memory reads are exact, bounded ReadProcessMemory(GetCurrentProcess()) calls; there is no unchecked C dereference of the copied pointers, write to target memory, opened handle, heap allocation or candidate warmup.

Admission requires the exact WADDLE_OPENCL_SLOT_TARGET value:

ntdll=651ac9041af8e6b5ebd331d53163244c0a27d36d1433f0f15cac1f28c5f2e82c;kernelbase=5c29b73622ce0976e1ada566f31f2197ba0f8beaea89c3ec033366126172c3d0

Absent, truncated or mismatched admission rejects before any TLS allocation. The helper additionally requires borrowed ntdll/kernelbase module paths to equal the actual System32 directory paths, and PEB version10.0.26300. This environment value is not authentication. The trusted root runner must independently hash the actual guest System32 files, verify the reviewed CodeView GUID/ages and versions before and after execution, and freeze those results with fixture/source hashes and the PDB/layout evidence. Another OS image, missing external admission proof, or another architecture is unsupported. The compile-time architecture check rejects non-AMD64. No error259 fallback exists for native TLS in the patch.

The serialized, quiescent caller owns a fixed1088-entry token array and34-word owner bitmap. Every successful TlsAlloc return must be in0..1087, absent from the baseline bitmap and not already owned; only then is it recorded. Allocation failure is accepted only with immediately captured error8, at least one owned token, nonnull readable expansion storage, all1088 bits full, unchanged PEB/TEB/descriptor identities, and exact equality of full bitmap to baseline OR the owned bitset. Any preexisting expansion array must retain its identity. A nonnull expansion pointer must be aligned8, in the canonical user range, and encompass0x2000 bytes; first/last8-byte RPM reads must succeed. This excludes the target's separate lazy-expansion-allocation failure path without treating error8 itself as exhaustion proof.

The probe frees its last owned token, verifies the one-bit hole, requires the same exact index on immediate reallocation, then requires immediate sentinel/error8 and the exact full bitmap again. It reverse-frees the known owners on every later path and requires final words equal baseline. Failed frees remain sticky fatal even if later cleanup can retire the index. Duplicate, baseline/preexisting, out-of-range or contradictory refill returns are ownership-ambiguous: they are never freed as borrowed tokens; known owners are retired and the fatal flag requires process termination before candidate unloading/reuse. The integration patch checks fatal independently of status because a later snapshot error may replace the diagnostic status. No successful result is returned on any cleanup error. Final count is set only after full success; failure count remains zero.

The helper owns temporary TLS indices, not the OS-owned TEB expansion allocation. The native allocator may create the current thread's expansion array during the probe; baseline-null to final-nonnull is recorded and does not alter the required bitmap equality. This is an index-capacity certificate only, not heap acceptance, first-load heap forgiveness, callback/FLS proof, or generic Windows support. Quiescence is an orchestration precondition; the helper does not claim an atomic snapshot of unrelated concurrently mutating callers.

The same test.c has a portable API-double branch and a real Windows branch. Portable controls include borrowed and duplicate tokens; out-of-domain values; unexpected/zero-success/early error8; all read and short-read points; pointer, descriptor, version, expansion endpoint and bitmap mutations; moved preexisting bits; refill/second-error failures; and real helper cleanup logic with failed-free injection. Injected known-owner leaks remain visible/fatal and are explicitly retired by the control before its state can reset. The Linux tests do not execute Windows.

The standalone Windows branch calls the real APIs, with one wrapper used only in that standalone test to force early error8 after two allocations. It runs four states: full certificate, forced early-error rejection with exact cleanup, one genuinely retained TLS token causing an exact one-index decrease against the first baseline, and explicit release/recovery of the original count/bitmap. It buffers its evidence until measurements end, avoiding stdio between the measured states. It emits all five snapshots and returned token IDs per case, followed by a terminal PASS only when all four controls meet the assertions. An absent/mismatched environment may be checked with `--expect-unsupported`, which requires SlotUnsupported and no probe allocations. The qualified fixture never contains this fault wrapper.

The scratch integration patch adds the include only for Windows and routes native TLS through the certificate before the old generic branch. The entire original include is recovered byte-for-byte by removing these two additions (inverse_proof.json). FLS and portable legacy tests keep their previous semantics. Ordinary OpenCL, transport-loss,1100 cycle threshold, exact lifetime workload, full-DLL source, and candidate inputs are unchanged. The original production fixture and pinned include/header dependencies were read/copied only to cross-build the proposed patched fixture. No Windows executable was run.

Current validation: portable O0/O2, UBSan, strict normal/-O coverage and both Windows cross-builds PASS. Actual helper coverage is101/101 lines and172/174 directional branch outcomes (98.85%), all6 functions, zero exclusions.192 portable cases execute293423 assertions. The ASan+LSan+UBSan executable returns-6 because LeakSanitizer explicitly reports that it cannot run under ptrace. That log is retained as FAILED(environment), never passed or hidden. Root must rerun the exact sanitized binary outside the tracing sandbox, with leak detection enabled, and must run the native controls and the final full reload qualification before acceptance. Prior native rejection at error8/acquired1086 stays failed; it is not relabeled by these tests.

## Root integration, target correction and native qualification

Astra frozen receipt b780ccf238065d61ce9a39b1850d0e6018f59c15bdf8494da622d16ad8021aae
binds100 scratch inputs. Root rehashes every binding before integration. The
old frozen receipt remains unchanged; root changes are separately evidenced
under `build/qualification_feature_20261021/tls_certificate_root/`.

Actual read-only layout diagnostic `slot_layout_diagnostic_a6d7ae01` confirms
PEB10.0.26300,64/1024-bit descriptors and buffers exactlyPEB+80/+240. DLL file
versions26100 do not identify the PEB OS build. The earlier native
`tls_certificate_native_6dc79aa5` rejects before allocation at the incorrect
26100 PEB assumption and remains failed. The correction requires the one
measured build26300, with an explicit regression rejecting26100; no accepted
version range or arbitrary error fallback is introduced.

The private GCC14 SDK intrinsic emits an array-bounds error from its synthetic
offset-address C lvalue. The owned helper reads the identical GS:0x30 identity
instruction directly on native AMD64 GNU/Clang Windows; all subsequent reads
still use bounded ReadProcessMemory. No warning suppression. Strict private
GCC14/O2/Werror standalone compilation passes. Portable API doubles retain
their separate root-identity adapter; their coverage is not native instruction
coverage. Root tests now pass193cases, ASan/LSan/UBSan,101/101 helper lines
and172/174 directional branch outcomes (98.85%), all six functions, no
coverage exclusions. Both Python coverage modes and unchanged portable reload
gates pass. The exact original sanitized binary also passes outside ptrace;
its worker ptrace/LSan failure remains preserved.

Fresh native Windows CPU controls `tls_certificate_native_a747436e` pass
using private14 fixture SHA256
13905b3aeadbc9586b8724bbec64aa8f83052b92e312bd727efbd87a24839d22.
Full bitmap saturation and exact returned-token reconciliation prove1086 free
indices. A genuinely held index reduces this to1085; explicit release returns
1086. Forced early error8 after two allocations is rejected and every temporary
owner retires. Both absent and mismatched target admission reject before
allocation. All native processes exit0 naturally with no survivors/forced
cleanup/stderr. Actual OS build26300 and both System32 file identities match
before/after, binding the already reviewed CodeView/PDB evidence transitively
through the full image hashes. Root independently verifies every raw bitmap,
one-bit hole, unique token, free count and restored baseline in
`tls_certificate_root/native_root_review.json`. No OpenCL/GPU DLL is loaded.

The first script-file launch was blocked before any native test launch by
Windows execution policy. Raw failure and read-only process absence are
preserved; the successful runner uses ordinary WinRM command execution and
changes no execution policy or security setting. This is target-specific TLS
capacity qualification only. FLS, final full fixture,1100 reloads, native heaps
and simultaneous Linux GPU acceptance remain mandatory. +0% progress.
