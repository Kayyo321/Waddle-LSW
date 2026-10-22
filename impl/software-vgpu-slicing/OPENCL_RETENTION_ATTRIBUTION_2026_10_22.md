# OpenCL first-cycle retention attribution — 2026-10-22

Status: **attributed; qualification still failed; human contract decision required.**
This records diagnostic evidence only. It grants no reload, heap or acceptance credit
and changes no task percentage. Parent 85.5% / In Progress, Time Ended TBD.

## 1. Question

R7 (`opencl_reload_final_7d5c6bb_revision7/`) rejected its first full reload cycle:
persistent baseline 65 handles / 1086 free TLS / 4078 free FLS, observed 148 / 1083 / 4075
(+83 handles, 3 TLS, 3 FLS). [Retention record](OPENCL_NATIVE_RETENTION_2026_10_21.md)
left the owners unattributed. This unit identifies every retained resource.

## 2. Census tooling and failed attempts (preserved)

All runs use canonical candidate `70ca4fdb…` (OpenCL.dll), loader `dcc3bb5a…`, ICD
`ba75a97c…`, bootstrap `7602b9fc…`, fixture `26de712f…`, the exact idle VM lease and the
user-approved lease-owner wrapper (`build/qualification_feature_20261021/lease_owner_wrapper_r1/`,
receipt `dc4c020c…`). Local evidence root: `build/qualification_feature_20261021/`.

| Attempt | Runner | Outcome |
|:--|:--|:--|
| `opencl_census_65eb0fd5` | frozen R2 | Fixture never launched: the final batch line was a Python raw string, so `cmd` received literal `\r\n` text and rejected `2> stderr.log\r\necho` as one path. No host started. |
| `opencl_census_b95e5f20` | R3 (batch fix) | Fixture reached `baseline`; runner asked for `…\attribution\baseline.ready`, which the reused R6 validator rejects (`…\audit\` only). Fixture retired by its own 60 s bound, exit 1. No OpenCL load, no host. |
| `opencl_census_547d3a74` | R4 (audit directory) | Full ordinary eight-context GPU life, natural exit 0, host retired with empty graph. Both snapshots stopped at the first non-duplicable handle (`DuplicateHandle` error 50). Partial TLS/FLS/module data only. |
| `opencl_census_85b04e7e` | R5 (helper revision 2) | **Complete.** Both snapshots complete, native exit 0, natural absence, host status 0 / worker 0, no signals, owned session empty. |

Each failed folder holds `failed_attempt_review.json` or `attempt_review.json`. R3, R4 and
R5 each change only reviewed lines (`runner_r*.patch`) and add a regression test that fails
on the previous revision. Helper revision 2 (`opencl_census_r5/diagnostic_snapshot.c`)
reads type names from `NtQueryObject(ObjectTypesInformation)` keyed by the handle type
index; it no longer duplicates target handles and no longer requests `PROCESS_DUP_HANDLE`.
Its native control (`opencl_census_r5/native_control_result.json`) typed 92 of 92 handles of
a `cmd.exe` target, including 32 `EtwRegistration`; an earlier PowerShell target exceeded the
fixed 512-handle capacity and is preserved as failed.

## 3. Complete ordinary-life delta (R5, baseline → unloaded_1)

| Resource | Delta | Owners |
|:--|--:|:--|
| Handles | +83 | EtwRegistration 40, Event 11, Key 8, File 4, Semaphore 4, IoCompletion 3, WindowStation 2, Mutant 2, ALPC Port 2, IRTimer 2, WaitCompletionPacket 2, Desktop 1, TpWorkerFactory 1, Section 1; none removed |
| TLS indexes | +3 | indexes 1, 2, 3 |
| FLS slots | +3 | slot 2 `ucrtbase.dll`+0xb3190, slot 3 null callback, slot 4 `ucrtbase.dll`+0x78980 |
| Threads | +2 | both start at `ntdll.dll`+0x24990 (thread-pool worker) |
| Modules | +22 | bcrypt, bcryptprimitives, CFGMGR32, combase, directxdatabasehelper, dxcore, dxgi, GDI32, gdi32full, IMM32, kernel.appcore, msvcp_win, mswsock, powrprof, RPCRT4, sechost, ucrtbase, UMPDC, USER32, win32u, windows.staterepositorycore, WS2_32 |

The ordinary life reproduces R7's exact +83 / +3 / +3. OpenCL.dll, vulkan-1.dll, the ICD
and the bootstrap are all absent after unload; every retained module is a Windows component.

## 4. Component controls (session 0, no GPU)

`component_retention_r1/component_retention.c` (source `c48cf12e…`, PE `cb655fee…`,
imports KERNEL32 and msvcrt only) measures modules, typed handles, TLS and FLS in a fresh
process before, after one component cycle and after an identical second cycle. Run
`run_35eca8cc/` (16 cases, all exit 0, no survivors):

| Case (balanced use, then FreeLibrary) | First cycle | Second cycle |
|:--|:--|:--|
| kernel32 load/free (noop control) | +0 | +0 |
| USER32, GDI32 load/free | +5: WindowStation 2, Desktop, Event, IoCompletion; module unloads | +0 |
| dwmapi, dxgi load/free | same +5, +1 Key | +0 |
| DXGI factory create/enumerate/release | +37, +1 TLS; USER32, GDI32, RPCRT4, dxcore, cfgmgr32, ucrtbase, msvcp_win stay loaded | +0 |
| Winsock startup/socket/close/cleanup | +10 (EtwRegistration 7), +2 TLS; ws2_32, mswsock, RPCRT4, ucrtbase stay loaded | +0 |
| bcrypt load/free | +11 (EtwRegistration 7); bcrypt, sechost stay loaded | +0 |
| ucrtbase, cfgmgr32 load/free | +0 | +0 |
| combase load/free | +1 Key | +0 |
| vulkan-1.dll load/free (no API call) | +1 Key | +0 |
| vulkan-1.dll + `vkEnumerateInstanceVersion` | +21, +1 TLS; dxgi, GDI32, USER32, dxcore and 9 more stay loaded | +0 |
| Waddle ICD load/free | +7: USER32 set plus 2 Key | +0 |
| Waddle TCP bootstrap load/free | +12: bcrypt set plus 1 Key | +0 |
| OpenCL.dll load/free | +7: USER32 set plus 2 Key | +0 |

The last row explains the previously unattributed +7 handles of the
[load-only control](OPENCL_NATIVE_RETENTION_2026_10_21.md#full-candidate-load-only-control-subsequent-native-diagnostic).

## 5. Attribution

1. **USER32/win32k connection.** Loading USER32 connects the process to a window station
   and desktop. Those handles stay after USER32 unloads. The Waddle ICD statically imports
   USER32, GDI32 and dwmapi only for `src/vgpu/venus_win32_present.c` (six calls).
   Loading OpenCL.dll alone leaves the same set; its static imports include SHELL32 and
   ole32, which import USER32.
2. **Khronos loader reference leak (upstream).** `windows_initialization` in
   `submodules/vulkan_loader/loader/loader_windows.c` (v1.4.307, `0508dee4`) calls
   `LoadLibraryEx("gdi32.dll")` and `LoadLibraryW(<System32>\dxgi.dll)` and never frees
   either. It runs on the first API call of every vulkan-1.dll instance, so each reload adds
   one more module reference. This pins the DXGI/DXCore/GDI closure.
3. **Winsock and bcrypt.** Both remain loaded after balanced use and free. The bootstrap
   imports `WS2_32` and `bcrypt`; `venus_tcp_socket.c` balances WSAStartup/WSACleanup per owner.
4. **C runtime FLS.** `ucrtbase.dll` unloads cleanly alone. It stays because pinned modules
   depend on it, and its per-thread data owns FLS slots 2 to 4.

No retained resource belongs to a Waddle or clvk module that remains loaded, and no
component grew on its second cycle. This does **not** prove Waddle code is leak-free: the
census sees only handles, TLS, FLS, threads and modules, not heap, and the OS-first-use
state hides any Waddle-owned resource of the same type.

## 6. Consequence for the reload contract

[OPENCL_RELOAD_CONTRACT.md](OPENCL_RELOAD_CONTRACT.md) requires the exact persistent
baseline before and after every reload, including the first, with no warm-up. Any process
that uses Winsock or loads USER32 keeps the state above after its first use, so the 1100-cycle
run cannot pass first-cycle equality as long as the bootstrap uses Winsock. This is a contract
conflict, not a measurement failure. Changing that rule needs an explicit human decision; no
threshold, predicate or baseline was changed here.

## 7. Diagnostic census after the approved pre-initialization (2026-10-22)

The approved four-step pre-initialization is implemented in
`tests/vgpu/opencl_os_preinit_windows.inc` (commit `d029a7a`) and enabled for this census by
the diagnostic heap-audit switch (commit `5bb843b`). Fixture `4127a966…` (receipt
`c09923a0…`, 301 dependencies) runs under census R6 (`opencl_census_r6/`, bindings
`dd2a46f4…` re-bind only the changed fixture source and add the new include).

Native switch controls (`os_preinit_switch_controls/`): a value of `2` in heap-audit mode and the
switch in reload mode exit 2, while the same arguments without the switch exit 1. The
no-audit case is not decisive because its arguments exit 2 either way.

Census `opencl_census_4ddb96ad`: complete snapshots, native exit 0, natural absence, host
status 0 / worker 0, no signals, empty owned session.

| Resource | Delta after pre-initialization |
|:--|:--|
| TLS, FLS, threads | 0 |
| Handles | +34: EtwRegistration 13, Event 8, Key 6, Semaphore 2, Mutant 2, File, IoCompletion, Section |
| Modules | +7: dxgi, combase, directxdatabasehelper, kernel.appcore, powrprof, UMPDC, windows.staterepositorycore |

The approved steps are therefore **not sufficient** for exact equality.

## 8. Remaining residual attribution (component controls revision 2)

`component_retention_r2/` (control source `c748c9d3…`, PE `36959718…`) compiles in the
committed pre-initialization include. After pre-initialization, the Waddle bootstrap,
Winsock, combase, ole32 and shell32 add nothing on either cycle. Three residual sources remain:

1. **Standard-search DLL resolution.** The first DLL resolved through the default search order
   makes ntdll open and keep `HKLM\SYSTEM\ControlSet001\Control\Session Manager` (+1 Key).
   The approved steps load only System32 paths, so they never trigger it. The Waddle ICD and
   OpenCL.dll trigger it through their dependencies. After one extra bare-name
   `LoadLibraryW("dwmapi.dll")`/`FreeLibrary`, the ICD, OpenCL.dll, the bootstrap and a DXGI
   factory each add exactly 0.
2. **OS package query.** The loader calls kernel32 `GetPackagesByPackageFamily` for
   `Microsoft.D3DMappingLayers_8wekyb3d8bbwe`. Windows then keeps `windows.staterepositorycore.dll`
   and one StateRepository key. One OS-only size query of the same family absorbs it.
3. **Upstream loader reference leak.** `windows_initialization` keeps `dxgi.dll` and `gdi32.dll`
   referenced forever, one more reference per loader instance. A control that loads `dxgi.dll`
   without freeing it reproduces the residual (+3 EtwRegistration, dxgi/powrprof/UMPDC).

`loader_release_eval/` holds a proposed 76-line patch
(`vulkan_loader_release_windows_module_references.patch`) that stores both module handles and
frees them in `loader_release()`, beside upstream's existing preloaded-ICD release on dynamic
unload. It is **evaluated, not integrated**: no submodule pin, product build or deployed
asset changed. Native A/B results after the three OS steps (approved four, plus items 1 and 2):

| Loader (scratch builds, same flags) | First cycle | Second cycle |
|:--|:--|:--|
| Unpatched `57679220…` | +3 handles, dxgi/powrprof/UMPDC stay loaded | +0 |
| Patched `d7a71318…` | +0 | +0 |

Items 1 and 2 are OS-only steps outside the approved four, and item 3 is an upstream change.
Each needs an explicit human decision. No predicate, threshold or baseline changed.

## 9. Six-step pre-initialization with the patched loader (2026-10-22)

Amendment 2 is implemented: six-step pre-initialization (commit `f871b9e`), the tracked loader
patch build (commit `5682038`, frozen MinGW loader `8a9e3d32…` in
`build/qualification_feature_20261021/patched_loader_8a9e3d32/`) and the reload-mode wiring
(commit `402c2e2`). Fixture `d3464a8a…` (receipt `a9cde7f0…`). Every census below completes
with native exit 0, natural absence, host status 0 / worker 0, no signals and an empty session.

| Census | Change | Handles | TLS / FLS / modules | Threads |
|:--|:--|:--|:--|:--|
| R7 `opencl_census_75122850` | six steps, patched loader | +2: IoCompletion, Mutant | 0 / 0 / 0 | +1 pool worker |
| R8 `opencl_census_d69c303d` | helper revision 3 names named objects | +2: IoCompletion, Mutant `\Sessions\1\BaseNamedObjects\DBWinMutex` | 0 / 0 / 0 | 0 |
| R9 `opencl_census_4ab3b215` | batch no longer sets `VK_LOADER_DEBUG` | +1: IoCompletion | 0 / 0 / 0 | 0 |

- **DBWinMutex.** The loader writes every emitted log message to `OutputDebugString` as well as
  stderr (`loader/log.c`), and Windows opens and caches `DBWinMutex` on first use. The harness's own
  `VK_LOADER_DEBUG=error,warn,driver` request caused it; nothing parses loader log output. Census R9
  drops that variable. That is a harness diagnostic setting, not a product change.
- **IoCompletion.** After the six steps, component controls (`component_retention_r2`, source
  `4f935af3…`) add one IoCompletion only for a nonblocking connect completed through `select`,
  which is Waddle's `venus_tcp_socket.c` connect path. A blocking connect with send/receive,
  `select` on a listener, and the CNG RNG sequence used by the bootstrap each add 0. A second
  cycle adds 0.
- **Snapshot helper revision 3** (`opencl_census_r8/`, source `866793a7…`, PE `0471ce57…`) adds
  `HANDLE_NAME` rows for Mutant, Event, Semaphore, Section, Key and Directory from a momentary
  duplicate; File names are never queried. Its native control names 35 objects of a `cmd.exe`
  target. R8 and R9 parse the rows, bind them only to listed named-type handles, and reject
  malformed or duplicate rows.

Exact equality now depends on one more OS-only step, a nonblocking loopback connect. That is
outside the approved six, so it needs a human decision. No predicate, threshold or baseline changed.

## 10. Zero-residual proof (amendment 3, 2026-10-22)

Step 7 is implemented (commit `4ce7a3e`), and the reload witness bound now admits the seven-step
witness (commit `4dd24d1`). Census R10 (`opencl_census_r10/`, fixture `622209a3…`, receipt
`bc547a4a…`, patched loader `8a9e3d32…`, no `VK_LOADER_DEBUG`) ran
`opencl_census_5a04e3d8`: one ordinary eight-context RTX 5080 life after the approved seven-step
pre-initialization.

| Resource | Baseline | After full unload | Delta |
|:--|--:|--:|--:|
| Handles | 109 | 109 | 0 added, 0 removed, 0 changed |
| TLS / FLS | unchanged | unchanged | 0 |
| Modules / threads | unchanged | unchanged | 0 |

Native exit 0, natural process absence, host status 0 / worker 0, no signals, empty owned session,
and exactly one seven-step witness line. This satisfies the proof required before reload runs. It
is one ordinary life, not the 1100-cycle reload or heap acceptance.

## 11. Reload attempt R8 and the promoted-structure ICD defect (2026-10-22)

Reload runner R8 (`opencl_reload_runner_r8/`, composite `20825553…`) started the 1100-cycle run
`opencl_reload_final_r8/`. Cycle 1 met exact handle/TLS/FLS equality. Cycle 2 failed when the
supervisor could not decode clvk's log as UTF-8: clvk printed `driverName`/`driverInfo` bytes it
never received. The native process retired naturally (exit 1), the host session was empty and no
guest survivor remained (`failed_attempt_review.json`).

Root cause: the Waddle ICD advertises Vulkan 1.3 but left every structure promoted into core
1.1-1.3 unwritten. clvk does not value-initialize its driver, ID, PCI and maintenance3 property
members, so cycle 2 read reused heap bytes. Features2 and device creation had the matching gap.
Commits `5c8a557`, `1614757` and `b271730` fix all three paths
([IMPL_DESC](IMPL_DESC.md#promoted-core-structure-publication-2026-10-22)).

The fixed Windows ICD `4a36da9b…` is built from `b271730` by
`icd_promoted_build/build_icd.py` with receipt `900a812e…`. The same builder applied to `b56f7b7`
reproduces the previously deployed ICD `ba75a97c…` except 12 link-identity bytes (PE timestamp,
its debug-directory copy and an 8-byte debug identifier). The deployed difference is therefore
exactly the three fix commits.

Prepared offline, **not run**:

- Census `opencl_census_promoted_icd/`: R10 with only the ICD asset replaced. The host-tree check
  admits exactly the four fixed sources, which must equal the ICD build receipt. Bindings
  `e59342f3…` differ from R10 only in those four hashes. All revision tests and the read-only
  preflight pass.
- Reload runner `opencl_reload_runner_r9/`: R8 with only the supervisor ICD override (16 added
  lines). R8 is kept as a history guard; R9, R8-history and both self-tests pass. Its composite
  manifest must be written after the census, with the census record as evidence.

## 12. Host reboot recovery and promoted-ICD census (2026-10-22)

The host shut down at 22:59 local on 2026-10-21, after the overnight pause, and booted again at
10:44 local. That ended QEMU 10732/62029 and swtpm 10727/62022. It also cleared the tmpfs `/tmp`,
which held hash-pinned frozen runner inputs.

VM recovery:

- `qemu-img check` found no errors. The preserved disk, `vars.fd` and TPM state were booted with
  the documented no-install-media launch (CODEX_HANDOFF section 9). Nothing was reset,
  reprovisioned or restored from backup.
- New identities are QEMU 7564/25543 and swtpm 7558/25429. The lease records them, keeps the old
  pair as superseded and stays idle. `vm_lease.pre_reboot_20261022.json` keeps the prior lease.
- Auto-logon is off, so the user signed in at the console.

Temp-file restoration (`build/qualification_feature_20261021/tmp_restore_20261022/`):

- `restore.py` admits a preserved copy only when its SHA-256 equals the pinned value. It never
  overwrites, and the census set was restored all-or-nothing. `receipt.json` lists every restored
  and every missing file.
- 517 distinct temp paths are pinned across preserved receipts, with no conflicting pins. 299 had
  byte-identical preserved copies and were restored.
- Two census inputs had no copy: the identity helper `waddle_opencl_final_heap_plan/runner.py` and
  `freeze_census_r2.py`. The R9 composite also inherits the evidence file
  `waddle_tls_fls_contract_root_20261021.md`, which had no copy either. All three were rebuilt from
  the Codex session transcripts and admitted only on exact hash equality.
  `reconstructed/PROVENANCE.md` records the transcript lines used.
- The other missing paths are historical scratch evidence (certificate build trees, coverage
  intermediates). No pending runner loads them. Their pins remain in the receipts that name them.

Census revision `opencl_census_promoted_icd_reboot/`:

- The census bound the VM identity through the R7 plan, which names the ended instance. The
  revision replaces only the identity source of the two lease checks with the leased instance
  (`runner_reboot.patch`, 2 lines removed, 6 added). The lease check stays exact: owner, idle
  state, identity equality and live start ticks.
- The prepared promoted-ICD census stays unchanged as evidence. Its R10-to-promoted patch is
  now checked against it as a history guard.

Native TLS/FLS controls `opencl_reload_controls_native_06d96b57/` were rerun on the new instance
with the unchanged revision-6 controls runner. R9 requires controls recorded under the leased VM
identity. Both controls and all ten malformed-input controls pass.

Census `opencl_census_483f3b38/` ran with the promoted-structure ICD `4a36da9b…`, the patched
loader and the seven-step OS pre-initialization:

| Resource | Delta from baseline to after full unload |
|:--|:--|
| Handles | 0 added, 0 removed, 0 changed |
| Modules / threads | 0 |
| TLS / FLS | 0 |

Native exit 0, natural process absence and a complete diagnostic. This matches the zero-residual
proof of section 10 with the fixed ICD. It is one ordinary life, not the 1100-cycle reload.
