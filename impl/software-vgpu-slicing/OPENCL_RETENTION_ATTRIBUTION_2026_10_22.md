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
