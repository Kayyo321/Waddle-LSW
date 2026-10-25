# Guest Launch Executor Contract (guest, Win32)

Scope: TODO #29 subtask 29i, the guest-side handler for `LaunchRequest` (0x0700, H→G) that returns
`LaunchResult` (0x0701, G→H). Source: `src/interop/guest_launch.c` and `.h` (portable core),
`src/interop/guest_launch_windows.c` and `.h` (Win32 table). Semantics: IMPL_DESC 3.9.3 (Guest launch),
6.5 (launch failures), 3.2.4 (`interop_status_t`), 3.2.5 (message rows), 3.2.2–3.2.3 (TLV and tags).

## 1. Scope and boundaries

In scope:
- Decode one `LaunchRequest` with the interop codec, validate its fields, resolve App keys through a catalog
  seam, and make exactly one native launch call.
- Encode one `LaunchResult{process_id, status}` with the request id echoed and the activation token echoed
  when the request carried one.
- Map every Win32 failure to a status.

Out of scope:
- The control runtime that receives frames, runs the executor on a worker, and sends the reply.
- The catalog itself (the resolver is a seam; the catalog lane owns the data).
- COM activation (`IApplicationActivationManager`). It is a C++ compatibility boundary and is not built here.
- Windows execution. The Linux suite runs the core over a double. The native test runs on a Windows desktop
  and is executed by the CI run only.

## 2. Interface

```c
int guest_launch_execute(const guest_launch_env_t *env, const uint8_t *request, size_t request_length,
                         uint8_t *result, size_t result_capacity, size_t *result_length);
interop_status_t guest_launch_status_for_error(uint32_t error);
```

- Returns **0** when a `LaunchResult` frame was written. This includes every launch failure; the status says
  which. Returns **-1** when no reply can be formed: a NULL argument, `result_capacity` below
  `GuestLaunchResultBytes` (2048), a header that does not decode, a frame whose length disagrees with its
  header, or a type other than `LaunchRequest`. On -1 no native call was made, and the caller sends nothing.
- `env->native` is a table of four functions (section 4). `env->resolve` maps an App key to a target
  (section 3).
- The capacity check and the frame checks happen before any native call, so a launch never happens without a
  reply the caller can send.

## 3. Request semantics per kind

The codec already enforces the TLV layout, the kind range 1..5, required fields, and App-key exclusivity
(`app_key` XOR `target_path`). The executor validates the text and the combinations.

| Kind | Native call | File / command | Directory | Rules |
|---|---|---|---|---|
| `InteropLaunchApp` | see below | from catalog | catalog or as Path | key must resolve; catalog text re-validated |
| `InteropLaunchPath` | ShellExecuteExW `open` | `target_path` | `working_dir` or the file's directory | absolute guest path |
| `InteropLaunchInstaller` | ShellExecuteExW `open` | `msiexec.exe`, parameters `/i "<path>"` plus `arguments` appended | as Path | `.msi` suffix (case-insensitive), else Unsupported |
| `InteropLaunchShortcut` | ShellExecuteExW `open` | the `.lnk` path, no parameters | as Path | `.lnk` suffix, else Unsupported; arguments refused (Unsupported) |
| `InteropLaunchConsole` | CreateProcessW `CREATE_NEW_CONSOLE` | `"<path>"` plus ` ` + `arguments` as one string, passed as-is | as Path | absolute guest path |

App kind resolution (`dispatch_app`):
1. Unknown key: `NotFound`. Resolver failure, or no resolver: `GuestError`. No native call in either case.
2. Packaged AUMID (`packaged` flag in the catalog entry): the `activate_aumid` seam (section 4). See the
   deviation in section 6.
3. Unpackaged AUMID: ShellExecuteExW `open` on `shell:AppsFolder\<aumid>`. Arguments cannot be carried by an
   AppsFolder item, so non-empty arguments are refused (`Unsupported`) without a native call.
4. No AUMID, a target path: launched exactly like Path kind with the entry's arguments and working directory.
5. Neither: `GuestError` ("catalog entry has no launch target").

Absolute guest path: `X:\...` or `X:/...` (either letter case), or a UNC path `\\server\...` longer than
three bytes. No quote character, no control byte, no NUL, valid UTF-8, at most 1024 bytes.
Arguments: at most 4096 bytes, valid UTF-8, a tab allowed, no other control byte.
A working directory, when present, is an absolute guest path. A file's directory is the text before its last
separator; `C:\x.exe` gives `C:\`.

## 4. Native table

```c
typedef struct guest_launch_native_t {
    void *context;
    void (*shell_execute)(void *context, const guest_launch_shell_t *shell, guest_launch_native_result_t *result);
    void (*create_console)(void *context, const char *command_line, const char *directory, guest_launch_native_result_t *result);
    void (*activate_aumid)(void *context, const char *aumid, const char *arguments, guest_launch_native_result_t *result);
    void (*close_process)(void *context, void *process);
} guest_launch_native_t;
```

- Strings are UTF-8 and NUL-terminated. The executor passes only text it has validated and measured.
- `result->error` is a Win32 error code, or 0 on success. `result->process` is an owned handle or NULL.
- The executor closes every non-NULL `result->process` through `close_process`, exactly once, on every path,
  including error paths. The Win32 table closes the handles it owns itself (the thread handle of
  `CreateProcessW`; the explorer handles of the AUMID seam).
- Win32 implementation (`guest_launch_windows.c`): ShellExecuteExW with `SEE_MASK_NOCLOSEPROCESS |
  SEE_MASK_FLAG_NO_UI`, verb `open`, `SW_SHOWNORMAL`. CreateProcessW with `CREATE_NEW_CONSOLE`, no inherited
  handles. UTF-8 is converted with `MB_ERR_INVALID_CHARS`.

## 5. Status mapping and messages

`guest_launch_status_for_error` maps Win32 codes:

| Status | Win32 codes |
|---|---|
| Ok | 0 |
| NotFound | 2 (file not found), 3 (path not found), 123 (invalid name) |
| AccessDenied | 5 |
| Unsupported | 11 (bad format), 31 (no association), 50 (not supported), 193 (bad EXE format), and every refusal the executor makes before a native call |
| Busy | 170 |
| Cancelled | 1223 (the user declined a prompt) |
| TimedOut | 1460 |
| GuestError | any other code, and every validation failure |

Failures carry `process_id` 0 and a message: `launch failed: Win32 error <n>` for native codes, or a fixed
diagnostic for validation (`target path is not an absolute guest path`, `installer launch needs a .msi file`,
and so on). Messages are at most 64 bytes, far below the 1024-byte limit of 3.2.2.

## 6. Deviations from IMPL_DESC 3.9.3 (listed, not hidden)

1. **Packaged AUMID is not activated through `IApplicationActivationManager::ActivateApplication`.** That is COM,
   C++-only, and a later compatibility boundary. The real table launches `explorer.exe shell:AppsFolder\<aumid>`
   through `activate_aumid`. Consequences:
   - The activated application's process id is unknown, so `process_id` is **0**. The host matches the first
     window of the app key instead (3.9.3 already allows this for `explorer.exe` and `ApplicationFrameHost.exe`).
   - `Ok` means explorer accepted the request. A failure inside explorer or the app is not observed.
   - Arguments cannot be passed through an AppsFolder item, so the table refuses them (`Unsupported`).
   - AUMIDs containing a space or a quote are refused (`NotFound`, invalid name) before any process starts.
2. **Installer arguments.** 3.9.3 specifies `msiexec.exe /i "<path>"` only. A non-empty `arguments` is appended
   after the quoted path. This is the faithful extension; the spec is silent.
3. **Shortcut arguments are refused** (`Unsupported`). ShellExecuteExW ignores parameters for a link, so
   accepting them would drop data silently.
4. **Document launches have no process.** ShellExecuteExW can open a document through its association without
   starting a process. Such a launch reports `Ok` with `process_id` 0.
5. **No `CREATE_BREAKAWAY_FROM_JOB`.** The agent creates no job. A child inherits the agent's own job membership,
   if any. Adding breakaway would fail under a job that forbids it. The native test therefore checks that the
   child's job membership equals the test process's, not that it is empty.

## 7. Process lifetime, bounds, threads

- The executor never waits for the launched process to exit and never assigns it to a job. A process outlives
  the call and the connection (3.9.3). `LaunchResult` reports the creation outcome only.
- Each request makes at most one native launch call. The native calls return when the target process exists
  (or when ShellExecuteExW or CreateProcessW fail).
- Timeouts: **no timeout is enforced inside this module.** ShellExecuteExW can block (for example behind a UAC
  prompt or a slow shell extension) and a blocked call cannot be cancelled. The caller must run the executor on
  a dedicated launch worker, never on the control thread that services the wire, and must report `TimedOut` to
  the host itself when its bound expires. The control runtime is not built here; this is a requirement on it.
- Stack: `guest_launch_execute` keeps its working storage (about 27 KB) on the caller's stack. There is no
  static state and no heap allocation in the portable core; the Win32 table allocates the UTF-16 copies it
  needs and frees them on every path.
- Reentrant when the native table and the resolver are reentrant.

## 8. Verification

Commands (worktree root):

```
make interop-guest-launch-test         # Linux suite over the double and the real codec object
make interop-guest-launch-sanitizers   # the same suite under ASan + LSan + UBSan, detect_leaks=1
make interop-guest-launch-coverage     # gcov line and branch gate, 90 percent, on guest_launch.c
make interop-guest-launch-windows      # mingw -Werror build of build/interop_guest_launch_native_test.exe
.\build\interop_guest_launch_native_test.exe   # on a Windows desktop session, not here
```

The Linux suite (`tests/interop/guest_launch_suite.c`, 23 tests) covers:
- every launch kind and its command formation, with the directory rules;
- the App kind through the resolver: packaged and unpackaged AUMID, target path, unknown key, resolver failure,
  no resolver, and every catalog validation failure;
- every Win32 code mapped to its status, with a process handle returned on error (closed once);
- handle accounting after every test: opened equals closed, no double close;
- the refusals that send no reply (NULL arguments, short output capacity, header and length faults, wrong
  type), and the malformed payload that gets a GuestError reply with the request id echoed;
- UTF-8 validity (valid multi-byte and twelve invalid sequences), control bytes, tabs, bounds, drive and UNC
  shapes, installer and shortcut suffixes;
- replies carry the request id and the activation token.

Results on this branch:
- Plain suite: all 23 tests pass.
- ASan + LSan + UBSan with `detect_leaks=1`: pass.
- Coverage of `guest_launch.c`: 97.90 percent lines (233/238), 90.16 percent branches (229/254).
  The uncovered branches are defensive: reads that the codec already guarantees, writer failures on a checked
  capacity, and buffer checks that the bounds of the wire make unreachable (for example, a command line that
  the length limits cannot overflow).
- mingw `-Werror` build of `guest_launch.c`, `guest_launch_windows.c` and the native test, with the Windows
  codec object: builds to `build/interop_guest_launch_native_test.exe`.

Native test (`tests/interop/guest_launch_native_windows.c`), run on Windows only:
(a) `cmd.exe /c exit 0` as Console kind: status Ok, the process id opens, waits to exit code 0;
(b) `notepad.exe` as Path kind: status Ok, the process is alive, its job membership equals the test process's
    (no job created by the agent), then the test terminates it;
(c) a nonexistent path: status NotFound, process id 0;
(d) an installer request forms `msiexec.exe` with `/i "<path>" /qn`, and a shortcut opens the link with no
    parameters. A recording wrapper stands in for ShellExecuteExW; no installer or shortcut ever runs.

## 9. Known limits

- Not executed on Windows in this session. The native test is the first Windows run and the CI job is the
  evidence; its results are not claimed here.
- Notepad: on current Windows the System32 `notepad.exe` may hand off to a packaged app and exit quickly. If the
  returned process exits before check (b), that check fails as written; a failure there is an environment
  finding, not an executor bug.
- No COM activation, no real packaged app, no UAC-prompt exercise.
- No launch timeout inside the module (section 7).
- Process ids come from ShellExecuteExW's `hProcess`. For a launch that brokers through an existing process
  (for example a DDE or an already-running shell), the id is the broker's, or 0.
