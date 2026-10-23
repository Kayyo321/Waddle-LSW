# clvk runtime correction proposal (v1 snapshot, superseded by v2 work)

These files are **evidence of a reviewed proposal, not applied code**. Nothing here is wired into the build.

| File | Meaning |
|:--|:--|
| `proposal-v1-runtime-tls-win32.diff` | The v1 proposal exactly as reviewed (sha256 `da538045da187c526cf3fc60588265230fc6378a9310992a9ecd7369849bf898`; 8 files, +1408/-20; `git apply --check` passes on HEAD `52a1a29`). Adds `src/vgpu/clvk_runtime_tls.c` (replacement for the libgcc_eh `emutls.o` and `gthr-win32-thread.o` members), its Linux fake-Windows tests and coverage gate, the Win32-thread-model switch and three post-link gates in `scripts/build_clvk_windows.py`, fixture rules in `scripts/vgpu_dxvk.mk`, driver tests in `tests/vgpu/clvk_build.py` and CI steps. |
| `proposal-v1-optional-probe-coverage.diff` | Optional stronger isolated probe (initialized `thread_local`, caught exception, `std::thread::id` identity). Used for the extended native run. |
| `approval_request_v1.md` | The approval packet drafted for the human. The human approved the **shim approach with a hardened v2** on 2026-10-21; see below. |

## Approval record (2026-10-21, given in the session by the user)

Approved: Win32 thread model; Waddle-authored C runtime shim replacing two libgcc_eh members; release of the libstdc++ emergency-exception pool at DLL teardown; hardened fail-closed build gates; integration as reviewed atomic commits, real-DLL rebuild and native OpenCL qualification with new TLS and heap oracles.
Not approved: any clvk/LLVM/clspv/SPIRV source change, any submodule pin or content change, `patches/clvk/*` changes, host package installs, README edits. The Win32 compiler stays the hash-verified private extraction on `PATH`.

## Why v1 is not integrated

Two independent reviews (see `../CLVK_RUNTIME_PROPOSAL_REVIEWS_2026_10_21.md`) found the mechanism sound but v1 not zero-leak: unfreed 0x16000-byte libstdc++ emergency pool per load, unmeasured loader retention of the 608-byte per-thread TLS template, a blind heap oracle, fail-open build gates (12 of 30 driver mutants and 4 of 27 shim mutants survive), missing IMPL_DESC/TRACKER entries and an unconditional POSIX removal. The v2 work list is in `../CHECKPOINT_2026_10_21.md`.

## Resuming

`git apply` the v1 diff in a scratch clone to recreate the reviewed state, or start from the four new files it adds; v2 edits them (`src/vgpu/clvk_runtime_tls.c`, `tests/vgpu/clvk_runtime_tls_test.c`, `tests/vgpu/clvk_runtime_tls_fake/windows.h`, `scripts/clvk_runtime_tls_coverage.py`) and adds `src/vgpu/clvk_runtime_eh_pool.c`. A working-tree copy of the four v1 files is kept under `build/qualification_20261020/clvk_candidate/v1_working_tree_copy/`.
