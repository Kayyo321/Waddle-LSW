# Desktop artifact materializer contract

Status: qualified bounded host materialization milestone for TODO #28; exact
source-bound tests and coverage are in
`desktop_artifacts_evidence_2026_10_25/qualification.json`. This
contract does not claim that desktop integration is installed, registered with
the compositor, trusted by a desktop shell, or supported on a specific host.

## Scope and composition boundary

`src/interop/desktop_artifacts.zig` is an allocation-owning, deterministic
producer. Given one device identity, the trusted absolute path of the Waddle
launcher, a filtered set of validated catalog items, and optional already
encoded PNG icon bytes, it produces immutable relative-path artifacts ready to
be handed to the existing `manifest_store.store_t.commit()` transaction API.
Each artifact carries a cumulative schema-1 candidate-manifest snapshot that
preserves the caller's prior records and adds exactly that artifact's record.
The array order is the publication order; each snapshot is suitable for one
`commit()` call, which the existing store requires to add exactly one record.
It produces exact artifact record IDs, paths, SHA-256 digests, modes, and
content. Publication remains owned by `manifest_store`; the producer itself
does no filesystem I/O. Multi-file global atomicity is not claimed: each store
transaction is durable and recoverable independently, so the higher-level
installer must sequence publication and tolerate/reconcile a partial set after
interruption. Tests include a real temporary rooted-store consumer that
publishes and recovers these artifacts through the existing API.

The milestone covers portable generation and real temporary rooted-store
publication/recovery qualification. It does not enumerate Windows
catalogs, derive canonical executable identity, inspect XDG configuration,
run `gtk-update-icon-cache`, call `gio`, mark shortcuts trusted, extract icons,
or claim runtime desktop-shell behavior. Menu XML and `.directory` artifacts
are generated only when the caller requests the menu; standalone per-app
entries can be marked `NoDisplay=true` for `menu_apps=none` or filtered apps.
An item marked `uncatalogued` is rejected as unsupported because this producer
requires a catalog key for its frozen `Exec` form. Desktop shortcuts are
outside this milestone.

## Bounds and trust inputs

The producer accepts at most 4096 app items/records, 16 MiB of aggregate
borrowed input bytes, and 32 MiB of aggregate returned logical bytes. Returned
bytes include each artifact payload, every cumulative candidate manifest,
each record ID, output path, kind spelling, and 64-byte lowercase digest.
Repeated cumulative snapshots are each counted in full. A generated artifact
and each icon are at most 4 MiB; icons must have the PNG signature. Each
generated file basename is at most 255 bytes.
Aggregate accounting uses checked arithmetic. Candidate manifest serialization
sorts one owned record snapshot and emits one shared JSON grammar twice while
all borrowed input slices remain immutable. The count pass saturates arithmetic
overflow to `maxInt(usize)` with an explicit overflow flag; one size/overflow
gate rejects the count before allocating the exact output slice. The fill pass
uses bounds-checked slices and asserts that its final cursor equals the count.
No duplicated encoded-size formula or per-append allocation is used. Other
owned intermediate buffers remain bounded by the validated input limits.
Every text input must be valid UTF-8, NUL-free, and within the per-field limits
listed in the public Zig declarations. Text destined for Desktop Entry `Name`
and `Comment` must contain no C0/C1 control characters; if a catalog display
name contains controls, the frozen rule substitutes its app key. Invalid UTF-8,
NUL, oversized fields, unsafe identity bytes, duplicate app keys, duplicate
output paths, or malformed trusted launcher paths fail the entire call without
changing the caller's output value.

The launcher path is a caller-supplied trusted host executable location,
absolute and canonicalized by the install layer. This module checks only
absolute POSIX spelling, UTF-8, bounds and forbidden control/NUL bytes; it does
not prove filesystem identity or executability. `=` and `%` are rejected in
the executable path. In validation, GLib's `GDesktopAppInfo` ignored a matching
existing executable whose path contained a literal percent sequence in `Exec`
even with `%%` escaping, so this producer does not admit such launchers. The
generic quoting helper still doubles literal percent bytes, and the frozen
standalone `%F` field code remains unquoted. The launcher path is emitted as
one quoted Desktop Entry `Exec` argument and in `TryExec`; no shell is involved. Device
names and app keys must already satisfy the frozen sanitizer alphabets and
lengths. The materializer never invents or lowercases identities.

Menu inclusion is an explicit input (`Installed`, `All`, or `None`) plus the
catalog flags and seed flag. `Installed` includes items not marked System and
items marked Seed; `All` includes all; `None` includes none. The generator
always emits each requested app `.desktop` file, setting `NoDisplay=true` when
the selected filter excludes it, so taskbar/app-id association remains
possible. Uncatalogued-window entries are rejected because they need a
separate trusted guest executable identity and title truncation path. The
`p-` identity must contain exactly 32 lowercase hexadecimal characters after
the prefix. The application ID and icon name are exactly
`waddle.<device>.<app_key>`.

## Exact generated formats

For every item, output path is
`applications/waddle.<device>.<app_key>.desktop`, kind `desktop_entry`, mode
0644, and ID `desktop:<device>:<app_key>`. The bytes are UTF-8 with LF line
endings and the exact frozen §3.8.5 key order and values. Desktop Entry value
escaping maps backslash to `\\`, newline/tab/carriage return to `\n`, `\t`,
`\r`, respectively; other control scalars are forbidden. `Exec` first quotes
each argument using the freedesktop reserved-character rules, doubles literal
percent signs in arguments (so a path `%F` remains literal, not a field code),
then applies Desktop Entry value escaping to the entire quoted argument. A
literal dollar therefore has two backslashes in the file and a literal
backslash has four. Literal `"`, backtick, `$`, and backslash are escaped
inside the quoted argument. The final `%F` field code alone remains unquoted.
Launcher paths containing `=` or `%` are rejected. Arguments are exactly
launcher, `open`, `--device`, device, `--app`, app key, `%F`; no concatenated
shell command is constructed. `TryExec` is the same launcher path with
Desktop Entry value escaping, not Exec-argument quoting. The
materializer's tests pin hostile spaces, quotes, backslashes, dollar
signs, percent signs, Unicode and invalid-control behavior to byte-for-byte
goldens.

When menu output is requested, one `applications-merged/waddle-<device>.menu`
(kind `menu`, ID `menu:<device>:applications-merged`) and one
`desktop-directories/waddle-<device>.directory` (kind `menu`, ID
`menu:<device>:directory`) are emitted under Config and Data roots respectively
as specified by the existing store root allowlist. XML text and attributes
are escaped (`&`, `<`, `>`, quotes in attributes, and apostrophe in
attributes); values outside the XML 1.0 character range fail closed. The menu
contains `<Directory>waddle-<device>.directory</Directory>` to bind the
directory file. Menu entries use the frozen `X-Waddle-<device>` category and
desktop-file IDs. The directory file
uses the same escaped `Windows (<device>)` display name and matching icon
identity. There is no localized content in this milestone.

For each supplied icon size in the frozen set (16, 24, 32, 48, 64, 128, 256),
the PNG bytes are copied unchanged to
`icons/hicolor/<size>x<size>/apps/waddle.<device>.<app_key>.png`, kind `icon`,
mode 0644, ID `icon:<device>:<app_key>:<size>`. Duplicate sizes reject the
request. PNG dimensions are not decoded by this producer; the icon provider
must supply dimensions matching the declared frozen size. This boundary is
explicit so portable tests do not claim image decoder validation.

## Output ownership, manifest composition, and failures

The returned `artifacts_t` owns every artifact path, byte buffer, record ID,
and candidate-manifest buffer allocated from the caller allocator. The caller
must call `deinit()` exactly once; it releases all owned allocations and
resets the result. Borrowed input slices are never retained. The generator is
reentrant for independent outputs; callers must serialize mutation/deinit of
one output object. Returned content hashes use SHA-256 over exact bytes and
lowercase hex. Candidate manifest is schema 1, records sorted by record ID,
with exact artifact paths under the frozen root-relative allowlist, `prior`
null, current creator version provided by input, and a deterministic
Gregorian-calendar-valid UTC-second timestamp (seconds 00..59; leap-second
spellings are rejected) provided by input (the module does not read the clock).
The first candidate includes validated existing records plus the first new
artifact; each following candidate includes the prior candidate's records plus
exactly one new artifact. Existing matching IDs and paths are explicit
conflicts (this milestone does not claim idempotence); any conflict fails
before returning a partial array. `manifest_store.commit()` and
`manifest_store.recover()` own publication/recovery; this function does not
implement filesystem rollback itself.

Errors are typed: `InvalidInput`, `InvalidUtf8`, `InvalidControl`,
`InvalidExecPath`, `DuplicateIdentity`, `DuplicateArtifactPath`,
`InvalidIcon`, `UnsupportedUncatalogued`, `TooManyItems`, `TooMuchInput`, `OutputTooLarge`,
`ManifestTooLarge`, `UnsupportedSchema`, `InvalidManifest`, `OutputNotEmpty`,
and `OutOfMemory`. The output argument must be initialized empty and its prior
owner must call `deinit()` before reuse. On any error the caller's output value
is byte-for-byte unchanged and all temporary allocations are freed. The API
is synchronous and does no I/O or global mutation.

## Parent acceptance allocation

Parent-approved allocation: desktop-entry generation is 20 percentage points,
menu and directory generation is 10 points, and managed-icon artifact
identity/publication data is 10 points. These allocations are earned only
after exact output, bounds, ownership, allocator-failure and coverage gates
pass. A further 20 points is reserved for rooted publication/recovery; it is
earned only after the temporary rooted-store consumer exercises real
interruption and recovery. Association/runtime is 20 points, desktop mirror
10 points, and native installed-desktop/shell qualification 10 points. This module
earns no credit for those remaining areas unless their specific gates pass.

## Verification

Tests cover exact `.desktop`, menu XML and `.directory` bytes; every
supported icon size and relative path; all menu filters and System/Seed
combinations; hostile but valid Exec arguments; escaping and Unicode; NUL,
invalid UTF-8, control characters, relative launcher path, unsupported icon
size, duplicate IDs/paths, record ordering and digest; exact limits and
overflow; unchanged output on failure; allocator failure at every allocation
index through first successful completion; and zero leaks under
`std.testing.allocator`. The temporary rooted-store test uses the real commit
and recovery APIs, then requires `desktop-file-validate` against the published entry. Each
artifact is tested with actual PREPARED journal plus published artifact/manifest
bytes, rollback twice, and actual COMMITTED journal followed by roll-forward
twice. Earlier committed records must retain their exact content hashes. A separate test-only C consumer is
compiled with `cc` and `pkg-config --cflags --libs gio-unix-2.0`; it loads the
generated file with `GDesktopAppInfo`, uses GLib's parsed command line to
recover argv, and compares every argument against the real temporary helper
path and frozen launch parameters. It never launches the helper. This consumer
is built with ASan, LeakSanitizer (detect_leaks=1) and UBSan; it frees all
GLib objects/errors/argv on every exit. This consumer requires the `desktop-file-utils` and `libglib2.0-dev` test packages; neither is
a production dependency. The repository's retained LLVM coverage runner qualifies the whole production
module at >=90% source lines and taken arcs. Zig ownership is checked with
`std.testing.allocator` and exhaustive allocation-failure sweeps; the C consumer
is the component executed under ASan/LSan/UBSan. Neither gate proves a live
installed desktop shell or launches a guest application.
