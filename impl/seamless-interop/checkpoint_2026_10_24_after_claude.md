# Verified checkpoint after Claude return and connected socket owner

Implementation source commit: `3b50946a7b9ac702f28ca16c72711f99efd80545`.
Branch: `feat/seamless-interop`; merge target: `origin`. Runtime implementation
is pushed. The later advisory and evidence commits are local, ready to accompany
the next source push. This checkpoint/CI receipt is in the final tracker entry.
No PR was opened or merged. Feature remains In Progress:18.00%, TODO#7=30%,
TODO#10=35%, TODO#34=0%; Time Ended stays TBD. Do not mark the feature complete.

## Accepted and verified

Claude returned `3714cf2` with ownership released. Root reviewed through
`73bc8330af5423989d62aad3824a771e169e356f` in the isolated worktree and imported
the qualified graph/observer snapshot in57847b1. The source/provenance and return
remain under `claude_window_graph/`; its isolated checkout is retained. Pure
graph25% and C ABI10% of#10 are accepted. Tracked-window snapshot, deferred
pruning, native PID/TID consistency and indeterminate fullscreen facts were
corrected and qualified; O5 still prevents observer acceptance.

The Linux byte owner takes one already-connected nonblocking CLOEXEC stream fd,
serializes peer operations on its creating pthread, copies bounded RX/TX,
ACKs positive sends exactly once, issues single-use poll cookies and closes once.
It does not connect AF_VSOCK, map memory, prove release or reconnect safely.
`linux_socket_owner_qualification_2026_10_24.json` binds the exact files/gates.
Final source SHA256 is74907d6f6fa2c5d95d210ad4552a28abef207b3dcbc6fc796b8f55c11dd2c581.

Local complete interop tests, leak-enabled ASan/LSan/UBSan and all production
coverage gates pass on committed source. Owner coverage is731/752 lines97.21%
and547/607 branches90.12%, with no owner exclusions; ordinary source object has
exactly8 frozen exports. Tests include128 fresh real fragmented socketpair
lifecycles; scalar/syscall faults are labelled containment tests.65 attempt
records and independent reviews remain in the evidence directory.

All three workflows at exact3b50946 are green:

- Interop40133523501: Linux full tests/sanitizers/coverage and actual Windows
  foundation/peer47/ABI84/C fixture. CI independently reproduces owner coverage.
- Graph40133523475: Linux and actual Windows graph148/ABI164/C28/observer65,
  real native smoke and separate synthetic native-fact failures.
- Native AV40133523473: both Windows and transport jobs succeed.

Exact metadata, Linux/Windows logs and hashes are in
`ci_qualification_3b50946.json` and `evidence/ci_3b50946/`.

## Ownership and next work

All Luna implementation/review lanes released their files. No worker still
owns source. Root owns shared headers/build/docs/tracker/Git and review. No
Claude CLI or subagent skill was used. No local test/watch process remains
from this phase after the final CI watch completed.

1. Consolidate the O5 protected-Destroy normative contract before editing the
   observer. `resume_proposals/observer_destroy_preservation/lead_blockers_review.md`
   identifies callback reserve safe points, promotion for candidates/Deferred
   children, bounded publication staging, hook lifecycle and quiesced evidence
   cleanup requirements. Older illustrative snapshot signatures are superseded
   by the accepted const/u32 ABI. Proposals are advisory, not accepted code.
2. Freeze the host admission/singleton slice using
   `resume_proposals/linux_admission_next_slice.md`. Preserve session/AV lease
   order, pre-start guest-exec-port5002 rejection and selected-device CID.
   GuestReady0x10/0x11 is not implemented; it means legacy guest-exec reachability,
   never interop Ready/mapping authority. Connection/singleton10% and daemon/
   local-control10% of#7 remain open; byte-owner5% alone is accepted.
3. Guest listener/mapping, signing/install and coordinator/capture/slot/wire
   runtime remain unfinished. No socket EOF, timeout, Goodbye, shared Free slot
   or proxy destruction proves external release or permits in-place epoch reset.

## Runner and recovery constraints

No VM/device/ACL action was taken in this phase. The execution surface currently
exposes no `/dev/kvm`, `/dev/vsock`, `/dev/dri` or renderD128. This observation
is not native acceptance and does not prove every software VM route impossible.
Keep existing VM assets and credentials intact; never print credentials.json.
Read `resume_proposals/native_guest_runner_plan.md` before native work.

Never run main `make clean` or `make test-sanitizers`: build contains the
preserved37GB Windows disk and vars. Use bounded non-clean interop targets and
isolated temporary outputs. Keep README unchanged. Continue atomic commits,
exact progress attribution and real >=90% production line/branch gates. A PR
merge waits for100% across every tracker task and actual native acceptance.
