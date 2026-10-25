# Deterministic GuestReady runtime fixture

Exact7007856 workflow40213910342 Linux job121230810012 failed the500ms observer
reply assertion before new native composition ran. The fixture used real
AF_VSOCK socket creation despite its hardware-free contract; that path can
invoke host kernel/device behavior and potentially contact an unrelated guest.
The log does not by itself prove which kernel/scheduling delay caused this
particular failure. The test now deterministically refuses only AF_VSOCK at the
socket boundary; real AF_UNIX server, framing, concurrency, timeout, status and
cleanup remain exercised. It requires actual attempted probes, exact timeout
status and the original500ms responsiveness budget. Native VSOCK tests remain
separate and receive no credit from this seam.

The response helper now uses nonblocking reads with one absolute deadline
covering partial headers and payloads. A real socketpair leaves an incomplete
response and verifies bounded timeout/output-length preservation. This closes
a potential unbounded wait in the test helper without relaxing any product gate.
Normal and leak-enabled ASan/LSan/UBSan runtime fixtures pass locally. CI rerun
is required; no feature progress change is assigned.
