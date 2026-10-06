# Feature Tracker: Software vGPU Slicing

- **Contributors / Agents**: Antigravity, Codex (/root)
- **Time Started**: 2026-10-05T18:35:00Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1      | Set up Venus protocol transport over IVSHMEM ring buffer | Done | 15% | 100% | Independent mapped-file/UNIX integration and native Windows fixtures verified; physical hypervisor validation delegated to user |
| #2      | Implement host-side Venus receiver (virglrenderer/vkr integration) | Done | 20% | 100% | Pinned negotiation and eight isolated worker contexts verified; Linux/native Windows CI, safety, coverage and local hardware workload pass |
| #3      | Implement guest-side WDDM render-only driver and standalone Vulkan ICD | Pending | 25% | 0% | Requires DXVK integration testing |
| #4      | Implement zero-copy DMA-BUF export and Wayland `zwp_linux_dmabuf_v1` integration | In Progress | 15% | 15% | Receiver export ownership verified; image metadata, Wayland integration and worker FD handoff pending |
| #5      | Implement OpenCL compute remoting layer (rusticocl over Venus) for Adobe compatibility | Pending | 20% | 0% | Required for high-perf compute |
| #6      | Pin, configure, build, and verify virglrenderer dependency | Done | 5% | 100% | Immutable pin, license audit and offline build verified in CI |

**Total Feature Completion**: `42.25%`

## Commit History & Progress Log

- **Commit `2cac52d`**: `docs(tracker): initialize implementation plan and tracker for software vGPU`
  - **Task Impact**: Scaffolding baseline (0% completion)
  - **Summary**: Created initial IMPL_DESC.md and TRACKER.md for software vGPU slicing.
- **Commit `af665cc`**: `docs(tracker): align vGPU architecture on Venus and DMA-BUFs`
  - **Task Impact**: Scaffolding baseline (0% completion)
  - **Summary**: Updated architecture to use Venus Vulkan remoting and DMA-BUFs.

- **Commit `587dc10`**: `docs(tracker): add OpenCL compute remoting for Adobe compatibility`
  - **Task Impact**: +0% to TODO: #5 (+0% overall).
  - **Summary**: Extended the requested compute scope; no implementation evidence.
- **Commit `8b3edcd`**: `refactor(headers): convert header guards to PascalCase per project convention`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Existing header naming cleanup; no Venus transport implementation.
- **Commit `5398e3f`**: `docs(vgpu): specify IVSHMEM ring transport contract`
  - **Task Impact**: +10% to TODO: #1 (+1.5% overall).
  - **Summary**: Defined cache-isolated C ABI, bounded Zig validation, SPSC ordering,
    errors, ownership, milestones, and transport-versus-renderer boundary.

The current entry is identified by its unique commit subject because a commit
cannot contain its own content-derived hash. Each following atomic commit records
the preceding entry's actual hash; `git log` resolves the latest entry directly.

- **Commit `59829ff`**: `feat(vgpu): implement bounded SPSC byte transport`
  - **Task Impact**: +35% to TODO: #1 (+5.25% overall).
  - **Summary**: Added the 192-byte C11 ABI, cache-isolated cursors, local capacity
    snapshots, allocation-free Zig validation/copy helpers, and basic tests.
  - **Verification**: `make vgpu-test vgpu-sanitizers` passed with ASan/LSan/UBSan;
    Zig allocator tests passed; Windows x86-64 C object compilation passed.
    Native Windows execution, stress, coverage, and session adapters remain pending.

- **Commit `b071cc8`**: `test(vgpu): exercise corruption and concurrent ring rollover`
  - **Task Impact**: +15% to TODO: #1 (+2.25% overall).
  - **Summary**: Exhausted every ring start and bounded transfer length, corrupted
    all ABI identity/padding bytes, verified capacity snapshots and error output
    preservation, and transferred 3.1 MB in both thread and independent-process
    stress starting near 32-bit rollover. Both stress variants have a deadline.
  - **Verification**: `make vgpu-test vgpu-sanitizers` passed; Zig allocator
    tests returned all allocations. Coverage and native adapters remain pending.

- **Commit `9ee99e0`**: `test(vgpu): gate C and Zig transport coverage`
  - **Task Impact**: +5% to TODO: #1 (+0.75% overall).
  - **Summary**: Added gcov gates for production C lines and branches, reused the
    repository LLVM instrumentation for Zig validation/copy routines, and added
    reproducible Windows test cross-link targets. Coverage excludes compiler
    panic guards but includes source validation errors.
  - **Verification**: C production line/branch coverage 100%; Zig production
    line/branch coverage 100%. Native CI remains pending.

- **Commit `63862ff`**: `chore(vgpu): verify transport on Linux and native Windows CI`
  - **Task Impact**: +0% to TODO: #1 (+0% overall); CI verification pending.
  - **Summary**: Added feature-branch Linux safety/stress/coverage and native
    Windows ABI/allocator jobs with pinned tools and bounded runtime.
    This verifies transport only; renderer, WDDM, presentation, and OpenCL
    integration remain required for feature completion.

- **Commit `bfc1bb5`**: `chore(deps): pin virglrenderer 1.2.0 for Venus execution`
  - **Task Impact**: +40% to TODO: #6 (+2.0% overall).
  - **Summary**: Added public HTTPS MIT-licensed renderer dependency at immutable
    `500b41d5c8638f9b80dd558f4044f3301c7457a4`; allocated a dedicated dependency
    task by splitting 5% from unimplemented TODO #2. No guest or GPU completion
    is implied by the dependency pin.

- **Commit `bca923e`**: `chore(vgpu): build pinned Venus renderer offline`
  - **Task Impact**: +40% to TODO: #6 (+2.0% overall); +5% to TODO: #1
    (+0.75% overall) for native transport CI verification.
  - **Summary**: Added deterministic Meson/Ninja targets with wrap downloads
    disabled and base-package build prerequisites; synchronized the dependency
    in the Linux job. Built the full Venus-enabled renderer locally.
  - **Verification**: Transport Linux and native Windows jobs both succeeded at
    `63862ff` in https://github.com/Kayyo321/Waddle-LSW/actions/runs/37385937849 .
    Dependency CI for this commit remains pending; this is not a rendering test.

- **Commit `b993e78`**: `feat(vgpu): define bidirectional dedicated IVSHMEM region`
  - **Task Impact**: +5% to TODO: #1 (+0.75% overall); +20% to TODO: #6
    (+1.0% overall) for verified dependency CI.
  - **Summary**: Added a 64-byte immutable region ABI, separate command/reply
    rings, a bounded resource area, single-snapshot Zig extent validation, and
    all-or-nothing local attachment. Added portable fixtures and native CI checks.
  - **Verification**: Ring lines/branches 100%; region lines 100%, branches
    96.15%; combined Zig validation/copy lines 100%, branches 97.5%. Sanitizers
    and allocator tests pass. Dependency CI succeeded at `bca923e` in
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37386229779 .
    Dedicated driver-backed host/guest integration is still pending.

- **Commit `acf8d73`**: `feat(vgpu): own sealed Linux IVSHMEM backing mappings`
  - **Task Impact**: +5% to TODO: #1 (+0.75% overall).
  - **Summary**: Created dedicated memfd-backed BAR ownership with resize seals,
    shared mmap, deterministic free, and preserved errno. Defined the native
    mapping API and Windows ownership contract for the next atomic implementation.
  - **Verification**: Real dual mapping exchanges commands/replies; all six
    injected acquisition/post-map failures release resources. Linux mapping
    line/branch coverage 100%; ASan/LSan/UBSan and Zig allocator tests pass.

- **Commit `ff50f69`**: `feat(vgpu): map signed Windows IVSHMEM regions`
  - **Task Impact**: +0% to TODO: #1 (+0% overall); Windows fixture execution pending.
  - **Summary**: Added signed-driver cached mapping, identity validation, bounded
    SetupAPI detail allocation, ownership flags for invalid map returns, preserved
    Windows errors, and symmetric mapping/handle cleanup. Guarded the upstream
    unguarded driver header without modifying the pinned dependency.
  - **Verification**: Cross-linked a native Windows SDK fixture covering success
    and fourteen allocation/driver/ABI failures, tracking every live allocation,
    device/list handle, and mapping. Native execution is queued next. This fake
    driver fixture does not establish real cross-VM IVSHMEM functionality.

- **Commit `a7e0756`**: `feat(vgpu): add cancellable ring backpressure waits`
  - **Task Impact**: +5% to TODO: #1 (+0.75% overall).
  - **Summary**: Added exact-transfer wait adapters with caller-owned bounded
    readiness callbacks, local cancellation/deadlines, disconnect closure,
    spurious-wakeup retries, and validation of unexpected callback results.
  - **Verification**: Wait adapter production line/branch coverage 100%; the
    complete local transport/mapping/wait sanitizer and allocator suite passes.
    Windows fixtures cross-link; native execution/lifecycle handoff still pending.

- **Commit `ace5ce5`**: `docs(vgpu): record native mapping verification`
  - **Task Impact**: +5% to TODO: #1 (+0.75% overall).
  - **Summary**: Credited the Windows mapping-owner milestone only after its
    fourteen-error-path fixture executed successfully on native Windows. Recorded
    the latest ring/region/Linux-owner CI verification without crediting the
    unimplemented lifecycle handoff or actual cross-VM signed-driver test.
  - **Verification**: Both Linux and native Windows jobs passed at `ff50f69`:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37387395968 .
    Wait adapters added after this run are awaiting their own native CI result.

- **Commit `71be1b8`**: `chore(vgpu): enforce C formatting and audit transport checks`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Applied four-space LLVM formatting with a 100-column limit to
    owned feature C headers/sources/fixtures. Preserved required SDK/GUID include
    ordering; no upstream or README changes.
  - **Verification**: Local transport/owner/wait sanitizers and allocator tests
    pass. C ring/owner/wait line/branch coverage 100%; region line coverage
    98.11%, branches 96.15%; Zig lines 100%, branches 97.5%. Windows fixtures
    cross-link and the production driver adapter compiles independently.
    New transport/native Windows jobs passed at `ace5ce5` in
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37387757325 .
    Existing AV coverage gates also passed locally after extending the shared
    LLVM coverage harness. Feature completion remains 18.5%.

- **Commit `9e7940f`**: `docs(vgpu): define lifecycle readiness handoff`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Specified the fixed little-endian control frame, bounded parsing,
    offer/acknowledgement/ready ordering, stale-session rejection, cancellation,
    disconnect closure, and mapping lifetime. Split the remaining lifecycle gate
    into tested state-machine and native stream-integration milestones.
  - **Verification**: Reviewed against the existing bidirectional region and
    cancellable ring contract. No implementation credit is assigned yet.

- **Commit `0474547`**: `feat(vgpu): validate lifecycle readiness transitions`
  - **Task Impact**: +1% to TODO: #1 (+0.15% overall).
  - **Summary**: Added bounded Zig little-endian control encoding/decoding and a
    C11 session state machine that gates readiness, snapshots layout/identity,
    rejects stale/out-of-order frames, and atomically closes both directions.
    Added exhaustive role/state transitions, malformed-frame and cleanup tests,
    sanitizer/coverage gates, Windows cross-link and native CI execution targets.
  - **Verification**: `make vgpu-test vgpu-sanitizers vgpu-coverage vgpu-windows`
    passed locally. Session C line/branch coverage 100%; control Zig lines 100%,
    branches 93.75%; Zig allocator tests leak no bytes. Native Windows execution
    is not yet credited; OS stream adapters and actual cross-VM validation remain.

- **Commit `1921804`**: `docs(vgpu): specify public Venus receiver bootstrap`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Defined singleton/context/blob ownership, private bounded commands,
    callback lifetime, CPU completion versus GPU completion, exact reply mapping,
    failure teardown and receiver milestones after exercising the public API.
  - **Verification**: A local exploratory public-ABI probe executed real Venus
    EnumerateInstanceVersion and received VK_SUCCESS after timeline-zero retirement.
    The probe is not production implementation or GPU-rendering evidence.

- **Commit `81e333c`**: `docs(vgpu): record native lifecycle verification`
  - **Task Impact**: +1% to TODO: #1 (+0.15% overall).
  - **Summary**: Credited the lifecycle state-machine native Windows execution
    milestone after both Linux and Windows jobs completed successfully.
  - **Verification**: https://github.com/Kayyo321/Waddle-LSW/actions/runs/37389407524
    passed at `0474547`. Stream integration (3%) and real cross-VM signed-driver
    validation (5%) remain pending; no GPU execution credit is assigned.

- **Commit `8bb90be`**: `feat(vgpu): own public Venus receiver bootstrap`
  - **Task Impact**: +10% to TODO: #2 (+2% overall).
  - **Summary**: Added scoped renderer singleton/context/blob/map ownership,
    bounded Zig private command/reply copies, CPU timeline retirement, poisoned
    submission handling and deterministic pending/partial cleanup. Added real
    public Venus dispatch and all acquisition/error-path ownership fixtures.
  - **Verification**: `make vgpu-receiver-test vgpu-receiver-sanitizers
    vgpu-receiver-coverage` passed locally. Real renderer returned nine successful
    Vulkan instance-version replies over repeated owners/submissions and survived
    pending teardown. Owned C and Zig line/branch coverage 100%; ASan/LSan/UBSan
    and Zig allocator gates pass. CI now executes the same real public-ABI test.
    This milestone proves CPU dispatch/ownership, not GPU execution, resource
    allocation policy, session integration, guest ICD or zero-copy presentation.

- **Commit `6281b0b`**: `docs(vgpu): specify native lifecycle stream adapters`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Defined borrowed Linux VSOCK/UNIX and Windows overlapped serial
    streams, exact private framing, monotonic operation deadlines, cancellation,
    bounded backpressure waits, partial EOF and completion-event cleanup.
  - **Verification**: Reviewed against session readiness and ring exact-transfer
    contracts; native implementation and runtime validation remain pending.

- **Commit `c20e8bb`**: `feat(vgpu): hand off native lifecycle streams with deadlines`
  - **Task Impact**: +2% to TODO: #1 (+0.3% overall).
  - **Summary**: Added borrowed native Linux stream/Windows overlapped adapters,
    complete readiness delivery, private partial framing, atomic cancellation,
    monotonic operation deadlines, stop/EOF closure and bounded ring wait callbacks.
    Windows requests are cancelled and joined before stack/event reuse. Added real
    Linux sockets and cross-linked real Windows pipe tests plus coverage/CI gates.
  - **Verification**: `make vgpu-test vgpu-sanitizers vgpu-coverage vgpu-windows`
    passed locally. Channel C lines 98.37%, branches 95.10%; Linux native boundary
    lines 100%, branches 97.62%. ASan/LSan/UBSan and Zig allocator gates pass.
    Real Linux two-peer handshake, one-byte framing, full-ring progress, partial
    EOF, final Stop, cancellation during waits and deadline shutdown passed.
    Native Windows execution and the real cross-VM signed-driver gate are pending.

- **Commit `f966258`**: `docs(vgpu): record native overlapped stream verification`
  - **Task Impact**: +1% to TODO: #1 (+0.15% overall).
  - **Summary**: Credited the Windows stream-adapter execution milestone after
    actual overlapped named-pipe handshake, fragmented control, pending cancellation,
    deadline/partial EOF and event cleanup tests passed on native Windows.
  - **Verification**: Both jobs passed at `c20e8bb` in
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37390952834 .
    Named-pipe fixtures exercise Windows completion APIs, not a VirtIO driver;
    actual cross-VM signed IVSHMEM/VirtIO-Serial driver validation remains 5%.

- **Commit `e2cb2e2`**: `docs(vgpu): adopt authorized standalone validation scope`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Recorded the user's explicit mock-development direction and
    delegation of physical cross-VM testing. Replaced the final development gate
    with independently mapped processes, real UNIX control and CPU Venus dispatch,
    preserving actual hardware validation as an unclaimed user follow-up.
  - **Verification**: Existing native Windows fixtures remain required. New
    integrated mock fixture is specified before implementation; no credit yet.

- **Commit `dc1f16b`**: `test(vgpu): integrate independent mock guest and Venus replies`
  - **Task Impact**: +5% to TODO: #1 (+0.75% overall).
  - **Summary**: Completed the user-authorized mock gate using independently mapped
    /dev/shm file views, a real UNIX control handshake, ordered ring requests,
    actual Venus CPU dispatch/replies, simulated guest crash and partial EOF.
    Host joins callbacks/child before unmapping; the exclusive test file is unlinked.
  - **Verification**: `make vgpu-integration vgpu-integration-sanitizers` passed;
    ASan/LSan/UBSan reported no findings. Existing production coverage/native
    Windows gates remain enforced. CI now runs integrated mock dispatch as well.
    Task #1 is complete for standalone development, with physical hypervisor
    testing explicitly delegated to the user; no real guest driver/GPU claim.

- **Commit `a2263c7`**: `docs(vgpu): define receiver resource registry and quota`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Specified bounded IDs/ledger, explicit declared-storage/count
    quotas, lazy CPU SHM copy ownership, refunds/reuse, poisoned map cleanup,
    and the distinction between registered storage and arbitrary Vulkan VRAM.
  - **Verification**: Reviewed public renderer create/map/unref behavior and
    separated resource ownership from subsequent wire dispatch/GPU export gates.

- **Commit `8766015`**: `feat(vgpu): bound receiver resource ownership and quotas`
  - **Task Impact**: +15% to TODO: #2 (+3.0% overall).
  - **Summary**: Implemented a fixed 64-entry resource ledger, Zig-validated IDs,
    flags and copy bounds, declared-storage/count quotas, lazy CPU SHM mappings,
    quota refunds and deterministic resource teardown, including SDK unmap errors.
  - **Verification**: `make vgpu-receiver-test vgpu-receiver-sanitizers
    vgpu-receiver-coverage vgpu-integration vgpu-integration-sanitizers` passed.
    Owned receiver C lines 100%, branches 99.33%; Zig bounds lines/branches 100%.
    ASan/LSan/UBSan and Zig allocator gates report no leaks. Real public renderer
    CPU SHM copies and independent mapped-file/UNIX integration passed. Device
    memory export, GPU completion and runtime wire dispatch remain separate gates.

- **Commit `e546879`**: `docs(vgpu): specify bounded receiver request envelopes`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Defined the 64-byte little-endian header, operation-specific
    fields, explicit wire statuses, payload bounds and sequential exchange contract.
    Split remaining dispatch work into portable codec and integrated runtime gates.
  - **Verification**: Checked against resource quotas, CPU fences, fixed capset
    extent and the backing-agnostic ring/channel ownership contracts.

- **Commit `b6e825e`**: `feat(vgpu): validate portable receiver request envelopes`
  - **Task Impact**: +0% to TODO: #2 (+0% overall); 5% codec gate awaits native CI.
  - **Summary**: Added a documented C ABI and allocation-free Zig request/response
    codec with explicit little-endian fields, per-operation policy validation,
    resource-policy reuse, exact payload bounds and error output guarantees.
    Added independent C golden-wire/policy fixtures, Zig mutation/allocator tests,
    sanitizer/coverage targets and Linux/native Windows CI execution.
  - **Verification**: `make vgpu-request-test vgpu-request-sanitizers
    vgpu-request-coverage build/vgpu_request_test.exe` passed. Codec branch coverage
    100%, line coverage 98.33%; sanitizers and Zig allocator tests pass. Native
    Windows execution remains pending. Resource commit `8766015` passed both jobs
    in https://github.com/Kayyo321/Waddle-LSW/actions/runs/37392534083 .

- **Commit `aa0d30d`**: `docs(vgpu): define sequential receiver runtime ownership`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Specified private bounded framing, chunked payloads, explicit
    operation deadlines, consumed sequence rules, output preservation, quota-safe
    dispatch and terminal failures without an unreadable response guarantee.
  - **Verification**: Reviewed against codec limits, channel lifecycle and receiver
    ownership; integrated implementation and runtime acceptance remain pending.

- **Commit `4253a51`**: `feat(vgpu): dispatch bounded sequential receiver exchanges`
  - **Task Impact**: +5% to TODO: #2 (+1.0% overall) for verified portable codec;
    runtime's separate 10% acceptance gate awaits native CI.
  - **Summary**: Added allocation-free caller-buffer RPC ownership, streamed
    private framing, exact echoed response validation, sequence exhaustion and
    terminal closure. Routed all eight operations to the existing receiver;
    oversized transfers return Limit without dispatch. Replaced the temporary
    integration envelopes with real protocol resource/CPU execution exchanges.
  - **Verification**: `make vgpu-runtime-test vgpu-runtime-sanitizers
    vgpu-runtime-coverage build/vgpu_runtime_test.exe vgpu-integration
    vgpu-integration-sanitizers` passed. RPC lines 100%, branches 97.73%; dispatch
    lines 98.70%, branches 93.48%. ASan/LSan/UBSan report no findings. Independent
    mapped-process/UNIX tests cover real SHM quota/refund/reuse, payloads larger
    than the ring, real CPU Venus replies, stale/malformed/truncated requests and
    peer crashes. Native runtime execution is queued; no GPU execution claim.
    Codec Linux/native Windows CI succeeded at `b6e825e` in
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37392958717 .

- **Commit `f99723a`**: `fix(vgpu): track runtime prerequisites before integration rules`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Moved runtime source-list definitions before Make expands the
    integration prerequisites, and added explicit ABI/private header dependencies.
    Runtime edits now reliably rebuild integrated and portable fixtures.
  - **Verification**: Inspected Make's expanded integration target and asserted
    both runtime sources and ABI/private headers are present. Runtime behavior
    is unchanged; the preceding validated integration build contained both sources.

- **Commit `d384dee`**: `docs(vgpu): specify GPU queue fence ownership`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Defined bounded queue timeline slots, independent issued/retired
    identities, inline callback publication, stale/future callback handling and
    CPU versus GPU semantics. Split ownership, hardware and recovery acceptance.
  - **Verification**: Matched the pinned public fence ABI and vkr queue association;
    callback retirement does not by itself prove successful GPU execution.

- **Commit `ec97da1`**: `feat(vgpu): track bounded GPU queue fence retirement`
  - **Task Impact**: +8% to TODO: #2 (+1.6% overall) for GPU fence ownership;
    +10% to TODO: #2 (+2.0% overall) for verified sequential runtime CI.
  - **Summary**: Added fixed per-queue issued/retired atomics, Zig timeline bounds,
    independent backpressure, inline callback publication, future-callback poison,
    monotonically retired maxima and explicit GPU fence polling. CPU completion
    remains separate. No queue/device creation or successful GPU work is implied.
  - **Verification**: `make vgpu-receiver-test vgpu-receiver-sanitizers
    vgpu-receiver-coverage vgpu-integration vgpu-integration-sanitizers` passed.
    Receiver C lines 100%, branches 98.91%; Zig bounds lines/branches 100%.
    All 63 GPU slots, inline/concurrent callbacks, SDK failure, exhaustion,
    pending cleanup and CPU/GPU separation tested through the SDK mock boundary.
    ASan/LSan/UBSan and allocator tests report no leaks. Real CPU Venus dispatch
    and independent mock integration still pass. Real hardware queue execution,
    runtime GPU fence operations and bounded hang recovery remain pending.
    Sequential runtime Linux/native Windows CI passed at `4253a51` in
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37393773120 and at
    `f99723a` in https://github.com/Kayyo321/Waddle-LSW/actions/runs/37393832459 .

- **Commit `59065fd`**: `docs(vgpu): define GPU fence protocol routing`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Defined GpuFence/GpuPoll extension fields, exact retirement
    semantics, matching-build requirement and separate wire/health/recovery gates.
  - **Verification**: Reviewed against bounded per-queue receiver APIs; no queue
    creation or hardware execution evidence is implied by the wire extension.

- **Commit `fb16a1d`**: `feat(vgpu): route GPU fence requests through bounded runtime`
  - **Task Impact**: +0% to TODO: #2 (+0% overall); 3% GPU wire gate awaits native CI.
  - **Summary**: Extended the codec with GpuFence/GpuPoll, validated u64 timelines
    before narrowing and routed accepted private requests to per-queue APIs.
    Retained zero-payload errors, explicit CPU/GPU separation and existing frames.
    Corrected asynchronous worker acceptance semantics from pinned proxy source.
  - **Verification**: Request/runtime unit, sanitizer, coverage and Windows
    cross-link targets passed; independent mapped-process real CPU/resource
    integration also passes. Codec lines 98.36%, branches 100%; RPC lines 100%,
    branches 97.73%; dispatch lines 98.78%, branches 93.75%. Native CI is pending.
    Mock routing does not prove real GPU queue execution or bounded hang recovery.

- **Commit `1c488f7`**: `docs(vgpu): specify receiver health deadline policy`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Defined independent absolute fence budgets, host-only policy,
    monotonic clock/cancellation poison and scoped runtime health callbacks.
  - **Verification**: Distinguished bounded health sampling from preempting SDK
    calls and isolated worker recovery, which remain separate requirements.

- **Commit `2568f9e`**: `feat(vgpu): enforce absolute receiver health deadlines`
  - **Task Impact**: +3% to TODO: #2 (+0.6% overall) for verified GPU wire CI;
    health's separate 3% gate awaits current CI.
  - **Summary**: Armed independent absolute CPU/per-queue GPU budgets, added
    quiescent host-only policy and clock/cancellation/expiry poison. Scoped health
    callbacks now run before chunks and inside backpressure retries, and are
    cleared on every host serve return. Poll exchanges cannot extend fence budgets.
  - **Verification**: Receiver/runtime tests, sanitizers, coverage, Windows
    cross-link and independent real CPU/resource mock integration passed. Receiver
    lines 100%, branches 98.76%; RPC lines 100%, branches 97.79%; dispatch lines
    98.92%, branches 93.75%. Clock/range/regression/overflow, exact retirement/expiry,
    independent queues, repeated polling, cancellation and callback lifetime covered.
    GPU wire Linux/native Windows CI passed at `fb16a1d` in
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37394675981 .
    Current health CI, real GPU execution and isolated worker recovery remain pending.

- **Commit `753e740`**: `docs(vgpu): specify isolated receiver process ownership`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Defined trusted exec/descriptor handoff, owned process groups,
    observe-before-reap identity safety and bounded TERM/KILL shutdown. Kept actual
    worker integration/restart separate from process ownership acceptance.
  - **Verification**: Explicitly retains unreaped identities on timeout; killing
    a stuck kernel task is not falsely represented as complete recovery.

- **Commit `a07549c`**: `feat(vgpu): own isolated receiver process groups`
  - **Task Impact**: +3% to TODO: #2 (+0.6% overall) for verified health CI;
    process ownership's separate 1% gate awaits current CI.
  - **Summary**: Added collision-safe trusted exec/descriptor handoff, symmetric
    launch cleanup, observe-before-reap process identity, descendant disposal and
    bounded TERM/KILL shutdown with retained ownership on timeout/OS failure.
  - **Verification**: `make vgpu-worker-test vgpu-worker-sanitizers
    vgpu-worker-coverage` passed; owned lines 98.35%, branches 94.26%.
    Real exec and TERM-resistant descendants, all launch acquisition failures,
    wait interruptions/external reaping, clock/timeout faults and descriptor
    preservation tested; ASan/LSan/UBSan report no findings. CI now runs these gates.
    Health Linux/native Windows CI passed at `2568f9e` in
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37395109626 .
    Actual receiver worker integration, guest restart notification and real GPU
    execution remain pending; process shutdown tests do not prove GPU recovery.

- **Commit `6ec88a2`**: `docs(vgpu): define isolated receiver service execution`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Specified trusted service policy, inherited mapping validation,
    random session identity, symmetric bootstrap/teardown, closure notification
    and fresh mapping/stream/worker restart requirements.
  - **Verification**: Kept parent process shutdown budgets mandatory for blocked
    SDK teardown; mock restart does not prove physical GPU recovery.

- **Commit `42637d1`**: `fix(build): declare worker coverage object prerequisite`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Build the transport bounds object before worker coverage linking
    on a clean checkout; avoid reliance on unrelated earlier targets.
  - **Verification**: Diagnosed missing object in Linux CI run `37395678271`;
    worker functional and sanitizer tests passed there. Local worker coverage
    passes at 98.35% lines and 94.26% branches.

- **Commit `623f17e`**: `feat(vgpu): run receiver in isolated mapped service`
  - **Task Impact**: +0% to TODO: #2 (+0% overall); the separate process/service
    acceptance gates await CI before progress credit.
  - **Summary**: Added a trusted worker executable and symmetric service ownership
    over borrowed CLOEXEC mapping/control descriptors, fresh random session IDs,
    host policy, private storage and real receiver dispatch. Added acquisition
    fault tests and exec-isolated guest disconnect/corruption/termination/restart
    fixtures with instrumented parent and service child.
  - **Verification**: Service, integrated mapped/UNIX tests and ASan/LSan/UBSan
    pass locally; service production coverage 100% lines and 98.65% branches.
    All acquisition failures release owned memory and retain borrowed descriptors.
    Native Windows framing is unchanged; physical GPU queue evidence remains pending.

- **Commit `035c2e2`**: `test(vgpu): exercise production worker cancellation`
  - **Task Impact**: +0% to TODO: #2 (+0% overall); CI acceptance pending.
  - **Summary**: Launch the actual default-policy executable, execute a real
    Venus CPU query, send SIGTERM and observe a cooperative zero-status exit,
    closed rings and guest notification without forced termination.
  - **Verification**: Normal and ASan/LSan/UBSan integrated tests pass with the
    production executable instrumented separately from its parent.

- **Commit `b61a83d`**: `docs(readme): add Waddle Usage section and application logo`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Separate documentation work; no software vGPU implementation credit.

- **Commit `171ccda`**: `docs(readme): add padding and spacing around images and text`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Separate documentation work; no software vGPU implementation credit.

- **Commit `2ac629b`**: `docs(vgpu): record isolated receiver CI acceptance`
  - **Task Impact**: +3% to TODO: #2 (+0.6% overall): +1% process ownership
    and +2% real service integration, terminal guest notification and fresh restart.
  - **Summary**: Credit completed acceptance gates without conflating mapped/UNIX
    recovery plumbing with physical GPU reset or cross-VM validation.
  - **Verification**: Linux and native Windows Software vGPU CI passed at
    `623f17e`, including service acquisition fault coverage, sanitizer-instrumented
    mapped/exec integration and fresh launches:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37396594127 .
    Production executable cancellation also passes locally at `035c2e2`; its
    additional CI run `37396744418` is tracked separately.

- **Commit `8aacbda`**: `Updated default-window-icon.png`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Separate application artwork update; no vGPU task credit.

- **Commit `c1d9c07`**: `docs(vgpu): specify real Venus queue acceptance`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Specify bounded Zig fixture serialization/parsing, physical
    device selection, queue timeline association, CPU/GPU ordering, ownership
    and software CI evidence. Split bootstrap (2%) from verified workload (6%)
    without treating empty submissions as GPU workload execution.

- **Commit `ff23bab`**: `fix(vgpu): preserve libc lookup across Zig objects`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Compile Linux C-linked Zig objects with libc so a freestanding
    weak getauxval implementation cannot override system startup/driver lookups.
    Add regression checks for both runtime and receiver link sets and CI gates.
  - **Verification**: Original object reproduces null auxiliary-vector lookup;
    corrected objects pass regression and ASan/LSan/UBSan tests. Existing transport,
    receiver, request, runtime, service and mapped/exec suites plus sanitizers pass.

- **Commit `c35ca4b`**: `docs(assets): preserve independent application artwork updates`
  - **Task Impact**: +0% to TODO: #1 (+0% overall).
  - **Summary**: Preserve separate README and application image changes staged
    concurrently; no vGPU implementation credit.

- **Commit `a54d571`**: `test(vgpu): execute real Venus queue bootstrap`
  - **Task Impact**: +0% to TODO: #2 (+0% overall); hardware bootstrap 2%
    gate awaits current CI before credit. Actual GPU workload output remains pending.
  - **Summary**: Add bounded Zig command/reply fixture for instance/device/family
    enumeration, device queue creation with timeline 1, queue submission and three
    monotonically retired GPU fences, then explicit object destruction.
  - **Verification**: Hardware-required normal and sanitizer runs pass on NVIDIA
    GeForce RTX 5080 and AMD RADV integrated hardware. Linux llvmpipe runs pass
    separately, and hardware-required mode rejects that CPU backend. Zig fixed
    scalar/bounds/allocator tests pass. Existing receiver/runtime/service coverage
    remains above 90%; Linux CI installs a system software Vulkan test driver.
    Production worker cancellation CI passed at `035c2e2` in run `37396744418`,
    and acceptance tracker CI passed at `2ac629b` in run `37397026658`.

- **Commit `fced4de`**: `docs(vgpu): define GPU timestamp workload verification`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Specify real recorded command submission and GPU-written query
    output checks, bounded blob parsing, availability/modular counter rules and
    quiescent query/command ownership. Keep graphics/compute/presentation tasks separate.

- **Commit `60321bc`**: `test(vgpu): verify GPU timestamp workload output`
  - **Task Impact**: +2% to TODO: #2 (+0.4% overall) for verified hardware
    queue bootstrap; timestamp workload separate 6% gate awaits current CI.
  - **Summary**: Record query reset and GPU top/bottom timestamp commands, submit
    the actual command buffer three times, fence GPU completion and parse exact
    nonblocking query result blobs with availability and modular counter checks.
    Destroy query/command pools before device/instance teardown.
  - **Verification**: Hardware-required normal and ASan/LSan/UBSan runs pass on
    NVIDIA RTX 5080 and AMD RADV; llvmpipe CI backend normal/sanitizer runs pass
    locally. Zig allocator tests cover all truncated lengths, invalid widths,
    failed/absent data, unavailable queries, wrap, reverse and equal timestamps.
    Bootstrap and libc-boundary Linux/native Windows CI passed at `a54d571`:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37398115404 .
    GPU timestamps verify queue commands, not guest shader/presentation/OpenCL APIs.

- **Commit `8f2e6f6`**: `docs(vgpu): define pinned capability decoding`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Specify exact portable capset offsets, flag shape, pinned
    version/profile compatibility, bounded explicit extension queries and
    private caller ownership. Allocate separate decoding, negotiation and
    multi-context acceptance gates without crediting unimplemented runtime policy.

- **Commit `0a43c16`**: `feat(vgpu): decode pinned capability snapshots safely`
  - **Task Impact**: +6% to TODO: #2 (+1.2% overall) for verified hardware
    workload; separate capability decoding 5% gate awaits current native CI.
  - **Summary**: Add bounded Zig private capset decoding, exact pinned profile
    policy and explicit extension queries, C ABI/header documentation and native
    fixtures. Mapped/UNIX guest now validates the actual public receiver capset.
  - **Verification**: Linux C/Zig tests, allocator checks, ASan/LSan/UBSan and
    independent mapped/exec integration pass; Windows cross-link passes. Owned
    Zig coverage 100% lines and 93.33% branches. CI adds Linux and native Windows
    capability gates; session negotiation and context runtime remain pending.
    GPU timestamp Linux/native Windows CI passed at `60321bc`:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37398576725 .


- **Commit `e9dcc0b`**: `docs(vgpu): specify negotiated isolated context completion`
  - **Task Impact**: +5% to TODO: #2 (+1.0% overall) for verified capability decoding.
  - **Summary**: Specify operation-11 profile negotiation and host dispatch gate,
    bounded independent worker contexts, aggregate budgets, handle lifetime,
    failure ownership and simultaneous execution acceptance before coding.
  - **Verification**: Linux/native Windows capability CI passed at `0a43c16`:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37399147082 .
    No credit yet for negotiation or context manager implementation.


- **Commit `35076e2`**: `feat(vgpu): gate receiver dispatch on pinned negotiation`
  - **Task Impact**: +0% to TODO: #2 (+0% overall); 10% negotiation gate awaits native CI.
  - **Summary**: Add bounded operation-11 guest profile declaration, pinned host
    compatibility checks, pre-negotiation SDK rejection and private session state.
    Existing mapped/exec guests now negotiate before quotas and CPU dispatch.
  - **Verification**: Request/runtime fixtures, real mapped/exec integration,
    ASan/LSan/UBSan and Zig allocator checks pass. Request codec production
    coverage exceeds 90%; dispatch lines 99.10%, branches 93.94%; RPC coverage
    passes. Windows request/runtime cross-link passes; native CI follows.


- **Commit `4b8913a`**: `feat(vgpu): bound isolated context lifetimes and mapping budgets`
  - **Task Impact**: +10% to TODO: #2 (+2.0% overall) for bounded controller ownership.
  - **Summary**: Add eight-slot process-isolated controller, monotonic nonreused
    handles, aggregate mapping/count budgets, duplicate backing rejection,
    retained CLOEXEC fd ownership, bounded individual shutdown and failure retry.
    Teardown uses validated creation-time offsets even after peer metadata changes.
  - **Verification**: Native Linux fixtures exercise all acquisition/shutdown
    failures, per-slot budget refunds and 128 churn cycles under ASan/LSan/UBSan.
    Production C line coverage 100%, branch coverage 96.61%. Existing worker
    process-launch and recovery fixtures remain separately gated. Simultaneous
    renderer execution is the next distinct 10% acceptance gate.


- **Commit `ae50f3d`**: `Updated README.md`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Independent documentation commit outside receiver work;
    no software vGPU task credit or receiver source change.

- **Commit `75d39cd`**: `test(vgpu): verify simultaneous isolated context execution`
  - **Task Impact**: +0% to TODO: #2 (+0% overall); simultaneous execution 10% awaits CI.
  - **Summary**: Execute eight live production workers over separate mapped
    regions/UNIX streams with identical registry/resource/fence IDs, distinct
    resource contents and actual Venus CPU replies. Exercise profile mismatch
    retry, repeated negotiation, one-worker SIGKILL, seven-context survival,
    fresh startup, stale-handle rejection and three full churn rounds.
  - **Verification**: Native Linux normal and ASan/LSan/UBSan integration runs
    pass with all caller descriptors returned to baseline. Workers use the
    sanitizer-instrumented production executable. Add these gates to Linux CI.
    Native Windows negotiation fixtures have passed; remaining Linux CI is running.


- **Commit `24f83f3`**: `chore(vgpu): complete standalone negotiation build dependencies`
  - **Task Impact**: +10% to TODO: #2 (+2.0% overall) for negotiated runtime acceptance.
  - **Summary**: Make standalone runtime sanitizer/coverage targets build the
    capability object they link, and track context consumer header dependencies.
  - **Verification**: `make -n -W src/vgpu/venus_capabilities.zig
    vgpu-runtime-sanitizers vgpu-runtime-coverage` includes codec rebuild before
    linking. Linux/native Windows negotiation, ownership, real mapped/exec,
    coverage and sanitizer CI passed at `35076e2`:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37400157106 .
    Simultaneous context runtime acceptance is the final 10% gate.


- **Commit `62b6393`**: `Ok look something changed in the README.md`
  - **Task Impact**: +0% to TODO: #2 (+0% overall).
  - **Summary**: Independent documentation commit outside receiver work; no
    vGPU source changes or task credit.

- **Commit `8541e4d`**: `docs(vgpu): record completed host receiver acceptance`
  - **Task Impact**: +10% to TODO: #2 (+2.0% overall) for simultaneous context runtime acceptance.
  - **Summary**: Mark only TODO #2 complete; record exact host receiver scope,
    context handle lifetime and verification evidence. Remaining feature tasks
    and feature Time Ended remain unchanged because the feature is incomplete.
  - **Verification**: Linux and native Windows Software vGPU gates passed at
    `75d39cd`, including negotiated request/runtime, eight-worker execution,
    isolated ownership, crash/restart stress, descriptor baseline, sanitizer,
    allocator, coverage, legacy transport and software Vulkan GPU fixtures:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37400635748 .
    Full local host receiver suite and NVIDIA RTX 5080 hardware timestamp workload
    pass; context C coverage 100% lines/96.61% branches, RPC 100%/97.14%,
    dispatch 99.10%/93.94%, receiver 100%/98.76%, request Zig 98.39%/100%.
    Formatting and diff checks pass. README changes were independent commits.
  - **TODO #2 Completed At**: 2026-10-06T01:50:12Z

- **Commit `c737b48`**: `docs(vgpu): specify guest and presentation acceptance boundaries`
  - **Task Impact**: +0% to TODO: #3; +0% to TODO: #4 (+0% overall).
  - **Summary**: Divide guest and presentation scope into independently verifiable
    milestones; specify public Venus DMA-BUF export, completion ordering,
    descriptor ownership, image-layout and cross-process handoff boundaries.
  - **Verification**: Inspected pinned public renderer export/query implementation,
    existing AV Wayland client and system Vulkan/Wayland dependencies. WDK/native
    guest access is requested; no driver or full ICD implementation credit.

- **Commit (current; resolve by subject)**: `feat(vgpu): export fence-ordered device-memory DMA-BUFs`
  - **Task Impact**: +15% to TODO: #4 (+2.25% overall).
  - **Summary**: Add Linux public-ABI export of registered shareable device memory
    after CPU and explicit GPU fence retirement, independent CLOEXEC FD ownership,
    strict FD type checks and symmetric export-failure cleanup without pixel copies.
  - **Verification**: Receiver unit/real CPU tests, ASan/LSan/UBSan and coverage
    pass. Production receiver lines 100%, branches 98.89%; Zig bounds 100%/100%.
    Faults include acquired-FD SDK error, opaque/SHM rejection, invalid metadata,
    CLOEXEC get/set failure, pending/poisoned fences, 128 export/close cycles and
    exported FD survival after ledger free. Hardware export is a separate gate.
