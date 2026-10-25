# Installed-session observer qualification

On 2026-10-25 a newly provisioned isolated Windows Server 2025 evaluation guest
(10.0.26100.32230, standard account `waddle_native`, CID52945) executed four
hash-bound native binaries through the actual Waddle guest-exec VSOCK channel
on port5242. All exited0: 28 graph C ABI cases; real observer facts and correlated
hints/protected Destroy/teardown; foreign-process graph admission, parent ordering,
title updates, quarantine and generation readmission; and the connected Windows
socket owner using real WinSock AF_INET. The socket binary includes055e8c7.
Exact outputs, binary hashes and cleanup results are in guest-exec-probe.json.
Root independently rechecked those hashes against the retained binaries.

Accept TODO #10 installed-session observer qualification15%, raising #10 from65%
to80% and overall completion by0.60%. This closes the bounded observer native
milestone together with the reviewed portable/native unit and CI evidence.
Coordinator/capture/slot/wire integration20% remains open. This run does not
qualify native interop port5002, IVSHMEM drivers, UIAccess/signing, elevated
windows or desktop/audio acceptance. The receipt therefore correctly retains
native_interop_acceptance=false; no #6/#34 completion is assigned.

The supervisor's guest_booted=false is its conservative detector result, not an
execution verdict: the independent guest command output proves the installed
session ran. QMP reconnected for cleanup; ACPI stopped QEMU cleanly without
forced termination. QEMU, swtpm and the probe were joined/reaped. Private disk,
TPM, firmware, answer media and credentials remain only in the ignored private
run root; no secret content or answer-file hashes are included here. The original
Windows11 disk was untouched. This is a retained test device, not a deployment.
