# Native OpenCL Transport-Loss Qualification Contract

This contract defines the opt-in `--transport-loss` mode of
`tests/vgpu/opencl_integration.c`. The existing six-path invocation remains the
ordinary OpenCL workload and retains its current assertions and cleanup path.
The fault mode is a separate supervised scenario and does not count as
acceptance unless the bridge and supervisor provide the independent witness
and receiver-retirement proof described here.

## Invocation and ownership

The executable accepts:

```text
opencl_integration.exe --transport-loss <loader> <OpenCL.dll> <manifest> <bootstrap> <private-config> <retirement-receipt>
```

All six paths are absolute Windows paths. The receipt path is fresh and trusted
by the supervisor, using the same exact `retired_session=%016llx\n` contents
and file validation as the ordinary test. The manifest path selects the
existing exact OpenCL implementation. The parent owns the process environment
snapshot, test invocation, bridge cancellation and final process/module
observation.

The application loads and retains the loader, OpenCL DLL, and bootstrap DLL.
It starts the authenticated session, discovers the configured RTX 5080, and
creates one context, one command queue, one program, one kernel, one buffer,
and an owned kernel event. These objects remain alive across the induced
transport loss. The fault worker owns only the enqueue/read call arguments and
their output fields. The parent does not read those fields or release any
OpenCL owner until it has joined the worker.

## In-flight witness and cancellation

The worker enqueues one real kernel and immediately calls
`clEnqueueReadBuffer(CL_TRUE, ...)` on the same buffer. This is the call whose
completion is tested. The call enqueues/flushes the work and waits for the
command event; the bridge witness is required to establish that the call has
reached the transport operation. API-entry logging is diagnostic only and
does not establish in-flight status.

The qualification-only TCP bridge writes one fresh marker after it has fully
received and validated the current `RequestSubmit` containing the submit
command, immediately before calling the guest worker exchange. It records
exactly:

```text
WADDLE_OPENCL_CANCEL_GATE_V1
session=<16 lowercase hexadecimal digits>
sequence=<decimal outer request sequence>
opcode=18
pid=<decimal host process ID>
```

The marker is created exclusively at a fresh path. Its session must equal the
session printed by the OpenCL process; its opcode must be 18 (`vkQueueSubmit`).
The bridge waits only to that request's original absolute deadline and observes
the same controller cancellation atomic. It does not forward a reply once the
gate is armed. The supervisor waits for this exact marker, cancels the matching
owned session, then independently proves the exact worker is reaped and writes
the existing retirement receipt. A timer, client API-entry marker, unrelated
submit, generic socket close, or test-generated synthetic request is not a
substitute for the bridge marker.

## Result mapping

The only accepted result for the blocking read is exactly `CL_OUT_OF_RESOURCES`.
The current clvk queue implementation returns this when a Vulkan submit or
queue-wait operation fails: `cvk_command_buffer::submit_and_wait()` reports
false, and `cvk_command_batchable::do_action()` converts that to
`CL_OUT_OF_RESOURCES`. `wait_for_events()` propagates the failed command-group
status to the blocking read. `CL_SUCCESS`, any unrelated error, a missing
joined status, or a missing bridge witness fails qualification.

`clFinish()` is not used as the failure oracle: its current implementation can
discard the command-group result and return `CL_SUCCESS` after a failed event.
The test does not perform a normal queue finish or attempt further work after
the read reports the loss.

## Retirement and teardown

After the worker returns, the caller joins it within 65 seconds, checks that
the shutdown entrypoint reports `ClvkShutdownBusy` while the CL owner graph is
still live, and only then reads its status fields. A non-Busy result fails
closed without attempting cleanup. A timeout fails closed by terminating the test
process; no DLL is gracefully unloaded while the API thread might still execute
inside it. Retirement proof has a bounded wait as well; on timeout the process
terminates without abandonment or graceful module unload, leaving actual worker
reaping to the supervisor. The receipt deadline is 90 seconds, keeping the
65-second API wait, receipt wait, and at most five seconds of terminal-shutdown
retry within the parent worker's 180-second join budget; module absence has a
separate ten-second bound.

For a completed API call, the process retains all CL owners and module
references until `wait_retired_bounded()` validates the exact matching session receipt.
It releases local OpenCL owners in reverse order while the loader's ICD
Vulkan dispatch objects remain valid: event, kernel, buffer, program, queue,
context. It then requires terminal `waddle_clvk_shutdown()` success within
five seconds, verifies idempotent success and `CL_INVALID_PLATFORM`, and only
then calls bootstrap `abandon(session)` exactly for the captured session.
The selected fixture uses only buffers and no context destructor callback or
mapped memory. Exact host reap permits SDK cleanup after transport failure;
it does not permit clearing the loader's device table before SDK destructors.
The ICD's sticky loss prevents stale transport work while local destruction
and SDK executor retirement complete. Abandon subsequently clears any
remaining poisoned local ICD records and releases the bootstrap module owner.

If the API worker started but failed before the blocking read returned, the
same exact receiver-reap proof precedes all CL destructors: enqueue may already
have issued a submit. For a setup failure before any API worker starts, local
CL owners and the SDK retire first, followed by normal bootstrap stop; a failed
stop still needs the exact original session receipt before abandon. Failed
CL releases remain failures. Missing receipt or failed/Busy shutdown never
authorizes abandon or graceful module unload.

After successful SDK retirement and abandonment, the process returns retained
module references to the parent. The parent joins the outer worker with its
existing finite deadline, unloads OpenCL before loader/bootstrap, checks module
absence within ten seconds, and restores the original environment. No API call
or SDK executor may survive into module unload.

## Source basis and limits

The result mapping follows the pinned clvk `src/queue.cpp` blocking wait,
submit-and-wait and command execution paths. The private shutdown ledger counts
all `refcounted` allocations and permits terminal teardown only when only root
devices remain and the executor pool is idle. The ICD's post-reap `abandon`
clears guest object tables; it does not release clvk C++ owners, which is why the
OpenCL release calls must precede shutdown, and shutdown must precede
bootstrap abandonment.

This mode is limited to one buffer-backed kernel submission and a transport
loss at the host bridge's actual submit request. It does not prove cancellation
of every OpenCL API, host Vulkan execution after submission, or arbitrary device
loss behavior. Native acceptance requires separate evidence for the exact
bridge marker, CL result, worker reap/receipt, abandonment, terminal shutdown,
and module retirement.

## Source-bound portable and argument controls (2026-10-21)

The portable test extracts the exact struct and both call/release definitions
from the actual native fixture using the existing brace-aware extractor. It
hashes and rechecks the full integration source, generated header, fixture,
extractor, script and compiler/gcov tools. It checks exact enqueue/read arguments,
borrowed output/event pointers, enqueue failure suppressing read, copied read
failure, reverse release order, null owners and each individual release failure
while all other owners retire and every handle clears. The generated header's
two actual functions cover17/17 executable lines and26/26 branch outcomes,
with100% per-function lines and unchanged90% floors. Root runs both Python modes
with ASan/LSan/UBSan and explicit non-suppressed options; both pass. Prior worker
LSan-under-ptrace failures remain preserved and are not zero-leak evidence.

Root independently cross-built Debug and optimized Windows fixtures with strict
warnings. Ten native Windows invalid-shape/path/audit-mode cases each exit2
naturally with no survivors, before OpenCL/GPU loading. Evidence:
`build/qualification_feature_20261021/opencl_cancellation_root/` and
`opencl_mode_native_fd4c4546/`; unique portable receipts under
`opencl_cancellation_subagent_*/receipt.json` record all source/tool and gcov
bindings. Make/CI run `vgpu-opencl-cancellation-test`; CI also cross-builds both
fixture configurations. These checks do not qualify native in-flight cancellation,
receiver retirement, CL owner release or physical GPU behavior. That supervised
fault run remains pending with the final admitted runtime.

## Actual native teardown defect and regression (2026-10-21)

Native `build/windows11_vm/tcp_gpu_7359408c/` witnessed authenticated opcode18,
joined the actual API caller with enqueue0/read-5 and proved exact host reaping.
The prior fixture then abandoned the ICD before CL release, so the Vulkan
loader rejected `vkDestroyBuffer` with an invalid device and the native process
exited3. This is a failed qualification run, preserved with its complete raw
logs. Root separately proves the native process, its owned cmd/c batch console
and host graph retired; the existing VM is preserved.

The correction retains ICD dispatch through reverse CL release and terminal
SDK shutdown. New source-extracted tests execute the actual four helper bodies
against ownership-sensitive doubles; the old abandon-first sequence is a
negative control. Missing receipt, every release failure, Busy retry/timeout,
non-Ok/non-idempotent shutdown, valid post-shutdown platform and abandon failure
are covered. Root normal/optimized Python runs compile O0/O2, run ASan/LSan/UBSan
and gate all36 actual helper lines and38 directional branch outcomes at100%,
with all four functions executed and no exclusions. The existing call/release
regressions also pass unchanged. Evidence:
`build/qualification_feature_20261021/opencl_cancellation_order_root.log`,
`build/coverage/vgpu/opencl_cancellation_order/run-jg9plsi2/receipt.json` and
`run-_f0idmkv/receipt.json`. Independent source review finds no additional
issue; fresh native cancellation remains mandatory. Production DLL inputs
are unchanged, so the current full image remains the native candidate.

## Complete cancellation orchestration and fresh native pass (2026-10-21)

The extended source-extracted regression executes the exact `run_transport_loss`
and its four helpers, including all SDK acquisition variants, thread setup/join
errors, Busy ownership checks, real call-result mapping and every cleanup edge.
Root normal/optimized runs pass O0/O2 and ASan/LSan/UBSan with108/108 executable
lines and92/92 directional branch outcomes, all five functions executed. No
coverage exclusion or threshold change. Receipts are
`build/coverage/vgpu/opencl_cancellation_order/run-3rf7398o/receipt.json` and
`run-p00vgs7j/receipt.json`; root logs are under
`build/qualification_feature_20261021/opencl_cancellation_full_root/`.

Fresh actual native run `build/windows11_vm/tcp_gpu_d5dbcc4f/`, authenticated
session4996ced9f2449702, passes the witnessed transport-loss scope. The actual
submit gate triggers intentional cancellation of only its exact host graph;
the joined API returns enqueue0/read-5. Exact receiver reaping precedes the
trusted receipt, reverse CL release, terminal SDK shutdown and ICD abandonment.
The native fixture exits0 naturally; module absence and exact native/batch
console absence pass. The host graph is empty; reviewed supervisor finally
closes its pidfds and the supervisor exits0. Cancellation signals and nonzero
host status are expected fault actions, not normal-exit evidence. Full DLL
SHA256 is70ca4fdbc2ca54954b7879f28f11e013c63a0a660cca718815abc24e98d5887a.

Root binds the complete raw run in
`build/qualification_feature_20261021/opencl_cancellation_full_root/native_root_review.json`.
The earlier invalid-device run remains failed. The older fixture builder's
single multi-TU dependency invocation omitted one preprocessing closure; its
manual logical source/tool bindings were reviewed, but this receipt is not
complete dependency-closure evidence. Final qualification requires the fresh
per-TU-union builder and rerun after fixture integration. This result awards no
heap, full reload, overlap or final feature completion credit.

## Complete-input final orchestration rerun

Root fresh `tcp_gpu_d62ddee8`, session2112ca43451ab2a1, passes the authenticated
transport-loss scenario with candidate70ca4fdb and fixture4979abb2.
The fixture receipt ad96b21f binds300preprocessing inputs across two separately
captured translation units. It closes the old incomplete multi-TU receipt gap.
Native enqueue0/read-5 occurs after the exact validated RequestSubmit witness;
scoped host cancellation returns expected status-4/worker0. The API thread joins,
reverse owners release before shutdown/abandon, and all modules retire.
The guest returns0 and its exact process/owned command console are absent;
the complete host graph is empty and root's observer closes its pidfds.
The supervisor returns0. This intentional cancellation includes one scoped
SIGTERM and must not be mistaken for a normal zero-signal positive GPU case.

Root recomputation binds41raw files in
`build/qualification_feature_20261021/opencl_cancellation_full_root/final_complete_native_review.json`;
its repeatable script is adjacent. Native medium-integrity and failure/sanitizer
checks remain active. No heap or full reload acceptance is claimed.
The later CSR API-array addition changes only the audit snapshot/emit helper;
the complete OpenCL orchestration source is byte-identical to this run's source.
Its inverse proof and fresh300-input fixture65846c14 are recorded separately,
with fresh native TLS/FLS/malformed controls `opencl_reload_controls_native_1fbe51f7`.
