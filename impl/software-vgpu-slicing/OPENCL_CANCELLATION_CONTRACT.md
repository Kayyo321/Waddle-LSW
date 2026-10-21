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
It then calls bootstrap `abandon(session)` exactly for that session. Only after
successful abandonment does it release local OpenCL owners in reverse order:
event, kernel, buffer, program, queue, context. The selected fixture uses only
buffers and no context destructor callback or mapped memory. The server has
already reaped the actual receiver, so clvk destructors may release local C++
state while ICD destroy entrypoints see cleared device/resource tables and do
not send stale resource IDs.

If the API worker started but failed before the blocking read returned, the
same exact receipt and abandon ordering is required before any OpenCL
destructor runs: the enqueue may already have issued a submit request. The
process then requires terminal `waddle_clvk_shutdown()` success, verifies the
post-shutdown platform query returns `CL_INVALID_PLATFORM`, and returns
retained module references to the parent. The parent joins the outer worker
with a finite deadline, unloads OpenCL before loader/bootstrap, checks module
absence with a finite deadline, and restores the original environment.
Missing retirement proof never authorizes abandonment or graceful module
unload; the process terminates fail-closed after its bounded wait, without
claiming success.

## Source basis and limits

The result mapping follows the pinned clvk `src/queue.cpp` blocking wait,
submit-and-wait and command execution paths. The private shutdown ledger counts
all `refcounted` allocations and permits terminal teardown only when only root
devices remain and the executor pool is idle. The ICD's post-reap `abandon`
clears guest object tables; it does not release clvk C++ owners, which is why the
OpenCL release calls must precede shutdown.

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
