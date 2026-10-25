# `waddle open` request parser contract (TODO #29a)

Module: `src/interop/open_request.zig`, C header `src/interop/open_request.h`.
Realizes IMPL_DESC section 3.9.1 (frozen grammar), 3.9.2 (PE routing through
`src/interop/pe_header.zig`) and the exit-code table of 3.9.1 and 6.5. It
produces a request description only. Path canonicalization, device mount
translation, quoting into a wire frame, LocalLaunch transport and the CLI spawn
policy belong to 29b, 29c and 29d and are out of scope here.

## 1. Scope

Inside: argv grammar validation, option and target conflict rules, per-item
kind decision, PE header request through a caller callback, the quoting of a
`-- EXE ARG...` argument tail (port of `waddle_quote`), exit-code constants,
error-to-exit mapping, LaunchResult status-to-exit mapping, and the mapping of
item kinds to wire `launch_kind` values.

Outside: any file system call (no `stat`, `open`, `realpath`), environment
reads, socket I/O, notifications, `waddle exec` delegation, timers, process
spawning, catalog lookup, and `working_dir`. The parser never reads
`XDG_ACTIVATION_TOKEN`; that is `activation_store.zig`.

## 2. Inputs

### 2.1 argv

- `argv` is the list of tokens that follow the subcommand word `open`. It does
  not include `waddle` or `open`. `argv[0]` is the first option or target.
- Each entry is a NUL-terminated byte string. Its byte length (excluding the
  NUL) must be at most **4096** (`MaxArgBytes`). A string with no NUL within
  4097 readable bytes is `ArgumentTooLong`.
- The count must be at most **64** (`MaxArgs`). Count 0 is `MissingTarget`.
- Checks run in this order before any grammar decision: count, then every
  argument length in index order. The first failure is reported.
- Arguments are compared bytewise. They are not normalized, case folded or
  trimmed. Non-ASCII bytes are ordinary path or argument bytes.

### 2.2 options (`options_t`)

| Field | Type | Meaning |
|---|---|---|
| `read_prefix` | optional callback | Reads up to 4096 leading bytes of one `.exe` path. |
| `read_context` | opaque pointer | Passed unchanged to the callback. Not dereferenced by the parser. |
| `stdio_is_tty` | u32, 0 or 1 | Caller sets 1 only when stdin and stdout are both terminals. |

`read_prefix` callback contract (C typedef `interop_open_read_prefix_fn`):

- Signature: `int (*)(void *context, const char *path, size_t path_length, uint8_t *buffer, size_t capacity, size_t *length_out)`.
- `path` is not necessarily NUL terminated. Use `path_length` (1..4096 bytes).
- `buffer` has `capacity` == 4096 writable bytes owned by the parser for the
  duration of the call. The callback writes at most `capacity` bytes and sets
  `*length_out` to the count written (0..4096).
- Return `ReadStatusOk` (0) on success. `ReadStatusNotFound` (1) means the path does not
  exist: the parser returns `NotFound`, exit 127. Any other nonzero value means the path
  exists or its state is unknown but it cannot be read: `ImageUnreadable`, exit 126.
  A reported length above 4096 is also `ImageUnreadable`.
- Thread safety: the callback runs synchronously on the calling thread.
- A NULL `read_prefix` makes every `.exe` item a read failure (`ImageUnreadable`).

The parser calls the callback **only** for items whose extension is `.exe`
(case-insensitive). It never calls it for `.msi`, `.lnk`, or other extensions,
and never for `--app`. It calls it at most once per item, in argv order.

### 2.3 Output (`request_t`)

Caller-owned. Every error except `InvalidArgument` zeroes the output; `InvalidArgument`
leaves it untouched (it may be NULL in the C ABI).

- `item_count` (usize, 0..64).
- `wait`, `new_console` (u32, 0 or 1). `--wait` and `--new-console` respectively.
- `device` (nullable pointer) and `device_length`. Null and 0 when `--device`
  is absent. When present, the pointer borrows the argv entry (valid while the
  argv storage lives). The caller applies the default device when null.
- `items[64]`: each `item_t` has `kind`, `image` (`pe_header` GUI or Console
  routing value, GUI when not an executable), `text` (borrowed pointer and
  length: the app key for App, the host path otherwise), `argument_first` and
  `argument_count` (indices into argv of the explicit argument tail; both 0
  when there is no tail).

The `image` field is `Gui` for every non-`.exe` item and for every GUI `.exe`.
It is `Console` only for a classified CUI `.exe`.

## 3. Grammar

```
waddle open [--device|-d NAME] [--wait] [--new-console] (--app APP_KEY | PATH... | -- EXE [ARG...])
```

Accepted token forms (exact spelling only):

- `--device NAME` or `-d NAME`. The value is the next token.
- `--app APP_KEY`. The value is the next token.
- `--wait`, `--new-console`: flags.
- `--`: introduces the EXE form. Everything after it is EXE then ARG tokens.
- Any other token beginning with `-` (including a lone `-`, `--device=NAME`,
  `--app=KEY`) is `UnknownOption`. Targets that begin with `-` cannot be given
  as a PATH; they must use the `--` form.
- Any other token is a PATH.

Rules:

1. **Options before positional items.** An option token is accepted only while
   no PATH and no `--` has been seen. After a PATH or `--`, a `--device`, `-d`,
   `--wait` or `--new-console` token is `OptionAfterTarget`. Repeating `--app`
   after a PATH is `MixedTargets`.
2. **Repetition.** Repeating any of `--device`/`-d`, `--app`, `--wait`,
   `--new-console` is `DuplicateOption`, even when the values are identical.
   `-d X --device Y` is a duplicate.
3. **Values.** An option value is always the next argv token, whatever its
   spelling. A value option with no next token is `MissingOptionValue`. A
   next token that fails its own validator is `InvalidDevice` or
   `InvalidAppKey`. Consequently `-d -dev` and `--device -dev` name the device
   `-dev`, and `-d --wait` names a device called `--wait`: the device validator
   (below) accepts these spellings, and no token is re-interpreted as an option.
   `--app --wait` is `InvalidAppKey`. Write the value after the option; there
   is no `--opt=value` form.
4. **Device name.** 1..63 bytes of `[A-Za-z0-9_-]`, matching
   `daemon_device_validate_name` (WaddleMaxDeviceNameLen 64 including NUL).
   Otherwise `InvalidDevice`.
5. **App key.** 3..200 bytes (`InteropMaxAppKeyBytes`), first byte `a` or `p`,
   second byte `-`, remaining bytes `[A-Za-z0-9_-]`, matching the codec rule
   `app_key_valid`. Otherwise `InvalidAppKey`. Case is not folded.
6. **Target exclusivity.** Exactly one target form is allowed: `--app`, one or
   more PATH, or one `--` EXE form. Any mix is `MixedTargets`. The first
   conflicting token is the one that reports the error.
7. **Missing target.** No `--app`, PATH or `--` is `MissingTarget`. This includes
   an argv made only of options.
8. **Empty PATH.** A zero-length PATH token is `EmptyPath`.
9. **EXE form.** `--` must be followed by an EXE token. Missing EXE, or a
   zero-length EXE, is `EmptyExecutable`. The EXE token must end with `.exe`
   (case-insensitive) after the extension rule of section 4, otherwise
   `ExecutableNotExe`. Tokens after EXE are ARG tokens, verbatim, including
   tokens equal to `--`, `--wait` and empty strings. A second `--` after EXE is
   an ARG.
10. **Argument tail length.** The quoted form of the ARG tokens, as produced by
    `quote_into` (section 6), must be at most **4096** bytes
    (`InteropMaxStringBytes`, the wire limit of `LaunchRequest.arguments`).
    Longer tails are `QuotedArgumentsTooLong`.
11. Targets are parsed left to right. Each token is consumed once.

`--new-console` with an `--app` target is accepted and has no effect, because
App items are never CUI routed. `--wait` has no effect on classification.

## 4. Item kind decision

Extension rule: take the final path component (bytes after the last `/`).
If it contains a `.`, the extension is the bytes after the last `.`, compared
ASCII case-insensitively. A final component without `.` has no extension. A
final component ending in `.` has an empty extension. A leading dot counts, so
the component `.exe` has extension `exe`.

| Source | Condition | `kind` | `image` |
|---|---|---|---|
| `--app KEY` | valid key | `App` | Gui |
| PATH | extension `msi` | `Installer` | Gui |
| PATH | extension `lnk` | `Shortcut` | Gui |
| PATH | extension `exe`, PE GUI or unknown | `Path` | Gui |
| PATH | extension `exe`, PE CUI, `stdio_is_tty` = 1, `--new-console` absent | `DelegateExec` | Console |
| PATH | extension `exe`, PE CUI, otherwise | `Console` | Console |
| PATH | any other extension, including none | `Path` | Gui |
| `-- EXE ARG...` | PE GUI or unknown | `ExeWithArgs` | Gui |
| `-- EXE ARG...` | PE CUI, `stdio_is_tty` = 1, `--new-console` absent | `DelegateExec` | Console |
| `-- EXE ARG...` | PE CUI, otherwise | `Console` | Console |

PE classification is `pe_header.classify` applied to the prefix returned by the
callback. Malformed PE content is GUI (`pe_header` contract). A read failure is
`ImageUnreadable` for the whole parse. It is never silently GUI.

`ExeWithArgs` is the GUI EXE form that carries an argument tail. A `--` form
with no ARG tokens is still `ExeWithArgs` with `argument_count` 0.

### 4.1 Wire `launch_kind` mapping (`item_kind_t.wire_launch_kind`)

| Item kind | Wire `launch_kind` | Notes |
|---|---|---|
| App | 1 `InteropLaunchApp` | `text` is app_key, no target path |
| Path | 2 `InteropLaunchPath` | |
| ExeWithArgs | 2 `InteropLaunchPath` | arguments carried in the tail |
| Installer | 3 `InteropLaunchInstaller` | |
| Shortcut | 4 `InteropLaunchShortcut` | |
| Console | 5 `InteropLaunchConsole` | |
| DelegateExec | none (null) | executed locally through `waddle exec`; never sent |

Codec cross-field rule (`interop_codec.zig` `cross_valid`): App requires app_key
and forbids target_path; every other wire kind requires target_path and forbids
app_key. The parser's output satisfies this for every non-delegated item.

## 5. Exit codes and mappings

| Constant | Value | Meaning |
|---|---|---|
| `ExitOk` | 0 | Launch handed off, or `--wait` got an Ok LaunchResult |
| `ExitGuestFailure` | 1 | Guest launch failure (any non-Ok LaunchResult) |
| `ExitUsage` | 2 | Usage error (every grammar and bound error) |
| `ExitLocalError` | 125 | Local failure or guest unreachable after 120 s |
| `ExitNotExecutable` | 126 | Outside share, not executable, or image unreadable |
| `ExitNotFound` | 127 | Local path not found |

`exit_code_for_error(open_error_t) u8`:

- `None` -> 0.
- Every grammar, count, length and quoted-length error -> 2.
- `ImageUnreadable`, `OutsideShare` -> 126.
- `NotFound` -> 127.
- `LocalFailure`, `GuestUnreachable`, `InvalidArgument` -> 125.
- `GuestLaunchFailed` -> 1.

`exit_code_for_launch_status(u32 status) u8`, over `interop_status_t`:

- `InteropOk` (0) -> 0.
- Every other value, known (1..9) or unknown -> 1.

Reading: IMPL_DESC 3.9.1 states "`--wait` ... returns 0 / 1" for LaunchResult.
A guest-reported NotFound or OutsideShare therefore yields 1. The 126 and 127
codes apply only to conditions the local side detects, reported through the
local error enum. This is the stricter reading, recorded here so 29f does not
guess.

## 6. Quoting of the argument tail

`quote_into(args, out)` ports `waddle_quote()` (`src/common/arguments.c`):

- Empty argument list -> empty output, length 0.
- Each argument is wrapped in `"`. Arguments are joined by one space.
- Within an argument, a run of `n` backslashes is emitted as `n` backslashes
  when followed by a non-quote byte, and as `2n` backslashes when followed by
  `"` or by the end of the argument. A `"` byte is emitted as `\"`. Other bytes
  are copied verbatim (UTF-8 bytes are never inspected).
- Empty argument -> `""`.
- Output is not NUL-terminated by the Zig function. The C wrapper
  (`interop_open_quote`) writes a NUL after the length when capacity allows.
- Errors: `QuotedTooLarge` when the output would exceed 4096 bytes (stricter
  than the 1 MiB `WaddleMaxPayloadSize` of `waddle_quote`, because the wire
  field is 4096 bytes), and `NoSpace` when `out` is too small.

The C consumer test calls `waddle_quote` and `interop_open_quote` on the same
vectors and compares bytes. Vectors cover spaces, tabs, embedded `"`, backslash
runs before quotes and at the end, empty arguments, UTF-8 bytes, a lone
backslash, and multiple arguments.

## 7. C ABI

```c
#define InteropOpenMaxArgs 64u
#define InteropOpenMaxArgBytes 4096u
#define InteropOpenMaxItems 64u
int interop_open_parse(size_t argc, const char *const *argv,
                       const interop_open_options_t *options,
                       interop_open_request_t *request);
int interop_open_quote(size_t argc, const char *const *argv, char *out,
                       size_t capacity, size_t *length_out);
uint8_t interop_open_exit_code(uint32_t error);
uint8_t interop_open_launch_exit_code(uint32_t status);
```

- `interop_open_parse`: returns `interop_open_error_t` (0 = None). `argv` may be
  NULL only when `argc` is 0. `options` and `request` must be non-NULL; a NULL
  one returns `InvalidArgument` (value 1) and leaves `request` untouched. Each
  `argv[i]` must be readable up to its NUL or 4097 bytes. `request` must not
  alias `argv` or `options`.
- `interop_open_quote`: returns 0 Ok, 1 InvalidArgument (NULL `out` or
  `length_out`, argc > 64, argument longer than 4096), 2 QuotedTooLarge, 3 NoSpace.
  `capacity` must be at least `length + 1` for the NUL. `out` and `argv` must not
  overlap.
- `interop_open_exit_code`: maps `interop_open_error_t` to the exit code above.
  Unknown values map to 125.
- Thread safety: all functions are pure and reentrant. Output storage is the
  caller's. Borrowed pointers in `request` are valid while argv lives.

Layout (both Zig `extern struct` and C struct; sizes asserted in both
languages): `item_t` 40 bytes, `request_t` 2592 bytes, `options_t` 24 bytes.

## 8. Error enum

Usage (exit 2): `UnknownOption`, `DuplicateOption`, `MissingOptionValue`,
`InvalidDevice`, `InvalidAppKey`, `OptionAfterTarget`, `MissingTarget`,
`MixedTargets`, `EmptyPath`, `EmptyExecutable`, `ExecutableNotExe`,
`TooManyArgs`, `ArgumentTooLong`, `QuotedArgumentsTooLong`.

Parse-time local (exit 126): `ImageUnreadable`.

Caller-reported outcomes (mapped here so every consumer shares one table; `NotFound`
is also raised by the parser for read status 1, section 2.2):
`OutsideShare` (126), `NotFound` (127), `LocalFailure` (125),
`GuestUnreachable` (125), `GuestLaunchFailed` (1).

`InvalidArgument` (C ABI only, NULL or aliasing pointers) is value 1 and maps to
125 in `interop_open_exit_code`, because it indicates a caller defect, not a
user usage error.

## 9. Threading and memory

- No allocation. The parser uses one 4096-byte stack buffer for the callback
  and a 64-entry stack array of argv slices in the C wrapper.
- Borrowed: argv bytes, the device pointer, item text pointers. The callee never
  retains a pointer after return.
- Single call, single thread. Independent calls are thread-safe.

## 10. Test plan (merged into `open_request.zig` tests and `tests/interop/open_request_abi.c`)

- Every grammar form: device long and short, flags, `--app`, one and many PATH,
  `--` with and without ARG, `--` ARG containing `--`, `--wait`, empty ARG.
- Every usage error, each at its first triggering token.
- Boundaries: 64 args accepted, 65 rejected; 4096-byte argument accepted, 4097
  rejected; 63-byte device accepted, 64 rejected; 200-byte app key accepted,
  201 rejected; 4096-byte quoted tail accepted, 4097 rejected.
- Classification table: every row of section 4, for both TTY values, both
  `--new-console` values, GUI, CUI, unknown, malformed and read-failed prefixes.
- Callback behavior: call count and order, path bytes, capacity 4096, failure
  return, length above capacity, NULL reader.
- Quoting: the vectors of section 6 against literal expected strings in Zig and
  against `waddle_quote` in C, plus NUL placement and `NoSpace`.
- Randomized sequences: 20000 seeded token streams. Every outcome must satisfy
  the output contract: errors zero the request; successes have 1..64 items whose
  text is one of the input tokens and whose kind is never Unused. A full reference
  recognizer is not implemented; the grammar is covered by the table tests.
- Exit tables: every error value, every launch status value.
- Gates: `python3 tests/av/coverage.py open_request` >= 90% line and taken
  branch; `interop-open-request-test` runs Zig tests plus the C ABI test; the
  sanitizer target runs the C ABI test under `-fsanitize=address,leak,undefined`
  with `ASAN_OPTIONS=detect_leaks=1`. Zig sanitizer execution is not claimed.
