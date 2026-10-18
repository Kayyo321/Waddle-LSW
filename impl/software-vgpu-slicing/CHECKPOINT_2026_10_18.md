# User-requested qualification checkpoint — 2026-10-18T01:25:19Z

Work is paused at the user's request to find a good stopping point. This is a
recoverable checkpoint, not qualification closure. Feature85.5%, WSI94%, both
Time Ended fields remain TBD. README is unchanged. Do not merge or mark the PR ready.

## Checkout and ownership

- Branch: `feature/software-vgpu-slicing`.
- HEAD before this documentation-only checkpoint: `50d5da28673733453ca304f2636d60db47b07700`.
  The checkpoint commit is identifiable by `docs(vgpu): checkpoint qualification at user pause`.
- All completed implementation/tests are committed. Generated Python caches were
  preserved under ignored build artifacts. Lead alone owns Git and VM/GPU runs.
- All three GPT-6 Luna high-effort workers completed and relinquished their files.
- Claude and its workers had already relinquished the checkout by explicit user confirmation.
- User approvals for the documented private clvk shutdown and buffer corrections
  persist. Do not ask again for those approved corrections. Submodules/pins are unchanged.

## Completed evidence

- Current owned ICD:394 Debug/ReleaseSafe tests,128 native C cycles, allocator
  and prescribed sanitizer gates. Branches90.32%, lines98.53%.
- Complete37-source owned-access proof and physical RTX5080 matrix pass:
  144 positive GPU lifetimes plus6 exact expected postflush cleanup failures,
  zero configured sanitizer/leak errors. See PHYSICAL_RECEIPT_2026_10_18.json.
- Private clvk patches0001/0002/0003 pass19 admission tests with actual13-body
  ASan/LSan/UBSan regressions and both negative controls. Production buffer
  coverage161/161 lines,200/212 directional outcomes (94.34%), no exclusions.
- Final three-patch DLL cross-build/export/import admission passes. Receipt
  `build/clvk_client.json`; log `clvk_buffer_map_final_cross_build.log`.
- OpenCL final native normal `dxvk_gpu_d0fc65d0` and sanitized-host
  `dxvk_gpu_ff4d1413` both pass8 primary exact kernel lifetimes, temporary
  malformed/context/alias/event scopes, deliberate build error, joined concurrent
  callers, terminal shutdown and native module retirement. Host graphs retire
  naturally with no signals; six guest artifacts and fixture manifests match
  after both runs. Native heap and simultaneous Linux GPU gates remain open.
- Native bounded WSI `tcp_gpu_683c0ee2` passes8 lifetimes/32 cases/24 exact
  presentations with original handle199 baseline and GDI/USER equality.
- Loader environment guard: native round-trip and mock ASan/LSan/UBSan pass;
  production84/84 lines and74/74 branches. OpenCL and DXVK restore environment
  after joined thread/TLS/module retirement; retries retain failure status.
- Fresh post-stdio DXVK heap `dxvk_heap_d79f3c51503adc22`:3 exact GPU lifetimes,
  native exit0, tool/image/PDB/registry evidence intact. The4096-byte stderr
  bucket is gone. Heap acceptance remains false; positive allocations remain.
- Four system-only UMDH controls each pass3 joined lifetimes. Exact narrow
  matches support some GUI/MSCTF/DXCore, compiler locale and direct Winsock/CNG
  ancestry. No totals were subtracted and unmatched paths remain unresolved.

## Open gates and next work

1. Unchanged48-frame presentation `tcp_gpu_7adb6000` fails after42 exact frames:
   handle baseline199, actual200. Host and worker retire naturally. This is
   failed evidence. Older diagnostic traces use another fixture and cannot
   attribute the current extra handle. Current-source diagnostic checkpoint
   `tcp_gpu_fe13e873` is prepared but NOT executed. Runner:
   `build/qualification_20261017/run_current_wsi_diagnostic.py`.
   It preserves assertions and adds a60-second inspection hold; NEVER count its
   result as acceptance. Do not weaken assertions, inflate baselines or widen budgets.
2. Finish DXVK heap attribution. New control results:
   `build/qualification_20261017/system_heap_control_results.json`.
   Exact matches do not waive remaining environment-size mismatch, bootstrap
   FIPS/timer, ETW/threadpool, DXGI/DirectXDatabaseHelper/WNF/RPC and Present/WinTrust
   stacks. Current native positive heap deltas are not zero-owned-leak proof.
3. Review the NOT EXECUTED OpenCL heap tool:
   `build/qualification_20261017/opencl_heap_tools/opencl_heap_audit.py`.
   It has separate `prepare --cycles 3` and `run <folder>` commands. Worker AST,
   source hashes and PE/PDB GUID/age checks passed; lead execution/review remains.
   Freeze actual live module bases for post-unload symbolization. Require exact
   original UST restoration and tool/binary hashes. Keep heap_acceptance false
   until every relevant owned allocation is proved released.
4. Run simultaneous Linux GPU work during actual OpenCL execution, recording
   real overlap, exact GPU outputs, sanitizer results and natural owner cleanup.
   Ordinary OpenCL context concurrency already passed; that is a different gate.
5. Assess/add actual OpenCL transport-loss/cancellation acceptance. Existing
   portable protocol and DXVK fault tests do not by themselves verify every
   clvk consumer failure path. No OpenCL-specific transport-loss fixture has
   been added at this checkpoint.
6. Reconcile applicable final CI/input hashes, current source documentation and
   progress only after all gates pass; then Time Ended and PR update. Do not merge.

## Live state and resumption

- All local GPU/native/build jobs have finished. No local test is intentionally
  left running. All image-specific UST registry changes were restored by each audit.
- The original Windows VM remains running at PID10895; disk/firmware/TPM and
  settings are preserved. Never provision/reset it. Credentials remain only in
  the existing private credentials JSON, never in logs or command arguments.
- Stopped only our verified localhost publishing servers PID15253/:55986 and
  PID65136/:37337 after transfers finished. Their files remain. On resume,
  explicitly start `python3 -B build/qualification_20261017/public_binaries_resume.py`
  and `python3 -B build/qualification_20261017/public_pdbs_resume.py` as owned jobs
  before any new deployment. The PDB restart rewrites its PID/port record.
- Prior source snapshots, failed runs, VM artifacts, control binaries/PDBs and
  logs remain intact. Root artifact directory: `build/qualification_20261017`.
- Draft PR6 already exists and is attached to this chat. No readiness/metadata
  update or feature-branch push occurred during this checkpoint interval.
  Feature PR must remain unqualified while these gates are open.
- Separate CI-only ref `codex/vgpu-qualification-20261017-a42994ee` points to
  `1e91c4833a285c7978afa0d50eaa4035f983cce7`, with no PR. All four earlier ddc7926
  workflows passed. New runs: software39085524347, CLI39085525928,
  guest39085527458, AV39085528946. Last saved observation: guest/AV success,
  CLI/software still running (software OpenCL admission success). Leave remote
  CI running; inspect once on resume. No follow-up automation was created.
