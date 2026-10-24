# Isolated Windows native acceptance runner: frozen harness boundary

The harness is test infrastructure, not production session acceptance. It may
create only a fresh0700 run directory, a QCOW2 overlay backed by the preserved
Windows image, private copies of matching UEFI vars and TPM state, read-only
payload ISO and private logs/QMP/TPM/result sockets. Original disk/vars/TPM,
credentials.json and registered devices must not be changed. Credential
contents must never be read or printed. No kernel module/device ACL mutation.

Before launch verify no process holds the base writable; QEMU opens backing
read-only and its default file locks must remain enabled. Assert overlay backing
path and original file stat fingerprints; hash original vars/TPM before/after.
Never invoke provision.py. Use pinned source-built QEMU9.2.4 under TCG; keep
swtpm and QEMU as supervised exact child processes, no daemonize/PID discovery
for cleanup. Match OVMF4M CODE with private vars and private TPM state. No host
USB/audio or vhost-vsock attachment is permitted while required nodes are absent.

The initial bounded run may prove Windows boots and report QMP status/topology
and a screenshot. No interop/native driver/session credit follows from that.
For executable tests, stage only built, hash-recorded test payloads in read-only
ISO; collect nonce-correlated bounded results through a private guestfwd Unix
endpoint or another reviewed owned channel. A guest TCP/serial test route is
labelled test-only and never claims production VSOCK admission. Do not type or
use credentials to bypass an unavailable desktop. If login blocks, retain the
screenshot/evidence and continue portable source work.

Use bounded startup/run/shutdown deadlines. Request QMP system_powerdown first;
if the disposable guest won't exit, QMP quit or termination of the exact owned
child is permitted for this private overlay only. Reap QEMU before stopping/
reaping the exact swtpm child. Preserve overlay/logs on every failure, and never
clean or remove assets while a child may still own them. No source VM PID may
be signalled. Report a forced disposable shutdown honestly, never as guest
producer quiescence or release proof.

Automated harness tests must cover base-path/refusal guards, missing capability,
partial child startup, timeout/cancel, result bounds/correlation and exact-child
cleanup. They may use fakes for orchestration; real guest output remains a
separate labelled evidence artifact. Root reviews harness before its first boot.

## Kernel-backed base writer exclusion amendment

The host hides descriptors even for some current-UID processes, so `/proc`
scanning cannot establish absence of writable holders here. Acquire an owned
O_RDONLY|O_CLOEXEC fd for the canonical regular base disk and a Linux
F_SETLEASE/F_RDLCK read lease before snapshot/overlay work. Failure (including
EAGAIN, unsupported filesystem or permission failure) refuses launch. Retain
that lease through all qemu-img operations and until the exact QEMU child is
reaped. This uses kernel conflict detection; `/proc` scans become explicitly
incomplete diagnostic observations and cannot bypass a failed lease.

Run on the main thread. Refuse an existing non-default SIGIO handler; install a
temporary handler that only records a break flag, set F_SETOWN to the runner PID,
and acquire/query the lease. Check the flag and F_GETLEASE==F_RDLCK immediately
before launch and every≤100ms during observation/startup. Read the kernel lease
break-time and require enough margin for the bounded preparation commands and
forced child cleanup; never change that system setting. On a break, skip ACPI
waiting, TERM/KILL/reap only exact owned QEMU, then swtpm, before releasing the
lease. Interrupted/failed startup must follow the same ownership order. Record
all acquired/broken/released lease facts and the diagnostic proc visibility
limit in the manifest. Never report an unreaped child or lost lease as success.
Restore signal handling only after local lease ownership is relinquished.

Test acquisition/query/release and rejection with an existing writer on private
temporary regular files; test break notification and run() cleanup with bounded
fault fixtures. The preserved base is only opened read-only; no real VM launch
until root reviews the amended source. Reference: Linux man-pages
[F_SETLEASE](https://www.man7.org/linux/man-pages/man2/F_SETLEASE.2const.html).
