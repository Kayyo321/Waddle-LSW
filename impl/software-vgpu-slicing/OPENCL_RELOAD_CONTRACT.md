# OpenCL Full Runtime Reload Qualification Contract

Root approved 2026-10-21: preserve ordinary eight-context mode, existing 1..8-cycle heap audit mode (three full reloads used for qualification), and transport-loss mode. Add `--runtime-reload` with exactly 1100 full module lifetimes in one process. Each reload runs the unchanged full `run_lifetime` once serially (with shutdown/version/busy/context/event mismatch probes) and once on each of two raw Win32 callers created and parked before candidate DLL loading. Total3300 actual exact GPU context lifetimes. Four additional raw idle threads predate the first load and remain alive across all1100 unloads. No first-load heap acceptance is inferred: records/verifier explicitly label heap qualification separate and mandatory.

Owned paths are limited to tests/vgpu/opencl_integration.c, scripts/vgpu_dxvk.mk, and new tests/vgpu/opencl_reload_windows.inc, tests/vgpu/opencl_reload_test.c, tests/vgpu/win32_fake/opencl_reload_fake.h, tests/vgpu/opencl_reload_output_test.py, tests/vgpu/opencl_reload_coverage.py, scripts/opencl_reload_output.py. Before editing existing paths copy byte-exact current main snapshots (opencl source b238878edb8a5c6e08fcf162fd828f6d9fdea62d7faa21b44f94521916565553; make18caee0fb34e015937a9e0a275bcc4109074da4162bd0c9e128f7342128a39c5). Root owns docs, CI integration, Git and all native execution. No production/runtime/build-driver/probe/pin changes. Before revision2 freeze, root authorized rebasing only the additive fixture changes over its corrected transport-loss teardown base b9d28f7e496daa703dd99151d11b19986da1466b5af2de0ca122aba394e4ef93; that current-main source is preserved in base_revision2 and byte-exact inverse proof must recover it. Earlier pre-correction sources and receipt remain preserved as superseded.

New support include owns bounded TLS/FLS exhaustion probes (capacities2048/4097, reverse retirement, no successful unfreed index), measured process count, fixed thread pools/events and mode orchestration. API-double tests compile and exercise the actual include, not a parallel model. Persistent thread pool contains4 threads, each with ready/go events; ephemeral foreign pool contains2. Parent-owned context storage outlives all joins. Publish callbacks/borrowed arguments before SetEvent; never change them after release. Setup errors signal cancelled/null callbacks and join all created threads before returning. Close or wait failure that prevents proven safe retirement terminates the test fail closed; it never authorizes module unload. Borrowed DLL callbacks/objects are cleared only after thread exit; borrowed thread identities are never closed.

Stress establishes measured entry and persistent-fixture baselines before any candidate load. TLS/FLS slots are exhausted then fully released, and handles measured with no marker/probe owners pending. Every allocation clears then immediately captures GetLastError. The API-specific TLS_OUT_OF_INDEXES/FLS_OUT_OF_INDEXES sentinel means allocation failure, not proof of exhaustion. This qualification initially permits only sentinel plus ERROR_NO_MORE_ITEMS as its explicit target policy; native controls must calibrate that policy before stress. Any other status, including unchanged ERROR_SUCCESS, rejects after every acquired index has been retired and reports API kind, captured error and partial count. Microsoft does not document ERROR_NO_MORE_ITEMS as a universal guarantee for these APIs. Capacity reached without a qualifying sentinel fails. No alternate error acceptance is inferred from a native failure. Exact persistent baseline handles/TLS/FLS must hold before and after EVERY reload including first, with no warmup, allowance or baseline reset. After final joins/event closes/CRT unpin, entry baseline must also return. Persistent idle wait is unbounded by design but its join is bounded; per-cycle native work and host handshake retain existing210-second/60-second limits. No live thread may survive a return to module teardown.

`run_native_work` gains only a stress-conditional path: start2 raw callers before LoadLibrary, load the exact graph and verify returned module paths, start authenticated session, perform1 serial lifetime then release2 foreign callers for1 lifetime each, join/close them, retain existing terminal shutdown/bootstrap stop. Transfer modules to existing outer parent, join its CRT worker, unload in original order, prove module absence and restore environment. Failure cancellation runs before any stack dispatch or module can die. Existing ordinary2serial+2x3 and cancellation logic remain reachable unchanged when stress flag is off.

Stress requires a fresh audit directory using the existing stage mechanism. At `reload_before_N` the root supervisor proves prior host retirement and starts exactly one new receiver before acknowledging. `reload_after_N` occurs only after exact count/module/thread checks; root archives current host proof and verifies its authenticated session. Root must not reuse stale acknowledgement/config/retirement evidence. `reload_cleanup` is a final capture after persistent owners close. Native success appears only after1100 ordered cycles and final cleanup, then natural exit0; intermediate cycle records are provisional.

Protocol `OPENCL_RELOAD_V1` JSON lines bind PID/creation, exact requested OpenCL module path encoded as UTF16 code units, fixed workload counts, per-cycle monotonic index/session/two distinct foreign TIDs, raw measured handle/TLS/FLS counts and module-path/absence proof. Standalone TLS/FLS negative-control modes allocate one explicitly owned slot, require the SAME baseline comparison to reject, free that exact owner, then require full restoration; they cannot emit stress success. Output verifier requires real native exit0, exact ordered1100-cycle shape, both streams, no contradictory fixture failure, and source-bound candidate/host proof manifest. It does not treat expected CL compiler-error output as generic failure, and does not confer heap acceptance.

Portable controls cover index exhaustion bounds, allocation/free errors, count/identity/module/path failures, thread/event creation/readiness/join/close cleanup, cancelled parked workers, wrong/failed callbacks, retained TLS/FLS detected despite handle equality, every orchestration failure stage, truncated/reordered/duplicate/mixed-session output, missing host retirement/candidate bindings, and native nonzero exit. All actual support functions need execution with >=90% lines/branches and zero ASan/LSan/UBSan errors; raw gcov and failed attempts retained. Strict Windows cross-build is required, not claimed as native execution. Qualified final DLL source identity inputs remain untouched.

Revision3 strengthens only output evidence and Make gates. The verifier requires independent pre-run expected SHA256 command arguments for both candidate and fixture; actual local files and mutable manifest must agree with those frozen values. Root freezes those arguments before native execution and independently records pre/post guest binary hashes. A manifest/current-file pair cannot self-authorize a substituted binary. The earlier receipt is preserved, not retrospectively upgraded.

Each host proof includes a complete historical graph (controller, worker, renderer parents/children as actually observed), exact process identities and all pidfd owners. State S/D remains historical, and open:true in that snapshot means the supervisor still owns the pidfd. Every unique member must have one matching top-level retired_members record with fields pid,starttime,verified_absent:true,observed_starttime:null,group,group_empty:true,session,session_empty:true,query_error:null. PID reuse, duplicates, missing members/owners/retirement proofs or incomplete groups/sessions reject. Every adopted_reaps identity must match a member; nonzero historical child wait status is not itself rejected.

After actual supervisor handle closure, top-level owner_close must contain owner_pid,all_pidfds_closed:true,error:null,pidfd_owners. Each closed-owner record has pid,starttime,descriptor,closed:true and must form an exact bijection with the earlier open pidfd ownership snapshot. No process absence or handle closure is inferred from list membership alone. Root performs the actual queries/closure, while the verifier validates their source-bound frozen receipts. New tests include the real four-member history shape and malformed/extra/missing/reused identities, closure failures, frozen-hash omission and mutable binary+manifest replacement. The127line/93.4343% coverage metric covers only opencl_reload_windows.inc; no full native main orchestration or Windows execution coverage is claimed.

## Root integration and native calibration state

Integrated Astra revision3 receipt SHA256
eb3da537a1927fc0bb10836fd620bd45eca99a2ddbecda6fcb52270adb01030a:
`build/qualification_feature_20261021/opencl_reload_integration/`. Root rehashes
all102 bindings, verifies inverse preservation of the ordinary/cancellation
paths and runs portable/sanitizer/coverage gates. API doubles pass4groups/4732
checks; strict output parser passes12groups in normal/optimized Python. Actual
support include127/127lines and185/198branch outcomes (93.43%) pass; all12
functions execute. These metrics do not cover the full Windows orchestration.

Fresh strict Windows cross-build uses the reviewed private compiler14 with
separate dependency generation for each translation unit, union297 inputs,
and before/after hashing. Fixture SHA256
d084a2543c013072ef3608d30ebfd132638c23b41e02ae64753acc56bb926cf0;
receipt c46c491091268662a6e53a31b4121f932cc1076b30a7d2ab33a2e81ec1b49aca,
`build/qualification_feature_20261021/opencl_cancel_fixture_54f3bfa9/`.
This is cross-compilation evidence only. No production DLL inputs changed.

Native controls `opencl_reload_controls_native_c3cdcd92` fail closed before
OpenCL/GPU loading: this target returns error8 after1086 TLS acquisitions,
not the provisional259. Both TLS/FLS control invocations stop at the initial
TLS baseline, so FLS exhaustion has not been measured. Ten malformed CLI cases
naturally exit2. No surviving process or forced cleanup. No successful control
receipt and no1100-cycle run exist. Matching OS disassembly shows status8 can
mean either full bitmap or lazy internal allocation failure; accepting error8
alone would not prove exhaustion. Strong target-specific capacity evidence
and fresh controls are required before stress. No threshold/status fallback
or acceptance allowance is authorized by these failures.

Heap qualification remains separate. Overall85.5%, M2 94% Review, all task
deltas0, Time Ended TBD. CI adds all three portable reload gates; Windows
cross-build is not native execution.


## First real full-candidate execution and failure diagnostics, 2026-10-21

The native TLS/FLS controls were subsequently qualified by exact target-specific
structural certificates: see [TLS](OPENCL_SLOT_CERTIFICATE.md) and
[FLS](OPENCL_FLS_CERTIFICATE.md). Both controls and ten malformed CLI cases pass
with fixture65846c143675b32c884494e2278a9c079c32903a679b14cc5a00215e960e17bd,
complete300-input/two-TU receiptfb37aef4cbf631c9435f52a05393f7fc018d8868d00c4bd8a1e6c3b96891fe09.
These results supersede the earlier failed error8 assumption only; they do not
supply a full-candidate reload or heap pass.

Root run `build/qualification_feature_20261021/opencl_reload_final_4688cf8_revision6`
uses that fixture and full OpenCL DLL70ca4fdbc2ca54954b7879f28f11e013c63a0a660cca718815abc24e98d5887a.
It executes three exact GPU context lifetimes on RTX5080, including malformed
buffer and event oracles, joins the two pre-load foreign callers, and prints
successful shutdown/session-stop/module-retirement witnesses. Its first
post-cycle check then rejects before the cycle record or after-stage. Native
PID4656/creation01dd61a72d95f06d and its exact signed console naturally exit1;
no process is forced to retire. The one host sessiond137330910f87f4f naturally
returns controller0/worker0, with no supervisor signals, an empty owned graph
and all four pidfds closed. Raw stdout/stderr, exact native identity, failed
owner graph, final closed journal and root retirement review remain preserved.
**Zero completed strict reload cycles are credited.** The original combined
predicate omitted its failure reason; this evidence does not establish which
count/result field caused rejection. A fixture/CRT initialization explanation
is a hypothesis requiring a separate native control, not a finding or waiver.

Earlier exact CLI/OS preflight failures are preserved in the revision4/5
folders. Runner corrections bind actual numeric version resources, use one
absolute canonical argument generator, retain coalesced stage output across
polls, compare exactly eight tokens with only ASCII separator flexibility,
and retain native/signed-parent identity checks. They grant no acceptance.

The diagnostic unit changes only the reload support include and its API-double
test. `ReloadRejected` evaluates each original condition exactly once and emits
`OPENCL_RELOAD_DIAG` to stderr only on that existing rejection. Records identify
stage/cycle/predicate, raw entry/baseline/observed handles/TLS/FLS, and the existing
session, caller IDs and module-result fields. Raw observations may be partial
or stale if their query failed; consumers must not treat them as a fresh
measurement. No condition is waived, no successful path prints a diagnostic,
and no diagnostic introduces a resource owner or vendor call.

Frozen review artifact: `build/qualification_feature_20261021/opencl_reload_failure_diagnostic/`,
receiptSHA9fc9bb7864c3d853ba27aa471bb6a422c58e4bbdfbf8a0dd769a9fd84ea5cfe2.
Root verifies all345 bindings, including300 Windows compiler dependencies,
reviews the two-file patch and source inverse proof, and reruns its exact
sanitized binary with leak detection enabled outside worker ptrace. Both that
run and `make vgpu-opencl-reload-test vgpu-opencl-reload-sanitizers vgpu-opencl-reload-coverage`
pass. Five groups/9291 checks include exact failure predicates and separate
handle/TLS/FLS deltas; the unchanged12-function coverage inventory reaches
142/142 lines and188/202 branches (93.07%, minimum90). The inverse proof
recovers every original predicate/token and exact original include bytes.
Worker ptrace/cross-build setup failures remain failed records. Cross-compiling
the diagnostic fixture is not a native rerun. The full DLL/build receipt and
production inputs are unchanged. Fresh native diagnostics, full1100-cycle
qualification, heap ownership, final CI and independent review remain open.

Native R7 diagnostic outcome: [measured first-cycle retention](OPENCL_NATIVE_RETENTION_2026_10_21.md). The exact post-cycle comparison rejects+83handles/threeTLS/threeFLS; zero strict cycles accepted. No count or ownership waiver follows.

## Approved amendment: OS-only pre-initialization (2026-10-22)

The user approved this amendment on 2026-10-22 after the retention
[attribution](OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md) showed that Windows keeps
process-lifetime state after the first use of USER32, Winsock, bcrypt and DXGI.

1. **Exact operations.** Before the entry baseline, the fixture performs these four steps,
   in this order, and nothing else:
   - Load and free System32 `user32.dll`.
   - Load System32 `ws2_32.dll`, call `WSAStartup(2.2)`, open one IPv4 TCP socket, close it,
     call `WSACleanup`, then free the module.
   - Load and free System32 `bcrypt.dll`.
   - Load System32 `dxgi.dll`, create one `IDXGIFactory1`, release it, then free the module.
     There is no adapter enumeration.
2. **No product code.** No Waddle, clvk or Vulkan loader code runs during pre-initialization.
   All modules load with `LOAD_LIBRARY_SEARCH_SYSTEM32`. Every module reference, Winsock
   startup, socket and COM reference it acquires is released before return on every path.
   Any failure stops the run before the entry baseline is measured.
3. **Equality is unchanged.** The entry and persistent baselines are measured after
   pre-initialization. Exact handle/TLS/FLS equality is still required before and after
   every one of the 1100 reloads, including the first candidate load, and the final cleanup
   must still return to the entry baseline. No allowance, tolerance or baseline reset is added.
4. **Evidence.** The baseline record states the completed operations, and the output verifier
   requires that statement.
5. **Proof first.** Before any reload run uses it, a diagnostic census must show that an
   ordinary eight-context GPU life after pre-initialization leaves no handle, TLS or FLS delta.
   Heap-audit mode enables pre-initialization only through an explicit diagnostic switch, so
   heap qualification is unchanged unless separately approved.
6. **Upstream loader.** The Vulkan loader's leaked `dxgi.dll`/`gdi32.dll` references are not
   approved for change here; any loader patch needs its own approval.

## Approved amendment 2: two more OS steps and the loader patch (2026-10-22)

The diagnostic census after the first amendment left +34 handles (see
[attribution sections 7-8](OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md#7-diagnostic-census-after-the-approved-pre-initialization-2026-10-22)).
The user then approved two more OS-only steps and one upstream change on 2026-10-22.

1. **Step 5, standard-search resolution.** Load System32 `dwmapi.dll` by bare name with
   `LoadLibraryW`, so ntdll resolves one module through the default search order and opens its
   `Session Manager` key, then free it. A full-path load does not trigger this, so the bare name is
   required. Because a bare name searches the application directory first, the step fails closed
   unless the loaded module's path is exactly System32 `dwmapi.dll`.
2. **Step 6, package-family query.** Call kernel32 `GetPackagesByPackageFamily` once for
   `Microsoft.D3DMappingLayers_8wekyb3d8bbwe` with no output buffers (size query only). Only
   `ERROR_SUCCESS` and `ERROR_INSUFFICIENT_BUFFER` are accepted. kernel32 is borrowed with
   `GetModuleHandleW`; no reference is taken.
3. **Loader patch.** Apply the 76-line Vulkan loader correction that stores the `gdi32.dll` and
   `dxgi.dll` references taken by `windows_initialization` and frees them in `loader_release()`,
   through a tracked `patches/vulkan_loader/` private build. The submodule pin does not change.
4. **Regression reruns.** Every Windows workload that loads the patched `vulkan-1.dll` is
   requalified on it: the M2 regression workloads, the Windows physical matrix and the DXVK checks.

The baseline witness becomes `user32,winsock,bcrypt,dxgi_factory,search_order,package_family`.
All other rules of the first amendment are unchanged, including exact equality on every reload.

## Approved amendment 3: loopback connect and loader logging (2026-10-22)

Censuses R7-R9 ([attribution section 9](OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md#9-six-step-pre-initialization-with-the-patched-loader-2026-10-22))
left one IoCompletion created by Winsock's first nonblocking connect. The user approved two
changes on 2026-10-22.

1. **Step 7, nonblocking loopback connect.** Load System32 `ws2_32.dll` and call `WSAStartup(2.2)`.
   Open one listener bound to `127.0.0.1` on an ephemeral port and listen. Open one client socket,
   set `TCP_NODELAY` and nonblocking mode, and connect it to the listener. Accept only an immediate
   success or `WSAEWOULDBLOCK`. Wait with `select` (bounded to 2 seconds) until the client is
   writable. Then close both sockets, call `WSACleanup` and free the module. There is no accept and
   no data transfer, and no Waddle, clvk or loader code runs.
2. **No loader debug logging in qualification batches.** Qualification batches no longer set
   `VK_LOADER_DEBUG`. The loader copies every emitted message to `OutputDebugString`, which makes
   Windows open and cache `DBWinMutex`, and no qualification check reads loader log output.

The baseline witness becomes
`user32,winsock,bcrypt,dxgi_factory,search_order,package_family,winsock_connect`. All other rules of
amendments 1 and 2 are unchanged, including exact equality on every reload.

## Amendment 4: explicit driver lists skip DXGI, step 4 loads dxgi.dll only (2026-10-22)

R9 failed cycle 3 with one Thread and one Event. Kernel handle tracing attributes both to RPCRT4
per-thread state created by a thread-pool timer callback
([attribution sections 13-14](OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md#13-reload-r9-and-the-rpc-timer-residual-2026-10-22)).
The user approved evaluating a loader patch on 2026-10-22 and told Claude to work autonomously and
resolve roadblocks itself. Under that instruction Claude integrates both changes below. Neither one
alters a predicate, threshold, baseline or count policy.

1. **Loader patch 0002.** When `VK_DRIVER_FILES` or `VK_ICD_FILENAMES` is set, which is the same
   override that already makes the loader skip the registry, `windows_initialize_dxgi` and
   `windows_read_sorted_physical_devices` create no DXGI factory. The loader behaves exactly as it
   already does when DXGI factory creation fails: no physical-device sorting feature and no sorted
   list. The fixture always sets `VK_DRIVER_FILES` to the Waddle manifest.
2. **Step 4 narrowed to a module load.** Step 4 now loads System32 `dxgi.dll` and frees it. It no
   longer creates and releases an `IDXGIFactory1`. A native per-step control shows that factory
   creation is the only pre-initialization step that opens RPC/ALPC client connections, and that a
   plain load and free opens none. With patch 0002 no cycle creates a factory, while the loader still
   loads and frees `dxgi.dll` each cycle. The narrowed step is a strict subset of the operation the
   user approved in amendment 1.

3. **Steps 8-10.** Without the factory, the first life left three OS first-use residuals that the
   factory used to absorb ([attribution section 14](OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md#14-loader-patch-0002-and-the-ten-step-pre-initialization-2026-10-22)).
   Each new step repeats, once, an operation a cycle already performs. Each runs only Windows code
   and releases everything it acquires.
   - **Step 8, user hive.** Load System32 `advapi32.dll`, open `HKEY_CURRENT_USER\Software` for
     reading and close it, then free the module. The loader's settings lookup reads HKCU, and the
     first current-user access opens and caches the `HKU\<SID>` key. The predefined
     `HKEY_CURRENT_USER` handle is never closed.
   - **Step 9, random.** Load System32 `bcrypt.dll`, open the `RNG` provider of
     `Microsoft Primitive Provider`, generate 16 bytes, close the provider and free the module. These
     are exactly the calls `venus_tcp_random` makes. The first open keeps `bcryptprimitives.dll`
     loaded with two ETW registrations and one device file.
   - **Step 10, D3DKMT adapters.** Load System32 `gdi32.dll`, call `D3DKMTEnumAdapters2` once with a
     zero count and no array, then free the module. The loader's driver-store implicit-layer scan
     makes this call, and the first one keeps `dxcore.dll` loaded with two ETW registrations.

A native per-step control shows that none of the ten steps opens an RPC/ALPC connection.

The baseline witness becomes
`user32,winsock,bcrypt,dxgi_module,search_order,package_family,winsock_connect,user_hive,random,d3dkmt_adapters`,
and verifiers reject both the previous `dxgi_factory` witness and the seven-step prefix. All other
rules of amendments 1-3 are unchanged, including exact equality on every reload.
