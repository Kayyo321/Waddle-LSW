# Feature Tracker: Native coverage recorder integrity

- **Contributors / Agents**: coverage diagnosis worker; independent coverage integrity reviewer
- **Time Started**: 2026-10-09T22:49:49Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Reproduce native-target site divergence | Done | 25% | 100% | Exact 76,512-site Zen 3 reproduction; matching 625/706 smaller modes |
| #2 | Implement metadata-bound atomic storage | Done | 25% | 100% | Exact native IDs, arities, digest binding and sizeof-checked 512KiB budget |
| #3 | Verify malformed, concurrency and whole-gate behavior | In Progress | 25% | 80% | Seven focused tests, whole native aggregate and exact Zen3 replay pass; fresh CI sanitizer acceptance pending |
| #4 | Independent review and atomic commit | Done | 25% | 100% | Final code, seven tests and transformed IR independently approved; atomic implementation commit |

**Total Feature Completion**: `95.0%`

## Commit History & Progress Log

- **Commit `e268984ec71b10e2cc848802001d5724299fd103`**: metadata-bound atomic masks and arity table, integrity checks, generated-IR summary cleanup, focused regressions and failure artifacts. Tasks #1/#2/#4 are100%; #3 is80% pending supported fresh-CI sanitizer acceptance. No hardware, Windows, renderer or DXVK acceptance credit.

## Exact evidence and limitations

- Root cause: identical GitHub source at c11ce21 produced 76,512 sites with the
  native Zen3 profile and 14,235 on the other runner. Both jobs used Debian image
  sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f,
  checked Zig0.13.0 archive d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea,
  LLVM19.1.7-3+b1 and identical 219 apt package-download records. No build-cache
  restoration occurred. Their CPU model was not logged; controlled znver3
  compilation reproduced all three distinctive counts (ICD76,512; device625;
  profiles706). Failed CI had not uploaded coverage IR, motivating retention.
- Exact old-commit Zen3 replay preserves every one of the 600,315 recorder calls
  and IDs0..76,511; all393 tests and128 native cycles pass. Branches4368/4837
  (90.30%); lines4689/4757 (98.57%); unchanged2614 excluded compiler guards.
  Storage382560/524288 bytes. The record SHA256 remains
  840eba531e60667785a8a4649e68a35bf8293a3552077b961d57192aa2782c4f. This is
  original-c11ce21 evidence, distinct from the newer frozen-source aggregate.
- Independent reviewer compared all3,166,108 transformed IR lines: exactly29
  summary lines changed beyond recorder-symbol binding; instructions, calls,
  debug metadata, target/features and complete arity table are preserved.
- `python3 -B tests/av/coverage_recorder_test.py -v`: seven tests pass, including
  76,512-site all-ID round-trip,32-thread edge union, exact512KiB maximum,
  malformed arity/index/overflow/declaration rejection, stale digest linker
  rejection and optimized LLVM round-trip of16,385 IDs. Independent rerun passes, including an actual zero-site harness failure-manifest path.
- Local ASan/LSan/UBSan attempt compiles, but four successful-exit probes hit the
  executor's LeakSanitizer ptrace restriction. It is not a sanitizer/leak pass.
  CI runs both ordinary and full-three-sanitizer probes, with no suppression;
  fresh CI sanitizer acceptance remains required. No hardware or Windows credit.

## Final frozen-source aggregate receipt

- 2026-10-09T23:17Z: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-coverage`
  exits0. Every prerequisite passes, followed by394 ICD tests and128 native cycles.
  Native target is x86-64 with the recorded host feature set; no CPU substitution.
  Exactly14,251 sites use71,256/524,288 bytes. Branches4369/4837 (90.32%);
  lines4694/4764 (98.53%);2410 compiler guards excluded by unchanged policy.
- Before/after ICD source SHA256 is
  8324b5e62656ce9b1da076658702d313c8567ae6065f0576d79f8837f94087d9.
  Artifact directory: build/coverage/vgpu/venus_icd/run-20261009T231403585647Z-584.
  Records SHA256 a91c60ef9878b06c3d1dd0411f5972db0b95c09d93090a4e79dafae0a11ba3b5.
  Raw IR SHA256 188cafaa7d1594f90bba1c3d3e7bdfeb3b4cb901bd06cec7b3944505f2cbdb07.
- Independent review proves the extraction/fixture/guard logic and hit-aggregation/
  90% gate segments remain byte-identical. Seven final tests pass independently.
- Legacy storage coverage, using unchanged test logic with only a fresh output
  directory, passes all4 tests and92/99 branches (92.93%); its default recorder
  symbol and old-limit boundary remain compatible.
- Supported CI must establish the remaining ASan/LSan/UBSan acceptance. Local
  ptrace restrictions are not suppressed or counted as a pass. Overall software
  vGPU/hardware milestones receive no completion-percentage increase.

- Verification receipt commit changes documentation only (+0%); it binds implementation `e268984ec71b10e2cc848802001d5724299fd103` to the receipts above. Fresh CI sanitizer verification remains open.

## Fail-closed sanitizer follow-on

- 2026-10-09T23:32Z: sanitizer fixture builds now add
  `-fno-sanitize-recover=all`; child runtime options are completely replaced with
  leak-enabled halt/abort/nonzero-exit settings for ASan/LSan/UBSan. Inherited
  suppression paths, disabled leak scanning and recovery/zero-exit options cannot
  weaken the probes. Ordinary test behavior and recorder/measurement code remain
  unchanged. This corrects recoverable UBSan's possible zero-exit false pass.
- The suite now has 8 tests: normal mode executes the original 7 and skips the
  sanitizer-only negative control; strict sanitizer mode executes all 8. Normal
  mode passes 7/7. The isolated signed-overflow negative control passes 1/1: exact
  UBSan diagnosis, SIGABRT, no post-error marker, and no false success caused by
  LeakSanitizer's unrelated ptrace failure, despite hostile inherited options.
- Complete local strict rerun remains failed on 4 positive-exit probes because
  LeakSanitizer cannot inspect this executor under ptrace. The negative control
  passes in that same run. No leak acceptance is claimed or suppression added.
  CI's existing strict invocation now exercises the fail-closed options and all 8
  tests. Independent review precedes the follow-on commit; +0% qualification or
  hardware milestone credit until supported fresh CI completes.

- **Commit `9dc89c7fc048c483dd1770b46790e7feeaf3f002`**: `test: make coverage sanitizer probes fail closed`.
  Independent code and documentation review approved the exact test source
  SHA256 `9f1374778868ab9ab469b47cd9566636a692f797bf745dc011b39f23dec8eca7`.
  Both compiler paths use non-recovery flags; all seven existing test methods
  remain AST-identical. Task impact: +0% overall or hardware milestone credit;
  supported-CI sanitizer acceptance remains open. This receipt is documentation
  only and adds no verification credit.
- Reproduction: after `source /workspace/shared/waddle-tools/env.sh`, run
  `python3 -B tests/av/coverage_recorder_test.py -v` for the ordinary suite; prefix
  `WADDLE_RECORDER_SANITIZERS=1` for the strict suite. To isolate the supported
  negative control, use that prefix and append
  `recorder_test_t.test_sanitizer_violation_is_nonrecoverable -v` after the script.
  Local logs and their SHA256 values are retained in
  `/workspace/shared/waddle-tools/coverage-recorder-sanitizer-hardening-manifest.json`.
