# M2 native process-handle gate: independent ownership audit and resolution, 2026-10-21

Scope: M2 = guest WSI resize/loss/retirement lifetime (`impl/wsi-window-lifetime`). Not software-vGPU TODO #2.
The strict native gate (exact `GetProcessHandleCount` equality to the baseline after each of 8 lifetimes, 100 samples x 10 ms)
is **unchanged**. This note records the independent audit, what the lead did about each finding, and the decision that is
not the implementer's to make.

## Verdict

**No Waddle-owned code path can explain the preserved 199->200 failure (`tcp_gpu_7adb6000`).** The same fixture sources and the
same five guest binaries (identical sha256, built from eb1ff4c whose `src/` and `include/` equal HEAD) passed in other runs. The
ICD and the bootstrap import no API that creates threads, events, timers, files or duplicates during normal running; every
bootstrap handle is created before the baseline; transport poison/timeout paths provably did not run in the failed run
(`worker_retired=1 status=0`; no "Retained native session"). The count oracle is process-global and sensitive to OS-owned churn
(RPC idle garbage collection: ALPC closes plus Thread/Event creations, 0 net in the GPU diagnostics, +1 net in the no-Vulkan
no-Waddle control at 59.8 s). It can also **hide** a real Waddle leak that an OS close offsets exactly. The handle type and value of
the original failure were never recorded, so no existing artifact identifies it. **No production fix is justified; none was made.**
Closing borrowed handles, prewarming an elevated baseline, reducing frames, widening the budget, replacing the assertion with
logging, or redefining acceptance remain forbidden and were not done.

## What the lead did

| Audit item | Disposition | Evidence |
|:--|:--|:--|
| 4.2 non-weakening diagnostics (baseline handle-table snapshot with type names, failure-only added/removed diff, per-lifetime QPC timeline, sample statistics, `GetProcessHandleCount` error text) | **Implemented** as passive failure-only code; acceptance rule byte-identical, pinned by exact source text in the host test | commits `faf9d19`, `56b89a9`; host test 18 cases / 6724 checks under ASan+LSan+UBSan; native test 7/7 in the Windows 11 guest, WinRM and interactive |
| 5.5 double `ReleaseDC` on failure (fixture) | **Fixed** | commit `7b6c9d5` |
| 5.6 stale `HWND` after successful `DestroyWindow` (fixture) | **Fixed** | commit `6faf4ed` |
| 5.7 weak failure diagnostics (`handle_baseline`) | **Fixed** by the diagnostics above | `faf9d19` |
| 5.1 `StretchDIBits` negative return on right-to-left windows would mark the surface permanently lost (inference from documentation) | **Not reproduced natively; no production change.** Probe on the real guest, interactive desktop and WinRM: `StretchDIBits` returned positive scanline counts (89, and 44 for half height) for a normal window, `WS_EX_LAYOUTRTL`, `WS_EX_LAYOUTRTL|WS_EX_NOINHERITLAYOUT`, and a left-to-right window whose DC was set to `LAYOUT_RTL`; the sink returned 1 in every case | `build/qualification_20261020/vm/rtl_probe.c` (sha256 cc58ad4d...), `rtl_probe_winrm_session_20261021.json`, `interactive_validation_20261021.json`. The existing native presentation fixture also passes on the interactive desktop (exit 0). An RTL regression case in `tests/vgpu/win32_present.c` is an optional follow-up, not done |
| 5.2 `DwmFlush` failure after pixels were copied becomes sticky `SURFACE_LOST` | Unchanged: documented design (IMPL_DESC pacing failure is a native failure); low severity, re-confirm if desired | - |
| 5.3 `destroy_instance` leaves leaked (invalid-usage) surfaces in the 16-slot registry until unbind | Unchanged: invalid application usage, no OS handle involved, low severity | - |
| 5.4 fixture window thread never pumps messages | **Unchanged on purpose**: adding a pump alters the workload and is an acceptance-owner decision; not shown to cause the +1 | - |

## Decision required (not the implementer's)

The M2 acceptance owner must choose how the native Windows gate proceeds; agents may not redefine acceptance:

- **A.** Keep the oracle as written. M2 stays blocked on this Windows image; future failures are now attributable (diagnostics above).
- **B.** An explicit, recorded **criteria amendment** (not a waiver) to an attribution-aware strict oracle: handle-table snapshots at baseline
  and at every check with handle tracing enabled from process start; fail on any added/removed handle whose creation/close stack contains
  a Waddle module, on any delta in handle types Waddle can create directly, and on any delta that cannot be attributed (fail closed); require
  the whole global delta to be explained by OS-only stacks. Strictly stronger for Waddle-owned handles and catches offset swaps, but it
  changes what "acceptance" measures.
- **C.** Change the platform under test (RPC/TSF behaviour or image). Not proposed as a remedy.

Meanwhile the instrumented fixture can be run repeatedly (runner prepared under `build/qualification_20261020/m2_runner/`, not executed): if
the historic failure recurs, the new output attributes it to a handle type and, with option B tracing, to a creator.

## Independent review of the diagnostics change (interim, stopped at the user request)

Opus review: no blocking defects. Acceptance integrity SOUND (only the listed lines changed; the new code creates no handles; under 1 microsecond
added between teardown and the first sample; supervisor parsers unaffected; heisenbug risk judged negligible against a ~60 s RPC-GC timer).
Windows API use SOUND for x64 (class 51 undocumented but stable since Windows 8, layout confirmed natively by exact access masks and type names).
Memory SOUND; 38 textual mutants of the `.inc`, 35 killed (the survivors are an equivalent `_Alignas` removal, a describe-line field swap hidden by
equal counts, and an untested rejected-layout fallback). Should-fix items S1/S2 and the native-test double close were fixed in `56b89a9`;
S3 (CI step) was added. Unverified: first-use passivity of `begin()` over a long run, the class-51 growth path, WOW64.

---

# Appendix: independent ownership/teardown audit (Opus, read-only, verbatim)

## M2 independent ownership/teardown review: native process-handle-count failure

- Repository: /home/dev/Waddle-LSW, branch feature/software-vgpu-slicing, HEAD 52a1a2970c2a182c155c718432a9dee520b0ca72
- Scope: M2 = impl/wsi-window-lifetime (guest WSI resize/loss/retirement). Not software-vGPU TODO #2.
- Mode: read-only. Nothing in the repo was modified; no VM/GPU/guest job was started.
- Legend: **[PROOF]** = established from source, binaries or artifacts. **[INFERENCE]** = reasoned, not demonstrated.

---

## 0. Provenance checks (what was actually run)

- **[PROOF]** The fixture sources at HEAD are byte-identical to the inputs of the failed run:
  `tests/vgpu/tcp_gpu_windows.c` b97fd1c6…, `tcp_gpu_present_windows.inc` 169ba63d…, `tcp_gpu_wsi_lifetime_windows.inc` fe304e5b… (sha256 at HEAD = `build/windows11_vm/tcp_gpu_7adb6000/native_fixture_inputs.json`).
- **[PROOF]** The guest binaries were built from snapshot `build/wddm_dxvk_checkpoint/dxvk_source_9bd62e9f/source` (commit eb1ff4c, `source_snapshot.json`). `diff -rq` of that snapshot's `src/` and `include/` against HEAD's: identical (rc=0).
- **[PROOF]** `gpu.exe`, `waddle_vulkan_experimental.dll`, `waddle_tcp_bootstrap.dll`, `vulkan-1.dll` and `runtime/waddle_vgpu_worker` have identical sha256 in the failed run (7adb6000) and both passing runs (41308226, df62f140).
- **Conclusion:** the failed and passing runs exercised the same code, so the failure has to come from runtime conditions, not from different code.

---

## 1. The oracle, reconstructed from source

- **What is counted.** `native_handles()` calls `GetProcessHandleCount(GetCurrentProcess(), &count)`. That is every kernel-object handle in the process handle table, from any owner: OS, third-party or Waddle. GDI and USER objects are not counted (`tests/vgpu/tcp_gpu_windows.c:50-53`). GDI/USER are checked separately, per probe, at `tcp_gpu_present_windows.inc:322,375-376`.
- **Order of operations before the baseline** (`tcp_gpu_windows.c:764-786`):
  1. `LoadLibraryExA(bootstrap)` (:764).
  2. `venus_tcp_bootstrap_start()` (:771). This reads the config file, checks token/ACL, takes the ICD module reference, runs `WSAStartup` + `socket` + `connect`, the BCrypt nonce, the authenticated hello, and the ICD bind probes.
  3. `LoadLibraryExA(vulkan-1.dll)` (:776).
  4. `initialize_system_graphics()` (:783; body :73-110): DXGI factory/adapter enumeration, `OutputDebugStringA`, `GetPackagesByPackageFamily`, then Sleep(100).
  5. `initialize_presentation_windows()` (:784; `present.inc:284-296`): visible STATIC popup, `DwmFlush`, `GetDC`/`GetPixel`/`ReleaseDC`, `DestroyWindow`.
  6. **Baseline** = one `GetProcessHandleCount` sample (:785). It was 199 in every triangle_present run.
- **Each iteration** (8 for triangle_present; :787-866):
  1. `vkCreateInstance` (:793) and `vkCreateDevice` (:849), through the pinned loader to the Waddle ICD.
  2. `native_swapchain_probe` (:858; `present.inc:306-378`): 2 present modes (:335) × 3 frames (:345) = 6 frames per iteration. Each frame is acquire → triangle render with readback → `vkQueuePresentKHR` → `DwmFlush` → GetDC and 4096×`GetPixel` → `ReleaseDC`.
  3. `vkDeviceWaitIdle`, `vkDestroyDevice`, `vkDestroyInstance` (:862-863).
  4. `handle_baseline(baseline)` (:864, body :54-58).
- **How the check works.** Up to 100 samples with `Sleep(10)` between them, roughly 1 s or more in total. It returns success on the first sample that equals the baseline. If no sample matches, it prints `baseline=… actual=<a fresh 101st sample>` and fails. `bootstrap_stop()` runs only after the loop (:867).
- **42 frames.** 7 iterations × 6 frames = 42. So the failing check was the one after lifetime 7.
- **Code that can run between the baseline and the failing check** (the main thread is the only Waddle-visible thread):
  - Fixture: USER/GDI/DWM calls, `getenv`, `printf`.
  - Pinned Khronos loader v1.4.307 (third party): manifest file I/O, registry and CM_* (cfgmgr32), DXGI factory, `GetPackagesByPackageFamily` (StateRepository RPC), ICD `LoadLibrary`/`FreeLibrary` refcount.
  - Waddle ICD (Zig, plus the C present sink).
  - Waddle bootstrap transport callback (Winsock I/O on the already-open socket).
  - Any OS background thread in the process: RPC runtime, threadpool, TSF/CoreMessaging.

---

## 2. Inventory of Waddle-owned OS-resource acquisitions in the guest process

Import-table closure (`x86_64-w64-mingw32-objdump -p` on the deployed binaries; the Delay Import Directory is empty for all three):

- **`waddle_vulkan_experimental.dll` (ICD)**
  - USER32: GetClientRect, GetDC, IsWindow, ReleaseDC
  - GDI32: StretchDIBits
  - dwmapi: DwmFlush
  - KERNEL32: SRW locks, Sleep, SwitchToThread, TlsGetValue, critical sections, console/WriteFile, VirtualAlloc/Protect/Query, Toolhelp32/Module32*, K32GetModuleFileNameExW
  - ntdll: NtCreateFile/NtCreateSection/NtMapViewOfSection/NtQueryObject/NtLockFile/NtDeviceIoControlFile, RtlQueryPerformance*, RtlWaitOnAddress
  - CRT: malloc/calloc/free/_msize, stdio
  - **Not imported:** LoadLibrary, GetProcAddress, CreateThread/_beginthreadex, CreateEvent, threadpool, timers, DuplicateHandle or OpenProcess. The ICD therefore cannot dynamically resolve further APIs.
- **`waddle_tcp_bootstrap.dll`**
  - WS2_32: WSAStartup/WSACleanup, socket, connect, select, send, recv, shutdown, closesocket, setsockopt, ioctlsocket, …
  - bcrypt: Open/GenRandom/CloseAlgorithmProvider
  - ADVAPI32: OpenProcessToken, GetSecurityInfo, …
  - KERNEL32: CreateFileA, ReadFile, CloseHandle, LoadLibraryExA, FreeLibrary, GetProcAddress, GetTickCount64, LocalFree, plus the same Zig/mingw std set as the ICD.
  - No thread, event, timer or threadpool APIs.
- **Kernel32/ntdll file, section and Toolhelp imports in both DLLs** come from Zig `std.debug` panic and stack-trace machinery.
  - **[PROOF]** `src/vgpu/*.zig` has no `dumpStackTrace`, `SelfInfo`, `std.fs` or `std.log` usage.
  - Every `std.debug.print` in production is gated by `WADDLE_ICD_DIAGNOSTICS` (`venus_icd.zig:532-547,3686,3705,7243,7252,7340`), and `run.cmd` does not set it (`tcp_gpu_actual.py:83`).
  - A ReleaseSafe panic terminates the process, so this path cannot leave a +1 in a live process.
- **Zig std on Windows (Zig 0.13.0)**
  - **[PROOF]** None of these create handles:
    - `std.time.sleep` → `kernel32.Sleep` (`std/time.zig:12-17`).
    - `std.Thread.Mutex` → SRWLOCK (`std/Thread/Mutex.zig:116-126`).
    - `std.Thread.yield` → SwitchToThread.
    - `Timer`/`Instant` → QueryPerformanceCounter.
    - `hasEnvVarConstant` → reads the PEB environment block (`std/process.zig:423-426,490-495`).
    - `threadlocal` → the PE TLS directory (`.tls` is present; no emutls/TlsAlloc).

Per acquisition, in Waddle source:

| # | Resource (kernel-handle?) | Acquire | Release (normal) | Rare / error paths | Reached after baseline? |
|---|---|---|---|---|---|
| 1 | Config file handle (yes) | `tcp_bootstrap_windows.c:101` | `:120` | CloseHandle failure → retained in `pending_config`, retried by `stop`/`start` (`:58-68,157,194`) | No: `start` only, `tcp_gpu_windows.c:771` |
| 2 | Process token (yes) | `:76` | `:95` | close failure → `pending_token` (`:95`) | No |
| 3 | Security descriptor (LocalAlloc, not a handle) | `:74` | `:96` | `pending_descriptor` | No |
| 4 | ICD module ref (not a handle) | `:162` | `:148` (`release_module`) | failure → `release_module` (`:188`) | No. Held across all iterations, so no per-iteration DllMain/CRT churn. The loader's LoadLibrary/FreeLibrary is a refcount on the same file. |
| 5 | Winsock runtime (OS-internal handles on first use) | `venus_tcp_socket.c:112` | `:107` (`socket_close`) | `:114-115` balanced | No |
| 6 | TCP socket (\Device\Afd file handle, yes) | `venus_tcp_socket.c:113` → moved at `venus_tcp_client.c:86-87` | `retire` `:203` / `poison` `:20` / `client_free` `:40` | every `exchange_common` failure → `poison` → close (`:136-159`) | Held (part of the 199). Not created again. |
| 7 | BCrypt RNG provider | `venus_tcp_socket.c:279` | `:281` | both statuses checked | No: `client_init` only (`venus_tcp_client.c:57`) |
| 8 | Per-exchange Winsock I/O (no Waddle-visible handle) | `send`/`recv`/`select` (`venus_tcp_socket.c:135,231,253`) | n/a | `select` timeouts loop (`:120-143`) without allocation | Yes. Any OS per-thread event cache is created on first use, which happened before the baseline (connect/hello/bind probes). [INFERENCE about mswsock internals] |
| 9 | Common DC (GDI object, not a kernel handle) | `venus_win32_present.c:41` | `:61` | extent-mismatch path `:47`; StretchDIBits failure still reaches `:61`; DwmFlush only after release (`:64`) | Yes. Iteration 7's GDI/USER equality passed: the failing stage is "handle retirement" (`tcp_gpu_windows.c:864`), which requires `native_swapchain_probe` to return 0, including the check at `present.inc:375-376`. |
| 10 | Pixel / mapping heap memory (not handles) | `venus_wsi.zig:293` (`defer free` :294); `venus_icd.zig:3388,3419` (c_allocator, `:46`) | defer / `release_shadow` | n/a | Yes |
| 11 | Threads, events, timers, threadpool, sections, DuplicateHandle | none | | | **[PROOF]** none exist in either DLL (import closure) |

Other Waddle components:
- **[PROOF]** `venus_mapping_windows.c`, `venus_stream_windows.c` and `venus_wddm.c` are not linked into these DLLs (`scripts/vgpu.mk:790,805-806`; `scripts/vgpu_tcp.mk:137-141`).
- **[PROOF]** The AV guest agent uses `WINEVENT_OUTOFCONTEXT` hooks and opens only its own target (`src/av/av_windows.c:392-401`). No `DuplicateHandle` exists anywhere under `src/`.
- **[PROOF]** The diagnostic module maps show no injected Waddle module.

Rare-path hunt, checked against the failed run's evidence:
- **Transport poison or timeout did not occur.** **[PROOF]**
  - A poisoned client makes `venus_tcp_client_retire` return `client->lost` (`venus_tcp_client.c:191`), so `bootstrap_stop` fails (`tcp_bootstrap_windows.c:200-202`).
  - The fixture would then print "Retained native session=…" (`tcp_gpu_windows.c:121,878-879`), and the supervisor would write `receipt_proof.json`.
  - Neither is present. The host `result` file shows `worker_retired=1 status=0 worker_exit_status=0`.
- **ICD device loss did not occur.** **[PROOF]** All 6 frames of iteration 7 passed exact pixel comparison, and `device_idle` succeeded before the check.
- **The sink's ReleaseDC-failure and extent-mismatch branches cannot add a kernel handle.** **[PROOF]** DCs are GDI objects, and the iteration-7 GDI/USER equality passed.
- **Polling and wait loops are timing-dependent but allocation-free.** **[PROOF]** `venus_icd.zig:595-612,4898-4909,4935-4956` use only Sleep, SwitchToThread and QPC.
- **No concurrency races are possible in Waddle code.** **[PROOF]** There are no Waddle threads, and the ICD mutex is an SRWLOCK.
- **Result.** No Waddle conditional path that could add exactly one persistent kernel handle exists. The paths that only rare conditions would reach (poison, pending-close retention) were not reached, and those that run every frame acquire no kernel handles.

---

## 3. Evidence from the preserved runs

- **Failed run `tcp_gpu_7adb6000`:**
  - stdout: lifetimes 1–6 PASS; 42 "exact4096RGB/BGRA PASS" lines; baseline=199.
  - stderr: `Native GPU handle baseline=199 actual=200` and `Native actual GPU fixture failed: exact native application handle retirement`. These lines are interleaved mid-line into the loader's buffered stderr, so stream order carries no timing.
  - 7 loader `vkCreateDevice` blocks (8 in the passing runs). The set of distinct stderr lines is otherwise identical to the passing runs: no extra warnings, errors or retries.
  - Native exit 1. Host retired normally. `controller_stdout.log` and `controller_stderr.log` are empty.
- **Handle type and value:** **NOT RECORDED.** The unmodified fixture logs only counts, so no artifact identifies which handle appeared. Each retry sample's value, and the min/max during the 1-s loop, are also unrecorded. The printed "actual" is a fresh sample taken after the loop.
- **Timing asymmetry. [PROOF of the measurement, cause unknown]**
  - Controller-ready to host-result: 7adb6000 took **81.8 s for 7 iterations** plus the 1-s fail loop.
  - The other six triangle_present runs on the same binaries took 66.4–68.5 s for 8 iterations: 41308226 68.5, df62f140 66.6, 400a228b 66.9, ed3d992a 66.4, 6290e08c 67.6, fe13e873 66.4.
  - The failed run was therefore about 20 s slower per comparable work. No guest timestamps exist to localize it: startup, frames or teardown.
  - It ran immediately after `wsi_lifetime` run 683c0ee2 (ended 21:10:50.9, next ready 21:10:51.9).
- **Diagnostic wrapper runs** (4 runs, same fixture with `GetProcessHandleCount` wrapped by snapshot diffs; `build/qualification_20261017/wsi_handle_*/guest_status.json`):
  - **[PROOF]** In every run, two **ALPC Port** handles created during `initialize_system_graphics` close during **lifetime 5**. These are part of the baseline 199 (seq 2: e.g. 0x17C/0x3FC, 0x18C/0x190, 0x1B8/0x1BC). At the same point one **Thread** and one **Event** appear.
  - Net change is 0, so the count stays 199. This equality is coincidental: it holds only because exactly 2 ALPC closes offset exactly 2 additions.
  - The thread/event pair disappears again during the 60-s post-run hold (`wsi_handle_resumed_f7b32b19/runner.log`).
- **No-Vulkan/no-Waddle system control** (`wsi_idle_system_only_b460c22e`):
  - −3 ALPC, +4 Thread/Event, net **+1**, at 59.8 s after the baseline. It stays at +1 through the unchanged 100×10 ms check.
  - The prior write-up attributes the creation frames to RPCRT4 `ThreadSelf` and `PerformGarbageCollection`. **I did not independently re-symbolize those traces.**
- **What this shows:**
  - The baseline itself contains OS-owned, timer-governed transient handles.
  - The RPC runtime's idle cleanup changes the count during the workload, and its net effect is variable: 0 in the GPU diagnostics, +1 in the control.
  - These states persist far longer than the 1-s retry window.
  - Additional asynchronous OS input-stack modules are loaded in the process: `msctf.dll`, `textinputframework.dll`, `coremessaging.dll`, `coreuicomponents.dll`, `windows.staterepositorycore.dll`, `cfgmgr32.dll` (module_map_after.json).
- **Limit:** none of this retrospectively identifies the 7adb6000 handle.

---

## 4. Verdict

**(b) No Waddle-owned path can explain the 199→200 delta. The oracle is sensitive to OS-owned handle churn.**

- **[PROOF] Waddle cannot create the handle directly.**
  - The only Waddle code executed between the baseline and the failing check is ICD Zig code, `venus_win32_present.c` and the bootstrap transport callback.
  - That code imports no handle-creating API except panic/stack-trace machinery, which is process-terminating.
  - Every pre-baseline acquisition (#1–#7) is either released before the baseline or held unchanged inside it.
  - The rare paths that would release or retain differently (poison, pending-close) provably did not run in 7adb6000.
- **[INFERENCE, residual] Indirect OS side effects cannot be ruled out statically.**
  - OS-internal state created as a side effect of Waddle's legitimate OS calls (Winsock `select`/`send`/`recv`, `DwmFlush`, `GetDC`) cannot be excluded without tracing.
  - Those resources would be OS-owned and OS-cached. Waddle does not hold them and must not close them.
  - The same calls are made before the baseline (Winsock), or by the fixture itself every frame (`DwmFlush`, `GetDC`).
- **[PROOF] The count oracle can also miss a real leak.** Count equality can mask a Waddle leak exactly offset by an OS close; the lifetime-5 swap shows that offsets of this size happen. A count-only gate is neither sound nor complete for Waddle ownership.
- **No production fix is proposed.** No root cause was found in Waddle code, so none is justified. The forbidden remedies are not proposed.

### 4.1 External decision required

- **Who decides:** the M2 acceptance owner, meaning the user/maintainer who owns `impl/wsi-window-lifetime/IMPL_DESC.md` verification criteria and the AGENTS.md §5 merge checklist. Agents may not redefine acceptance.
- **What must be decided:** whether M2 native acceptance keeps the process-global `GetProcessHandleCount` equality as written. As written, it is blocked on this Windows image by OS-owned churn, and repeated passes do not waive 7adb6000.
- **Options, described without endorsement:**
  - **A. Keep the oracle unchanged.** M2 stays blocked on this platform. Future failures should at least be made attributable (§4.2). The historical 7adb6000 failure remains unattributed by any existing artifact; how to record it is itself an owner decision.
  - **B. Owner-approved amendment to an attribution-aware strict oracle.** This must be explicitly recorded in IMPL_DESC/TRACKER as a criteria change, not a waiver.
    - Take full handle-table snapshots (`NtQueryInformationProcess` class 51) at baseline and at every check, with handle tracing (class 32) enabled from process start.
    - Fail if any added or removed handle has a creation or close stack containing a Waddle module (`waddle_vulkan_experimental.dll`, `waddle_tcp_bootstrap.dll`).
    - Fail on any delta in handle types Waddle can create directly (file, token, CNG/AFD file handles).
    - Fail on any delta whose stack is unavailable or unresolved (fail-closed).
    - Require the global count delta to be fully explained by records attributed to OS-only stacks. Otherwise fail.
    - This stays exact for Waddle-owned handles. It is strictly stronger than the current count because it detects masked swaps. It does change what "acceptance" measures, so it needs the owner.
  - **C. Change the test platform's OS configuration** (RPC/TSF behavior) or the image. This is a system-settings decision for the owner. It changes the platform under test, so it is not a remedy proposed here.

### 4.2 Non-weakening diagnostics the fixture could carry

The assertion, baseline point, budget and workload all stay unchanged with these additions:

1. At the baseline (`tcp_gpu_windows.c:785`), store a class-51 snapshot: handle value, type index and granted access. Map type indices to names via `NtQueryObject(ObjectTypeInformation)`.
2. In `handle_baseline` (`:54-58`), keep every sample's count. On failure only, print:
   - the last compared value, min and max, and number of samples;
   - elapsed ms since the baseline (QPC);
   - the iteration index;
   - a full added/removed diff against the baseline snapshot with type names. Names should be queried only for Key/ALPC/Section/Event/Mutant/Semaphore; avoid ObjectNameInformation on File handles, which can block on synchronous pipes.
3. Optionally enable process handle tracing at startup and, on failure only, print creation/close stacks as module+offset for the diff entries, plus a module map for offline symbolization.
4. Print a per-iteration QPC timestamp, to correlate with OS idle-cleanup timers and explain wall-clock anomalies like 7adb6000's +20 s.

Steps 1, 2 and 4 are passive reads. Step 3 adds kernel-side bookkeeping only (no process-table handles) and should be documented as such. None of these change any assertion.

---

## 5. Other latent defects in M2 scope (separate from the handle question)

Production code:
1. **`src/vgpu/venus_win32_present.c:58-63`: StretchDIBits return-value check (inference from Microsoft's StretchDIBits documentation; not verified natively).** Success is checked as `copied == (int)height`, but the return value can be negative for mirrored content. On a `WS_EX_LAYOUTRTL` (RTL-mirrored) application window a successful copy would be reported as failure. `venus_wsi.zig:312-315` then marks the surface permanently `SURFACE_LOST`. Needs a native RTL-window test and a fix that accepts `abs(copied) == height`.
2. **`venus_win32_present.c:64` with `venus_wsi.zig:312-315`: FIFO pacing failure becomes permanent surface loss.** A `DwmFlush` failure after pixels were already copied and the DC released is reported as 0, which makes the surface sticky lost. A transient DWM condition (desktop switch or secure desktop) would permanently lose the surface. IMPL_DESC lists pacing failure as a native failure, so this is intentional, but it is a design risk worth re-confirming. Low severity.
3. **`src/vgpu/venus_icd.zig:713-756`: instance destruction leaves leaked surfaces in the registry.** `destroy_instance` does not retire `wsi_state` surfaces owned by the instance. They persist until `clear()` (`:392`) at unbind. With `MaxSurfaces=16` (`venus_wsi.zig:8`), an application that leaks surfaces (invalid usage) exhausts the registry for the rest of the binding. Low severity; no OS handle involved.

Fixture (`tests/vgpu`), not production:
4. **No message pump.** The window-owning thread never retrieves messages: gpu.exe imports no PeekMessage/GetMessage/DispatchMessage. Windows are created at `present.inc:287,325` and `wsi_lifetime.inc:199,368`, are visible and topmost, and `present.inc:326` activates without `SWP_NOACTIVATE`. Posted messages accumulate, windows can be classed as not responding after about 5 s, and asynchronous TSF/CoreMessaging traffic is never drained. This is a source of OS-side nondeterminism in a test that asserts process-global equality. **[INFERENCE: not shown to cause the +1.]** Whether to change it is an owner decision; it is not a baseline prewarm or a budget change.
5. **Double `ReleaseDC` on failure.** `present.inc:359`: when `ReleaseDC` fails, the code jumps to cleanup with `dc` still set, and `:368` calls `ReleaseDC` on the same DC again. Fix: clear `dc` before testing the result.
6. **Stale HWND after a successful destroy.** `wsi_lifetime.inc:385-388`: after `DestroyWindow` succeeds, `window` stays non-NULL until the post-checks pass. If a post-check fails, cleanup (`:436`) calls `DestroyWindow` on a destroyed, possibly reused, HWND value. Fix: NULL it immediately after a successful destroy.
7. **Weak failure diagnostics in `handle_baseline`.** `tcp_gpu_windows.c:56-57`: a `GetProcessHandleCount` failure returns 1 with no message. The failure message reports a fresh 101st sample rather than the compared samples. This is diagnostic weakness only (see §4.2).

---

## 6. Limits of this review

- No Windows execution was performed. Claims about mswsock, DWM and RPC internals are marked as inference.
- RPC frame attributions from earlier receipts were not re-symbolized.
- The vendored Vulkan loader was reviewed only for its Windows registry/DXGI/package paths (`submodules/vulkan_loader/loader/loader_windows.c:173-234,419-572,939-1023,1098-1160`); those are balanced per call. The loader is third party, not Waddle-owned.
- The cause of the 7adb6000 wall-clock excess is unknown.
