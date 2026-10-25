# Linux host mapping contract

## Title and high-level scope

This module owns one Linux host-side mapping of the frozen layout-v3, exactly
2 GiB backing object. It accepts an already-open borrowed descriptor and an
explicit backing-kind value. It does not select or parse a pathname, create,
truncate, initialize, unlink, or resize a backing object, negotiate Hello,
prove peer shutdown, drain compositor buffers, or authorize reuse of a backing
object. It only unmaps the local host view and releases its duplicated
descriptor. The caller remains responsible for host drain and object lifetime.

The caller must keep object size and identity stable while this owner exists.
A file descriptor prevents pathname replacement from changing the opened
object, but it cannot prevent another process from truncating that object while
mapped. This API makes no active-lease or cross-process exclusivity claim.

## Architecture and component interactions

The daemon/session layer chooses a backing object and opens it under its own
policy. It passes the raw descriptor plus an explicit `backing_kind` to this
module; this layer does not see or parse the path. The existing Hello exchange
selects a nonzero epoch before the caller requests header validation. The
mapping owner delegates immutable layout facts to the existing Zig
`interop_layout_validate_shm_header` ABI and native atomic address/lock-free
checks to `interop_atomic_validate_mapping`. A successful validated view is
borrowed by the caller for local host use. When local users have stopped, the
caller asserts that fact to close, which unmaps only this process's view and
releases only the duplicated descriptor. Remote session drain and compositor
buffer release remain owned by other components.

```text
caller-selected raw fd + backing kind
                 |
                 v
duplicate CLOEXEC -> validate object facts -> mmap shared RW
                 |                              |
             owner record                       v
Hello-selected nonzero epoch -> private snapshot -> Zig layout check
                                                  -> native atomic check
                                                  -> borrowed view
local users stopped + explicit assertion -> munmap -> close duplicate -> destroy
```

## Data structures, protocols, and memory layout

The opaque heap owner records exactly: creating `pthread_t`, duplicated fd
(or `-1`), mapping base (or `NULL`), exact mapping length (or zero), caller
selected backing-kind enum, and booleans for mapped, header-validated, and
view-published states. The caller's raw fd remains borrowed and is neither
closed nor modified. `interop_mapping_linux_view_t` is a caller-owned value
containing only `uintptr_t mapping_base`, `uint64_t mapping_length`, and two
zero `uint32_t` reserved fields. `interop_shm_header_snapshot_t` is the
existing 64-byte validator output and contains no retained mapping pointer.

The frozen mapping is little-endian layout v3 and exactly `0x80000000` bytes.
The header at offset zero is 4096 bytes, aligned to 64. The two live atomic
heartbeat objects are at offsets 56 and 64. Header snapshot assembly copies
ordinary mapping bytes `[0,56)` and `[72,4096)` only. It acquire-loads each
heartbeat individually and copies each scalar into the matching private
snapshot offset. It never copies the live atomic objects as bytes. No protocol
fields, wire format, descriptor metadata, or mapping bytes are modified by
this module.

## Step-by-step execution

1. `create` validates the output pointer, allocates a zeroed owner before any
   mapping exists, records the current pthread, and publishes the owner.
2. `open` checks same-thread and empty-owner state, duplicates the caller fd
   using `F_DUPFD_CLOEXEC`, checks read/write access and `fstat` facts for the
   explicitly selected kind, obtains KVMFR capacity through the existing
   ioctl only for KVMFR, then requests a shared read/write mapping of exactly
   2 GiB at offset zero. It never initializes or touches mapping contents.
3. After Hello has selected its epoch, `validate_header` stages the header in
   private stack storage, loads heartbeat values individually, calls the Zig
   layout validator with actual mapped address/length and expected epoch, then
   calls the native atomic helper for the two heartbeat objects and first
   descriptor state. Only after both succeed does it publish the copied
   snapshot to the caller and mark the owner validated.
4. `view` copies base/length into disjoint caller output and sets
   `view_published`. The caller may then give local readers access under its
   own synchronization.
5. After the caller stops and joins those local readers, `close` validates
   the `local_users_quiesced` value. A valid close expires the published view
   before calling `munmap`. On successful unmap it clears mapping ownership
   and attempts descriptor close. On failed unmap it retains both records and
   does not close the backing descriptor. A later close retries the retained
   operation.
6. `destroy` frees and NULLs the owner only when no map or descriptor remains.
   It never performs implicit cleanup or drops a failure record.

## Concurrency, threading, and synchronization

The owner is bound to the pthread that calls `create`. Every public lifecycle,
validation, view, close, and destroy operation checks `pthread_equal` before
touching mutable resources. `WrongThread` leaves state unchanged. The API is
not internally synchronized: its caller must serialize all calls and ensure
that no reader uses a published view after a valid close attempt begins. A
published-view close requires the caller's truthful assertion that all local
users have stopped and joined. This assertion is not produced by a thread
join, inferred from owner state, or treated as remote proof.

Header ordinary fields are immutable only under the caller's completed-Hello
precondition. Heartbeat values use individual C11 atomic loads with
`memory_order_acquire`; these loads do not claim publication of ordinary
header fields. Native `atomic_is_lock_free` checks cover both live header
heartbeats and the first descriptor state object. No mutex, worker thread,
atomic owner state, or cross-process synchronization primitive is created.

## API and ownership

`interop_mapping_linux_create` allocates a zeroed opaque owner before any
mapping operation and records `pthread_self()` as its sole owner thread. The
record is noncopyable. All lifecycle, validation, view, close, and destroy calls must
run on that same thread. Calls on another thread return `WrongThread` without
changing state or touching operating-system resources. The caller serializes
all calls and stops local readers before close.

`interop_mapping_linux_open(owner, borrowed_fd, backing_kind)` duplicates
the borrowed descriptor with close-on-exec before inspection. It never closes
or changes flags on the source descriptor. For `RegularPrivateFile`, the
duplicate must refer to a regular file with exact size 2 GiB, `st_uid ==
geteuid()`, permission bits exactly 0600, and link count one. For explicit
`KvmfrCharacter`, the duplicate must be a character device and its capacity
must be exactly 2 GiB according to the existing KVMFR capacity ioctl. Both
kinds require a read/write descriptor. The
backing kind is a caller-selected fact; the module does not infer it from a
path. Both kinds map exactly 2 GiB with shared read/write permissions. Open
does not inspect or alter mapped contents. A partial failure unwinds every
acquired resource. If descriptor close reports an error during this unwind,
the Linux descriptor number is relinquished before the one close attempt, the
owner retains no descriptor to retry, and the function returns
`CloseDiagnostic` with `errno` restored to the close failure.

`interop_mapping_linux_validate_header(owner, hello_epoch, out)` is called only
after the handshake has selected a nonzero Hello epoch and the peer's ordinary
header fields are initialized and immutable for the session. It checks that
the actual heartbeat and descriptor-state atomic objects are lock-free before
accessing either heartbeat. It creates a private 4096-byte snapshot without
copying live atomic objects: copy bytes `[0,56)` and `[72,4096)` from the
mapping, then acquire-load `guest_heartbeat` and `host_heartbeat` individually
and encode those values in the private snapshot at offsets 56 and 64. It then
calls the existing `interop_layout_validate_shm_header` Zig ABI with the actual mapping address,
actual length, selected epoch, and private snapshot. A successful validation
publishes a caller-owned immutable header snapshot; failures preserve the
output. An unsupported live atomic returns a distinct `NotLockFree` status
rather than malformed-header status. It does not publish a raw view.

`interop_mapping_linux_view(owner, out)` publishes the borrowed base and length
only after header validation succeeds. The caller must not retain it beyond
the first valid close attempt or let local readers continue after close begins.
It is a local mapping view, not evidence of guest/host drain, compositor
release, detach, or safe epoch reset.

`interop_mapping_linux_close(owner, local_users_quiesced)` accepts only the
literal values zero and one. If a view was published, one is required and is a
truthful caller assertion that all local users have stopped and joined. The
assertion establishes no remote quiescence and is never synthesized by the
module. A valid close attempt expires the published view and sets the owner's
sticky `closing` state immediately. Until cleanup fully succeeds, open,
validation, and view calls are blocked. A failed `munmap` keeps both mapping
and descriptor ownership; the descriptor is not closed while the mapping
remains. Successful unmap clears mapping ownership before descriptor close.
Before its single Linux `close` call, the owner sets its duplicate descriptor
to `-1`. Any close error returns `CloseDiagnostic` after all local resources
are relinquished and restores `errno`; it never retries that descriptor
number, because Linux may have released it despite reporting an error.
`destroy` frees and nulls the caller's owner pointer only after all resources
are gone. It never silently discards an unreleased mapping.

## State and failure behavior

The owner contains the owner-thread identity, duplicated descriptor, mapping
base/length, backing kind, whether a view was published, whether validation
succeeded, and whether close is in progress. The descriptor record is cleared
before `close`; a failed `munmap` retains both descriptor and mapping. No status or diagnostics
include the borrowed descriptor's path or mapped content.

The API distinguishes invalid arguments, wrong thread, busy state, allocation
failure, backing validation failure, mapping failure, invalid header, missing
local-quiescence assertion, pending cleanup, close diagnostic, and unsupported non-lock-free
atomics. `open` is permitted only on
an empty owner. `validate_header` requires a mapped owner and no published
view. `view` requires successful validation. `close` on an empty owner is
idempotent. `destroy` on a nonempty owner returns `Busy` and preserves the
pointer.

The implementation uses checked arithmetic and fixed-size private metadata;
it allocates the owner before `mmap`, and does not allocate after mapping.
Every public declaration documents parameters, output preservation, ownership,
thread constraints, and return statuses. Type names use `_t`, ordinary
identifiers use `snake_case`, and constants use `PascalCase`.

## Verification

Tests use temporary sparse regular files with exact size and metadata
initialized before validation. They do not launch guests or touch production
backing objects. Fault injection covers duplicate/fcntl, access-mode query,
`fstat`, ioctl, `mmap`, `munmap`, close, allocation, layout validation, native
atomic validation, and wrong-thread paths. The close-error test actually
releases the duplicate and reuses its descriptor number for an unrelated file;
it verifies cleanup never closes that reused descriptor. Assertions check
source descriptor preservation, retryable unmap ownership, sticky close state,
unchanged metadata, no publication on failure, output preservation, and
close/destroy symmetry. Coverage retains each unique run, exact input hashes,
toolchain, command logs, gcov reports, summary, and failure details under
`build/coverage/interop/mapping_linux/run-*`. It requires at least 90% line and
taken-branch coverage. The final run uses `-fsanitize=address,leak,undefined`,
`ASAN_OPTIONS=detect_leaks=1`, and `UBSAN_OPTIONS=halt_on_error=1`; sanitizer
runtime failure fails the run and remains in its evidence directory.
