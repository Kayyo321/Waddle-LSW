# Independent review: Claude window graph return

Scope reviewed read-only: `HANDOFF_RETURN.md`, `CLAUDE_HANDOFF.md`, graph implementation `src/interop/window_graph.zig`, ABI wrapper `src/interop/window_graph_abi.zig`, public contract `include/waddle/interop_window_graph.h`, and the returned `IMPL_DESC.md` decisions. No builds or runtime/native tests were run.

## Actionable contract concern

- **P2 — create writes through an aliased output before rejecting the overlap.** In `src/interop/window_graph_abi.zig:208-212`, `interop_window_graph_create` validates the output slot's own alignment/range, forms a typed pointer at line 209, and writes NULL at line 210 before `spans_valid` checks whether `out_graph` overlaps `config` at line 211. For a valid `interop_window_graph_config_t` object passed as both the config storage and an overlapping output slot (e.g. `out_graph = (interop_window_graph_t **)((char *)&config + 16)`), the call returns `InvalidArgument` but first overwrites the callback pointer and adjacent config bytes. This conflicts with the wrapper module's opening claim that all pointers and overlap are checked before a typed pointer is formed/used, and makes the rejected input config observably modified. The header does explicitly promise to NULL a valid output slot on *every* failure, so the desired precedence is ambiguous rather than an unambiguous violation of the public header. Recommended decision: either explicitly define output-slot clearing as taking precedence even for aliased config (and test/document that input mutation), or reject overlap before writing and narrow the header's “every failure” promise to failures after a valid, disjoint slot is established. Given the existing wrapper-wide overlap policy and non-mutating rejected inputs, the latter is the safer consistent choice.

I found no other concrete graph/ABI defect in the inspected paths. The fixed bounds, exact-generation retirement proof gate, event coalescing barrier at RetireIntent, refresh queue-room preflight, output staging on normal ABI calls, and epoch/thread/reentrancy checks appear consistent with their documented contracts.

## Accepted risks / acceptance limits (not implementation defects in this review)

- The returned handoff reports Windows cross-compilation only; no Windows execution, installed guest, UIAccess/secure-desktop, IVSHMEM, compositor, or native QEMU proof. This is a blocking verification gap for native acceptance, not evidence of a source defect.
- The spec itself documents the delayed/equal-tick destroy-hint case that may retire and readmit a newer live incarnation (`IMPL_DESC.md` decision 24 / O25). The return does not establish the required native frequency measurement. Treat this as an explicit unresolved contract acceptance, not as a newly discovered implementation bug.
- `OwnerHidden` promotion is explicitly a refinement needing Codex sign-off (`IMPL_DESC.md` decision 20). No sign-off is inferred by this review.
- The handoff lists residual identical-tuple HWND reuse after a lost/overflow-discarded destroy hint, quarantine capacity pinning, and other native assumptions. They are documented limitations, not hidden claims of proof.

