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
| #3      | Implement guest-side WDDM render-only driver and standalone Vulkan ICD | In Progress | 25% | 50% | Frontend, userland adapter, object ownership and native loader/shared ICD production-worker dispatch verified; complete API/command/mapping/DXVK gates pending |
| #4      | Implement zero-copy DMA-BUF export and Wayland `zwp_linux_dmabuf_v1` integration | Done | 15% | 100% | Production worker export, native Wayland import/release and exact-once guest acknowledgements verified on RTX5080; safety/coverage and Linux/native Windows CI pass; physical compositor acceptance downstream |
| #5      | Implement OpenCL compute remoting layer (rusticocl over Venus) for Adobe compatibility | Pending | 20% | 0% | Required for high-perf compute |
| #6      | Pin, configure, build, and verify virglrenderer dependency | Done | 5% | 100% | Immutable pin, license audit and offline build verified in CI |
| #7      | Pin, configure and verify guest Venus serializer generator | Done | 0% | 100% | Exact receiver schema reproduced offline; Linux/Windows declarations compile |
| #8      | Pin, build and verify native Vulkan loader with existing protocol headers | Done | 0% | 100% | Offline Linux normal/sanitized and native Windows loader device/fence discovery pass in GitHub CI |

**Total Feature Completion**: `67.5%`

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

- **Commit `33dd751`**: `feat(vgpu): export fence-ordered device-memory DMA-BUFs`
  - **Task Impact**: +15% to TODO: #4 (+2.25% overall).
  - **Summary**: Add Linux public-ABI export of registered shareable device memory
    after CPU and explicit GPU fence retirement, independent CLOEXEC FD ownership,
    strict FD type checks and symmetric export-failure cleanup without pixel copies.
  - **Verification**: Receiver unit/real CPU tests, ASan/LSan/UBSan and coverage
    pass. Production receiver lines 100%, branches 98.89%; Zig bounds 100%/100%.
    Faults include acquired-FD SDK error, opaque/SHM rejection, invalid metadata,
    CLOEXEC get/set failure, pending/poisoned fences, 128 export/close cycles and
    exported FD survival after ledger free. Hardware export is a separate gate.

- **Commit `41ebf74`**: `docs(vgpu): specify image layout and feedback validation`
  - **Task Impact**: +0% to TODO: #3; +0% to TODO: #4 (+0% overall).
  - **Summary**: Record reaffirmed userland WDDM/standalone acceptance scope and
    bounded packed/NV12 plane and complete-feedback-table validation contracts.
  - **Verification**: Checked system linux-dmabuf protocol table/index schema;
    preserve separate import/allocation/descriptor-handoff acceptance gates.

- **Commit `0ab1bf9`**: `feat(vgpu): validate image planes and complete DMA-BUF feedback`
  - **Task Impact**: +15% to TODO: #4 (+2.25% overall); remaining 5% validation
    credit waits for native Windows CI.
  - **Summary**: Add allocation-free Zig packed/NV12 geometry and allocation-range
    validation, bounded native-endian format-table/tranche matching and documented
    C ABI. Ignore protocol-defined unused padding. Add independent Make and CI gates.
  - **Verification**: Linux C ABI, Zig allocator tests, ASan/LSan/UBSan and Windows
    x86_64 cross-link pass. Zig production coverage 100% lines/100% branches;
    all 4096 table indices, late corruption and descriptor fields exercised.
    No wl_buffer imports or worker FD handoff are claimed by codec tests.

- **Commit `5bcaa0b`**: `docs(vgpu): define negotiated guest frontend ownership`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify constant guest encoder profile, borrowed ready RPC,
    private host capabilities, wire-status translation and sticky session loss.
  - **Verification**: Keep serialized Vulkan entry points, userland adapter stub
    and DXVK acceptance separate from the command/resource/fence backend.

- **Commit `1e2ad7d`**: `feat(vgpu): own negotiated guest frontend and sticky session loss`
  - **Task Impact**: +5% to TODO: #3 (+1.25% overall); integrated/native acceptance pending.
  - **Summary**: Add caller-owned allocation-free guest command/resource/fence
    frontend, constant mandatory-only encoder declaration, private host capability
    checks, exact status translation and terminal loss preventing further publication.
  - **Verification**: Linux unit/fault/churn fixtures, ASan/LSan/UBSan and Windows
    x86_64 cross-link pass. Production C lines 100%, branches 100%. Every init
    precondition, profile mismatch, exchange acquisition failure, wire status,
    sticky terminal result and idempotent teardown tested; native CI follows.

- **Commit `4ca8195`**: `test(vgpu): exercise guest frontend across isolated receiver contexts`
  - **Task Impact**: +0% to TODO: #3 (+0% overall); integrated/native 5% gate awaits CI.
  - **Summary**: Route real negotiation, resource accesses, CPU submission/poll/reply
    through the guest frontend across eight isolated workers and fresh crash recovery.
  - **Verification**: Three rounds/27 production launches pass in normal and
    ASan/LSan/UBSan runs with descriptor counts returned to baseline. Retain
    pre-negotiation/profile retry/repeated negotiation negative protocol tests.

- **Commit `b8b8231`**: `docs(vgpu): specify Wayland import pacing and release ownership`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Define borrowed target surface/global, atomic feedback snapshots,
    bounded asynchronous import slots, damage validation, frame-versus-buffer
    retirement, rejection and safe display-loss teardown before implementation.
  - **Verification**: Preserve allocation FD ownership and prohibit freeing active
    listener cookies or CPU fallback; window roles and worker handoff stay separate.

- **Commit `8a065bd`**: `feat(vgpu): own asynchronous Wayland DMA-BUF presentation`
  - **Task Impact**: +30% to TODO: #4 (+4.5% overall): 25% presenter ownership
    and 5% native image-validation CI; +5% to TODO: #3 (+1.25% overall) for
    integrated/native guest frontend acceptance.
  - **Summary**: Add version-four surface feedback snapshots, real received-FD
    map/close cleanup, supported-pair gating, asynchronous plane import, bounded
    damage, independent frame pacing/buffer release and safe display-loss teardown.
  - **Verification**: Linux unit/fault/churn tests, actual generated Wayland
    protocol compilation and ASan/LSan/UBSan pass. Presenter production lines
    99.07%, branches 94.23%; image/damage Zig lines/branches 100%.
    Tests cover acquired descriptor/proxy faults, import rejection, three owned
    buffers, both callback orders, stale frames, teardown refusal and abandonment.
    Guest/image codec Linux and native Windows CI passed at 4ca8195:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37402655882 .
    Hardware allocation/export, worker FD handoff and full ICD/DXVK remain pending.

- **Commit `d60a8ab`**: `docs(vgpu): define standalone render-only adapter lifetimes`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify userland render/compute/no-scanout adapter identity,
    bounded negotiated context bindings, nonreused handles and fence-ordered
    registered-allocation teardown; distinguish this stub from kernel installation.
  - **Verification**: Preserve host quota/codec authority and explicit lost-context
    discard without remote requests or undocumented ownership transfer.

- **Commit `b7e7a48`**: `feat(vgpu): implement standalone render-only adapter ownership`
  - **Task Impact**: +20% to TODO: #3 (+5.0% overall); native DLL acceptance 5% pending.
  - **Summary**: Add no-scanout render/compute adapter capabilities, eight distinct
    negotiated frontend bindings, monotonic context/allocation handles, 64-resource
    ledgers, GPU-fence-ordered free and explicit lost-context discard. Build a
    userland Windows DLL exporting exactly eight snake_case stub ABI symbols.
  - **Verification**: Linux faults/churn, ASan/LSan/UBSan and Windows x86_64 test/DLL
    cross-link pass. Production C lines 100%, branches 98.15%; PE export table
    inspected. Native DLL fixture loads exports, calls init/query/free and verifies
    absence of kernel DriverEntry/internal frontend exports; CI acceptance pending.
    This implements the user-authorized stub scope, not an installed WDDM driver.

- **Commit `1c42a82`**: `docs(vgpu): specify credential-bound presentation FD handoff`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Define a separate trusted seqpacket/SCM_RIGHTS channel, exact
    bounded Zig frame schema, kernel credential/context checks, descriptor rollback
    and the separate worker/surface/release runtime integration requirement.
  - **Verification**: No host FD enters the guest ring; borrowed socket/native
    identity lifetime and every malformed-message cleanup obligation are explicit.

- **Commit `0d8d0b6`**: `feat(vgpu): transfer presentation frames with credential-bound FDs`
  - **Task Impact**: +10% to TODO: #4 (+1.5% overall) for bounded codec/native FD
    ownership; +5% to TODO: #3 (+1.25% overall) for native adapter/DLL acceptance.
  - **Summary**: Add exact Zig image/damage frame codec and separate nonblocking
    seqpacket SCM_RIGHTS/SCM_CREDENTIALS bridge with sender/context/count checks,
    CLOEXEC receipt and full acquired-FD rollback on corruption/truncation.
  - **Verification**: Real Linux same/process-separated packets, duplicate-plane
    FDs, all surplus/truncated FD counts, wrong PID/UID/context, malformed payloads,
    syscall faults and 128 churn cycles pass with descriptor baseline and
    ASan/LSan/UBSan. Native C coverage 96.77% lines/90.74% branches; Zig frame
    100%/100%; Windows codec cross-link passes. Worker/service routing remains pending.
    Native adapter tests and actual restricted-export DLL loading passed in both
    Linux/Windows CI at b7e7a48:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37403866555 .
    Wayland presenter CI passed at 8a065bd:
    https://github.com/Kayyo321/Waddle-LSW/actions/runs/37403394245 .

- **Commit `c22c0fe`**: `test(vgpu): verify native Wayland frame descriptor integration`
  - **Task Impact**: +10% to TODO: #4 (+1.5% overall).
  - **Summary**: Exercise production frame channel and presenter across actual
    libwayland client/server sockets, real FD transfer and generated protocol.
  - **Verification**: Sixteen frames verify backing inode identity, partial damage,
    frame callbacks, release ownership, rejected import and subsequent recovery;
    normal and ASan/LSan/UBSan runs pass. Synthetic allocation remains explicitly
    mock-only; hardware Venus image/export and worker routing remain pending.

- **Commit `e897295`**: `chore(deps): pin exact guest Venus serializer generator`
  - **Task Impact**: +100% to TODO: #7 (+0% overall); +0% to TODO: #3.
  - **Summary**: Add MIT generator/Apache-2.0 Vulkan declarations as a public HTTPS
    submodule pinned to 7157163d5fed65f0742542a8e77e5412dea6a4bc; generate offline.
  - **Verification**: Reproduce all thirty-nine renderer headers and three Vulkan
    declaration headers byte-for-byte; generate thirty-seven driver headers and
    compile their complete declarations on Linux and Windows x86_64. This verifies
    schema/dependency configuration; ICD dispatch and serialization remain pending.

- **Commit `b0a481c`**: `feat(vgpu): bind worker frame ownership to native Wayland surfaces`
  - **Task Impact**: +5% to TODO: #4 (+0.75% overall).
  - **Summary**: Add credential/context-bound surface owner with pending FD retry,
    monotonic frame identities, three compositor-owned metadata ledgers, explicit
    unsubmitted cancellation and release callbacks retaining resource identities.
  - **Verification**: Production C lines/branches 100%; acquisition faults, retained
    backpressure FDs, three slots, stale callbacks/frames, sticky loss and 128 churn
    cycles pass ASan/LSan/UBSan. Actual native Wayland wire fixture now routes through
    this owner and passes normal/sanitized runs. Worker export service and guest
    release acknowledgement delivery remain pending, as does hardware image export.

- **Commit `44f05d8`**: `test(vgpu): present rendered Venus hardware images through native Wayland`
  - **Task Impact**: +10% to TODO: #4 (+1.5% overall).
  - **Summary**: Allocate/bind linear external-memory BGRA8 image through bounded
    Venus packets, clear on the real GPU, release external ownership, retire fence,
    export DMA-BUF and retain allocation through native mock compositor releases.
  - **Verification**: RTX 5080 normal and ASan/LSan/UBSan clear/export/protocol runs
    pass (offset0, pitch128, image2048/allocation4096 bytes), fifteen released frames
    plus one rejected import; no pixel mapping/copy. Existing GPU allocator/unit,
    real queue and native mock normal/sanitized regressions pass. CI compiles the
    hardware fixture; physical compositor scanout/pixel readback is not claimed.
    Production worker export and guest release acknowledgement remain pending.

- **Commit `beba08e`**: `fix(vgpu): reject zero surface completion identities`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Keep empty accepted-ledger entries from matching zero callback IDs.
  - **Verification**: Zero and unmatched nonzero callbacks prohibit further receives
    without reporting a completion; production line/branch coverage remains 100%,
    ASan/LSan/UBSan and native hardware/Wayland fixture pass.


- **Commit `78176a5`**: `docs(vgpu): specify authenticated presentation release packets`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Specify exact reverse-channel release schema, allowed completion
    statuses, sender validation, descriptor rollback and bounded retry ownership.
  - **Verification**: Cross-checked surface callback outcomes and native frame
    credential contract; worker leases, acknowledgement retry and guest polls remain
    separate integration gates, with no runtime completion credit.


- **Commit `b0e1b66`**: `feat(vgpu): encode bounded presentation release records`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add documented C ABI and allocation-free Zig release codec with
    exact little-endian size, explicit completion statuses and zeroed error output.
  - **Verification**: Five Zig allocator tests, Linux frame/native sanitizer tests,
    Windows x86_64 cross-link and coverage pass. Frame codec production lines100%,
    branches95.59%; native frame transport96.77%/90.74%. Native Windows CI retains
    the ABI roundtrip/corruption fixture; release socket routing remains pending.


- **Commit `7b06672`**: `feat(vgpu): authenticate native presentation release delivery`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add nonblocking reverse-channel release send/receive with exact
    controller PID/UID/context checks and closure of every unexpected received FD.
  - **Verification**: Real fork credentials, kernel send-buffer exhaustion/retry,
    all completion statuses, 128 churn cycles, descriptor baselines, malformed
    credentials/payloads and truncated/unexpected FD batches pass normal and
    ASan/LSan/UBSan runs. Native transport coverage exceeds90% lines/branches;
    Zig codecs100%/95.59%. Worker leases and guest polling remain pending.


- **Commit `4fea0af`**: `docs(vgpu): define bounded worker presentation leases`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Specify three receiver-backed leases, ordered frame publication,
    fence-bound plane export rollback, retained resource busy checks and exact-once
    guest completion consumption before implementing worker ownership.
  - **Verification**: Native release transport coverage97.92% lines/91.07% branches;
    runtime routing and controller acknowledgement retry remain pending.


- **Commit `5c4d29c`**: `feat(vgpu): retain bounded worker presentation allocation leases`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add caller-owned three-slot fence-bound export owner with actual
    DMA-BUF extent checks, ordered successful publication, resource busy guards,
    authenticated completion pumping and exact-once guest completion consumption.
  - **Verification**: Production C lines/branches100%; plane export rollback,
    native send faults, allocation-size mismatch/query faults, full/busy leases,
    duplicate/out-of-order acknowledgements and128 churn cycles pass normal and
    ASan/LSan/UBSan runs with descriptor baselines. Add Linux CI gates. Service
    envelopes, worker launch wiring and controller acknowledgement retry pending.


- **Commit `80d3100`**: `docs(vgpu): specify guest presentation request envelopes`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Specify operations12/13, exact frame/release payloads, explicit GPU
    ordering and publication/retry/consumption semantics without native FD exposure.
  - **Verification**: Cross-checked existing negotiation/RPC field/status contracts;
    envelope codec, service binding and controller retries remain pending.


- **Commit `d2aa43f`**: `feat(vgpu): validate presentation request envelope operations`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add bounded operations12/13 with exact frame/release payload,
    GPU timeline/fence and completion identity rules. Explicitly reject both in
    unbound host dispatch, preventing accidental fallthrough to CPU polling.
  - **Verification**: Linux/Zig allocator and C ABI tests, request/runtime sanitizer
    suites and Windows x86_64 cross-links pass. Request coverage98.46% lines/97.80%
    branches; RPC/dispatch coverage exceeds90%. Unbound pre/post-negotiation frames
    consume payload and return Invalid with no receiver calls. Bound routing pending.


- **Commit `ae2e60d`**: `docs(vgpu): specify optional bound presentation dispatch`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Define trusted sole-thread submit/take/busy/pump callbacks, resource
    free guards, portable unbound behavior and bounded health-monitor integration.
  - **Verification**: Request and unbound RPC tests/coverage/sanitizers pass; earlier
    Linux/native Windows CI passed at beba08e, run37406662469. Bound routing pending.


- **Commit `4b06374`**: `feat(vgpu): route bounded presentation requests through trusted dispatch`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Route negotiated frame/release requests through complete trusted
    bindings, guard resource frees, pump acknowledgements during receiver health
    checks and validate guest RPC release response capacity before publication.
  - **Verification**: Linux normal/ASan/LSan/UBSan runtime and Windows cross-link
    pass; dispatch coverage99.24% lines/95.65% branches, RPC exceeds90%. Test partial
    bindings, pre-negotiation rejection, all local/terminal outcomes, release bytes,
    busy guards and health-pump loss with cleared call-scoped monitor pointers.
    Production service/worker launch and controller acknowledgement retry pending.


- **Commit `d39082b`**: `test(vgpu): verify bound presentation negotiation enforcement`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Explicitly verify both bound operations before negotiation consume
    their full request and return Invalid without submit/take or receiver calls.
  - **Verification**: Normal and ASan/LSan/UBSan runtime suites, required dispatch/RPC
    coverage and native Windows cross-link pass; monitor pointers remain cleared.


- **Commit `d6707d5`**: `docs(vgpu): define production service presentation lifetime`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Specify optional borrowed frame channel binding, distinct descriptor
    checks, export owner acquisition and teardown order with existing service state.
  - **Verification**: Bound dispatch/native Windows cross-link and negotiation
    tests pass; service construction and worker launch wiring are next gates.


- **Commit `5030b20`**: `feat(vgpu): bind presentation leases into the production receiver service`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add optional trusted frame endpoint/controller/context service run,
    acquire caller-owned export leases, route bound callbacks and encode consumed
    completions through Zig; abandon leases with old receiver during teardown.
  - **Verification**: Service acquisition faults/callback outcomes/descriptor checks
    pass normal/ASan/LSan/UBSan; production coverage100% lines/99.02% branches.
    Real mapped/exec-isolated receiver and fresh restart regressions pass normal
    and sanitized production workers. Native frame launch/ack retry still pending.


- **Commit `faa8eae`**: `docs(vgpu): define presented worker exec binding`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Specify prepared fd5 isolation, three-source duplication rollback,
    fixed bounded argument encoding, retained parent identity and unbound compatibility.
  - **Verification**: Service lifetime tests and actual mapped/exec regressions pass;
    worker launcher and argument decoding implementation follows separately.


- **Commit `1edc2ba`**: `feat(vgpu): decode fixed worker context arguments safely`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add allocation-free bounded Zig fixed hexadecimal argument decoder
    with explicit C ABI, zero failure output and strict lowercase/nonzero policy.
  - **Verification**: Six Zig allocator tests, native frame transport sanitizers,
    Windows cross-link and frame codec coverage100% lines/95.12% branches pass.
    Tests cover every invalid character position, bounds, zero and maximum identity.


- **Commit `6aa5255`**: `feat(vgpu): launch isolated workers with trusted presentation channels`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add prepared fd5 presented launch, collision-safe three-source
    duplication/rollback, fixed context/controller arguments and production entry
    point parsing/parent validation before bound service acquisition.
  - **Verification**: Worker unit/real exec faults and ASan/LSan/UBSan pass; production
    coverage98.73% lines/95.24% branches. Actual production worker negotiates over
    mapped rings, rejects unregistered present/unknown polls without native frames,
    and terminates on authenticated unknown release; both normal/sanitized workers
    return descriptors to baseline. Existing mapped/exec restart regressions pass.
    Add Linux CI gates. Controller acknowledgement retry/render routing pending.


- **Commit `722962e`**: `docs(vgpu): define controller acknowledgement retry ownership`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Specify opt-in four-slot surface release queue, receive gating
    during send pressure, observer lifetime and retry-preserving teardown behavior.
  - **Verification**: Production presented worker launch/native FD tests pass;
    controller queue implementation and rendered-frame runtime validation pending.


- **Commit `eaf075a`**: `feat(vgpu): retain surface release acknowledgements under backpressure`
  - **Task Impact**: +0% to TODO: #4 (+0% overall).
  - **Summary**: Add opt-in acknowledged surface owner with four bounded release
    slots, receive gating during pressure, sticky send loss and retry-preserving
    teardown after presenter destruction; preserve explicit callback compatibility.
  - **Verification**: Production C lines/branches100%; partial flush, all completion
    statuses, queue overflow, native loss, optional observer and cancelled teardown
    retry pass ASan/LSan/UBSan. Actual native Wayland/mock and real RTX5080 image
    harness roundtrip exact acknowledgements after every release/rejection in normal
    and sanitized runs. Successful rendered production worker routing still pending.


- **Commit `6a5149d`**: `docs(vgpu): define mapped guest hardware presentation verification`
  - **Task Impact**: +0% to TODO: #4; +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify explicit fixture transport backend, guest-only image metadata,
    production worker/native FD/Wayland roundtrip and exact completion-before-reuse
    validation. Keep standalone ICD and DXVK acceptance separate.
  - **Verification**: Acknowledged native Wayland/RTX5080 path passes; mapped rendered
    production worker fixture and final runtime acceptance remain pending.


- **Commit `8984e9d`**: `test(vgpu): add explicit guest transport backend to the GPU fixture`
  - **Task Impact**: +0% to TODO: #4; +0% to TODO: #3 (+0% overall).
  - **Summary**: Add private receiver/negotiated guest backends with exact request
    fields, shared image preparation/teardown and metadata-only remote callback.
    Format fixture signatures/conditions/calls within100 columns using zig fmt.
  - **Verification**: Three Zig allocator tests cover all remote operation fields,
    malformed replies, status propagation and argument failures. Existing local
    timestamp and image native/hardware paths pass normal and ASan/LSan/UBSan.
    Successful mapped remote image/Wayland fixture remains pending; no ICD credit.


- **Commit `59d31bd`**: `test(vgpu): verify mapped guest production worker hardware presentation`
  - **Task Impact**: +0% to TODO: #4; +0% to TODO: #3 (+0% overall).
  - **Summary**: Route real guest Vulkan image creation through an exec-isolated
    production worker, native DMA-BUF export, acknowledged Wayland surface and
    exact-once guest release polling. Add fresh-context descriptor-baseline churn.
  - **Verification**: Three contexts and48 frames per normal/sanitized execution
    pass on RTX5080:45 successful releases and3 import rejections, wrong-context
    and inflated-extent rejection, resource busy before completion consumption,
    duplicate completion rejection, subsequent reuse and healthy worker teardown.
    Existing native/local hardware regressions pass ASan/LSan/UBSan. CI compilation
    and native ABI gates remain pending before final TODO #4 acceptance credit.


- **Commit `b9dc18e`**: `docs(vgpu): specify serialized guest command reply ownership`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify allocation-free exclusive command/reply staging, fixed
    pinned reply-stream prefix, CPU completion state and bounded reply identity.
  - **Verification**: Cross-checked pinned SetReplyCommandStreamMESA encoding and
    production Submit/Poll/Reply semantics. No loader or DXVK credit assigned.


- **Commit `2fba038`**: `docs(vgpu): accept verified production presentation milestone`
  - **Task Impact**: +5% to TODO: #4 (+0.75% overall); TODO #4 now100%.
  - **Summary**: Accept final guest/production-worker/native Wayland release gate
    after mapped hardware and sanitized fresh-context runtime verification.
  - **Verification**: Linux and native Windows CI at59d31bd both passed, run
    37477413111. Hardware normal/sanitized runs each verified48 frames over3 fresh
    contexts with45 releases/3 import rejections and descriptor baselines. Existing
    protocol/memory ownership coverage gates remain above90%; no pixel copying.
    Physical compositor/VM acceptance remains downstream; standalone ICD/DXVK
    TODO #3 still35%, and unrequested OpenCL TODO #5 remains pending.


- **Commit `425bb0e`**: `feat(vgpu): own bounded serialized guest command replies`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Add documented allocation-free Zig command owner, exclusive
    submission/reply staging, pinned prefix, retained CPU fence and exact-once
    identity-validated reply views. Add Linux/native Windows ABI and CI gates.
  - **Verification**: Five Zig tests include128 cycles, all state boundaries,
    buffer overlap/overflow, callback statuses and malformed successful shapes.
    Native Linux C ABI, ASan/LSan/UBSan and Windows cross-link pass. Production
    lines100%, branches91.67%. Public Vulkan dispatch/DXVK remain pending.


- **Commit `b7c3025`**: `refactor(vgpu): keep command tests within the column limit`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Format six long test assertions with explicit trailing commas
    so zig fmt preserves the100-column repository limit.
  - **Verification**: All5 Zig command tests and line-length audit pass; behavior
    and production code are unchanged.


- **Commit `960aa49`**: `test(vgpu): query real Vulkan versions through command ownership`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Bind the command owner to a negotiated frontend in the actual
    exec-isolated production worker fixture; query pinned Vulkan instance version
    eight times before existing presentation isolation/loss checks.
  - **Verification**: Normal and ASan/LSan/UBSan production worker/client execution
    pass with healthy/corrupt-release teardown and descriptor baselines. Command
    ownership coverage remains100% lines/91.67% branches. Loader/DXVK pending.


- **Commit `7872808`**: `docs(vgpu): record native command ownership verification`
  - **Task Impact**: +0% to TODO: #3; +0% to TODO: #4 (+0% overall).
  - **Summary**: Record successful native Linux/Windows command owner verification
    without crediting unfinished public ICD dispatch, object/memory lifetimes or DXVK.
  - **Verification**: Both CI jobs passed at960aa49, run37479173379. Linux gates
    include real production-worker queries, sanitizer and coverage checks; native
    Windows executes C ABI and Zig state tests. TODO #4 remains100%; TODO #3 remains
    35% and the feature remains In Progress with Time Ended TBD.


- **Commit `2adb81b`**: `docs(vgpu): specify Vulkan object identity and lifetime storage`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Subdivide remaining runtime milestones and specify loader headers,
    dispatchable address validation, namespaced nondispatchable identities, parent
    lifetime rules and bounded allocation-free storage before implementation.
  - **Verification**: Cross-check Vulkan loader header ABI and existing per-worker
    object ID isolation; no new runtime or DXVK credit claimed by this design.


- **Commit `ae50e2d`**: `feat(vgpu): retain bounded guest Vulkan object identities`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; native CI acceptance pending).
  - **Summary**: Add documented caller-owned Zig registry with loader-first-word
    layout, monotonic host IDs, namespaced application tokens, bounded dispatchable
    address lookup, exact kinds and parent/child retirement. Add native ABI/CI gates.
  - **Verification**: Four Zig tests include4096 allocator-backed lifecycle cycles,
    boundaries, namespaces, counter exhaustion and private metadata fault injection.
    Native Linux loader header helpers, C ABI, ASan/LSan/UBSan and Windows cross-link
    pass. Production lines100%, branches98.75%; exclude test-only helper assertions
    from coverage. Complete ICD/device dispatch and DXVK still pending.


- **Commit `bd311f6`**: `docs(vgpu): accept native Vulkan object ownership milestone`
  - **Task Impact**: +5% to TODO: #3 (+1.25% overall); TODO #3 now40%.
  - **Summary**: Accept the documented identity/lifetime storage milestone after
    local allocator/sanitizer/coverage verification and native Linux/Windows CI.
  - **Verification**: Both jobs atae50e2d passed, run37483582807. Linux verifies
    actual loader-word helpers, allocator churn, sanitizer and98.75% branch/100%
    line coverage; Windows executes native ABI and Zig state tests. This accepts
    storage only; public ICD instance/device/API dispatch and DXVK remain pending.


- **Commit `4699df2`**: `docs(vgpu): specify bounded physical-device reply decoding`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify exact core property/feature/memory reply layouts, fixed
    array tags, scalar padding, semantic validation and preserved output behavior.
  - **Verification**: Cross-check pinned command IDs and generated field/array
    encodings; public ICD instance/device dispatch and DXVK remain independent gates.


- **Commit `d4c7059`**: `chore(vgpu): canonicalize generated protocol line endings`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Normalize generated headers to LF before immutable receiver
    comparison so Windows text-mode output retains the exact pinned schema.
  - **Verification**: Offline generation matches all39 receiver headers and37
    guest headers. Pinned source files are unchanged; no runtime progress credited.


- **Commit `b88853f`**: `feat(vgpu): decode bounded native Vulkan query values`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Add allocation-free Zig native property/feature/memory conversion
    with exact tags, bounded reads, semantic checks and preserved failure output.
    Compare independent pinned C encoder output and gate Linux/Windows ABI tests.
    Correct design IDs to pinned properties6/features3/memory8.
  - **Verification**: Four allocator tests, native C, ASan/LSan/UBSan and Windows
    cross-link pass; production lines100%, branches90%. Public ICD/API dispatch
    and DXVK remain unfinished; no completion credit from decoder helpers alone.


- **Commit `b2d6354`**: `test(vgpu): validate complete core replies from real receivers`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Replace partial property parsing with production fixed-value
    decoders; also query complete features and memory metadata before GPU work.
    Encode exact fixed memory array tags required by the receiver input schema.
  - **Verification**: Mesa llvmpipe normal/sanitized queue workloads pass; RTX5080
    queue and mapped guest/production-worker48-frame fresh-session tests pass,
    including ASan/LSan/UBSan. This is actual receiver verification, not ICD/DXVK.


- **Commit `add3a30`**: `docs(vgpu): specify core instance dispatch wire boundary`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify production instance creation/destruction serialization,
    bounded name input, exact transaction replies, reservation rollback and
    unsupported initial core inputs before ICD lifecycle integration.
  - **Verification**: Cross-check command IDs and pinned guest encoding, including
    output host IDs and nullable application info. Full ICD/DXVK remains pending.


- **Commit `634e76c`**: `feat(vgpu): serialize bounded core instance transactions`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Add production core create/destroy packets and exact creation
    reply identity/result checks with preserved failure output. Compare independent
    pinned guest serializer; add portable native ABI/sanitizer/coverage CI gates.
  - **Verification**: Five allocator tests, native C and ASan/LSan/UBSan pass;
    Windows cross-link passes. Production lines100%, branches95.59%. Prior fixed
    query changes atb2d6354 pass Linux and native Windows CI run37488148604.
    Public loader/device/API dispatch and DXVK remain pending with no helper credit.


- **Commit `0dea65c`**: `test(vgpu): retire guest instance reservations through production worker`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Integrate registry reservation, production instance packet,
    exclusive command owner and negotiated frontend through two fresh worker
    sessions; validate host ID/result and destroy reply before local retirement.
  - **Verification**: Normal and ASan/LSan/UBSan worker/client fixtures pass;
    namespaces never reuse and success ends with zero live registry objects.
    Error paths abandon transport before clearing borrowed storage. Public
    ICD/device/API dispatch and real DXVK still pending; TODO #3 remains40%.


- **Commit `d8657a1`**: `docs(vgpu): record native core wire verification`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Record Linux and native Windows acceptance of bounded core query
    and instance wire components; synchronize remaining ICD/device/DXVK scope and
    complete the test encoder length hook's structured ownership documentation.
  - **Verification**: Both jobs at0dea65c pass, run37490205909. Linux runs native
    ABI, allocator, sanitizer, coverage and actual production-worker instance
    retirement; Windows executes native ABI and all five instance wire Zig tests.
    TODO #3 remains40%, overall65.0%, In Progress, Time Ended TBD. No public ICD
    loader/device/API implementation or DXVK acceptance is claimed by these tests.


- **Commit `92eb676`**: `docs(vgpu): specify physical enumeration dispatch transactions`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Define bounded count/fill physical-device enumeration and fixed
    query request serialization, reserved identities and reply preservation before
    connecting application-facing dispatch. Full ICD/device/DXVK remains required.
  - **Verification**: Cross-check pinned generated guest/renderer schemas, command
    IDs, partial fixed arrays and output handle preallocation; implementation pending.


- **Commit `26aee9b`**: `feat(vgpu): encode bounded physical enumeration transactions`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Implement count/fill request encoding, reserved identity reply
    validation and fixed-query partial packets in allocation-free Zig; compare
    independent pinned C generator and add native Linux/Windows CI gates.
  - **Verification**: All3 allocator tests and native C/ASan/LSan/UBSan pass;
    production coverage100% lines/90% branches; Windows fixtures cross-link.
    Full ICD/device/API/DXVK acceptance remains required; no helper milestone credit.


- **Commit `f757c40`**: `docs(vgpu): specify standalone ICD instance lifecycle`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Define loader export aliases, serialized borrowed frontend,
    registry-backed instance/physical dispatch, count/fill caching and sticky loss.
  - **Verification**: Reviewed lifecycle against existing command, object and wire
    contracts; public device/API/DXVK acceptance remains pending.


- **Commit `a4bbf75`**: `refactor(vgpu): format physical query wire fixtures`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Expand long Zig signatures/calls and fixture records with explicit
    trailing commas to preserve the100-column limit under zig fmt.
  - **Verification**: Query request/oracle and allocator tests pass; prior behavior
    and production coverage remain unchanged. No runtime milestone credit.


- **Commit `58ad94d`**: `feat(vgpu): dispatch standalone ICD instance lifecycles`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; native CI acceptance pending).
  - **Summary**: Add mutex-serialized allocation-free experimental ICD, exact
    loader export aliases, borrowed negotiated backend, registry-backed instance/
    physical dispatch and complete core physical query conversion. Build bounded
    test manifest/shared library and Windows DLL with exactly9 public exports.
  - **Verification**: Native C fixtures exercise128 cycles plus128 concurrent
    cycles, rollback, cached identities, corrupt replies and sticky timeout/loss.
    System Vulkan loader performs8 manifest-discovered instance lifecycles.
    ASan/LSan/UBSan pass; Zig production lines99.18%, branches91.37%; native
    Windows tests/DLL cross-link. Device creation explicitly returns unsupported;
    complete device/memory/command/synchronization APIs and DXVK remain required.


- **Commit `e0d4be9`**: `test(vgpu): exercise public ICD through production workers`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; native CI acceptance pending).
  - **Summary**: Bind public ICD instance/procedure/query functions to the actual
    negotiated production worker in each of2 fresh mapped sessions; execute8
    instance/create/enumerate/property/feature/memory/destroy cycles per session.
  - **Verification**: Normal and ASan/LSan/UBSan worker/client paths pass; success
    retires all cached physical and instance reservations before unbind, failure
    abandons the guest before clearing ICD objects. Descriptor baseline checks
    remain enforced. Device/API and real DXVK runtime acceptance remains pending.


- **Commit `53158a7`**: `fix(vgpu): link shared query decoder once in remote image fixture`
  - **Task Impact**: +0% to TODO: #3; +0% to TODO: #4 (+0% overall).
  - **Summary**: Remove duplicate value decoder object after public ICD worker
    integration made it part of the shared guest object list. Restore clean
    remote image fixture linking without changing wire/runtime behavior.
  - **Verification**: GitHub native Windows ICD dispatch/DLL tests passed in both
    push37496066320 and PR37496073296 runs. Linux ICD safety/coverage and real
    worker tests passed; remote image link exposed the duplicate object. Local
    image/remote consumers now compile, actual RTX5080 normal/sanitized48-frame
    fresh-context presentation runs pass, and loader/worker regressions pass.
    Full updated-head GitHub CI remains required.

- **Commit `6d2f73e`**: `docs(vgpu): specify bounded device and queue dispatch`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify device creation input bounds, parent ownership, stable queue
    identity, exact pinned command11/12/17/19/20 semantics, error rollback, teardown
    and verification before implementation. Full dispatch and DXVK remain pending.

- **Commit `a086ca8`**: `fix(test): recognize compiler stack canary coverage guards`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Exclude branches whose compiler-generated destination calls
    __stack_chk_fail, matching the existing exclusion for compiler panic guards.
    Keep stack protection enabled; no source bounds/error branch is excluded.
  - **Verification**: Inspect generated LLVM destinations, run three ICD Zig/native
    tests; 99.49% production lines and93.29% branches with the device increment.
    Commit53158a7 has all eight GitHub checks green. New device code remains under
    local verification and is not yet credited toward full dispatch acceptance.

- **Commit `449f7e4`**: `feat(vgpu): dispatch bounded device and queue lifecycles`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; complete runtime gate pending).
  - **Summary**: Add bounded core device creation/destruction and stable cached queue
    dispatch. Validate native queue counts/priorities/features, strip loader-only
    device chain records, reserve identities and release negative creation results.
    Enforce parent teardown order,16 device caches and63 exclusive queue ring indices.
    Initialize queues through pinned command155 and required Venus timeline info;
    never submit forbidden legacy queue/idle commands. Idle/fence APIs remain pending.
  - **Verification**: Pinned C encoder compares device/queue/destruction packets;
    native128-cycle churn,16-device/63-ring limits, invalid input and transport/reply
    failure cases pass. Three Zig tests pass, production coverage99.50% lines and
    93.29% branches. ASan/LSan/UBSan local/native and actual two-worker-session device
    create/queue/cache/destroy integration pass; Windows DLL/tests cross-link.
    Raise integration deadline20s to60s for16 actual host device lifecycles. Native
    Windows/updated-head GitHub checks remain required. TODO #3 stays40%; complete
    graphics/compute/memory/synchronization dispatch and actual DXVK remain pending.

- **Commit `79325dc`**: `docs(vgpu): specify receiver GPU fence idle waits`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify queue/device idle through explicit issued and retired GPU
    identities, exact envelope shapes and a monotonic whole-call deadline. Preserve
    sole frontend ownership and session retirement on any terminal failure.

- **Commit `e41eda8`**: `fix(ci): include pinned ICD encoder oracle on Windows`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Add the encoder shim/generated driver/test hook include directories
    to the native Windows ICD fixture's direct compiler invocation. Make targets
    already include them; preserve the workflow's compiler warnings/error policy.
  - **Verification**: Exact same directories pass local Windows cross-link; native
    job112393470812 identified missing vn_cs.h before executing the fixture.
    Updated-head native Windows verification remains required.

- **Commit `4620bf9`**: `feat(vgpu): wait for explicit GPU retirement in ICD idle calls`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; full synchronization gate pending).
  - **Summary**: Resolve queue/device idle through negotiated GPU fence issue/poll
    envelopes; validate successful shapes and strictly increasing per-ring IDs.
    Bound the entire call with a monotonic timer, retain sticky loss ownership on
    deadline/transport/peer failure, and serialize the borrowed frontend.
  - **Verification**: Pending issue/retirement, malformed flags/poll argument, zero
    and replayed identities, transport failure and both timeout phases pass under
    native ASan/LSan/UBSan and three Zig tests. Production lines99.54%, branches
    92.53%; Windows tests/DLL cross-link. Actual two-session worker queue/device
    idle integration passes normal and sanitizers with explicit GPU retirement.
    Native Windows and updated-head GitHub CI remain required. TODO #3 remains40%;
    complete graphics/compute/memory APIs, public synchronization objects and real
    DXVK execution are pending.

- **Commit `e4df455`**: `docs(vgpu): specify public fence ownership and polling`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify bounded fence creation/destruction/reset/status/wait wire
    commands, device-parent validation, nonblocking receiver polls, caller timeout
    semantics and mutex release so another thread can signal pending fences.

- **Commit `da388de`**: `fix(test): record concurrent branch hits atomically`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Use relaxed C11 atomic bytes in the fixed branch recorder so the
    four-thread native ICD fixture can record source edges without data races.
    Preserve bounds/abort behavior and the joined-thread exit report format.
  - **Verification**: Native thread fixtures and coverage gates remain enforced;
    no production runtime, threshold or ownership policy changes.

- **Commit `27c93da`**: `feat(vgpu): dispatch core fence objects and caller timed waits`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; full synchronization gate pending).
  - **Summary**: Add device-parented nondispatchable fence create/destroy/reset/status
    and any/all waits. Validate native counts/flags/parents before serialization;
    poll host waits with timeout0 and release the binding mutex between caller-timed
    rounds. Refuse device retirement while nonqueue children remain.
  - **Verification**: Independent pinned C encoders compare commands35..39. Native
    signaled/unsignaled/reset/any/all/zero/finite/delayed waits, stale/foreign handles,
    508-fence registry exhaustion, negative creation rollback and every command's
    corrupt/transport path pass ASan/LSan/UBSan. Four Zig tests pass, including every
    result-reply truncation; atomic coverage recorder reports99.63% lines/93.29%
    branches. Actual two-worker-session fence lifecycle and GPU idle cycles pass
    normal and sanitizers; Windows test/DLL cross-link succeeds. Updated-head native
    CI remains required. Prior4620bf9 vGPU Linux and Windows checks are green; CLI
    pending. TODO #3 remains40%; full API dispatch and real DXVK are still pending.

- **Commit `beb49d3`**: `docs(vgpu): specify pinned native loader acceptance`
  - **Task Impact**: +0% to TODO: #3; +0% to TODO: #8 (+0% overall).
  - **Summary**: Specify the exact public Vulkan-Loader v1.4.307 SHA, Apache-2.0/GPL-3.0
    compatibility, test-only offline build boundary using existing pinned headers,
    private DLL/manifest lifetime and real loader device/fence verification gates.

- **Commit `5135652`**: `chore(deps): pin native Vulkan loader to v1.4.307`
  - **Task Impact**: +30% to TODO: #8; +0% to TODO: #3 (+0% overall).
  - **Summary**: Add public HTTPS KhronosGroup/Vulkan-Loader under submodules/vulkan_loader
    at0508dee4ff864f5034ae6b7f68d34cb2822b827d, with shallow clone preference.
    Audit preserved Apache-2.0/more-permissive notices against repository GPL-3.0;
    header307 matches the existing pinned protocol SDK.
  - **Verification**: Public tag/commit resolves upstream, exact detached checkout
    is clean; no loose vendor files, source changes or runtime substitutions.
    Offline build and actual native loader acceptance remain pending.

- **Commit `2326afc`**: `test(vgpu): verify native loader device and fence dispatch`
  - **Task Impact**: +35% to TODO: #8; +0% to TODO: #3 (+0% overall).
  - **Summary**: Build the pinned loader offline with the existing exact header307
    target, no downloads/codegen/install or vendor edits. Extend dynamic loader
    fixture to eight instance/device/queue/idle/fence cycles, portable owned library
    handles/environment snapshots, private Windows manifest and medium-integrity
    test process behavior that preserves upstream elevated-search protections.
    Gate Linux pinned loader safety and native Windows DLL discovery in CI.
  - **Verification**: System/pinned Linux loader device discovery passes; build an
    ASan-enabled pinned loader and run fixture with ASan/LSan/UBSan, zero leaks.
    Native Windows fixture cross-links with strict warnings and explicit Advapi32.
    Workflow parses. The native oracle backend remains explicitly fake; production
    worker runtime remains separately verified. Native Windows/updated-head CI
    required before completing dependency task or loader ABI milestone credit.

- **Commit `623e1a5`**: `docs(vgpu): specify experimental API version consistency`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Align public physical properties/instance requests with declared
    experimental API1.0, keep host query data private until validated, and preserve
    the separate complete runtime/DXVK acceptance requirements.

- **Commit `4e61936`**: `fix(vgpu): enforce experimental public API version ceiling`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Reject unsupported instance API requests before object reservation
    or host submission. Decode physical properties privately, reject invalid host
    API variants/majors without changing caller storage, and cap successful public
    properties at declared API1.0 while preserving all other host fields.
  - **Verification**: Native default/patch/unsupported API requests, unchanged
    outputs on invalid host versions, 128 healthy cycles and independent newer-host
    encoder pass. ASan/LSan/UBSan reports no leaks; Zig tests pass; production
    coverage99.63% lines/93.35% branches. Updated native loader/Windows CI required.

- **Commit `9fb1558`**: `docs(vgpu): specify core buffer ownership and requirements`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify private buffer identity, bounded sharing inputs, exact
    create/destroy/query wire fields, staged host requirements and failure
    ownership. Full memory execution and DXVK acceptance remain required.

- **Commit `40fa600`**: `feat(vgpu): dispatch core buffers and host memory requirements`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; full memory execution pending).
  - **Summary**: Add private device-parented buffers, bounded exclusive/concurrent
    sharing, exact creation identity and host destruction, staged validated actual
    memory requirements. Preserve uncertain ownership on transport/peer loss.
  - **Verification**: Independent pinned encoders compare commands30/50/51;
    invalid tags/flags/usage/sharing/families, native creation rollback, stale and
    foreign parents,507-buffer registry exhaustion, device-before-child refusal,
    malformed requirements and every command corruption/transport path pass.
    ASan/LSan/UBSan reports no leaks; production coverage99.68% lines/93.61%
    branches. Actual two-session worker create/query/destroy cycles pass normal
    and sanitizers; native Windows fixtures/DLL cross-link and pinned Linux loader
    pass. Native updated-head CI and full runtime/DXVK remain required.

- **Commit `9914c38`**: `docs(vgpu): record native loader dependency acceptance`
  - **Task Impact**: +35% to TODO: #8; +0% to TODO: #3 (+0% overall).
  - **Summary**: Complete loader dependency build/runtime acceptance with the exact
    pinned offline loader and existing headers. Preserve pending full ICD/runtime
    and DXVK gates; this zero-weight dependency task does not claim them complete.
  - **Verification**:2326afc GitHub native Windows job112407789435 passes all
    dispatch/DLL/manifest tests. Linux jobs112407789703 and112407762126 both pass
    pinned loader discovery and its ASan/LSan/UBSan fixture. Local Linux loader
    eight device/queue/fence cycles and strict Windows cross-link pass. Full latest
    branch CI remains required independently of this dependency acceptance.

- **Commit `70b62cd`**: `docs(vgpu): specify device memory allocation and buffer binding`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify private memory ownership, per-slot generation metadata,
    exact allocation/free/bind wire, actual cached requirements and overflow-safe
    range validation, relationship teardown and independent/real-worker gates.

- **Commit `18350fb`**: `feat(vgpu): allocate private memory and bind core buffers`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; mapping/commands/DXVK pending).
  - **Summary**: Add exact device-parented memory allocation/free, fixed per-slot
    generation metadata, cached actual buffer requirements, type/alignment/range
    checks using subtraction, and relationship publication only after host success.
    Retire bound relationships after validated buffer destruction; refuse premature
    memory free and device destruction while children remain.
  - **Verification**: Pinned independent encoders compare commands21/22/28;
    native allocation rollback, unsupported tags/type bounds, offset overflow,
    type mismatch, wrong parents, rebinding,505-allocation exhaustion, cached query
    failure and every command's malformed/transport result path pass. ASan/LSan/
    UBSan reports zero leaks; production coverage99.58% lines/93.83% branches.
    Actual two-session worker allocation/binding/GPU idle/destruction passes normal
    and sanitizers; pinned Linux loader and sanitized loader execute the same public
    API with their independent oracle backend. Native Windows fixture/DLL cross-link
    succeeds. Updated-head native CI remains required; TODO #3 remains40%.

- **Commit `1123f95`**: `docs(vgpu): specify native loader production worker proof`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify real loader/shared ICD dispatch through the actual
    negotiated worker, explicit borrowed callback and owned library/environment
    lifetimes, two-session object cycles and independent loader milestone gates.

- **Commit `11ca7e7`**: `test(vgpu): drive production workers through the native loader`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; updated native CI required).
  - **Summary**: Add actual pinned loader/shared ICD production worker variant,
    owned library/environment lifetimes, original host driver environment before
    guest manifest selection, two-session object cycles and receiver-retired
    failure cleanup. Capture destroy callbacks to retire loader CPU dispatch tables;
    never query stale loader handles. Gate actual normal/sanitized worker dispatch
    and injected live-allocation failure in Linux CI with a20-minute job budget.
  - **Verification**: Local real loader/receiver sixteen device/queue/fence/buffer/
    memory lifecycle cycles pass normal and ASan/LSan/UBSan. Forced failure after
    binding retires receiver and loader dispatch state with zero leaks and restored
    descriptor baseline; expected exit1 confirmed. Static worker normal/sanitized
    regressions pass; workflow YAML parses. Native Windows18350fb loader dispatch
    and DLL ABI are green. Latest native Linux integration CI remains required
    before the loader10% milestone; full command/mapping/DXVK remains incomplete.

- **Commit `45c8494`**: `docs(vgpu): specify core command pool ownership`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify bounded pool flags/family validation, exact host identity,
    private device ownership, reset results and child retirement boundary with
    independent encoder and actual worker/loader verification gates.

- **Commit `8061962`**: `feat(vgpu): dispatch core command pool lifecycles`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; command-buffer execution pending).
  - **Summary**: Add device-parented pool tokens, core flags/configured-family
    validation, exact host creation/destruction/reset, fixed pool metadata and
    implicit command-buffer child retirement after validated host destruction.
  - **Verification**: Pinned independent encoders compare commands85..87;
    all core flag combinations, unknown families/flags/tags/chains, native negative
    creation rollback,507-pool exhaustion, foreign/stale handles and device-child
    ordering pass. Every command's transport/malformed paths and reset negative/
    unexpected-positive/device-loss results pass ASan/LSan/UBSan, no leaks.
    Production coverage99.11% lines/93.70% branches; actual two-session worker
    create/reset/destroy cycles pass normal/sanitizers, Windows fixtures/DLL
    cross-link. Updated actual-loader local runs and native CI remain required.
    Full recording/submission, memory mapping and DXVK acceptance remain pending.

- **Commit `246be94`**: `docs(vgpu): specify command buffer allocation and recording states`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify transactional bounded batch allocation/free, loader-owned
    dispatch prefixes, primary/secondary inheritance, recording/reset transitions,
    pool child retirement and exact wire/error/verification boundaries.

- **Commit `c0a4a54`**: `docs(vgpu): clarify primary command buffer begin flags`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Record Khronos VUID02840 primary-only prohibition of combined
    one-time/simultaneous flags5, preserving the supported secondary combination.

- **Commit `8e5f540`**: `docs(vgpu): accept native loader milestone through production workers`
  - **Task Impact**: +10% to TODO: #3 (+2.5% overall).
  - **Summary**: Accept loader ABI/manifest milestone with real shared ICD device
    creation and public object/memory calls through the production negotiated
    worker. TODO #3 reaches50%, overall67.5%; no full runtime/DXVK acceptance.
  - **Verification**:11ca7e7 GitHub Linux job112420950151 passes the actual pinned
    loader production worker normal/sanitized and injected failure-cleanup gate.
    Native Windows job112420950697 passes DLL export/dispatch and actual pinned
    loader manifest/device/fence/memory tests. Local pool increment additionally
    passes actual loader normal/sanitized worker cycles and forced cleanup.
    Full latest-head CI remains required at task completion independently.

- **Commit `4df9b74`**: `feat(vgpu): dispatch command buffer allocation and recording lifecycles`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; actual command execution pending).
  - **Summary**: Add transactional1..64 dispatchable pool-owned allocation batches,
    validated batch free, primary/secondary begin inheritance, explicit Initial/
    Recording/Executable/Invalid states and individual/pool reset. Publish only
    exact complete host identities; retain uncertain ownership on loss and retire
    implicit children after validated pool destruction. Confine generated encoder
    GNU pointer arithmetic warning adaptation to the imported oracle headers.
  - **Verification**: Independent pinned encoders compare commands88..92. Every
    batch reply truncation, wrong count/identity/positive result, primary-only
    flags5 rejection, secondary inheritance guards, native allocation/recording
    errors, duplicate/stale/foreign arrays, local57-slot partial rollback and
    505-buffer exhaustion pass. Five Zig tests, ASan/LSan/UBSan and coverage pass:
    99.68% lines/94.10% branches. Actual static and shared-loader production worker
    allocate/begin/end/reset/free/implicit-pool-release cycles pass normal/safety;
    forced loader failure restores descriptors and leaks zero bytes. Actual pinned
    Linux loader command-buffer dispatch/first-word trampoline tests pass; native
    Windows fixture/DLL cross-link succeeds. Latest native Windows/runtime CI
    remains required. TODO #3 stays50%; commands/mapping/full API/DXVK pending.

- **Commit `f2633b8`**: `fix(vgpu): recover invalid command buffers through implicit begin reset`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Follow the pinned Vulkan1.4.307 lifecycle: reset-capable pools
    permit Begin from Invalid as well as Executable. Recording and Pending still
    fail locally; non-reset pools permit Initial only. Validate recovery from a
    host Begin allocation error and ensure non-reset Invalid rejection sends no
    request. Document the versioned specification boundary explicitly.
  - **Verification**: Native unit/loader tests, five Zig tests and ASan/LSan/UBSan
    pass; coverage99.69% lines/94.09% branches. Windows native fixture, DLL and
    pinned-loader fixture cross-link pass. TODO #3 remains50%.

- **Commit `f3d4530`**: `docs(vgpu): specify core fill recording and resource invalidation`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Define bounded buffer range validation, exact command118 encoding,
    generation-safe slot reference tracking and invalidation on buffer destruction.
    Specify reset/free ownership, Pending protection and recording-only acceptance.
  - **Verification**: Inspect pinned encoder and Vulkan fill-buffer valid usage;
    implementation and execution proof remain pending. TODO #3 remains50%.

- **Commit `fbbd1ac`**: `feat(vgpu): record bounded buffer fills with resource lifetime tracking`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; GPU execution pending).
  - **Summary**: Route core fill command118 using requested buffer size/usage,
    same-device binding and overflow-safe range checks. Track buffer references
    in fixed512-bit sets, invalidate recordings before referenced slot release,
    clear references on successful begin/reset/free and reject Pending buffer or
    pool destruction before host dispatch. No heap allocation or native pointer
    retention; CPU acknowledgment does not imply GPU retirement.
  - **Verification**: Independent pinned encoder, explicit/WHOLE_SIZE/rounded
    ranges, invalid inputs/state recovery, reference invalidation/reset/slot reuse
    and transport/tag errors pass. Six Zig tests include Pending destruction
    protection. ASan/LSan/UBSan and coverage99.70% lines/94.13% branches pass.
    Normal/sanitized actual static and shared-loader production worker recording
    pass; forced loader failure restores descriptors and leaks zero bytes. Pinned
    Linux loader public fill dispatch passes normal/sanitized; Windows fixture,
    loader fixture and DLL cross-link pass. Native Windows latest CI remains
    required. TODO #3 stays50%; submission/full API/mapping/DXVK still pending.

- **Commit `1f50cff`**: `docs(vgpu): specify bounded buffer copy and alias validation`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify exact command112, byte-granular bounded region arrays,
    requested-size requirements validation and binding-offset overflow proof.
    Define all-pairs source/destination union overlap checks for same-allocation
    buffers and shared recording lifetime rules.
  - **Verification**: Inspect pinned encoder and versioned Vulkan1.4.307 common
    validity; implementation/execution gates remain pending. TODO #3 stays50%.

- **Commit `c7bf471`**: `feat(vgpu): record bounded buffer copies with allocation alias checks`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; GPU submission pending).
  - **Summary**: Dispatch command112 only after validating every1..64 region,
    same-device bound transfer usage and requested-size bounds. Retain successful
    memory binding offsets; reject undersized host requirements while preserving
    caller output. Check all source/destination interval pairs for same-allocation
    aliases, including distinct buffers, and retain both recording dependencies.
  - **Verification**: Independent pinned encoder, byte-sized/64-region copies,
    zero/65/null guards, bad-final-region no-partial dispatch, overflow/range,
    foreign/unbound/usage guards, cross-index alias overlap, legal adjacency and
    distinct allocation tests pass. Six Zig tests and zero-leak ASan/LSan/UBSan
    pass; coverage99.72% lines/94.12% branches. Actual static/shared-loader worker
    fill/copy recording passes normal/sanitized and failure cleanup; pinned native
    Linux loader public copy dispatch passes normal/sanitized. Windows fixture,
    DLL and loader fixture cross-link pass. Latest CI remains required. TODO #3
    stays50%; actual submission/full API/mapping/DXVK still pending.

- **Commit `4696452`**: `docs(vgpu): specify full-size inline update staging`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Define native1..65536-byte update input, exact command117, fixed
    disjoint scratch/tx staging and source capture/scrub lifecycle. Preserve the
    small writer stack bound and shared buffer lifetime/recording rules.
  - **Verification**: Inspect pinned blob encoder4-byte stride, command owner's
    overlap checks/36-byte prefix and Vulkan update valid usage. Implementation
    and full-size production-worker proof remain pending. TODO #3 stays50%.

- **Commit `690b4ce`**: `fix(vgpu): size default worker commands for full inline updates`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Raise the bounded default command/private-transfer allocation
    to131072 bytes so65536-byte native data plus48-byte command and36-byte
    reply-stream prefix fit. Keep one service-owned aligned allocation with
    deterministic teardown and preserve explicit smaller trusted policy support.
    Document required backend scratch/command extent for full ICD binding.
  - **Verification**: Service acquisition/fault tests, ASan/LSan/UBSan and
    coverage100% lines/99.02% branches pass. Inline update worker fixture initially
    rejected its oversized request with prior4096-byte guest scratch; the pending
    recording increment expands scratch and verifies fragmented full-size input.
    TODO #3 stays50%.

- **Commit `ce01f55`**: `feat(vgpu): capture full-size inline buffer updates in bounded staging`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; GPU execution pending).
  - **Summary**: Route command117 with native1..65536-byte data after complete
    identity/binding/usage/alignment/range validation. Keep the8192-byte stack
    writer; use disjoint fixed65584-byte scratch and65620-byte tx. Capture source
    before dispatch, retain destination dependency after exact acknowledgment and
    erase scratch/used tx on success and every error. Document backend extent;
    provide131072-byte guest scratch and180-second full-fixture watchdog for
   16 cycles of full updates fragmented through64-byte rings.
  - **Verification**: Independent pinned encoder verifies4/8/65536-byte patterns;
    rejected scalar guards do not read deliberately invalid source pointers.
    Seven Zig tests include source capture and exact erasure/reference publication
    on success/transport/malformed reply paths. Zero-leak ASan/LSan/UBSan and
    coverage99.73% lines/94.04% branches pass. Actual static/shared-loader full-size
    worker recording passes normal/safety plus injected failure descriptor/heap
    cleanup. Pinned native Linux loader public update passes normal/sanitized;
    Windows fixture, DLL and loader fixture cross-link pass. Latest CI required.
    TODO #3 stays50%; synchronization/submission/full API/mapping/DXVK pending.

- **Commit `e7beb91`**: `docs(vgpu): specify core memory and buffer pipeline barriers`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Define exact command126 and bounded canonical memory/buffer
    barrier arrays, core masks and family/range validation. Specify execution-only
    normalization, no partial dispatch, shared resource lifetime and native semantic
    caller requirements. Image scope remains pending, without completion credit.
  - **Verification**: Inspect pinned encoder record strides/ordering and Vulkan
    valid usage; implementation and actual worker dependency recording pending.
    TODO #3 stays50%.

- **Commit `184f002`**: `feat(vgpu): record bounded global and buffer pipeline barriers`
  - **Task Impact**: +0% to TODO: #3 (+0% overall; full execution pending).
  - **Summary**: Dispatch pinned command126 for execution-only/global/private
    buffer dependencies. Validate complete canonical arrays, core masks, family
    pairs and byte/WHOLE_SIZE bounds before sending; track buffer references only
    after acknowledgment. Normalize ignored zero-count pointers and reject image
    scope until image layout/ownership APIs are implemented. Add transfer hazards
    and host-read dependencies to the real worker's fill/copy/update recording.
  - **Verification**: Independent encoder checks zero/one/64 arrays; invalid final
    record sends no partial command. Range, identity/binding/family/mask/tag guards
    and destruction/reset tests pass. Seven Zig tests and ASan/LSan/UBSan pass;
    coverage99.74% lines/93.78% branches. Actual static/shared-loader worker and
    native loader recording pass normal/sanitized, including failure cleanup.
    Windows fixture/DLL/loader cross-link pass. Native Windows inline update CI
    ce01f55 passes; one Linux coverage run aborts while parallel same-head run
    passes that stage, under investigation/retry. Latest-head checks remain
    required. TODO #3 stays50%; submission/full API/mapping/DXVK still pending.

- **Commit `a32ce25`**: `test(vgpu): diagnose coverage recorder bounds and output failures`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Report bounded recorder branch/edge violations and output path/
    open/close failures before aborting. Publish instrumentation capacity counts
    and validate them against the runtime's named bounds before execution. Preserve
    atomic recording and all existing coverage thresholds; do not mask failures.
  - **Verification**: Strict C probe accepts final valid4095/31 site/edge and
    rejects4096/0 and0/32 with diagnostic SIGABRT. ICD seven-test coverage passes
    at99.74% lines/93.78% branches with2158/4096 sites, maximum3/32 edges. CI's
    single same-head coverage abort remains without a confirmed cause; parallel
    ce01f55 run passed that gate and failed job is retried. TODO #3 stays50%.

- **Commit `c9bafe7`**: `docs(vgpu): specify binary semaphore ownership and pending references`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify canonical core binary semaphore commands40/41, exact
    transactional identities, negative rollback and uncertain loss retention.
    Define fixed in-flight reference counts for subsequent submission lifetime
    enforcement, with no synthetic signal state or timeline claim.
  - **Verification**: Inspect pinned semaphore encoders and existing private
    object/identity transaction contracts; implementation/execution pending.
    TODO #3 stays50%.

- **Commit `cb463c6`**: `test(vgpu): expand coverage recorder for CI instrumentation`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Increase fixed atomic site capacity from4096 to16384 without
    changing edge limits, generated metadata guards or coverage thresholds.
    CI diagnostics identify4712 sites exceeding4096; local compiler emits fewer.
    The recorder owns512KiB static storage, performs no dynamic allocation and
    rejects out-of-range sites before indexing. TODO #3 remains50%.
  - **Verification**: Strict C boundary probe accepts16383/31 and aborts on
    16384/0 and0/32. Latest native ICD coverage remains required.

- **Commit `36cc757`**: `feat(vgpu): implement binary semaphore ownership and pending guards`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Dispatch canonical binary semaphore create/destroy40/41 with
    exact identity publication, known-error rollback and loss retention. Add fixed
    in-flight counts and refuse pending destruction. Shared constructors accept
    null failure outputs only after negative native results, preserving exact
    positive/success identities and result-less queue behavior.
  - **Verification**: Independent pinned encoder, native malformed/negative/null/
    capacity/foreign/stale/transport guards and private pending tests pass. Eight
    Zig tests and ASan/LSan/UBSan pass; coverage99.75% lines/94.01% branches with
    2233/16384 sites. Real static/shared-loader worker and pinned native loader
    normal/sanitized semaphore lifecycles pass, including forced-failure cleanup.
    Windows fixture/DLL/loader cross-link passes. Queue submission and full API/
    mapping/DXVK remain pending; TODO #3 stays50%. Latest CI required.

- **Commit (current; resolve by subject)**: `docs(vgpu): specify queue submission and proven GPU retirement`
  - **Task Impact**: +0% to TODO: #3 (+0% overall).
  - **Summary**: Specify command18 canonical binary submits, exact bounded arrays,
    primary/family/state validation and128 fixed reference tickets. Define real
    fence/timeline proofs, any/all retirement, pending reset/destruction suppression
    and ONE_TIME/SIMULTANEOUS final state transitions without CPU completion credit.
  - **Verification**: Inspect pinned queue encoder and renderer native QueueSubmit
    dispatch. Implementation, oracle coverage and actual GPU worker submission
    remain pending; TODO #3 stays50%.
