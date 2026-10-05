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
| #1      | Set up Venus protocol transport over IVSHMEM ring buffer | In Progress | 15% | 10% | Transport contract specified; runtime and adapters pending |
| #2      | Implement host-side Venus receiver (virglrenderer/vkr integration) | Pending | 25% | 0% | - |
| #3      | Implement guest-side WDDM render-only driver and standalone Vulkan ICD | Pending | 25% | 0% | Requires DXVK integration testing |
| #4      | Implement zero-copy DMA-BUF export and Wayland `zwp_linux_dmabuf_v1` integration | Pending | 15% | 0% | - |
| #5      | Implement OpenCL compute remoting layer (rusticocl over Venus) for Adobe compatibility | Pending | 20% | 0% | Required for high-perf compute |

**Total Feature Completion**: `1.5%`

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
- **Commit (current; resolve by subject)**: `docs(vgpu): specify IVSHMEM ring transport contract`
  - **Task Impact**: +10% to TODO: #1 (+1.5% overall).
  - **Summary**: Defined cache-isolated C ABI, bounded Zig validation, SPSC ordering,
    errors, ownership, milestones, and transport-versus-renderer boundary.

The current entry is identified by its unique commit subject because a commit
cannot contain its own content-derived hash. Each following atomic commit records
the preceding entry's actual hash; `git log` resolves the latest entry directly.
