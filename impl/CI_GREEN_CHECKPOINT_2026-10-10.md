# Verified native CI checkpoint · 2026-10-10

Branch `feature/software-vgpu-slicing` reached all-green published commit
`879d0c38c29e22b400c29fd79236b36df6f015f3`, identical to local
`6c91d14e3bb0461060fd7d9889650fe27e87fed8`, tree
`cfdb046b8e4508eed620f96f797b566a116352d1`. All six workflow records completed
successfully, verified at 2026-10-10T00:31Z. This receipt describes that exact
source, not an automatic pass for subsequent changes.

## Observed results

- Native Windows ICD: 394/394 Debug and 394/394 ReleaseSafe tests, 128 native
  lifecycle cycles, DLL/adapter checks and eight pinned-loader lifecycles pass.
  [PR Windows job](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005370988/job/114072817899)
  · [push Windows job](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005366534/job/114072802110).
- Linux recorder: ordinary suite runs eight tests with one intentional
  sanitizer-only skip; strict ASan/LSan/UBSan mode passes all eight, including
  `test_sanitizer_violation_is_nonrecoverable`. Runtime options explicitly retain
  leak detection and forbid recovery; the negative control proves fatal UBSan.
- Linux ICD: 1,594 functions instrumented for the scoped sanitizer gate. Native
  whole-ICD coverage passes 4,368/4,837 branches (90.30%) and 4,689/4,757 lines
  (98.57%) with unchanged measurement policy and all raw sites retained.
- Real direct and pinned-loader production worker fixtures pass normal and
  sanitizer targets with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`.
  Export-lease, surface, Wayland, receiver/runtime and context/crash-isolation
  gates also pass. Later `ICD lifecycle mode 0` failure output belongs to the
  deliberate `WADDLE_TEST_LOADER_FAILURE=1` negative control; its exact make
  recipe requires exit 1, so this is not an ignored source failure.
  [PR Linux job](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005370988/job/114072817986)
  · [push Linux job](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005366534/job/114072802295).
- [Native AV PR](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005371015),
  [Native AV push](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005366472),
  [native guest](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005370985)
  and [Linux CLI](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38005370992)
  workflows all pass.

## Meaning and limits

This closes the bounded Windows test-environment, extension-oracle, WSI fixture,
worker-link/query-oracle and coverage-recorder CI repairs. Earlier failed local
LeakSanitizer attempts remain failed/blocked receipts; successful supported CI
supplements them rather than reclassifying them.

CI software rendering and native fixtures do not prove physical SendInput/UIPI,
real compositor interaction, cross-VM mapping, shared RTX 5080 execution,
Photoshop or Premiere acceleration. The broader 32-module ICD-owned/full-seam
instrumentation migration remains separate from the passing scoped ICD gate.
The worker alarm is not a full descendant-tree supervisor; that known tooling
limit is not evidence of a hang in these successful runs.

Continue from the [single prioritized review checklist](REVIEW_CHECKLIST.md)
and [exact hardware handoff](HARDWARE_TEST_HANDOFF.md). Preserve tested source,
binary hashes and logs for each future acceptance run.
