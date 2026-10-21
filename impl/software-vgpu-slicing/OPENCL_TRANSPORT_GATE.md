# Qualification-only OpenCL submit cancellation witness

## Scope and boundary

`WaddleOpenclTransportGate` is a test-only POSIX controller compile definition.
It is absent from every ordinary bridge target. Default preprocessing is
byte-identical to the prior server and its object has no gate reference.
The qualified controller additionally links `tests/vgpu/opencl_transport_gate.c`.
This is not a production environment toggle, retry, timeout extension or
synthetic OpenCL request. It cannot itself establish native acceptance.

## Actual request boundary

The existing server authenticates the session, decodes and bounds the full
request, checks sequence and receives the complete payload. Only the current
RequestSubmit with payload at least44bytes, prefix178 and opcode18 enters the
hook, immediately before the actual guest exchange. The hook cannot forward
that request. The trusted process-global WADDLE_OPENCL_CANCEL_GATE path names
one fresh absolute marker in a supervisor-owned private directory. It is copied
into fixed4096byte stack storage; missing, relative, overlong and control-byte
paths fail. Calls sharing the configured path are serialized; a second caller
collides and fails closed.

The exclusive, non-following, close-on-exec regular file has exact mode0600.
Its record is WADDLE_OPENCL_CANCEL_GATE_V1, followed by session=16lowercasehex,
sequence=decimal, opcode=18 and pid=decimal, each terminated by newline. Partial
writes and EINTR are handled; metadata/write/close failures reject and remove
only the newly created partial file. Existing files/symlinks are preserved.
The descriptor closes before waiting. The observer reads a complete record,
verifies exact owned PID/session/sequence, then cancels that actual controller.

The hook acquire-loads the existing transport cancel atomic and returns
RingCancelled only after observing it. It polls at1ms only until the existing
absolute request deadline; clock, polling or deadline failures reject. The
server poisons and returns without calling the guest exchange. The caller owns
all session/request/cancel storage through this synchronous call. No heap
allocation, descriptor or borrowed reference escapes.

## Verification and limits

Real helper tests cover the complete witness and closed descriptor before a
threaded cancel, malformed arguments/paths, collision and symlink preservation,
expired/maximum deadlines, zero clock, partial/EINTR writes and poll, and all
metadata/write/close error controls. They retain exact fd count. Root runs full
ASan/LSan/UBSan with leak detection enabled outside ptrace; all pass, zero
reported owned leaks. Worker LSan-under-ptrace failures remain failed logs;
its leak-disabled ASan run alone is not leak evidence.

The source-bound gcov gate reuses the unchanged90% line/branch floor and requires
all three actual helper functions. Root measured47/47lines and63/66branch
outcomes95.45%, normally and optimized, with no exclusions. Root also compiled
the qualified server and independently compared default preprocessing and
undefined symbols. Raw receipts/logs:
`build/qualification_feature_20261021/opencl_gate_root/`, especially
`make/default_equivalence.json`; coverage under
`build/coverage/vgpu/opencl_transport_gate/20261021T184237_1uvl41g6/` and
`20261021T184238_ox5nht7c/` (later Make invocations retain their own directories).

CI and Make run `vgpu-opencl-transport-gate-test`,
`vgpu-opencl-transport-gate-sanitizers` and
`vgpu-opencl-transport-gate-coverage`. These Linux controls do not prove a real
native OpenCL caller was blocked, the exact receiver was reaped, CL owners
released after abandonment, or terminal shutdown/module retirement. Those
require the separately reviewed native fixture and supervisor evidence.
