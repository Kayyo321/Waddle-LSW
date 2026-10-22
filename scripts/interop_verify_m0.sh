#!/usr/bin/env bash
# interop_verify_m0.sh - Collect seamless-interop milestone 0 evidence on real hardware.
#
# Runs on the Linux host inside a Wayland session with a device already prepared
# by `waddle av setup`. It exercises the EXISTING per-PID AV pipeline:
#   audio   - guest audio passthrough (TODO #2 of impl/seamless-interop/TRACKER.md)
#   window  - guest window behaves like a native window (TODO #3)
# Every run writes raw evidence plus a Markdown summary into an output directory;
# paste the summary into impl/seamless-interop/VERIFICATION_M0.md.
#
# Usage:
#   scripts/interop_verify_m0.sh audio  [--device NAME] [--duration SECONDS] [--out DIR] [--non-interactive]
#   scripts/interop_verify_m0.sh window [--device NAME] [--out DIR] [--non-interactive]
#
# Environment:
#   WADDLE                   waddle CLI to use (default: build/waddle in this repo, else PATH)
#   WADDLE_M0_STREAM_WAIT    seconds to wait for the PipeWire stream to appear (default 120)
#
# Exit codes: 0 all automatic checks passed, 1 an automatic check failed,
#             2 usage error, 3 missing prerequisite.
set -u -o pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$script_dir/.." && pwd)

usage() {
    sed -n '11,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

mode=${1:-}
[[ "$mode" == audio || "$mode" == window ]] || usage
shift
device=""
duration=300
out_dir=""
interactive=1
while (($#)); do
    case "$1" in
        --device|-d) device=${2:-}; [[ -n "$device" ]] || usage; shift 2 ;;
        --duration)
            duration=${2:-}
            if ! [[ "$duration" =~ ^[0-9]+$ ]] || ((duration < 30)); then usage; fi
            shift 2 ;;
        --out) out_dir=${2:-}; [[ -n "$out_dir" ]] || usage; shift 2 ;;
        --non-interactive) interactive=0; shift ;;
        -h|--help) usage ;;
        *) usage ;;
    esac
done

if [[ -n "${WADDLE:-}" ]]; then waddle=$WADDLE
elif [[ -x "$repo_dir/build/waddle" ]]; then waddle=$repo_dir/build/waddle
else waddle=$(command -v waddle || true)
fi
[[ -n "$waddle" && -x "$waddle" ]] || { echo "interop_verify_m0: waddle CLI not found (set WADDLE)" >&2; exit 3; }
for tool in python3 pw-dump; do
    command -v "$tool" >/dev/null || { echo "interop_verify_m0: missing prerequisite: $tool" >&2; exit 3; }
done
[[ -n "${WAYLAND_DISPLAY:-}" ]] || { echo "interop_verify_m0: run inside a Wayland session (WAYLAND_DISPLAY unset)" >&2; exit 3; }

stamp=$(date -u +%Y%m%dT%H%M%SZ)
[[ -n "$out_dir" ]] || out_dir="$PWD/waddle-m0-$mode-$stamp"
mkdir -p "$out_dir" || exit 3
summary="$out_dir/summary.md"
device_args=()
[[ -n "$device" ]] && device_args=(--device "$device")

exec_pid=""
av_pid=""
stop_child() { # stop_child PID: SIGTERM (handled by waddle like Ctrl+C), SIGKILL after 15 s
    local child=$1
    [[ -n "$child" ]] || return 0
    kill -TERM "$child" 2>/dev/null || return 0
    for _ in $(seq 1 150); do
        kill -0 "$child" 2>/dev/null || { wait "$child" 2>/dev/null; return 0; }
        sleep 0.1
    done
    kill -KILL "$child" 2>/dev/null
    wait "$child" 2>/dev/null
}
cleanup() {
    # The AV session owns only its exec bridge; stop it first, then the guest helper.
    # Background jobs of a non-interactive shell ignore SIGINT, so SIGTERM is used.
    stop_child "$av_pid"
    stop_child "$exec_pid"
    av_pid=""
    exec_pid=""
}
trap cleanup EXIT INT TERM

failures=0
record() { # record STATUS NAME DETAIL
    printf '| %s | %s | %s |\n' "$2" "$1" "$3" >>"$summary"
    [[ "$1" == FAIL ]] && failures=$((failures + 1))
    printf '%-4s %s: %s\n' "$1" "$2" "$3"
}
ask() { # ask NAME QUESTION -> records PASS/FAIL/UNRECORDED from the operator
    local answer=""
    if ((interactive)); then
        read -r -p "$2 [y/n/s(kip)] " answer </dev/tty || answer=s
    fi
    case "$answer" in
        y|Y) record PASS "$1" "operator confirmed: $2" ;;
        n|N) local note=""; ((interactive)) && read -r -p "  what happened? " note </dev/tty
             record FAIL "$1" "operator: ${note:-no detail}" ;;
        *) record UNRECORDED "$1" "not answered: $2" ;;
    esac
}

environment() {
    {
        echo "# Waddle M0 $mode evidence ($stamp)"
        echo
        echo "- Host: $(uname -srm)"
        echo "- Desktop: ${XDG_CURRENT_DESKTOP:-unknown} / session ${XDG_SESSION_TYPE:-unknown} / WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
        echo "- PipeWire: $(pw-cli --version 2>/dev/null | tail -n1 || echo unknown)"
        echo "- Waddle: $("$waddle" --version 2>/dev/null || echo unknown) at $waddle"
        echo "- Git: $(git -C "$repo_dir" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "- Device: ${device:-default}"
        echo
        echo "| Check | Result | Detail |"
        echo "|:--|:--:|:--|"
    } >"$summary"
    "$waddle" status "${device_args[@]}" >"$out_dir/status.txt" 2>&1 || true
}

# start_guest_helper POWERSHELL_COMMAND -> sets exec_pid and guest_pid (first stdout line)
start_guest_helper() {
    local command=$1
    : >"$out_dir/guest_helper.out"
    # The guest cannot use a Linux working directory; run from C:\Windows like `waddle av`.
    "$waddle" exec "${device_args[@]}" --pipe --cwd 'C:\Windows' -- powershell.exe -NoProfile -NonInteractive \
        -Command "$command" >"$out_dir/guest_helper.out" 2>"$out_dir/guest_helper.err" &
    exec_pid=$!
    guest_pid=""
    for _ in $(seq 1 120); do
        guest_pid=$(head -n1 "$out_dir/guest_helper.out" 2>/dev/null | tr -dc '0-9')
        [[ -n "$guest_pid" ]] && return 0
        kill -0 "$exec_pid" 2>/dev/null || break
        sleep 1
    done
    return 1
}

start_av() { # start_av PID
    "$waddle" av run "$1" "${device_args[@]}" >"$out_dir/av_run.out" 2>"$out_dir/av_run.err" &
    av_pid=$!
}

pipewire_node_state() { # prints "<state>" of node.name=waddle-av-playback or "absent"
    pw-dump 2>/dev/null | python3 -c '
import json, sys
try:
    objects = json.load(sys.stdin)
except Exception:
    print("absent"); sys.exit(0)
for item in objects:
    info = item.get("info") or {}
    props = info.get("props") or {}
    if props.get("node.name") == "waddle-av-playback":
        print(info.get("state", "unknown")); break
else:
    print("absent")'
}

run_audio() {
    local seconds=$((duration + 120))
    # PID first, then loop a stock WAV until the deadline: SoundPlayer renders in this process,
    # which is exactly what per-process WASAPI loopback captures.
    start_guest_helper "\$PID; \$p = New-Object System.Media.SoundPlayer 'C:\\Windows\\Media\\Alarm01.wav'; \$p.PlayLooping(); Start-Sleep -Seconds $seconds" \
        || { record FAIL guest_tone "guest tone helper did not report a PID (see guest_helper.err)"; return; }
    record PASS guest_tone "guest PowerShell tone helper PID $guest_pid"
    start_av "$guest_pid"
    local state=absent wait_limit=${WADDLE_M0_STREAM_WAIT:-120}
    [[ "$wait_limit" =~ ^[0-9]+$ ]] || wait_limit=120
    for _ in $(seq 1 "$wait_limit"); do
        state=$(pipewire_node_state)
        [[ "$state" != absent ]] && break
        kill -0 "$av_pid" 2>/dev/null || break
        sleep 1
    done
    if [[ "$state" == absent ]]; then
        record FAIL pipewire_stream "node waddle-av-playback never appeared (see av_run.err)"
        return
    fi
    record PASS pipewire_stream "node waddle-av-playback present, state=$state"
    pw-dump >"$out_dir/pw-dump-start.json" 2>/dev/null || true
    local samples=0 running=0 end=$((SECONDS + duration))
    : >"$out_dir/node_states.txt"
    while ((SECONDS < end)) && kill -0 "$av_pid" 2>/dev/null; do
        state=$(pipewire_node_state)
        echo "$(date -u +%H:%M:%S) $state" >>"$out_dir/node_states.txt"
        samples=$((samples + 1))
        [[ "$state" == running ]] && running=$((running + 1))
        sleep 5
    done
    pw-dump >"$out_dir/pw-dump-end.json" 2>/dev/null || true
    if ((samples == 0)); then
        record FAIL stream_running "AV session ended before sampling"
    elif ((running * 100 >= samples * 95)); then
        record PASS stream_running "$running/$samples samples running over ${duration}s"
    else
        record FAIL stream_running "$running/$samples samples running over ${duration}s"
    fi
    # Guest counters: "AV audio: backlog=X.XXms dropped=N discontinuities=M" every 5 s.
    python3 - "$out_dir/av_run.err" >"$out_dir/audio_stats.txt" <<'PY'
import re, sys
pattern = re.compile(r"AV audio: backlog=([0-9.]+)ms dropped=(\d+) discontinuities=(\d+)")
rows = [tuple(map(float, m.groups())) for m in map(pattern.search, open(sys.argv[1], errors="replace")) if m]
if not rows:
    print("samples=0"); sys.exit(0)
warm = rows[2:] if len(rows) > 2 else rows        # ignore the first 10 s of stream start-up
backlogs = sorted(r[0] for r in warm)
p95 = backlogs[min(len(backlogs) - 1, int(len(backlogs) * 0.95))]
print(f"samples={len(rows)} backlog_mean_ms={sum(backlogs)/len(backlogs):.2f} backlog_p95_ms={p95:.2f} "
      f"backlog_max_ms={backlogs[-1]:.2f} dropped_total={int(rows[-1][1])} "
      f"dropped_after_warmup={int(rows[-1][1] - warm[0][1])} discontinuities={int(rows[-1][2])}")
PY
    local stats
    stats=$(cat "$out_dir/audio_stats.txt")
    if [[ "$stats" == samples=0 ]]; then
        record FAIL guest_counters "no 'AV audio:' counter lines in av_run.err"
    else
        local dropped
        dropped=$(sed -n 's/.*dropped_after_warmup=\([0-9]*\).*/\1/p' <<<"$stats")
        # Pass bound: at most 0.1% of the warm-up-excluded frames dropped.
        local budget=$(( (duration - 10) * 48 ))
        if ((dropped <= budget)); then record PASS guest_counters "$stats"
        else record FAIL guest_counters "$stats (drop budget $budget frames)"
        fi
    fi
    ask audible "Did you hear the Windows alarm sound loop continuously, without crackles or gaps?"
    ask mixer_entry "Does the Linux sound settings/mixer show a 'Waddle game audio' playback stream whose volume slider changes the loudness?"
    cat >>"$summary" <<'EOF'

Latency note: `backlog_*` is the guest ring occupancy (guest capture to host
consumption). End-to-end acoustic latency additionally includes the host trim
target (<= 8 ms, `av_pipewire.c`) and the PipeWire graph quantum recorded in
`pw-dump-start.json`; an acoustic measurement is optional and recorded by hand.
EOF
}

run_window() {
    # Launch Notepad, report the newest Notepad PID, keep the helper alive while it runs.
    start_guest_helper "Start-Process notepad.exe; Start-Sleep -Seconds 3; \$n = Get-Process -Name notepad | Sort-Object StartTime | Select-Object -Last 1; \$n.Id; Wait-Process -Id \$n.Id" \
        || { record FAIL guest_window "Notepad helper did not report a PID (see guest_helper.err)"; return; }
    record PASS guest_window "guest Notepad PID $guest_pid"
    start_av "$guest_pid"
    sleep 15
    if ! kill -0 "$av_pid" 2>/dev/null; then
        record FAIL av_session "waddle av run exited early (see av_run.err)"
        return
    fi
    record PASS av_session "waddle av run is presenting the Notepad window"
    ask window_visible "Is the Notepad window visible on the Linux desktop with its own Windows title bar and no second (Linux) frame around it?"
    ask taskbar_entry "Does the Linux taskbar/dock/overview show an entry for it? (Generic icon is expected until TODO #28.)"
    ask alt_tab "Does Alt+Tab (or the overview) switch between it and a Linux window?"
    ask click_focus "Clicking into the Notepad text area focuses it (the first click may be lost until TODO #16)?"
    ask typing "Typing 'hello Waddle' into Notepad shows exactly that text?"
    ask title_update "After typing, does the taskbar/Alt+Tab title change to start with '*' (unsaved marker)?"
    ask compositor_move "Moving with the compositor shortcut (GNOME/KDE: Super+drag or Alt+F7) moves the window?"
    ask compositor_resize "Resizing with the compositor shortcut (Alt+F8 or Super+right-drag) resizes Notepad's content to the new size?"
    ask fullscreen "Toggling fullscreen with the compositor shortcut (KDE: configurable; GNOME: none by default — answer s) fills the screen?"
    ask close "Closing from the Linux side (taskbar close or Alt+F4) closes Notepad, or prompts to save first?"
    cat >>"$summary" <<'EOF'

Known gaps at M0 (tracked, not failures of this run): dragging the Windows
caption/edges does not move/resize (TODO #12), no maximize (TODO #13), generic
taskbar icon (TODO #28), menus/dialogs owned by Notepad are not shown (TODO #10/#11).
EOF
}

environment
case "$mode" in
    audio) run_audio ;;
    window) run_window ;;
esac
cleanup
trap - EXIT INT TERM
echo
echo "Evidence written to $out_dir; summary: $summary"
((failures == 0)) || exit 1
exit 0
