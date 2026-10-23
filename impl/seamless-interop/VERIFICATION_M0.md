# Seamless Interop — Milestone 0 Verification

Milestone 0 verifies the **existing** per-PID AV pipeline on real hardware
before seamless-interop changes build on it (TRACKER TODO #2 and #3). Results
are recorded only from real runs; nothing in this file is inferred from CI.

## Prerequisites

1. A device prepared with `waddle av setup --device <name>` (see
   `impl/high-perf-av-passthrough/IMPL_DESC.md`), virtual display driver
   installed in the guest, Windows build ≥ 20348 (per-process loopback).
2. A Wayland session (GNOME and KDE Plasma are both required for TODO #3).
3. Host tools: `python3`, `pw-dump`/`pw-cli` (PipeWire), the Waddle build
   (`make cli av-distribution` or an installed `waddle`).

## Procedure

Audio (TODO #2), 5-minute run:

```bash
scripts/interop_verify_m0.sh audio --device <name> --duration 300
```

The script starts a PowerShell helper in the guest that loops
`C:\Windows\Media\Alarm01.wav` through `System.Media.SoundPlayer`, attaches
`waddle av run <PID>`, then automatically checks:

| Check | Pass rule |
|---|---|
| `guest_tone` | Guest helper reported its PID |
| `pipewire_stream` | PipeWire node `waddle-av-playback` appears within 120 s |
| `stream_running` | Node state `running` in ≥ 95 % of 5-second samples |
| `guest_counters` | Guest `AV audio:` counters present; frames dropped after the first 10 s ≤ 0.1 % (`(duration − 10) × 48` frames) |

and asks the operator two questions (`audible`, `mixer_entry`).

Window (TODO #3), once on GNOME and once on KDE Plasma:

```bash
scripts/interop_verify_m0.sh window --device <name>
```

The script launches Notepad in the guest, attaches `waddle av run`, and asks
the operator the ten questions listed in the results table below.

Paste each run's `summary.md` (printed path) under **Results**, and attach or
reference the evidence directory (`pw-dump-*.json`, `av_run.err`,
`node_states.txt`, `audio_stats.txt`).

## Fixes made for Milestone 0 before the hardware run

| Commit | Change |
|---|---|
| `d5dcc25` | Request client-side decorations through `zxdg_decoration_manager_v1`, so KWin does not draw a second frame around the Windows frame |
| `23dd01f` | Re-issue `xdg_toplevel.set_title` when the guest title changes |
| `fix(av): handle decoration configure events before presenting frames` | Deliver and stage decoration configuration; acknowledge the surface before falling back from a forced server frame; test failure and teardown paths |

## Results

### Audio — TODO #2

| Field | Value |
|---|---|
| Date (UTC) | _not yet run_ |
| Host / desktop | |
| Guest Windows build | |
| Waddle commit | |
| Duration | |
| `guest_tone` | |
| `pipewire_stream` | |
| `stream_running` | |
| `guest_counters` (backlog mean / p95 / max, dropped after warm-up, discontinuities) | |
| `audible` | |
| `mixer_entry` | |
| Optional acoustic latency (method, value) | |
| Verdict | |

### Window — TODO #3, GNOME

| Check | Result | Notes |
|---|---|---|
| `window_visible` (own Windows frame, no second frame) | _not yet run_ | |
| `taskbar_entry` | | Generic icon expected until TODO #28 |
| `alt_tab` | | |
| `click_focus` | | First click may be lost until TODO #16 |
| `typing` | | |
| `title_update` | | Verifies `23dd01f` |
| `compositor_move` | | |
| `compositor_resize` | | |
| `fullscreen` | | GNOME has no default shortcut: expected UNRECORDED |
| `close` | | |
| Verdict | | |

### Window — TODO #3, KDE Plasma

| Check | Result | Notes |
|---|---|---|
| `window_visible` (own Windows frame, no second frame) | _not yet run_ | Verifies `d5dcc25` |
| `taskbar_entry` | | |
| `alt_tab` | | |
| `click_focus` | | |
| `typing` | | |
| `title_update` | | |
| `compositor_move` | | |
| `compositor_resize` | | |
| `fullscreen` | | |
| `close` | | |
| Verdict | | |

## Known gaps at Milestone 0 (not failures of these runs)

- Dragging the Windows caption or edges does not move or resize the window
  (TODO #12); there is no maximize (TODO #13).
- Generic Wayland icon in the taskbar (TODO #28).
- Menus, tooltips and owned dialogs (for example Notepad's Save As) are not
  shown (TODO #10, #11).
- Audio is a single stream named "Waddle game audio" per attached process; the
  per-application streams come with TODO #23.
