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
