# Desktop input cloud verification, 2026-10-09 UTC

## Source and scope

- Contract: `0df71a8`.
- Canonical codec and portable ownership: `1585dac`.
- Host/guest integration, socket/callback/native fixture: `7894608`.
- CI wiring: `55d24fb`; workflow changed only, no CI or Windows execution result
  is implied by this commit.
- The containing evidence commit adds explicit native DPI-context restoration
  assertions; it does not change production input behavior.

This is an ordinary-window physical-input implementation milestone. It is not
roadmap B completion, a real compositor/Windows workflow acceptance, or a GPU
result. No live VM, GPU, KVM, physical compositor or Windows session was used.
Independent read-only review of `7894608` found no additional source-level blocker
within this scoped prototype, and independently repeated codec, portable state,
socket and callback tests. Native limitations below remain acceptance blockers.

## Toolchain and normal tests

GCC 14.2.0; Zig 0.13.0 from the checksum-verified official distribution; a
user-space SDK extracted from signed Debian package metadata supplied Wayland
1.23.1, wayland-protocols 1.44, PipeWire 1.4.2 and LLVM/Clang 19. No new repository
third-party source, dependency pin or package installation was introduced.
`WaylandProtocolsDir` now defaults to the existing system directory and permits
an alternate SDK XML root. The cloud SDK's `CPATH`/`LIBRARY_PATH` were unset for
Windows cross-compilation to prevent Linux header leakage.

The following commands exited zero, using the extracted SDK XML directory for
`WaylandProtocolsDir` and its tools/libraries in the environment:

```sh
make av-test WaylandProtocolsDir="$SDK/usr/share/wayland-protocols"
make av-input-test WaylandProtocolsDir="$SDK/usr/share/wayland-protocols"
make build/waddle-av-host WaylandProtocolsDir="$SDK/usr/share/wayland-protocols"
env -u CPATH -u LIBRARY_PATH make av-windows build/av_windows_test.exe
python3 tests/av/input_coverage.py
python3 tests/av/coverage.py av_codec
```

- Portable state fixtures: ownership, focus transfer, partial release failures,
  failed activation, stale incarnation/no-focus no-ops, unsupported scan codes,
  replay and serial exhaustion; 100 repeated full held-key/button cleanup cycles.
- Socket fixtures: actual nonblocking socketpair plus production peer/codec/state;
  128-frame full-queue release rejection followed by disconnect cleanup; replay;
  truncated-frame EOF; fresh-session isolation.
- Headless Wayland callbacks: two surfaces, physical keymap FD closure, held-enter
  suppression, pointer/focus ordering, valid and out-of-bounds pointer state,
  release forwarding, wheel direction, keyboard/pointer leave, stale leave,
  legacy view-only, retirement, overflow/failure and teardown dispatch.
- Full pre-existing AV regression suite also passed, including video/audio,
  packaging, environment, peer, deployment and setup fixtures.
- Linux client linked with warnings as errors. Windows guest and native fixture
  cross-compiled with warnings as errors; no Windows binary was executed.

## Coverage

Production-source coverage gates passed without lowering the required 90%:

| Module | Lines | Branch edges | Method |
|:--|:--|:--|:--|
| `src/av/av_input.c` | 78/78, 100% | 104/104, 100% | GCC gcov JSON, no exclusions |
| `src/av/av_codec.zig` | 83/83, 100% | 144/149, 96.64% | Existing source-debug LLVM instrumentation |

Codec instrumentation reports nine compiler panic/stack-canary guards separately,
using the unchanged existing gate's semantics. These are not claimed as covered
source validation branches. Seven Zig codec tests passed.

## Sanitizers: required leak gate is blocked

The exact input fixture rebuild with ASan/LSan/UBSan compiled, then failed at the
first fixture during LeakSanitizer initialization:

```sh
make -B av-input-test \
  WaylandProtocolsDir="$SDK/usr/share/wayland-protocols" \
  CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer' \
  LDFLAGS='-fsanitize=address,leak,undefined'
```

The runtime reported: `LeakSanitizer does not work under ptrace (strace, gdb, etc)`.
The command exited 2; this is **blocked**, not a passing leak gate or zero-leak
receipt. Required CI leak detection remains enabled in `make av-sanitizers`.

A separate supplemental run of the same three input binaries with
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1` exited zero. This
establishes ASan/UBSan behavior only and does not satisfy the leak requirement.
The next unrestricted runner must repeat the original leak-enabled gate.

Logs and generated gcov/LLVM reports are preserved in ignored
`build/desktop-input-evidence/` and unique `build/coverage/av/` run directories.
They are build outputs, not permanent hardware receipts.

## Unqualified native gates

1. Run `build/av_windows_test.exe` and the opt-in
   `build/av_windows_test.exe --input` on an authorized isolated interactive
   Windows desktop with the matching WGC fixture DLL. The input fixture checks
   real A down/up, center motion, button/wheel and held Shift release after
   foreground loss. It fails rather than bypassing foreground/UIPI restrictions.
2. Exercise genuine Wayland seat removal/readdition, keyboard/pointer focus
   switching between Linux and exported Windows windows, app closure, full queue
   failure and transport loss while keys/buttons are down; observe no stuck input.
3. Qualify actual PMv2 physical coordinates, fractional/HiDPI compositor behavior,
   resize, mixed-DPI and negative-origin Windows monitors. The input implementation
   currently uses surface scale one; compilation is not a coordinate-fidelity test.
4. Qualify Windows UIPI/foreground restrictions and native injection failure
   cleanup. SendInput plus foreground checks is not an atomic isolation boundary.
5. Test delayed WinEvent delivery with HWND destruction and same-process reuse.
   The incarnation guarantee covers observed tracker retirement/readmission only;
   native handle reuse before that observation remains unresolved.
6. Run an ordinary app type/select/save/reopen/close workflow without QMP/VNC
   assistance and verify saved bytes. Owned dialogs/process families, host layout
   translation, IME, repeat, clipboard, cursor images, relative input and tablet
   behavior remain outside this milestone and cannot be inferred from it.
