# Current qualification update, 2026-10-17

Qualification remains in progress, feature completion 85.5%, Time Ended TBD.
README is unchanged. Earlier sections below are historical checkpoints; their
unchanged-source, unexecuted-OpenCL and growing-handle statements describe their
recorded checkpoints, not this current update.

Current tested production ICD is `03caf9b` (later feature commits add the
approved private clvk patch/build contract, tests and documentation).
Current fixture commit is `b040fe2`; tracked feature HEAD is recorded in Git.

- Full current ICD: 394 Debug and 394 ReleaseSafe tests; native C dispatch
  128 cycles; prescribed sanitizer/allocator gates pass. Whole production
  coverage 4,369/4,837 branches (90.32%), 4,696/4,766 lines (98.53%). Native
  Windows 394-case Debug/ReleaseSafe execution passed before the host reboot.
  Logs: `icd_debug_chain_final.log`, `native_icd_chain_execution.log`.
- Nineteen additional module groups pass unit/sanitizer/coverage commands:
  capabilities, dmabuf, instance-wire, query-wire, values, extensions-wire,
  objects, command, guest, present, wddm, frame, export, surface, request,
  context, service, runtime, worker. Every executed production coverage gate
  remains at least 90% for both lines and branches, with unchanged tooling.
  Exact commands, isolated Zig cache environment, exit codes and per-module
  logs: `build/qualification_20261017/remaining_cpu_results.json`; extracted
  gate summaries: `remaining_cpu_coverage_summary.json`. All nineteen exit zero.
- `make vgpu-tcp-server-test vgpu-tcp-server-sanitizers
  vgpu-tcp-server-coverage vgpu-tcp-bridge-test vgpu-tcp-bridge-sanitizers
  vgpu-tcp-bootstrap-coverage vgpu-tcp-bootstrap-sanitizers` exits zero. These
  are actual sockets/process ownership with synthetic frontend/SDK boundaries,
  not GPU proof. Bootstrap lines 133/133, branches 174/182 (95.60%); bridge
  lines 167/167, branches 216/234 (92.31%). All configured leak detection stays
  enabled. Log: `tcp_ownership_gates.log`.
- `make vgpu-libc-test vgpu-libc-sanitizers vgpu-receiver-test
  vgpu-receiver-sanitizers vgpu-receiver-coverage vgpu-native-loader-test
  vgpu-native-loader-sanitizers vgpu-present-integration
  vgpu-present-integration-sanitizers` passes its checks with leak detection
  enabled. This includes actual pinned Linux loader discovery/lifetimes and
  synthetic presentation protocol integration, not a physical compositor run.
  Log: `linux_receiver_loader_integration.log`.
- Source freeze `build/wddm_dxvk_checkpoint/dxvk_source_ac95c458` binds the
  corrected ICD and joined-caller DXVK fixture. Fresh physical GPU runs after
  Windows startup pass: sanitized positive `dxvk_gpu_7bbd3a2f`, normal postflush
  `dxvk_gpu_4f1be74b`, sanitized postflush `dxvk_gpu_84281cd7`. Positive requires
  all 4,096 rendered/presented pixels and 64 compute words, natural whole-session
  retirement and zero controller sanitizer diagnostics. Fault cases require
  the existing recorded-submission/device-removal assertions, exact trusted
  retirement proof and no SIGKILL. These are expected failure-cleanup tests,
  not positive workload receipts. `dxvk_current_results.json` binds logs and
  exit codes; each checkpoint stores its full acceptance/process journal.

## Native heap status

The original growing-handle audit remains failed and preserved. Exact native
creator stacks resolve to static winpthreads attached to the fixture's calling
thread during DXVK global logger construction. Commit `0c8d144` moves the entire
transaction to a joined CRT thread and retains all vendor DLLs through TLS/FLS
destruction before parent unload. It does not alter native baselines or free
borrowed/vendor blocks. The corrected three-life historical audit
`dxvk_heap_710ae4228123a45f` has 183/183/183 handles with exact GPU outputs and
natural host retirement. Raw UMDH baseline-to-final delta remains +99,489 bytes;
this is not zero-total-process-heap evidence and heap_acceptance remains false.

Its recurring 880-byte allocation (0x370, not the unrelated 0x880 GDI allocation)
has the LocalAlloc/MSCTF/IMM32/USER32 stack. The OS-only joined GUI-thread
control `system_window_audit_bfd98078` reproduces the same stack sequence and
one allocation per cycle without Vulkan/DXVK/Waddle runtime modules. Independent
Luna high-effort read-only review and exact-image LLVM symbolization connect
other large private ancestry frames to DXVK discovery and Windows loader,
DirectXDatabaseHelper, RPC and Configuration Manager internals. No unbalanced
project allocator was established. Process environment state (4,062 bytes) and
CRT temporary stderr buffering (4,096 bytes) also survive snapshots. These are
explicit attribution items; totals from unlike controls are not subtracted and
unknown ownership is not treated as a leak pass. Fresh current-ICD audit
`dxvk_heap_56dca3b969766ebc` is in progress with signed tools rechecked.

## OpenCL status

No OpenCL kernel acceptance has passed. `dxvk_gpu_14ce8bed` fails at instance
creation; `dxvk_gpu_17aa5f54` times out before discovery; `dxvk_gpu_005527db`
reaches platform discovery/three adapters but fails the old one-adapter fixture.
The last run also proves twenty retained clvk-owned ICD objects; its scoped
forced cleanup grants no natural-retirement credit. Artifacts remain preserved.

The user explicitly approved the private-copy correction in
[the versioned lifetime review](CLVK_LIFETIME_REVIEW.md). Commit `b4481b9` adds
terminal ownership preflight/shutdown and exact patch/ABI/export cache binding;
independent review finds an executor candidate could be stranded bound.
`fac8c24` fixes selection ordering while preserving strict free/idle shutdown.
Sixteen admission tests pass, including actual patched owner accounting under
ASan/LSan/UBSan and the exact patched pool with a controlled executor boundary.
The previous implementation fails its reuse negative control. The first owned
cross-build was intentionally interrupted without publishing a receipt; partial
inputs/logs are retained. `clvk_pool_cross_build.log` records the corrected build
in progress. The new fixture requires private shutdown, rejects live owners,
checks terminal discovery/idempotence and unloads only after caller TLS/FLS exit.
Strict Windows cross-build passes; actual new-DLL execution remains required.

## Environment and remaining qualification

The host rebooted during the interruption. Existing VM disk, firmware and TPM
files were preserved; the VM restarted with the existing QEMU configuration.
Windows applied its pending update without intervention. Fresh OS inventory is
`resumed_windows_inventory.json` (Windows 11 Pro 10.0.26300, UBR 9711, interactive
explorer session 1). Later native/physical receipts must bind this environment;
earlier results are not silently reused across the OS change. Native heap, full
physical WSI matrix, new OpenCL DLL kernels/retirement/heaps, simultaneous Linux
GPU workload and final source/native CI remain open. No PR, merge, feature
completion or Time Ended is recorded.

---

# Verification checkpoint, 2026-10-17

Qualification remains **in progress**. No PR or completion claim follows from
this checkpoint. README, dependency pins, production code and thresholds are
unchanged during this verification phase.

Entry: `feature/software-vgpu-slicing`, clean tracked tree at
`7214c8e7007e7d9aa8236b90b90a98eb9f89255e`. Claude/workers relinquished editing
by direct user confirmation. Milestone 2 means the bounded WSI lifetime milestone
identified by CODEX_HANDOFF.md and HARDWARE_TEST_HANDOFF.md.

## Local and cross-build evidence

Commands executed from the entry source; subsequent commits add only tests,
build targets and documentation. Zig 0.13.0; prescribed leak detection remains on.

| Command | Result and evidence class |
| --- | --- |
| `make vgpu-wsi-lifecycle-test vgpu-wsi-lifecycle-sanitizers vgpu-wsi-lifecycle-coverage` | Exit 0. Twelve Debug/ReleaseSafe Zig cases, C API doubles/unsupported platform, prescribed C ASan/LSan/UBSan and owned Zig instrumentation/allocator pass. WSI lines 148/149 (99.33%), branches 145/156 (92.95%); C sink lines 47/47, branches 71/72 (98.61%). Portable evidence. |
| `make vgpu-icd-test vgpu-icd-sanitizers vgpu-icd-units vgpu-icd-coverage` | Exit 0. Full current 394-case aggregate; native dispatch fixture 128 cycles. Whole-ICD lines 4694/4764 (98.53%), branches 4369/4837 (90.32%). Owned sanitizer/allocator runs pass. Mocks/portable execution, not physical acceptance. |
| `make vgpu-test vgpu-sanitizers vgpu-coverage` | Exit 0. Includes concurrency/process stress and protocol/mapping/lifecycle checks. All executed coverage gates pass; zero prescribed sanitizer findings. |
| `WADDLE_RECORDER_SANITIZERS=1 python3 -B tests/av/coverage_recorder_test.py -v` | Exit 0, all eight cases including fatal-UBSan negative control. |
| `make av-test av-input-test av-sanitizers av-coverage` | Initial/retry cached Zig imports fail `FileNotFound`, preserved in av.log/av_retry.log. Same unchanged command with isolated ZIG_GLOBAL_CACHE_DIR/ZIG_LOCAL_CACHE_DIR passes, including 100000 concurrent video/PCM frames and all coverage gates. No source or acceptance change to work around cache failure. |
| `make vgpu-windows build/vgpu_win32_present_test.exe vgpu-dxvk-client-build` | Exit 0; Windows cross-build only. DXVK cache revalidated, not rebuilt. |
| `make vgpu-opencl-pin vgpu-opencl-build` | Exit 0; pins and existing DLL cache verified, not rebuilt or executed. |
| `make vgpu-opencl-test vgpu-opencl-fixture` | Ten build-tool methods and strict Windows C11 fixture cross-link pass. Kernel execution remains pending. |

Full log and executable/source hash inventory:
`build/qualification_20261017/checkpoint_sha256.json`, SHA-256
`4635d7fa9466420a2945cb697c1072d2ac2b8b5b06ed347a6a28b375c654d540`.
This immutable checkpoint predates later diagnostics; it does not bind later
appended logs. The coverage/sanitizer tools preserve uniquely named detailed
artifacts, recorded inside the corresponding logs.

| Log under build/qualification_20261017 | SHA-256 |
| --- | --- |
| wsi.log | bb2f8d99cbfd271e2b81124686be686da44f132d4c3c4da69278bec91795bb33 |
| icd.log | 4354b57c1654ba95adf8df3f264b85a64e88ae430abafdd39fd250f944a40367 |
| transport.log | 39042b9cd7748b572df3c5e728b1a8a8c4aaa14f948dc0ffabb34b8d11f8c115 |
| av_fresh_cache.log | b5f52d8fd69b2898dc1836cc251c723fb037d20c739522bb7c3098d23beba54f |
| recorder.log | bb03151166e713b71fd77805ec8a28a71576c4a7ce2ebb576a1daa69e562bc14 |
| wsi_windows_native.log | 4d407f9428f98c0d9f19a3a91cf73b8636a64be2128c7455609b5cd158bcf60a |

## Native Windows state fixture

Cross-linked `zig test src/vgpu/venus_wsi.zig -Iinclude
-Isubmodules/venus_protocol/include -lc -target x86_64-windows-gnu
--test-no-exec -femit-bin=build/qualification_20261017/wsi_windows.exe`.
Transferred through authenticated WinRM, checked SHA-256 before running:
`6e1eaf4ecdc6ac3f8f046351fa10ba340aa8fcb3255204ea2e60d28e44d9a338`.
Native Windows execution exits zero, all twelve cases pass. This fixture uses
state/backend doubles: it is native CPU evidence, not actual HWND/GPU evidence.

## Fresh physical DXVK audit

Frozen source/runtime: `build/wddm_dxvk_checkpoint/dxvk_source_2c89e944`,
source commit `7347352` (its DXVK fixture equals current tracked fixture; this
includes compiler-retention commit e8961c5). Rebuilt isolated Windows/runtime
artifacts; unchanged renderer reused only after exact hash/pin comparison.

Fresh three-lifetime audit:
`build/windows11_vm/dxvk_heap_dea02b8aca67b7f1`. Every lifetime produces all 4096
exact rendered and presented pixels and 64 exact compute words, with medium
integrity and natural authenticated controller/worker/whole-session retirement.
Signed Microsoft UMDH inputs revalidated before and after; image-specific UST
registry key restored to its original absence. Deployed binaries rehashed after
execution. Prior artifacts and VM state retained. The VM initially required
interactive sign-in; no workload was launched by the first keyboard attempt.
The existing private credentials were used without logging or changing them.

**Heap acceptance remains false.** Native handle counts after unload are
201, 205, 209; this growth has not been independently attributed. Raw captures,
original and symbol-resolved differences are retained. The system-only compiler
reproduction executes all three ordering modes for eight cycles; it reaches a
stable busy-byte total after startup, which does not explain or clear the DXVK
handle growth. Raw byte totals never substitute for allocation-stack acceptance.
A separate diagnostic-only handle-tracing run is being prepared/performed;
it cannot receive qualification credit or silently replace the original audit.

## Open work

- Attribute and eliminate owned native DXVK handle/heap survivors; rerun normal,
  sanitizer/fault and native heap gates as affected.
- Execute the prepared OpenCL fixture through the real guest/host path, fix
  defects, prove natural retirement, native heap/handle stability and overlapping
  Linux GPU work. Prepared run: `build/windows11_vm/dxvk_gpu_869aaa04`, no execution
  or acceptance claimed at this checkpoint.
- Complete remaining applicable native/physical WSI and shared-GPU cases and
  exact-source CI/deployment verification before closure. Cross-builds and native
  state mocks do not substitute for physical presentation or signed-driver use.
- Upstream clvk changes, if needed, require the handoff's explicit user approval
  and a tracked private-copy patch. No submodule was edited.

WSI tracker stays open pending final qualification accounting; software-vGPU
stays 85.5%, Time Ended TBD. No assertions, coverage thresholds, suppressions,
production exclusions or workload acceptance budgets were weakened.


Loader guard production coverage is now an independent >=90% line-and-branch
Make/CI gate. It compiles the unchanged production prefix and counts only its
seven exact functions, never the injected tests. Fresh mock ASan/LSan/UBSan,
coverage and strict Windows cross-build pass in
`build/qualification_20261017/environment_guard_complete_gates.log`:
84/84 lines and74/74 branches (100%). Additional regressions cover malformed
GetEnvironmentVariable lengths/NUL, every absent API member, invalid active
restore with retained snapshot, and retry. Actual Windows round-trip passes in
`loader_environment_native_42ea77cc/record.json`, with matching executable hashes
before and after. This fixture-only change does not establish native heap or GPU
acceptance; the guard production prefix is unchanged from ef77895.


Native heap follow-up: configure both borrowed CRT diagnostic streams unbuffered
in wmain before any use. The prior audit's stderr-triggered4096-byte UCRT stdio
buffer survives cycle snapshots. Explicit _IONBF configuration avoids acquiring
that buffer during measured work; no borrowed stream is closed, no snapshot or
baseline is moved, and no leak is suppressed. Strict DXVK fixture cross-build
passes (`dxvk_unbuffered_build.log`); fresh native GPU/heap evidence is required.


Native lifetime verification fixture (qualification addition): the separate
`wsi_lifetime` workload repeats eight actual device lifetimes. Each owns its
popup HWND, surface, chains, binary semaphores and temporary DCs, with exact
GDI/USER equality and the existing exact process-handle baseline. It renders to
an acquired image before a valid failed extent-mismatch replacement, requires
that replacement to retire the old chain while preserving that acquired image
for presentation, and compares all4096 client pixels against GPU readback.
It checks paired zero capabilities, sticky OUT_OF_DATE with unchanged index,
fresh replacement and exact rendering after restore;80x64 resize capabilities,
old acquire rejection and64x64 replacement/rendering; actual HWND destruction,
SURFACE_LOST before replacement, and retired-chain OUT_OF_DATE precedence after
a valid lost-surface replacement attempt. It claims no unobserved HWND reuse.
All owners are released before starting the next case. The original48-frame
presentation workload, retry bounds and native assertions are unchanged.
Strict integrated Windows cross-build passes (`native_wsi_lifetime_build.log`);
this is compilation only, and actual native/physical execution remains required.
