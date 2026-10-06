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
| #2      | Implement host-side Venus receiver (virglrenderer/vkr integration) | In Progress | 20% | 10% | Scoped public renderer/context/reply owner and real CPU dispatch verified; resource/session/GPU integration pending |
| #3      | Implement guest-side WDDM render-only driver and standalone Vulkan ICD | Pending | 25% | 0% | Requires DXVK integration testing |
| #4      | Implement zero-copy DMA-BUF export and Wayland `zwp_linux_dmabuf_v1` integration | Pending | 15% | 0% | - |
| #5      | Implement OpenCL compute remoting layer (rusticocl over Venus) for Adobe compatibility | Pending | 20% | 0% | Required for high-perf compute |
| #6      | Pin, configure, build, and verify virglrenderer dependency | Done | 5% | 100% | Immutable pin, license audit and offline build verified in CI |

**Total Feature Completion**: `22.0%`

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

- **Commit (current; resolve by subject)**: `test(vgpu): integrate independent mock guest and Venus replies`
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
