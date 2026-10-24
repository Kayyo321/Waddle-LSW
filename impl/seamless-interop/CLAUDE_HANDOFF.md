# Claude Code handoff: session-wide guest window graph

Status: **reserved for a separate Claude Code session; not launched by Codex**.
The user requested document-based parallel collaboration, without the Claude
subagent skill. Codex continues the main seamless-interop work and critically
reviews the returned patch. This assignment is intentionally independent of the
active slot-runtime, canonical-path and manifest-store lanes.

## Start here

Open Claude Code with this exact working directory:

`/home/dev/.codex/worktrees/seamless-window-graph/Waddle-LSW`

Branch: `feat/seamless-window-graph`.
Base: `e86cf8e344c6bafea5e562f04129a408e19629ff` from
`feat/seamless-interop`. The worktree is registered with this Codex chat.
It deliberately excludes Codex's newer uncommitted work. Do not copy the main
checkout into this directory or use its build outputs.

Suggested initial prompt:

> Read AGENTS.md and impl/seamless-interop/CLAUDE_HANDOFF.md. Implement the
> bounded guest window graph and Win32 observation adapter described there in
> this isolated worktree. Keep strictly to the ownership list. Finish with
> impl/seamless-interop/claude_window_graph/HANDOFF_RETURN.md using the supplied
> template. Do not merge, push, operate a VM, or change files owned by Codex.

## Meaningful deliverable and tracker scope

Implement the reusable engine and native observation adapter for TODO **#10**:
session-wide window classification, parent/owner graph, monotonic identities,
bounded callback ingestion, parent-first events, retirement and refresh/capacity
recovery. Deliver production code and executable tests, ready for Codex to wire
into the future guest session main loop. This removes substantial independent
work from the guest-session critical path.

This is not a request to mark #10 complete. Actual session dispatch/capture/
slot integration and native installed-agent qualification remain with Codex.
Return exact evidence and integration requirements; Codex awards global tracker
credit only after review. Do not edit the global TRACKER or IMPL_DESC.

## Exclusive ownership

You may create/edit only these paths in **your isolated worktree**:

- `src/interop/window_graph.zig`
- `src/interop/window_graph_abi.zig`
- `include/waddle/interop_window_graph.h`
- `src/interop/window_observer_windows.c`
- `src/interop/window_observer_windows.h`
- `tests/interop/window_graph.c`
- `tests/interop/window_observer_windows.c`
- `tests/interop/window_observer_api_double.h` (if needed)
- `scripts/interop_window_graph.mk` (standalone proposed targets only)
- `impl/seamless-interop/claude_window_graph/IMPL_DESC.md`
- `impl/seamless-interop/claude_window_graph/TRACKER.md`
- `impl/seamless-interop/claude_window_graph/HANDOFF_RETURN.md`

The handoff and return-template files supplied by Codex are read-only.
Additional paths require a documented request in the return, not unilateral
edits. Do not touch the main checkout at `/home/dev/Waddle-LSW`.

Codex reserves every other path, especially `interop_slot_runtime.zig`,
`manifest_store.zig`, layout/heap/codec/native atomics, shared wire headers,
CLI/daemon/common paths, build/workflows, global docs/tracker, submodules and
legacy `src/av`. Codex and its Luna workers will not edit your listed paths
until you explicitly hand them back through the return document.

## Read-only inputs

Read these from your baseline:

1. `AGENTS.md` — naming, language, documentation, tests and atomic commits.
2. `impl/seamless-interop/IMPL_DESC.md`: §§2.2/2.4,3.2.8,3.3,4.3/4.4,5.2/5.3,
   6.2 and7. In particular, the exact classification precedence is in §4.4.
3. `include/waddle/interop_protocol.h` — frozen protocolv2 roles, state masks,
   field bounds, generations and UTF-8 conventions.
4. `src/interop/interop_codec.zig` and `interop_layout.zig` — validation/API
   conventions; don't duplicate or change wire parsing.
5. `src/av/av_windows.c/.h`, `av_identity.c/.h`, and relevant Windows fixtures —
   observation/lifetime patterns to study. The legacy per-PID16-window tracker
   is not a compatible implementation of this session-wide64-window task.

No secret files, environment dumps, credentials, guest disks or unrelated
worktrees are needed. Do not read them or put them into the return document.

## Required behavior and boundaries

Document your exact APIs and memory/state ownership in the task-local
IMPL_DESC before or during implementation. Public C functions/types/constants
need structured docs; Zig symbols need `///` docs. C is for Win32 observation,
Zig for bounded graph/storage and validation; no managed runtime or new library.
Own identifiers follow snake_case, constants/enum members PascalCase, types `_t`.

- One caller-owned graph per nonzero session epoch, mutated only on the guest
  event-loop owner thread. Bounded value observations; callbacks cannot keep
  borrowed native pointers or mutate the graph reentrantly. No worker writes
  protocol frames. No live transport, Wayland, capture, input or heap ownership
  is added in this assignment.
- Up to64 exported windows, plus the separate bounded64 deferred announcements.
  A full deferred set evicts oldest deferrals for refresh retry; a full exported
  graph never evicts a live window. Retry admission on250ms refresh; rate-limit
  capacity diagnostics to once/minute with an injectable monotonic clock.
- Identity is `(session_epoch, HWND-value, generation)`. Generations start at1,
  increase for every admission, never reset/reuse within an epoch, and never
  wrap. HWND reuse must get a fresh generation. Validate the captured epoch on
  asynchronous work. Stale observations/actions cannot mutate a newer window
  incarnation or fail-close an unrelated new session.
- Implement the exact §4.4 role precedence/exclusions using a portable snapshot
  of native facts. Preserve disabled visible windows, zero-area/transparent
  exclusion, owner/root rules, menu versus tooltip, owned APPWINDOW handling,
  shell-host-specific CoreWindow exclusion, agent/preedit exception and all
  UAC modes. Do not broaden exclusion to all processes or arbitrary classes.
- Parent references name an earlier-announced still-exported generation. Defer
  children until that parent is eligible/announced, flush parent before child,
  reject cycles/self-ownership, and handle parent retirement/reclassification
  explicitly. Refresh/snapshot order must be deterministic.
- Emit owned/bounded value events for Announce/Update/retirement intent; do not
  emit FrameReady or invent protocol messages. Wire serialization remains a
  future consumer of the frozen codec. Validate/copy UTF-8 and field bounds;
  no callback-borrowed title/app-key slices escape their lifetime.
- App-key lookup is a caller-supplied bounded callback/value. App catalog and
  icon extraction are not yours. Handle lookup/admission failure without a
  partially published graph or a fabricated application identity.
- Retirement intent blocks new actions and quarantines the graph entry until
  the coordinator confirms capture stopped and outstanding frame pins drained.
  A stale or duplicate completion cannot release a newer entry. The observer
  does not free heap runs or unmap shared memory. Role/identity changes that
  require a new host surface use explicit retirement/new generation.
- On transport reset/stop: cease callback production, unhook native hooks,
  drain/ignore queued observations, and free only owned handles/allocations.
  Document the boundary where the session coordinator supplies frame-drain
  proof. Never turn loss of connection into proof that host buffers are gone.
- The Win32 adapter observes the interactive desktop session, not one PID tree.
  Use WinEvent callbacks as hints and requery current facts. Prove delayed
  destroy/hide callbacks and recycled HWNDs cannot retire a newer live object.
  If you use per-window native identity markers, own and remove only your exact
  markers; do not overwrite unrelated properties. Describe access-denied and
  desktop-switch behavior without claiming UIAccess or secure-desktop proof.
- Failures are explicit: allocation/capacity/callback delivery/queue overflow,
  native hook failure, invalid facts, generation exhaustion and thread misuse.
  Stage mutations or fail closed with retained ownership; never silently drop
  required control events. APIs must document output/state behavior on error.

Use an explicit opaque C boundary for the Zig graph. You own the new module
header; it must not change wire structs. If dynamic storage is used, provide
symmetric create/destroy with one owner and allocation-failure tests. Propose a
small stable API rather than exposing Zig internal layout to C.

## Tests and evidence

Keep outputs/caches within this worktree's `build/` or uniquely named `/tmp`
directories. Do not call make targets that resolve the main checkout's build.
There are no VM images here. Do not initialize all submodules or install tools.
Use the existing Zig0.13/system C toolchain where available.

Required portable tests:

- All64 admissions,65th rejection/retry,64 deferred entries and deterministic
  overflow/refresh; exact generation growth/exhaustion and same-HWND reuse.
- Every classification row/precedence, process/class exclusions, visibility,
  alpha/zero geometry, parent/root edge cases and all UAC settings.
- Parent-before-child, deferred cycles, missing/retiring parent, deep chains,
  ownership change and stale parent generation.
- Update coalescing if used, immutable event values, bounded title/app-key,
  invalid UTF-8, exact snapshot/output behavior on errors.
- Delayed/reordered callback, reentrancy, wrong thread/epoch/generation,
  teardown with queued callbacks and native API partial-init failure.
- Retirement with outstanding capture/pin proof, duplicate/stale completion,
  reconnect/new epoch, event sink failure and queue/counter exhaustion.
- Random/stress traces and every allocator failure with zero leaked bytes.

Add C ABI layout/link checks for Linux and x86_64 Windows where applicable;
use API doubles for deterministic Win32 lifecycle tests. A native Windows
fixture may be built, but **do not run a VM or claim native execution**. Codex
owns the only Windows/GPU runner and will schedule native tests after review.
Likewise API doubles are not real-session or desktop acceptance.

Provide standalone module targets in your new `.mk` file and exact commands.
Do not include it from GNUmakefile/scripts/interop.mk yourself. Aim >=90% line
and branch coverage for the graph, actual production scope only. Run C tests
under ASan/LSan/UBSan with leak detection enabled where supported. Record any
runner/compiler limitation; disabling leak checks is not a passing gate.

## Git and non-interference

Work only on `feat/seamless-window-graph` in the isolated worktree. You may make
small local conventional commits for your owned files and task-local tracker,
with detailed bodies and `Affects TODO: #10 (+0 global; awaiting Codex review)`.
Keep the global tracker untouched. Do not push, merge, rebase, switch the main
checkout, change Git config, stage unrelated paths, run git clean/reset/stash,
or update submodules. No nested agents or background hardware/CI jobs.

Codex may advance/rebase its main feature independently. Your base stays pinned
until handback. Return your exact branch/HEAD, commit list and clean/dirty state.
Codex will review/cherry-pick selected commits or patches; do not self-integrate.
Stop after writing the return document and explicitly release file ownership.
Do not keep editing while Codex reviews your returned revision.

## Mandatory return document

Write `impl/seamless-interop/claude_window_graph/HANDOFF_RETURN.md`, based on
`impl/seamless-interop/CLAUDE_RETURN_TEMPLATE.md`. It must state:

- status complete/partial/blocked for this assignment, not the entire feature;
- exact base/HEAD/branch and every changed/untracked file/commit;
- API and ownership contracts, integration steps and assumptions;
- test commands with cwd/environment/exit codes and durable log locations;
- production coverage denominator/exclusions and sanitizer/native limitations;
- unresolved findings, failed attempts, unsupported cases and rollback;
- remaining global #10 work and explicit stopped-process/ownership handback.

No confidence-only completion claims. Codex will inspect code and reproduce
critical checks before accepting any implementation or tracker credit.
