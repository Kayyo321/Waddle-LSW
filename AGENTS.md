# Agent Workflow & Development Rules: Waddle-LSW

This document defines the strict operating standards, branching models, implementation documentation rules, and commit hygiene required for all AI agents and contributors working on the **Waddle-LSW** codebase.

---

## 1. Core Operating Principles

1. **Atomic & Frequent Commits**:
   - Every small, discrete change requires its own git commit with a detailed, explanatory commit message.
   - Never batch multiple unrelated changes into a single commit.
   - Never leave completed work uncommitted.

2. **Zero Ambiguity Policy**:
   - Implementation descriptions must be documented in minute detail before or during development.
   - Every question, edge case, memory model, and interface boundary must be completely specified without leaving anything to interpretation.

3. **Strict Progress Auditing**:
   - All feature work must maintain a synchronized progress tracker that quantifies progress on a per-task basis and attributes progress changes directly to specific commits.

---

## 2. Branching & Git Workflow

### 2.1 Refactoring & Maintenance
- **Standard Branch** (e.g., `origin` / `main`):
  - Refactoring work (code cleanups, performance optimizations without feature alterations, reorganization, formatting fixes, minor bugfixes to existing code) must be conducted directly on the standard branch.
  - Every individual refactoring step still requires an atomic commit with a detailed commit message.

### 2.2 Adding or Removing Features
- **Feature Branches**:
  - Whenever adding a new feature or removing an existing feature, **always create a dedicated feature branch**.
  - **Naming Convention**: `feature/<feature-name>` or `feat/<feature-name>` (lowercase, hyphen-separated).
    *Example*: `feature/guest-window-tracker`, `feature/ivshmem-transport-ring`
  - All implementation work, commits, and implementation documentation for that feature must reside strictly within this feature branch.
  - Feature branches are integrated into the standard branch exclusively via **Pull Requests (PRs)** once all tasks in the feature tracker reach 100% completion and verification passes.

### 2.3 External Dependencies & Git Submodule Workflow
- **Mandatory Submodule Architecture**:
  - All third-party libraries, vendor SDKs, protocols, and headers not provided by base system packages must be vendored as **Git submodules** under `submodules/<library-name>`.
  - Loose source vendoring directly into the repository tree is strictly forbidden.
- **Submodules Introduced in Feature Branches**:
  - If a feature requires an external library, the submodule must be added **directly on the feature branch**, never prematurely on the standard branch.
  - The feature's `IMPL_DESC.md` must document the library rationale, license compatibility, architectural boundary, and exact commit SHA.
  - The feature's `TRACKER.md` must allocate a dedicated task for submodule addition, configuration, and verification.
  - Submodule additions or version bumps must be isolated in dedicated atomic commits prefixed with `chore(deps):`.
- **Branch Switching & Submodule Hygiene**:
  - Switching between branches that have different submodules or different submodule commits requires synchronizing the working tree:
    ```bash
    git submodule update --init --recursive
    ```
  - Contributors are encouraged to configure Git to recurse submodules automatically:
    ```bash
    git config submodule.recurse true
    ```
  - **Handling Residual Directories**: When switching from a feature branch that added a submodule back to a branch where it does not exist, Git leaves the submodule directory untracked. To prevent ghost directories and build artifacts from polluting the workspace, deinitialize and clean residual directories:
    ```bash
    git submodule deinit -f submodules/<library-name>
    git clean -dff submodules/
    ```
- **Rebase & Merge Conflict Resolution**:
  - When rebasing a feature branch onto the standard branch, run `git submodule update --init --recursive` immediately after rebasing.
  - **Textual Conflicts in `.gitmodules`**: Resolve by retaining valid configuration blocks for all required submodules; ensure `path` and `url` directives are correct.
  - **Gitlink (Commit Pointer) Conflicts**: Resolve by inspecting the conflicting commit SHAs within `submodules/<library-name>`, checking out the agreed-upon commit SHA, and staging the submodule pointer with `git add submodules/<library-name>`.
- **Commit Pinning & Remote Availability**:
  - Submodule pointers must always reference an explicit, immutable commit SHA (or release tag) that exists on the upstream public remote repository. Never commit pointers to local unpushed commits or ephemeral development branches.
  - Submodule URLs in `.gitmodules` must use public HTTPS (`https://github.com/...`) to ensure unauthenticated clones in CI environments and developer setups.

---

## 3. Feature Implementation Requirements (`impl/`)

When beginning work on any new feature, create a dedicated directory under `impl/` named after the feature title:

```
impl/<feature-title>/
├── IMPL_DESC.md
└── TRACKER.md
```

*Example*: `impl/guest-window-tracking-agent/IMPL_DESC.md` and `impl/guest-window-tracking-agent/TRACKER.md`.

Both files are mandatory.

---

### 3.1 `IMPL_DESC.md` (Implementation Description)

`IMPL_DESC.md` must be **minute in detail and cover every topic in redundant detail**. Leave absolutely no topic, design decision, data structure, or question up to interpretation.

The document must contain the following required sections:

1. **Title & High-Level Scope**:
   - Precise definition of what the feature does.
   - Explicit boundaries: what is inside scope vs. what is out of scope.
2. **Architecture & Inter-Component Interactions**:
   - Component diagrams, system layers, and operational flow.
   - Interactions between host, guest, hypervisor, or external libraries.
3. **Data Structures, Protocols & Memory Layouts**:
   - Exact struct definitions, memory alignment (e.g. 64-byte cache line packing), atomic primitives.
   - Wire formats, byte order, serialization schemas, error response codes.
4. **Step-by-Step Execution Sequence**:
   - Chronological step-by-step walkthrough of each functional code path (initialization, normal execution loop, teardown).
5. **Concurrency, Threading & Synchronization**:
   - Thread ownership, mutexes, atomic operations, lock-free queue guarantees, memory fences.
6. **Error Handling & Failure Modes**:
   - Handling of disconnects, invalid handles, buffer exhaustion, hypervisor faults, crash recovery.
7. **Verification & Testing Criteria**:
   - Unit tests, integration tests, benchmark thresholds, stress tests.

---

### 3.2 `TRACKER.md` (Implementation Tracker)

`TRACKER.md` tracks the real-time progress of the implementation. It must strictly conform to the following schema:

#### 1. Header Metadata
```markdown
# Feature Tracker: <Feature Title>

- **Contributors / Agents**: <Name(s) or Agent IDs who worked on this feature>
- **Time Started**: <ISO 8601 Timestamp, e.g., 2026-10-04T14:30:00Z>
- **Time Ended**: <ISO 8601 Timestamp or TBD>
- **Feature Branch**: <feature/branch-name>
- **Target Merge Branch**: <origin or main>
- **Current Overall Status**: <Planning | In Progress | Review | Completed>
```

#### 2. TODO Table & Progress Percentage
A structured table listing all discrete tasks and their progress:

```markdown
## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1      | Define shared memory layout and registry structs | Done | 15% | 100% | Validated byte alignment |
| #2      | Implement WinEvent hook lifecycle listener | In Progress | 25% | 40% | EVENT_OBJECT_CREATE wired |
| #3      | Implement DXGI surface capture pipeline | Pending | 35% | 0% | Waiting on #2 |
| #4      | Integrate ring buffer IPC notification | Pending | 25% | 0% | - |

**Total Feature Completion**: `25.0%`
```

#### 3. Commits Log & Task Progress Attribution
Directly below the table, record every commit made to the feature branch. Each commit entry must explicitly detail the commit hash, short summary, and the `+/- %` impact on specific tasks:

```markdown
## Commit History & Progress Log

- **Commit `a1b2c3d`**: `feat(guest-agent): define memory layout and registry structs`
  - **Task Impact**: +100% to TODO: #1 (+15% overall feature completion)
  - **Summary**: Implemented IVSHMEM registry header with cache-line alignment and magic word verification.

- **Commit `e4f5g6h`**: `feat(guest-agent): register WinEvent hooks for create/destroy`
  - **Task Impact**: +40% to TODO: #2 (+10% overall feature completion)
  - **Summary**: Hooked EVENT_OBJECT_CREATE and EVENT_OBJECT_DESTROY, filtered non-application windows.
```

---

## 4. Commit Standards & Hygiene

1. **Commit Message Format**:
   - Use conventional commit prefixes:
     - `feat:` New feature implementation
     - `fix:` Bug fix
     - `refactor:` Code refactoring without behavior modification
     - `docs:` Documentation updates
     - `test:` Adding or modifying tests
     - `chore:` Build scripts, tooling, dependencies
   - **Detailed Body**: Every commit must provide a detailed explanation of what was changed and why:
     ```
     feat(guest-tracker): implement HWND filter for top-level shell windows

     - Filter out tooltips, IME windows, and hidden background utilities.
     - Inspect WS_EX_TOOLWINDOW and WS_VISIBLE styles.
     - Extract window bounds using GetWindowRect and DwmGetWindowAttribute.

     Affects TODO: #2 (+15% progress)
     ```
     ```
     chore(deps): add mylib as git submodule pinned to v1.2.3 (abc1234)

     - Track upstream repository https://github.com/example/mylib.git under submodules/mylib.
     - Pin commit SHA abc1234 corresponding to tagged release v1.2.3.
     - Required for DXGI surface transformation pipeline in guest agent.
     - Audited license: MIT (compatible with Waddle-LSW).

     Affects TODO: #3 (+10% progress)
     ```
2. **One Small Change Per Commit**:
   - Do not bundle multi-file sweeping edits unless strictly required by a signature refactor.
   - Ensure the codebase builds or passes verification checks at each commit point whenever feasible.

---

## 5. Review & Merging Checklist

Before any feature branch is merged into the standard branch:
- [ ] `IMPL_DESC.md` is complete, thoroughly detailed, and contains no remaining TODOs or unaddressed questions.
- [ ] `TRACKER.md` shows `100%` progress across all tasks.
- [ ] `TRACKER.md` header has a concrete `Time Ended` timestamp recorded.
- [ ] All commits in the feature branch are documented in `TRACKER.md` with their task impact.
- [ ] Feature tests pass on both host and guest environments where applicable.
- [ ] All external dependencies are tracked as Git submodules under `submodules/` (no loose source trees).
- [ ] Submodule remote URLs use public HTTPS protocol (no private or SSH-only URLs).
- [ ] Pinned submodule commit SHAs exist on the upstream remote repositories.
- [ ] `.gitmodules` contains clean, valid stanzas without merge conflict markers.
- [ ] CI pipeline and local builds execute `git submodule update --init --recursive` cleanly.
- [ ] Pull Request is opened with links to `IMPL_DESC.md` and `TRACKER.md`.
