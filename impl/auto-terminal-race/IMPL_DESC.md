# CLI terminal-delivery race

## Scope and observed failure

Fix the CLI's receive/exit ordering when a peer stops accepting writes after
sending a completed guest result. This is a CLI session error-handling fix, not a
wire-format change. Mock/Windows guest production code, daemon lifecycle code,
AV protocol/input leases, dependencies, security settings, and README are outside
scope.

Linux CLI CI run `38009725287`, job `114086676919`, failed during
`make test-sanitizers` on documentation-only commit
`414d7e47a02e42ea8d632b62eb8c51ec75f1dd81`. The fixture printed
`AUTO_SESSION_SUCCESS`, then `waddle: session failed: Broken pipe`, then failed
its exact exit-zero assertion at `tests/daemon/test_auto_terminal.c:100`.
This exposed an existing nondeterministic race; it was not a documentation-caused
code regression. Evidence:
<https://github.com/Kayyo321/Waddle-LSW/actions/runs/38009725287/job/114086676919>.

Unmodified `src/cli/session.c` blob
`a52787c37600b23129aab22a5338c5bc9ff04303` reproduces deterministically with the
actual session, terminal, and protocol implementation: queue `SpawnResp`, stdout,
both output EOFs, and `ProcessExit(0)` on an anonymous socketpair, then close the
peer before starting the session with stdin at EOF. The session emits the stdout
marker but returns 125 with EPIPE instead of returning zero. The fix returns zero
for the same ordering. No retry, sleep, relaxed assertion, or fake success is
involved.

## Architecture and interactions

The host session owns its event loop. The guest independently owns process exit,
output EOF, final protocol delivery, and its socket close. The mock peer exits
once its terminal frame queue has been flushed. It need not wait for a racing
host stdin EOF that its completed process no longer needs.

Each host poll cycle can flush stdout/stderr, flush transport TX, handle stdin,
and decode one inbound frame. A peer close may therefore occur while complete
terminal frames are already in the receive buffer but the host still has stdin,
stdin EOF, or a signal queued for transmission. EPIPE on that write says nothing
about whether a complete guest result remains unread in the opposite direction.
Changing read/write priority alone would not establish correctness: more than one
terminal frame may remain, and the close can occur between any two syscalls.

The fixed state sequence is:

1. Normal duplex session: existing queue limits, protocol checks, signals,
   deadline, and backpressure behavior apply.
2. Terminal TX failure: only EPIPE or ECONNRESET from the transport queue flush
   starts receive draining. Other transport failures and all local stdout/stderr
   failures remain immediate errors.
3. Draining: discard unsendable TX, stop reading stdin, and drain signal
   notifications without queuing them. Keep reading and validating guest frames,
   while continuing normal stdout/stderr delivery.
4. Valid completion: both output stream EOFs followed by a complete, valid
   `ProcessExit` authorize the exact guest status (low eight bits), including
   nonzero status. Drain already-accepted local output before returning.
5. Failed completion: transport EOF without a valid exit, malformed/truncated
   frames, or expiration of the fixed receive-drain cap returns local error 125.
   A prior explicit session deadline still returns 124.

## State, wire format, and ownership

No public struct, ABI, message type, frame layout, checksum, sequence number,
stream ordering rule, or protocol version changes. Existing wire validation
remains authoritative. `ProcessExit` still requires its 16-byte body, both output
EOFs, and an accepted reason value. Duplicate EOFs, data after an output EOF,
invalid sequence/checksum, and early exits remain errors.

The session adds only local scalars: `tx_error` stores the first terminal write
errno and `drain_deadline` stores an absolute monotonic millisecond deadline.
`TransportDrainMs` is fixed at 2000 milliseconds. There are no new allocations or
threads. The existing two output queues and decoder are released on every return.
The caller retains ownership of the transport descriptor and input TX allocation;
its unsendable `off` and `len` are reset when drain mode starts.

The session is the sole I/O owner of its borrowed descriptor and TX queue during
the call. It is not safe to operate either concurrently or to invoke the
process-global terminal/signal APIs concurrently. Test socketpairs and pipe ends
are explicitly closed by their owning parent/child; the parent always checks the
child's real wait status.

## Execution and timing

The receive-drain deadline bounds only the wait for a validated remote result;
it does not impose a total operation bound on delivery to a blocked local output
consumer after that result is validated. The receive-drain deadline is set once,
on the first terminal write failure.
Subsequent frames, partial reads, EAGAIN, poll interruption, and signals never
extend it. Poll uses the earlier of the explicit session deadline and the drain
cap. When the scheduler resumes after both deadlines, the earlier deadline decides
the error class. A valid exit disables the transport drain cap while buffered
local output is delivered; an explicit overall session deadline continues to
apply. This preserves ordinary output backpressure semantics.

The two-second cap bounds a peer that shuts down only its read half and leaves its
write half open indefinitely. It is error recovery after an observed failure,
not a retry of the command, socket connection, write, or test. No timeout is added
to ordinary healthy sessions.

## Error handling

- Valid zero/nonzero guest result after EPIPE: return that result only after all
  accepted output is flushed.
- EOF before valid result: return 125, retaining the terminal write errno when
  clean transport EOF provides no more specific parse error.
- Truncated/corrupt/early terminal frame: return 125 with the existing protocol
  error; do not reinterpret an error as success.
- Two-second cap: return 125 with the saved transport error.
- Earlier user deadline: return 124 and the existing timeout diagnostic.
- Local output failure: return 125 even if valid terminal frames are waiting.
- Additional signals (tested with SIGTERM) after terminal TX failure: drain wake
  notifications; do not restart the timer or add unsendable protocol frames.

## Verification and acceptance

`tests/cli/session_shutdown.c` links the real production session, terminal, and
protocol files. Anonymous socketpairs establish ordering before the session runs.
Linker read/write/poll wrappers observe unchanged real syscall results: the read
wrapper acknowledges an actual EAGAIN so the parent sends a fragmented exit tail
only after the decoder has encountered it; the write wrapper asserts exactly one
terminal write failure and no subsequent transport write attempts. The poll
wrapper acknowledges that transport polling has stopped after validated exit while
stdout remains queued, allowing a parent-controlled backpressure release. No wrapper
substitutes success, payload bytes, errno, or return values.

The 15 focused cases cover zero and nonzero results; no exit; truncated exit body
and header; exit before stream EOFs; corrupt exit CRC; synchronized fragmented
exit; read-half shutdown with a peer held open; signals during that fixed cap; an
earlier explicit deadline; stdin EOF delivered before terminal close; and local
stdout failure. Two prefilled nonblocking stdout-pipe cases also verify that a
validated terminal result cannot bypass queued output delivery, and that an explicit
session deadline still wins when that local consumer stays blocked. Each case has
an independent six-second outer failure bound.
`--stress` runs 512 exact-status sessions (128 each of close-before-flush zero,
nonzero, fragmented exit, and stdin-EOF-first), without retries. Both invocations
are part of `make test`, therefore also `make test-sanitizers`.

The real CLI integration fixture adds eight named-socket close/EOF/status cases,
including a peer held open until the CLI actually exits. The auto-terminal fixture
explicitly presents `/dev/null` stdin, requires the original CLI zero exit status,
and now also waits for a natural zero peer exit and verifies socket cleanup. It
runs 32 independent real CLI/mock sessions under a single 20-second outer alarm;
every session has a distinct parent-PID/iteration socket path, must pass, and any
assertion failure stops the run immediately. It no longer kills the peer after CLI
success.

Local plain and UBSan-only checks are separate evidence. Full ASan/LSan remains a
required native CI gate with leak detection enabled. This cloud executor rejects
named AF_UNIX socket creation with EPERM and LeakSanitizer reports its ptrace
runtime limitation, including on the actual new regression binary. Those are
blocked checks, never passes or race reproductions. No settings or sanitizer
options are weakened. Full CLI/daemon integration and native sanitizer validation
must be reported against the exact published revision by the parent workflow.
