#!/usr/bin/env bash
# verify_m0.sh - Exercise scripts/interop_verify_m0.sh against fake waddle/pw-dump tools.
#
# The evidence script needs a real VM and compositor; this fixture replaces the
# `waddle` CLI and `pw-dump` with deterministic fakes so the parsing, pass/fail
# rules, cleanup and summary format are verified in CI without hardware.
# Exit 0 when every scenario behaves as expected.
set -eu -o pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"

# Fake waddle: `exec` prints a guest PID then idles; `av run` emits audio counters
# on stderr every second (FAKE_DROPS selects the dropped-frame growth per line).
cat >"$work/bin/waddle" <<'EOF'
#!/usr/bin/env bash
case "$1" in
    --version) echo "waddle fake"; exit 0 ;;
    status) echo "Running"; exit 0 ;;
    exec) echo "4242"; exec sleep 600 ;;
    av)
        dropped=0
        trap 'exit 130' INT
        for i in $(seq 1 600); do
            dropped=$((dropped + ${FAKE_DROPS:-0}))
            echo "AV audio: backlog=4.${i}0ms dropped=$dropped discontinuities=0" >&2
            sleep 1
        done ;;
esac
EOF
# Fake pw-dump: the playback node is running unless FAKE_NODE=absent.
cat >"$work/bin/pw-dump" <<'EOF'
#!/usr/bin/env bash
if [[ "${FAKE_NODE:-running}" == absent ]]; then echo '[]'; exit 0; fi
echo '[{"id":1,"info":{"state":"running","props":{"node.name":"waddle-av-playback"}}}]'
EOF
cat >"$work/bin/pw-cli" <<'EOF'
#!/usr/bin/env bash
echo "pw-cli fake"
EOF
chmod +x "$work/bin/"*
export PATH="$work/bin:$PATH" WADDLE="$work/bin/waddle" WAYLAND_DISPLAY=wayland-test

run() { "$repo_dir/scripts/interop_verify_m0.sh" "$@"; }

# 1. Healthy stream: automatic checks pass, operator questions are UNRECORDED.
out="$work/healthy"
run audio --duration 30 --out "$out" --non-interactive >"$work/healthy.log"
grep -q '| pipewire_stream | PASS |' "$out/summary.md"
grep -q '| stream_running | PASS |' "$out/summary.md"
grep -q '| guest_counters | PASS | samples=' "$out/summary.md"
grep -q '| audible | UNRECORDED |' "$out/summary.md"
grep -q 'dropped_after_warmup=0' "$out/audio_stats.txt"
if pgrep -f "$work/bin/waddle" >/dev/null; then   # cleanup must stop both fakes
    echo "fake waddle processes survived cleanup" >&2; exit 1
fi

# 2. Sustained drops beyond the 0.1% budget fail with exit 1.
out="$work/drops"
if FAKE_DROPS=500 run audio --duration 30 --out "$out" --non-interactive >"$work/drops.log"; then
    echo "drop scenario unexpectedly passed" >&2; exit 1
fi
grep -q '| guest_counters | FAIL |' "$out/summary.md"

# 3. Missing PipeWire node fails fast with exit 1 (bounded wait shortened by absent node).
out="$work/absent"
if FAKE_NODE=absent WADDLE_M0_STREAM_WAIT=3 run audio --duration 30 --out "$out" --non-interactive >"$work/absent.log"; then
    echo "absent-node scenario unexpectedly passed" >&2; exit 1
fi
grep -q '| pipewire_stream | FAIL |' "$out/summary.md"

# 4. Window mode: automatic session checks pass; every operator question is UNRECORDED.
out="$work/window"
run window --out "$out" --non-interactive >"$work/window.log"
grep -q '| guest_window | PASS | guest Notepad PID 4242 |' "$out/summary.md"
grep -q '| av_session | PASS |' "$out/summary.md"
[[ $(grep -c '| UNRECORDED |' "$out/summary.md") -eq 10 ]]
grep -q 'Known gaps at M0' "$out/summary.md"

# 5. Usage errors exit 2 without creating output.
set +e
run bogus >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "bad mode exit" >&2; exit 1; }
run audio --duration 5 >/dev/null 2>&1; [[ $? -eq 2 ]] || { echo "short duration exit" >&2; exit 1; }
WAYLAND_DISPLAY='' run audio --out "$work/nowl" >/dev/null 2>&1; [[ $? -eq 3 ]] || { echo "no wayland exit" >&2; exit 1; }
set -e

echo "interop verify_m0: healthy, drop budget, absent node, window, usage and prerequisite scenarios passed"
