# Host control runtime qualification

Root independently built the actual production waddle-session executable and
ran --help, then exact full-source CLI/runtime integration and fault fixtures.
Runtime317/328lines96.65%,328/362taken arcs90.61%; CLI230/238lines96.64%,
213/230taken arcs92.61%. No production source exclusions. The same compositions
pass ASan/LSan/UBSan with leak detection enabled; commands, hashes, raw coverage
and sanitizer output are retained. Bounds-checked Zig ABI objects remain linked.

Positive tests call the actual main entry through its renamed fixture symbol,
with isolated real XDG profile/registry, authenticated real AF_UNIX daemon
listener, real admission/locks, one VSOCK-to-socketpair seam, real peer ABI and
2GiB sparse backing through the authentic Linux mapper. Actual Hello mapping
validation gates Ack and cap-zero Ready; own host heartbeat and bounded Goodbye
run. Negative paths cover wrong epochs, daemon loss/data, signal/cancel/timeouts,
clock/random/poll faults, partial I/O and retryable retained cleanup owners.
The private fatal process-exit fixture is a separate _exit boundary; sanitizer
success does not claim successful normal cleanup of that deliberate child.

Accept TODO #7 VSOCK/singleton admission10% and daemon Status/GuestReady5%,
raising #7 from30% to45%. Local session command socket/control5% remains open.
Accept TODO #5 live header mapping5% of its15% runtime allocation; live slot
consumer5% and heap/audio integration5% remain open. Combined overall gain0.75%,
raising total20.85% to21.60%. This diagnostic advertises zero application
capabilities. It does not qualify real VSOCK, KVMFR/IVSHMEM drivers, Windows peer,
Wayland/capture/audio/input, restart/autostart, remote detach or backing reuse.
