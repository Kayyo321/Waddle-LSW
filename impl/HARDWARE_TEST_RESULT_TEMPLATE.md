# Hardware test result: FILL IN run ID

Copy into a new private run directory before testing. Do not replace an older
result. No row starts verified. Source guide: `impl/HARDWARE_TEST_HANDOFF.md`
in the recorded repository commit (the original template also sits beside it).

## Identity and scope

- UTC start/end:
- Operator/reviewer:
- Exact repository commit and branch:
- Clean source/submodule status; source-tree/pin manifest and hashes:
- Purpose and acceptance boundaries:
- Authorized machine/session/device/input/deployment scope and owner lease:
- Previous accepted baseline (historical only):
- Changes since baseline; gates invalidated:

## Reproduction prerequisites

- Host OS/kernel/architecture, compiler/Zig/Python/LLVM/build-tool versions:
- Wayland compositor/version, protocol package, session/output/scale/refresh:
- PipeWire version/session:
- Physical GPU name, UUID/PCI identity, driver, Vulkan device/profile:
- Guest Windows build/edition/image, virtual display mode and guest drivers:
- Preserved VM disk/firmware/TPM identities and configuration receipt (no secrets):
- AV bundle source/hash, active deployment identity, device/CID/mapping:
- Host/guest keyboard layouts; integrity and isolated input-desktop conditions:
- App exact version/PID/process identity and input-project/media hashes:
- Expected output/tolerance and native-reference evidence fixed before test:
- Exact dependencies, downstream patches, loader/ICD/renderer/driver inventory:
- Normal/instrumented EXE/DLL/DSO/shader manifests and hashes before/after transfer:
- Native supervisor source/hash, private config transfer and retirement mechanism:
- Missing prerequisites and why:

## Gate ledger

Statuses: verified / failed / blocked / untested / not claimed. Evidence classes:
CPU/mock, cross-build, native Windows component, live Wayland component, Linux
physical GPU, Windows physical GPU, integrated app, shared GPU. Do not combine.

| Gate | Evidence class | Status | Command/manual case + log/artifact/hash | Actual result, exit code and blocker |
| --- | --- | --- | --- | --- |
| Source/dependency/build freeze | build | untested | | |
| AV portable input/codec/Wayland callbacks | CPU/mock | untested | | |
| Generation identity, stale lifecycle and frozen legacy compatibility | CPU/mock | untested | | |
| AV prescribed sanitizers + >=90% coverage | CPU/mock | untested | | |
| WSI D/RS + fake native boundary | CPU/mock | untested | | |
| WSI prescribed sanitizers + >=90% coverage | CPU/mock | untested | | |
| Full ICD regression/sanitizers/coverage | CPU/mock | untested | | |
| Matching Windows/SDK/distribution build | cross-build/native build | untested | | |
| Native AV lifecycle and observed hide/readmit controls | native Windows component | untested | | |
| Production guest failure quiescence API-double fixture | native Windows API-double | untested | | |
| Coordinated upgrade / approved mixed-version diagnostics | integrated compatibility | untested | | |
| Native --input and foreground-loss release | native Windows component | untested | | |
| WGC capture/backend | native Windows component | untested | | |
| Win32 exact present/resize/native owners | native Windows component | untested | | |
| Real Wayland/PipeWire attachment/release | live Wayland component | untested | | |
| Real exclusive idle KVMFR mapping | Linux native driver | untested | | |
| Guest layout + all input matrix rows | integrated app | untested | | |
| Focus/held release/queue/disconnect recovery | integrated app | untested | | |
| Save/close/reopen + exact output | integrated app | untested | | |
| Resize/minimize/loss/replacement/acquired-old | Windows physical GPU | untested | | |
| Queue waits/results/loss/native retirement | Windows physical GPU | untested | | |
| Unobserved same-value HWND reuse in WSI | integrated lifetime | blocked | | Requires window-generation/event integration |
| Same-process AV HWND reuse before delayed retirement | integrated lifetime | blocked | | Native identity qualification gap |
| Guest swapchain to correct Wayland surface | integrated app | untested | | |
| Real triangle/compute/push + normal/sanitized/error | Windows physical GPU | untested | | |
| Full 48-frame/eight-lifetime WSI | Windows physical GPU | untested | | |
| Patched DXVK normal/sanitized/fault | Windows physical GPU | untested | | |
| Final compiler-lifetime native heap audit | Windows native heap | untested | | |
| Simultaneous correct Linux/Windows work on RTX 5080 | shared GPU | untested | | |
| Fresh deployment and rollback | integrated deployment | untested | | |
| Window family/modal/menu/helper policy | integrated app | untested | | |
| IME/clipboard/drag-drop/tablet | integrated app | not claimed | | Not supplied by current input milestone |
| D3D12/Photoshop chosen workflow | integrated app | untested | | |
| OpenCL real kernel + selected filter | Windows physical GPU | blocked | | TODO #5 implementation/selection pending |
| Premiere effects | integrated app | untested | | |
| Premiere hardware decode | integrated app | untested | | |
| Premiere hardware encode | integrated app | untested | | |
| Latency/color/scale/audio/resource/recovery budgets | integrated app | untested | | |

Add one row per individual manual case and failed attempt. A nonzero expected
fault exit is verified only with its complete required stage/retirement evidence.

## Observed ownership and cleanup

- Input held keys/buttons/focus empty after every relevant exit:
- Surface/swapchain/image/allocation/DC/GDI/USER/FD/handle accounting:
- Acquire signaling and present waits/results under each error:
- Native/project/DXVK DLL absence and allocation-stack differences:
- Exact controller/worker identity, session, wait/reap, natural exit and empty graph:
- Any retained owners/forced cancellation/manual recovery; why not a normal pass:
- Registry/configuration/deployment changes and exact restoration receipt:
- Sanitizer scope (what is actually instrumented); raw outputs and zero-leak result:
- Coverage numerator/denominator, measured production scope and threshold:

## Shared GPU and application evidence

- Linux workload/version/command/output and overlapping UTC interval:
- Windows workload/version/command/output and overlapping UTC interval:
- Same physical GPU identity proof; fallback/API/backend evidence for each:
- Predeclared latency/responsiveness/resource budgets and actual samples:
- Saved outputs/reference hashes and comparisons:
- Separate effects/decode/encode or rendering/compute/capture backend findings:

## Durable artifact inventory and disposition

- Access-controlled artifact location, manifest hash and authorized readers:
- Raw stdout/stderr/commands/exit codes, coverage, sanitizer and heap reports:
- Source and all actual loaded binary/dependency/patch hashes:
- Screenshots/recordings and exact input/output fixtures:
- Failures preserved and changed-source rerun references:
- Verified gates (narrow claims only):
- Remaining failed/blocked/untested/not-claimed gates:
- Next concrete action, owner and prerequisite:
- Reviewer sign-off and UTC:

Do not store credentials, capability tokens, private configuration bytes or
unredacted unrelated personal data in this report or a public artifact.
