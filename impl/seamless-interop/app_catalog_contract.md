# App catalog snapshot and diff contract

Status: implementation contract for the bounded TODO #27 snapshot/diff and
read-only native enumeration milestone. The protocol and app-key rules below
remain those frozen in `IMPL_DESC.md` §3.8. This document does not enable
catalog wire messages or host artifact creation.

## Scope and boundary

The core accepts borrowed, already-read catalog candidates, validates them,
derives frozen application keys, deduplicates by exact identity, builds an
immutable owned snapshot, and computes deterministic Add/Update/Remove
records between snapshots. A Windows-only source adapter enumerates AppsFolder
and Start Menu/Desktop shortcuts without launching applications or writing
registry/files. The adapter supplies candidates to the same core. Icons,
uninstall registry enrichment, seed selection, watchers/debounce, wire
serialization/runtime, host desktop files/icons/backup ownership and launch
are separate milestones and are not implemented by this API.

## Bounds and input validity

The API accepts at most `AppCatalogMaxEntries` (4096) candidates and at most
`AppCatalogMaxBytes` (16 MiB) of aggregate variable-length input bytes across
all candidates. This input bound includes identity, display name, launch
strings, AUMID, source identifier, and package-family bytes, even for
duplicates that will not be selected. The independently checked snapshot
storage bound includes generated keys and the selected candidates' copied
identity, display name, launch strings, and AUMID. Source identifiers and
package-family values are borrowed for selection/flag derivation and are not
copied into the snapshot. Limits are checked with overflow guards before
allocation. Candidate text is borrowed only for the duration of
the call and must be valid UTF-8, contain no U+0000, and satisfy the frozen
codec limits: display name 1..255 bytes; optional target path 1..4096;
arguments 0..4096; optional working directory 1..4096; AUMID 1..512. Empty
target/working-directory values are valid for AUMID-activated packaged apps.
Path-identity candidates require a nonempty target path.
Invalid or over-limit input fails the whole build and leaves the caller's
output snapshot byte-for-byte unchanged.

Each candidate has exactly one identity form:

* AUMID: exact original UTF-8 bytes. The frozen
  `interop_app_key_from_aumid()` builds its `a-` key. No case folding or Unicode
  normalization is performed.
* Path: a nonempty, NUL-free, absolute canonical Windows target encoded as
  invariant-uppercase UTF-16LE code units, without a terminator. Production
  identity is supplied only by the Windows adapter after canonicalizing the
  actual target; raw wire fields and caller-provided display paths never enter
  that provider. The C core has a documented trusted-candidate precondition and
  computes `p-` plus lowercase hex for the first 16 bytes of SHA-256 over the
  supplied identity bytes, exactly as frozen in `IMPL_DESC.md` §3.8.2.

The Zig validation boundary checks UTF-16LE well-formedness, absolute drive/UNC
form, ASCII lowercase rejection, and canonical component spelling (`.` and
`..` components are forbidden). It does not prove Unicode invariant uppercase
or filesystem identity. The Windows adapter is the trusted path-identity
provider: for each present target it opens it with
`CreateFileW(FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
FILE_SHARE_DELETE, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)`, obtains
`GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)`, and closes
the handle exactly once. This follows filesystem aliases;
`GetFullPathNameW` alone is lexical normalization and is not identity evidence.
The adapter converts `\\?\C:\...` to `C:\...` and
`\\?\UNC\server\share\...` to `\\server\share\...`, matching ordinary
DOS/UNC spelling, then applies
`LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE)` and encodes UTF-16LE
without a terminator. If open, canonicalization, uppercase conversion, or
close fails, enumeration returns a typed native error; it never hashes the
original spelling as a fallback. The adapter hashes only the identity bytes it
derived in that same operation. The snapshot/diff core never opens target files
and is identical on Windows and Linux. Linux fixture tests exercise syntax and
hashing with explicitly synthetic canonical identities; they do not establish
Unicode case or filesystem-identity evidence. Canonical identity does not
claim the target is safe to launch.

Only `InteropAppPackaged` and `InteropAppDesktopShortcut` may be supplied in
candidate flags. Packaged is derived from a valid, nonempty package family
name: 1..36 ASCII alphanumeric/dot/dash name bytes, one underscore, then 13
lowercase ASCII alphanumeric publisher-id bytes. DesktopShortcut is set only for user/public Desktop shortcut sources,
never Programs. The core ORs DesktopShortcut over all exact-identity
duplicates and derives Packaged if any duplicate has a valid package family.
System, Seed, and Uninstallable are always clear in this milestone. Uninstall
registry enrichment and explicit seed production require separate producers.

## Identity grouping and deterministic merge

Candidates are grouped by derived app key and exact identity. If one app key
maps to two distinct identities, build fails with `IdentityCollision`; it
never drops or overwrites either entry. Exact duplicates merge as follows:

1. The primary candidate is selected by source priority: AppsFolder with valid
   AUMID, user Programs, common Programs, user Desktop, public Desktop.
   AppsFolder items without a valid AUMID use the common Programs rank; only
   AppsFolder observations with AUMIDs receive the top rank.
2. For equal priority, the candidate with the bytewise lexicographically
   smallest source identifier wins. Source identifiers are nonempty canonical
   UTF-8 source names supplied by the adapter (AppsFolder parsing name or
   absolute shortcut path); ties are broken by target, arguments, working
   directory, display name, and then original input index. This total ordering
   chooses one complete launch tuple; target, arguments, and working directory
   are never spliced from different candidates.
3. The selected candidate supplies display name, launch tuple, and AUMID.
   DesktopShortcut and Packaged are ORed/derived as stated above.

The resulting snapshot is strictly sorted by app-key bytes. Input order does
not affect the result. The snapshot records `duplicate_launch_conflicts` as
the count of nonselected exact-identity candidates whose target/arguments/
working-directory tuple differs from the selected candidate. This is a
diagnostic count; deterministic selection still yields one complete published
entry and does not silently combine conflicting tuples.

## Ownership, atomicity, and lifecycle

Snapshot storage is allocated from the caller-provided allocator and owned by
the returned snapshot. `app_catalog_snapshot_deinit()` releases it and resets
the object to the documented empty state; it is safe on that empty state.
Snapshot fields and their byte slices are immutable and remain valid until
deinit. No global mutable state is used. Distinct operations may run
concurrently; callers must serialize mutation/deinit of the same snapshot.

Build and diff have strong output guarantees: on any error, output objects and
their referenced bytes are unchanged. They construct temporary owned state,
then publish it only after all validation/allocation succeeds. Candidate data
is never retained. Diff output owns a bounded array of change descriptors but
borrows entry data from the before/after snapshots; both snapshots must remain
alive and immutable until diff deinit. Diff changes are sorted by app key, with
Remove for keys absent after, Add for keys absent before, and Update only when
published fields differ. A no-op diff has zero changes.

## Windows enumeration rules

The Windows adapter is read-only and initializes a single-threaded COM
apartment for the duration of enumeration. It balances each successful `CoInitializeEx`
with `CoUninitialize`, releases every COM interface and task-allocated path on all
returns, does not call `IShellLink::Resolve`, `ShellExecute`, or process
creation, and does not access the uninstall registry in this milestone.
Windows enumeration requires a caller-owned, initialized cleanup owner. A
failed `CloseHandle` or `FindClose` records the exact native handle and close
kind there, aborts enumeration before snapshot publication, and stops further
source traversal. The bounded owner can retain the 65 nested search handles
from the depth limit plus one target metadata handle. The caller must keep it
alive and serialized and use the retry API until all handles close; a pending
handle prevents reuse of the owner. The core snapshot/diff path does not own or
close native handles.

AppsFolder children contribute their display name, AUMID when valid, target
parsing path when present, and package family when valid. An AUMID entry with
no parsing target remains a valid catalog candidate with empty optional launch
fields. Destination-list separator items are skipped. Shortcuts are read from user/common Programs and
user/public Desktop roots; Programs recursion is bounded. Every directory
reparse point is skipped. Traversal is bounded by the global entry cap, a
documented node cap, path/string limits, and a monotonic deadline; an optional
cancellation callback is checked between nodes. Limits/cancellation return a
typed error and discard the incomplete snapshot. Invalid individual shell
items that lack required readable metadata may be skipped without logging
their application metadata. A stale/non-file target with no canonical identity
skips that individual path-key item. An otherwise valid AUMID item retains its
activation identity with empty optional target fields when canonical target
metadata is unavailable. Any failed native handle close remains terminal and
retains the exact handle; it is never downgraded to a skipped item. Malformed global state,
collision, cap exhaustion, or allocation failure fails the whole enumeration.
Desktop shortcut origin is preserved for the flag rule above.

Source enumeration never claims that a catalog item is launchable, installed,
system-provided, or safe. It observes shell metadata only. Native enumeration
is qualified separately on Windows; Linux fixture tests qualify the core and
testable path/identity rules only.

## Errors and acceptance

Public functions return explicit status values for invalid arguments/text,
quota, allocation failure, invalid canonical identity, key collision,
unsupported platform, cancellation, timeout, and native enumeration failure.
No error exposes an unbounded message or logs application metadata. Errors do
not mutate caller snapshots.

Acceptance requires deterministic fixture tests for identity vectors,
collision refusal, duplicate merging and source priority, every validation
limit, snapshot atomicity, every diff kind/order/equality field, allocator
failure at every allocation index, cleanup-owner handling for real file and
search handles, injected close failures followed by real retry-close, and
sanitizer-clean teardown. Whole-module
line and taken-branch coverage must be at least 90% without source exclusions.
The Windows fixture test creates its four shortcut roots and `.lnk` inputs only
inside a uniquely created temporary directory, then removes only those files
and directories. A private root-mapping entrypoint points the real shell/link
and filesystem enumerator at those roots; it does not replace COM, shortcut
parsing, metadata handles, or path identity operations. The test records the
SHA-256 of each generated `.lnk` fixture before cleanup. This private fixture
entrypoint also has a one-shot close-error callback used only to make genuine
file/search handles take the quarantine path; it never consumes or substitutes
for those handles. Tests then retry closure using the real Win32 APIs. A separate public
enumeration smoke uses the user's actual AppsFolder and known folders in
read-only mode. Windows CI must compile and run these tests; Linux tests do not
claim native Windows API runtime coverage. Catalog wire transport,
watchers, icons, host artifacts, backup/restore, and runtime capability
advertisement remain out of scope.


Native path traversal holds at most65recursive search frames. Each frame owns
one heap allocation containing two32768-unit UTF-16 path buffers (131072bytes)
and frees it on empty-directory, error and normal exits. Thus traversal path
storage is bounded by8519680bytes independent of input tree breadth; it does
not reserve these buffers on each native Windows stack frame. This temporary
native working set is separate from the16MiB catalog input/snapshot cap.
An allocation failure aborts the incomplete enumeration with OutOfMemory.
The native fixture traverses24real nested temporary directories and verifies
unchanged dedup results plus complete reverse directory cleanup.
