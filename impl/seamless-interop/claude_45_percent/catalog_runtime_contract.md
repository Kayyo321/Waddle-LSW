# Host catalog runtime contract (TODO #27 wire/runtime and watch/enrichment portable halves, TODO #28 runtime/association)

Status: lane-level implementation contract for the portable host side of guest catalog
synchronization. It implements the host state machine that consumes guest `Catalog*` events,
requests and reassembles icons, and publishes desktop entries, menus and icons through the
rooted manifest store. It is not a native acceptance record, and it earns no credit by itself.
Parent references: `IMPL_DESC.md` 3.2.5, 3.2.6, 3.8.1 to 3.8.5, 4.3, 8.45;
`app_catalog_contract.md`, `desktop_artifacts_contract.md`, `icon_pipeline_contract.md`,
`png_writer_contract.md`, `manifest_store_contract.md`, `manifest_replace_contract.md`,
`session_control_host_contract.md`, `host_feature_runtime_contract.md`.

## 1. Scope

Inside this contract:

* Per-session snapshot of guest catalog applications, bounded to 480 entries (`MaxApps`) and 16 MiB
  of key and name bytes, with the frozen app-key rules and the desktop-file name length rule.
* Full-sync transactions: `CatalogBegin` opens, `CatalogEnd` closes and removes applications not
  announced since the matching Begin. Add, Update and Remove outside a transaction are incremental.
* Icon retrieval: per (app, frozen size) requests with `IconRequest`, a bounded outstanding window
  of 8, a 10 s begin and idle deadline, bounded retries, reassembly through the codec's transfer
  accounting, SHA-256 verification, full PNG validation (CRC-32 per chunk, IHDR first, IEND last,
  at most 64 chunks), a 1 MiB per-icon limit, and a 32 MiB budget covering retained icons and
  receives in flight. A content-hash change cancels that application's open transfers.
* Publication: desktop entries, menu and directory artifacts and icons are rendered by
  `desktop_artifacts.build` and committed or replaced through `manifest_store.store_t`, with
  byte-identical candidate manifests from `catalog_runtime.emit_manifest`.
* Uncatalogued windows: `ensure_uncatalogued` publishes a NoDisplay desktop entry before the
  window's first commit, so that `waddle.<device>.<app_key>` matches a desktop file (association
  prerequisite for TODO #28).
* The C session handler that decodes frames and drives the runtime, and the C ABI header.

Outside this contract (explicit, not done):

* No guest-side catalog watch, enumeration, debounce or uninstall enrichment. Those are the guest
  and Windows adapter halves and remain as recorded in `app_catalog_contract.md`.
* No native Windows qualification, no installed desktop-shell behaviour, no compositor
  association. The runtime proves only host-side publication into rooted temporary trees.
* No desktop-shortcut mirror. `desktop_shortcuts=mirror` is rejected with `Unsupported` at
  creation (TODO #28 mirror 10 percent stays open).
* Removal of published records is a seam, not a store API. `set_unpublisher` takes a caller
  function; the runtime asks it to remove at most one deferred record per tick and then checks that
  the record left `manifest.json` (a surviving record backs the seam off). `deferred_removals`
  counts published records of removed applications. The manifest store has no remove API on this
  branch (TODO #33, a required follow-up), and the session handler installs no seam, so in the
  composed build removed applications' records persist and are reported only by the counter.
* No launch path. The host sends only the app key; launch is owned by the launch lane (#29).
* The session control runtime is not composed here. `session_app` does not exist on this branch,
  so the handler is tested against the real codec, the real runtime and a rooted store with a fake
  feature sender, not through `interop_host_control_run`. Composition is the session lane's task.
* Uncatalogued entries use the frozen `open --app <key>` Exec form, not the 3.8.5 image-path form.
  Launching an uncatalogued window is therefore not functional yet (see section 9).

## 2. Blocking cross-lane finding: icon location

`desktop_artifacts` writes icons to `<data>/icons/hicolor/<N>x<N>/apps/waddle.<device>.<key>.png`,
matching IMPL_DESC 3.8.3. `manifest_store` accepts icon artifacts only at `<data>/hicolor/...`
(its `icon_path_allowed`). Every icon commit is therefore refused by the store's path allowlist (code
reading of `icon_path_allowed`; the runtime maps store errors to `InvalidManifest`, so the
exact store error is not surfaced) and retried with backoff. The desktop entries, menus and all catalog state are unaffected, because a failing
unit does not stop others (section 5). The required fix belongs to the manifest lane and is one
line in `manifest_store.zig`: accept the `icons/hicolor/<N>x<N>/apps/` prefix under the Data root,
with the same single-component rules. Until that patch lands, icon files are not published on
disk, and the runtime's tests assert this blocked state explicitly rather than claiming icon files.

## 3. API and ownership

`src/interop/catalog_runtime.zig` exports the C ABI declared in `src/interop/catalog_runtime.h`.
Every exported symbol is documented there (in/out direction, nullability, ownership, returns,
threading). The Zig API is `runtime_t` with `create`, `destroy`, `begin`, `end`, `upsert`,
`remove`, `reset_session`, `transfer_begin`, `transfer_chunk`, `transfer_end`, `transfer_cancel`,
`tick`, `ensure_uncatalogued`, `next_deadline`, `next_action` and `stats`.

* `create` duplicates the three directory descriptors' roots through `manifest_store.store_t.init`,
  takes the exclusive manifest lock, recovers journals, and reads `manifest.json` from the state
  directory through the same descriptor (no-follow, bounded by 4 MiB). A missing file means no
  records. The runtime owns every copied string and the store until `destroy`.
* Every runtime call is single-threaded and must come from the session pump. No internal locks.
* Outbound actions live in a bounded ring of 64. A push into a full ring is dropped and counted in
  `dropped_actions`. New IconRequests are issued only while at least `MaxTransfers` ring slots stay
  free, so cancels for open transfers always fit. `set_send_blocked` stops new requests while the
  sender is refusing.
* The session handler copies an action's key into its own 200-byte buffer (`memmove`) when a send
  returns `QueueFull`, retains that one action, retries it after 100 ms, and blocks new requests. A
  retained IconRequest whose transfer is no longer live is dropped unsent.
* `transfer_state_t` mirrors `interop_transfer_t` layout; the three codec transfer symbols are
  declared `extern` and resolved by linking `interop_codec.o`.

Allocation: all runtime allocations use the allocator passed to `create` (the C ABI uses
`std.heap.c_allocator`). Each allocation has one owner: snapshot strings and icon bytes are owned
by their app entry; transfer keys and buffers are owned by their table entry; candidate manifests
are owned by the publication step until they become the base manifest.

## 4. Snapshot and transactions

Validation (`parse_input`) follows the frozen rules: app key 3..200 bytes with an `a-` or `p-`
prefix and the desktop file-name length limit for the configured device; display name 1..255 bytes
of valid UTF-8 without NUL; flags within `InteropAppFlagMask`; icon sizes an even-length, strictly
ascending little-endian u16 list of frozen sizes (at most 14 bytes); content hash exactly 32 bytes
when present. Target path, arguments, working directory and AUMID are validated by the codec and
not retained: the host sends only the app key, and launch resolves on the guest.

Transactions: `begin` increments an epoch and opens the transaction; applications announced in the
epoch carry its mark. `end` removes applications whose mark differs and closes the transaction.
A second `begin` inside a transaction restarts the epoch without changing the snapshot.

Poisoning is per transaction: inside a transaction, a duplicate upsert of a key already announced in
the same epoch, or any invalid upsert, marks it poisoned. `end` of a poisoned transaction closes it
without removing anything, reports `Rejected`, and leaves `sync_required` set, so publication stays
blocked. A later clean full sync clears `sync_required` and sweeps. `reset_session` also clears it.
Outside a transaction, invalid frames are counted and ignored, and the snapshot is unchanged. Updates
of unknown keys are rejected without poisoning. An `end` without a matching `begin` is counted.

Timeout: a transaction open for `TransactionTimeoutMs` (60 s) from `begin(now)` without its `end` is
aborted without a sweep. The abort counts in `aborted_transactions` and sets `sync_required`.

Quota: 480 applications (`MaxApps`); exceeding it is `Quota` (poisoning inside a transaction). Two
compile-time checks hold: 480 times the maximum key and name sizes (200 + 255 bytes) is below 16 MiB,
and 480 times eight records, plus two menu records and `MaxUncatalogued` (128) entries, is within
`manifest.MaxManifestRecords`.

## 5. Publication

Work units are applications (desktop entry plus advertised icons, rendered together by one
`desktop_artifacts.build` call per application), the menu pair (rendered from all applications
when the menu is dirty), and uncatalogued entries. A pass visits each unit once from a rotating
cursor, publishes the dirty ones, and consumes at most 32 store operations (`MaxOpsPerPass`). A unit
that fails stays dirty and has its own backoff, so a failing unit never blocks the others. A
regression test runs one good application beside a hundred permanently failing units and publishes it
within a bounded number of ticks.

Each artifact is compared with the base manifest by record identity and digest:

* Absent record: `store.commit` with a candidate manifest that adds exactly this record.
* Present record with a different digest: `store.inspect_record` for the exact target identity,
  then `store.replace` with `backup_modified=false`. A modified target is refused and stays
  pending, which keeps user edits.
* Equal digest: no store operation. Repeated syncs of unchanged content therefore never rewrite
  files, and the tests assert identical inode and mtime.

After a failed store operation, the runtime checks whether the store's manifest digest already
equals the candidate (the failure happened after the manifest was published). If so, the candidate
becomes the base. Otherwise the previous base stays authoritative and the failure is returned.

Parent directories are created below the owning store root with `mkdirat` and `O_NOFOLLOW`
descriptor-relative opens. Existing components are accepted; a file or symlink in the way fails
closed with backoff. The store does not create parents itself.

Backoff: a failed unit waits `125 ms << failures`, capped at 8 s, before its next attempt.
`tick` and `ensure_uncatalogued` both honour it, and `ensure_uncatalogued` publishes only its own
entry. A pass that ran out of budget with failures retries after `PassBackoffMs` (250 ms). No path
busy-loops, and the stats field `publish_failures` counts units currently backing off.

Stale generation: `reset_session` drops the snapshot, open transfers and the sync requirement
without changing published files or the manifest; the next full sync decides their fate.

Menu policy: `Installed`, `All`, `None` are passed to `desktop_artifacts` as menu filters.
Menu regeneration calls `desktop_artifacts.build` with every application in one request and
`cumulative_candidates = false`. Each candidate then carries only its own record, and
`publish_artifact` composes the commit candidate from the base manifest. The producer's cumulative
32 MiB candidate cap therefore does not apply to menu output, and a 600-application menu is covered
by a `desktop_artifacts` test. Menu size is bounded only by `MaxApps`.

## 6. Icon retrieval state machine

Per application and frozen size: `want` (requested or re-requested), `requested` (a transfer is
open), `attempts`, `not_before`, and the stored PNG bytes. Invariant: `want` implies
`attempts < 3`, because the third failure clears `want`.

* `want` is set for every advertised size when an application is new, when its content hash
  changes, or when its advertised mask grows. Re-announcing unchanged content does not re-request.
  A content change also cancels the application's open transfers (`TransferCancel(Cancelled)`), so
  data for the old content can never be stored.
* `tick` issues `IconRequest` for wanted, unrequested slots in key order while fewer than 8
  transfers are open, outside transactions and sync-blocked states. Transfer identifiers are
  nonzero, unique among open transfers, and never reuse a live value.
* Begin deadline and idle deadline: 10 s from issue and from each accepted chunk. Expiry sends
  `TransferCancel(TimedOut)` and schedules a retry after `5 s x attempts`.
* `TransferBegin`: MIME must be `image/png` (else cancel Unsupported); total must be 1..1 MiB (else
  cancel TooLarge). The budget check, which the codec's `interop_transfer_begin` does not perform, is
  `icon_bytes - replaced + icon_inflight + total <= 32 MiB`: the slot's replaced icon is not counted,
  and every other receive in flight is. A receive that fails the check is cancelled TooLarge. A second
  begin on a receiving transfer is refused with cancel NotFound.
* `TransferChunk`: contiguous offsets only (codec); a gap, overlap or wrong-state chunk cancels
  with GuestError.
* `TransferEnd`: the codec verifies the SHA-256 of the whole payload. A mismatch cancels with
  GuestError and frees the buffer. The payload must pass `png_valid`: the PNG signature; IHDR first,
  length 13, width and height equal to the requested size, bit depth 8, colour type 6, and no
  compression, filter or interlace variant; every chunk's length inside the buffer and CRC-32
  correct; at most 64 chunks; an IDAT present; IEND last with zero length and no trailing bytes.
  APNG control chunks (`acTL`, `fcTL`, `fdAT`) and unknown critical chunks are refused. A payload that
  fails validation is freed without a cancel (the guest has finished), counts as a failure and in
  `rejected_frames`, and returns `Rejected`.
* Guest `TransferCancel` and host-side cancellations (removal, content change, timeout, reset) free
  the entry. Removal, content change and reset emit `TransferCancel(Cancelled)` only when the entry
  was open. Removing an application also purges its queued IconRequests, so a request for a
  released transfer is never sent.
* Unknown transfer identifiers in Begin, Chunk, End receive `TransferCancel(NotFound)`. Cancel
  for an unknown identifier is a no-op.

## 7. Wire behaviour of the session handler

`src/interop/session_catalog_handler.c` implements `session_handler_t` with capability
`InteropCapCatalog`, owning `CatalogBegin..CatalogEnd` and `TransferBegin..TransferCancel`.
Frames are complete protocol frames. The header length must equal the payload length, and the
payload must pass `interop_payload_validate` for `InteropFromGuest`; otherwise the frame is
counted and ignored. Required fields are read after validation, so their absence cannot occur;
optional fields are read with their status checked. A `ContentHash` must be 32 bytes, which the
codec enforces.

Outbound frames are built with the codec writer (IconRequest: app key, transfer, icon size;
TransferCancel: transfer, status) and sent through the injected sender. `QueueFull` retains the
single action and retries it 100 ms later; later actions stay in the runtime's ring, so ordering is
preserved, and new requests are blocked until the retained action is sent. Other send results drop
the action. `next_deadline` returns the retry instant while an action is retained. `on_closing` resets the runtime's session. The handler never fails the run:
every callback returns zero, except a wall-clock failure in `on_tick` (returns one).

Clocks: `on_frame` reads the injected monotonic clock (default `CLOCK_MONOTONIC`). `on_tick`
receives the pump's `now_ms` and formats `updated` from the injected wall clock (default
`time(NULL)`) as `YYYY-MM-DDTHH:MM:SSZ`. The send function defaults to `interop_host_feature_send`
through a weak reference, so fixtures that do not link the control runtime get
`HostFeatureUnsupportedFrame` and drop actions.

## 8. Error codes

C ABI statuses (`catalog_runtime_status_t`): `Ok`, `InvalidArgument`, `Rejected`, `Quota`,
`OutOfMemory`, `Pending`, `StoreFailed`, `Unsupported`. Interop cancel statuses (`interop_status_t`):
`NotFound`, `Cancelled`, `TimedOut`, `TooLarge`, `Unsupported`, `GuestError`. Store and build
failures inside publication are reported as `StoreFailed` or retried; they never change the
previous published state.

## 9. Failure, recovery and known limits

* A failed store operation leaves the previous record, file and manifest intact. Interrupted
  commits are recovered by the store on the next commit or on `create`. The runtime reconciles
  its base manifest through the digest check described in section 5.
* Injected failures at every durable checkpoint of an add and of a replacement recover to a
  consistent state (test `store failures at every durable checkpoint...` and
  `every durable checkpoint of a replacement...`).
* Allocation failure at every allocation index of create, transactions, publication and icon
  transfer completes without leaks (`catalog state under allocation failure never leaks`, and the
  exhaustive `emit_manifest` sweep).
* Open transfers at teardown are released by `destroy`.
* Known limits, recorded for the lead: icon files blocked by the store allowlist (section 2);
  deferred removals, with no store remove API and no seam installed by the session handler
  (section 1); the uncatalogued Exec form (section 1). The pre-existing store behaviour that refuses a stale base (`ManifestMismatch`)
  makes the runtime fail closed with backoff if the state directory is edited by another writer.

## 10. Verification

Zig (`zig test src/interop/catalog_runtime.zig -Iinclude -lc`, 170 tests passing, including the
imported module tests): regressions for the review findings (action-ring bounds and drop policy,
send backpressure, queued-key lifetime and purge, per-transaction poisoning and clean clear, the
transaction timeout, per-unit backoff and starvation, content-change cancel, the in-flight budget
and its M3 mutant, PNG chunk validation, the removal seam and deferred counter), unit and helper branches, the 100-application sync, idempotent repeated syncs
(inode and mtime unchanged), changed content replacement, removed applications, transaction
semantics, poisoning and recovery, hostile frames and quotas, the icon request and transfer state
machine (including each cancel and timeout path and the retained budget), store fault injection at
every durable checkpoint, allocation-failure sweeps, a 40-round randomized sequence against a
reference model, the restart path, and the C ABI (every export, null guards and every status).
All Zig tests use `std.testing.allocator`, which reports leaks at test end.

Coverage: `python3 tests/av/coverage.py catalog_runtime` gates production lines and taken arcs
(marker `tests/av/coverage_modes/catalog_runtime`). The latest run FAILED the gate: 97.76% lines
(871/891) and 87.17% taken arcs (530/608), below the 90% threshold. The uncovered arcs are in the C ABI
null guards of `catalog_runtime_create` and the new exports; they are not yet covered by tests.

C: `tests/interop/session_catalog_handler.c` runs the real codec, the real runtime and a rooted
store through a fake sender. It asserts the exact IconRequest and TransferCancel bytes and the
header/payload layout, the QueueFull retention and retry, hostile frames (header mismatch,
validation failure, unknown transfer identifiers, a mismatched content hash), transfer refusals
and timeouts, clock defaults, the wall-clock failure, and closing. The gcov gate
(`tests/interop/session_catalog_handler_coverage.py`) requires at least 90% of production lines and
taken arcs of `session_catalog_handler.c`, and then runs the same fixture under
`-fsanitize=address,leak,undefined` with `detect_leaks=1`.

Not run here: Windows, native, installed-desktop, compositor, and the GitHub `windows-2022` jobs.
