# Feature Tracker: Seamless Interop

- **Contributors / Agents**: Claude Code (cloud session)
- **Time Started**: 2026-10-22T00:59:49Z
- **Time Ended**: TBD
- **Feature Branch**: feat/seamless-interop
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

Base: `feature/software-vgpu-slicing` at `8a6e25e`. Design and frozen contracts:
[IMPL_DESC.md](IMPL_DESC.md). Manual evidence: [VERIFICATION_M0.md](VERIFICATION_M0.md)
(created by TODO #2).

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1  | IMPL_DESC.md + TRACKER.md, contract freeze (protocol v1, layout v2, manifest schema, config keys) | Done | 2% | 100% | Contracts frozen in IMPL_DESC §3 |
| #2  | Verify audio passthrough on the existing pipeline: evidence script + manual run recorded in VERIFICATION_M0.md | Pending | 3% | 0% | Manual hardware run by user |
| #3  | Verify a guest window appears like a native window on GNOME + KDE; fix defects found (title updates, CSD negotiation) | Pending | 3% | 0% | Manual hardware run by user |
| #4  | Interop protocol v1 header + Zig codec, Hello/capabilities, chunked transfers | Pending | 4% | 0% | - |
| #5  | IVSHMEM layout v2 (slot heap, 64 windows, 8 audio rings) | Pending | 3% | 0% | Waits on #4 |
| #6  | Guest session agent skeleton, guest-local code signing + UIAccess install, logon autostart, versioned deploy/update | Pending | 4% | 0% | Waits on #4 |
| #7  | `waddle-session` host client, module extraction from av_wayland.c, singleton, reconnect, autostart | Pending | 4% | 0% | Waits on #4, #5 |
| #8  | VM devices: explicit network + CUPS guestfwd, qemu-xhci, PipeWire audiodev, QEMU configure flags, new config keys | Pending | 3% | 0% | - |
| #9  | Export all virtiofs mounts + root path fallback fix | Pending | 1% | 0% | - |
| #10 | Session-wide window tracking, classification, ownership graph, generation ids | Pending | 4% | 0% | Waits on #6 |
| #11 | Host toplevel/popup/parent mapping, positioner, title/app_id updates, decoration negotiation | Pending | 4% | 0% | Waits on #7, #10 |
| #12 | Move/resize via WM_NCHITTEST interception, caption double-click maximize | Pending | 3% | 0% | Waits on #11 |
| #13 | Minimize/maximize/fullscreen/restore/close state machine | Pending | 3% | 0% | Waits on #11 |
| #14 | Guest display sizing, fractional scale/DPI, multi-output | Pending | 3% | 0% | Waits on #11 |
| #15 | Keyboard completeness (full scancode table, host repeat, lock sync, layout sync, shortcut inhibit) | Pending | 3% | 0% | Waits on #7 |
| #16 | Pointer completeness (XBUTTONs, value120, activating click) + cursor shape/image sync | Pending | 3% | 0% | Waits on #7, #10 |
| #17 | Relative pointer & pointer lock | Pending | 2% | 0% | Waits on #16 |
| #18 | Touch + pen/tablet via synthetic pointer devices | Pending | 3% | 0% | Waits on #16 |
| #19 | Pinch gesture mapping | Pending | 1% | 0% | Waits on #16 |
| #20 | IME (guest default, host text-input-v3 mode with guest-drawn preedit) | Pending | 3% | 0% | Waits on #15 |
| #21 | Clipboard both directions (text, HTML, RTF, images, files incl. cut) | Pending | 4% | 0% | Waits on #7, #9 |
| #22 | Drag and drop both directions | Pending | 3% | 0% | Waits on #21 |
| #23 | Microphone (PipeWire audiodev + discard sink) + per-app audio streams | Pending | 3% | 0% | Waits on #5, #8 |
| #24 | USB redirection (CLI, daemon QMP, polkit ACL grant) for webcams, wired gamepads, other USB | Pending | 3% | 0% | Waits on #8 |
| #25 | Bluetooth/evdev gamepads via virtual XInput | Pending | 2% | 0% | **Dependency approval gate** (ViGEmBus); IMPL_DESC §3.18 |
| #26 | Printing via IPP guestfwd to host CUPS | Pending | 2% | 0% | Waits on #8 |
| #27 | Guest app catalog, icon extraction (PNG), change notifications | Pending | 4% | 0% | Waits on #6 |
| #28 | Host desktop-entry/icon/menu materialization, app_id matching, toplevel icon, desktop-shortcut mirroring | Pending | 4% | 0% | Waits on #27, #33 |
| #29 | `waddle open` launch path, activation tokens, cold-boot feedback, File Explorer seed | Pending | 3% | 0% | Waits on #7, #27 |
| #30 | Installer flow: UAC modes window/auto/native, secure-desktop safety net, elevated windows, auto entries on install/uninstall | Pending | 3% | 0% | Waits on #6, #28, #29 |
| #31 | .exe/.msi/.lnk MIME handler, PE classifier, `exe_handler` setting | Pending | 2% | 0% | Waits on #29, #33 |
| #32 | binfmt_misc system artifacts, `install --system`, status/conflict reporting | Pending | 1% | 0% | Waits on #31 |
| #33 | Integration manifest + install/remove/repair/status/migrate, TryExec self-hiding | Pending | 3% | 0% | Waits on #1 |
| #34 | End-to-end acceptance matrix (GNOME/KDE/Sway), CI workflow, coverage/sanitizer gates | Pending | 4% | 0% | Waits on all |

**Total Feature Completion**: `2.0%`

## Commit History & Progress Log

- **Commit `a2f99c9`**: `docs(seamless-interop): freeze interop contracts and design`
  - **Task Impact**: +80% to TODO: #1 (+1.6% overall feature completion)
  - **Summary**: Adds IMPL_DESC.md with scope, recorded user decisions, frozen
    interop protocol v1, IVSHMEM layout v2, daemon/config/manifest contracts,
    peripheral and desktop-integration mechanisms, execution sequences,
    threading, failure modes and verification criteria.

- **Commit `HEAD`**: `docs(seamless-interop): add feature tracker`
  - **Task Impact**: +20% to TODO: #1 (+0.4% overall feature completion)
  - **Summary**: Adds this tracker with 34 weighted tasks (sum 100%),
    dependencies and blockers. `HEAD` denotes the containing commit until
    resolved by the next atomic commit.
