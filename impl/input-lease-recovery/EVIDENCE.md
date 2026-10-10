# Input lease implementation evidence

Date: 2026-10-10 UTC. Branch: `feature/software-vgpu-slicing`.

## Exact source and scope

Protocol/core commits: `8b5232c6c56028a5a003df9e37faddae900a4846` and `aebe710eaeab272dc8555fd145cc875bef4f8cdd`. Integration checkpoint: `14d0d84330029114aa377aa3a1ed9baadcdd4902`. The integration source plus the deterministic host-poll follow-on is pinned by `SOURCE_SHA256SUMS`, manifest SHA-256 `32b36c5a4f450475cf8e251e27d885dc815797fb205985467beca2aeb733c95d`. Verify with `sha256sum --check impl/input-lease-recovery/SOURCE_SHA256SUMS`.

These results cover the minimal CreateV3/Revoked/Ack/Ready vertical slice and its old-peer compatibility. They do not replace native desktop acceptance or imply that `SendInput` is target-bound.

## Passed local gates

- `make -B av-test`: complete fresh rebuild and AV aggregate passed. The earlier incremental attempt found a stale pre-existing sanitizer-instrumented setup object; the fresh rebuild resolved that artifact mismatch without a source workaround.
- `make av-test av-input-test build/waddle-av-host build/av_platform_test`: final frozen-source functional aggregates and real host/platform links passed.
- `make av-coverage`: all unchanged minimum 90% production line/branch gates passed:
  - lease: 99.19% lines (122/123), 93.01% branches (173/186)
  - codec: 100% lines (104/104), 95.89% branches (210/219)
  - input: 100% lines (83/83), 97.32% branches (109/112)
  - identity: 100% lines (26/26), 96.77% branches (60/62)
  - audio: 100% lines, 94.44% branches
  - layout/video: 100% lines and branches
- `make av-windows build/av_windows_test.exe build/av_guest_quiescence_test.exe build/av_windows_lease_test.exe`: Windows guest and all listed fixtures cross-link with Linux `CPATH` and `LIBRARY_PATH` cleared. This is compilation/link evidence, not Windows execution.
- Nine production Zig codec tests, pinned V1/V2 fixture checks, portable lease/socket FIFO tests, real Wayland callback tests and production Windows-adapter API doubles pass.
- The extracted actual production `host_poll` boundary passes deterministic EINTR, unchanged-deadline, late-readable traffic, errno preservation, EAGAIN writable-interest and clock/flush failure regressions. Both AV aggregates pass after a fresh rebuild including this fixture; see `eintr-plain-aggregates.log` and `host-poll-followup.log`.
- `git diff --check` and the frozen source manifest check pass.

Detailed local outputs are retained under `build/input-lease-evidence/`: `av-test-rebuild.log`, `final-plain-aggregates.log`, `coverage.log`, `windows-crosslinks.log`, `final-windows-crosslinks.log`, and `source.sha256`. Coverage tools preserve their uniquely named instrumented runs under `build/coverage/av/`.

## Distinct sanitizer and execution limits

The unsuppressed `make av-sanitizers` gate built all aggregate targets and exited 2 at its first runtime test with the explicit LeakSanitizer ptrace fatal error. It is recorded separately in `build/input-lease-evidence/full-sanitizers.log`. The separately built full-sanitizer host-poll fixture likewise exited 134 with this restriction (`host-poll-sanitizers.log`). Focused full sanitizer attempts report LeakSanitizer's ptrace restriction in this container. This is not a leak-test pass and no production gate or threshold is relaxed. Separate adapter ASan/UBSan-only diagnostic runs passed with leak detection disabled; those diagnostics are not the required ASan/LSan/UBSan qualification. The supported CI gate must still run unsuppressed on the exact integration commit.

Native Windows desktop, UIPI, physical SendInput delivery, compositor and driver acceptance are pending. `NATIVE_ACCEPTANCE.md` supplies the isolated two-window traces and opt-in `--lease-fixture` receipt logger. Independent integrated review is required before publication; its exact source/commit receipt is recorded separately when complete.
