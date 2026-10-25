# Connected WinSock owner local qualification

Exact production source and all qualification inputs are hashed in inputs.json.
The retained root attempt is build/coverage/interop/windows_socket_owner/
run-20261025T160024-82ed9836. Production scope:842/873 lines (96.45%),
627/695 taken branch arcs (90.22%); no source/branch exclusions. The fixture
passes ASan+LSan+UBSan with leak detection enabled and abort/halt on errors.

Root normal make also passes the actual production source through the private
API double, and Zig Windows SDK compilation and native AF_INET loopback fixture
link pass. Windows runtime execution remains a separate CI gate.

Review preserves exact SOCKET ownership after native close errors: only the
documented WSAEWOULDBLOCK case permits explicit retry, all other errors retain
an indeterminate owner until caller-owned quarantine handoff. A repeat abort
now preserves the pending/indeterminate work status without another native close,
while successful-close snapshots remain stable. Regressions cover both states.

This is connected byte ownership only: no VSOCK listener, map initialization,
signing/deployment/autostart, host-detach or native driver acceptance claimed.
