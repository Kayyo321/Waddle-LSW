# Fresh native guest install supervisor contract

## Scope and launch boundary

`tests/interop/fresh_native_install.py` is a test-only observer for a newly
prepared disposable Windows Server guest. Importing the module and running its
tests starts no process, opens no device and reads no media. Its only launch
entry point is `run_fresh_install(run_root, manifest, source_vhost_fd, ...)`.
The actual preparation receipt and launch remain under root review; this
contract does not authorize a production launch, grant native acceptance, or
alter the preserved Windows 11 VM. The module does not discover, signal or
wait for processes by PID. It owns only exact process objects returned by its
`Popen` calls.

The scope ends when the installer QEMU process and its swtpm child are reaped
or a bounded cleanup failure is recorded. It does not itself verify Windows
login, driver load, guest-exec startup, VSOCK port 5242, or a real guest
command. Root may run that independent probe and request shutdown through the
optional caller-owned `stop_fd`; the receipt always sets
`native_acceptance_credit` and `guest_booted` false.

## Inputs and preflight invariants

1. The caller must be an ordinary non-root process with `geteuid() == getuid()`.
   A privileged helper may separately transfer a previously opened descriptor
   using the repository's scoped handoff. This function never invokes `sudo`,
   changes ACLs, changes device nodes, or opens `/dev/vhost-vsock` itself.
2. `/dev/kvm` must be accessible read/write to that same caller. The prepared
   QEMU command must explicitly select `accel=kvm`; there is no TCG fallback.
3. `run_root` must resolve to an existing, non-symlink directory with exact
   mode 0700. The supervisor creates no run directory and never reuses a
   previous run automatically.
4. The supplied manifest must contain string argument lists named
   `qemu_argv_template` and `swtpm_argv`. The QEMU list must contain exactly
   one `CALLER_VALIDATED_VHOST_VSOCK_FD` marker and an exact private QMP
   endpoint `unix:<run_root>/qmp.sock,server=on,wait=off`. NUL bytes are refused.
5. Optional `stop_fd` must be a valid nonblocking pollable descriptor, normally
   a pipe read end or eventfd. The supervisor registers it with `poll`, never
   reads or closes it, and checks readiness at least once per one-second
   observation interval. Read, hangup, or error readiness sets `stop_requested`
   and requests graceful ACPI shutdown. It is never passed to a child.
6. The source descriptor is borrowed: the caller retains it unchanged. The
   supervisor duplicates it, marks the duplicate close-on-exec, verifies by
   `fstat` that it is character device 10:241 and verifies read/write access.
   Only the duplicate is placed in QEMU's `pass_fds`. The duplicate is closed
   on every path after process cleanup. A failed duplicate or invalid device
   fails before any child is created.

## Components and sequence

```text
root-reviewed launch caller
  ├─ lends descriptor ──> validated CLOEXEC duplicate ──pass_fds──> QEMU
  ├─ prepared argv ────────────────────────────────────────────────> QEMU
  ├─ private TPM argv ────────────────────────────────────────────> swtpm
  └─ private QMP socket <────────────── bounded QMP commands ───── QEMU
```

1. Preflight validates caller UID, KVM access, finite screenshot interval,
   optional nonblocking stop descriptor, run-root privacy, manifest argument
   shapes, QMP endpoint and descriptor identity. Establish one absolute
   monotonic 1,800-second install deadline before starting either child.
2. Start swtpm first with stdin at `/dev/null`; stdout and stderr share an
   exclusively created mode-0600 `install-swtpm.log`.
3. Before QEMU launch, require that the exact swtpm child remains alive and has
   created a nonsymlink Unix control socket at `<run_root>/tpm.sock` within ten
   seconds and the same absolute deadline. On early exit or readiness timeout,
   do not start QEMU; proceed to bounded cleanup. Then start QEMU with stdin at
   `/dev/null`, output to exclusive mode-0600 `install-qemu.log`,
   `close_fds=True`, and only the validated VSOCK descriptor in `pass_fds`.
   The stop pipe is polled during readiness; a stop request prevents QEMU
   launch. Persist a non-secret `install-manifest.json` atomically as mode 0600.
4. Wait up to 30 seconds, bounded by that deadline, for the private QMP
   socket. Each
   QMP connection attempt is bounded to at most one second; the greeting and
   capabilities handshake must finish within that attempt. The greeting must
   contain a `QMP` object. A caller stop notification is polled during this
   retry loop too. Commands carry monotonically increasing IDs and
   responses must match the outstanding ID. QMP lines are capped at 65,536
   bytes, with at most 32 asynchronous events before a command response. Every
   connect, send, and receive uses an absolute monotonic deadline; each
   subsequent receive cannot reset or extend it. Ordinary QMP commands have a
   five-second cap; screenshots have a one-second cap.
5. Until QEMU exits, a caller stop notification arrives, or the absolute
   1,800-second deadline elapses, request a QMP `screendump` at the configured
   interval (default 60 seconds; maximum 300 seconds). Each successful
   screenshot is recorded only
   by its basename; its final mode is 0600. Screenshot failures increment a
   count and do not extend the deadline. Stop readiness is checked at least
   every second between bounded QMP calls (and at least every 100 ms while
   waiting for the swtpm socket). Any screenshot command timeout or
   protocol failure poisons that QMP stream; before ACPI cleanup, close it and
   establish a fresh bounded QMP connection so a late screenshot response
   cannot be mistaken for the powerdown response. Unexpected QEMU or swtpm
   exit fails the run; a return code is recorded when available.
6. If the deadline or caller stop notification arrives, issue QMP
   `system_powerdown` with a five-second maximum and record its outcome. On an
   exceptional exit, make the same best-effort ACPI request before closing QMP.
   The QMP cleanup deadline includes the bounded shutdown allowance. Wait up
   to 90 seconds for the exact QEMU object to exit.
7. If QEMU remains alive, call `terminate()` on that object only, then wait up
   to five seconds. If still alive, call `kill()` on that same object and wait
   up to five more seconds. Never signal a process group, process name, PID
   obtained through discovery, or any child not created by this invocation.
8. Guard each poll, wait, terminate, and kill separately. One operation's
   failure is recorded by exception class and never skips remaining cleanup:
   continue with QEMU escalation, exact swtpm cleanup, descriptor close, output
   permission repair, and the final manifest write attempt. Only after the
   QEMU cleanup attempt, terminate the exact swtpm object and
   wait up to five seconds; escalate to `kill()` and a final five-second wait
   if required. Reaping states are recorded independently. Close QMP and the
   owned descriptor. Apply mode 0600 to logs and screenshots, then persist the
   final receipt. Any failure, forced termination, or unreaped child retains
   the complete mode-0700 run directory for review and recovery.

## Output schema and privacy

The private JSON receipt records only: canonical run-root path, fixed deadline,
whether child launch began, whether the deadline elapsed, whether ACPI was
requested, whether QEMU termination was forced, each child reaping result,
QMP status, screenshot basenames/counts, exception class on failure, and
explicit false acceptance/secret-content/hash flags. It never copies the input
preparation manifest, command output, QMP response contents, command stderr,
XML, answer-ISO contents/hash, passwords, or arbitrary exception text. Process
argv exists only in memory and is not included in logs or receipt. Child output
is confined to private log files. Files remain under the mode-0700 run root;
all supervisor-created files are mode 0600. A failure to repair an output mode
is recorded and causes manual review.

## Ownership, concurrency, and errors

One synchronous caller thread owns both `Popen` objects, the QMP connection,
the duplicated descriptor and receipt writes. There are no worker threads,
locks, shared queues or background cleanup handlers. The caller-provided
descriptor remains borrowed; child handles, QMP socket and duplicated fd are
owned until explicit close/reap. Child creation order is swtpm then QEMU;
cleanup order is QEMU then swtpm. Reaping success is determined by each exact
handle's `poll()` result after bounded `wait()` calls.

Preflight errors occur before any child launch. A launch, QMP, deadline,
screenshot, signal or manifest error is represented by a generic exception
class and safe booleans/counts in the receipt; diagnostic details are
suppressed. Cleanup continues for every child even when an earlier cleanup
step fails. If either process cannot be reaped, the API raises a generic
supervisor failure and retains evidence; it never deletes or retries the run.

## Verification criteria

The isolated unit tests must establish that no test invokes a real process;
only fake child objects, fake descriptors and a fake QMP socket are used. They
check exact-child ownership, swtpm-before-QEMU readiness, descriptor isolation
through `pass_fds`, borrowed stop-pipe ownership, KVM requirement, invalid
manifest refusal, QMP greeting/IDs/deadlines/event bounds, forced termination,
failure-continuing reaping, private log/receipt modes, secret-free metadata,
and run-root mode enforcement. Preparation regression
tests separately verify that QEMU provenance returned by the real validator
shape has no `path` member and that `_validate_config` retains its canonical
binary path separately. These tests establish software control flow only;
they do not establish that KVM, VSOCK, QMP, Windows Setup, VirtIO, or guest
acceptance works on a host.

## Explicit installer firmware input (2026-10-25)

Root's first live run reached the CD/DVD boot-key prompt and timed out before
installing; the existing stop pipe requested cleanup, with forced QEMU stop
recorded and both children reaped. Its secret-free receipts and screenshot are
retained in native_install_evidence_2026_10_25/boot_prompt_attempt.
`install_boot_key` is an opt-in boolean test mode for a newly prepared blank
disk. After QMP connects, it sends Space at most24 times over12 seconds, each
hold100ms and command deadline1s, checking stop/child exit between attempts.
This enters Windows Setup on the new disk; it is disabled by default and grants
no acceptance. The exact finite input sequence has an independent fake-clock
unit. Normal installed-system boots do not use this mode.
