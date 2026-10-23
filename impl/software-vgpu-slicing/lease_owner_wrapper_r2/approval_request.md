# Codex lease-owner wrapper revision 2

The 2026-10-22 handoff requires explicit approval before this wrapper is used.
The reviewed source is [lease_owner_wrapper.py](lease_owner_wrapper.py); the
[inverse patch](lease_owner_wrapper_r2.patch) exactly recovers revision 1.

- Required owner: `Codex software-vgpu-slicing continuation (sole VM/GPU runner)`.
- Required previous owner: `Claude Opus 5.5 Fable-handoff continuation (sole VM/GPU runner)`.
- Wrapper SHA-256: `a08bf833b11cbfe1ed998202bc33d2350782c763cd189f0cc671673a8827914e`.
- All original idle-state, identity-set, PID and start-tick checks still execute.
- The frozen executor/admission and their TargetOwner constants are unchanged.
- Only a copied lease's owner field is mapped for the frozen validator; the real
  lease is never written by this wrapper.

Eleven offline tests pass in normal and optimized Python, including rejection of
wrong owners, wrong/missing predecessors, active/uncertain states, reused/dead/missing
PIDs, incorrect identities, non-dictionary leases and validator failures. There is
no VM/GPU execution in these tests. The r5 heap launcher pins the new wrapper and
keeps r4's runner bytes unchanged. A separate runner revision must bind the newly
launched VM identities; the old r4/r5 identity is never edited in place.

Approval status: explicitly approved in this chat on 2026-10-23: "Approve wrapper r2 for native tests".
Approval authorizes this exact wrapper hash. Preparing, importing and testing
these files does not constitute native qualification.
