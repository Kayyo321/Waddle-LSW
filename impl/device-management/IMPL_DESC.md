# Implementation Description: Device Management & Multi-Device CLI Lifecycle

## 1. Title & High-Level Scope

### 1.1 Purpose and completion boundary

A device is a named, isolated Windows VM profile with its own configuration,
QCOW2 disk, VSOCK CID, supervisor, runtime sockets, and logs. This feature ends
with a complete CLI for creating, discovering, inspecting, selecting, configuring,
starting, stopping, renaming, removing, cloning, backing up, restoring, and
diagnosing devices. Creation and multi-device startup alone do not complete it.

This document is the required final behavior, not a claim that every command
already exists. `TRACKER.md` distinguishes existing implementation from remaining
work. The specifications-only revision can be reviewed in a draft PR; merging
the feature requires all implementation and verification tasks to reach 100%.

### 1.2 In scope

- Retain `init`, `--init`, `devices`, `list`, existing lifecycle aliases, terminal
  launch, process passthrough, filesystem inspection, and log retrieval.
- Add a canonical `device` command group, safe offline registry mutations,
  persistent default selection, validated configuration changes, portable local
  backups, explicit batch lifecycle operations, structured output, and diagnostics.
- Fix VirtIO-FS readiness checking using `stat()` / `S_ISSOCK` and a stabilization
  delay. Probing a vhost-user socket with `connect()` consumes its single connection
  and can terminate virtiofsd before QEMU attaches.
- Make mutations recoverable after interruption, serialize CID allocation, and
  prevent rename/removal while any supervisor, QEMU, or virtiofsd owns the device.

### 1.3 Out of scope

GUI management, cloud downloads, remote hosts, guest OS installation or licensing,
hardware/USB passthrough, live migration, snapshots, disk resize/reset, and scheduled
autostart are separate features. Clones and backups are offline only. Removal
operates on host registration and owned files; it never uninstalls guest software
or deletes host filesystem exports. No third-party dependency is introduced by
this specification; any later dependency must follow the submodule policy.

## 2. Architecture & Inter-Component Interactions

```text
CLI command / alias
  -> argument validation (Zig for untrusted text; C ABI)
  -> registry lock, recovery, target resolution, device metadata
     -> offline mutation: staged config/state/default -> journal -> commit
     -> live command: per-device daemon client -> supervisor -> QEMU / virtiofsd
     -> exec / shell: configured CID:port -> existing guest execution protocol
  -> text or versioned JSON result, cleanup, exit status
```

C owns filesystem operations, subprocesses, locks, lifecycle control, and the CLI.
Zig owns bounded parsing of INI, JSON manifests/journals, command values, and paths
crossing untrusted boundaries. Existing guest execution and daemon wire contracts
remain authoritative; no host registry struct is transmitted as raw memory.

### 2.1 Storage and ownership

Resolve absolute, nonempty `XDG_CONFIG_HOME` and `XDG_STATE_HOME`, falling back to
`$HOME/.config` and `$HOME/.local/state`. Reject relative XDG paths and missing HOME
when a fallback is required. Device-owned directories are mode 0700 and metadata
files mode 0600. Disk creation uses restrictive permissions from the outset.

| Location relative to the resolved root | Meaning / owner |
|:--|:--|
| config: `waddle/devices/<name>.ini` | Published registration; registry owns it |
| config: `waddle/default_device` | Optional ASCII device name plus newline |
| config: `waddle/registry.lock` | Stable global lock inode; never unlink during operation |
| config: `waddle/transactions/<transaction_id>.json` | Durable mutation journal |
| state: `waddle/devices/<name>/disk.qcow2` | Device-owned overlay or standalone image |
| state: `waddle/devices/<name>/` | Owned disk and persistent logs |
| state: `waddle/retained/<transaction_id>/` | Unregistered files saved by `--keep-data` |
| runtime: `waddle/<name>/` | Daemon, QMP, virtiofsd sockets and supervisor lease |

Runtime root is `$XDG_RUNTIME_DIR`, or `/tmp/waddle-<uid>` in its absence, followed
by `waddle/<name>` for XDG and `<name>` for the fallback. Check ownership, modes,
and UNIX socket path capacity before use. Registered devices always use named
sockets, including the only device. The legacy global `daemon.sock` belongs only
to an explicit legacy transport invocation and cannot represent a named device.
Read-only discovery does not create directories, boot a guest, or initialize disks.

Published INI files define membership. Missing state directories, malformed INI,
and orphaned state directories are diagnostic findings, not implicit registrations
or reasons to silently use defaults. Sort names bytewise for deterministic results.
Report capacity overflow rather than truncating the registry to 32 entries.

### 2.2 Complete CLI inventory

`NAME`, `NEW_NAME`, and `SOURCE` are literal device names. Square brackets indicate
optional arguments; mutually exclusive alternatives are separated by `|`. Every
command supports `--help`; host management commands support `--json`. Flags may
follow the command in any order before `--`; positional arguments follow their
listed order. Unknown flags, extra operands, and duplicate singleton flags fail.

| Command | Contract and supported options |
|:--|:--|
| `waddle device init NAME [--base-disk PATH | --blank-disk SIZE]` | Register a stopped device; optional `--memory-mb N`, `--vcpus N`, `--shell COMMAND`, `--dry-run`. `waddle init` / `--init` remain aliases; their `--disk PATH` means `--base-disk PATH`. |
| `waddle device list [--running | --stopped]` | List every registered profile, configuration health, observed state, CID, and default marker. `devices` / `list` remain aliases. Filters exclude unknown/invalid states. |
| `waddle device show NAME` | Show effective config, paths, ownership, backing chain, default marker, and runtime state without starting anything. |
| `waddle device rename NAME NEW_NAME [--dry-run]` | Rename a fully quiescent registration and its owned state; preserve CID and disk contents; update default selection and managed paths. |
| `waddle device remove NAME [--keep-data] [--yes] [--dry-run]` | Remove a fully quiescent registration; default deletes owned data. Retention moves data to the retained area and returns its path. `--yes` acknowledges deletion; it never kills a running device. |
| `waddle device default [NAME | --clear]` | No argument displays the default or `null`; NAME sets an existing healthy profile; `--clear` deletes the selection. Neither boots a guest. |
| `waddle device config get NAME [KEY]` | Read effective typed values for one key or all mutable keys. |
| `waddle device config set NAME KEY=VALUE... [--dry-run]` | Validate the entire change and atomically save it on a quiescent device. Duplicate/unknown keys reject the whole command. |
| `waddle device config reset NAME KEY... [--dry-run]` | Reset specified mutable keys to documented defaults as one transaction. No implicit reset of every key. |
| `waddle device clone SOURCE NEW_NAME [--dry-run]` | Copy a quiescent profile using a flattened standalone disk; allocate a fresh CID, never change default, and never boot. |
| `waddle device export NAME --output DIRECTORY [--dry-run]` | Create a new backup directory containing a portable manifest and standalone disk; source must be quiescent; never overwrite a destination. |
| `waddle device import NAME --input DIRECTORY [--dry-run]` | Register a new device from a validated backup; allocate a fresh CID; restore portable settings with host exports disabled. |
| `waddle device doctor [NAME | --all] [--repair] [--dry-run]` | Inspect registry, configuration, disk/backing files, CID collisions, required binaries, runtime leases, and pending journals. Repair is limited to journal recovery and proven stale owned runtime files. |
| `waddle start [NAME | --device NAME | --all] [--wait | --no-wait] [--timeout N]` | Start selected devices; already running is success. Waiting means guest-agent readiness. |
| `waddle stop [NAME | --device NAME | --all] [--force] [--timeout N]` | Graceful guest shutdown, then supervisor exit; force permits immediate termination. Already stopped is success. |
| `waddle restart [NAME | --device NAME | --all] [--force] [--timeout N]` | Complete stop and supervisor exit before start; apply saved config on restart. |
| `waddle kill [NAME | --device NAME | --all]` | Explicit immediate termination of owned processes; does not remove registration/data. |
| `waddle status [NAME | --device NAME | --all]` | Observe guest state, supervisor/child PIDs, uptime, and health; never auto-start. |
| `waddle logs [NAME | --device NAME] [--follow] [--lines N] [--target daemon|qemu|virtiofsd|all]` | One device only; never auto-start. `-f` and `-n` retain their log meanings. |
| `waddle shell [NAME | --device NAME]` | Auto-start selected device and enter its configured ConPTY shell. `waddle [NAME]` remains shorthand. |
| `waddle exec [--device NAME] [existing exec options] -- COMMAND ARG...` | Execute on one resolved device; `run`, `--exec`, `-e` retain existing dispatch semantics. Preserve guest argument boundaries and exit status. |
| `waddle fs [NAME | --device NAME]` / `waddle fs test [NAME | --device NAME]` | Inspect/test the selected device's filesystem exports using existing filesystem behavior; never operate on every device implicitly. |
| `waddle --help` / `waddle --version` | General command discovery and version; command-specific help lists aliases, defaults, effects, and exit codes. |

No top-level `remove`, `rename`, or `default` aliases are introduced: namespace
these new operations under `device` to avoid collision with terminal shorthand.
A device named like a command remains addressable with `shell --device NAME`.
`-d` is the alias of `--device`. Positional NAME plus `--device`, even if equal,
is a usage error. Device mutations always require explicit names and never accept
`--all` or infer the default. `--force` is a lifecycle flag only.

### 2.3 Target selection and batch semantics

For lifecycle, shell, exec, logs, and filesystem commands, resolve explicit name
first, then configured default, then the sole registered device. Zero devices
produces an initialization hint. Multiple devices without a default produce a
sorted listing and an explicit-selection hint, with no mutation or interactive
prompt. A missing/invalid saved default is an error, never a silent fallback.
Raw mock/guest transport flags in exec bypass registry inference; combining them
with `--device` is rejected. Preserve transport-only development without devices.

`--all` is accepted only by start/stop/restart/kill/status and doctor. It is
mutually exclusive with a device selector. Snapshot the registry under lock, then
process devices sequentially in sorted order, continue after individual failures,
and return aggregate failure if any target fails. An empty batch succeeds with
an empty result list. Do not add/remove targets midway through a batch. Revalidate
each target before acting; a disappeared/renamed target fails that result.

### 2.4 Output, confirmations, and exit status

Text output is for humans; JSON is UTF-8, one newline-terminated object with
`schema_version: 1`, `command`, `ok`, `results` (array), and `error` (null or object
with snake_case `code` and `message`). Every device result identifies `name` and
`ok`; operation-specific data lives in `data`; failure has `error`. List/show/status
include `state`, `config_valid`, `is_default`, `vsock_cid`, `vsock_port`,
`memory_mb`, `vcpus`, `config_path`, `state_dir`, and `disk_image`; unknown observed
state is `unknown`, not `stopped`. Single target results still use an array.
Default query returns one result with nullable `data.default_device`. Output
arrays are sorted by name; PIDs/uptime are nullable when unavailable. Never parse
ANSI colors or diagnostic prose to obtain machine results.

Host management exits are 0 success, 2 usage/selection/validation error, 1 runtime
or transaction failure, 125 host resource/internal failure, and 130 interrupted.
JSON errors preserve stable symbolic codes (`invalid_name`, `not_found`,
`already_exists`, `busy`, `ambiguous_device`, `invalid_config`, `capacity`,
`permission_denied`, `io_error`, `timeout`, `recovery_required`, `cancelled`).
Shell/exec retain existing guest process exit forwarding and stream semantics;
`--json` is rejected there and for `logs --follow` rather than mixing JSON and
stream bytes. Normal log queries put bounded text in `data.text`.

Removal without `--yes` prompts on an attached terminal, showing the name, exact
owned deletion paths, and whether retention is requested. Require the exact name
as confirmation. EOF/mismatch cancels without effects. Non-TTY or JSON removal
requires `--yes`. `--dry-run` suppresses prompts and all writes, prints the plan,
validates reachable prerequisites, and does not reserve a name/CID. A real run
must revalidate; a dry-run is not a guarantee against concurrent changes.

## 3. Data Structures, Protocols & Memory Layouts

### 3.1 Host registry bounds and representation

Keep `device_info_t` and `device_list_t` in `src/daemon/daemon_device.h` as host-only
C ABI structures with natural compiler alignment, no packing and no shared-memory
atomics. `WaddleMaxDeviceNameLen = 64` includes NUL: names are 1–63 ASCII bytes
from `[A-Za-z0-9_-]`, case-sensitive. Reject non-ASCII regardless of locale;
length 64 is invalid. Paths use `WaddleMaxPathLen = 1024` including NUL; socket
paths additionally fit `sockaddr_un.sun_path`. Reject truncation, never silently
shorten names/paths. `MaxDeviceCount = 32`; initializing/importing/cloning entry
33 fails before creating data.

`device_info_t` contains inline owned copies of `name[64]`, `config_path[1024]`,
`state_dir[1024]`, `disk_image[1024]`, `socket_path[1024]`, four `uint32_t` values
(`vsock_cid`, `vsock_port`, `memory_mb`, `vcpus`), and `int is_running`.
`device_list_t` contains `devices[MaxDeviceCount]` and `size_t count`, bounded
0–32. Existing `is_running` is insufficient to distinguish absent, failed, and
unreachable supervisors: new output obtains authoritative state from the daemon
status protocol and reports uncertainty separately. Socket existence/connect
success alone never proves a guest is running or proves it is safe to mutate.

Caller-provided output structs/buffers are non-null unless explicitly optional;
functions borrow inputs for the duration of a call and never retain argv pointers.
Registry functions return 0 (list: nonnegative count) or -1 with preserved errno.
Allocate temporary parsing buffers with explicit allocator ownership and one
cleanup path; free and NULL C pointers on every success/error/cancellation path.
All added public declarations require field and lifecycle documentation, bounds,
nullability, parameter direction, error codes, and concurrency contracts.

### 3.2 Configuration and defaults

Use existing INI sections and `daemon_config_t` validation. Mutable config keys:

| CLI key | INI key | Range / reset value |
|:--|:--|:--|
| `memory_mb` | `[subsystem] memory_mb` | 512–65536 MiB / 4096 |
| `vcpus` | `[subsystem] vcpus` | 1–128 / 4 |
| `default_shell` | `[subsystem] default_shell` | Nonempty UTF-8, no NUL/CR/LF, at most 255 bytes / `powershell.exe` |
| `vsock_port` | `[subsystem] vsock_port` | 1–4294967295 / 5242 |
| `start_timeout` | `[timeouts] start_timeout` | 1–600 seconds / 60 |
| `stop_timeout` | `[timeouts] stop_timeout` | 1–300 seconds / 15 |

Name, CID, disk path, and mount exports are read-only in this config command;
changing them requires rename, clone/import, or the existing filesystem interface.
Default query/get returns typed values, not INI text. Config writes must preserve
existing filesystem sections and unknown keys/comments byte-for-byte, replacing
only recognized values. Parse with Zig first; stage the complete edited file,
reparse and validate it, then atomically replace it. No live reconfiguration.

Allocate a free CID from 3 through 4294967294 under the global lock, retaining the
existing highest-plus-one policy where valid; on overflow scan for an unused CID.
Never use reserved CIDs 0, 1, 2, or 4294967295. Invalid profiles reserve their
parseable CIDs and block allocation if their CID cannot be established safely.
System-wide CID conflicts at startup fail explicitly; per-user registration
cannot promise cross-user reservation. Rename preserves CID; remove releases it
only after supervisor and children exit and the transaction commits.

### 3.3 Persistence and local protocols

Keep device INI serialization textual and paths absolute. Default selection is
at most 63 ASCII bytes plus newline, not a shell fragment. Journals and backup
manifests use versioned JSON and bounded Zig parsing: maximum metadata size
64 KiB, reject duplicate/unknown required-schema keys, invalid UTF-8, excessive
nesting (more than 8 levels), and unsupported versions. Never execute file text.

A journal v1 has `schema_version`, `transaction_id` (32 lowercase hex bytes),
`operation` (`init`, `rename`, `remove`, `config`, `default`, `clone`, `import`,
`export`), `phase` (`prepared` or `committed`), nullable `source_name` and
`destination_name`, and bounded `moves`/`replacements` arrays (maximum 16 each)
recording old, staged, final, and rollback locations. Store paths relative to
known roots plus a root identifier; validate containment before recovery. For
external export destinations use an opened parent directory and record its
absolute canonical path plus device/inode identity; refuse recovery if changed.
Existing data is never overwritten without a recorded rollback copy.

Backup directories contain exactly `manifest.json` and `disk.qcow2`. Manifest v1
has `schema_version`, `source_name`, `disk_file` (literal `disk.qcow2`),
`disk_size_bytes`, lowercase hex `disk_sha256`, and `config` with the six mutable
keys above. Do not serialize CIDs, host mounts, runtime paths, or credentials.
SHA-256 is an integrity check, not an authenticity claim. Reject symlinks, missing
files, size/hash mismatch, unsupported disk format, and any external backing chain.
Use `qemu-img info --output=json` through bounded parsing to validate images.

Existing local lifecycle IPC uses the 16-byte v1 header from
`include/waddle/daemon_protocol.h` and little-endian payloads. Do not replace it
with the generic cross-domain header described elsewhere in project docs. Add
`DaemonMsgShutdownReq = 0x000D` (empty payload) and
`DaemonMsgShutdownResp = 0x000E` (existing `waddle_daemon_result_resp_t`) for explicit
supervisor exit after the guest is stopped and children reaped. Return `EBUSY`
otherwise. Reply before closing clients, then release the lease and exit. Old
supervisors rejecting this request leave rename/remove safely blocked; no guessing
from socket age, PID reuse, or pathname absence. Stop/kill wait for lease release
under their timeout before reporting full completion. No new guest protocol.

## 4. Step-by-Step Execution Sequence

### 4.1 Shared preparation and cleanup

Parse all arguments before opening resources. Resolve roots, validate names and
path capacities, acquire locks, and inspect pending journals. A normal mutation
recovers valid journals before proceeding; read-only commands report pending
recovery and do not perform it. Refuse destructive actions if recovery cannot
establish ownership. Check complete destination nonexistence including orphan
state directories, temporary entries, and config files. Invoke child tools with
an argv array (`execve`/`posix_spawn`), never a shell command string. Check fork,
exec, EINTR-safe wait, exit status, and stderr. Close all descriptors and release
locks on every return. Keep tool diagnostics bounded and available on failure.

### 4.2 Initialization

Validate capacity/name and select a readable base from explicit `--base-disk`,
then `$HOME/.local/share/waddle-vm-validation/windows.qcow2`,
`$HOME/.local/state/waddle/vm/windows.qcow2`, `/var/lib/waddle/windows.qcow2`,
and `build/windows.qcow2`, in that order. An invalid explicit base fails without
fallback. Canonicalize and verify a QCOW2 base and every backing ancestor.
No usable base is an error unless `--blank-disk SIZE` is explicit. SIZE is a
positive decimal followed by `M` or `G`, 1 MiB–2 TiB with checked multiplication;
a blank disk is not a bootable Windows installation. Create a staged QCOW2 using
`qemu-img create -f qcow2 -b ABSOLUTE_BASE -F qcow2 TARGET`, or a standalone image
for blank mode. Verify the image; a failed tool must never produce a touched
placeholder masquerading as a disk. Allocate CID under lock, write/reparse INI,
and publish disk/config using the transaction protocol. Do not auto-set default.

### 4.3 Lifecycle, shell, execution, and readiness

Resolve the target per section 2.3, read its valid config, and use only its named
runtime socket/CID. Startup holds the registry lock until supervisor ownership
and config handoff are acknowledged, then releases it while awaiting readiness.
Supervisor spawns virtiofsd, checks its socket inode without connecting, stabilizes,
starts QEMU, performs QMP handshake, and waits for the guest when requested.
Status/logs/list never create a daemon. Stop requests graceful shutdown or force,
reaps QEMU/virtiofsd, requests supervisor shutdown, and waits for lease release.
Restart performs the whole stop before startup. Shell/exec auto-start only a
resolved healthy device; initialize neither profile nor disk implicitly.

### 4.4 Rename

Validate both names and require a registered source. Equal names return success
without writes, after source validation. Any existing destination fails `EEXIST`.
Under lock establish full quiescence, including an idle supervisor's exit. Stage
new INI with updated `[subsystem] name` and owned disk/state/log paths. Preserve
external absolute paths and all settings/CID. Move owned state to the new name,
publish the new config, remove the old registration, and update a matching default
within one recoverable transaction. Remove only proven stale old runtime files;
new runtime paths are created at next start. Disk/backing contents are unchanged.
A shared global base image remains external; managed disks cannot be backing files
for another registration, or rename/remove must fail `EBUSY`.

### 4.5 Remove

Resolve explicit name, enumerate owned files and dependent disks, and prepare the
confirmation before locking for mutation. After confirmation, reacquire/revalidate:
any runtime owner, malformed ownership, shared backing dependency, or unrecognized
external path blocks removal. Stage config and owned state out of the active
namespace; clear a matching default and mark committed. With `--keep-data`, retain
state plus original config in a uniquely named retained directory and print it.
Otherwise delete staged owned data only after commitment, using directory-relative
no-follow traversal. Base images, external disks, export directories, and unrelated
logs are never deleted. A partially failed purge leaves a committed journal and
reports `io_error` plus retained cleanup location; retry recovery finishes cleanup.
A second removal of an absent name returns `not_found`, never infers another target.

### 4.6 Config and default mutations

Config set/reset requires full quiescence and applies all keys or none. Default
set requires a healthy existing profile but can select a running one; clear is
idempotent. Stage writes in the destination filesystem, flush metadata, and replace
atomically. Failed validation/writes retain the original values and default. Rename
and removal update the default within their same journal, not as a later best effort.

### 4.7 Clone, export, and import

Clone/export require fully quiescent sources and sufficient destination capacity;
flatten with `qemu-img convert -O qcow2 SOURCE STAGED_DISK`. Validate standalone
output and flush before publication. Clone copies portable configuration and
explicitly retains the same host exports (show these in dry-run/help), but always
has a fresh CID and independent writable disk. Export writes the manifest/hash
and atomically publishes a new output directory in its parent filesystem.

Import validates manifest, hash, image, bounds, and destination before writing.
Copy the standalone image into owned staged state (never execute/mount the backup),
assign a fresh CID, and generate INI with no host exports. Ignore source_name for
target identity; NAME controls registration. All three commands leave devices
stopped, defaults unchanged, and incomplete output unpublished. Disk copies may
be large; interruption and ENOSPC must clean or journal every partial allocation.

## 5. Concurrency, Threading & Synchronization

The single-threaded CLI obtains a whole-file `F_OFD_SETLKW` exclusive registry lock for
metadata mutations and startup handoff. Shared locks protect consistent discovery;
diagnostics never turn a shared read into an implicit write. Stable lock files
are opened with close-on-exec and no-follow, ownership checked; child utilities
cannot inherit locks. Blocked acquisition observes SIGINT and exits 130.

Supervisor lifetime uses a separate per-device open-file-description (OFD) lease
(`F_OFD_SETLK`, Linux) held by waddled until all
children are reaped and it exits. Registry mutations acquire registry lock first,
then try the device lease without waiting: busy returns `EBUSY`. Supervisors must
never acquire registry lock while holding their lifetime lease; startup receives
validated config from the CLI handoff. This ordering prevents deadlock and closes
the race between start and remove/rename. No mutation may equate a dead supervisor
with dead children: verify tracked child identities/ownership as well; ambiguous
or orphan live QEMU/virtiofsd means busy and requires explicit lifecycle recovery.

Transaction sequence: write/fsync a `prepared` journal describing every planned
staging path before allocating disk copies or moving existing files. Fsync its
containing directory, create and fsync staged files and their directories, then
perform recorded reversible
moves/replacements, fsync affected directories, write/fsync `committed` journal,
then purge rollback copies and journal and fsync directories. Before commitment,
recovery rolls back to the old registry; after commitment, it finishes cleanup.
Keep rollback copies until the durable commit point. Operations crossing config
and state filesystems use staging on each filesystem; never rely on cross-device
rename being atomic. Registry readers hold a shared lock and refuse an incomplete
journal, so they never observe a partly renamed profile. Recovery treats not-yet-created staged paths as absent, never as permission to
delete an unexpected replacement. Before moving existing data, persist its inode
identity in the journal. Recovery is idempotent
and validates device/inode identity and root containment before moving/deleting.

No shared-memory transport layout or atomics change. Existing supervisor runtime
state synchronization remains in force; metadata locking is host-local only.

## 6. Error Handling & Failure Modes

| Condition | Required result |
|:--|:--|
| Invalid name/value, unknown option, conflicting selector | EINVAL; exit 2; no effects |
| Missing source/default, duplicate destination | ENOENT / EEXIST; no effects |
| Full registry or CID exhaustion | ENOSPC; no publication |
| Running/idle supervisor, children, or disk dependency | EBUSY; never stop implicitly |
| Unreadable or malformed registered config | Show invalid profile; block unsafe mutation/allocation |
| Tool missing/fails, fork/wait fails | Preserve errno/tool diagnostic; rollback; no placeholder disk |
| Permission error, symlink, path overflow | EACCES / ELOOP / ENAMETOOLONG; never follow/delete outside roots |
| ENOSPC or write/fsync failure before commit | Roll back; keep recoverable journal if rollback fails |
| Crash/cancel before durable commit | Recover old registration, paths, and default |
| Crash/cancel after durable commit | Recover new state; complete cleanup once |
| Supervisor unreachable or PID identity uncertain | State unknown; mutation blocked, doctor explains |
| QEMU exit or missing backing base | Preserve logs/status; no success or substituted mock |
| Unsupported journal/backup version | Reject and retain files for diagnosis |

Removal is not secure erasure. Retained data consumes storage until deliberately
removed by the owner. Doctor `--repair` never deletes registered data, rewrites
malformed INI, reallocates colliding CIDs, or kills processes. Any unrecoverable
journal requires manual diagnosis with exact paths reported, not forced deletion.

## 7. Verification & Testing Criteria

### 7.1 Required automated coverage

- Parser/unit tests: NULL/empty names, 1/63/64-byte boundaries, non-ASCII in changed
  locales, slashes/dots/NUL, reserved command names, repeated flags, typed config
  ranges, arithmetic overflow, overlong paths, UNIX socket limits, malformed INI,
  journal/manifest bounds and versions, CID overflow/collision, registry capacity.
- CLI tests: every command/alias/help, text and JSON, default precedence, zero/one/
  many devices, dangling defaults, explicit transports, invalid mixed selectors,
  deterministic ordering, batch partial failure, guest argv and exit propagation.
- Mutation integration: create two independent devices; rename one preserving
  disk bytes/CID/config/mounts/default; old name fails and new name starts; config
  edits/reset persist; remove rejects live/idle owners and shared backing disks;
  stopped removal deletes owned files and clears default; keep-data preserves
  exact contents; untouched base/export files remain byte-identical.
- Backup integration: clone writes do not change source/base; export/import
  round-trip portable config and disk; import has new CID/no mounts/no default;
  corrupt hash, extra files, symlinks, external backing chains, existing
  destinations, and insufficient space reject without publication.
- Fault injection at every stage/write/fsync/rename/unlink/fork/exec/wait and
  journal phase, with process termination and repeated recovery; old or new state
  must be complete, never mixed. Concurrent init allocates unique CIDs; races
  between start, rename, config, and remove preserve runtime ownership.
- Stress: 1000 create/rename/config/clone/remove cycles in isolated XDG roots,
  concurrent registry readers, 32-profile capacity boundary, batch start/stop,
  process/descriptor/lock cleanup, and signal cancellation during disk copying.

Use temporary XDG/config/state/runtime roots and fixtures, never the developer's
profiles or disk. Mock qemu-img failures in unit tests; use real qemu-img for
QCOW2 dependency/copy verification. Existing transport mocks test host paths;
real Windows VM acceptance separately verifies two CIDs, independent disks,
terminal execution, shutdown, and renamed-device boot. Do not claim VM acceptance
from mocks or claim planned tests exist because existing CI is green.

### 7.2 Gates and definition of done

All public symbols follow repository naming/documentation rules. Run C suites with
ASan/LSan/UBSan (`-fsanitize=address,leak,undefined`), leak detection enabled, and
Zig parsers with `std.testing.allocator`; zero leaked bytes or leaked descriptors.
Concurrency/state-machine changes require TSan verification. Enforce at least 90%
statement and branch coverage for new codecs/protocol/memory structures and 100%
error recovery/timeout paths. Extend coverage to device registry and transactions,
rather than treating the existing CLI codec coverage as sufficient evidence.

Final feature completion requires every CLI row implemented and documented in
help, every required unit/integration/stress and Windows acceptance result
recorded, tracker at 100% with Time Ended, all branch commits attributed, and CI
passing at the PR head. `README.md` remains untouched. Specification review is
complete when the CLI contracts, safety/recovery model, tests, and tracker tasks
are synchronized; it does not complete the implementation.


### Implementation evidence: registry safety foundation

`daemon_device_get_config_dir`, `daemon_device_get_state_dir`, and
`daemon_device_get_socket_path` now compute paths only. Discovery traverses
absolute directory components with `openat(O_DIRECTORY|O_NOFOLLOW)` and reads
regular, user-owned metadata without contacting supervisors. `config_valid`
is an additional host-only `int` in `device_info_t`: one means the bounded INI
parsed successfully with explicit valid CID and disk fields; zero leaves the
profile registered but unhealthy. `is_running` is reserved until authoritative
status is collected by the command layer. Invalid profiles block allocation.
Readers take the existing stable registry lock when present; before the first
mutation a missing lock is allowed, as no publication has occurred. Writers
create the lock with 0600 permissions and retain its fd through CID allocation
and creation. OFD shared acquisitions can nest without an inner close releasing an outer lock. Exclusive operations use unlocked helpers and never reacquire the lock.
Creation currently fails utility errors without a placeholder. This foundation
does not yet implement the durable journal/publication sequence specified above.


### Implementation evidence: inspection and default command layer

`src/cli/device_commands.zig` parses bounded UTF-8 arguments and owns temporary
JSON through one per-call arena. C continues to own registry and socket operations.
`device list`, aliases, `device show`, `device default`, and `device config get`
use the schema v1 result envelope and typed six-key settings. List filters exclude
invalid configurations from stopped results. A responding daemon supplies the
observed state via the existing status protocol; an existing unreachable socket
is unknown. A missing socket is provisionally stopped until child/lease auditing
is completed in task #7. Show currently includes paths and effective settings;
backing-chain and ownership expansion remains tracked, so it is not yet the entire
final show contract. Default single-file persistence uses a 0600 staging file,
file fsync, atomic rename and parent fsync; post-rename fsync failure reports I/O
with uncertain durability. Rename/remove default changes still require the
multi-operation journal described in section 5.


### Implementation evidence: supervisor exit and stable leases

Shutdown v1 IDs 0x000D/0x000E are implemented. The event loop reaps children,
requires stopped/failed state with both child PIDs zero, rejects a nonempty payload
with EINVAL, and sends the result before exiting. Stop and kill request shutdown
and wait for the socket to disappear and the stable private `waddle.lock` OFD
lease to become available. Restart stops on any failed stop/shutdown and no longer
uses a fixed sleep as proof of teardown. The lease inode is never unlinked by
these client commands. OFD locks prevent another fd close in the same process
(e.g. threaded native tests) from accidentally dropping a supervisor's lease.
Teardown unlinks owned sockets while still holding the lease, then releases it.
Legacy supervisors rejecting shutdown remain blocked rather than guessed dead.
Startup serialization and orphan QEMU/virtiofsd auditing remain required in #7.

### Implementation evidence: bounded configuration editing

`daemon_config_edit` validates 1–6 distinct mutable-key requests in Zig before
rendering a replacement. It parses the original and the combined candidate,
rejects unknown keys, embedded line breaks/NUL, invalid UTF-8 and out-of-range
numbers, and reparses the final output. The caller supplies separate input/output
buffers; output is at most 64 KiB and is unusable on failure. There are no dynamic
allocations. Every occurrence of an edited key in its recognized section receives
the new value, including repeated sections; values in unknown sections remain
unchanged. Indentation, assignment spacing, trailing whitespace, line endings,
comments, exports and unknown keys remain byte-identical. Missing mutable keys
are appended in explicit section blocks. Reset uses the six documented defaults.
This pure codec has no filesystem effects; command persistence and quiescence
checks remain separate pending integration work.


### Implementation evidence: configuration-to-lease handoff

Before acquiring its lifetime lease, a named supervisor takes an OFD registry
read lock, rereads the registered healthy profile and reloads its configuration,
then acquires the lifetime lease before releasing the registry lock. The canonical
named runtime socket identifies this path; explicit legacy runtime directories
retain their separate configuration behavior. A missing or malformed registration
fails acquisition and cannot fall back to another device. The earlier init read
is provisional only. This order gives writers either a stopped profile before
startup or an occupied lease after startup. Nested discovery read locks do not
release the outer OFD lock. No registry lock is acquired while a lifetime lease
is already held. Tests verify nested-reader exclusion, config changes between
initialization and acquisition, and disappearance before acquisition.

Cleanup after failed lease acquisition never unlinks named supervisor/QMP/VirtIO-FS
sockets: that instance has not established ownership. A collision regression binds
an actual Unix socket, runs competing acquisition/cleanup in another process, and
verifies that the original socket pathname survives.

Offline config edits now validate first, acquire a nonblocking lifetime lease,
check same-user readable descriptor identities and obtain qemu-img check's exclusive
writer lock. Protected unrelated /proc entries cannot establish identity; the image
lock supplies the independent live-QEMU check. A complete 0600 candidate is fsynced,
renamed and its parent fsynced; dry-run creates no paths. Set/reset preserve mounts
and unknown content through the bounded Zig editor.

### Implementation evidence: offline storage transactions

`device_storage.zig` owns bounded metadata parsing and one per-call arena. The
journal is `transactions/<id>.json`; private staging trees reside at
`transactions/<id>/` in config/state roots. Move entries record root-relative
old/final locations and device/inode identities. Replacements are expressed as
old-to-rollback then staged-to-final moves; the reserved replacements array is
empty. Prepared rollback walks moves in reverse; committed recovery purges only
staging trees. External export journals pin the parent device/inode and a random
`.waddle-<id>` staging directory. Readers refuse pending journals; writers recover
under the registry lock before new mutation. The test-only environment variable
`WADDLE_DEVICE_TEST_CRASH` terminates at prepared/staged/move_N/committed boundaries
with exit 99, allowing subprocess acceptance to verify repeated recovery.

Creation validates options/base/size before allocating a name or CID. All new
files inherit 0077 umask and directories are fsynced at creation. Clone/import
convert to standalone QCOW2, and export stores the six portable settings with
streamed SHA-256/size verification. Rename appends authoritative identity/settings
sections without dropping original comments/unknown keys/exports. Removal checks
managed-disk containment and registered backing dependencies. Retention returns
the unique retained state directory with original.ini. No default is inferred for
mutations; JSON removal requires --yes. Expanded acceptance remains tracked.

### Implementation evidence: metadata durability and diagnostics

Configuration/default edits use the same prepared/committed journal and reversible
moves as device storage. Single-file atomic replacement alone is no longer their
recovery boundary. Recovery accepts only known operations and references to the
journal's source/destination names, its staging tree, its retention ID or pinned
export parent. Empty, publicly accessible or hard-linked profile metadata is
unhealthy. Import/export traverse supplied paths without following symlinks.

Inspection returns bounded JSON for ownership, backing chain, CID collisions,
required executable availability and runtime findings. Explicit repair holds the
registry then runtime lease, audits same-user process command references and disk
holders, and refuses any UNIX endpoint still present in `/proc/net/unix`. The
kernel table is parsed with bounded Zig slices; no connection to VirtIO-FS is made.
Only absent kernel endpoints with owned stale socket files are removed. Stable
lease files, registered data and processes are never removed by doctor.

### Implementation evidence: complete command routing

Named start/stop/restart/kill/status, logs and fs use one bounded Zig parser and
v1 result envelope. Lifecycle batches snapshot sorted registration names, recheck
each target, continue failures and return aggregate failure. Start keeps a shared
registry lease until the supervisor has acquired its lifetime lease, then releases
it before readiness waits. Stop/kill wait for supervisor exit and independently
verify quiescence; restart completes that path before start. Unavailable PID and
uptime fields are null; missing sockets with unresolved ownership report unknown.
Explicit targets bypass damaged/default selection metadata.

Named logs persist at state `devices/<name>/logs/`; the supervisor creates that
path only after protected config-to-lease handoff. Log queries read bounded tails
for the selected targets without spawning, reject symlinks, support JSON, and
follow uses a fresh iteration arena to keep memory bounded. Fs inspection reads
only declared mounts; fs test reproduces the existing host round-trip fixture
with exclusive temporary files and deterministic cleanup, including read-only
export checks. Doctor reports orphan state without deleting it; dry repair validates
its plan without changing paths. Shell remains a ConPTY stream and rejects JSON.
Raw socket transport development remains handled by the original C interface.


### Final storage error and cancellation contract

Management commands install a main-thread SIGINT scope and restore the previous
signal disposition before returning. The handler only sets a sig_atomic_t flag,
kills the currently owned qemu-img PID with SIGKILL, and preserves errno. The
parent always waits/reaps that child, closes its pipes, releases its arena and
locks, and emits the v1 cancelled error with exit 130. Hash loops and log-follow
loops observe the same flag. Guest exec/shell retain their existing signal path.
Utility stdout/stderr are independently bounded to 64 KiB. A capture/spawn/wait
error kills and reaps the utility through errdefer; no background copy continues
writing a staging disk after command return. Utility invocation never uses a shell.

Import pins the private source disk's device/inode, SHA-256 and size before
conversion, then checks all three again before publication. A changed source
rejects the import and rolls back staging. It never publishes a profile for a
copy made while its backup was changing. Diagnostic show/doctor use qemu-img
info -U to observe a live disk; mutation/dependency validation retains normal
QEMU image locking and the independent exclusive offline audit.

An error after journal replacement may have occurred after rename but before
directory fsync. Error cleanup rereads the on-disk journal phase instead of using
the last in-memory phase: prepared rolls back, committed finishes cleanup.
Partially written newly created metadata is removed by its creating scope.
Journal recovery refuses malformed/foreign identities instead of guessing.

The disabled-by-default WADDLE_DEVICE_TEST_FAIL=KIND:ORDINAL seam injects one
ENOSPC at the selected write/fsync/rename/unlink/fork/exec/wait boundary. Its
counter advances only for that kind, and only one ordinal fails, allowing cleanup
itself to run. Utility fork is checked before spawn, exec after spawn with an
owned child, wait after bounded output capture. It does not replace qemu-img.
WADDLE_DEVICE_TEST_CRASH still exits 99 at prepared, staged, each move_N and
committed. tests/integration/device_faults.py enumerates all reachable boundaries
for init/clone/import/export/remove/retention/default/config/rename, then performs
two repair passes and checks complete registrations/defaults, distinct CIDs,
managed disks and empty staging/journal directories. The current matrix covers
356 single failure/crash boundaries. These are deterministic boundary injections,
not an assertion that every kernel/filesystem failure mechanism was reproduced.


### Reproducible completion gates and real VM fixture

`make test` includes device command/storage/failure integration. `make device-stress`
performs 1000 init/clone/config/rename/remove cycles while a separate reader repeatedly
checks sorted complete registry snapshots and distinct CIDs. The observed run
completed 2641 concurrent snapshots and left no devices, journals or runtime files.
`make coverage` retains the existing protocol/argument/guest/path line gates and
adds aggregated kcov coverage for daemon_device.c, device_storage.zig and
device_commands.zig. Native test bodies are excluded; implementation source lines
from unit and isolated real-QCOW2 CLI tests are combined. Observed initial values
were 92.34%, 94.36%, 92.22%; final-head reports supersede these values.

`tests/device_branches.py [cli]` instruments ReleaseSafe LLVM IR source conditional
and switch successors in the owned storage codecs (relative/name/blank/settings,
bounded JSON wrapper, journal reference validator) and CLI argument/name parser.
Innermost debug scopes must belong to the exact implementation file. Source sites
are identified by function/line/column; optimized generic/inline copies contribute
to that same source site. Standard-library branches and line-zero compiler blocks
are outside the owned-source denominator; compiler-generated panic destinations
are separately reported. Both gates enforce >=90% observed source outcomes and
save branch metadata, hits and missed outcomes under build/coverage/branches.
Allocation failure is independently enumerated with std.testing.checkAllAllocationFailures;
all parser arenas/JSON/reference temporaries use std.testing.allocator and leak zero.
The observed gates are storage 92/99 (92.93%) and CLI 140/140 (100%). This is source
conditional/switch coverage for the named owned codecs, not a kcov branch-rate
placeholder or coverage assertion for every standard-library/kernel instruction.

`tests/acceptance/device_vm.py` requires an explicit opt-in, private /tmp/wdvm-* XDG
roots and two prepared real guests. The local Windows Server fixture uses the
existing immutable validation base, separate overlay disks, SYSTEM startup VSOCK
listeners, QEMU/KVM and real virtiofsd. Its preinstalled drivers require an isolated
launcher adapter for UEFI pflash variables, IDE boot disk and the existing export
tag waddle_export. That adapter changes only fixture launch arguments and guest
agent setup; device selection, supervisors, lifetime leases, VSOCK, ConPTY and
QCOW2 transactions execute the real application. No mock transport substitutes
for VM acceptance. Two devices use distinct CIDs and disks; independent persistent
markers and exit codes 37/42 are verified. A named shell returns 23 and restores
termios. Live mutation refuses, both devices stop, rename preserves disk bytes/CID,
renamed and second guests reboot with startup listeners and unchanged markers,
then sorted batch stop/removal leaves an empty registry. The complementary real
transport suite verifies argv/cwd/environment, stream/exit status, 16 MiB duplex,
timeout/peer loss, 39x101 PTY resizing/Ctrl-C/restoration and 32 reconnects.
Temporary /dev/vhost-vsock ACL access is restored exactly from the saved ACL;
fixtures never change the base or developer registrations. VM evidence is local;
CI repeats native Windows guest regression/heap checks and Linux acceptance.

Staged disk handles use a lexical defer before chmod/fsync, including export.
A failed disk durability operation closes the handle before error recovery. The
fsync ordinal seam covers staged disk flushes as well as metadata and rename
parent flushes; the final reported fault count includes these additional sites.

Text removal confirmation polls stdin in 100 ms intervals and reads at most 65
bytes without automatic EINTR retry. SIGINT cancels with exit 130 immediately,
EOF/mismatch/overlong input also cancels, and no registry/runtime write lock is
held while prompting. PTY acceptance signals the waiting prompt without a
newline and proves the original profile and disk are preserved. The final disk
flush-expanded crash/failure matrix contains 360 boundaries.
