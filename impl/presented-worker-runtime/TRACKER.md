# Feature Tracker: Presented Worker API1.0 Query Runtime

- **Contributors / Agents**: Presented-worker runtime repair worker; independent runtime-gate reviewer
- **Time Started**: 2026-10-09T23:27:46Z
- **Time Ended**: 2026-10-10T00:31:20Z
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Completed

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Identify and reproduce the exact production query-admission contradiction | Done | 25% | 100% | Real ICD dispatcher: original no-extension pointers both null, original assertion exits1 |
| #2 | Correct direct-only extension setup and retain exact legal query assertions | Done | 25% | 100% | Enabled KHR instance/physical routes agree; API1.0 core remains hidden |
| #3 | Build four actual worker binaries and verify native boundary regressions | Done | 20% | 100% | Four final-source binaries link; complete C ABI and native-loader fixtures pass |
| #4 | Independent source/evidence review and atomic commit receipts | Done | 15% | 100% | Final exact source/binary hashes, regression logs and limits independently approved |
| #5 | Observe updated native normal/sanitized production-worker CI | Done | 15% | 100% | Exact 879d0c3 native CI passed; see dated receipt below |

**Total Feature Completion**: `100.0%`

## Verification Receipts

- CI failure source: f04e3583ad2af32f28145933c2df563ee4cac295,
  https://github.com/Kayyo321/Waddle-LSW/actions/runs/38002124471/job/114062429263 .
  Step30 links and executes the actual direct worker acceptance executable, then
  emits its generic mode0 failure before the first device-name log. Sanitized
  worker execution is not reached.
- Exact production ICD admission probe: derived the existing native C ABI make
  recipe, substituted only a temporary probe translation unit including the
  existing `tests/vgpu/icd.c` independent encoder oracle, and linked unchanged
  production ICD/support objects. No mock renderer/worker was introduced.
  Original API1.0/no-extension create succeeds but both Features2 pointers are
  null; the original assertion's predicate exits **1**. Enabled KHR create
  succeeds, core remains null and KHR resolves; exit **0**. The unchanged
  `features2_public` native oracle then passes all repeated cached query,
  header/link and unknown-canary checks. This proves admission/dispatch only,
  not actual renderer execution. Logs and probe source/recipe reside in
  `build/presented-worker-runtime/`.
- Unchanged aggregate `make vgpu-presented-worker-test
  vgpu-presented-worker-sanitizers` exits **2** during renderer prerequisite
  configuration: `/bin/sh: 1: meson: not found`. Neither actual normal nor
  sanitized worker executes. `exact-ci-gate.log` records this blocker.

- Final fixture SHA-256:
  `46cf1d6951fec19b67fe3fe408721a1719d460e43745847c3cc5803aec081146`.
  After the final diagnostic wording change, the actual build rules link
  `build/vgpu_presented_worker_test`, `build/vgpu_presented_worker_sanitized`,
  `build/vgpu_loader_worker_test` and `build/vgpu_loader_worker_sanitized` with
  strict warnings and unchanged sanitizer flags; exit **0**. The command uses
  `make -o build/waddle_vgpu_worker -o build/waddle_vgpu_worker_sanitized` only
  to omit unavailable worker prerequisites for this link-only check. This does
  not create, execute or substitute a worker. Final log:
  `worker-links-final.log`; hashes: `source-executable-sha256.txt`.
- Existing `make build/vgpu_icd_test build/vgpu_icd_loader_test`, followed by
  executing both binaries, exits **0**. Each runs its full native C ABI suite,
  including128 lifecycle cycles and exact Features2 cache/canary tests. The
  loader variant also passes manifest discovery and eight instance/device/
  queue/fence lifecycles. Log: `native-c-abi.log`. These use their existing
  independent encoder backend, not the actual receiver renderer.
- Running the final normal worker acceptance executable with its unchanged
  default path exits **1** and accurately reports
  `stage=production worker executable, mode=0`, with descriptors **6/6**.
  The actual worker and render-server executables are absent locally.
  `missing-worker-diagnostic.log` verifies failure reporting and this local
  descriptor baseline only; it is not positive worker acceptance.
- Running the final sanitized worker acceptance executable with unchanged
  `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1` and the actual
  expected worker/render-server paths reports the same missing-executable
  failure and descriptor baseline, then LeakSanitizer aborts with its process-
  inspection/ptrace limitation; exit **134**. Log:
  `sanitized-runtime-blocked.log`. No sanitizer pass or zero-leak claim is made.
- The independent reviewer reproduced both admission-probe outcomes and
  confirmed strict C11 syntax/warnings in direct and loader modes. The compute
  and triangle workload helper bodies are byte-identical to the pre-change
  source. Diagnostic expansion preserves the old failure-message substring
  consumed by `scripts/icd_physical_sanitizers.py`. Final review independently
  reran both complete native C ABI/loader fixtures, verified the source and four
  binary hashes, and approved the bounded three-file correction. No source
  blockers remain; real normal/sanitized worker acceptance remains pending.
- Exact block comparison against `e7c410b` verifies the feature-query output/
  cache/canary block is byte-identical except its explanatory comment; the
  unregistered presentation and corrupt-release/worker-exit guards are byte-
  identical except diagnostic stage assignments. Production sources, public
  headers and make rules have no changes from this repair. `git diff --check`
  passes.

### Exact local evidence hashes

| Receipt | SHA-256 |
|:--|:--|
| `instance-gate-probe.log` | `c6b65252fedf8c2efaa00dcf9caa52758d5606877bbc862ebbcd364564a39966` |
| `native-c-abi.log` | `c55d52361dbdcb4214d802bf50f8d71c05d9a27964dce295a27e1ff082b4e4e4` |
| `worker-links-final.log` | `b005a7404bc89a4eac1ee5c45952b983f4786a483060aef1c33bab134073c02b` |
| `missing-worker-diagnostic.log` | `db4a366779f96b0f3d845dedac59fe4b759137e3b7ab43c14a489e36a47df076` |
| `sanitized-runtime-blocked.log` | `54f1cd172c4262af0b617cacccb1dd4c72b14e90f94c3fd745e1ef17a6e6df2d` |

## Commit History & Progress Log

- **Commit `2742007ad9fb88d5f8a915ea79d4537517a4964f`**:
  `test(vgpu): enable legal KHR queries in worker acceptance`
  - **Task Impact**: #1 +100% (+25% overall), #2 +100% (+25%), #3 +100%
    (+20%), #4 +100% (+15%); total **85%**. #5 remains0%.
  - **Summary**: Explicit direct-instance KHR enablement, identical KHR
    instance/physical lookup routes with the core1.1 spelling still hidden,
    unchanged cache/canary/ownership guards, bounded failure-stage diagnostics,
    four exact acceptance-binary links, independently reproduced dispatch
    failure/correction and complete C ABI/loader regressions. Local real-worker
    and sanitizer limitations remain explicit above.

This documentation-only attribution receipt changes no source and adds **+0%**
completion. Actual updated native normal/sanitized worker CI remains pending;
no software-vGPU milestone advance is claimed. Parent owns publication and
remote acceptance monitoring.

## Supported CI closure · 2026-10-10

Published `879d0c38c29e22b400c29fd79236b36df6f015f3` passed all six native
workflow runs. [Exact source, results and job links](../CI_GREEN_CHECKPOINT_2026-10-10.md)
verify the formerly pending bounded gate. Task #5 is now 100% (+15% overall),
bringing this scoped repair to 100%. This receipt records observed CI evidence;
it changes no source and grants no physical GPU/Adobe acceptance. Earlier local
LSan failures and pending statements remain historical; the
[current review index](../REVIEW_CHECKLIST.md) is authoritative.
