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
