# Portable ICD test environment mutation

## Scope and root cause

The native Windows CI ICD unit aggregate links every diagnostic test. The two existing diagnostic tests called POSIX `setenv` and `unsetenv` directly; those symbols do not exist in the Windows GNU runtime. The complete Windows Debug unit link failed before executing tests. This change fixes only test fixtures in `src/vgpu/venus_icd.zig`, preserving all production dispatch, environment reads, ownership logic, test inclusion, coverage rules, and thresholds. The six queue/WSI regressions from `c5ea2c7` remain unchanged.

## Architecture and native API boundary

`test_environment_t` snapshots an environment variable through Zig 0.13's `std.process.getEnvVarOwned`. Diagnostic production code uses `std.process.hasEnvVarConstant`. In Zig 0.13, both APIs select `std.process.getenvW` on Windows, which scans `windows.peb().ProcessParameters.Environment`. Therefore the test mutation must reach the process environment; changing only the C runtime's private `_environ` array with `_putenv` would not reliably exercise the production reads.

Windows mutation uses the standard-library declaration of `kernel32.SetEnvironmentVariableW` (including its native calling convention). A non-null wide string sets a value; a null pointer deletes the variable. POSIX targets retain the existing libc `setenv`/`unsetenv` APIs. Compile-time OS dispatch prevents POSIX mutation symbols from becoming Windows references. The guard is referenced only by tests; no production initialization or dependency changes are introduced.

Primary references:

- Zig 0.13 bundled `lib/std/process.zig`: `getEnvVarOwned`, `hasEnvVarConstant`, and `getenvW` (lines 399–432 and 490–522).
- Zig 0.13 bundled `lib/std/os/windows/kernel32.zig`: `SetEnvironmentVariableW` and `GetLastError` declarations.
- [Microsoft SetEnvironmentVariableW documentation](https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-setenvironmentvariablew): current-process mutation and null-pointer deletion semantics.
- [Microsoft .NET native environment wrapper](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Private.CoreLib/src/System/Environment.Variables.Windows.cs): explicitly accepts `ERROR_ENVVAR_NOT_FOUND` when deleting an already-absent variable. This fixture narrows that exception to deletion only.

## Data, encoding, and ownership

The guard contains a borrowed NUL-terminated key slice and an optional owned byte-slice snapshot. `null` means the key was absent; an allocated zero-length slice means it was present with an empty value. Keys in this suite are nonempty ASCII names without `=`. Values have no embedded NUL. No protocol layout, alignment, atomic, or ABI struct changes occur.

On Windows, temporary key and value strings are converted using `wtf8ToWtf16LeAllocZ`, not ANSI conversion or strict UTF-8 conversion. `getEnvVarOwned` represents native UTF-16 values as WTF-8, so this preserves non-ASCII characters, supplementary characters, and isolated UTF-16 surrogate code units during restoration. A present empty value uses an allocated terminator pointer rather than null. On POSIX, values are copied with `dupeZ`, preserving arbitrary non-NUL byte sequences without imposing UTF-8 validation.

The testing allocator owns snapshots and temporary conversions. Each successful allocation has an immediate matching deferred free. Initialization does not mutate the environment. `deinit` restores the snapshot, releases it exactly once, and invalidates the guard. Allocation failure while constructing a mutation leaves the process environment unchanged. An error returned by a native mutation propagates as `EnvironmentMutationFailed`; a failed restoration panics to prevent a contaminated test process being counted as successful.

## Execution sequence and cleanup

1. Initialize a guard before the diagnostic fixture changes the environment.
2. Defer guard teardown immediately after successful initialization.
3. For each enabled/disabled diagnostic branch, set `"1"` or remove the key through the guard.
4. Check presence through the same `hasEnvVarConstant` API production uses, then execute the existing diagnostic/ownership assertions.
5. Run existing ICD fixture cleanup in its original deferred order.
6. Restore the prior environment value or prior absence, then free the snapshot.

Removal is idempotent. POSIX `unsetenv` already supports absent variables. Windows ignores `ENVVAR_NOT_FOUND` only when the requested operation was deletion; every other native error, and every failed non-null assignment, remains a failure. Repeated deletion is explicitly exercised.

## Concurrency and lifetime contract

The standard Zig test runner executes these tests serially. Environment mutation is process-global and must not run concurrently with other environment readers or writers. The fixture does not add threads, callbacks, locks, production globals, or synchronization. Native API return precedes reading the environment again; no borrowed pointer into the Windows environment block survives a mutation. The guard borrows its key through teardown, and all current keys are static string literals.

## Regression and acceptance criteria

The new regression loops over prior absence, prior empty value, retained ASCII, Unicode with both BMP and supplementary characters, and a platform-specific lossless case (isolated high surrogate on Windows, invalid-UTF-8 opaque byte on POSIX). Each nested guard sets an enabled value, sets an empty value, deletes twice, and then restores its captured prior state. Both presence and exact byte contents are checked using Zig's production read APIs. An outer guard restores any preexisting value of the dedicated test key.

Both existing diagnostic tests use this same guard; none is skipped or conditionally disabled. Required local checks are the complete Linux Debug/ReleaseSafe ICD suite, targeted environment/diagnostic tests, the unchanged whole-ICD coverage target, and both Windows Debug/ReleaseSafe full-unit cross-links with the CI's complete oracle/library list. The Windows local commands append `--test-no-exec` and a dedicated emitted filename: this proves linking only, not native execution. Native Windows CI remains the authority for Windows runtime behavior. This bounded repair does not claim real GPU/VM, Wayland, DXVK, or leak-sanitizer acceptance.
