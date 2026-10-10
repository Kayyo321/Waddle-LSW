# Hardware testing handoff: shared GPU Windows desktop

Start with the [concise prioritized review checklist](REVIEW_CHECKLIST.md), the current pending-status index. Detailed cases and historical evidence remain below.

Last reviewed: **2026-10-09 UTC**. Branch: `feature/software-vgpu-slicing`.

For fresh GitHub checkouts, use the [published source checkpoint map](CLOUD_PUBLICATION_CHECKPOINT.md)
to translate original local commit IDs in these receipts to their byte-identical published commits.

This is the restart point for testing on a compatible Linux/Windows/RTX 5080
machine. It records runnable repository gates, missing automation, manual cases,
and evidence requirements. **It is a test plan, not a passing hardware report or
permission to deploy, change drivers, mutate a VM, or interrupt another owner.**
Use [the result template](HARDWARE_TEST_RESULT_TEMPLATE.md) for every new run.

## 1. Start here; do not inherit a green status

1. Read [AGENTS.md](../AGENTS.md), [the desktop roadmap](SHARED_GPU_DESKTOP_ROADMAP.md),
   [input contract/tracker](desktop-input/IMPL_DESC.md),
   [WSI contract/tracker](wsi-window-lifetime/IMPL_DESC.md), and the current
   [vGPU tracker](software-vgpu-slicing/TRACKER.md). Each linked feature directory
   contains its own `TRACKER.md`.
2. Record the exact commit you will test, not a moving branch name. Obtain the
   machine owner's approval/lease for the specific Windows session, GPU workload,
   VM/control access, input injection, and any deployment changes. Preserve an
   existing owner's immutable binaries, logs, disk, firmware, TPM and sessions.
3. Start with a fresh, isolated checkout at that commit. Never use `make clean`,
   destructive submodule cleanup, reset, a provisioning script, broad process
   termination, or an in-place rebuild against somebody else's acceptance run.
   Build targets share `build/`; run the phases below serially in that checkout.
4. Classify each result: `verified`, `failed`, `blocked`, `untested`, or
   `not claimed`. Every `verified` needs its own evidence class and immutable
   source/binary manifest. A build is not execution; a fake native boundary is
   not Windows; a software Vulkan backend is not the RTX 5080.
5. After any relevant source, compiler, dependency, DLL, driver, manifest or VM
   image change, rerun affected gates. Previous physical receipts are historical
   evidence only. Do not grant feature-completion credit merely for this guide.

### Current status and exact source checkpoints

The final checkpoint ledger in section 10 identifies the changes documented here.
The cloud development machine cannot establish real Windows input, a live Wayland
compositor, physical GPU, KVM or integrated application behavior. Its passing
portable tests and cross-builds must remain separate from these unrun gates.

- Input scope: physical Linux evdev keys interpreted by the **guest keyboard
  layout**, pointer/buttons/wheel, bounded focus/held-input ownership. Host XKB
  text translation, IME, compositor-generated repeat, clipboard, drag/drop,
  tablets and broad app/dialog-family admission are not implemented by this
  milestone.
- WSI scope: resize/minimize/observed loss/replacement and image retirement in
  the synchronous Win32 sink. It does not connect a guest swapchain directly to
  host Wayland DMA-BUF presentation. Reuse of the same numeric HWND without an
  observed destruction/loss is **unsolved**. Do not label that generation-safe.
- Existing vGPU TODO #3 stays at its recorded partial completion; final patched
  DXVK compiler-lifetime/native-heap qualification is still pending. OpenCL
  TODO #5, D3D12 feasibility, integrated Photoshop/Premiere and simultaneous
  Linux/Windows shared-GPU acceptance stay open independently.
- AV incarnations only distinguish observed tracker retirement/readmission.
  Same-process HWND reuse before a delayed WinEvent retirement is another open
  native identity gap; do not infer that AV solves all HWND recycling.
- A cloud LSan failure under executor ptrace is a **blocked prescribed leak
  gate**, even if the same fixture passes ASan/UBSan with leak detection disabled.
  Do not copy `detect_leaks=0` into an acceptance command.

## 2. Prerequisites and environment receipt

### Build host

Use Linux x86-64; Zig **0.13.0**; GNU make; a C11 compiler; Python 3; Clang/LLVM
19 coverage tools; GCC-backed `cc` and matching `gcov` with JSON output for
the native coverage scripts; `pkg-config`; Wayland client development files and
`wayland-scanner`; PipeWire development files; Wayland protocols containing both
`stable/xdg-shell/xdg-shell.xml` and `stable/linux-dmabuf/linux-dmabuf-v1.xml`.
The tracked package/tool recipes are
[AV CI](../.github/workflows/av_passthrough.yml) and
[vGPU CI](../.github/workflows/software_vgpu.yml). Read them at the tested SHA.
Do not replay their system-ICD manifest edits on a personal machine.

The protocol generator requires **both** `submodules/venus_protocol` and
`submodules/virglrenderer`; without the receiver checkout its header-set mismatch
message can look like a serializer defect. The cloud build used Mako 1.3.8,
MarkupSafe 3.0.2 and PyYAML 6.0.2; record the versions actually used.

Additional vGPU/dependency builds use Meson, Ninja, CMake, Python Mako/YAML,
Vulkan/DRM/Epoxy development packages, and the pinned submodules. DXVK's verifier
also requires `glslangValidator` and MinGW x86-64 GCC/G++. Python must support
`tarfile.extractall(..., filter='data')` for the tracked DXVK builder. Native
SDK capture needs Visual Studio C++ tools and the Windows SDK; Zig cross-building
cannot produce the WinRT projection DLL by itself. `make av-distribution` fails
intentionally without a matching `build/av_wgc.dll`.

In the **new test checkout only**, initialize the recorded dependency pins:

```bash
git submodule update --init --recursive
```

Record tool versions and package versions, all recursive submodule SHAs and
whether any submodule is dirty. Do not silently upgrade a pin to make a gate pass.
The important tracked downstream fixes are
`scripts/patches/kvmfr_capacity.patch`, the Mesa patches under `scripts/patches/`,
and `patches/dxvk/setupapi_device_info_owner.patch`. Record each applied patch
and its hash; pinned DXVK plus the patch is not a pristine upstream DLL.

### Real host/guest

- Linux retains physical GPU/driver ownership. Have a real Wayland session,
  correct `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY`, a connected PipeWire session and
  working graphics-driver/Vulkan installation. Record compositor/version,
  outputs, scale, refresh, color settings, kernel, driver, GPU UUID/PCI identity
  and Vulkan physical-device selection. Do not accept a software fallback.
- A preserved, authorized Windows VM/session has the existing execution bridge,
  signed IVSHMEM and Viosock/VirtIO-FS prerequisites, and a matching host/guest AV
  bundle. The AV mapping is **2 GiB**, with the patched KVMFR 64-bit size ioctl
  when KVMFR is used. Record the real mapping path, guest CID and named device.
  `av-native-kvmfr-test` uses `/dev/kvmfr0` and writes fixture bytes: use an
  exclusively leased, idle test mapping, never a live guest's mapping.
- Windows native input requires an **isolated interactive ordinary desktop**
  at compatible integrity, with no unrelated input workload. `SendInput` is
  desktop-global; foreground checks are not an atomic security boundary.
  Secure desktop, UAC/elevated targets and simultaneous unrelated guest input
  are not qualified. Record Windows build/edition and both host/guest layouts.
- The native capture path needs the matching Windows SDK-built `av_wgc.dll` next
  to its executable. WGC can fall back to WARP; record the selected capture and
  application rendering backends separately. A display probe is not GPU proof.
- A dedicated real GPU/VM supervisor must own process identities and complete
  retirement. Keep tokens, credentials and private configuration out of argv,
  logs, commits and public artifacts. Deployment, driver installs, per-image
  debugger registry changes and control of a running VM need the owner's
  explicit authorization. Never solve an unavailable path by PCI passthrough,
  unbinding the Linux driver or enabling test-signing/security bypasses.

## 3. Save a durable receipt before running

The following Bash is a **documentation logging wrapper**, not a new Waddle CLI.
Run from the repository root after dependencies are present. A clean tree is the
normal acceptance input. Preserve any intentional source changes as a reviewed
commit before testing instead of relying on an untracked machine snapshot.

```bash
set -euo pipefail
umask 077
test -z "$(git status --porcelain)"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$(git rev-parse --short=12 HEAD)"
results="$PWD/build/hardware-tests/$run_id"
mkdir -p "$results/logs" "$results/manifests" "$results/artifacts"
cp impl/HARDWARE_TEST_RESULT_TEMPLATE.md "$results/results.md"
git rev-parse HEAD > "$results/manifests/source-commit.txt"
git status --short > "$results/manifests/worktree-status.txt"
git submodule status --recursive > "$results/manifests/submodules.txt"
git ls-tree -r HEAD > "$results/manifests/source-tree.txt"
uname -a > "$results/manifests/host-kernel.txt"
printf 'gate\tstart_utc\tend_utc\texit_code\n' > "$results/gates.tsv"
run_gate() {
    local gate="$1"; shift
    local start end status
    start="$(date -u +%FT%TZ)"
    printf '%q ' "$@" > "$results/logs/$gate.command"
    printf '\n' >> "$results/logs/$gate.command"
    if "$@" > "$results/logs/$gate.log" 2>&1; then status=0; else status=$?; fi
    end="$(date -u +%FT%TZ)"
    printf '%s\t%s\t%s\t%s\n' "$gate" "$start" "$end" "$status" >> "$results/gates.tsv"
    cat "$results/logs/$gate.log"
    return "$status"
}
```

Use a distinct gate name on every attempt; do not overwrite a failure. Copy the
coverage/sanitizer run directories named by the tools into the run artifacts,
not just the final success line. Preserve normal and instrumented binary hashes,
all loaded Waddle/DXVK/loader/renderer/driver DLL or DSO hashes, shader/input data,
manifest/configuration (redacted), actual commands and exit codes. For AV also
preserve `sha256sums.txt` from the matching bundle. Compare hashes **after transfer
and after execution**, on both machines. Do not hash only the executable while
letting it load an unrecorded DLL from another directory.

Example hash operations, with an actual verified file path filled in:

```bash
sha256sum build/waddle-guest-av.exe build/av_windows_test.exe > "$results/manifests/av-pe-sha256.txt"
```

```powershell
Get-FileHash .\build\av_windows_test.exe -Algorithm SHA256
Get-FileHash .\build\av_wgc.dll -Algorithm SHA256
```

Keep raw local artifacts access-controlled and back them up to an owner-approved
location before changing machines or deleting a build directory. Commit a
sanitized result summary plus immutable artifact references/hashes; ignored
`build/` paths alone are not a portable handoff.

## 4. Rebuild and rerun hardware-independent gates first

These target names and options are defined in `GNUmakefile`, `scripts/av.mk`,
`scripts/vgpu.mk` and `scripts/vgpu_wsi_lifecycle.mk`. They are execution recipes
for the new checkout, **not assertions that every command passed in the cloud**.
Use the `run_gate` helper above, or an equivalent logger that preserves exit status.

```bash
run_gate av-unit make av-test
run_gate av-input make av-input-test
run_gate av-sanitizers make av-sanitizers
run_gate av-coverage make av-coverage
run_gate av-host-build make build/waddle-av-host
run_gate av-windows-build env -u CPATH -u LIBRARY_PATH make av-windows build/av_windows_test.exe build/av_guest_quiescence_test.exe
run_gate vgpu-protocol make vgpu-protocol-test
run_gate vgpu-transport make vgpu-test vgpu-sanitizers vgpu-coverage
run_gate wsi-lifetime make vgpu-wsi-lifecycle-test
run_gate wsi-sanitizers make vgpu-wsi-lifecycle-sanitizers
run_gate wsi-coverage make vgpu-wsi-lifecycle-coverage
run_gate recorder-probes python3 -B tests/av/coverage_recorder_test.py -v
run_gate recorder-sanitizers env WADDLE_RECORDER_SANITIZERS=1 python3 -B tests/av/coverage_recorder_test.py -v
run_gate icd-units make vgpu-icd-units
run_gate icd-regression make vgpu-icd-test vgpu-icd-sanitizers vgpu-icd-coverage
run_gate windows-crossbuild make vgpu-windows build/vgpu_win32_present_test.exe
```

Expected: all commands exit zero; all explicit test assertions hold; the required
production line/branch coverage meets **90% without rounding or exclusions added
for this run**; prescribed ASan/LSan/UBSan and allocator checks report zero leaks
and no errors. These include fake Wayland callbacks and fake Win32 calls on Linux.
Do not count them as live compositor/native SDK acceptance.

The recorder suite currently contains eight tests: ordinary mode runs seven and
skips the sanitizer-only negative control; strict sanitizer mode runs all eight.
The deliberate signed-overflow control must prove fatal UBSan detection, with no
post-error marker, even when inherited options request recovery. Expected negative cases reject
malformed metadata, invalid indices and stale linkage; the test suite itself must
exit zero. Keep leak detection enabled. A ptrace-restricted local sanitizer abort
is a blocked check, never a clean leak receipt. For a failed aggregate, preserve
its exact `build/coverage/**/recorder.json`, `recorder_metadata.h`, `branches.json`,
`test.ll`, `instrumented.ll`, edge output and missed-key reports. These bind the
native compiler target, all raw sites and arities to the tested runtime. Do not
switch CPU targets, drop sites or relax thresholds to obtain a passing result.
[Recorder design and receipts](coverage-recorder-integrity/TRACKER.md) explain the
CPU-dependent historical failure and its all-sites-preserving correction.

The aggregate ICD coverage gate is required even when focused WSI coverage passes.
A historical 375-unit/90.07% receipt belongs to its old source; record current
counts. Historical `vgpu-icd-*-owned-sanitizers`/physical seam tools have fixed
source inventories, unit counts and frozen-runtime assumptions. Do not substitute
an old 117-unit seam receipt for the current integrated gate or relax its guards
when a new source graph is rejected.

If protocol XML lives outside the default system directory, pass the real verified
root using the supported `WaylandProtocolsDir=/absolute/protocol-root` make
variable. Record that root/version/hash and use it consistently. Do not copy XML
from an unrelated unrecorded version just to satisfy a filename.

### Native SDK capture and distribution build

On Windows from the same checked-out SHA, with the native SDK installed:

```bat
scripts\build_wgc.cmd
```

Use the native compilation sequence in the same-SHA AV CI workflow, or transfer
its same-SHA Windows artifact plus source/binary receipt into the new build.
After the matching DLL is present, the Linux packaging commands are:

```bash
run_gate av-package make av-distribution
run_gate av-packaging-regression python3 tests/av/package.py
mkdir "$results/artifacts/av_bundle"
run_gate av-bundle-extract tar -xzf build/waddle_av.tar.gz -C "$results/artifacts/av_bundle"
run_gate av-bundle-integrity sh -c 'cd "$1/waddle_av" && sha256sum --check sha256sums.txt' sh "$results/artifacts/av_bundle"
```

`tests/av/package.py` exercises synthetic temporary fixtures and archive-preservation
failures; it does not inspect the actual distribution archive. The separate
extraction/hash gate above verifies the real `build/waddle_av.tar.gz`. Packaging
builds/downloads the verified signed driver **package**; it does not itself
prove driver deployment or native capture. Replacing the Windows image, drivers,
active bundle or configuration is a separately approved deployment step. A fresh
machine lacking a configured AV device should record `blocked: deployment
prerequisites`, then follow the tracked AV design/setup with its owner. Do not run
`waddle av setup --gpu ...` as a shortcut: that older setup surface includes GPU
assignment paths outside the selected simultaneous-sharing architecture.

## 5. Real Windows and Wayland input acceptance

### A. Native component probes (no guest-to-Wayland claim)

On an isolated Windows desktop with the matching `av_wgc.dll` beside the fixture,
run each explicitly in **PowerShell 7.4 or newer**. This logger records the native exit code before
any subsequent command can replace it; launch errors are logged as
`launch_failure`, and it stops after a failed gate. It is for
the authorized manual desktop run, never automatic global input in normal CI.

```powershell
$native_results = Join-Path (Get-Location) ("build\hardware-tests\native-" + [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ"))
New-Item -ItemType Directory -Path $native_results -ErrorAction Stop | Out-Null
function run_native_gate {
    param([string]$gate, [string]$file, [string[]]$native_arguments = @())
    $started = [DateTime]::UtcNow.ToString("o")
    $log = Join-Path $native_results ($gate + ".log")
    $previous_error_action = $ErrorActionPreference
    $previous_native_error_action = $PSNativeCommandUseErrorActionPreference
    $ErrorActionPreference = "Stop"
    $PSNativeCommandUseErrorActionPreference = $false
    $status = "launch_failure"
    $launch_error = $null
    try {
        ($file + " " + ($native_arguments -join " ")) | Set-Content (Join-Path $native_results ($gate + ".command")) -ErrorAction Stop
        $global:LASTEXITCODE = $null
        & $file @native_arguments *> $log
        if ($null -eq $global:LASTEXITCODE) { throw "No native exit code was produced" }
        $status = $global:LASTEXITCODE
    } catch {
        $launch_error = $_
        $_ | Out-String | Add-Content $log
    } finally {
        try {
            $ended = [DateTime]::UtcNow.ToString("o")
            "$gate`t$started`t$ended`t$status" | Add-Content (Join-Path $native_results "gates.tsv")
        } finally {
            $ErrorActionPreference = $previous_error_action
            $PSNativeCommandUseErrorActionPreference = $previous_native_error_action
        }
    }
    Get-Content $log -ErrorAction Stop
    if ($null -ne $launch_error) { throw $launch_error }
    if ($status -ne 0) { throw "$gate failed with native exit $status; preserve this attempt" }
}
run_native_gate "av-lifecycle" ".\build\av_windows_test.exe"
run_native_gate "av-quiescence" ".\build\av_guest_quiescence_test.exe"
run_native_gate "av-input" ".\build\av_windows_test.exe" @("--input")
run_native_gate "av-capture" ".\build\av_windows_test.exe" @("--capture")
run_native_gate "win32-present" ".\build\vgpu_win32_present_test.exe"
```

- Default AV fixture: tracked ordinary-window discovery/lifecycle and component
  cleanup, including same-numeric-HWND hide/readmit stale-control rejection. It
  deliberately filters its tool window. It is not a dialog-family test.
- Guest quiescence fixture: production guest callback/capture-loop API doubles on
  Windows. It checks terminal failure suppresses later work; it does not capture
  an application or establish physical input/graphics acceptance. Build/copy this
  test executable explicitly; it is not a promised distribution-bundle member.
- `--input`: opt-in real key/pointer/button/wheel injection and foreground-loss
  held-key release. A foreground/integrity restriction is a failed/blocked native
  gate, not grounds to skip the check or elevate the agent automatically.
- `--capture`: real WGC path and newly painted center pixels behind an occluder,
  followed by native audio checks; independent of input and the Vulkan bridge.
  Exact fixture expectations are in `tests/av/windows.c`; preserve that source
  hash and output rather than treating an arbitrary screenshot as the oracle.
- Win32 present fixture: 128 scaled/exact pixel cycles, FIFO/IMMEDIATE selection,
  resize mismatch rejection, and stable GDI/USER counts. It does not drive the
  guest Vulkan queue or a host Wayland window.

In the authorized live Linux Wayland/PipeWire session:

```bash
run_gate native-platform make av-native-platform-test
```

Expect actual attachment/release and PCM callbacks, returned video slots and no
leaked native owners. Its synthetic window uses modern CreateV2 admission and can receive compositor
focus/input requests. Those requests are fixture observations, not injection
into a Windows guest. It does not use a Windows app or establish input round trips.
For an exclusively leased idle `/dev/kvmfr0` only, after confirming its size and
that no guest/other test owns it:

```bash
run_gate native-kvmfr make av-native-kvmfr-test
```

### Generation-safe lifecycle upgrade and regression checks

**Upgrade the host and guest together.** The current guest announces
`MsgWindowCreateV3` in the unchanged 328-byte control envelope. This required
first-window capability is not negotiated fallback. V1 guests (zero/nonzero old
Create token) remain visibly view-only on the new host: no input, resize or close
requests. V2 guests retain their existing generation-safe, fail-closed input
behavior without lease recovery. V2 hosts reject a V3 guest at its first window.
Preserve the diagnostic and prompt termination; waiting before any window exists
is not protocol rejection. The V3 epoch handshake follows the first admitted
window and cannot silently upgrade a V1/V2 session.

The normal `make av-test av-input-test` gates now include the frozen historical
codec/peer compatibility fixture, identity effect gate, stale lifecycle/FIFO and
Wayland buffer-retirement tests. To rerun the built compatibility fixture alone:

```bash
run_gate av-lifecycle-transport ./build/av_lifecycle_transport_test
```

On the authorized real Windows/Wayland setup, repeat these manual observations
with logs and source/binary hashes; no complete physical automation is supplied:

1. Use matching upgraded binaries. Close, resize, minimize and restore several
   ordinary windows; confirm effects stay on the selected incarnation.
2. Hide/readmit an owned window, then repeat destroy/recreate and reconnect.
   The default Windows fixture covers an observed same-value handle case; actual
   delayed WinEvent delivery and real handle recycling still require qualification.
   Reject stale controls without changing the replacement's geometry, focus,
   saved style or retained buffers. Preserve a trace if actual reuse cannot be
   demonstrated rather than marking that case passed.
3. Exercise a busy compositor buffer while its window retires; ensure the old
   lease drains without releasing a replacement's buffer or delivering late
   configure/close callbacks. Check terminal queue failure/disconnect stops
   further capture and mutations and cleanup releases owned resources.
4. If testing mixed binaries is separately approved, use isolated copies of the
   recorded old/new builds. Verify the view-only diagnostic and absent controls
   for new-host/V1-guest; retain V2 generation/input behavior without lease
   recovery; verify first-window rejection for V2-host/V3-guest. Do not downgrade
   or overwrite an active deployment for this probe.

[Implementation/coverage receipt](av-window-generation/TRACKER.md) and
[frozen-source provenance](../tests/av/legacy/PROVENANCE.md) identify the automated
scope. FrameReady still carries its frame counter, not an independent window
generation. Unobserved same-process HWND reuse, frame replay safety and direct
AV-to-WSI/GPU lifetime binding are not solved by this checkpoint.

### Revocable input lease recovery (new native acceptance, still pending)

Use an isolated interactive guest desktop. Foreground checks and hooks cannot
make desktop-global `SendInput` an atomic HWND-bound operation. Unobserved
transitions and concurrent unrelated guest input remain unqualified.

The new supported fixture command is `av_windows_test.exe --lease-fixture`.
Run it in the guest with its console output captured; it prints PID and HWNDs for
ordinary A/B windows plus tool/owned rejection targets, and logs input receipts.
Start the managed AV path below for that exact printed PID. Do not use unrelated
applications as rejection targets containing real work. Close A and B to stop
the fixture; record all session/fixture exit statuses.

[Exact ten-case native trace](input-lease-recovery/NATIVE_ACCEPTANCE.md) covers
startup without ownership, held-release and A/B recovery, same-HWND stale epochs,
observed A→B→A, held-key/button suppression, first click/wheel coordinates,
forbidden foreground targets, anchor destruction, timeout/disconnect and UIPI.
Record each case separately in the result template. Debugger/transport fault
injection is manual and separately authorized; absent evidence stays pending.

Portable checks are already wired into `make av-test av-input-test av-coverage`;
[exact source and local receipts](input-lease-recovery/EVIDENCE.md) distinguish
real callback/API-double tests, cross-links, local LSan limits and physical
acceptance. No Save-dialog/window-family or GPU integration is established.

### B. Integrated ordinary application (manual; no complete automated gate)

1. Select an exact ordinary Windows editor/application build and owned test file.
   Record PID/process identity, integrity, layout and expected saved UTF-8/plain
   text bytes. Use a single-process ordinary top-level window first; a helper
   process or filtered modal dialog may currently block the workflow.
2. Confirm the prepared device and matching deployment. The supported managed
   command is below; replace both values with the actual selected device/PID:

   ```bash
   ./build/waddle av probe --device "$device_name"
   ./build/waddle av run "$guest_process_id" --device "$device_name"
   ```

   `device_name` and `guest_process_id` are shell variables you set from verified
   configuration/session data. The command may start the managed device and owns
   its AV children; use it only in the approved session. Record both readiness and
   exit status. Do not invent a `--shared-gpu`, `--input`, `--headless` or attach flag.
3. Interact through the native Wayland window, not the VM console. Type a fixed
   string, select/edit it with Shift/Ctrl, use arrows and supported extended keys,
   click/drag-select with left/right/middle buttons, scroll both axes, save, close,
   reopen and compare exact file bytes. Keep a screen recording or annotated
   observations connecting each input to the application and saved output.
4. Repeat the focused matrix below. Every row gets its own actual result; do not
   promote the whole milestone because the first line types successfully.

| Case | Procedure and required observation |
| --- | --- |
| Physical layout | Record host and guest layouts. Repeat with a deliberately different guest layout. The guest layout interprets physical evdev scan codes; host-composed text matching is not expected or claimed. Unsupported mappings fail closed. |
| Enter with held keys | Hold Shift/Ctrl in a Linux app, enter the guest surface, then release. No imported press or unintended shortcut; a fresh subsequent press/release works. |
| Focus transfer | Hold a modifier and mouse button; transfer focus to another exported window, then a Linux window. All owned holds release; no input reaches an obsolete incarnation or unrelated guest window. |
| Leave/capability removal | Pointer leave releases buttons; keyboard leave clears all focus/held keys. If the test compositor supports controlled seat capability removal/re-addition, verify fresh proxies and no stuck input; otherwise record that subcase blocked. |
| Guest foreground change | In the isolated test session deliberately change Windows foreground while an owned key is down. The AV session fails closed/releases holds; no further input injection into the new unrelated target. |
| Resize/scale | Resize and move the window, test current pixel bounds at edges and on each selected output/scale. Stale/out-of-bounds coordinates reject instead of targeting another location. Test PMv2 context restoration, mixed guest DPI and negative-origin monitors explicitly. Scale-one behavior alone does not qualify fractional scaling. |
| Destroy/hide/reopen | Retire the focused window while keys/buttons are down. No stuck key/button, no stale input to the replacement. Record HWND and AV Create.sequence where instrumented. AV only covers observed retirement/readmission: same-process reuse before delayed WinEvent delivery remains unresolved, as does the separate WSI reuse gap. |
| Disconnect/reconnect | Terminate only the owned AV session through its approved control path while a key/button is held. Verify release in the guest, then a new session starts with empty state. Record failure/cancellation honestly. |
| Queue pressure | Exercise sustained physical input while rendering. No silent loss of release, unbounded memory or serial reuse; a bounded peer-queue overflow must fail the session and release owned input. No public CLI forces overflow; deterministic pressure/fault injection needs a reviewed fixture. |
| Save/dialog boundary | Complete the intended save workflow in Wayland. If an owned dialog/menu/palette/helper is filtered or cannot receive focus, retain the failure and leave ordinary-app/window-family acceptance open. |

The input wire is fixed 328 bytes, with a 128-frame peer queue; evdev codes are
1..127, pointer bounds 0..8191 and wheel axes -1200..1200. Codec boundary/replay/
malformed tests already exist in the portable gates. Never inject arbitrary raw
frames into a live user's session to reproduce them.

**Acceptance:** correct saved output, correct focus/delivery, no stuck holds,
bounded failure cleanup, native owners retired, and repeatable fresh-session
behavior. IME/composed text, clipboard, drag/drop, host text translation, tablet
and full helper-window policy remain `not claimed`/separate roadmap gates.

## 6. Real WSI lifecycle acceptance

Run section 4's state/native-boundary gates first. The native fixture in section
5A covers only the Win32 sink. The existing real GPU fixture supports normal
`triangle_present` and `triangle_present_smoke` workloads; it does **not** provide
a general CLI for injecting resize/replacement/device-loss transitions.

The following is the required native integration matrix for a reviewed
instrumented fixture or application. If it cannot observe API return values,
acquire signal counts, present wait handling and image/native owner retirement,
record the case blocked rather than judging from a screenshot.

| Case | Required result/evidence |
| --- | --- |
| Normal present | Real acquire, GPU render/readback, exact pixels, present and repeated teardown. Record selected adapter and actual Vulkan calls. Preserve the full 48-frame/eight-lifetime workload as well as smoke runs. |
| Resize before acquire | Mismatch returns `VK_ERROR_OUT_OF_DATE_KHR`; failed acquire does not publish an image index or signal the acquire semaphore/fence. Explicitly recreate the chain; resizing back cannot revive the old out-of-date chain. |
| Resize before/during present | Check before readback, after readback and at the exact-size native sink. No stretching or publication of stale pixels to changed dimensions; report out-of-date and preserve owners for explicit destruction. |
| Minimize/zero area | A valid zero-area window is not surface-lost. Acquire/present and an otherwise-valid nonzero-extent create reject as out-of-date without stale signaling/drawing. A create request whose own image extent is zero fails initial validation with INITIALIZATION_FAILED before old-chain retirement. Restore requires valid replacement; old out-of-date state stays sticky. |
| Observed destruction/loss | Invalid HWND observed by native query produces sticky `VK_ERROR_SURFACE_LOST_KHR`. Later numeric HWND validity cannot resurrect that observed-lost surface. A new surface requires a new identity. |
| Failed replacement | Supply a valid `oldSwapchain`; then exercise recoverable replacement failure (allocation, fixed registry capacity, identity limit or native extent/loss). Old chain is retired for new acquire even when replacement fails; existing image owners stay until explicit destruction. |
| Acquired old images | Present an already acquired image from a retired old chain if surface/extent remain valid; do not retire/free backing before its ownership permits it. Out-of-date/lost cases still reject. |
| Invalid replacement | Wrong-owner/stale/already-retired old handle or malformed request must not mutate an unrelated chain. Failure outputs follow their documented state-module/ICD boundary contracts. |
| Queue and batch semantics | Batch wait semaphores are consumed once before individual presentation. Verify aggregate precedence DEVICE_LOST > SURFACE_LOST > OUT_OF_DATE > SUBOPTIMAL > SUCCESS independent of list order, with per-chain `pResults`. Success/out-of-date/surface-lost release acquisition while retaining backend backing. First local OOM before queue effects stops the batch with unchanged acquisition; local OOM after waits/earlier effects and every ambiguous backend readback failure become DEVICE_LOST and poison the binding. Observe the real wait/signal and owner state; a surface rejection does not cancel submitted GPU work. |
| Error/native ownership | Readback/allocation/GetDC/draw/pacing/release failures leave bounded known owners; attempt ReleaseDC once for every acquired DC and report failure; free temporary buffers, keep backend image/memory owners for explicit teardown, never count forced process termination as normal retirement. |
| Same numeric HWND reuse | Distinguish observed loss followed by reuse (sticky lost is covered) from destruction/recreation between all observations (still unsolved). Without a window-generation/event integration fixture this row remains blocked, even if other lifecycle tests pass. |
| Guest-to-Wayland association | Prove each actual app swapchain maps to the correct exported host window through resize/dialog/multiwindow/replacement. Current Win32 sink and independent DMA-BUF demos do not establish this connection. |

No public switch currently forces allocation/identity exhaustion or in-readback
OS resize. Use a checked-in, reviewed fixture for deterministic faults; do not
weaken assertions, fake native calls, or mutate a live application's handles.

## 7. Physical Vulkan, patched DXVK and native heap gates

### A. Existing reproducible builds

```bash
run_gate renderer-build make vgpu-renderer
run_gate native-loader-build make vgpu-native-loader-win64
run_gate bootstrap-build make vgpu-tcp-bootstrap-windows
run_gate gpu-fixture-build make build/vgpu_tcp_gpu_windows_draft.exe
run_gate dxvk-pin make vgpu-dxvk-pin
run_gate dxvk-client-build make vgpu-dxvk-client-build
run_gate dxvk-fixture-build make vgpu-dxvk-build
```

Preserve `build/dxvk_client.json` and its referenced per-build `provenance.json`;
use their DLL paths/hashes, not an older pristine or private build. The pinned
DXVK source is `c3dd74be6baec53786d4e064a572185b70347a17` (2.7.1); the standard
builder applies the tracked SetupAPI owner fix to a private copy. All nested
source pins and tools are checked by `scripts/verify_dxvk.sh`.

On a leased real GPU, the tracked Linux workload targets include:

```bash
run_gate linux-triangle make vgpu-triangle-worker-test vgpu-triangle-worker-sanitizers
run_gate linux-compute make vgpu-compute-worker-test vgpu-compute-worker-sanitizers
run_gate linux-compute-push make vgpu-compute-push-worker-test vgpu-compute-push-worker-sanitizers
run_gate linux-image-export make vgpu-image-hardware vgpu-image-hardware-sanitizers
run_gate linux-remote-export make vgpu-image-remote-hardware vgpu-image-remote-sanitizers
```

Freeze the actual driver discovery/runtime inventory first and record which
physical adapter each workload selected. The image integration targets use a
mock compositor even when the GPU is real; these are not Windows-to-Wayland
application tests. Vulkan software fallback invalidates an RTX acceptance claim.

### B. Real Windows supervisor is a known automation gap

The historical Windows GPU/DXVK supervisors cited in the old
[handoff](software-vgpu-slicing/HANDOFF.md) are ignored local `build/` artifacts,
not a clean-checkout prerequisite you can reproduce by guessing their paths.
A portable reviewed launcher with private config transfer, exact process identity,
finite nested deadlines and complete retirement proof is still needed if that
supervisor is unavailable. **Keep this gate blocked until it exists or the owner
supplies and reviews a frozen complete runner.** Do not create a replacement VM
or run an undocumented historical script merely because its name appears in a log.

The tracked fixture interfaces are documented here to make that missing piece
precise. These are positional **argument contracts**, not unattended launch recipes:

- `build/vgpu_tcp_gpu_windows_draft.exe`: absolute bootstrap DLL, absolute pinned
  Vulkan loader DLL, absolute ICD JSON, absolute private bootstrap config, fresh
  absolute retirement-proof file, workload, expected device name. Valid workloads:
  `triangle`, `compute`, `compute_push`, `triangle_queries`, `triangle_present`,
  `triangle_present_smoke`. Expected hardware for this project is
  `NVIDIA GeForce RTX 5080`.
- `build/vgpu_dxvk_integration.exe`: absolute pinned `vulkan-1.dll`, patched
  `dxgi.dll`, patched `d3d11.dll`, absolute ICD JSON, absolute bootstrap DLL,
  private config, fresh retirement-proof file. See
  [TCP bootstrap contract](software-vgpu-slicing/TCP_BOOTSTRAP.md) and
  [heap audit contract](software-vgpu-slicing/DXVK_HEAP_AUDIT.md).

A successful supervisor must prove authenticated session identity, exact
controller/worker identities/start times, natural exit/wait/reap and the entire
owned session/process graph empty before accepting retirement. Receipt files
are proof published **after** real retirement; manually creating one to release
a stuck fixture is not valid. Preserve failures and retained owners for diagnosis.
Normal, host-sanitized, local postflush-error, genuine device/transport-loss,
48-frame WSI and native-heap runs are separate rows. A local error with ordinary
retirement is not evidence of genuine device loss. Correct error cases require
the fixture's expected nonzero code plus every stage/cleanup assertion; exit 1
alone does not pass a fault test.

### C. DXVK heap acceptance remains mandatory

Repeat normal/sanitized/fault functional acceptance against the final
compiler-lifetime fixture and patched DLLs, then the external native audit.
Require exact rendering (4096 pixels), compute (64 words), presented pixels and
COM/thread/module retirement. `WADDLE_DXVK_HEAP_AUDIT` selects a fresh absolute
private audit directory; `WADDLE_DXVK_HEAP_CYCLES` accepts 1..8 lifetimes. These
variables do not replace the required stage-aware supervisor.

The signed Microsoft debugger tools, baseline/unloaded markers and continuation
handshake are specified in `DXVK_HEAP_AUDIT.md`. Before any approved per-image
UST/registry change, capture original state and prove exact restoration afterward.
Use exact DLL/PDB/tool hashes and baseline/unload allocation-stack diffs.

The tracked SetupAPI patch removed its identified leak in a historical run;
compiler locale/TLS allocation growth remained. Current affected runs must prove
those owners retire. No inflated handle baseline, blanket OS-cache exclusion,
allocation suppression, manual free of borrowed resources, shorter workload or
successful process exit can substitute for native heap acceptance. Sanitizers
alone do not prove native Windows DLL unloading is leak-free.

## 8. Integrated app, shared GPU and remaining roadmap gates

Run the accepted ordinary-window workflow and real Windows rendering workload
while a separately identified Linux application uses the **same physical RTX
5080 concurrently**. Record both start/end times, adapter UUID/PCI identity,
correct outputs, CPU/GPU/VRAM use and desktop responsiveness over an overlapping
interval. Keep Linux driver ownership. A sequential test, idle compositor,
exclusive passthrough or two different GPUs does not close this gate. Agree load
and measurable responsiveness/resource budgets before the run; no invented
near-native threshold. Hardware/load monitoring is evidence, not backend proof.

Do not lose the remaining roadmap cases just because the two new milestones
pass:

- Reproducible fresh deployment and rollback with exact host/guest/bundle/driver
  manifest; real cross-VM signed-driver/mapping handshake remains distinct from
  independent-process mocks and native shim fixtures.
- Main/helper/modal/menu/palette window families, lost focus, reconnect, multiple
  windows, capacity/VRAM/queue pressure, HiDPI/fractional scale/multi-output,
  color/alpha/ICC/SDR, audio continuity/sync and bounded crash recovery.
- IME/clipboard/drag-drop/tablet input: explicitly unimplemented/unqualified
  portions need implementation and dedicated cases, not an optimistic pass.
- D3D12 feasibility is independent of DXVK D3D11. Pick the exact Photoshop build
  and bounded project/operation; prove actual required API/backend, adapter
  acceptance, GPU-enabled canvas/operation and saved reference output. A launch,
  triangle, CPU fallback or generic GPU load is insufficient.
- OpenCL TODO #5 remains pending; select/pin a policy-compliant implementation
  before claiming a real Windows kernel/filter. Do not quietly add Rust or treat
  an unselected clvk/clspv proposal as an installed dependency.
- Premiere effects, hardware decode and hardware encode are three separate gates
  with a pinned build/media/project/native reference. Vulkan does not establish
  CUDA/NVDEC/NVENC availability. If software codecs are chosen, label them so.
- Latency/high-refresh testing needs the actual selected Windows display mode.
  Supported probes are `waddle-guest-av.exe --probe-display` and
  `av_windows_test.exe --probe-display`; they require 1920x1080 at nominal 144 Hz
  for that distinct performance claim. A failed probe does not by itself refute
  ordinary lower-refresh correctness. `av_windows_test.exe --round-trip-fixture`
  and managed `waddle av run PID --device NAME --latency` are existing bounded
  timing fixtures, not a proof of total input-to-photon latency or Adobe performance.

Use the roadmap's acceptance matrix for every final product claim. Record
`not claimed` with a reason for out-of-scope functionality; keep implementation
or evidence blockers visible instead of silently dropping a row.

## 9. Failure triage and stopping rules

| Symptom | Preserve/check next; do not work around the gate |
| --- | --- |
| Missing protocol XML/tool/header | Record versions and exact missing prerequisite; use approved base packages and the supported protocol-root override. No source-pin drift. |
| LSan reports ptrace/fatal or lacks a report | Reproduce on an untraced compatible test environment. Leak-disabled runs are supplementary only; keep leak gate blocked. |
| Build passes, Windows native fixture fails | Verify same-SHA DLL/executable hashes, architecture, SDK/runtime prerequisites and actual interactive desktop before diagnosing source. Cross-build success is not native acceptance. |
| Input absent/session closes | Preserve host/guest logs; check matching protocol/incarnation, target PID, interactive integrity, foreground restrictions, key mapping, pixel bounds, seat focus and bounded queue failure. Do not bypass UIPI or discard release errors. |
| Stuck key/button after failure | Stop input tests; isolate the guest; record exact held event/focus/disconnect sequence and release failure. User/manual recovery does not qualify automatic cleanup. |
| WSI out-of-date/lost | Record dimensions, HWND/surface/swapchain IDs, acquire/present results, wait/signal observations and replacement sequence. No stretching stale backing or reviving sticky old state. |
| GPU fixture hangs/loses ownership proof | Retain modules/callbacks/process evidence until an authorized exact-owner supervisor resolves it. No guessed retirement Ack or killing unrelated processes. Timeout/cancellation remains a failure. |
| Functional DXVK pass with heap growth | Preserve all stacks, marker records, binaries/PDBs and per-lifetime baselines; attribute/release owners. Do not lower cycles or suppress survivors. |
| GPU identity/fallback mismatch | Fail the physical row; inspect recorded loader/manifest/driver selection. Software Vulkan/WARP results remain separately useful. |
| Wrong/missing modal window or save output | Keep integrated app gate open even when ordinary surface input works. Record the actual owning process/window family and admission decision. |
| CI green on another SHA | Run/check required jobs for the exact frozen commit. Historical CI or local source changes do not transfer acceptance. |

Stop immediately for unowned process/device access, lost process ownership,
unexpected driver/security changes, corrupted output, stuck input or unexplained
resource growth. Preserve diagnostic evidence, mark the gate failed/blocked,
and obtain the smallest missing authorization or implementation fix. Do not mark
this feature complete or merge while its existing required gates are pending.

## 10. Checkpoint and validation ledger

### Input source and measured cloud scope

- `0df71a8ab05ee23143856de435d9f1574674d445`: input contract.
- `1585dac8a4b628febb2980daf3cb405e939c7943`: codec and bounded input state.
- `789460882284c793ecd09cab159ca04a78f7544c`: Wayland/guest integration.
- `55d24fbb76cf316cd3bcb46e0f81c62aa8c52511`: input CI wiring only.
- `7ac4306315bb620f149d4add110d842248c3fe97`: native DPI assertions and evidence.
- `5f3a88444724a8f91475a626a6494cb33223b229`: correct measured GCC version.

[Input EVIDENCE.md](desktop-input/EVIDENCE.md) records the actual source-scoped
commands, SDK override and results: `av-test`, `av-input-test`, host link and
Windows guest/fixture cross-build passed. Portable core coverage is 78/78 lines,
104/104 branches; codec 83/83 lines, 144/149 branches. Required leak-enabled
input sanitizer execution was blocked by ptrace; supplementary ASan/UBSan passes
do not close it. Native Windows, Wayland and integrated workflow remain unrun.

### WSI source and measured cloud scope

- `5be8152610619c49578f89581370d65ba0146181`: production WSI lifetime/queue
  semantics, exact-size Win32 sink and portable/native-fixture coverage.
- `57b07f629b9836b4260be1480ce365b6b3116da2`: WSI Linux/Windows CI gates and
  native ICD oracle/sink/link/manifest parity. This is CI **wiring**, not an
  executed CI/native acceptance receipt.
- `908f83b919695eb79ed10eb31480be35fc662c8f`: explicit Linux embedded ICD
  units and native Windows ReleaseSafe execution alongside Debug. The public
  `vgpu-icd-test` target does not include the embedded queue-regression suite.
  This CI wiring does not imply that a native run has completed.

The [WSI tracker](wsi-window-lifetime/TRACKER.md) records commands and limitations.
`make vgpu-wsi-lifecycle-test` passed 12 Debug and 12 ReleaseSafe cases plus
portable C API-double/unsupported-platform tests. Production WSI coverage:
148/149 lines (99.33%) and 145/156 branches (92.95%); native C source with test
doubles: 47/47 lines (100%) and 71/72 branches (98.61%). The Win32 sink and Windows
state fixtures cross-linked. `make vgpu-icd-units` passed 386 Debug and 386
ReleaseSafe cases. The prescribed LSan runs remain blocked by ptrace; Windows
native execution, actual GPU lifecycle and the final integrated product are unrun.
The reviewed attribution checkpoint is
`d1bb0ea972f56b5441e4a791ec2635849988b69c`
(`docs(vgpu): record reviewed WSI lifetime verification`).

### Validation of this guide

All documented literal make targets and fixture options were checked against
tracked recipes/source; local Markdown links resolve. Eleven Bash blocks passed
`bash -n`. The logging helper was exercised only with harmless exit-0/exit-7
commands: it preserved both statuses and stopped after failure. The PowerShell
wrapper was source-reviewed but not executed here; native command errors and
logging failures must stop the run. No hardware/native test, deployment, VM
mutation, driver change or actual artifact publication was performed by this
documentation task.

This documentation itself adds **zero feature-completion credit**. Its own commit
is discoverable without a self-referential hash:

```bash
git log -1 --format='%H %s' -- impl/HARDWARE_TEST_HANDOFF.md
```
