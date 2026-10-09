# Feature Tracker: Native WSI test device capabilities

- **Contributors / Agents**: Native WSI capability repair worker; independent reviewer
- **Time Started**: 2026-10-09T23:00:34Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Diagnose native capability admission and shared synthetic fixture | Done | 15% | 100% | Both native failures omitted explicit swapchain enablement |
| #2 | Opt in test WSI device and preserve disabled-extension rejection | Done | 30% | 100% | Exact name/bit assertion and four disabled masks; production unchanged |
| #3 | Verify Linux full suites, C fixtures, and complete Windows links | Done | 25% | 100% | 394 Debug + 394 ReleaseSafe, focused 7 + 7, three C fixtures, both full Windows links |
| #4 | Independently review source preservation and verification evidence | Done | 15% | 100% | Independent final-source and evidence approval; all 196 original test bodies unchanged |
| #5 | Observe exact-commit native Windows runtime and leak-capable CI | Pending | 15% | 0% | Parent owns publication and native CI |

**Total Feature Completion**: `85.0%`

## Commit History & Progress Log

The initial source/documentation commit completes tasks #1, #2, #3, and #4: +15%, +30%, +25%, and +15% overall respectively, for **85%** bounded completion. Its exact hash will be bound in the subsequent documentation receipt. Task #5 remains 0%; native runtime and leak-capable CI qualification are not inferred from local success.

## Verification receipts

All final verification applies to source SHA-256 `8324b5e62656ce9b1da076658702d313c8567ae6065f0576d79f8837f94087d9`. The unchanged `tests/vgpu/icd.c` hash remains `cc82349ba37c730a5945b3c289173faae14b4809cc93162733a7ab127ce74879`. Toolchain: official Zig 0.13.0 with `/workspace/shared/waddle-tools/env.sh`. Build-only evidence is retained under `build/native-wsi-test-capabilities/`.

- `bash build/native-wsi-test-capabilities/focused.sh`: exit **0**, seven focused WSI tests in Debug and seven in ReleaseSafe, including both formerly failing native tests and all four disabled-mask cases.
- `make vgpu-icd-units`: exit **0**, the complete unfiltered **394 Debug + 394 ReleaseSafe** suites. Both modes also execute the unchanged 128-cycle C native dispatch fixture. No test is skipped or conditionally returned early.
- `make build/vgpu_icd_test build/vgpu_icd_loader_test build/vgpu_icd_mapping_fault_test`, followed by all three executables: exit **0**. Each C fixture passes 128 lifecycle cycles; the loader variant additionally verifies manifest discovery and eight instance/device/queue/fence lifecycles. All affected object/shared-library/executable dependencies were rebuilt after the final fixture refinement.
- `bash build/native-wsi-test-capabilities/cross-link.sh`: exit **0**, complete unfiltered Windows Debug and ReleaseSafe unit executables, both PE32+ x86-64. All 23 workflow C oracle objects and renamed-main C fixture were rebuilt with Windows target, strict warnings, and assertions enabled. The established values oracle and six codec libraries are unchanged dependencies. Linux SDK `CPATH` and `LIBRARY_PATH` are cleared. Only `--test-no-exec` and dedicated output paths differ from runtime execution; these links do **not** execute Windows tests.
- Exact source-boundary audit against `9762151`: restore only `wsi_status_graph_t` and remove only the new disabled-extension test; all remaining bytes are identical. Normalized SHA-256: `9e5dbf27f97937a1d5b1a186151fe6494ee349fe95dd0d11ec8f6da34b022980`. The independent reviewer separately verified all **196 preexisting test bodies** and production `create_swapchain` are byte-identical. Production function SHA-256: `bd1c1b3209ab419678268bcb88b6e97163ed52465f47fb84414531781dcaa34c`.
- Independent source/evidence review approved the final source hash, exact Windows-only named opt-in, Linux zero requested extensions, canonical disabled features, four negative masks, acquired-image and owner conservation, native test-sink export, and every completed receipt above. `git diff --check` passes. No production, original test body, README, dependency, capability, build, coverage threshold, or exclusion-policy change belongs to this commit.

### Sanitizer and native runtime boundaries

The unchanged direct and allocation-fault C sanitizer recipes were extracted from `make -n vgpu-icd-sanitizers` into `c-abi-sanitizer.sh` and `c-mapping-sanitizer.sh` to run these specific relevant fixtures without unrelated prerequisites. Both final-source binaries compile with `-fsanitize=address,leak,undefined` and execute with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`; both terminate with exit **134** when LeakSanitizer reports its fatal ptrace/process-inspection limitation. This occurred both before and after the final fixture refinement. These are **not sanitizer passes or proof of zero leaks**. No sanitizer setting, suppression, security restriction, assertion, or threshold was relaxed. The loader sanitizer executable was rebuilt successfully but not executed after the shared runtime limitation was confirmed. Whole owned-Zig sanitizer instrumentation and the full aggregate were not rerun by this bounded test-fixture task.

Actual native Windows execution on the exact published commit remains the original failure oracle and task #5's pending gate. No Windows runtime, real GPU, guest VM, DXVK, Wayland, or physical presentation acceptance is claimed. Parent owns publication and CI monitoring.

### Coordinated coverage

The coverage-recorder repair worker owns the full coverage validation against the frozen final ICD hash above. This task does not run the moving shared harness or substitute any earlier coverage receipt. The final recorder/gate receipt will be linked when available; its pending status is independent of the completed local unit, ABI, link, and source-preservation checks.

### Final evidence digests

- `focused.log`: `7e4bd9449f875924512586e30c8961ea36825d9c09fdbc0ab6991198bc5f6e53`
- `linux-units.log`: `67423cee0549e7e84ca6955bdc4bd027ae3dda8a860106fff778441b5cc3691f`
- `linux-c-abi.log`: `1fa4903fcc1f2c3fa3dba829e96b99cf337b99f4354d3fbc3a6b1da24ad7a599`
- `windows-cross-links.log`: `006b00250db9643c009aa161f11922d8811bf47998bf73ffec81b3d7199bf6a6`
- `source-boundary-audit.txt`: `7c5e80faacfc77ee1bfdd0620825cb5b60efd09fb9d9683b33f90d3b3a5821c8`
- `linux-c-abi-sanitizer.log`: `13842ea65185acbe0307c970134b3427467564a0eb55c0484ccf751a555bb635`
- `linux-c-mapping-sanitizer.log`: `dff10b7cd8eb2a830d246424bcf6cd6b180a3a96d503ddbe5ba4d402f7531703`
- `linux-loader-sanitizer-build.log`: `f33c39ca3268d3088e4f80e5d0f17bf8a68421e63cd24fbc4d6340c7882b8147`
