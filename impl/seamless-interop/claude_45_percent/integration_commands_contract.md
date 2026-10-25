# Integration command contract (TODO #33, tranche 2)

Scope of this contract: the `waddle integration` command for the per-user
desktop-entry handler (IMPL_DESC 3.14) and its MIME defaults. It is one producer
of the schema-1 manifest. This contract does not claim the all-producers
milestone: guest scope, shortcuts, and `--system` are not implemented in this
build and fail with `not_implemented` or `permission_denied` as described below.

## Command surface

```
waddle integration install [--device N] [--json]
waddle integration remove [--device N | --all] [--json]
waddle integration repair [--device N] [--force] [--json]
waddle integration status [--device N] [--json]
waddle integration migrate [--json]
```

- `--device N` selects one device-scoped handler. MIME defaults are device-less
  and are touched only when no `--device` is given.
- `--all` on remove selects every device, equivalent to no `--device`.
- `--force` on repair replaces a modified handler artifact (see repair).
- `--system` requires euid 0. This build has no system producers, so root gets
  `not_implemented` and a non-root caller gets `permission_denied`. No store
  access happens in either case.
- `--guest` and the shortcut operation are `not_implemented`.
- `--help` prints the usage and exits 0 without touching state. Help may
  appear as any later operand.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success. Drift (a missing or modified artifact reported by `status`) is reported and still exits 0. |
| 1 | Blocked or failed: a refused change, a user-modified artifact or value kept, a manifest error, a journal needing recovery, or an I/O failure. |
| 2 | Usage error: unknown subcommand, unknown or duplicate flag, or a missing operand. |
| 125 | Resource failure: allocation failure (`resource_error`) or the process could not launch. |

## JSON output (`--json`)

One object is written to stdout with a trailing newline:

```json
{"schema_version":1,"command":"integration","operation":"install","ok":true,"results":[...]}
```

- `schema_version` is always `1`. A future change to this envelope increments it.
- `command` is always `"integration"`. `operation` is one of `install`,
  `remove`, `repair`, `status`, `migrate`.
- `ok` is true exactly when the exit status is 0.
- `error` is present only on failure, as `{"code": ..., "message": ...}`; on
  success it is `null`.
- `results` is an array of rows in the order the operation produced them.
- A row has `id`, `kind`, `path`, `state`, `action`, and `blocked`. `device` is
  included only when the record has one, and `backup` only when a replacement
  retained a backup. `kind` is `handler_entry` or `mime_default`.

Human output prints one line per row as `<id>: <state> <action>` followed by
optional ` device=<d>`, ` backup=<txid>`, and the path. Failures print
`waddle integration: <code>: <message>` to stderr.

## Failure codes

| Code | Source | Exit |
|---|---|---|
| `usage` | parser | 2 |
| `resource_error` | allocation failure | 125 |
| `schema_newer` | manifest schema is newer than this Waddle; nothing is modified | 1 |
| `version_downgrade` | manifest `waddle_version` is newer than this Waddle | 1 |
| `busy` | another process holds the manifest lock | 1 |
| `permission_denied` | unsafe permissions, ownership, or symlink in the integration scope | 1 |
| `recovery_required` | a journal needs manual inspection; its evidence is kept | 1 |
| `artifact_conflict` | the artifact changed while the operation ran | 1 |
| `manifest_mismatch` | the manifest record does not match the requested change | 1 |
| `invalid_environment` | HOME or an XDG variable is not absolute | 1 |
| `not_implemented` | guest, shortcut, or system producers in this build | 1 |
| `not_found` | a required file or record is absent | 1 |
| `io_error` | any other filesystem failure, including a `mimeapps.list` larger than 1 MiB | 1 |

## Ordering

Every operation takes the store lock through the store and recovers any pending
journal before it reads or changes anything.

- **install**: the handler record and artifact are committed first. The MIME
  steps run only when the handler step succeeded, the handler is enabled by
  policy, and no `--device` was given. The MIME types are processed in the
  frozen order of IMPL_DESC 3.14 (`application/x-ms-dos-executable`,
  `application/x-msdownload`, `application/vnd.microsoft.portable-executable`,
  `application/x-msi`, `application/x-ms-shortcut`,
  `application/x-msdos-program`, `application/x-ms-installer`). Each type is
  registered, then edited.
- **remove**: for each MIME record, the default is restored first and the
  record is forgotten second. Then the handler record and artifact are removed.
- **repair**: the handler only. MIME defaults are not repaired by this command.
- **status**: the handler rows from the store, then one MIME row per record
  read from `mimeapps.list`.
- **migrate**: rewrites the root metadata through the first manifest record. It
  reports `nothing_to_migrate` for an empty manifest.

## Handler behaviour

- `install` writes `waddle-open.desktop` under `$XDG_DATA_HOME/applications`
  with the launcher path, `--device`, and the MIME list of IMPL_DESC 3.14. It
  reports `installed`, `unchanged` (byte-identical to the record), or
  `refused` (an unrecorded file or a different artifact, exit 1; nothing
  changes).
- `remove` deletes an unmodified artifact (`removed`), reports absence
  (`not_installed`), and keeps a modified artifact (`kept_modified`, exit 1).
  An artifact that is already missing removes the record (`record_removed_artifact_missing`).
- `repair` recreates a missing artifact (`repaired`). A modified artifact is
  blocked unless `--force` is given. With `--force`, the artifact is replaced
  through `store.replace` and the exact previous bytes are kept in a private
  backup named in the row's `backup` field. A foreign object is blocked and is
  never touched.
- `status` reports `ok`, `modified`, `missing`, or `foreign` and always exits 0.

## MIME defaults

Records: kind `mime_default`, id `mime:<subtype>`, path `mimeapps.list` (relative
to `$XDG_CONFIG_HOME`, default `$HOME/.config`), `device` absent, `mime` set to
the full type, `sha256` set to the SHA-256 of the handler leaf that is written,
`prior` set to the previous value (text) or absent.

File edits go through `src/cli/integration_mime.zig` and the pure rules of
`src/interop/mimeapps.zig`:

- Reads are bounded to 1 MiB. A larger file fails with `io_error`.
- A write goes to a temporary file in the target's directory, which carries the
  target's permission bits (0644 for a new file), is synced, and is renamed over
  the target. A failed write removes the temporary file.
- A symbolic link to `mimeapps.list` is followed (up to 8 hops). The target is
  replaced and the link is kept. A dangling link names the file to create. A
  loop fails.
- Only the `[Default Applications]` group is edited; other groups and lines are
  preserved. A missing file is created. A missing group is appended.

Install rules:

- A type with no record: the current value is read. A value equal to the
  handler leaf is recorded as absent prior (it was already ours). Any other value
  becomes the recorded prior and is overwritten. The record is registered
  (`register_record`) before the file is edited. Row action `registered`.
- A type whose record exists: a file that already holds the handler leaf is
  `unchanged`. A file whose value equals the recorded prior is repaired by
  setting the handler leaf again (`repaired`). Any other value is kept
  (`kept_modified`, exit 1); the user value is never overwritten.

Remove rules, in order per record:

- The file is restored only while the key still holds the handler leaf: it is
  set back to the recorded prior, or the key is deleted when no prior existed.
  Row action `restored`.
- A key that holds another value is kept, row action `kept_modified`, exit 1.
- A key that is absent (for example after a crash) is a no-op, row action
  `not_applied`.
- A missing file is row action `file_absent`.
- The record is then forgotten through `remove` (record-only). The row is
  `missing`.

Status rows: `ok` for the handler leaf, `missing` for an absent key or file,
`modified` for any other value.

Known residue: when install creates the `[Default Applications]` group, removal
restores the keys but keeps the empty group header. Removing the header would
need a record of whether the group was created, which the schema does not hold.

## Recovery and crash behaviour

- Each record is journaled before its file is edited, so a crash between the
  two steps leaves a record whose restore guard is a no-op. Removal then reports
  `not_applied` and forgets the record. A crash between the restore and the
  forget leaves the record; the next removal restores nothing, reports
  `not_applied`, and forgets it.
- A store journal that cannot be validated stays in place. Its operation fails
  with `recovery_required` and the evidence is kept for inspection.
- A legacy icon record written by earlier code (`hicolor/<size>/apps/<leaf>`)
  is still inspected and removed (see the store contract). New commits never
  write that form.

## Verification (this commit set)

- `zig test` of `src/cli/integration_commands.zig`, `src/cli/integration_mime.zig`,
  and `src/interop/integration_producers.zig` through the lane target
  `interop-integration-unit-test`.
- `tests/interop/integration_commands_e2e.py` runs `build/waddle integration`
  with a scrubbed environment and a temporary HOME and XDG layout, covering
  exit codes, XDG resolution, `--system`, and a relative HOME.
- Allocation-failure sweeps: the plain suite fails every allocation index of each
  command, each MIME edit, and each restore, and requires every induced failure
  to leave allocated and freed bytes equal. The instrumented coverage run
  (`WADDLE_INSTRUMENTED_SWEEP`) stops each sweep after 300 indices so it finishes
  in bounded time. The bound applies only to that run.
- Coverage gates from `tests/av/coverage.py`: `integration_commands`,
  `integration_mime`, and `integration_producers` each at least 90% branch
  coverage over their whole files.
