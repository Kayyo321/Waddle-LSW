//! POSIX manifest-store primitives for the frozen seamless integration schema.
//! The pure codec remains in manifest.zig; this module owns lock and filesystem
//! state. Artifact callers are authorized by record kind and configured roots.
const std = @import("std");
const builtin = @import("builtin");
const manifest = @import("manifest.zig");
const c = @cImport({
    @cInclude("errno.h");
    @cInclude("fcntl.h");
    @cInclude("stdio.h");
    @cInclude("sys/file.h");
    @cInclude("sys/stat.h");
    @cInclude("sys/syscall.h");
    @cInclude("linux/fs.h");
    @cInclude("unistd.h");
});

/// Private POSIX dispatch table. Production defaults always call libc/Linux;
/// tests can fail one selected operation while preserving real filesystem
/// effects for every other call.
const io_ops_t = struct {
    open_fn: *const fn ([*c]const u8, c_int) callconv(.C) c_int,
    openat3_fn: *const fn (c_int, [*c]const u8, c_int) callconv(.C) c_int,
    openat4_fn: *const fn (c_int, [*c]const u8, c_int, c_uint) callconv(.C) c_int,
    fstat_fn: *const fn (c_int, [*c]c.struct_stat) callconv(.C) c_int,
    fstatat_fn: *const fn (c_int, noalias [*c]const u8, noalias [*c]c.struct_stat, c_int) callconv(.C) c_int,
    fsync_fn: *const fn (c_int) callconv(.C) c_int,
    flock_fn: *const fn (c_int, c_int) callconv(.C) c_int,
    mkdirat_fn: *const fn (c_int, [*c]const u8, c_uint) callconv(.C) c_int,
    fchmod_fn: *const fn (c_int, c_uint) callconv(.C) c_int,
    unlinkat_fn: *const fn (c_int, [*c]const u8, c_int) callconv(.C) c_int,
    renameat_fn: *const fn (c_int, [*c]const u8, c_int, [*c]const u8) callconv(.C) c_int,
    read_fn: *const fn (c_int, ?*anyopaque, usize) callconv(.C) isize,
    write_fn: *const fn (c_int, ?*const anyopaque, usize) callconv(.C) isize,
};
const io_ops = io_ops_t{
    .open_fn = real_open,
    .openat3_fn = real_openat3,
    .openat4_fn = real_openat4,
    .fstat_fn = c.fstat,
    .fstatat_fn = c.fstatat,
    .fsync_fn = c.fsync,
    .flock_fn = c.flock,
    .mkdirat_fn = c.mkdirat,
    .fchmod_fn = c.fchmod,
    .unlinkat_fn = c.unlinkat,
    .renameat_fn = c.renameat,
    .read_fn = c.read,
    .write_fn = c.write,
};
var injected_io_at: usize = 0;
var injected_io_count: usize = 0;
var injected_eintr_at: usize = 0;
var injected_short_at: usize = 0;
var injected_zero_at: usize = 0;
var injected_short_limit: usize = 1;
var injected_replace_fstatat_at: usize = 0;
var injected_replace_fstatat_count: usize = 0;
var injected_replace_fstatat_fd: c_int = -1;
var injected_replace_fstatat_source: [*:0]const u8 = "";
var injected_replace_fstatat_target: [*:0]const u8 = "";
var injected_renameat2_errno: c_int = 0;
var injected_renameat2_at: usize = 0;
var injected_renameat2_errno_at: usize = 0;
var injected_renameat2_count: usize = 0;

const io_fault_t = enum { None, IoError, Interrupted, ShortTransfer, ZeroTransfer };

fn real_open(path: [*c]const u8, flags: c_int) callconv(.C) c_int {
    return c.open(@ptrCast(path), flags);
}
fn real_openat3(dir_fd: c_int, path: [*c]const u8, flags: c_int) callconv(.C) c_int {
    return c.openat(dir_fd, @ptrCast(path), flags);
}
fn real_openat4(dir_fd: c_int, path: [*c]const u8, flags: c_int, mode: c_uint) callconv(.C) c_int {
    return c.openat(dir_fd, @ptrCast(path), flags, mode);
}
fn take_io_fault() io_fault_t {
    if (!builtin.is_test) return .None;
    if (injected_io_at == 0 and injected_eintr_at == 0 and injected_short_at == 0 and injected_zero_at == 0) return .None;
    injected_io_count += 1;
    if (injected_io_at != 0 and injected_io_count == injected_io_at) {
        injected_io_at = 0;
        c.__errno_location().* = c.EIO;
        return .IoError;
    }
    if (injected_eintr_at != 0 and injected_io_count == injected_eintr_at) {
        injected_eintr_at = 0;
        c.__errno_location().* = c.EINTR;
        return .Interrupted;
    }
    if (injected_short_at != 0 and injected_io_count == injected_short_at) {
        injected_short_at = 0;
        return .ShortTransfer;
    }
    if (injected_zero_at != 0 and injected_io_count == injected_zero_at) {
        injected_zero_at = 0;
        return .ZeroTransfer;
    }
    return .None;
}
fn io_open(path: [*c]const u8, flags: c_int) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.open_fn(path, flags);
}
fn io_openat3(dir_fd: c_int, path: [*c]const u8, flags: c_int) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.openat3_fn(dir_fd, path, flags);
}
fn io_openat4(dir_fd: c_int, path: [*c]const u8, flags: c_int, mode: c_uint) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.openat4_fn(dir_fd, path, flags, mode);
}
fn io_fstat(fd: c_int, stat: *c.struct_stat) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.fstat_fn(fd, stat);
}
fn io_fstatat(fd: c_int, path: [*c]const u8, stat: *c.struct_stat, flags: c_int) c_int {
    if (take_io_fault() == .IoError) return -1;
    if (builtin.is_test and injected_replace_fstatat_at != 0) {
        injected_replace_fstatat_count += 1;
        if (injected_replace_fstatat_count == injected_replace_fstatat_at) {
            injected_replace_fstatat_at = 0;
            if (c.renameat(injected_replace_fstatat_fd, injected_replace_fstatat_source, injected_replace_fstatat_fd, injected_replace_fstatat_target) != 0) return -1;
        }
    }
    return io_ops.fstatat_fn(fd, path, stat, flags);
}
fn io_fsync(fd: c_int) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.fsync_fn(fd);
}
fn io_flock(fd: c_int, operation: c_int) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.flock_fn(fd, operation);
}
fn io_mkdirat(fd: c_int, path: [*c]const u8, mode: c_uint) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.mkdirat_fn(fd, path, mode);
}
fn io_fchmod(fd: c_int, mode: c_uint) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.fchmod_fn(fd, mode);
}
fn io_unlinkat(fd: c_int, path: [*c]const u8, flags: c_int) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.unlinkat_fn(fd, path, flags);
}
fn io_renameat(old_fd: c_int, old_path: [*c]const u8, new_fd: c_int, new_path: [*c]const u8) c_int {
    if (take_io_fault() == .IoError) return -1;
    return io_ops.renameat_fn(old_fd, old_path, new_fd, new_path);
}
fn io_renameat2(old_fd: c_int, old_path: [*c]const u8, new_fd: c_int, new_path: [*c]const u8, flags: c_uint) c_int {
    if (take_io_fault() == .IoError) return -1;
    if (builtin.is_test and (injected_renameat2_at != 0 or injected_renameat2_errno_at != 0)) {
        injected_renameat2_count += 1;
        if (injected_renameat2_count == injected_renameat2_at) {
            c.__errno_location().* = c.EIO;
            injected_renameat2_at = 0;
            return -1;
        }
        if (injected_renameat2_errno_at != 0 and injected_renameat2_count == injected_renameat2_errno_at) {
            c.__errno_location().* = injected_renameat2_errno;
            injected_renameat2_errno_at = 0;
            injected_renameat2_errno = 0;
            return -1;
        }
    }
    if (builtin.is_test and injected_renameat2_errno != 0 and injected_renameat2_errno_at == 0) {
        c.__errno_location().* = injected_renameat2_errno;
        injected_renameat2_errno = 0;
        return -1;
    }
    return @intCast(c.syscall(c.SYS_renameat2, old_fd, @as([*:0]const u8, @ptrCast(old_path)), new_fd, @as([*:0]const u8, @ptrCast(new_path)), flags));
}
fn io_read(fd: c_int, buffer: [*]u8, count: usize) isize {
    const fault = take_io_fault();
    if (fault == .IoError or fault == .Interrupted) return -1;
    if (fault == .ZeroTransfer) return 0;
    return io_ops.read_fn(fd, @ptrCast(buffer), if (fault == .ShortTransfer) @min(count, injected_short_limit) else count);
}
fn io_write(fd: c_int, buffer: [*]const u8, count: usize) isize {
    const fault = take_io_fault();
    if (fault == .IoError or fault == .Interrupted) return -1;
    if (fault == .ZeroTransfer) return 0;
    return io_ops.write_fn(fd, @ptrCast(buffer), if (fault == .ShortTransfer) @min(count, injected_short_limit) else count);
}

/// Maximum configured roots.
pub const MaxRoots: usize = 3;
/// Maximum transactions kept for crash recovery.
pub const MaxJournals: usize = 32;
/// Maximum private journal size.
pub const MaxJournalBytes: usize = 4 * 1024 * 1024;
/// Maximum one artifact payload.
pub const MaxArtifactBytes: usize = 32 * 1024 * 1024;
/// Maximum retained backup transactions inspected by one inventory call.
pub const MaxBackups: usize = 256;
/// Exact byte length of the private version-1 backup metadata record.
pub const BackupMetadataBytes: usize = 124;

/// Supported host artifact root class.
pub const root_kind_t = enum {
    /// Freedesktop data root.
    Data,
    /// Freedesktop configuration root.
    Config,
    /// Explicit user desktop root.
    Desktop,
};

/// Trusted canonical root supplied by a caller after no-follow validation.
pub const root_t = struct {
    /// Fixed host root role.
    kind: root_kind_t,
    /// Borrowed canonical absolute spelling with no trailing slash; init
    /// copies it and requires it to resolve without symlinks to `dir_fd`.
    canonical_path: []const u8,
    /// Borrowed open directory descriptor; init retains a close-on-exec copy.
    dir_fd: c_int,
};

/// Prior target state required to authorize a replacement.
pub const prior_state_t = union(enum) {
    /// Target must be absent.
    Absent,
    /// Target must have the given SHA-256 digest.
    Sha256: [32]u8,
};

/// One schema-1 host artifact replacement request.
pub const request_t = struct {
    /// Unique manifest record id.
    record_id: []const u8,
    /// Expected current artifact state.
    prior: prior_state_t,
    /// Borrowed new artifact bytes.
    artifact_bytes: []const u8,
    /// New mode, restricted to 0644 or 0755.
    artifact_mode: u9 = 0o644,
    /// Borrowed full candidate schema-1 manifest.
    candidate_manifest: []const u8,
    /// Reserved force authorization; true currently returns UnsupportedReplacement.
    backup_modified: bool = false,
};

const journal_t = struct {
    version: u32 = 1,
    transaction_id: []const u8,
    phase: []const u8,
    record_id: []const u8,
    root: []const u8,
    relative_path: []const u8,
    old_artifact_hash: ?[]const u8,
    old_artifact_mode: u32,
    new_artifact_hash: []const u8,
    new_artifact_mode: u32,
    new_artifact_device: u64,
    new_artifact_inode: u64,
    old_manifest_hash: ?[]const u8,
    old_manifest_device: ?u64 = null,
    old_manifest_inode: ?u64 = null,
    new_manifest_hash: []const u8,
    new_manifest_device: u64,
    new_manifest_inode: u64,
    old_artifact_device: ?u64 = null,
    old_artifact_inode: ?u64 = null,
    backup_hash: ?[]const u8 = null,
    backup_mode: u32 = 0,
    backup_device: ?u64 = null,
    backup_inode: ?u64 = null,
};
const owned_root_t = struct { kind: root_kind_t, canonical_path: []u8, fd: c_int };
const parent_t = struct { fd: c_int, leaf: []const u8 };
const file_info_t = struct { hash: [32]u8, mode: u32, device: u64, inode: u64 };
/// Private artifact classification shared by `status` and `remove`. `Ok`
/// carries the exact identity; the other states never authorize mutation.
const artifact_state_t = union(enum) { Ok: file_info_t, Missing, Modified, Foreign };

/// Exact identity of one regular artifact observed through the store's rooted
/// descriptor walk. Callers may retain the value and supply it as a
/// compare-before-replace precondition; it conveys no path or deletion authority.
pub const artifact_identity_t = struct {
    /// Device number from `fstat` on the opened artifact.
    device: u64,
    /// Inode number from `fstat` on the opened artifact.
    inode: u64,
    /// Permission bits from `fstat`; accepted artifact modes are 0644/0755.
    mode: u32,
    /// SHA-256 of the exact bounded file contents.
    sha256: [32]u8,
};

/// Coherent by-value snapshot used to construct a later replacement request.
/// The snapshot is not a lock on the artifact after this method returns.
pub const record_snapshot_t = struct {
    /// SHA-256 of the exact manifest bytes observed under the store lock.
    manifest_sha256: [32]u8,
    /// Exact target identity observed through the allowlisted root descriptor.
    target: artifact_identity_t,
};

/// Stable locator and exact metadata for one retained private backup.
/// It is a reference, not a filesystem path or authorization token.
pub const backup_ref_t = struct {
    /// Random lowercase-hex transaction identifier that owns the backup.
    transaction_id: [32]u8,
    /// SHA-256 of the validated schema record identifier.
    record_id_sha256: [32]u8,
    /// SHA-256 of the exact backup bytes.
    sha256: [32]u8,
    /// Original artifact permission bits preserved in the private backup.
    artifact_mode: u32,
    /// Backup file device number verified by `fstat`.
    device: u64,
    /// Backup file inode number verified by `fstat`.
    inode: u64,
};

/// Caller-owned result for one bounded retained-backup inventory operation.
/// `items` is allocated from `allocator` and must be released with `deinit`.
pub const backup_list_t = struct {
    /// Allocator that owns `items`.
    allocator: std.mem.Allocator,
    /// Matching validated backup references, in filesystem iteration order.
    items: []backup_ref_t,

    /// Release the owned result and clear this value. Safe to call repeatedly.
    pub fn deinit(self: *backup_list_t) void {
        self.allocator.free(self.items);
        self.items = &.{};
    }
};

/// Replacement request for one existing schema-1 artifact record.
pub const replace_request_t = struct {
    /// Existing record identifier; borrowed for this call.
    record_id: []const u8,
    /// Exact old manifest digest expected under the held store lock.
    expected_manifest_sha256: [32]u8,
    /// Exact existing artifact identity expected under the held store lock.
    expected_target: artifact_identity_t,
    /// Borrowed replacement artifact bytes, bounded by `MaxArtifactBytes`.
    artifact_bytes: []const u8,
    /// Replacement file mode, restricted to 0644 or 0755.
    artifact_mode: u9 = 0o644,
    /// Borrowed complete schema-1 candidate manifest.
    candidate_manifest: []const u8,
    /// Permit replacement of a file whose bytes differ from the old record hash.
    /// The exact current bytes and mode are retained in a private backup first.
    backup_modified: bool = false,
};

/// Explicit restore request naming a retained backup and expected current state.
pub const restore_request_t = struct {
    /// Existing record identifier; borrowed for this call.
    record_id: []const u8,
    /// Exact old manifest digest expected under the held store lock.
    expected_manifest_sha256: [32]u8,
    /// Exact current artifact identity expected under the held store lock.
    expected_target: artifact_identity_t,
    /// Validated retained-backup reference returned by replacement.
    backup: backup_ref_t,
    /// Borrowed candidate manifest whose record hash identifies backup bytes.
    candidate_manifest: []const u8,
};

/// Result of a replacement, with an optional retained backup reference.
pub const replace_receipt_t = struct {
    /// Exact identity of the newly published artifact.
    target: artifact_identity_t,
    /// Backup reference when a modified artifact was force-replaced.
    backup: ?backup_ref_t,
};

/// Result of an explicit backup restore transaction.
pub const restore_receipt_t = struct {
    /// Exact identity of the restored artifact.
    target: artifact_identity_t,
};

/// True for record kinds whose artifact is a file the store can resolve under a root.
/// Other kinds are deleted as records only.
fn store_managed_kind(kind: manifest.record_kind_t) bool {
    return switch (kind) {
        .DesktopEntry, .HandlerEntry, .Icon, .Menu, .DesktopShortcut, .Autostart => true,
        else => false,
    };
}

/// Read-only classification of one recorded artifact returned by `status`.
pub const record_state_t = enum {
    /// Owned regular file with mode 0644 or 0755 whose SHA-256 equals the record.
    Ok,
    /// No file exists at the recorded path, including a missing parent directory.
    Missing,
    /// Owned regular file whose SHA-256 differs from the record, or whose size
    /// exceeds `MaxArtifactBytes` and therefore cannot match any valid artifact.
    Modified,
    /// The record is outside the authorized roots, or the object at the path is
    /// not an owned single-link regular file with mode 0644/0755 and no
    /// setuid/setgid/sticky bits (symlink, hardlink, directory, foreign owner).
    Foreign,
};

/// One classified manifest record. `record_id` is owned by the containing list.
pub const record_status_t = struct {
    /// Owned copy of the schema record identifier.
    record_id: []u8,
    /// Classification of the recorded artifact at the time of the call.
    state: record_state_t,
};

/// Caller-owned status snapshot. The list and every `record_id` are allocated
/// from `allocator` and released by `deinit`; the snapshot is not a lock.
pub const status_list_t = struct {
    /// Allocator that owns `items` and every `record_id`.
    allocator: std.mem.Allocator,
    /// Classified records in manifest order; empty when no manifest exists.
    items: []record_status_t,

    /// Release all owned identifiers and the item array, then clear `items`.
    /// Safe to call repeatedly.
    pub fn deinit(self: *status_list_t) void {
        for (self.items) |item| self.allocator.free(item.record_id);
        self.allocator.free(self.items);
        self.items = &.{};
    }
};

/// Owned copy of a record's prior value. `Text` stores at most
/// `manifest.MaxManifestStringBytes` bytes inline, so receipts never borrow the
/// parsed manifest document.
pub const prior_copy_t = union(enum) {
    /// The record has no restorable prior value.
    None,
    /// Textual prior value copied into the receipt.
    Text: bounded_text_t,
    /// DWORD prior value of a guest policy record.
    U32Value: u32,
};

/// Inline text buffer for `prior_copy_t`. Only `bytes[0..len]` is meaningful.
pub const bounded_text_t = struct {
    /// Storage for the copied text, bounded by the manifest codec.
    bytes: [manifest.MaxManifestStringBytes]u8,
    /// Number of meaningful bytes in `bytes`.
    len: usize,
};

/// Explicit removal request for one recorded artifact.
pub const remove_request_t = struct {
    /// Schema record identifier; borrowed for this call.
    record_id: []const u8,
    /// Borrowed complete schema-1 manifest with exactly this record deleted.
    /// It is parsed on every call and its record delta is verified only when a
    /// removal would proceed.
    candidate_manifest: []const u8,
};

/// Final state of a recorded artifact name after a removal transaction.
pub const artifact_disposition_t = union(enum) {
    /// The recorded identity was moved aside, verified, and unlinked.
    Unlinked,
    /// The name was already absent; nothing was removed.
    AlreadyAbsent,
    /// The name held a file that is not the recorded identity. It was returned to
    /// its name and left in place.
    KeptUnchanged,
    /// A file that failed verification could not be returned to its name. It stays
    /// at this private staging name (`.waddle-<txid>.tmp` in the artifact's directory).
    KeptAtTempName: [44]u8,
};

/// Decision made by `remove` under the held store lock. Record deletions are
/// journaled; the caller restores any prior value before it asks for a deletion.
pub const remove_outcome_t = enum {
    /// The owned artifact matched the record; it was unlinked and the record deleted.
    Removed,
    /// The artifact exists but its digest differs from the record. Both are kept.
    KeptModified,
    /// The record is not in the manifest (or no manifest exists). Nothing changed;
    /// repeated removal converges to this outcome with `record_found = false`.
    AlreadyMissing,
    /// The path is unauthorized or unsafe (symlink, hardlink, foreign owner,
    /// unexpected mode, path outside the roots). Nothing changed.
    KeptForeign,
    /// The artifact file is absent. Only the record was deleted, in one journal.
    RecordRemovedArtifactMissing,
    /// The record kind has no store-managed file (for example `mime_default`,
    /// `binfmt`, or guest kinds). Only the record was deleted, in one journal.
    RecordRemovedNonFile,
};

/// Explicit metadata migration request for one recorded artifact.
pub const migrate_request_t = struct {
    /// Existing record identifier; borrowed for this call.
    record_id: []const u8,
    /// Borrowed complete schema-1 candidate manifest. Only root-level
    /// `waddle_version` and `updated` may differ from the current manifest.
    candidate_manifest: []const u8,
};

/// Explicit recreation request for one recorded artifact whose file is absent.
pub const recreate_request_t = struct {
    /// Existing record identifier; borrowed for this call.
    record_id: []const u8,
    /// Borrowed complete candidate manifest. Only the record digest and creator may
    /// differ from the current record; the target identity must be unchanged.
    candidate_manifest: []const u8,
    /// Borrowed artifact bytes to publish; the candidate record digest must match them.
    artifact_bytes: []const u8,
    /// Mode of the published artifact, 0644 or 0755.
    artifact_mode: u9 = 0o644,
};

/// Result of one recreation: the published identity.
pub const recreate_receipt_t = struct {
    /// Exact identity of the newly published artifact.
    target: artifact_identity_t,
};

/// Result of one migration request.
pub const migrate_receipt_t = struct {
    /// True when a new manifest was published; false for an identical candidate.
    changed: bool,
};

/// Result of one removal request. Nothing in the receipt borrows store or
/// manifest memory.
pub const remove_receipt_t = struct {
    /// Decision for the request.
    outcome: remove_outcome_t,
    /// True when a record with the requested identifier existed before the call.
    record_found: bool,
    /// Prior value of the record for the caller's restoration step. The store
    /// never restores a prior value itself. `None` when no record was found.
    prior: prior_copy_t,
    /// Final state of the artifact name for a `Removed` outcome; null otherwise.
    artifact: ?artifact_disposition_t = null,
};

/// Exclusive manifest lock and owned artifact roots.
///
/// The caller must serialize every method on one store instance on one thread
/// and obey session -> AV -> manifest lock order. flock coordinates cooperating
/// processes, but is not an intra-process mutex. `allocator` owns `roots` and
/// copied paths until deinit; callers must keep it alive for that interval.
pub const store_t = struct {
    /// Allocator used for retained roots and temporary bounded parse buffers.
    allocator: std.mem.Allocator,
    /// Owned close-on-exec descriptor for private state directory.
    dir_fd: c_int,
    /// Owned lock descriptor held exclusively while live.
    lock_fd: c_int,
    /// Owned root descriptor copies and canonical path copies.
    roots: []owned_root_t,
    /// False after deinit; no further operations are accepted.
    live: bool = true,
    /// Durable-boundary count used by deterministic in-module fault tests.
    checkpoint_count: std.atomic.Value(usize) = std.atomic.Value(usize).init(0),
    /// Test hook: fail after the selected durable-boundary checkpoint; zero disables it.
    fail_after: std.atomic.Value(usize) = std.atomic.Value(usize).init(0),

    /// Duplicate root descriptors, take a nonblocking exclusive manifest.lock,
    /// create the private transactions directory, and recover journals.
    /// Inputs are borrowed for the call; retained roots are allocator-owned.
    /// Caller acquires session then AV locks before init when needed. Returns
    /// Busy if another cooperating process owns the lock; invalid or insecure
    /// scopes return InvalidArgument, InvalidScope, or AccessDenied.
    pub fn init(allocator: std.mem.Allocator, manifest_dir_fd: c_int, roots: []const root_t) !store_t {
        if (manifest_dir_fd < 0 or roots.len == 0 or roots.len > MaxRoots) return error.InvalidArgument;
        var st: c.struct_stat = undefined;
        if (io_fstat(manifest_dir_fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFDIR or
            st.st_uid != c.getuid() or st.st_mode & 0o777 != 0o700) return error.AccessDenied;
        const dir_fd = dup_cloexec(manifest_dir_fd);
        if (dir_fd < 0) return native_error();
        errdefer _ = c.close(dir_fd);
        const owned = try allocator.alloc(owned_root_t, roots.len);
        var count: usize = 0;
        errdefer {
            for (owned[0..count]) |root| {
                allocator.free(root.canonical_path);
                _ = c.close(root.fd);
            }
            allocator.free(owned);
        }
        for (roots, 0..) |root, i| {
            try validate_absolute_root(root.canonical_path);
            if (root.dir_fd < 0) return error.InvalidArgument;
            for (roots[0..i]) |prior| {
                if (prior.kind == root.kind or paths_overlap(prior.canonical_path, root.canonical_path)) return error.InvalidArgument;
            }
            var root_stat: c.struct_stat = undefined;
            if (io_fstat(root.dir_fd, &root_stat) != 0 or root_stat.st_mode & c.S_IFMT != c.S_IFDIR or root_stat.st_mode & 0o022 != 0) return error.AccessDenied;
            try validate_root_binding(root.canonical_path, root.dir_fd);
            const root_path = try allocator.dupe(u8, root.canonical_path);
            const duplicate = dup_cloexec(root.dir_fd);
            if (duplicate < 0) {
                allocator.free(root_path);
                return native_error();
            }
            owned[i] = .{ .kind = root.kind, .canonical_path = root_path, .fd = duplicate };
            count += 1;
        }
        const lock = io_openat4(dir_fd, "manifest.lock", c.O_RDWR | c.O_CREAT | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (lock < 0) return native_error();
        errdefer _ = c.close(lock);
        var lock_stat: c.struct_stat = undefined;
        if (io_fstat(lock, &lock_stat) != 0 or lock_stat.st_mode & c.S_IFMT != c.S_IFREG or
            lock_stat.st_uid != c.getuid() or lock_stat.st_nlink != 1 or lock_stat.st_mode & 0o777 != 0o600) return error.AccessDenied;
        if (io_flock(lock, c.LOCK_EX | c.LOCK_NB) != 0) {
            if (errno_value() == c.EWOULDBLOCK or errno_value() == c.EAGAIN) return error.Busy;
            return native_error();
        }
        if (io_fsync(lock) != 0) return native_error();
        errdefer _ = io_flock(lock, c.LOCK_UN);
        var result = store_t{ .allocator = allocator, .dir_fd = dir_fd, .lock_fd = lock, .roots = owned };
        try result.ensure_private_dir("transactions");
        try result.recover_locked();
        return result;
    }

    /// Unlock and close all owned file descriptors and paths. Repeated calls
    /// are safe; allocator must still be alive and no method may be concurrent.
    pub fn deinit(self: *store_t) void {
        if (!self.live) return;
        self.live = false;
        _ = io_flock(self.lock_fd, c.LOCK_UN);
        _ = c.close(self.lock_fd);
        _ = c.close(self.dir_fd);
        for (self.roots) |root| {
            self.allocator.free(root.canonical_path);
            _ = c.close(root.fd);
        }
        self.allocator.free(self.roots);
        self.roots = &.{};
    }

    /// Retry startup recovery while retaining the exclusive lock. Returns
    /// RecoveryRequired for untrusted journal state and preserves its evidence.
    pub fn recover(self: *store_t) !void {
        if (!self.live) return error.Closed;
        try self.recover_locked();
    }

    /// Inspect one current managed record under the held store lock. Returns
    /// the current manifest digest and exact target identity by value; the
    /// snapshot is a precondition for a later operation, not a reservation.
    /// `record_id` is borrowed only for this call and no path escapes.
    /// Parameters: `self` is an initialized, live mutable store owner; `record_id`
    /// is a non-null UTF-8 identifier bounded by the manifest schema.
    /// Returns: a by-value snapshot; no caller memory is retained.
    /// Errors: Closed, InvalidArgument, FileNotFound, InvalidManifest,
    /// InvalidPath, RecoveryRequired, OutOfMemory, or a filesystem error.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn inspect_record(self: *store_t, record_id: []const u8) !record_snapshot_t {
        if (!self.live) return error.Closed;
        if (!valid_record_id(record_id)) return error.InvalidArgument;
        try self.recover_locked();
        const manifest_bytes = try self.read_manifest();
        defer self.allocator.free(manifest_bytes);
        var parsed = try manifest.parse(self.allocator, manifest_bytes);
        defer parsed.deinit();
        try validate_global_paths(parsed.records);
        const record = find_record(parsed.records, record_id) orelse return error.FileNotFound;
        const target = try self.resolve_allowed(record.*);
        defer self.allocator.free(target.relative);
        const info = (try self.inspect(target.root.fd, target.relative)) orelse return error.FileNotFound;
        return .{ .manifest_sha256 = hash_bytes(manifest_bytes), .target = identity_from_info(info) };
    }

    /// List validated retained backups for one record. The scan is a single
    /// bounded pass over `backups/`; any malformed entry fails the whole call
    /// without returning a partial list. Result memory belongs to the caller
    /// and must be released with `backup_list_t.deinit`.
    /// Parameters: `self` is an initialized, live mutable store owner;
    /// `allocator` owns returned `items`; `record_id` is borrowed and schema-bounded.
    /// Returns: at most `MaxBackups` validated references; no partial output on error.
    /// Errors: Closed, InvalidArgument, RecoveryRequired, OutOfMemory, or I/O error.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn list_backups(self: *store_t, allocator: std.mem.Allocator, record_id: []const u8) !backup_list_t {
        if (!self.live) return error.Closed;
        if (!valid_record_id(record_id)) return error.InvalidArgument;
        try self.recover_locked();
        const record_hash = hash_bytes(record_id);
        const backup_root = self.open_private_dir("backups") catch |err| {
            if (err == error.FileNotFound) return .{ .allocator = allocator, .items = try allocator.alloc(backup_ref_t, 0) };
            return err;
        };
        defer _ = c.close(backup_root);
        const scan = dup_cloexec(backup_root);
        if (scan < 0) return native_error();
        var dir = std.fs.Dir{ .fd = scan };
        defer dir.close();
        var iter = dir.iterate();
        var found: [MaxBackups]backup_ref_t = undefined;
        var count: usize = 0;
        var scanned: usize = 0;
        while (try iter.next()) |entry| {
            scanned += 1;
            if (scanned > MaxBackups or entry.kind != .directory or !valid_backup_txid(entry.name)) return error.RecoveryRequired;
            const txid = try txid_array(entry.name);
            const reference = try self.inspect_backup_entry(backup_root, txid);
            if (std.mem.eql(u8, &reference.record_id_sha256, &record_hash)) {
                found[count] = reference;
                count += 1;
            }
        }
        const items = try allocator.alloc(backup_ref_t, count);
        @memcpy(items, found[0..count]);
        return .{ .allocator = allocator, .items = items };
    }

    /// Classify every recorded artifact without publishing, unlinking, or
    /// rewriting any file. Runs the same recovery as `inspect_record` first;
    /// with no pending journal it performs no filesystem writes. Records keep
    /// manifest order. A malformed or newer-schema manifest fails before any
    /// classification, and a missing manifest yields an empty list.
    /// Parameters: `self` is an initialized, live mutable store owner;
    /// `allocator` owns the returned list and is independent of the store allocator.
    /// Returns: a snapshot owned by the caller and released with `deinit`.
    /// Errors: Closed, RecoveryRequired, InvalidManifest, UnsupportedSchema,
    /// ManifestMismatch, AccessDenied for an insecure manifest, OutOfMemory, or
    /// an I/O error that cannot be classified as missing, modified, or foreign.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn status(self: *store_t, allocator: std.mem.Allocator) !status_list_t {
        if (!self.live) return error.Closed;
        try self.recover_locked();
        const manifest_bytes = self.read_manifest() catch |err| {
            if (err == error.FileNotFound) return .{ .allocator = allocator, .items = try allocator.alloc(record_status_t, 0) };
            return err;
        };
        defer self.allocator.free(manifest_bytes);
        var parsed = try manifest.parse(self.allocator, manifest_bytes);
        defer parsed.deinit();
        try validate_global_paths(parsed.records);
        const items = try allocator.alloc(record_status_t, parsed.records.len);
        var built: usize = 0;
        errdefer {
            for (items[0..built]) |item| allocator.free(item.record_id);
            allocator.free(items);
        }
        for (parsed.records, 0..) |record, index| {
            const state: record_state_t = if (try self.resolve_for_record(record)) |target| blk: {
                defer self.allocator.free(target.relative);
                break :blk switch (try self.classify_target(target, record.sha256)) {
                    .Ok => .Ok,
                    .Missing => .Missing,
                    .Modified => .Modified,
                    .Foreign => .Foreign,
                };
            } else .Foreign;
            const record_id = try allocator.dupe(u8, record.id);
            items[index] = .{ .record_id = record_id, .state = state };
            built += 1;
        }
        return .{ .allocator = allocator, .items = items };
    }

    /// Resolve a record to an allowlisted root and relative path. Records outside
    /// the authorized roots or kinds return null so callers classify them as
    /// foreign; allocation failures propagate. The caller frees `relative`.
    fn resolve_for_record(self: *store_t, record: manifest.record_t) !?resolved_t {
        return self.resolve_allowed(record) catch |err| switch (err) {
            error.InvalidPath, error.InvalidScope, error.UnsupportedArtifactKind => null,
            else => err,
        };
    }

    /// Classify one resolved target through the rooted, no-follow walk against the
    /// record digest `record_sha256` (64 lowercase or uppercase hex bytes). Unsafe
    /// objects (symlink, hardlink, directory, foreign owner, special bits, parent
    /// symlink or non-directory) map to `.Foreign`; a missing parent or leaf maps
    /// to `.Missing`; an owned regular file with mode 0644/0755 is `.Ok` only when
    /// its digest equals the record. Allocation and unexpected I/O errors
    /// propagate. Mutates nothing.
    fn classify_target(self: *store_t, target: resolved_t, record_sha256: []const u8) !artifact_state_t {
        const info = self.inspect(target.root.fd, target.relative) catch |err| return switch (err) {
            error.FileNotFound => .Missing,
            error.AccessDenied => .Foreign,
            error.FileTooLarge => .Modified,
            else => err,
        };
        const current = info orelse return .Missing;
        if (current.mode != 0o644 and current.mode != 0o755) return .Foreign;
        if (!hash_text_equal(current.hash, record_sha256)) return .Modified;
        return .{ .Ok = current };
    }

    fn checkpoint(self: *store_t) !void {
        const step = self.checkpoint_count.fetchAdd(1, .monotonic) + 1;
        const fail_at = self.fail_after.load(.monotonic);
        if (fail_at != 0 and step == fail_at) return error.InjectedFailure;
    }

    /// Add one previously absent allowlisted host file and publish its
    /// candidate manifest. Request byte slices are borrowed for this call.
    /// Existing-target replacement and force backup return
    /// UnsupportedReplacement before transaction mutation. A prepared journal
    /// rolls back; a committed journal verifies published state and cleans up.
    /// Returns ManifestMismatch for invalid schema/delta/hash, ArtifactConflict
    /// when expected absence changed, RecoveryRequired for unsafe recovery, or
    /// an I/O/allocation error. Does not provide global multi-file atomicity.
    pub fn commit(self: *store_t, request: request_t) !void {
        if (!self.live) return error.Closed;
        try self.recover_locked();
        if (!valid_record_id(request.record_id) or request.artifact_bytes.len > MaxArtifactBytes or
            request.candidate_manifest.len == 0 or request.candidate_manifest.len > manifest.MaxManifestBytes or
            (request.artifact_mode != 0o644 and request.artifact_mode != 0o755)) return error.InvalidArgument;
        var candidate = try manifest.parse(self.allocator, request.candidate_manifest);
        defer candidate.deinit();
        try validate_global_paths(candidate.records);
        const new_record = find_record(candidate.records, request.record_id) orelse return error.ManifestMismatch;
        const target = try self.resolve_allowed(new_record.*);
        defer self.allocator.free(target.relative);
        const digest_text = hash_hex(request.artifact_bytes);
        if (!std.ascii.eqlIgnoreCase(new_record.sha256, &digest_text)) return error.ManifestMismatch;

        const old_manifest = self.read_manifest() catch |err| if (err == error.FileNotFound) null else return err;
        defer if (old_manifest) |bytes| self.allocator.free(bytes);
        var old_manifest_digest: ?[32]u8 = null;
        var old_manifest_info: ?file_info_t = null;
        if (old_manifest) |bytes| {
            old_manifest_digest = hash_bytes(bytes);
            old_manifest_info = (try self.inspect_manifest()) orelse return error.ArtifactConflict;
            if (!std.mem.eql(u8, &old_manifest_info.?.hash, &old_manifest_digest.?)) return error.ArtifactConflict;
            var parsed_old = try manifest.parse(self.allocator, bytes);
            defer parsed_old.deinit();
            try validate_global_paths(parsed_old.records);
            const old_record = find_record(parsed_old.records, request.record_id);
            try validate_delta(parsed_old.records, candidate.records, request.record_id);
            if (old_record) |old| {
                if (!same_target_identity(old.*, new_record.*)) return error.ManifestMismatch;
                return error.UnsupportedReplacement;
            } else switch (request.prior) {
                .Absent => if (candidate.records.len != parsed_old.records.len + 1) return error.ManifestMismatch,
                .Sha256 => return error.ManifestMismatch,
            }
            try validate_delta(parsed_old.records, candidate.records, request.record_id);
        } else switch (request.prior) {
            .Absent => if (candidate.records.len != 1) return error.ManifestMismatch,
            .Sha256 => return error.ManifestMismatch,
        }

        const current = try self.inspect(target.root.fd, target.relative);
        switch (request.prior) {
            .Absent => if (current != null) return error.ArtifactConflict,
            .Sha256 => return error.UnsupportedReplacement,
        }
        if (request.backup_modified) return error.UnsupportedReplacement;
        const transaction_id = try random_id();

        var has_journal = false;
        var staged_artifact: ?file_info_t = null;
        var staged_manifest: ?file_info_t = null;
        errdefer if (!has_journal) {
            if (staged_artifact) |info| self.remove_target_temp(target.root.fd, target.relative, transaction_id, info) catch {};
            if (staged_manifest) |info| self.remove_state_temp(transaction_id, info) catch {};
            self.remove_tx_tree(transaction_id) catch {};
        };
        try self.create_tx_dir(transaction_id);
        try self.write_tx_file(transaction_id, "new-artifact", request.artifact_bytes, request.artifact_mode);
        try self.write_tx_file(transaction_id, "new-manifest", request.candidate_manifest, 0o600);
        if (old_manifest) |bytes| try self.write_tx_file(transaction_id, "old-manifest", bytes, 0o600);

        staged_artifact = try self.create_target_temp(target.root.fd, target.relative, request.artifact_bytes, request.artifact_mode, transaction_id);
        staged_manifest = try self.create_state_temp(request.candidate_manifest, 0o600, transaction_id);

        const prior_manifest_digest = if (old_manifest_digest) |found| hash_hex_from_array(found) else null;
        const new_manifest_digest = hash_hex(request.candidate_manifest);
        const journal = journal_t{
            .transaction_id = &transaction_id,
            .phase = "prepared",
            .record_id = request.record_id,
            .root = root_name(target.root.kind),
            .relative_path = target.relative,
            .old_artifact_hash = null,
            .old_artifact_mode = 0,
            .new_artifact_hash = &digest_text,
            .new_artifact_mode = request.artifact_mode,
            .new_artifact_device = staged_artifact.?.device,
            .new_artifact_inode = staged_artifact.?.inode,
            .old_manifest_hash = if (prior_manifest_digest) |*digest| digest else null,
            .old_manifest_device = if (old_manifest_info) |info| info.device else null,
            .old_manifest_inode = if (old_manifest_info) |info| info.inode else null,
            .new_manifest_hash = &new_manifest_digest,
            .new_manifest_device = staged_manifest.?.device,
            .new_manifest_inode = staged_manifest.?.inode,
        };
        try self.write_journal(transaction_id, journal);
        has_journal = true;
        try self.checkpoint();
        self.apply_prepared(journal) catch |err| {
            return err;
        };
        try self.checkpoint();
        self.publish_phase(transaction_id, journal, "committed") catch |err| {
            return err;
        };
        try self.checkpoint();
        self.apply_committed(journal) catch |err| {
            return err;
        };
        try self.remove_published_old_manifest_temp(transaction_id, journal);
        self.remove_tx_tree(transaction_id) catch |err| {
            return err;
        };
    }

    /// Replace one existing managed artifact under the store's existing lock.
    /// The candidate manifest may change only the selected record's digest and
    /// creator version. The request is borrowed for this call. If the current
    /// artifact differs from its old manifest digest, `backup_modified` must
    /// be true; the exact bytes and mode are durably retained before PREPARED.
    /// An exact manifest, artifact-byte, and mode no-op returns the current
    /// identity with no backup and performs no filesystem mutation. A manifest
    /// byte-identical request that would change artifact bytes or mode is
    /// rejected as UnsupportedReplacement because its journal would have no
    /// distinct old and new manifest image.
    /// A prepared journal rolls back by exchanging the exact old inode back;
    /// committed recovery verifies the new artifact and retains the backup.
    /// Returns ManifestMismatch, ArtifactConflict, UnsupportedReplacement,
    /// RecoveryRequired, OutOfMemory, or a filesystem error on failure.
    /// Parameters: `self` is an initialized, live mutable store owner;
    /// `request` and its slices are borrowed only for the call.
    /// Returns: exact published target identity and optional retained backup
    /// reference by value; no request memory or path is retained.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn replace(self: *store_t, request: replace_request_t) !replace_receipt_t {
        if (!self.live) return error.Closed;
        try self.recover_locked();
        if (!valid_record_id(request.record_id) or request.artifact_bytes.len > MaxArtifactBytes or
            request.candidate_manifest.len == 0 or request.candidate_manifest.len > manifest.MaxManifestBytes or
            (request.artifact_mode != 0o644 and request.artifact_mode != 0o755)) return error.InvalidArgument;

        var candidate = try manifest.parse(self.allocator, request.candidate_manifest);
        defer candidate.deinit();
        try validate_global_paths(candidate.records);
        const new_record = find_record(candidate.records, request.record_id) orelse return error.ManifestMismatch;
        const target = try self.resolve_allowed(new_record.*);
        defer self.allocator.free(target.relative);
        const new_digest = hash_hex(request.artifact_bytes);
        if (!std.ascii.eqlIgnoreCase(new_record.sha256, &new_digest)) return error.ManifestMismatch;

        const old_manifest = try self.read_manifest();
        defer self.allocator.free(old_manifest);
        const old_manifest_digest = hash_bytes(old_manifest);
        const old_manifest_info = (try self.inspect_manifest()) orelse return error.ArtifactConflict;
        if (!std.mem.eql(u8, &old_manifest_info.hash, &old_manifest_digest)) return error.ArtifactConflict;
        if (!std.mem.eql(u8, &old_manifest_digest, &request.expected_manifest_sha256)) return error.ArtifactConflict;
        var parsed_old = try manifest.parse(self.allocator, old_manifest);
        defer parsed_old.deinit();
        try validate_global_paths(parsed_old.records);
        const old_record = find_record(parsed_old.records, request.record_id) orelse return error.UnsupportedReplacement;
        if (!same_target_identity(old_record.*, new_record.*) or parsed_old.records.len != candidate.records.len)
            return error.ManifestMismatch;
        try validate_delta(parsed_old.records, candidate.records, request.record_id);

        const current = (try self.inspect(target.root.fd, target.relative)) orelse return error.ArtifactConflict;
        if (!same_identity(current, request.expected_target)) return error.ArtifactConflict;
        if (current.mode != 0o644 and current.mode != 0o755) return error.AccessDenied;
        const current_hash_text = hash_hex_from_array(current.hash);
        const modified = !std.ascii.eqlIgnoreCase(old_record.sha256, &current_hash_text);
        if (modified and !request.backup_modified) return error.UnsupportedReplacement;
        const old_artifact = try self.read_target_bytes(target.root.fd, target.relative);
        defer self.allocator.free(old_artifact);
        if (!std.mem.eql(u8, &hash_bytes(old_artifact), &current.hash)) return error.ArtifactConflict;
        const confirmed = (try self.inspect(target.root.fd, target.relative)) orelse return error.ArtifactConflict;
        if (!same_file_info(current, confirmed)) return error.ArtifactConflict;
        if (std.mem.eql(u8, old_manifest, request.candidate_manifest)) {
            if (current.mode == request.artifact_mode and std.mem.eql(u8, old_artifact, request.artifact_bytes))
                return .{ .target = identity_from_info(current), .backup = null };
            return error.UnsupportedReplacement;
        }

        const transaction_id = try random_id();
        var has_journal = false;
        var staged_artifact: ?file_info_t = null;
        var staged_manifest: ?file_info_t = null;
        var backup_info: ?file_info_t = null;
        errdefer if (!has_journal) {
            if (staged_artifact) |info| self.remove_target_temp(target.root.fd, target.relative, transaction_id, info) catch {};
            if (staged_manifest) |info| self.remove_state_temp(transaction_id, info) catch {};
            self.remove_tx_tree(transaction_id) catch {};
        };
        try self.create_tx_dir(transaction_id);
        try self.write_tx_file(transaction_id, "new-artifact", request.artifact_bytes, request.artifact_mode);
        try self.write_tx_file(transaction_id, "new-manifest", request.candidate_manifest, 0o600);
        try self.write_tx_file(transaction_id, "old-artifact", old_artifact, @intCast(current.mode));
        try self.write_tx_file(transaction_id, "old-manifest", old_manifest, 0o600);
        if (modified) {
            backup_info = try self.create_backup(transaction_id, request.record_id, old_artifact, @intCast(current.mode));
        }
        staged_artifact = try self.create_target_temp(target.root.fd, target.relative, request.artifact_bytes, request.artifact_mode, transaction_id);
        staged_manifest = try self.create_state_temp(request.candidate_manifest, 0o600, transaction_id);

        const old_manifest_hash = hash_hex_from_array(old_manifest_digest);
        const new_manifest_hash = hash_hex(request.candidate_manifest);
        const old_artifact_hash = hash_hex_from_array(current.hash);
        const backup_hash = if (backup_info) |info| hash_hex_from_array(info.hash) else null;
        const journal = journal_t{
            .version = 2,
            .transaction_id = &transaction_id,
            .phase = "prepared",
            .record_id = request.record_id,
            .root = root_name(target.root.kind),
            .relative_path = target.relative,
            .old_artifact_hash = &old_artifact_hash,
            .old_artifact_mode = current.mode,
            .new_artifact_hash = &new_digest,
            .new_artifact_mode = request.artifact_mode,
            .new_artifact_device = staged_artifact.?.device,
            .new_artifact_inode = staged_artifact.?.inode,
            .old_manifest_hash = &old_manifest_hash,
            .old_manifest_device = old_manifest_info.device,
            .old_manifest_inode = old_manifest_info.inode,
            .new_manifest_hash = &new_manifest_hash,
            .new_manifest_device = staged_manifest.?.device,
            .new_manifest_inode = staged_manifest.?.inode,
            .old_artifact_device = current.device,
            .old_artifact_inode = current.inode,
            .backup_hash = if (backup_hash) |*hash| hash else null,
            .backup_mode = if (backup_info) |info| info.mode else 0,
            .backup_device = if (backup_info) |info| info.device else null,
            .backup_inode = if (backup_info) |info| info.inode else null,
        };
        try self.write_journal(transaction_id, journal);
        has_journal = true;
        try self.checkpoint();
        try self.apply_prepared(journal);
        try self.checkpoint();
        try self.publish_phase(transaction_id, journal, "committed");
        try self.checkpoint();
        try self.apply_committed(journal);
        try self.remove_target_temp(target.root.fd, target.relative, transaction_id, current);
        try self.remove_published_old_manifest_temp(transaction_id, journal);
        try self.remove_tx_tree(transaction_id);
        return .{
            .target = identity_from_info(staged_artifact.?),
            .backup = if (backup_info) |info| backup_reference(transaction_id, request.record_id, info) else null,
        };
    }

    /// Restore bytes from one explicit retained backup through the same
    /// replacement journal. The backup is validated by its derived private
    /// name and exact device/inode/hash/mode before its bytes are read. The
    /// request slices are borrowed only for this call; the backup remains
    /// retained after success and failure.
    /// Parameters: `self` is an initialized, live mutable store owner;
    /// `request` and its slices are borrowed only for the call.
    /// Returns: restored target identity by value; backup bytes remain private.
    /// Errors: Closed, InvalidArgument, ArtifactConflict, ManifestMismatch,
    /// RecoveryRequired, OutOfMemory, or a filesystem error.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn restore(self: *store_t, request: restore_request_t) !restore_receipt_t {
        if (!self.live) return error.Closed;
        if (!valid_record_id(request.record_id)) return error.InvalidArgument;
        const backup = try self.read_backup(request.record_id, request.backup);
        defer self.allocator.free(backup);
        const digest = hash_hex(backup);
        var candidate = try manifest.parse(self.allocator, request.candidate_manifest);
        defer candidate.deinit();
        const record = find_record(candidate.records, request.record_id) orelse return error.ManifestMismatch;
        if (!std.ascii.eqlIgnoreCase(record.sha256, &digest)) return error.ManifestMismatch;
        const replaced = try self.replace(.{
            .record_id = request.record_id,
            .expected_manifest_sha256 = request.expected_manifest_sha256,
            .expected_target = request.expected_target,
            .artifact_bytes = backup,
            .artifact_mode = @intCast(request.backup.artifact_mode),
            .candidate_manifest = request.candidate_manifest,
            .backup_modified = false,
        });
        // This call disables backup creation, and replace rejects modified
        // targets before staging; a restore therefore cannot return a backup.
        return .{ .target = replaced.target };
    }

    /// Remove one recorded artifact through the journaled rooted engine. The
    /// store decides the outcome under the held lock:
    /// - `Removed`: the artifact is an owned regular file with mode 0644/0755 and
    ///   a digest equal to the record. The candidate manifest is published in the
    ///   PREPARED phase, the artifact is unlinked only after COMMITTED is durable,
    ///   and the journal is retired.
    /// - `KeptModified`, `KeptForeign`, `AlreadyMissing`: nothing on disk changes
    ///   and the record stays, as IMPL_DESC 3.12.4 requires for reported items.
    /// A missing manifest or missing record returns `AlreadyMissing` with
    /// `record_found = false`, so repeated calls after a removal are harmless.
    /// Prior values are returned in the receipt; this method never restores them.
    /// Parameters: `self` is an initialized, live mutable store owner; `request`
    /// and its slices are borrowed only for the call.
    /// Returns: a by-value receipt with no borrowed memory.
    /// Errors: Closed, InvalidArgument, InvalidManifest, UnsupportedSchema,
    /// ManifestMismatch, ArtifactConflict, RecoveryRequired, AccessDenied,
    /// OutOfMemory, or a filesystem error. Errors before the PREPARED journal
    /// leave the manifest and artifact unchanged.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn remove(self: *store_t, request: remove_request_t) !remove_receipt_t {
        if (!self.live) return error.Closed;
        try self.recover_locked();
        if (!valid_record_id(request.record_id) or request.candidate_manifest.len == 0 or
            request.candidate_manifest.len > manifest.MaxManifestBytes) return error.InvalidArgument;
        var candidate = try manifest.parse(self.allocator, request.candidate_manifest);
        defer candidate.deinit();
        try validate_global_paths(candidate.records);

        const snapshot = self.read_manifest_snapshot() catch |err| {
            if (err == error.FileNotFound) return .{ .outcome = .AlreadyMissing, .record_found = false, .prior = .None };
            return err;
        };
        defer self.allocator.free(snapshot.bytes);
        var parsed = try manifest.parse(self.allocator, snapshot.bytes);
        defer parsed.deinit();
        try validate_global_paths(parsed.records);
        const record = find_record(parsed.records, request.record_id) orelse
            return .{ .outcome = .AlreadyMissing, .record_found = false, .prior = .None };
        const prior = copy_prior(record.prior);
        if (!store_managed_kind(record.kind)) {
            try self.forget_record(snapshot, parsed.records, candidate.records, request.record_id, request.candidate_manifest);
            return .{ .outcome = .RecordRemovedNonFile, .record_found = true, .prior = prior };
        }
        const target = (try self.resolve_for_record(record.*)) orelse
            return .{ .outcome = .KeptForeign, .record_found = true, .prior = prior };
        defer self.allocator.free(target.relative);
        const artifact = switch (try self.classify_target(target, record.sha256)) {
            .Ok => |info| info,
            .Missing => {
                try self.forget_record(snapshot, parsed.records, candidate.records, request.record_id, request.candidate_manifest);
                return .{ .outcome = .RecordRemovedArtifactMissing, .record_found = true, .prior = prior };
            },
            .Modified => return .{ .outcome = .KeptModified, .record_found = true, .prior = prior },
            .Foreign => return .{ .outcome = .KeptForeign, .record_found = true, .prior = prior },
        };
        try validate_removal(parsed.records, candidate.records, request.record_id);
        const transaction = try self.run_manifest_transaction(.{
            .operation = "remove",
            .record_id = request.record_id,
            .root = target.root,
            .relative = target.relative,
            .artifact = artifact,
            .old_manifest = snapshot.bytes,
            .old_info = snapshot.info,
            .candidate = request.candidate_manifest,
        });
        return .{ .outcome = .Removed, .record_found = true, .prior = prior, .artifact = transaction.artifact };
    }

    /// Rewrite the manifest-level schema metadata for one recorded artifact
    /// through the journaled engine. The artifact file is never opened for
    /// writing, renamed, or unlinked, and its state is not a precondition.
    /// The candidate must keep every record equal by schema fields (id, kind,
    /// path, sha256, device, app_key, created_by, prior, mime); only the
    /// root-level `waddle_version` and `updated` may change. Schema 1 has no
    /// per-record migration field, so there is no migrated flag to set.
    /// A candidate byte-identical to the current manifest returns `changed = false`
    /// with no journal and no filesystem write. A candidate with another schema
    /// version is refused by the codec with UnsupportedSchema before any mutation.
    /// Parameters: `self` is an initialized, live mutable store owner; `request`
    /// and its slices are borrowed only for the call.
    /// Returns: a by-value receipt with no borrowed memory.
    /// Errors: Closed, InvalidArgument, InvalidManifest, UnsupportedSchema,
    /// ManifestMismatch, FileNotFound (no manifest or no such record),
    /// RecoveryRequired, ArtifactConflict, AccessDenied, OutOfMemory, or a
    /// filesystem error. Errors before the PREPARED journal change nothing.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn migrate_record(self: *store_t, request: migrate_request_t) !migrate_receipt_t {
        if (!self.live) return error.Closed;
        try self.recover_locked();
        if (!valid_record_id(request.record_id) or request.candidate_manifest.len == 0 or
            request.candidate_manifest.len > manifest.MaxManifestBytes) return error.InvalidArgument;
        var candidate = try manifest.parse(self.allocator, request.candidate_manifest);
        defer candidate.deinit();
        try validate_global_paths(candidate.records);
        const snapshot = try self.read_manifest_snapshot();
        defer self.allocator.free(snapshot.bytes);
        var parsed = try manifest.parse(self.allocator, snapshot.bytes);
        defer parsed.deinit();
        try validate_global_paths(parsed.records);
        if (find_record(parsed.records, request.record_id) == null) return error.FileNotFound;
        try validate_metadata_only(parsed.records, candidate.records);
        if ((try version_order(candidate.waddle_version, parsed.waddle_version)) == .lt) return error.VersionDowngrade;
        if (std.mem.eql(u8, snapshot.bytes, request.candidate_manifest)) return .{ .changed = false };
        _ = try self.run_manifest_transaction(.{
            .operation = "migrate",
            .record_id = request.record_id,
            .old_manifest = snapshot.bytes,
            .old_info = snapshot.info,
            .candidate = request.candidate_manifest,
        });
        return .{ .changed = true };
    }

    /// Publish an absent artifact for an existing record through the journaled
    /// engine. The target must not exist; a file of any kind yields ArtifactConflict
    /// before mutation. The record keeps its target identity; the candidate may
    /// change only its digest and creator, which must match the new bytes.
    /// Parameters: `self` is an initialized, live mutable store owner; `request` and
    /// its slices are borrowed only for the call.
    /// Returns: the published artifact identity by value.
    /// Errors: Closed, InvalidArgument (also for a candidate byte-identical to the
    /// current manifest), InvalidManifest, UnsupportedSchema,
    /// ManifestMismatch, FileNotFound (no manifest or record, or missing parent),
    /// ArtifactConflict, RecoveryRequired, AccessDenied, OutOfMemory, or filesystem
    /// errors. Errors before the PREPARED journal change nothing published.
    /// Thread safety: serialized owner-thread use only; do not call concurrently.
    pub fn recreate(self: *store_t, request: recreate_request_t) !recreate_receipt_t {
        if (!self.live) return error.Closed;
        try self.recover_locked();
        if (!valid_record_id(request.record_id) or request.artifact_bytes.len == 0 or
            request.artifact_bytes.len > MaxArtifactBytes or request.candidate_manifest.len == 0 or
            request.candidate_manifest.len > manifest.MaxManifestBytes or
            (request.artifact_mode != 0o644 and request.artifact_mode != 0o755)) return error.InvalidArgument;
        var candidate = try manifest.parse(self.allocator, request.candidate_manifest);
        defer candidate.deinit();
        try validate_global_paths(candidate.records);
        const snapshot = try self.read_manifest_snapshot();
        defer self.allocator.free(snapshot.bytes);
        var parsed = try manifest.parse(self.allocator, snapshot.bytes);
        defer parsed.deinit();
        try validate_global_paths(parsed.records);
        if (std.mem.eql(u8, snapshot.bytes, request.candidate_manifest)) return error.InvalidArgument;
        const old_record = find_record(parsed.records, request.record_id) orelse return error.FileNotFound;
        const new_record = find_record(candidate.records, request.record_id) orelse return error.ManifestMismatch;
        if (!same_recreate_identity(old_record.*, new_record.*)) return error.ManifestMismatch;
        const digest_text = hash_hex(request.artifact_bytes);
        if (!std.ascii.eqlIgnoreCase(new_record.sha256, &digest_text)) return error.ManifestMismatch;
        try validate_recreate_delta(parsed.records, candidate.records, request.record_id);
        const target = (try self.resolve_for_record(old_record.*)) orelse return error.InvalidPath;
        defer self.allocator.free(target.relative);
        if ((try self.inspect(target.root.fd, target.relative)) != null) return error.ArtifactConflict;
        const transaction = try self.run_manifest_transaction(.{
            .operation = "recreate",
            .record_id = request.record_id,
            .root = target.root,
            .relative = target.relative,
            .old_manifest = snapshot.bytes,
            .old_info = snapshot.info,
            .candidate = request.candidate_manifest,
            .new_artifact_bytes = request.artifact_bytes,
            .new_artifact_mode = request.artifact_mode,
        });
        const staged = transaction.staged orelse return error.RecoveryRequired;
        return .{ .target = identity_from_info(staged) };
    }

    /// Create every missing directory component of `relative_dir` below the root of
    /// `kind`, walking each component with no-follow descriptors. Existing components
    /// must be current-user-owned directories without group or other write permission.
    /// Missing directories are created with mode 0755. Not journaled: a directory is
    /// not an artifact and an interrupted call leaves only ordinary empty directories.
    /// Errors: Closed, InvalidScope, InvalidPath, AccessDenied, OutOfMemory, or I/O.
    /// Thread safety: serialized owner-thread use only.
    pub fn ensure_directory(self: *store_t, kind: root_kind_t, relative_dir: []const u8) !void {
        if (!self.live) return error.Closed;
        const root = self.find_root(kind) orelse return error.InvalidScope;
        try validate_relative(relative_dir);
        var current = dup_cloexec(root.fd);
        if (current < 0) return native_error();
        errdefer _ = c.close(current);
        var parts = std.mem.splitScalar(u8, relative_dir, '/');
        while (parts.next()) |part| {
            const name = try self.allocator.dupeZ(u8, part);
            defer self.allocator.free(name);
            if (io_mkdirat(current, name, 0o755) != 0 and errno_value() != c.EEXIST) return native_error();
            const next = io_openat3(current, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
            if (next < 0) return native_error();
            var st: c.struct_stat = undefined;
            if (io_fstat(next, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFDIR or st.st_uid != c.getuid() or st.st_mode & 0o022 != 0) {
                _ = c.close(next);
                return error.AccessDenied;
            }
            _ = c.close(current);
            current = next;
        }
        defer _ = c.close(current);
        if (io_fsync(current) != 0 or io_fsync(root.fd) != 0) return native_error();
    }

    /// Copy the current manifest bytes into `allocator`, or return null when no manifest
    /// exists. The method runs the same recovery as the other read paths first and never
    /// writes. The returned bytes are caller-owned.
    /// Errors: Closed, RecoveryRequired, AccessDenied, OutOfMemory, or an I/O error.
    /// Thread safety: serialized owner-thread use only.
    pub fn manifest_snapshot(self: *store_t, allocator: std.mem.Allocator) !?[]u8 {
        if (!self.live) return error.Closed;
        try self.recover_locked();
        const snapshot = self.read_manifest_snapshot() catch |err| {
            if (err == error.FileNotFound) return null;
            return err;
        };
        defer self.allocator.free(snapshot.bytes);
        return try allocator.dupe(u8, snapshot.bytes);
    }

    /// Verify a recreate journal's staged manifests and record delta rules, then
    /// the record resolves to the journal's root and relative path with the digest
    /// that the journal recorded for the artifact. Every failure is RecoveryRequired.
    fn validate_recreate_evidence(self: *store_t, journal: journal_v3_t, old_records: []const manifest.record_t, new_records: []const manifest.record_t) !void {
        validate_recreate_delta(old_records, new_records, journal.record_id) catch return error.RecoveryRequired;
        const new_record = find_record(new_records, journal.record_id) orelse return error.RecoveryRequired;
        const root = self.find_root_by_name(journal.root.?) orelse return error.RecoveryRequired;
        const target = self.resolve_for_record(new_record.*) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        const resolved = target orelse return error.RecoveryRequired;
        defer self.allocator.free(resolved.relative);
        if (resolved.root.kind != root.kind or !std.mem.eql(u8, resolved.relative, journal.relative_path.?) or
            !std.ascii.eqlIgnoreCase(new_record.sha256, journal.new_artifact_hash.?)) return error.RecoveryRequired;
    }

    /// Delete one record without touching any file, in a version-3 `forget`
    /// transaction. The candidate must be the current manifest minus exactly that record.
    fn forget_record(self: *store_t, snapshot: manifest_snapshot_t, old_records: []const manifest.record_t, new_records: []const manifest.record_t, record_id: []const u8, candidate: []const u8) !void {
        try validate_removal(old_records, new_records, record_id);
        _ = try self.run_manifest_transaction(.{
            .operation = "forget",
            .record_id = record_id,
            .old_manifest = snapshot.bytes,
            .old_info = snapshot.info,
            .candidate = candidate,
        });
    }

    /// Shared journaled engine for version-3 operations. `spec.artifact` is set
    /// only for removal, where the target must stay unchanged until COMMITTED.
    /// PREPARED publishes the candidate manifest; COMMITTED unlinks a removed
    /// artifact only after the manifest is durable. Any error after the journal
    /// is written is left to `recover` and the journal stays as evidence.
    /// Result of one version-3 transaction: the staged artifact identity for a
    /// recreate, and the final artifact disposition for a removal.
    const transaction_result_t = struct { staged: ?file_info_t, artifact: ?artifact_disposition_t };

    fn run_manifest_transaction(self: *store_t, spec: transaction_spec_t) !transaction_result_t {
        const transaction_id = try random_id();
        var has_journal = false;
        var staged_manifest: ?file_info_t = null;
        var staged_artifact: ?file_info_t = null;
        errdefer if (!has_journal) {
            if (spec.root) |root| if (staged_artifact) |info| self.remove_target_temp(root.fd, spec.relative.?, transaction_id, info) catch {};
            if (staged_manifest) |info| self.remove_state_temp(transaction_id, info) catch {};
            self.remove_tx_tree(transaction_id) catch {};
        };
        try self.create_tx_dir(transaction_id);
        try self.write_tx_file(transaction_id, "new-manifest", spec.candidate, 0o600);
        try self.write_tx_file(transaction_id, "old-manifest", spec.old_manifest, 0o600);
        if (spec.new_artifact_bytes) |bytes| {
            const root = spec.root orelse return error.InvalidArgument;
            staged_artifact = try self.create_target_temp(root.fd, spec.relative.?, bytes, spec.new_artifact_mode, transaction_id);
        }
        staged_manifest = try self.create_state_temp(spec.candidate, 0o600, transaction_id);

        const new_hash = hash_hex(spec.candidate);
        const old_hash = hash_hex_from_array(spec.old_info.hash);
        const artifact_hash: [64]u8 = if (spec.artifact) |info| hash_hex_from_array(info.hash) else [_]u8{'0'} ** 64;
        const new_artifact_hash: [64]u8 = if (staged_artifact) |info| hash_hex_from_array(info.hash) else [_]u8{'0'} ** 64;
        const journal = journal_v3_t{
            .version = 3,
            .transaction_id = &transaction_id,
            .phase = "prepared",
            .operation = spec.operation,
            .record_id = spec.record_id,
            .root = if (spec.root) |root| root_name(root.kind) else null,
            .relative_path = spec.relative,
            .old_artifact_hash = if (spec.artifact != null) artifact_hash[0..] else null,
            .old_artifact_mode = if (spec.artifact) |info| info.mode else 0,
            .old_artifact_device = if (spec.artifact) |info| info.device else null,
            .old_artifact_inode = if (spec.artifact) |info| info.inode else null,
            .old_manifest_hash = old_hash[0..],
            .old_manifest_device = spec.old_info.device,
            .old_manifest_inode = spec.old_info.inode,
            .new_manifest_hash = new_hash[0..],
            .new_manifest_device = staged_manifest.?.device,
            .new_manifest_inode = staged_manifest.?.inode,
            .new_artifact_hash = if (staged_artifact != null) new_artifact_hash[0..] else null,
            .new_artifact_mode = if (staged_artifact) |info| info.mode else 0,
            .new_artifact_device = if (staged_artifact) |info| info.device else null,
            .new_artifact_inode = if (staged_artifact) |info| info.inode else null,
        };
        try self.write_journal(transaction_id, journal);
        has_journal = true;
        try self.checkpoint();
        try self.apply_prepared_v3(journal);
        try self.checkpoint();
        try self.publish_phase(transaction_id, journal, "committed");
        try self.checkpoint();
        const disposition = try self.apply_committed_v3(journal);
        try self.remove_displaced_manifest_temp(transaction_id, v3_old_manifest_info(journal));
        if (v3_is_recreate(journal)) try self.remove_target_temp(spec.root.?.fd, spec.relative.?, transaction_id, v3_new_artifact_info(journal));
        try self.remove_tx_tree(transaction_id);
        return .{ .staged = staged_artifact, .artifact = disposition };
    }

    /// Publish the candidate manifest for a PREPARED version-3 journal. The
    /// artifact of a removal must still be exactly the recorded identity. A
    /// manifest already holding the candidate is accepted only with the displaced
    /// old manifest at the temp name, or without it after a cleanup.
    fn apply_prepared_v3(self: *store_t, journal: journal_v3_t) !void {
        const txid = try txid_array(journal.transaction_id);
        if (v3_is_remove(journal)) try self.expect_v3_artifact(journal);
        if (v3_is_recreate(journal)) try self.publish_v3_artifact(txid, journal);
        const old_info = v3_old_manifest_info(journal);
        const new_info = v3_new_manifest_info(journal);
        const current = (try self.inspect_manifest()) orelse return error.ArtifactConflict;
        const published = if (same_file_info(current, new_info)) true else if (same_file_info(current, old_info)) false else return error.ArtifactConflict;
        const manifest_temp = manifest_temp_name(txid);
        if (published) {
            try self.validate_optional_temp(self.dir_fd, manifest_temp[0..].ptr, old_info, false, false);
        } else {
            try self.validate_optional_temp(self.dir_fd, manifest_temp[0..].ptr, new_info, true, false);
            try self.publish_manifest(txid, new_info, journal.old_manifest_hash, old_info);
        }
    }

    /// Restore the old manifest for a PREPARED version-3 journal, or do nothing
    /// when the candidate was never published. The artifact is never touched in
    /// this phase, so a changed artifact fails closed with ArtifactConflict.
    /// A removal never touched its artifact in PREPARED state, so rollback restores
    /// only the manifest and ignores the artifact whatever the user did to it. A
    /// recreated artifact is unlinked only when it still carries the recorded
    /// identity; any other file at that path belongs to the user and is kept.
    fn rollback_prepared_v3(self: *store_t, journal: journal_v3_t) !void {
        const txid = try txid_array(journal.transaction_id);
        const recreated_live = if (v3_is_recreate(journal)) (try self.recorded_artifact_live(journal, v3_new_artifact_info(journal))) != null else false;
        switch (try self.classify_v3_manifest(journal)) {
            .Candidate => try self.restore_displaced_manifest(txid, journal),
            .Original => {},
        }
        if (recreated_live) try self.unpublish_v3_artifact(journal);
    }

    /// Return the live artifact identity when it is exactly `expected`. Absent,
    /// unsafe (AccessDenied), or user-changed files all return null so the caller
    /// keeps them; other errors propagate.
    fn recorded_artifact_live(self: *store_t, journal: journal_v3_t, expected: file_info_t) !?file_info_t {
        const found = self.inspect_v3_artifact(journal) catch |err| {
            if (err == error.AccessDenied) return null;
            return err;
        };
        const current = found orelse return null;
        if (!same_file_info(current, expected)) return null;
        return current;
    }

    /// Publish a staged recreate artifact with no-clobber semantics. The target
    /// must be absent or already carry the recorded identity (an earlier attempt).
    fn publish_v3_artifact(self: *store_t, txid: [32]u8, journal: journal_v3_t) !void {
        const root = self.find_root_by_name(journal.root.?) orelse return error.ArtifactConflict;
        const relative = journal.relative_path.?;
        const info = v3_new_artifact_info(journal);
        if (try self.inspect_v3_artifact(journal)) |current| {
            if (!same_file_info(current, info)) return error.ArtifactConflict;
            return;
        }
        try self.publish_file(root.fd, relative, txid, info);
    }

    /// Unlink a recreated artifact during rollback when its recorded identity is live.
    fn unpublish_v3_artifact(self: *store_t, journal: journal_v3_t) !void {
        const root = self.find_root_by_name(journal.root.?) orelse return error.RecoveryRequired;
        _ = try self.unlink_owned_artifact(root.fd, journal.relative_path.?, v3_new_artifact_info(journal), try txid_array(journal.transaction_id));
    }

    /// Identify the live manifest as the recorded candidate or the recorded old
    /// manifest. Any other manifest, or a missing one, is an ArtifactConflict.
    fn classify_v3_manifest(self: *store_t, journal: journal_v3_t) !v3_manifest_state_t {
        const current = (try self.inspect_manifest()) orelse return error.ArtifactConflict;
        if (same_file_info(current, v3_new_manifest_info(journal))) return .Candidate;
        if (same_file_info(current, v3_old_manifest_info(journal))) return .Original;
        return error.ArtifactConflict;
    }

    /// Exchange the displaced old manifest back into place after a published
    /// candidate. The displaced temp must hold exactly the recorded old identity.
    fn restore_displaced_manifest(self: *store_t, txid: [32]u8, journal: journal_v3_t) !void {
        const manifest_temp = manifest_temp_name(txid);
        const displaced = (try self.inspect_named(self.dir_fd, manifest_temp[0..].ptr)) orelse return error.RecoveryRequired;
        if (!same_file_info(displaced, v3_old_manifest_info(journal))) return error.ArtifactConflict;
        try self.publish_manifest(txid, v3_old_manifest_info(journal), journal.new_manifest_hash, v3_new_manifest_info(journal));
    }

    /// Finish a COMMITTED version-3 journal. The manifest must already hold the
    /// candidate. A removal unlinks the artifact only if it still has the recorded
    /// identity; an already absent artifact is the result of an earlier attempt.
    /// Every parent, the state directory, the root, and the journal are synced.
    fn apply_committed_v3(self: *store_t, journal: journal_v3_t) !?artifact_disposition_t {
        const txid = try txid_array(journal.transaction_id);
        const current_manifest = (try self.inspect_manifest()) orelse return error.RecoveryRequired;
        if (!same_file_info(current_manifest, v3_new_manifest_info(journal))) return error.RecoveryRequired;
        try self.validate_displaced_manifest_temp(txid, v3_old_manifest_info(journal));
        const disposition: ?artifact_disposition_t = if (v3_is_remove(journal)) try self.retire_v3_artifact(journal) else null;

        const transactions_fd = try self.open_private_dir("transactions");
        defer _ = c.close(transactions_fd);
        const tx_name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(tx_name);
        const tx_fd = io_openat3(transactions_fd, tx_name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (tx_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(tx_fd);
        const journal_fd = io_openat3(tx_fd, "journal.json", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (journal_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(journal_fd);
        if (io_fsync(journal_fd) != 0 or io_fsync(tx_fd) != 0 or io_fsync(self.dir_fd) != 0) return native_error();
        if (journal.root) |name| {
            const root = self.find_root_by_name(name) orelse return error.RecoveryRequired;
            const parent = try self.open_parent(root.fd, journal.relative_path.?);
            defer _ = c.close(parent.fd);
            if (io_fsync(parent.fd) != 0 or io_fsync(root.fd) != 0) return native_error();
        }
        return disposition;
    }

    /// Verify the live artifact of a removal journal in PREPARED state. Absence
    /// and any other identity are conflicts because no phase before COMMITTED
    /// may change the artifact.
    fn expect_v3_artifact(self: *store_t, journal: journal_v3_t) !void {
        const current = (try self.inspect_v3_artifact(journal)) orelse return error.ArtifactConflict;
        if (!same_file_info(current, v3_artifact_info(journal))) return error.ArtifactConflict;
    }

    /// Unlink the removed artifact of a COMMITTED journal when its recorded
    /// identity is still present. Absence is accepted as a completed unlink.
    fn retire_v3_artifact(self: *store_t, journal: journal_v3_t) !artifact_disposition_t {
        const root = self.find_root_by_name(journal.root.?) orelse return error.RecoveryRequired;
        return self.unlink_owned_artifact(root.fd, journal.relative_path.?, v3_artifact_info(journal), try txid_array(journal.transaction_id));
    }

    /// Inspect the artifact named by a removal journal. A missing parent or leaf
    /// is returned as null; other errors propagate unchanged.
    fn inspect_v3_artifact(self: *store_t, journal: journal_v3_t) !?file_info_t {
        const root = self.find_root_by_name(journal.root.?) orelse return error.RecoveryRequired;
        const found = self.inspect(root.fd, journal.relative_path.?) catch |err| {
            if (err == error.FileNotFound) return null;
            return err;
        };
        return found;
    }

    /// Verify the displaced old manifest at the manifest temp name. Absence is
    /// accepted after an earlier cleanup; any other identity fails closed.
    fn validate_displaced_manifest_temp(self: *store_t, txid: [32]u8, expected: file_info_t) !void {
        const temp = manifest_temp_name(txid);
        const found = (try self.inspect_named(self.dir_fd, temp[0..].ptr)) orelse return;
        if (!same_file_info(found, expected)) return error.ArtifactConflict;
    }

    /// Delete the verified displaced old manifest after a COMMITTED publish.
    fn remove_displaced_manifest_temp(self: *store_t, txid: [32]u8, expected: file_info_t) !void {
        const temp = manifest_temp_name(txid);
        const found = (try self.inspect_named(self.dir_fd, temp[0..].ptr)) orelse return;
        if (!same_file_info(found, expected)) return error.ArtifactConflict;
        try unlink_named_if_matches(self.allocator, self.dir_fd, temp[0..].ptr, found);
        if (io_fsync(self.dir_fd) != 0) return native_error();
        try self.checkpoint();
    }

    /// Validate and finish one version-3 journal during `recover`. The journal
    /// names only identities this store wrote; every manifest and artifact fact
    /// is rechecked before any rollback, roll-forward, or cleanup.
    fn recover_v3(self: *store_t, entry_name: []const u8, bytes: []const u8) !void {
        var parsed = try parse_journal_v3(self.allocator, bytes);
        defer parsed.deinit();
        const journal = parsed.value;
        try validate_journal_v3(journal, entry_name);
        const txid = try txid_array(journal.transaction_id);
        try self.validate_v3_manifests(journal);
        if (std.mem.eql(u8, journal.phase, "prepared")) {
            try self.rollback_prepared_v3(journal);
            try self.remove_state_temp(txid, v3_new_manifest_info(journal));
        } else {
            _ = try self.apply_committed_v3(journal);
            try self.remove_displaced_manifest_temp(txid, v3_old_manifest_info(journal));
        }
        if (v3_is_recreate(journal)) {
            const root = self.find_root_by_name(journal.root.?) orelse return error.RecoveryRequired;
            // A user-changed file at the staging name is theirs; the journal still finishes.
            self.remove_target_temp(root.fd, journal.relative_path.?, txid, v3_new_artifact_info(journal)) catch |err| {
                if (err != error.ArtifactConflict) return err;
            };
        }
        try self.remove_tx_tree(txid);
    }

    /// Check the staged manifests of a version-3 journal against their hashes and
    /// against the operation's delta rules. Every failure maps to RecoveryRequired
    /// so that evidence stays in place.
    fn validate_v3_manifests(self: *store_t, journal: journal_v3_t) !void {
        const txid = try txid_array(journal.transaction_id);
        const new_bytes = self.read_tx_file(txid, "new-manifest", manifest.MaxManifestBytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer self.allocator.free(new_bytes);
        const old_bytes = self.read_tx_file(txid, "old-manifest", manifest.MaxManifestBytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer self.allocator.free(old_bytes);
        if (!hash_text_equal(hash_bytes(new_bytes), journal.new_manifest_hash) or
            !hash_text_equal(hash_bytes(old_bytes), journal.old_manifest_hash)) return error.RecoveryRequired;
        var old = manifest.parse(self.allocator, old_bytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer old.deinit();
        var candidate = manifest.parse(self.allocator, new_bytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer candidate.deinit();
        validate_global_paths(old.records) catch return error.RecoveryRequired;
        validate_global_paths(candidate.records) catch return error.RecoveryRequired;
        const old_record = find_record(old.records, journal.record_id) orelse return error.RecoveryRequired;
        if (v3_is_recreate(journal)) {
            try self.validate_recreate_evidence(journal, old.records, candidate.records);
            return;
        }
        if (std.mem.eql(u8, journal.operation, "forget")) {
            validate_removal(old.records, candidate.records, journal.record_id) catch return error.RecoveryRequired;
            return;
        }
        if (!v3_is_remove(journal)) {
            validate_metadata_only(old.records, candidate.records) catch return error.RecoveryRequired;
            return;
        }
        validate_removal(old.records, candidate.records, journal.record_id) catch return error.RecoveryRequired;
        const root = self.find_root_by_name(journal.root.?) orelse return error.RecoveryRequired;
        const target = self.resolve_for_record(old_record.*) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        const resolved = target orelse return error.RecoveryRequired;
        defer self.allocator.free(resolved.relative);
        if (resolved.root.kind != root.kind or !std.mem.eql(u8, resolved.relative, journal.relative_path.?) or
            !std.ascii.eqlIgnoreCase(old_record.sha256, journal.old_artifact_hash.?)) return error.RecoveryRequired;
    }

    /// Read manifest bytes with their exact identity from one opened file.
    fn read_manifest_snapshot(self: *store_t) !manifest_snapshot_t {
        const fd = io_openat3(self.dir_fd, "manifest.json", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o777 != 0o600 or st.st_mode & 0o7000 != 0) return error.AccessDenied;
        const bytes = try read_all(self.allocator, fd, manifest.MaxManifestBytes);
        return .{ .bytes = bytes, .info = .{ .hash = hash_bytes(bytes), .mode = 0o600, .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) } };
    }

    fn ensure_private_dir(self: *store_t, leaf: []const u8) !void {
        const name = try self.allocator.dupeZ(u8, leaf);
        defer self.allocator.free(name);
        if (io_mkdirat(self.dir_fd, name, 0o700) != 0 and errno_value() != c.EEXIST) return native_error();
        const fd = io_openat3(self.dir_fd, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFDIR or st.st_uid != c.getuid() or st.st_mode & 0o777 != 0o700) return error.AccessDenied;
        if (io_fsync(self.dir_fd) != 0) return native_error();
        try self.checkpoint();
    }

    fn recover_locked(self: *store_t) !void {
        const fd = try self.open_private_dir("transactions");
        defer _ = c.close(fd);
        const scan = dup_cloexec(fd);
        if (scan < 0) return native_error();
        var dir = std.fs.Dir{ .fd = scan };
        defer dir.close();
        var iter = dir.iterate();
        var count: usize = 0;
        while (try iter.next()) |entry| {
            if (entry.kind != .directory) return error.RecoveryRequired;
            count += 1;
            if (count > MaxJournals) return error.RecoveryRequired;
            if (cleanup_txid(entry.name)) |txid| {
                try self.remove_cleanup_dir(fd, txid);
                continue;
            }
            if (!valid_txid(entry.name)) return error.RecoveryRequired;
            const journal_bytes = self.read_tx_file(entry.name[0..32].*, "journal.json", MaxJournalBytes) catch |err| {
                if (err == error.FileNotFound) return error.RecoveryRequired;
                if (err == error.OutOfMemory) return error.OutOfMemory;
                return error.RecoveryRequired;
            };
            defer self.allocator.free(journal_bytes);
            if (try journal_version(self.allocator, journal_bytes) == 3) {
                try self.recover_v3(entry.name, journal_bytes);
                continue;
            }
            const parsed = parse_journal(self.allocator, journal_bytes) catch |err| return err;
            defer parsed.deinit();
            const journal = parsed.value;
            try validate_journal_header(journal, entry.name);
            const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
            const target = try self.resolve_from_journal(root, journal.relative_path);
            self.allocator.free(target);
            try self.validate_journal_manifests(journal, root);
            if (std.mem.eql(u8, journal.phase, "prepared")) {
                try self.rollback_prepared(journal);
            } else if (std.mem.eql(u8, journal.phase, "committed")) {
                try self.apply_committed(journal);
            } else return error.RecoveryRequired;
            const artifact_temp_info = if (journal.version == 2 and std.mem.eql(u8, journal.phase, "committed"))
                journal_old_artifact_info(journal)
            else
                journal_artifact_info(journal);
            try self.remove_target_temp(root.fd, journal.relative_path, entry.name[0..32].*, artifact_temp_info);
            if (std.mem.eql(u8, journal.phase, "committed")) {
                try self.remove_published_old_manifest_temp(entry.name[0..32].*, journal);
            } else {
                try self.remove_state_temp(entry.name[0..32].*, journal_manifest_info(journal));
            }
            try self.remove_tx_tree(entry.name[0..32].*);
        }
    }

    fn resolve_from_journal(self: *store_t, root: owned_root_t, path: []const u8) ![]u8 {
        validate_relative(path) catch return error.RecoveryRequired;
        const allowed = switch (root.kind) {
            .Data => first_component(path, "applications") or icon_path_allowed(path) or std.mem.startsWith(u8, path, "desktop-directories/"),
            .Config => first_component(path, "autostart") or std.mem.startsWith(u8, path, "menus/applications-merged/"),
            .Desktop => std.mem.indexOfScalar(u8, path, '/') == null,
        };
        if (!allowed) return error.RecoveryRequired;
        return self.allocator.dupe(u8, path);
    }

    fn find_root_by_name(self: *store_t, name: []const u8) ?owned_root_t {
        for (self.roots) |root| if (std.mem.eql(u8, root_name(root.kind), name)) return root;
        return null;
    }

    fn validate_journal_manifests(self: *store_t, journal: journal_t, root: owned_root_t) !void {
        if (!std.mem.eql(u8, journal.root, root_name(root.kind))) return error.RecoveryRequired;
        const txid = try txid_array(journal.transaction_id);
        const new_artifact = self.read_tx_file(txid, "new-artifact", MaxArtifactBytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer self.allocator.free(new_artifact);
        if (!hash_text_equal(hash_bytes(new_artifact), journal.new_artifact_hash)) return error.RecoveryRequired;
        const new_bytes = self.read_tx_file(txid, "new-manifest", manifest.MaxManifestBytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer self.allocator.free(new_bytes);
        if (!hash_text_equal(hash_bytes(new_bytes), journal.new_manifest_hash)) return error.RecoveryRequired;
        var candidate = manifest.parse(self.allocator, new_bytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer candidate.deinit();
        try validate_global_paths(candidate.records);
        const new_record = find_record(candidate.records, journal.record_id) orelse return error.RecoveryRequired;
        const target = self.resolve_allowed(new_record.*) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer self.allocator.free(target.relative);
        if (target.root.kind != root.kind or !std.mem.eql(u8, target.relative, journal.relative_path) or
            !std.ascii.eqlIgnoreCase(new_record.sha256, journal.new_artifact_hash)) return error.RecoveryRequired;

        const old_bytes = self.read_tx_file(txid, "old-manifest", manifest.MaxManifestBytes) catch |err| if (err == error.FileNotFound) null else return err;
        defer if (old_bytes) |bytes| self.allocator.free(bytes);
        if (journal.version == 2) {
            const old_artifact_bytes = self.read_tx_file(txid, "old-artifact", MaxArtifactBytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
            defer self.allocator.free(old_artifact_bytes);
            const old_artifact_digest = hash_bytes(old_artifact_bytes);
            if (journal.old_artifact_hash == null or journal.old_artifact_device == null or journal.old_artifact_inode == null or
                !hash_text_equal(old_artifact_digest, journal.old_artifact_hash.?) or
                (journal.old_artifact_mode != 0o644 and journal.old_artifact_mode != 0o755)) return error.RecoveryRequired;
        }
        if (journal.version == 2) try self.verify_backup_for_journal(journal);
        if (journal.old_manifest_hash) |old_hash| {
            const bytes = old_bytes orelse return error.RecoveryRequired;
            if (!hash_text_equal(hash_bytes(bytes), old_hash)) return error.RecoveryRequired;
            if (hash_text_equal(hash_bytes(bytes), journal.new_manifest_hash)) return error.RecoveryRequired;
            var old = manifest.parse(self.allocator, bytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
            defer old.deinit();
            try validate_global_paths(old.records);
            try validate_delta(old.records, candidate.records, journal.record_id);
            const old_record = find_record(old.records, journal.record_id);
            if (journal.version == 1) {
                if (old_record != null) return error.RecoveryRequired;
            } else {
                const record = old_record orelse return error.RecoveryRequired;
                if (!same_target_identity(record.*, new_record.*)) return error.RecoveryRequired;
                if (!std.ascii.eqlIgnoreCase(record.sha256, journal.old_artifact_hash.?) and journal.backup_hash == null)
                    return error.RecoveryRequired;
            }
        } else {
            if (old_bytes != null or candidate.records.len != 1) return error.RecoveryRequired;
        }
    }

    fn apply_prepared(self: *store_t, journal: journal_t) !void {
        if (journal.version == 2) return self.apply_prepared_replace(journal);
        const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
        const txid = try txid_array(journal.transaction_id);
        const candidate_manifest = try self.read_tx_file(txid, "new-manifest", manifest.MaxManifestBytes);
        defer self.allocator.free(candidate_manifest);
        if (!hash_text_equal(hash_bytes(candidate_manifest), journal.new_manifest_hash)) return error.RecoveryRequired;
        const new_artifact = try self.read_tx_file(txid, "new-artifact", MaxArtifactBytes);
        defer self.allocator.free(new_artifact);
        if (!hash_text_equal(hash_bytes(new_artifact), journal.new_artifact_hash)) return error.RecoveryRequired;
        const artifact_info = journal_artifact_info(journal);
        const manifest_info = journal_manifest_info(journal);
        var parsed_candidate = manifest.parse(self.allocator, candidate_manifest) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer parsed_candidate.deinit();
        const intended = find_record(parsed_candidate.records, journal.record_id) orelse return error.RecoveryRequired;
        const intended_target = self.resolve_allowed(intended.*) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
        defer self.allocator.free(intended_target.relative);
        if (intended_target.root.kind != root.kind or !std.mem.eql(u8, intended_target.relative, journal.relative_path) or
            !std.ascii.eqlIgnoreCase(intended.sha256, journal.new_artifact_hash)) return error.RecoveryRequired;
        const current = try self.inspect(root.fd, journal.relative_path);
        const artifact_published = if (current) |found| blk: {
            if (!same_file_info(found, artifact_info)) return error.ArtifactConflict;
            break :blk true;
        } else false;
        const current_manifest = try self.inspect_manifest();
        const manifest_published = if (current_manifest) |found| blk: {
            if (same_file_info(found, manifest_info)) break :blk true;
            const old_hash = journal.old_manifest_hash orelse return error.ArtifactConflict;
            if (!hash_text_equal(found.hash, old_hash)) return error.ArtifactConflict;
            if (journal_old_manifest_info(journal)) |expected| if (!same_file_info(found, expected)) return error.ArtifactConflict;
            if (hash_text_equal(found.hash, journal.new_manifest_hash)) return error.RecoveryRequired;
            break :blk false;
        } else blk: {
            if (journal.old_manifest_hash != null) return error.ArtifactConflict;
            break :blk false;
        };
        const parent = try self.open_parent(root.fd, journal.relative_path);
        defer _ = c.close(parent.fd);
        const artifact_temp = artifact_temp_name(txid);
        try self.validate_optional_temp(parent.fd, artifact_temp[0..].ptr, artifact_info, !artifact_published, artifact_published);
        const manifest_temp = manifest_temp_name(txid);
        if (manifest_published and journal.old_manifest_hash != null) {
            try self.validate_old_manifest_temp(journal, manifest_temp[0..].ptr);
        } else {
            try self.validate_optional_temp(self.dir_fd, manifest_temp[0..].ptr, manifest_info, !manifest_published, manifest_published);
        }
        if (!artifact_published) try self.publish_file(root.fd, journal.relative_path, txid, artifact_info);
        if (!manifest_published) try self.publish_manifest(txid, manifest_info, journal.old_manifest_hash, journal_old_manifest_info(journal));
    }

    fn apply_prepared_replace(self: *store_t, journal: journal_t) !void {
        const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
        const txid = try txid_array(journal.transaction_id);
        const old_info = journal_old_artifact_info(journal);
        const new_info = journal_artifact_info(journal);
        const manifest_info = journal_manifest_info(journal);
        const current = (try self.inspect(root.fd, journal.relative_path)) orelse return error.ArtifactConflict;
        const parent = try self.open_parent(root.fd, journal.relative_path);
        defer _ = c.close(parent.fd);
        const temp = artifact_temp_name(txid);
        const temporary = try self.inspect_named(parent.fd, temp[0..].ptr);
        const target_is_old = same_file_info(current, old_info);
        const target_is_new = same_file_info(current, new_info);
        const temp_is_new = if (temporary) |info| same_file_info(info, new_info) else false;
        const temp_is_old = if (temporary) |info| same_file_info(info, old_info) else false;
        if (!((target_is_old and temp_is_new) or (target_is_new and temp_is_old))) return error.ArtifactConflict;

        const current_manifest = try self.inspect_manifest();
        const manifest_published = if (current_manifest) |found| blk: {
            if (same_file_info(found, manifest_info)) break :blk true;
            const old_hash = journal.old_manifest_hash orelse return error.ArtifactConflict;
            if (!hash_text_equal(found.hash, old_hash)) return error.ArtifactConflict;
            if (journal_old_manifest_info(journal)) |expected| if (!same_file_info(found, expected)) return error.ArtifactConflict;
            break :blk false;
        } else return error.ArtifactConflict;
        const manifest_temp = manifest_temp_name(txid);
        if (manifest_published) {
            try self.validate_old_manifest_temp(journal, manifest_temp[0..].ptr);
        } else {
            try self.validate_optional_temp(self.dir_fd, manifest_temp[0..].ptr, manifest_info, true, false);
        }
        if (target_is_old) try self.exchange_artifact(root.fd, journal.relative_path, txid, new_info, old_info);
        if (!manifest_published) try self.publish_manifest(txid, manifest_info, journal.old_manifest_hash, journal_old_manifest_info(journal));
    }

    fn rollback_prepared(self: *store_t, journal: journal_t) !void {
        if (journal.version == 2) return self.rollback_prepared_replace(journal);
        const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
        const txid = try txid_array(journal.transaction_id);
        if (journal.old_artifact_hash != null or journal.old_artifact_mode != 0) return error.RecoveryRequired;
        const artifact_info = journal_artifact_info(journal);
        const manifest_info = journal_manifest_info(journal);
        const current = try self.inspect(root.fd, journal.relative_path);
        const old_manifest = self.read_tx_file(txid, "old-manifest", manifest.MaxManifestBytes) catch |err| if (err == error.FileNotFound) null else return err;
        defer if (old_manifest) |bytes| self.allocator.free(bytes);
        const artifact_published = if (current) |found| blk: {
            if (!same_file_info(found, artifact_info)) return error.ArtifactConflict;
            break :blk true;
        } else false;
        const current_manifest = try self.inspect_manifest();
        const manifest_published = if (current_manifest) |found| blk: {
            if (same_file_info(found, manifest_info)) break :blk true;
            if (journal.old_manifest_hash) |old_hash| {
                if (hash_text_equal(found.hash, old_hash)) {
                    if (journal_old_manifest_info(journal)) |expected| if (!same_file_info(found, expected)) return error.ArtifactConflict;
                    break :blk false;
                }
            }
            return error.ArtifactConflict;
        } else blk: {
            if (journal.old_manifest_hash != null) return error.ArtifactConflict;
            break :blk false;
        };
        const parent = try self.open_parent(root.fd, journal.relative_path);
        defer _ = c.close(parent.fd);
        const artifact_temp = artifact_temp_name(txid);
        try self.validate_optional_temp(parent.fd, artifact_temp[0..].ptr, artifact_info, false, artifact_published);
        const manifest_temp = manifest_temp_name(txid);
        if (manifest_published and journal.old_manifest_hash != null) {
            try self.validate_old_manifest_temp(journal, manifest_temp[0..].ptr);
        } else {
            try self.validate_optional_temp(self.dir_fd, manifest_temp[0..].ptr, manifest_info, false, manifest_published);
        }
        if (journal.old_manifest_hash) |old_hash| {
            const previous = old_manifest orelse return error.RecoveryRequired;
            if (!hash_text_equal(hash_bytes(previous), old_hash)) return error.RecoveryRequired;
        }
        var restored_manifest: ?file_info_t = null;
        if (manifest_published) {
            if (old_manifest) |previous| restored_manifest = try self.create_or_reuse_old_manifest_temp(txid, journal, previous);
        }
        if (artifact_published) _ = try self.unlink_owned_artifact(root.fd, journal.relative_path, artifact_info, try txid_array(journal.transaction_id));
        if (manifest_published) {
            if (old_manifest != null) {
                const restored = restored_manifest orelse return error.RecoveryRequired;
                try self.publish_manifest(txid, restored, journal.new_manifest_hash, journal_manifest_info(journal));
            } else try self.unlink_owned_manifest(manifest_info);
        }
    }

    fn rollback_prepared_replace(self: *store_t, journal: journal_t) !void {
        const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
        const txid = try txid_array(journal.transaction_id);
        const old_info = journal_old_artifact_info(journal);
        const new_info = journal_artifact_info(journal);
        const manifest_info = journal_manifest_info(journal);
        const current = (try self.inspect(root.fd, journal.relative_path)) orelse return error.ArtifactConflict;
        const old_manifest = try self.read_tx_file(txid, "old-manifest", manifest.MaxManifestBytes);
        defer self.allocator.free(old_manifest);
        if (!hash_text_equal(hash_bytes(old_manifest), journal.old_manifest_hash.?)) return error.RecoveryRequired;
        const parent = try self.open_parent(root.fd, journal.relative_path);
        defer _ = c.close(parent.fd);
        const temp = artifact_temp_name(txid);
        const entry = try self.inspect_named(parent.fd, temp[0..].ptr);
        const target_is_old = same_file_info(current, old_info);
        const target_is_new = same_file_info(current, new_info);
        const temp_is_new = if (entry) |info| same_file_info(info, new_info) else false;
        const temp_is_old = if (entry) |info| same_file_info(info, old_info) else false;
        if (!((target_is_old and (temp_is_new or entry == null)) or (target_is_new and temp_is_old))) return error.ArtifactConflict;
        const current_manifest = (try self.inspect_manifest()) orelse return error.ArtifactConflict;
        const manifest_published = if (same_file_info(current_manifest, manifest_info)) true else blk: {
            if (!hash_text_equal(current_manifest.hash, journal.old_manifest_hash.?)) return error.ArtifactConflict;
            if (journal_old_manifest_info(journal)) |expected| if (!same_file_info(current_manifest, expected)) return error.ArtifactConflict;
            break :blk false;
        };
        const manifest_temp = manifest_temp_name(txid);
        if (manifest_published) {
            try self.validate_old_manifest_temp(journal, manifest_temp[0..].ptr);
        } else {
            const temp_info = try self.inspect_named(self.dir_fd, manifest_temp[0..].ptr);
            if (temp_info) |found| {
                if (!same_file_info(found, manifest_info)) return error.ArtifactConflict;
            }
        }
        const restored_manifest = if (manifest_published)
            try self.create_or_reuse_old_manifest_temp(txid, journal, old_manifest)
        else
            null;
        if (target_is_new) try self.exchange_artifact(root.fd, journal.relative_path, txid, old_info, new_info);
        if (manifest_published) {
            const restored = restored_manifest orelse return error.RecoveryRequired;
            try self.publish_manifest(txid, restored, journal.new_manifest_hash, manifest_info);
        }
    }

    fn apply_committed(self: *store_t, journal: journal_t) !void {
        if (journal.version == 2) return self.apply_committed_replace(journal);
        const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
        const current = (try self.inspect(root.fd, journal.relative_path)) orelse return error.RecoveryRequired;
        if (!same_file_info(current, journal_artifact_info(journal))) return error.RecoveryRequired;
        const current_manifest = (try self.inspect_manifest()) orelse return error.RecoveryRequired;
        if (!same_file_info(current_manifest, journal_manifest_info(journal))) return error.RecoveryRequired;
        const manifest_temp = manifest_temp_name(try txid_array(journal.transaction_id));
        if (journal.old_manifest_hash != null) try self.validate_old_manifest_temp(journal, manifest_temp[0..].ptr);

        // A committed phase may be visible after a crash even when the prior
        // publish_phase directory fsync failed. Re-establish file and namespace
        // durability before transaction evidence can be retired.
        const txid = try txid_array(journal.transaction_id);
        const transactions_fd = try self.open_private_dir("transactions");
        defer _ = c.close(transactions_fd);
        var tx_name: [33]u8 = undefined;
        @memcpy(tx_name[0..32], &txid);
        tx_name[32] = 0;
        const tx_fd = io_openat3(transactions_fd, &tx_name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (tx_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(tx_fd);
        const journal_fd = io_openat3(tx_fd, "journal.json", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (journal_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(journal_fd);
        var journal_stat: c.struct_stat = undefined;
        if (io_fstat(journal_fd, &journal_stat) != 0 or journal_stat.st_mode & c.S_IFMT != c.S_IFREG or
            journal_stat.st_uid != c.getuid() or journal_stat.st_nlink != 1 or journal_stat.st_mode & 0o777 != 0o600)
            return error.RecoveryRequired;
        if (io_fsync(journal_fd) != 0 or io_fsync(tx_fd) != 0) return native_error();
        const parent = try self.open_parent(root.fd, journal.relative_path);
        defer _ = c.close(parent.fd);
        if (io_fsync(parent.fd) != 0 or io_fsync(self.dir_fd) != 0 or io_fsync(root.fd) != 0) return native_error();
    }

    fn apply_committed_replace(self: *store_t, journal: journal_t) !void {
        const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
        const current = (try self.inspect(root.fd, journal.relative_path)) orelse return error.RecoveryRequired;
        if (!same_file_info(current, journal_artifact_info(journal))) return error.RecoveryRequired;
        const parent = try self.open_parent(root.fd, journal.relative_path);
        defer _ = c.close(parent.fd);
        const txid = try txid_array(journal.transaction_id);
        const temp = artifact_temp_name(txid);
        if (try self.inspect_named(parent.fd, temp[0..].ptr)) |displaced| {
            if (!same_file_info(displaced, journal_old_artifact_info(journal))) return error.RecoveryRequired;
        }
        const current_manifest = (try self.inspect_manifest()) orelse return error.RecoveryRequired;
        if (!same_file_info(current_manifest, journal_manifest_info(journal))) return error.RecoveryRequired;
        const manifest_temp = manifest_temp_name(txid);
        try self.validate_old_manifest_temp(journal, manifest_temp[0..].ptr);
        try self.verify_backup_for_journal(journal);
        const transactions_fd = try self.open_private_dir("transactions");
        defer _ = c.close(transactions_fd);
        const tx_name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(tx_name);
        const tx_fd = io_openat3(transactions_fd, tx_name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (tx_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(tx_fd);
        const journal_fd = io_openat3(tx_fd, "journal.json", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (journal_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(journal_fd);
        if (io_fsync(journal_fd) != 0 or io_fsync(tx_fd) != 0 or io_fsync(parent.fd) != 0 or
            io_fsync(self.dir_fd) != 0 or io_fsync(root.fd) != 0) return native_error();
    }

    fn open_private_dir(self: *store_t, name: []const u8) !c_int {
        if (name.len >= 256 or std.mem.indexOfScalar(u8, name, 0) != null) return error.InvalidArgument;
        var zname: [256]u8 = undefined;
        @memcpy(zname[0..name.len], name);
        zname[name.len] = 0;
        const fd = io_openat3(self.dir_fd, &zname, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (fd < 0) return native_error();
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFDIR or
            st.st_uid != c.getuid() or st.st_mode & 0o777 != 0o700)
        {
            _ = c.close(fd);
            return error.AccessDenied;
        }
        return fd;
    }

    fn read_target_bytes(self: *store_t, root_fd: c_int, relative: []const u8) ![]u8 {
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        const leaf = try self.allocator.dupeZ(u8, parent.leaf);
        defer self.allocator.free(leaf);
        const fd = io_openat3(parent.fd, leaf, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o7000 != 0)
            return error.AccessDenied;
        return read_all(self.allocator, fd, MaxArtifactBytes);
    }

    fn create_backup(self: *store_t, txid: [32]u8, record_id: []const u8, bytes: []const u8, artifact_mode: u9) !file_info_t {
        try self.ensure_private_dir("backups");
        const backups_fd = try self.open_private_dir("backups");
        defer _ = c.close(backups_fd);
        try self.validate_backup_capacity(backups_fd);
        const tx_name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(tx_name);
        const pending_name = try std.fmt.allocPrintZ(self.allocator, ".pending-{s}", .{txid});
        defer self.allocator.free(pending_name);
        if (io_mkdirat(backups_fd, pending_name, 0o700) != 0) return native_error();
        if (io_fsync(backups_fd) != 0) return native_error();
        const backup_fd = io_openat3(backups_fd, pending_name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (backup_fd < 0) return native_error();
        defer _ = c.close(backup_fd);
        if (io_fchmod(backup_fd, 0o700) != 0) return native_error();
        var backup_dir_stat: c.struct_stat = undefined;
        if (io_fstat(backup_fd, &backup_dir_stat) != 0 or backup_dir_stat.st_mode & c.S_IFMT != c.S_IFDIR or
            backup_dir_stat.st_uid != c.getuid() or backup_dir_stat.st_mode & 0o777 != 0o700) return error.AccessDenied;
        const record_hash = hash_hex(record_id);
        const leaf = try self.allocator.dupeZ(u8, &record_hash);
        defer self.allocator.free(leaf);
        const fd = io_openat4(backup_fd, leaf, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, 0o600);
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o7000 != 0)
            return error.AccessDenied;
        try write_all(fd, bytes);
        if (io_fchmod(fd, 0o600) != 0 or io_fsync(fd) != 0 or io_fsync(backup_fd) != 0)
            return native_error();
        if (io_fstat(fd, &st) != 0) return native_error();
        const info = file_info_t{ .hash = hash_bytes(bytes), .mode = artifact_mode, .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) };
        const metadata = encode_backup_metadata(txid, record_id, info);
        const metadata_fd = io_openat4(backup_fd, "metadata.bin", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, 0o600);
        if (metadata_fd < 0) return native_error();
        defer _ = c.close(metadata_fd);
        try write_all(metadata_fd, &metadata);
        if (io_fchmod(metadata_fd, 0o600) != 0 or io_fsync(metadata_fd) != 0 or io_fsync(backup_fd) != 0)
            return native_error();
        try self.checkpoint();
        if (io_renameat2(backups_fd, pending_name, backups_fd, tx_name, c.RENAME_NOREPLACE) != 0) return native_error();
        if (io_fsync(backups_fd) != 0) return native_error();
        return info;
    }

    fn validate_backup_capacity(self: *store_t, backups_fd: c_int) !void {
        const scan_fd = dup_cloexec(backups_fd);
        if (scan_fd < 0) return native_error();
        var dir = std.fs.Dir{ .fd = scan_fd };
        defer dir.close();
        var iter = dir.iterate();
        var count: usize = 0;
        while (try iter.next()) |entry| {
            count += 1;
            if (count > MaxBackups or entry.kind != .directory or !valid_backup_txid(entry.name)) return error.RecoveryRequired;
            const txid = try txid_array(entry.name);
            _ = try self.inspect_backup_entry(backups_fd, txid);
        }
        if (count >= MaxBackups) return error.RecoveryRequired;
    }

    fn inspect_backup_entry(self: *store_t, backups_fd: c_int, txid: [32]u8) !backup_ref_t {
        if (!valid_backup_txid(&txid)) return error.RecoveryRequired;
        const tx_name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(tx_name);
        const backup_dir = io_openat3(backups_fd, tx_name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (backup_dir < 0) return error.RecoveryRequired;
        defer _ = c.close(backup_dir);
        var dir_stat: c.struct_stat = undefined;
        if (io_fstat(backup_dir, &dir_stat) != 0 or dir_stat.st_mode & c.S_IFMT != c.S_IFDIR or
            dir_stat.st_uid != c.getuid() or dir_stat.st_nlink != 2 or dir_stat.st_mode & 0o777 != 0o700) return error.RecoveryRequired;
        const meta_fd = io_openat3(backup_dir, "metadata.bin", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (meta_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(meta_fd);
        var meta_stat: c.struct_stat = undefined;
        if (io_fstat(meta_fd, &meta_stat) != 0 or meta_stat.st_mode & c.S_IFMT != c.S_IFREG or
            meta_stat.st_uid != c.getuid() or meta_stat.st_nlink != 1 or meta_stat.st_mode & 0o777 != 0o600 or
            meta_stat.st_size != BackupMetadataBytes) return error.RecoveryRequired;
        const meta_bytes = try read_all(self.allocator, meta_fd, BackupMetadataBytes);
        defer self.allocator.free(meta_bytes);
        const reference = try decode_backup_metadata(txid, meta_bytes);
        const leaf = hash_hex_from_array(reference.record_id_sha256);
        var leaf_z: [65]u8 = undefined;
        @memcpy(leaf_z[0..64], &leaf);
        leaf_z[64] = 0;
        const payload_fd = io_openat3(backup_dir, &leaf_z, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (payload_fd < 0) return error.RecoveryRequired;
        defer _ = c.close(payload_fd);
        var payload_stat: c.struct_stat = undefined;
        if (io_fstat(payload_fd, &payload_stat) != 0 or payload_stat.st_mode & c.S_IFMT != c.S_IFREG or
            payload_stat.st_uid != c.getuid() or payload_stat.st_nlink != 1 or payload_stat.st_mode & 0o777 != 0o600 or
            @as(u64, @intCast(payload_stat.st_dev)) != reference.device or
            @as(u64, @intCast(payload_stat.st_ino)) != reference.inode) return error.RecoveryRequired;
        const bytes = try read_all(self.allocator, payload_fd, MaxArtifactBytes);
        defer self.allocator.free(bytes);
        if (!std.mem.eql(u8, &hash_bytes(bytes), &reference.sha256)) return error.RecoveryRequired;
        const scan_fd = dup_cloexec(backup_dir);
        if (scan_fd < 0) return native_error();
        var child = std.fs.Dir{ .fd = scan_fd };
        defer child.close();
        var iter = child.iterate();
        var saw_meta = false;
        var saw_payload = false;
        var entries: usize = 0;
        while (try iter.next()) |entry| {
            entries += 1;
            if (entries > 2 or entry.kind != .file) return error.RecoveryRequired;
            if (std.mem.eql(u8, entry.name, "metadata.bin")) saw_meta = true else if (std.mem.eql(u8, entry.name, &leaf)) saw_payload = true else return error.RecoveryRequired;
        }
        if (!saw_meta or !saw_payload or entries != 2) return error.RecoveryRequired;
        return reference;
    }

    fn read_backup(self: *store_t, record_id: []const u8, reference: backup_ref_t) ![]u8 {
        if (!valid_record_id(record_id) or !valid_backup_txid(&reference.transaction_id) or
            (reference.artifact_mode != 0o644 and reference.artifact_mode != 0o755)) return error.InvalidArgument;
        const record_hash = hash_hex(record_id);
        if (!std.mem.eql(u8, &record_hash, &hash_hex_from_array(reference.record_id_sha256))) return error.ArtifactConflict;
        const backups_fd = try self.open_private_dir("backups");
        defer _ = c.close(backups_fd);
        const actual = self.inspect_backup_entry(backups_fd, reference.transaction_id) catch |err| {
            if (err == error.OutOfMemory) return err;
            return error.ArtifactConflict;
        };
        if (!same_backup_ref(actual, reference)) return error.ArtifactConflict;
        const tx_name = try self.allocator.dupeZ(u8, &reference.transaction_id);
        defer self.allocator.free(tx_name);
        const backup_dir = io_openat3(backups_fd, tx_name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (backup_dir < 0) return error.ArtifactConflict;
        defer _ = c.close(backup_dir);
        var backup_dir_stat: c.struct_stat = undefined;
        if (io_fstat(backup_dir, &backup_dir_stat) != 0 or backup_dir_stat.st_mode & c.S_IFMT != c.S_IFDIR or
            backup_dir_stat.st_uid != c.getuid() or backup_dir_stat.st_mode & 0o777 != 0o700) return error.AccessDenied;
        const leaf = try self.allocator.dupeZ(u8, &record_hash);
        defer self.allocator.free(leaf);
        const fd = io_openat3(backup_dir, leaf, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) return error.ArtifactConflict;
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or
            st.st_nlink != 1 or st.st_mode & 0o777 != 0o600 or
            @as(u64, @intCast(st.st_dev)) != reference.device or @as(u64, @intCast(st.st_ino)) != reference.inode)
            return error.ArtifactConflict;
        const bytes = try read_all(self.allocator, fd, MaxArtifactBytes);
        if (!std.mem.eql(u8, &hash_bytes(bytes), &reference.sha256)) {
            self.allocator.free(bytes);
            return error.ArtifactConflict;
        }
        return bytes;
    }

    fn verify_backup_for_journal(self: *store_t, journal: journal_t) !void {
        if (journal.backup_hash == null) {
            if (journal.backup_mode != 0 or journal.backup_device != null or journal.backup_inode != null) return error.RecoveryRequired;
            return;
        }
        if (journal.version != 2 or journal.backup_device == null or journal.backup_inode == null or
            (journal.backup_mode != 0o644 and journal.backup_mode != 0o755)) return error.RecoveryRequired;
        const txid = try txid_array(journal.transaction_id);
        const record_hash_text = hash_hex(journal.record_id);
        const reference = backup_ref_t{
            .transaction_id = txid,
            .record_id_sha256 = hash_from_text(&record_hash_text),
            .sha256 = hash_from_text(journal.backup_hash.?),
            .artifact_mode = journal.backup_mode,
            .device = journal.backup_device.?,
            .inode = journal.backup_inode.?,
        };
        const backup = try self.read_backup(journal.record_id, reference);
        defer self.allocator.free(backup);
        if (journal.old_artifact_hash == null or !hash_text_equal(hash_bytes(backup), journal.old_artifact_hash.?))
            return error.RecoveryRequired;
    }

    fn resolve_allowed(self: *store_t, record: manifest.record_t) !resolved_t {
        const kind: root_kind_t = switch (record.kind) {
            .DesktopEntry, .HandlerEntry, .Icon => .Data,
            .Menu => if (self.path_under(.Config, record.path)) .Config else .Data,
            .Autostart => .Config,
            .DesktopShortcut => .Desktop,
            else => return error.UnsupportedArtifactKind,
        };
        const root = self.find_root(kind) orelse return error.InvalidScope;
        if (!std.mem.startsWith(u8, record.path, root.canonical_path) or record.path.len <= root.canonical_path.len or record.path[root.canonical_path.len] != '/') return error.InvalidPath;
        const relative = try self.allocator.dupe(u8, record.path[root.canonical_path.len + 1 ..]);
        errdefer self.allocator.free(relative);
        try validate_relative(relative);
        const allowed = switch (record.kind) {
            .DesktopEntry, .HandlerEntry => first_component(relative, "applications"),
            .Icon => icon_path_allowed(relative),
            .Autostart => first_component(relative, "autostart"),
            .DesktopShortcut => std.mem.indexOfScalar(u8, relative, '/') == null,
            .Menu => if (kind == .Config) std.mem.startsWith(u8, relative, "menus/applications-merged/") else std.mem.startsWith(u8, relative, "desktop-directories/"),
            else => false,
        };
        if (!allowed) return error.InvalidPath;
        return .{ .root = root, .relative = relative };
    }

    fn find_root(self: *store_t, kind: root_kind_t) ?owned_root_t {
        for (self.roots) |root| if (root.kind == kind) return root;
        return null;
    }

    fn path_under(self: *store_t, kind: root_kind_t, path: []const u8) bool {
        const root = self.find_root(kind) orelse return false;
        return std.mem.startsWith(u8, path, root.canonical_path) and path.len > root.canonical_path.len and path[root.canonical_path.len] == '/';
    }

    fn inspect(self: *store_t, root_fd: c_int, relative: []const u8) !?file_info_t {
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        const leaf = try self.allocator.dupeZ(u8, parent.leaf);
        defer self.allocator.free(leaf);
        const fd = io_openat3(parent.fd, leaf, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) {
            if (errno_value() == c.ENOENT) return null;
            // A socket, FIFO-less device node, or other special object cannot be opened as a regular file.
            if (errno_value() == c.ENXIO) return error.AccessDenied;
            return native_error();
        }
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0) return native_error();
        if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o7000 != 0) return error.AccessDenied;
        const bytes = try read_all(self.allocator, fd, MaxArtifactBytes);
        defer self.allocator.free(bytes);
        return .{ .hash = hash_bytes(bytes), .mode = @intCast(st.st_mode & 0o777), .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) };
    }

    fn inspect_named(self: *store_t, parent_fd: c_int, name: [*c]const u8) !?file_info_t {
        const fd = io_openat3(parent_fd, name, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) {
            if (errno_value() == c.ENOENT) return null;
            return native_error();
        }
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o7000 != 0)
            return error.AccessDenied;
        const bytes = try read_all(self.allocator, fd, MaxArtifactBytes);
        defer self.allocator.free(bytes);
        return .{ .hash = hash_bytes(bytes), .mode = @intCast(st.st_mode & 0o777), .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) };
    }

    fn exchange_artifact(self: *store_t, root_fd: c_int, relative: []const u8, txid: [32]u8, temp_expected: file_info_t, target_expected: file_info_t) !void {
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        const temp = artifact_temp_name(txid);
        try self.verify_named_file(parent.fd, temp[0..].ptr, temp_expected);
        const target = try self.inspect(root_fd, relative);
        if (target == null or !same_file_info(target.?, target_expected)) return error.ArtifactConflict;
        const leaf = try self.allocator.dupeZ(u8, parent.leaf);
        defer self.allocator.free(leaf);
        if (io_renameat2(parent.fd, temp[0..].ptr, parent.fd, leaf, c.RENAME_EXCHANGE) != 0) {
            if (errno_value() == c.ENOSYS or errno_value() == c.EINVAL or errno_value() == c.EOPNOTSUPP)
                return error.UnsupportedAtomicReplace;
            return native_error();
        }
        if (io_fsync(parent.fd) != 0) return native_error();
        try self.checkpoint();
        try self.verify_named_file(parent.fd, temp[0..].ptr, target_expected);
        const installed = (try self.inspect(root_fd, relative)) orelse return error.ArtifactConflict;
        if (!same_file_info(installed, temp_expected)) return error.ArtifactConflict;
    }

    fn open_parent(self: *store_t, root_fd: c_int, relative: []const u8) !parent_t {
        try validate_relative(relative);
        var parts = std.mem.splitScalar(u8, relative, '/');
        var current = dup_cloexec(root_fd);
        if (current < 0) return native_error();
        errdefer _ = c.close(current);
        var leaf: ?[]const u8 = null;
        while (parts.next()) |part| {
            if (leaf) |previous| {
                const name = try self.allocator.dupeZ(u8, previous);
                defer self.allocator.free(name);
                const next = io_openat3(current, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
                if (next < 0) return native_error();
                _ = c.close(current);
                current = next;
            }
            leaf = part;
        }
        return .{ .fd = current, .leaf = leaf orelse unreachable };
    }

    fn create_tx_dir(self: *store_t, txid: [32]u8) !void {
        const parent = try self.open_private_dir("transactions");
        defer _ = c.close(parent);
        const name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(name);
        if (io_mkdirat(parent, name, 0o700) != 0 or io_fsync(parent) != 0) return native_error();
        try self.checkpoint();
    }

    fn write_tx_file(self: *store_t, txid: [32]u8, leaf: []const u8, bytes: []const u8, mode: u9) !void {
        const parent = try self.open_private_dir("transactions");
        defer _ = c.close(parent);
        const name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(name);
        const tx = io_openat3(parent, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (tx < 0) return native_error();
        defer _ = c.close(tx);
        const leafz = try self.allocator.dupeZ(u8, leaf);
        defer self.allocator.free(leafz);
        const fd = io_openat4(tx, leafz, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, mode));
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        try write_all(fd, bytes);
        if (io_fchmod(fd, @as(c_uint, mode)) != 0 or io_fsync(fd) != 0 or io_fsync(tx) != 0) return native_error();
        try self.checkpoint();
    }

    fn read_tx_file(self: *store_t, txid: [32]u8, leaf: []const u8, limit: usize) ![]u8 {
        const parent = try self.open_private_dir("transactions");
        defer _ = c.close(parent);
        const name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(name);
        const tx = io_openat3(parent, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (tx < 0) return native_error();
        defer _ = c.close(tx);
        const leafz = try self.allocator.dupeZ(u8, leaf);
        defer self.allocator.free(leafz);
        const fd = io_openat3(tx, leafz, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o022 != 0 or st.st_mode & 0o7000 != 0) return error.AccessDenied;
        const mode: u32 = @intCast(st.st_mode & 0o777);
        if ((std.mem.eql(u8, leaf, "journal.json") or std.mem.eql(u8, leaf, "journal.next") or
            std.mem.eql(u8, leaf, "new-manifest") or std.mem.eql(u8, leaf, "old-manifest")) and mode != 0o600) return error.AccessDenied;
        if ((std.mem.eql(u8, leaf, "new-artifact") or std.mem.eql(u8, leaf, "old-artifact")) and mode != 0o644 and mode != 0o755) return error.AccessDenied;
        return read_all(self.allocator, fd, limit);
    }

    fn write_journal(self: *store_t, txid: [32]u8, journal: anytype) !void {
        const bytes = try std.json.stringifyAlloc(self.allocator, journal, .{});
        defer self.allocator.free(bytes);
        if (bytes.len > MaxJournalBytes) return error.JournalTooLarge;
        try self.write_tx_file(txid, "journal.json", bytes, 0o600);
        const parent = try self.open_private_dir("transactions");
        defer _ = c.close(parent);
        if (io_fsync(parent) != 0) return native_error();
    }

    fn publish_phase(self: *store_t, txid: [32]u8, journal: anytype, phase: []const u8) !void {
        var updated = journal;
        updated.phase = phase;
        const bytes = try std.json.stringifyAlloc(self.allocator, updated, .{});
        defer self.allocator.free(bytes);
        if (bytes.len > MaxJournalBytes) return error.JournalTooLarge;
        const parent = try self.open_private_dir("transactions");
        defer _ = c.close(parent);
        const name = try self.allocator.dupeZ(u8, &txid);
        defer self.allocator.free(name);
        const tx = io_openat3(parent, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (tx < 0) return native_error();
        defer _ = c.close(tx);
        const fd = io_openat4(tx, "journal.next", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        try write_all(fd, bytes);
        if (io_fsync(fd) != 0 or io_renameat(tx, "journal.next", tx, "journal.json") != 0 or io_fsync(tx) != 0) return native_error();
        try self.checkpoint();
    }

    fn read_manifest(self: *store_t) ![]u8 {
        const snapshot = try self.read_manifest_snapshot();
        return snapshot.bytes;
    }

    fn create_target_temp(self: *store_t, root_fd: c_int, relative: []const u8, bytes: []const u8, mode: u9, txid: [32]u8) !file_info_t {
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        const temp = artifact_temp_name(txid);
        const fd = io_openat4(parent.fd, &temp, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, mode));
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0) return native_error();
        var info = file_info_t{ .hash = hash_bytes(bytes), .mode = @intCast(st.st_mode & 0o777), .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) };
        errdefer unlink_named_if_matches(self.allocator, parent.fd, temp[0..].ptr, info) catch {};
        try write_all(fd, bytes);
        if (io_fchmod(fd, @as(c_uint, mode)) != 0 or io_fsync(fd) != 0) return native_error();
        if (io_fstat(fd, &st) != 0) return native_error();
        info.mode = @intCast(st.st_mode & 0o777);
        if (io_fsync(parent.fd) != 0) return native_error();
        try self.checkpoint();
        return info;
    }

    fn create_state_temp(self: *store_t, bytes: []const u8, mode: u9, txid: [32]u8) !file_info_t {
        const temp = manifest_temp_name(txid);
        const fd = io_openat4(self.dir_fd, &temp, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, mode));
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0) return native_error();
        var info = file_info_t{ .hash = hash_bytes(bytes), .mode = @intCast(st.st_mode & 0o777), .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) };
        errdefer unlink_named_if_matches(self.allocator, self.dir_fd, temp[0..].ptr, info) catch {};
        try write_all(fd, bytes);
        if (io_fchmod(fd, @as(c_uint, mode)) != 0 or io_fsync(fd) != 0) return native_error();
        if (io_fstat(fd, &st) != 0) return native_error();
        info.mode = @intCast(st.st_mode & 0o777);
        if (io_fsync(self.dir_fd) != 0) return native_error();
        try self.checkpoint();
        return info;
    }

    fn remove_target_temp(self: *store_t, root_fd: c_int, relative: []const u8, txid: [32]u8, expected: file_info_t) !void {
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        const temp = artifact_temp_name(txid);
        try unlink_named_if_matches(self.allocator, parent.fd, temp[0..].ptr, expected);
        if (io_fsync(parent.fd) != 0) return native_error();
        try self.checkpoint();
    }

    fn remove_state_temp(self: *store_t, txid: [32]u8, expected: file_info_t) !void {
        const temp = manifest_temp_name(txid);
        try unlink_named_if_matches(self.allocator, self.dir_fd, temp[0..].ptr, expected);
        if (io_fsync(self.dir_fd) != 0) return native_error();
        try self.checkpoint();
    }

    fn remove_published_old_manifest_temp(self: *store_t, txid: [32]u8, journal: journal_t) !void {
        if (journal.old_manifest_hash == null) return self.remove_state_temp(txid, journal_manifest_info(journal));
        const temp = manifest_temp_name(txid);
        const found = try self.inspect_named(self.dir_fd, temp[0..].ptr) orelse return;
        if (found.mode != 0o600 or !hash_text_equal(found.hash, journal.old_manifest_hash.?)) return error.ArtifactConflict;
        if (journal_old_manifest_info(journal)) |expected| {
            if (!same_file_info(found, expected)) return error.ArtifactConflict;
        }
        try unlink_named_if_matches(self.allocator, self.dir_fd, temp[0..].ptr, found);
        if (io_fsync(self.dir_fd) != 0) return native_error();
        try self.checkpoint();
    }

    fn create_or_reuse_old_manifest_temp(self: *store_t, txid: [32]u8, journal: journal_t, previous: []const u8) !file_info_t {
        const temp = manifest_temp_name(txid);
        const found_optional = try self.inspect_named(self.dir_fd, temp[0..].ptr);
        const found = if (found_optional) |present| present else blk: {
            // Older v1 journals did not record the displaced manifest inode
            // and used rename publication, so a published PREPARED journal may
            // have no old-manifest temp. Recreate only that validated legacy
            // state, before any artifact mutation; v2 must keep its exact inode.
            if (journal.version != 1 or journal_old_manifest_info(journal) != null or
                !hash_text_equal(hash_bytes(previous), journal.old_manifest_hash orelse return error.RecoveryRequired))
                return error.RecoveryRequired;
            break :blk try self.create_state_temp(previous, 0o600, txid);
        };
        if (found.mode != 0o600 or !hash_text_equal(found.hash, journal.old_manifest_hash orelse return error.RecoveryRequired))
            return error.ArtifactConflict;
        if (journal_old_manifest_info(journal)) |expected| {
            if (!same_file_info(found, expected)) return error.ArtifactConflict;
        }
        return found;
    }

    fn validate_old_manifest_temp(self: *store_t, journal: journal_t, name: [*c]const u8) !void {
        const found = try self.inspect_named(self.dir_fd, name) orelse return;
        if (found.mode != 0o600 or !hash_text_equal(found.hash, journal.old_manifest_hash orelse return error.RecoveryRequired))
            return error.ArtifactConflict;
        if (journal_old_manifest_info(journal)) |expected| {
            if (!same_file_info(found, expected)) return error.ArtifactConflict;
        }
    }

    /// Unlink the artifact at `relative` only when the inode it names has exactly the
    /// `expected` identity. The name is first moved atomically to the transaction's
    /// private staging name with no-clobber semantics, so no later name lookup can
    /// race the check. The moved inode is verified by device, inode, mode, and digest
    /// before it is unlinked. A moved file that fails verification is returned to its
    /// name with no-clobber semantics; if that return fails it stays at the staging name
    /// and KeptAtTempName reports that name. A staging file left by an interrupted
    /// attempt is removed only when it has the expected identity.
    fn unlink_owned_artifact(self: *store_t, root_fd: c_int, relative: []const u8, expected: file_info_t, txid: [32]u8) !artifact_disposition_t {
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        const leaf = try self.allocator.dupeZ(u8, parent.leaf);
        defer self.allocator.free(leaf);
        const staging = artifact_temp_name(txid);
        const staging_name: [:0]const u8 = staging[0..44 :0];
        const disposition_kept = artifact_disposition_t{ .KeptAtTempName = staging[0..44].* };
        if (try self.inspect_named(parent.fd, staging_name.ptr)) |residue| {
            if (!same_file_info(residue, expected)) return disposition_kept;
            try unlink_named_if_matches(self.allocator, parent.fd, staging_name.ptr, residue);
            if (io_fsync(parent.fd) != 0) return native_error();
            try self.checkpoint();
            return .Unlinked;
        }
        if (io_renameat2(parent.fd, leaf.ptr, parent.fd, staging_name.ptr, c.RENAME_NOREPLACE) != 0) {
            if (errno_value() == c.ENOENT) return .AlreadyAbsent;
            return native_error();
        }
        try self.checkpoint();
        unlink_named_if_matches(self.allocator, parent.fd, staging_name.ptr, expected) catch |err| {
            if (err != error.ArtifactConflict) return err;
            if (io_renameat2(parent.fd, staging_name.ptr, parent.fd, leaf.ptr, c.RENAME_NOREPLACE) != 0) return disposition_kept;
            if (io_fsync(parent.fd) != 0) return native_error();
            try self.checkpoint();
            return .KeptUnchanged;
        };
        if (io_fsync(parent.fd) != 0) return native_error();
        try self.checkpoint();
        return .Unlinked;
    }

    fn unlink_owned_manifest(self: *store_t, expected: file_info_t) !void {
        const current = (try self.inspect_manifest()) orelse return error.ArtifactConflict;
        if (!same_file_info(current, expected)) return error.ArtifactConflict;
        var st: c.struct_stat = undefined;
        if (io_fstatat(self.dir_fd, "manifest.json", &st, c.AT_SYMLINK_NOFOLLOW) != 0 or
            @as(u64, @intCast(st.st_dev)) != expected.device or @as(u64, @intCast(st.st_ino)) != expected.inode) return error.ArtifactConflict;
        if (io_unlinkat(self.dir_fd, "manifest.json", 0) != 0 or io_fsync(self.dir_fd) != 0) return native_error();
        try self.checkpoint();
    }

    fn publish_file(self: *store_t, root_fd: c_int, relative: []const u8, txid: [32]u8, expected: file_info_t) !void {
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        const target = try self.inspect(root_fd, relative);
        if (target != null) return error.ArtifactConflict;
        const temp = artifact_temp_name(txid);
        try verify_named_file(self, parent.fd, temp[0..].ptr, expected);
        var leaf: [256]u8 = undefined;
        if (parent.leaf.len >= leaf.len) return error.InvalidPath;
        @memcpy(leaf[0..parent.leaf.len], parent.leaf);
        leaf[parent.leaf.len] = 0;
        if (io_renameat2(parent.fd, temp[0..].ptr, parent.fd, &leaf, c.RENAME_NOREPLACE) != 0 or io_fsync(parent.fd) != 0) return native_error();
        try self.checkpoint();
    }

    fn publish_manifest(self: *store_t, txid: [32]u8, expected: file_info_t, old_hash: ?[]const u8, old_info: ?file_info_t) !void {
        const temp = manifest_temp_name(txid);
        try verify_named_file(self, self.dir_fd, temp[0..].ptr, expected);
        if (old_hash) |digest| {
            const current = try self.inspect_manifest();
            const found = current orelse return error.ArtifactConflict;
            if (!hash_text_equal(found.hash, digest)) return error.ArtifactConflict;
            if (old_info) |exact| if (!same_file_info(found, exact)) return error.ArtifactConflict;
            if (io_renameat2(self.dir_fd, temp[0..].ptr, self.dir_fd, "manifest.json", c.RENAME_EXCHANGE) != 0) {
                if (errno_value() == c.ENOSYS or errno_value() == c.EINVAL or errno_value() == c.EOPNOTSUPP)
                    return error.UnsupportedAtomicReplace;
                return native_error();
            }
            const displaced = (try self.inspect_named(self.dir_fd, temp[0..].ptr)) orelse return error.ArtifactConflict;
            if (!same_file_info(displaced, found)) return error.ArtifactConflict;
        } else {
            if (io_renameat2(self.dir_fd, temp[0..].ptr, self.dir_fd, "manifest.json", c.RENAME_NOREPLACE) != 0) return native_error();
        }
        if (io_fsync(self.dir_fd) != 0) return native_error();
        try self.checkpoint();
    }

    fn verify_named_file(self: *store_t, parent_fd: c_int, name: [*c]const u8, expected: file_info_t) !void {
        const fd = io_openat3(parent_fd, name, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) return error.ArtifactConflict;
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o7000 != 0) return error.ArtifactConflict;
        const bytes = try read_all(self.allocator, fd, MaxArtifactBytes);
        defer self.allocator.free(bytes);
        const hash = hash_bytes(bytes);
        if (@as(u64, @intCast(st.st_dev)) != expected.device or @as(u64, @intCast(st.st_ino)) != expected.inode or
            @as(u32, @intCast(st.st_mode & 0o777)) != expected.mode or !std.mem.eql(u8, &hash, &expected.hash)) return error.ArtifactConflict;
    }

    fn validate_optional_temp(self: *store_t, parent_fd: c_int, name: [*c]const u8, expected: file_info_t, must_exist: bool, must_be_absent: bool) !void {
        const fd = io_openat3(parent_fd, name, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) {
            if (errno_value() == c.ENOENT and !must_exist) return;
            return error.ArtifactConflict;
        }
        defer _ = c.close(fd);
        if (must_be_absent) return error.ArtifactConflict;
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o7000 != 0) return error.ArtifactConflict;
        const bytes = try read_all(self.allocator, fd, MaxArtifactBytes);
        defer self.allocator.free(bytes);
        const hash = hash_bytes(bytes);
        if (@as(u64, @intCast(st.st_dev)) != expected.device or @as(u64, @intCast(st.st_ino)) != expected.inode or
            @as(u32, @intCast(st.st_mode & 0o777)) != expected.mode or !std.mem.eql(u8, &hash, &expected.hash)) return error.ArtifactConflict;
    }

    fn inspect_manifest(self: *store_t) !?file_info_t {
        const fd = io_openat3(self.dir_fd, "manifest.json", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) {
            if (errno_value() == c.ENOENT) return null;
            return native_error();
        }
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o777 != 0o600 or st.st_mode & 0o7000 != 0) return error.AccessDenied;
        const bytes = try read_all(self.allocator, fd, manifest.MaxManifestBytes);
        defer self.allocator.free(bytes);
        return .{ .hash = hash_bytes(bytes), .mode = 0o600, .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) };
    }

    fn remove_tx_tree(self: *store_t, txid: [32]u8) !void {
        const parent = try self.open_private_dir("transactions");
        defer _ = c.close(parent);
        var name: [33]u8 = undefined;
        @memcpy(name[0..32], &txid);
        name[32] = 0;
        const clean = cleanup_name(txid);
        if (io_renameat2(parent, &name, parent, &clean, c.RENAME_NOREPLACE) != 0) {
            if (errno_value() != c.ENOENT and errno_value() != c.EEXIST) return native_error();
        }
        if (io_fsync(parent) != 0) return native_error();
        try self.checkpoint();
        try self.remove_cleanup_dir(parent, txid);
    }

    fn remove_cleanup_dir(self: *store_t, parent: c_int, txid: [32]u8) !void {
        const clean = cleanup_name(txid);
        const tx = io_openat3(parent, &clean, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (tx < 0) {
            if (errno_value() == c.ENOENT) return;
            return error.RecoveryRequired;
        }
        {
            defer _ = c.close(tx);
            var dir_stat: c.struct_stat = undefined;
            if (io_fstat(tx, &dir_stat) != 0 or dir_stat.st_uid != c.getuid() or dir_stat.st_mode & 0o777 != 0o700) return error.AccessDenied;
            for ([_][]const u8{ "journal.next", "new-artifact", "new-manifest", "old-artifact", "old-manifest", "journal.json" }) |leaf| {
                var leafz: [32]u8 = undefined;
                if (leaf.len >= leafz.len) return error.InvalidArgument;
                @memcpy(leafz[0..leaf.len], leaf);
                leafz[leaf.len] = 0;
                var file_stat: c.struct_stat = undefined;
                if (io_fstatat(tx, &leafz, &file_stat, c.AT_SYMLINK_NOFOLLOW) != 0) {
                    if (errno_value() == c.ENOENT) continue;
                    return native_error();
                }
                if (file_stat.st_mode & c.S_IFMT != c.S_IFREG or file_stat.st_uid != c.getuid() or file_stat.st_nlink != 1) return error.RecoveryRequired;
                if (io_unlinkat(tx, &leafz, 0) != 0) return native_error();
            }
            if (io_fsync(tx) != 0) return native_error();
        }
        try self.checkpoint();
        const name = cleanup_name(txid);
        if (io_unlinkat(parent, &name, c.AT_REMOVEDIR) != 0 or io_fsync(parent) != 0) return native_error();
        try self.checkpoint();
    }
};

const resolved_t = struct { root: owned_root_t, relative: []u8 };
/// Manifest bytes and the exact identity of the same opened `manifest.json`.
const manifest_snapshot_t = struct { bytes: []u8, info: file_info_t };

/// Version-3 journal shared by manifest-level operations. A removal names one
/// allowlisted artifact by its exact identity; a metadata migration names none.
/// Both name the exact old and candidate manifest identities. Strict parsing
/// rejects unknown and duplicate fields.
const journal_v3_t = struct {
    /// Journal format version; exactly 3 for this family.
    version: u32,
    /// Lowercase hex transaction identifier equal to the directory name.
    transaction_id: []const u8,
    /// `prepared` or `committed`.
    phase: []const u8,
    /// `remove` or `migrate`.
    operation: []const u8,
    /// Schema record identifier that the operation changes.
    record_id: []const u8,
    /// Root role of the removed artifact; null for migration.
    root: ?[]const u8 = null,
    /// Root-relative path of the removed artifact; null for migration.
    relative_path: ?[]const u8 = null,
    /// SHA-256 hex of the removed artifact; null for migration.
    old_artifact_hash: ?[]const u8 = null,
    /// Permission bits of the removed artifact; zero for migration.
    old_artifact_mode: u32 = 0,
    /// Device number of the removed artifact; null for migration.
    old_artifact_device: ?u64 = null,
    /// Inode number of the removed artifact; null for migration.
    old_artifact_inode: ?u64 = null,
    /// SHA-256 hex of the displaced old manifest.
    old_manifest_hash: []const u8,
    /// Device number of the old manifest.
    old_manifest_device: u64,
    /// Inode number of the old manifest.
    old_manifest_inode: u64,
    /// SHA-256 hex of the candidate manifest.
    new_manifest_hash: []const u8,
    /// Device number of the staged candidate manifest.
    new_manifest_device: u64,
    /// Inode number of the staged candidate manifest.
    new_manifest_inode: u64,
    /// SHA-256 hex of a recreated artifact; null unless the operation is `recreate`.
    new_artifact_hash: ?[]const u8 = null,
    /// Permission bits of a recreated artifact; zero unless the operation is `recreate`.
    new_artifact_mode: u32 = 0,
    /// Device number of a recreated artifact; null unless the operation is `recreate`.
    new_artifact_device: ?u64 = null,
    /// Inode number of a recreated artifact; null unless the operation is `recreate`.
    new_artifact_inode: ?u64 = null,
};

/// Live manifest state observed during version-3 recovery.
const v3_manifest_state_t = enum { Candidate, Original };

/// Private argument bundle for `run_manifest_transaction`. Removal sets every
/// optional artifact field; migration leaves them null.
const transaction_spec_t = struct {
    /// Journal operation, `remove` or `migrate`.
    operation: []const u8,
    /// Schema record identifier, borrowed for the transaction.
    record_id: []const u8,
    /// Allowlisted root of a removed artifact; null for migration.
    root: ?owned_root_t = null,
    /// Root-relative path of a removed artifact; null for migration.
    relative: ?[]const u8 = null,
    /// Exact identity of a removed artifact, verified before any manifest publish;
    /// null for migration, which never touches the artifact.
    artifact: ?file_info_t = null,
    /// Exact bytes of the displaced manifest, retained as transaction evidence.
    old_manifest: []const u8,
    /// Identity of the displaced manifest.
    old_info: file_info_t,
    /// Borrowed candidate manifest bytes.
    candidate: []const u8,
    /// Bytes of an absent artifact to publish (`recreate`); null for other operations.
    new_artifact_bytes: ?[]const u8 = null,
    /// Mode of the artifact published by `recreate`.
    new_artifact_mode: u9 = 0o644,
};

/// True for a version-3 recreate journal, which publishes an absent artifact.
fn v3_is_recreate(journal: journal_v3_t) bool {
    return std.mem.eql(u8, journal.operation, "recreate");
}

/// Identity of the artifact staged or published by a recreate journal.
noinline fn v3_new_artifact_info(journal: journal_v3_t) file_info_t {
    return .{ .hash = hash_from_text(journal.new_artifact_hash.?), .mode = journal.new_artifact_mode, .device = journal.new_artifact_device.?, .inode = journal.new_artifact_inode.? };
}

noinline fn v3_old_manifest_info(journal: journal_v3_t) file_info_t {
    return .{ .hash = hash_from_text(journal.old_manifest_hash), .mode = 0o600, .device = journal.old_manifest_device, .inode = journal.old_manifest_inode };
}
noinline fn v3_new_manifest_info(journal: journal_v3_t) file_info_t {
    return .{ .hash = hash_from_text(journal.new_manifest_hash), .mode = 0o600, .device = journal.new_manifest_device, .inode = journal.new_manifest_inode };
}
noinline fn v3_artifact_info(journal: journal_v3_t) file_info_t {
    return .{ .hash = hash_from_text(journal.old_artifact_hash.?), .mode = journal.old_artifact_mode, .device = journal.old_artifact_device.?, .inode = journal.old_artifact_inode.? };
}

/// Copy a manifest prior value into a receipt-owned buffer. Parsed text is
/// already bounded by the manifest codec, so the copy cannot overflow.
fn copy_prior(prior: manifest.prior_value_t) prior_copy_t {
    return switch (prior) {
        .None => .None,
        .Text => |text| blk: {
            var copy = bounded_text_t{ .bytes = undefined, .len = text.len };
            @memcpy(copy.bytes[0..text.len], text);
            break :blk .{ .Text = copy };
        },
        .U32Value => |value| .{ .U32Value = value },
    };
}

/// Parse one bounded version-3 journal with unknown and duplicate fields rejected.
fn parse_journal_v3(allocator: std.mem.Allocator, bytes: []const u8) !std.json.Parsed(journal_v3_t) {
    return std.json.parseFromSlice(journal_v3_t, allocator, bytes, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = false,
        .duplicate_field_behavior = .@"error",
    }) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
}

/// Read only the top-level `version` of a journal so the dispatcher can choose
/// its parser. Unknown fields are ignored here and rejected by the full parser.
fn journal_version(allocator: std.mem.Allocator, bytes: []const u8) !u32 {
    const Probe = struct { version: u32 = 0 };
    const parsed = std.json.parseFromSlice(Probe, allocator, bytes, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
        .duplicate_field_behavior = .@"error",
    }) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
    defer parsed.deinit();
    return parsed.value.version;
}

/// Validate every immutable version-3 field before any filesystem use. The
/// transaction must name its own directory, a known phase and operation, and
/// exact nonzero manifest identities that differ between old and candidate.
fn validate_journal_v3(journal: journal_v3_t, entry_name: []const u8) !void {
    const phase_valid = std.mem.eql(u8, journal.phase, "prepared") or std.mem.eql(u8, journal.phase, "committed");
    if (journal.version != 3 or !std.mem.eql(u8, journal.transaction_id, entry_name) or !phase_valid or
        !valid_record_id(journal.record_id) or !valid_hash_text(journal.old_manifest_hash) or
        !valid_hash_text(journal.new_manifest_hash) or std.mem.eql(u8, journal.old_manifest_hash, journal.new_manifest_hash) or
        journal.old_manifest_device == 0 or journal.old_manifest_inode == 0 or
        journal.new_manifest_device == 0 or journal.new_manifest_inode == 0) return error.RecoveryRequired;
    if (v3_is_remove(journal)) {
        const artifact_hash = journal.old_artifact_hash orelse return error.RecoveryRequired;
        const artifact_device = journal.old_artifact_device orelse return error.RecoveryRequired;
        const artifact_inode = journal.old_artifact_inode orelse return error.RecoveryRequired;
        if (journal.root == null or journal.relative_path == null or !valid_hash_text(artifact_hash) or
            (journal.old_artifact_mode != 0o644 and journal.old_artifact_mode != 0o755) or
            artifact_device == 0 or artifact_inode == 0 or journal.new_artifact_hash != null or
            journal.new_artifact_mode != 0 or journal.new_artifact_device != null or journal.new_artifact_inode != null) return error.RecoveryRequired;
    } else if (v3_is_recreate(journal)) {
        const artifact_hash = journal.new_artifact_hash orelse return error.RecoveryRequired;
        const artifact_device = journal.new_artifact_device orelse return error.RecoveryRequired;
        const artifact_inode = journal.new_artifact_inode orelse return error.RecoveryRequired;
        if (journal.root == null or journal.relative_path == null or !valid_hash_text(artifact_hash) or
            (journal.new_artifact_mode != 0o644 and journal.new_artifact_mode != 0o755) or
            artifact_device == 0 or artifact_inode == 0 or journal.old_artifact_hash != null or
            journal.old_artifact_mode != 0 or journal.old_artifact_device != null or journal.old_artifact_inode != null) return error.RecoveryRequired;
    } else if (std.mem.eql(u8, journal.operation, "migrate") or std.mem.eql(u8, journal.operation, "forget")) {
        if (journal.root != null or journal.relative_path != null or journal.old_artifact_hash != null or
            journal.old_artifact_mode != 0 or journal.old_artifact_device != null or journal.old_artifact_inode != null or
            journal.new_artifact_hash != null or journal.new_artifact_mode != 0 or journal.new_artifact_device != null or journal.new_artifact_inode != null)
            return error.RecoveryRequired;
    } else return error.RecoveryRequired;
}

/// Compare two dotted numeric Waddle versions component by component; a missing
/// component counts as zero. Any non-numeric component or an empty component is
/// InvalidManifest, so a root version that cannot be ordered never passes a migration.
fn version_order(candidate: []const u8, current: []const u8) !std.math.Order {
    var left = std.mem.splitScalar(u8, candidate, '.');
    var right = std.mem.splitScalar(u8, current, '.');
    while (true) {
        const left_part = left.next();
        const right_part = right.next();
        if (left_part == null and right_part == null) return .eq;
        const left_value = if (left_part) |text| try version_part(text) else 0;
        const right_value = if (right_part) |text| try version_part(text) else 0;
        const order = std.math.order(left_value, right_value);
        if (order != .eq) return order;
    }
}

/// Parse one numeric version component of at most nine decimal digits.
fn version_part(text: []const u8) !u64 {
    if (text.len == 0 or text.len > 9) return error.InvalidManifest;
    var value: u64 = 0;
    for (text) |byte| {
        if (!std.ascii.isDigit(byte)) return error.InvalidManifest;
        value = value * 10 + (byte - '0');
    }
    return value;
}

/// True for a version-3 removal journal. Validated journals carry artifact
/// fields exactly when the operation is a removal.
fn v3_is_remove(journal: journal_v3_t) bool {
    return std.mem.eql(u8, journal.operation, "remove");
}

/// True when a recreated record keeps its target identity. The digest and creator
/// may change, because the artifact bytes are being regenerated.
fn same_recreate_identity(old: manifest.record_t, new: manifest.record_t) bool {
    return old.kind == new.kind and std.mem.eql(u8, old.id, new.id) and std.mem.eql(u8, old.path, new.path) and
        optional_text_equal(old.device, new.device) and optional_text_equal(old.app_key, new.app_key) and
        optional_text_equal(old.mime, new.mime) and same_prior(old.prior, new.prior);
}

/// Verify that a recreate candidate changes only the target digest and creator:
/// equal record counts, the same identity for the target, and every other record
/// equal by schema fields.
fn validate_recreate_delta(old_records: []const manifest.record_t, new_records: []const manifest.record_t, target_id: []const u8) !void {
    if (old_records.len != new_records.len) return error.ManifestMismatch;
    for (old_records) |old| {
        const candidate = find_record(new_records, old.id) orelse return error.ManifestMismatch;
        if (std.mem.eql(u8, old.id, target_id)) {
            if (!same_recreate_identity(old, candidate.*)) return error.ManifestMismatch;
        } else if (!same_record(old, candidate.*)) return error.ManifestMismatch;
    }
}

/// Verify that a migration changes no record: the record counts match and every
/// old record has an equal counterpart by schema fields. Root-level metadata
/// (`waddle_version`, `updated`) is outside this check and may differ.
fn validate_metadata_only(old_records: []const manifest.record_t, new_records: []const manifest.record_t) !void {
    if (old_records.len != new_records.len) return error.ManifestMismatch;
    for (old_records, new_records) |old, new| if (!std.mem.eql(u8, old.id, new.id)) return error.ManifestMismatch;
    for (old_records) |old| {
        const candidate = find_record(new_records, old.id) orelse return error.ManifestMismatch;
        if (!same_record(old, candidate.*)) return error.ManifestMismatch;
    }
}

/// Verify that `new_records` equals `old_records` minus the single target record
/// with every other record byte-for-byte equal by its schema fields. The target
/// must be present in `old_records`; the caller checks that before calling.
fn validate_removal(old_records: []const manifest.record_t, new_records: []const manifest.record_t, target_id: []const u8) !void {
    if (find_record(new_records, target_id) != null or new_records.len + 1 != old_records.len) return error.ManifestMismatch;
    for (old_records) |old| {
        if (std.mem.eql(u8, old.id, target_id)) continue;
        const candidate = find_record(new_records, old.id) orelse return error.ManifestMismatch;
        if (!same_record(old, candidate.*)) return error.ManifestMismatch;
    }
}

fn validate_delta(old_records: []const manifest.record_t, new_records: []const manifest.record_t, target_id: []const u8) !void {
    const old_target = find_record(old_records, target_id);
    const new_target = find_record(new_records, target_id) orelse return error.ManifestMismatch;
    if (old_target) |old| {
        if (!same_target_identity(old.*, new_target.*) or
            old_records.len != new_records.len) return error.ManifestMismatch;
    } else if (new_records.len != old_records.len + 1) return error.ManifestMismatch;

    for (old_records) |old| {
        if (std.mem.eql(u8, old.id, target_id)) continue;
        const candidate = find_record(new_records, old.id) orelse return error.ManifestMismatch;
        if (!same_record(old, candidate.*)) return error.ManifestMismatch;
    }
    for (new_records) |candidate| {
        if (std.mem.eql(u8, candidate.id, target_id)) continue;
        if (find_record(old_records, candidate.id) == null) return error.ManifestMismatch;
    }
}

/// Parse one bounded journal with unknown and duplicate fields rejected.
/// The returned parsed value owns all decoded strings until deinit.
fn parse_journal(allocator: std.mem.Allocator, bytes: []const u8) !std.json.Parsed(journal_t) {
    if (bytes.len == 0 or bytes.len > MaxJournalBytes) return error.RecoveryRequired;
    return std.json.parseFromSlice(journal_t, allocator, bytes, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = false,
        .duplicate_field_behavior = .@"error",
    }) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
}

fn validate_journal_header(journal: journal_t, entry_name: []const u8) !void {
    if ((journal.version != 1 and journal.version != 2) or !std.mem.eql(u8, journal.transaction_id, entry_name) or
        journal.phase.len > 16 or journal.record_id.len > manifest.MaxManifestStringBytes or
        journal.root.len > 16 or journal.relative_path.len >= 4096 or
        !valid_record_id(journal.record_id) or !valid_hash_text(journal.new_artifact_hash) or
        !valid_hash_text(journal.new_manifest_hash) or
        journal.new_artifact_device == 0 or journal.new_artifact_inode == 0 or
        journal.new_manifest_device == 0 or journal.new_manifest_inode == 0 or
        (journal.old_manifest_hash != null and !valid_hash_text(journal.old_manifest_hash.?)) or
        (journal.old_manifest_device == null) != (journal.old_manifest_inode == null) or
        (journal.old_manifest_hash == null and journal.old_manifest_device != null)) return error.RecoveryRequired;
    if (journal.version == 1) {
        if (journal.old_artifact_hash != null or journal.old_artifact_mode != 0 or journal.old_artifact_device != null or
            journal.old_artifact_inode != null or journal.backup_hash != null or journal.backup_mode != 0 or
            journal.backup_device != null or journal.backup_inode != null) return error.RecoveryRequired;
    } else {
        if (journal.old_artifact_hash == null or !valid_hash_text(journal.old_artifact_hash.?) or
            (journal.old_artifact_mode != 0o644 and journal.old_artifact_mode != 0o755) or
            journal.old_artifact_device == null or journal.old_artifact_device.? == 0 or
            journal.old_artifact_inode == null or journal.old_artifact_inode.? == 0 or journal.old_manifest_hash == null or
            journal.old_manifest_device == null or journal.old_manifest_device.? == 0 or
            journal.old_manifest_inode == null or journal.old_manifest_inode.? == 0) return error.RecoveryRequired;
        if (journal.backup_hash) |digest| {
            if (!valid_hash_text(digest) or journal.backup_device == null or journal.backup_device.? == 0 or
                journal.backup_inode == null or journal.backup_inode.? == 0 or
                (journal.backup_mode != 0o644 and journal.backup_mode != 0o755)) return error.RecoveryRequired;
        } else if (journal.backup_mode != 0 or journal.backup_device != null or journal.backup_inode != null) return error.RecoveryRequired;
    }
}

fn validate_global_paths(records: []const manifest.record_t) !void {
    for (records, 0..) |record, i| {
        if (record.path.len == 0 or record.path[0] != '/') continue;
        for (records[0..i]) |prior| {
            if (prior.path.len == 0 or prior.path[0] != '/') continue;
            if (paths_overlap(prior.path, record.path)) return error.ManifestMismatch;
        }
    }
}

fn same_target_identity(a: manifest.record_t, b: manifest.record_t) bool {
    return a.kind == b.kind and std.mem.eql(u8, a.id, b.id) and std.mem.eql(u8, a.path, b.path) and
        optional_text_equal(a.device, b.device) and optional_text_equal(a.app_key, b.app_key) and
        optional_text_equal(a.mime, b.mime) and same_prior(a.prior, b.prior);
}
fn same_prior(a: manifest.prior_value_t, b: manifest.prior_value_t) bool {
    return switch (a) {
        .None => b == .None,
        .Text => |value| b == .Text and std.mem.eql(u8, value, b.Text),
        .U32Value => |value| b == .U32Value and value == b.U32Value,
    };
}

fn same_record(a: manifest.record_t, b: manifest.record_t) bool {
    if (!std.mem.eql(u8, a.id, b.id) or a.kind != b.kind or !std.mem.eql(u8, a.path, b.path) or
        !std.mem.eql(u8, a.sha256, b.sha256) or !std.mem.eql(u8, a.created_by, b.created_by) or
        !optional_text_equal(a.device, b.device) or !optional_text_equal(a.app_key, b.app_key) or
        !optional_text_equal(a.mime, b.mime)) return false;
    return switch (a.prior) {
        .None => b.prior == .None,
        .Text => |value| b.prior == .Text and std.mem.eql(u8, value, b.prior.Text),
        .U32Value => |value| b.prior == .U32Value and value == b.prior.U32Value,
    };
}
fn optional_text_equal(a: ?[]const u8, b: ?[]const u8) bool {
    if (a == null or b == null) return a == null and b == null;
    return std.mem.eql(u8, a.?, b.?);
}

fn find_record(records: []const manifest.record_t, id: []const u8) ?*const manifest.record_t {
    var found: ?*const manifest.record_t = null;
    for (records) |*record| if (std.mem.eql(u8, record.id, id)) {
        if (found != null) return null;
        found = record;
    };
    return found;
}
fn root_name(kind: root_kind_t) []const u8 {
    return switch (kind) {
        .Data => "data",
        .Config => "config",
        .Desktop => "desktop",
    };
}
fn artifact_temp_name(txid: [32]u8) [64]u8 {
    var name = [_]u8{0} ** 64;
    const prefix = ".waddle-";
    @memcpy(name[0..prefix.len], prefix);
    @memcpy(name[prefix.len .. prefix.len + txid.len], &txid);
    @memcpy(name[prefix.len + txid.len .. prefix.len + txid.len + 4], ".tmp");
    return name;
}
fn manifest_temp_name(txid: [32]u8) [64]u8 {
    var name = [_]u8{0} ** 64;
    const prefix = ".manifest-";
    @memcpy(name[0..prefix.len], prefix);
    @memcpy(name[prefix.len .. prefix.len + txid.len], &txid);
    @memcpy(name[prefix.len + txid.len .. prefix.len + txid.len + 4], ".tmp");
    return name;
}
fn unlink_named_if_matches(allocator: std.mem.Allocator, parent_fd: c_int, name: [*c]const u8, expected: file_info_t) !void {
    var st: c.struct_stat = undefined;
    if (io_fstatat(parent_fd, name, &st, c.AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno_value() == c.ENOENT) return;
        return native_error();
    }
    if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o7000 != 0 or
        @as(u64, @intCast(st.st_dev)) != expected.device or @as(u64, @intCast(st.st_ino)) != expected.inode or
        @as(u32, @intCast(st.st_mode & 0o777)) != expected.mode) return error.ArtifactConflict;
    const fd = io_openat3(parent_fd, name, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
    if (fd < 0) return error.ArtifactConflict;
    defer _ = c.close(fd);
    var opened: c.struct_stat = undefined;
    if (io_fstat(fd, &opened) != 0 or opened.st_mode & c.S_IFMT != c.S_IFREG or opened.st_uid != c.getuid() or opened.st_nlink != 1 or opened.st_mode & 0o7000 != 0 or
        @as(u64, @intCast(opened.st_dev)) != expected.device or @as(u64, @intCast(opened.st_ino)) != expected.inode or
        @as(u32, @intCast(opened.st_mode & 0o777)) != expected.mode) return error.ArtifactConflict;
    const bytes = try read_all(allocator, fd, MaxArtifactBytes);
    defer allocator.free(bytes);
    const content_hash = hash_bytes(bytes);
    if (!std.mem.eql(u8, &content_hash, &expected.hash)) return error.ArtifactConflict;
    var current: c.struct_stat = undefined;
    if (io_fstatat(parent_fd, name, &current, c.AT_SYMLINK_NOFOLLOW) != 0 or
        @as(u64, @intCast(current.st_dev)) != expected.device or @as(u64, @intCast(current.st_ino)) != expected.inode) return error.ArtifactConflict;
    if (io_unlinkat(parent_fd, name, 0) != 0) return native_error();
}
fn validate_absolute_root(path: []const u8) !void {
    if (path.len < 2 or path[0] != '/' or path[path.len - 1] == '/' or path.len >= 4096 or std.mem.indexOfScalar(u8, path, 0) != null) return error.InvalidPath;
    try validate_relative(path[1..]);
}
fn paths_overlap(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b) or path_contains(a, b) or path_contains(b, a);
}
fn path_contains(parent: []const u8, child: []const u8) bool {
    return child.len > parent.len and std.mem.startsWith(u8, child, parent) and child[parent.len] == '/';
}
fn validate_root_binding(path: []const u8, root_fd: c_int) !void {
    const current = io_open("/", c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (current < 0) return native_error();
    var fd = current;
    errdefer _ = c.close(fd);
    var parts = std.mem.splitScalar(u8, path[1..], '/');
    while (parts.next()) |part| {
        var name: [256]u8 = undefined;
        @memcpy(name[0..part.len], part);
        name[part.len] = 0;
        const next = io_openat3(fd, &name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (next < 0) return native_error();
        _ = c.close(fd);
        fd = next;
    }
    var expected: c.struct_stat = undefined;
    var actual: c.struct_stat = undefined;
    if (io_fstat(root_fd, &expected) != 0 or io_fstat(fd, &actual) != 0) return native_error();
    if (expected.st_dev != actual.st_dev or expected.st_ino != actual.st_ino) return error.InvalidScope;
    _ = c.close(fd);
}
fn validate_relative(path: []const u8) !void {
    if (path.len == 0 or path.len >= 4096 or path[0] == '/' or std.mem.indexOfScalar(u8, path, 0) != null or !std.unicode.utf8ValidateSlice(path)) return error.InvalidPath;
    var parts = std.mem.splitScalar(u8, path, '/');
    var count: usize = 0;
    while (parts.next()) |part| {
        count += 1;
        if (count > 64 or part.len == 0 or part.len > 255 or std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return error.InvalidPath;
    }
}
fn valid_record_id(text: []const u8) bool {
    return text.len > 0 and text.len <= manifest.MaxManifestStringBytes and std.unicode.utf8ValidateSlice(text) and std.mem.indexOfScalar(u8, text, 0) == null;
}
fn valid_txid(text: []const u8) bool {
    if (text.len != 32) return false;
    for (text) |byte| if (!std.ascii.isHex(byte)) return false;
    return true;
}
fn valid_backup_txid(text: []const u8) bool {
    if (!valid_txid(text)) return false;
    for (text) |byte| if (byte >= 'A' and byte <= 'F') return false;
    return true;
}
fn cleanup_name(txid: [32]u8) [42]u8 {
    var name = [_]u8{0} ** 42;
    const prefix = ".cleanup-";
    @memcpy(name[0..prefix.len], prefix);
    @memcpy(name[prefix.len .. prefix.len + txid.len], &txid);
    return name;
}
fn cleanup_txid(name: []const u8) ?[32]u8 {
    const prefix = ".cleanup-";
    if (name.len != prefix.len + 32 or !std.mem.startsWith(u8, name, prefix)) return null;
    const txid = name[prefix.len..];
    if (!valid_txid(txid)) return null;
    return txid_array(txid) catch null;
}
fn valid_hash_text(text: []const u8) bool {
    if (text.len != 64) return false;
    for (text) |byte| if (!std.ascii.isHex(byte)) return false;
    return true;
}
fn first_component(path: []const u8, component: []const u8) bool {
    return std.mem.startsWith(u8, path, component) and path.len > component.len and path[component.len] == '/';
}
fn icon_path_allowed(path: []const u8) bool {
    if (!std.mem.startsWith(u8, path, "hicolor/")) return false;
    var parts = std.mem.splitScalar(u8, path, '/');
    _ = parts.next();
    const size = parts.next() orelse return false;
    if (size.len == 0 or !std.mem.eql(u8, parts.next() orelse return false, "apps")) return false;
    const leaf = parts.next() orelse return false;
    return leaf.len > 0 and parts.next() == null;
}
fn hash_bytes(bytes: []const u8) [32]u8 {
    var hash: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &hash, .{});
    return hash;
}
fn hash_hex(bytes: []const u8) [64]u8 {
    const digest = hash_bytes(bytes);
    return bytes_to_lower_hex(32, &digest);
}
fn hash_hex_from_array(hash: [32]u8) [64]u8 {
    return bytes_to_lower_hex(32, &hash);
}
fn bytes_to_lower_hex(comptime input_len: usize, input: *const [input_len]u8) [input_len * 2]u8 {
    const digits = "0123456789abcdef";
    var result: [input_len * 2]u8 = undefined;
    for (input.*, 0..) |byte, index| {
        result[index * 2] = digits[byte >> 4];
        result[index * 2 + 1] = digits[byte & 0x0f];
    }
    return result;
}
fn encode_backup_metadata(txid: [32]u8, record_id: []const u8, info: file_info_t) [BackupMetadataBytes]u8 {
    var bytes = [_]u8{0} ** BackupMetadataBytes;
    @memcpy(bytes[0..4], "WDBK");
    std.mem.writeInt(u32, bytes[4..8], 1, .little);
    @memcpy(bytes[8..40], &txid);
    const record_hash = hash_bytes(record_id);
    @memcpy(bytes[40..72], &record_hash);
    @memcpy(bytes[72..104], &info.hash);
    std.mem.writeInt(u32, bytes[104..108], info.mode, .little);
    std.mem.writeInt(u64, bytes[108..116], info.device, .little);
    std.mem.writeInt(u64, bytes[116..124], info.inode, .little);
    return bytes;
}
fn decode_backup_metadata(expected_txid: [32]u8, bytes: []const u8) !backup_ref_t {
    if (bytes.len != BackupMetadataBytes or !std.mem.eql(u8, bytes[0..4], "WDBK") or
        std.mem.readInt(u32, bytes[4..8], .little) != 1 or !std.mem.eql(u8, bytes[8..40], &expected_txid)) return error.RecoveryRequired;
    const artifact_mode = std.mem.readInt(u32, bytes[104..108], .little);
    const device = std.mem.readInt(u64, bytes[108..116], .little);
    const inode = std.mem.readInt(u64, bytes[116..124], .little);
    if ((artifact_mode != 0o644 and artifact_mode != 0o755) or device == 0 or inode == 0) return error.RecoveryRequired;
    var record_hash: [32]u8 = undefined;
    var content_hash: [32]u8 = undefined;
    @memcpy(&record_hash, bytes[40..72]);
    @memcpy(&content_hash, bytes[72..104]);
    return .{
        .transaction_id = expected_txid,
        .record_id_sha256 = record_hash,
        .sha256 = content_hash,
        .artifact_mode = artifact_mode,
        .device = device,
        .inode = inode,
    };
}
fn same_backup_ref(a: backup_ref_t, b: backup_ref_t) bool {
    return std.mem.eql(u8, &a.transaction_id, &b.transaction_id) and
        std.mem.eql(u8, &a.record_id_sha256, &b.record_id_sha256) and
        std.mem.eql(u8, &a.sha256, &b.sha256) and a.artifact_mode == b.artifact_mode and
        a.device == b.device and a.inode == b.inode;
}
fn hash_from_text(text: []const u8) [32]u8 {
    var hash: [32]u8 = undefined;
    for (0..hash.len) |i| hash[i] = (hex_digit(text[i * 2]) << 4) | hex_digit(text[i * 2 + 1]);
    return hash;
}
fn hex_digit(byte: u8) u8 {
    return if (byte >= '0' and byte <= '9') byte - '0' else std.ascii.toLower(byte) - 'a' + 10;
}
fn journal_artifact_info(journal: journal_t) file_info_t {
    return .{ .hash = hash_from_text(journal.new_artifact_hash), .mode = journal.new_artifact_mode, .device = journal.new_artifact_device, .inode = journal.new_artifact_inode };
}
fn journal_old_artifact_info(journal: journal_t) file_info_t {
    return .{ .hash = hash_from_text(journal.old_artifact_hash orelse unreachable), .mode = journal.old_artifact_mode, .device = journal.old_artifact_device orelse unreachable, .inode = journal.old_artifact_inode orelse unreachable };
}
fn journal_manifest_info(journal: journal_t) file_info_t {
    return .{ .hash = hash_from_text(journal.new_manifest_hash), .mode = 0o600, .device = journal.new_manifest_device, .inode = journal.new_manifest_inode };
}
fn journal_old_manifest_info(journal: journal_t) ?file_info_t {
    if (journal.old_manifest_hash == null) return null;
    if (journal.old_manifest_device == null or journal.old_manifest_inode == null) return null;
    return .{ .hash = hash_from_text(journal.old_manifest_hash.?), .mode = 0o600, .device = journal.old_manifest_device.?, .inode = journal.old_manifest_inode.? };
}
fn same_file_info(a: file_info_t, b: file_info_t) bool {
    return a.device == b.device and a.inode == b.inode and a.mode == b.mode and std.mem.eql(u8, &a.hash, &b.hash);
}
fn same_identity(info: file_info_t, identity: artifact_identity_t) bool {
    return info.device == identity.device and info.inode == identity.inode and info.mode == identity.mode and
        std.mem.eql(u8, &info.hash, &identity.sha256);
}
fn identity_from_info(info: file_info_t) artifact_identity_t {
    return .{ .device = info.device, .inode = info.inode, .mode = info.mode, .sha256 = info.hash };
}
fn backup_reference(txid: [32]u8, record_id: []const u8, info: file_info_t) backup_ref_t {
    return .{ .transaction_id = txid, .record_id_sha256 = hash_bytes(record_id), .sha256 = info.hash, .artifact_mode = info.mode, .device = info.device, .inode = info.inode };
}
fn hash_text_equal(hash: [32]u8, text: []const u8) bool {
    return std.ascii.eqlIgnoreCase(&hash_hex_from_array(hash), text);
}
fn txid_array(text: []const u8) ![32]u8 {
    if (!valid_txid(text)) return error.RecoveryRequired;
    var result: [32]u8 = undefined;
    @memcpy(&result, text);
    return result;
}
fn random_id() ![32]u8 {
    var bytes: [16]u8 = undefined;
    std.crypto.random.bytes(&bytes);
    return bytes_to_lower_hex(16, &bytes);
}
fn errno_value() c_int {
    return c.__errno_location().*;
}
fn dup_cloexec(fd: c_int) c_int {
    return io_openat3(fd, ".", c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
}
fn native_error() anyerror {
    return native_error_code(errno_value());
}
fn native_error_code(code: c_int) anyerror {
    return switch (code) {
        c.ENOENT => error.FileNotFound,
        c.EEXIST => error.PathAlreadyExists,
        c.EACCES, c.EPERM, c.ELOOP, c.ENOTDIR => error.AccessDenied,
        c.ENOSPC => error.NoSpaceLeft,
        c.EINTR => error.Interrupted,
        c.EBUSY => error.Busy,
        else => error.IoError,
    };
}
fn native_error_union(code: c_int) anyerror!void {
    return native_error_code(code);
}
fn write_all(fd: c_int, bytes: []const u8) !void {
    var offset: usize = 0;
    while (offset < bytes.len) {
        const amount = io_write(fd, bytes[offset..].ptr, bytes.len - offset);
        if (amount < 0) {
            if (errno_value() == c.EINTR) continue;
            return native_error();
        }
        if (amount == 0) return error.IoError;
        offset += @intCast(amount);
    }
}
fn read_all(allocator: std.mem.Allocator, fd: c_int, limit: usize) ![]u8 {
    var output = std.ArrayList(u8).init(allocator);
    errdefer output.deinit();
    var buffer: [8192]u8 = undefined;
    while (true) {
        const amount = io_read(fd, &buffer, buffer.len);
        if (amount < 0) {
            if (errno_value() == c.EINTR) continue;
            return native_error();
        }
        if (amount == 0) break;
        const count: usize = @intCast(amount);
        if (count > limit -| output.items.len) return error.FileTooLarge;
        try output.appendSlice(buffer[0..count]);
    }
    return output.toOwnedSlice();
}

test "relative path rejects traversal and symlink-shaped ambiguities" {
    for ([_][]const u8{ "", "/etc/passwd", "../x", "a/../x", "a//b", "a/./b", "a/" }) |path|
        try std.testing.expectError(error.InvalidPath, validate_relative(path));
    try validate_relative("applications/waddle.desktop");
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("/"));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root(""));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("\x00"));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("relative"));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("/tmp/"));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("/tmp/a\x00b"));
    var too_long_root: [4096]u8 = undefined;
    @memset(&too_long_root, 'a');
    too_long_root[0] = '/';
    try std.testing.expectError(error.InvalidPath, validate_absolute_root(&too_long_root));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("/tmp/a\x00b"));
    try validate_absolute_root("/tmp/safe-root");
}

test "scalar lowercase hex conversion preserves every byte value exactly" {
    var input: [256]u8 = undefined;
    for (&input, 0..) |*byte, index| byte.* = @intCast(index);
    const encoded = bytes_to_lower_hex(256, &input);
    const digits = "0123456789abcdef";
    for (input, 0..) |byte, index| {
        try std.testing.expectEqual(digits[byte >> 4], encoded[index * 2]);
        try std.testing.expectEqual(digits[byte & 0x0f], encoded[index * 2 + 1]);
    }
    const prefix: [16]u8 = input[0..16].*;
    const encoded_prefix = bytes_to_lower_hex(16, &prefix);
    try std.testing.expectEqualStrings("000102030405060708090a0b0c0d0e0f", &encoded_prefix);
}

test "bounded transfer loops handle real short writes, short reads, and EINTR" {
    defer {
        injected_io_at = 0;
        injected_eintr_at = 0;
        injected_short_at = 0;
        injected_zero_at = 0;
        injected_io_count = 0;
    }
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const fd = c.openat(temporary.dir.fd, "transfer", c.O_RDWR | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC, @as(c_uint, 0o600));
    if (fd < 0) return error.IoError;
    defer _ = c.close(fd);
    defer _ = c.unlinkat(temporary.dir.fd, "transfer", 0);

    const expected = "bounded short transfer payload";
    injected_io_count = 0;
    injected_short_at = 1;
    injected_short_limit = 3;
    try write_all(fd, expected);
    try std.testing.expectEqual(@as(usize, 0), injected_short_at);
    try std.testing.expectEqual(@as(c_long, 0), c.lseek(fd, 0, c.SEEK_SET));

    injected_io_count = 0;
    injected_io_at = 1;
    try std.testing.expectError(error.IoError, read_all(std.testing.allocator, fd, 128));
    injected_io_at = 0;
    try std.testing.expectEqual(@as(c_long, expected.len), c.lseek(fd, 0, c.SEEK_END));
    injected_io_count = 0;
    injected_io_at = 1;
    try std.testing.expectError(error.IoError, write_all(fd, "unwritten"));
    injected_io_at = 0;
    injected_io_count = 0;
    injected_zero_at = 1;
    try std.testing.expectError(error.IoError, write_all(fd, "unwritten"));
    injected_zero_at = 0;
    try std.testing.expectEqual(@as(c_long, expected.len), c.lseek(fd, 0, c.SEEK_END));
    try std.testing.expectEqual(@as(c_long, 0), c.lseek(fd, 0, c.SEEK_SET));
    try std.testing.expectError(error.FileTooLarge, read_all(std.testing.allocator, fd, 2));
    try std.testing.expectEqual(@as(c_long, 0), c.lseek(fd, 0, c.SEEK_SET));

    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    try std.testing.expectError(error.OutOfMemory, read_all(failing.allocator(), fd, 128));
    try std.testing.expect(failing.has_induced_failure);
    try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    try std.testing.expectEqual(@as(c_long, 0), c.lseek(fd, 0, c.SEEK_SET));

    injected_io_count = 0;
    injected_eintr_at = 1;
    const interrupted_read = try read_all(std.testing.allocator, fd, 128);
    defer std.testing.allocator.free(interrupted_read);
    try std.testing.expectEqual(@as(usize, 0), injected_eintr_at);
    try std.testing.expectEqualStrings(expected, interrupted_read);

    try std.testing.expectEqual(@as(c_long, 0), c.lseek(fd, 0, c.SEEK_SET));
    injected_io_count = 0;
    injected_short_at = 1;
    const short_read = try read_all(std.testing.allocator, fd, 128);
    defer std.testing.allocator.free(short_read);
    try std.testing.expectEqual(@as(usize, 0), injected_short_at);
    try std.testing.expectEqualStrings(expected, short_read);
    try std.testing.expectEqual(@as(c_long, expected.len), c.lseek(fd, 0, c.SEEK_END));
    injected_io_count = 0;
    injected_eintr_at = 1;
    try write_all(fd, "!");
    try std.testing.expectEqual(@as(usize, 0), injected_eintr_at);
    injected_io_at = 0;
    injected_eintr_at = 0;
    injected_short_at = 0;
}

test "root and record path authorization is component scoped" {
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("/tmp/../etc"));
    try std.testing.expect(first_component("applications/a.desktop", "applications"));
    try std.testing.expect(!first_component("applications-foreign/a.desktop", "applications"));
    try std.testing.expect(icon_path_allowed("hicolor/48x48/apps/test.png"));
    try std.testing.expect(!icon_path_allowed("hicolor/48x48/evil/test.png"));
    const duplicate_paths = [_]manifest.record_t{
        .{ .id = "one", .kind = .DesktopEntry, .path = "/data/applications/a.desktop", .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
        .{ .id = "two", .kind = .HandlerEntry, .path = "/data/applications/a.desktop/child", .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
    };
    try std.testing.expectError(error.ManifestMismatch, validate_global_paths(&duplicate_paths));
    const nested_paths = [_]manifest.record_t{
        .{ .id = "parent", .kind = .DesktopEntry, .path = "/data/applications", .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
        .{ .id = "child", .kind = .HandlerEntry, .path = "/data/applications/child.desktop", .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
    };
    try std.testing.expectError(error.ManifestMismatch, validate_global_paths(&nested_paths));
    const independent_paths = [_]manifest.record_t{
        .{ .id = "one", .kind = .DesktopEntry, .path = "/data/applications/one.desktop", .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
        .{ .id = "two", .kind = .HandlerEntry, .path = "/data/applications/two.desktop", .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
    };
    try validate_global_paths(&independent_paths);
}

test "store lock is nonblocking and preopened roots must bind to canonical paths" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state_dir = try temporary.dir.openDir("state", .{});
    defer state_dir.close();
    var data_dir = try temporary.dir.openDir("data", .{});
    defer data_dir.close();
    const state_path = try state_dir.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data_dir.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data_dir.fd }};
    try validate_root_binding(data_path, data_dir.fd);
    try std.testing.expectError(error.InvalidScope, validate_root_binding(state_path, data_dir.fd));
    try std.testing.expectError(error.InvalidArgument, store_t.init(std.testing.allocator, state_dir.fd, &.{}));
    const too_many = [_]root_t{ roots[0], roots[0], roots[0], roots[0] };
    try std.testing.expectError(error.InvalidArgument, store_t.init(std.testing.allocator, state_dir.fd, &too_many));
    var invalid_descriptor = roots[0];
    invalid_descriptor.dir_fd = -1;
    try std.testing.expectError(error.InvalidArgument, store_t.init(std.testing.allocator, state_dir.fd, &.{invalid_descriptor}));
    const duplicate_kind = [_]root_t{ roots[0], .{ .kind = .Data, .canonical_path = data_path, .dir_fd = data_dir.fd } };
    try std.testing.expectError(error.InvalidArgument, store_t.init(std.testing.allocator, state_dir.fd, &duplicate_kind));
    try data_dir.makeDir("subroot");
    var subroot = try data_dir.openDir("subroot", .{});
    defer subroot.close();
    const subroot_path = try subroot.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(subroot_path);
    const overlapping_roots = [_]root_t{ roots[0], .{ .kind = .Config, .canonical_path = subroot_path, .dir_fd = subroot.fd } };
    try std.testing.expectError(error.InvalidArgument, store_t.init(std.testing.allocator, state_dir.fd, &overlapping_roots));
    var first = try store_t.init(std.testing.allocator, state_dir.fd, &roots);
    defer first.deinit();
    try std.testing.expectError(error.Busy, store_t.init(std.testing.allocator, state_dir.fd, &roots));

    if (c.symlinkat("data", temporary.dir.fd, "data-link") != 0) return error.IoError;
    const temporary_path = try temporary.dir.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(temporary_path);
    const alias_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/data-link", .{temporary_path});
    defer std.testing.allocator.free(alias_path);
    try std.testing.expectError(error.AccessDenied, validate_root_binding(alias_path, data_dir.fd));
    const aliased = [_]root_t{.{ .kind = .Data, .canonical_path = alias_path, .dir_fd = data_dir.fd }};
    try std.testing.expectError(error.AccessDenied, store_t.init(std.testing.allocator, state_dir.fd, &aliased));
}

test "init accepts only owned private transaction and lock files" {
    for (0..5) |case_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        if (case_index == 0 or case_index == 1) {
            try state.makeDir("transactions");
            if (c.fchmodat(state.fd, "transactions", if (case_index == 1) 0o755 else 0o700, 0) != 0) return error.IoError;
        } else if (case_index == 2 or case_index == 3) {
            const lock_fd = c.openat(state.fd, "manifest.lock", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, if (case_index == 2) 0o644 else 0o600));
            if (lock_fd < 0) return error.IoError;
            _ = c.close(lock_fd);
            if (case_index == 3 and c.linkat(state.fd, "manifest.lock", state.fd, "lock-link", 0) != 0) return error.IoError;
        } else {
            if (c.symlinkat("missing", state.fd, "manifest.lock") != 0) return error.IoError;
        }

        if (case_index == 0) {
            var store = try store_t.init(std.testing.allocator, state.fd, &roots);
            store.deinit();
        } else if (case_index == 1) {
            try std.testing.expectError(error.AccessDenied, store_t.init(std.testing.allocator, state.fd, &roots));
        } else if (case_index == 2 or case_index == 3) {
            try std.testing.expectError(error.AccessDenied, store_t.init(std.testing.allocator, state.fd, &roots));
        } else {
            try std.testing.expectError(error.AccessDenied, store_t.init(std.testing.allocator, state.fd, &roots));
        }
    }
}

test "prepared transaction rolls back and committed transaction verifies after injected durable failures" {
    for (2..17) |failure_step| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state_dir = try temporary.dir.openDir("state", .{});
        defer state_dir.close();
        var data_dir = try temporary.dir.openDir("data", .{});
        defer data_dir.close();
        try data_dir.makeDir("applications");
        const state_path = try state_dir.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data_dir.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const artifact_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/example.desktop", .{data_path});
        defer std.testing.allocator.free(artifact_path);
        const artifact_bytes = "managed desktop entry\n";
        const digest = hash_hex(artifact_bytes);
        const manifest_bytes = try std.fmt.allocPrint(
            std.testing.allocator,
            "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:test\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}",
            .{ artifact_path, digest },
        );
        defer std.testing.allocator.free(manifest_bytes);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data_dir.fd }};
        var store = try store_t.init(std.testing.allocator, state_dir.fd, &roots);
        defer store.deinit();
        store.fail_after.store(failure_step, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.commit(.{
            .record_id = "desktop:test",
            .prior = .Absent,
            .artifact_bytes = artifact_bytes,
            .candidate_manifest = manifest_bytes,
        }));
        store.fail_after.store(0, .monotonic);
        if (failure_step == 9) {
            store.fail_after.store(store.checkpoint_count.load(.monotonic) + 1, .monotonic);
            try std.testing.expectError(error.InjectedFailure, store.recover());
            store.fail_after.store(0, .monotonic);
        }
        try store.recover();
        if (failure_step < 12) {
            try std.testing.expectError(error.FileNotFound, data_dir.access("applications/example.desktop", .{}));
            try std.testing.expectError(error.FileNotFound, state_dir.access("manifest.json", .{}));
        } else {
            const written = try data_dir.readFileAlloc(std.testing.allocator, "applications/example.desktop", 4096);
            defer std.testing.allocator.free(written);
            try std.testing.expectEqualStrings(artifact_bytes, written);
            const stored_manifest = try state_dir.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
            defer std.testing.allocator.free(stored_manifest);
            try std.testing.expectEqualStrings(manifest_bytes, stored_manifest);
        }
    }
}

test "prepared application is idempotent after both allowlisted files are published" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const state_path = try state.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/idempotent.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const payload = "idempotent artifact";
    const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:idempotent", payload);
    defer std.testing.allocator.free(candidate);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.commit(.{ .record_id = "desktop:idempotent", .prior = .Absent, .artifact_bytes = payload, .artifact_mode = 0o755, .candidate_manifest = candidate }));
    store.fail_after.store(0, .monotonic);

    var transactions = try state.openDir("transactions", .{ .iterate = true });
    defer transactions.close();
    var iterator = transactions.iterate();
    const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
    const txid = try txid_array(entry.name);
    var transaction = try transactions.openDir(entry.name, .{});
    defer transaction.close();
    const bytes = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(bytes);
    var parsed = try parse_journal(std.testing.allocator, bytes);
    defer parsed.deinit();
    var invalid = parsed.value;
    invalid.record_id = "desktop:missing";
    try std.testing.expectError(error.RecoveryRequired, store.apply_prepared(invalid));
    invalid = parsed.value;
    invalid.relative_path = "applications/other.desktop";
    try std.testing.expectError(error.RecoveryRequired, store.apply_prepared(invalid));
    try transaction.writeFile(.{ .sub_path = "new-artifact", .data = "tampered bytes" });
    try std.testing.expectError(error.RecoveryRequired, store.apply_prepared(parsed.value));
    try transaction.writeFile(.{ .sub_path = "new-artifact", .data = payload });
    try store.apply_prepared(parsed.value);
    try store.apply_prepared(parsed.value);
    try data.writeFile(.{ .sub_path = "applications/idempotent.desktop", .data = "foreign bytes" });
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared(parsed.value));
    try data.writeFile(.{ .sub_path = "applications/idempotent.desktop", .data = payload });
    try state.writeFile(.{ .sub_path = "manifest.json", .data = "foreign manifest" });
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared(parsed.value));
    try state.writeFile(.{ .sub_path = "manifest.json", .data = candidate });
    try store.apply_prepared(parsed.value);
    try store.publish_phase(txid, parsed.value, "committed");
    try store.recover();
    const published = try data.readFileAlloc(std.testing.allocator, "applications/idempotent.desktop", 4096);
    defer std.testing.allocator.free(published);
    try std.testing.expectEqualStrings(payload, published);
    const published_info = (try store.inspect(data.fd, "applications/idempotent.desktop")).?;
    try std.testing.expectEqual(@as(u32, 0o755), published_info.mode);
    const stored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(stored);
    try std.testing.expectEqualStrings(candidate, stored);
}

test "committed recovery retains evidence when a published file is missing or modified" {
    for (0..4) |case_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/committed-conflict.desktop", .{data_path});
        defer std.testing.allocator.free(target_path);
        const payload = "committed bytes";
        const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:committed-conflict", payload);
        defer std.testing.allocator.free(candidate);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(12, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.commit(.{ .record_id = "desktop:committed-conflict", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
        store.fail_after.store(0, .monotonic);

        if (case_index == 0) {
            if (c.unlinkat(data.fd, "applications/committed-conflict.desktop", 0) != 0) return error.IoError;
        } else if (case_index == 1) {
            try data.writeFile(.{ .sub_path = "applications/committed-conflict.desktop", .data = "modified artifact" });
        } else if (case_index == 2) {
            if (c.unlinkat(state.fd, "manifest.json", 0) != 0) return error.IoError;
        } else {
            try state.writeFile(.{ .sub_path = "manifest.json", .data = "modified manifest" });
        }
        try std.testing.expectError(error.RecoveryRequired, store.recover());
        var transactions = try state.openDir("transactions", .{ .iterate = true });
        defer transactions.close();
        var iterator = transactions.iterate();
        const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
        try transaction_evidence_exists(&transactions, entry.name);
    }
}

test "commit allocation failures recover without allocator leaks" {
    var observed_failures: usize = 0;
    for (0..256) |failure_offset| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state_dir = try temporary.dir.openDir("state", .{});
        defer state_dir.close();
        var data_dir = try temporary.dir.openDir("data", .{});
        defer data_dir.close();
        try data_dir.makeDir("applications");
        const state_path = try state_dir.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data_dir.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const artifact_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/failure.desktop", .{data_path});
        defer std.testing.allocator.free(artifact_path);
        const artifact_bytes = "allocator failure entry\n";
        const digest = hash_hex(artifact_bytes);
        const manifest_bytes = try std.fmt.allocPrint(
            std.testing.allocator,
            "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:failure\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}",
            .{ artifact_path, digest },
        );
        defer std.testing.allocator.free(manifest_bytes);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data_dir.fd }};
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        const allocator = failing.allocator();
        var store = try store_t.init(allocator, state_dir.fd, &roots);
        defer store.deinit();
        failing.fail_index = failing.alloc_index + failure_offset;
        const result = store.commit(.{
            .record_id = "desktop:failure",
            .prior = .Absent,
            .artifact_bytes = artifact_bytes,
            .candidate_manifest = manifest_bytes,
        });
        if (!failing.has_induced_failure) {
            try result;
            store.deinit();
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed_failures += 1;
        if (result) |_| {
            return error.ExpectedOutOfMemory;
        } else |err| if (err != error.OutOfMemory) return err;
        failing.fail_index = std.math.maxInt(usize);
        try store.recover();
        const artifact_exists = if (data_dir.access("applications/failure.desktop", .{})) |_| true else |_| false;
        const manifest_exists = if (state_dir.access("manifest.json", .{})) |_| true else |_| false;
        try std.testing.expectEqual(artifact_exists, manifest_exists);
        store.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    try std.testing.expect(observed_failures > 0);
}

test "recovery rejects identical foreign target and preflights manifest before rollback" {
    for ([_]usize{ 8, 9 }, 0..) |failure_step, case_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state_dir = try temporary.dir.openDir("state", .{});
        defer state_dir.close();
        var data_dir = try temporary.dir.openDir("data", .{});
        defer data_dir.close();
        try data_dir.makeDir("applications");
        const state_path = try state_dir.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data_dir.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const artifact_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/foreign.desktop", .{data_path});
        defer std.testing.allocator.free(artifact_path);
        const artifact_bytes = "same content is not ownership\n";
        const digest = hash_hex(artifact_bytes);
        const manifest_bytes = try std.fmt.allocPrint(
            std.testing.allocator,
            "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:foreign\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}",
            .{ artifact_path, digest },
        );
        defer std.testing.allocator.free(manifest_bytes);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data_dir.fd }};
        var store = try store_t.init(std.testing.allocator, state_dir.fd, &roots);
        defer store.deinit();
        store.fail_after.store(failure_step, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.commit(.{
            .record_id = "desktop:foreign",
            .prior = .Absent,
            .artifact_bytes = artifact_bytes,
            .candidate_manifest = manifest_bytes,
        }));
        store.fail_after.store(0, .monotonic);
        if (case_index == 0) {
            try data_dir.writeFile(.{ .sub_path = "applications/foreign.desktop", .data = artifact_bytes });
            try std.testing.expectError(error.ArtifactConflict, store.recover());
            const retained = try data_dir.readFileAlloc(std.testing.allocator, "applications/foreign.desktop", 4096);
            defer std.testing.allocator.free(retained);
            try std.testing.expectEqualStrings(artifact_bytes, retained);
            try std.testing.expectError(error.FileNotFound, state_dir.access("manifest.json", .{}));
        } else {
            const foreign = "foreign manifest state";
            const fd = c.openat(state_dir.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
            if (fd < 0) return error.IoError;
            defer _ = c.close(fd);
            try write_all(fd, foreign);
            try std.testing.expectError(error.ArtifactConflict, store.recover());
            const retained_artifact = try data_dir.readFileAlloc(std.testing.allocator, "applications/foreign.desktop", 4096);
            defer std.testing.allocator.free(retained_artifact);
            try std.testing.expectEqualStrings(artifact_bytes, retained_artifact);
            const retained_manifest = try state_dir.readFileAlloc(std.testing.allocator, "manifest.json", 4096);
            defer std.testing.allocator.free(retained_manifest);
            try std.testing.expectEqualStrings(foreign, retained_manifest);
        }
    }
}

fn test_record(id: []const u8, path: []const u8, digest: []const u8) manifest.record_t {
    return .{ .id = id, .kind = .DesktopEntry, .path = path, .sha256 = digest, .device = null, .app_key = null, .created_by = "0.2.0", .prior = .None, .mime = null };
}

fn test_manifest_bytes(allocator: std.mem.Allocator, path: []const u8, id: []const u8, bytes: []const u8) ![]u8 {
    const digest = hash_hex(bytes);
    return std.fmt.allocPrint(allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"{s}\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ id, path, digest[0..] });
}

fn replace_bytes_once(allocator: std.mem.Allocator, source: []const u8, needle: []const u8, replacement: []const u8) ![]u8 {
    const index = std.mem.indexOf(u8, source, needle) orelse return error.TestExpectedEqual;
    const result = try allocator.alloc(u8, source.len - needle.len + replacement.len);
    @memcpy(result[0..index], source[0..index]);
    @memcpy(result[index .. index + replacement.len], replacement);
    const suffix_start = index + needle.len;
    @memcpy(result[index + replacement.len ..], source[suffix_start..]);
    return result;
}

fn change_json_value_byte(allocator: std.mem.Allocator, source: []const u8, marker: []const u8, replacement: u8) ![]u8 {
    const start = (std.mem.indexOf(u8, source, marker) orelse return error.TestExpectedEqual) + marker.len;
    if (start >= source.len) return error.TestExpectedEqual;
    const result = try allocator.dupe(u8, source);
    result[start] = replacement;
    return result;
}

test "bounded test mutation helpers reject absent markers and out-of-range writes" {
    try std.testing.expectError(error.TestExpectedEqual, replace_bytes_once(std.testing.allocator, "source", "absent", "x"));
    try std.testing.expectError(error.TestExpectedEqual, change_json_value_byte(std.testing.allocator, "source", "absent", 'x'));
    try std.testing.expectError(error.TestExpectedEqual, change_json_value_byte(std.testing.allocator, "", "", 'x'));
}

test "journal decoder rejects malformed bounded input and unknown duplicate fields" {
    const good = "{\"version\":1,\"transaction_id\":\"0123456789abcdef0123456789abcdef\",\"phase\":\"prepared\",\"record_id\":\"x\",\"root\":\"data\",\"relative_path\":\"applications/x.desktop\",\"old_artifact_hash\":null,\"old_artifact_mode\":0,\"new_artifact_hash\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"new_artifact_mode\":420,\"new_artifact_device\":1,\"new_artifact_inode\":2,\"old_manifest_hash\":null,\"new_manifest_hash\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"new_manifest_device\":1,\"new_manifest_inode\":3}";
    var parsed = try parse_journal(std.testing.allocator, good);
    defer parsed.deinit();
    try std.testing.expectEqual(@as(u32, 1), parsed.value.version);
    try std.testing.expectError(error.RecoveryRequired, parse_journal(std.testing.allocator, ""));
    try std.testing.expectError(error.RecoveryRequired, parse_journal(std.testing.allocator, "{"));
    try std.testing.expectError(error.RecoveryRequired, parse_journal(std.testing.allocator, "{\"unknown\":1}"));
    try std.testing.expectError(error.RecoveryRequired, parse_journal(std.testing.allocator, "{\"version\":1,\"version\":1}"));
}

test "journal header rejects every invalid immutable identity and ownership field" {
    const good = journal_t{
        .transaction_id = "0123456789abcdef0123456789abcdef",
        .phase = "prepared",
        .record_id = "desktop:test",
        .root = "data",
        .relative_path = "applications/test.desktop",
        .old_artifact_hash = null,
        .old_artifact_mode = 0,
        .new_artifact_hash = "0000000000000000000000000000000000000000000000000000000000000000",
        .new_artifact_mode = 0o644,
        .new_artifact_device = 1,
        .new_artifact_inode = 2,
        .old_manifest_hash = null,
        .new_manifest_hash = "1111111111111111111111111111111111111111111111111111111111111111",
        .new_manifest_device = 1,
        .new_manifest_inode = 3,
    };
    try validate_journal_header(good, good.transaction_id);
    var changed = good;
    changed.version = 2;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.version = 3;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(good, "ffffffffffffffffffffffffffffffff"));
    changed = good;
    changed.phase = "phase-name-is-way-too-long";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.record_id = "bad\x00id";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.root = "this-root-name-is-too-long";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    var long_path: [4096]u8 = undefined;
    @memset(&long_path, 'x');
    changed = good;
    changed.relative_path = &long_path;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.new_artifact_hash = "bad";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.new_manifest_hash = "bad";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.old_artifact_hash = good.new_artifact_hash;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.old_artifact_mode = 1;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.new_artifact_device = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.new_artifact_inode = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.new_manifest_device = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.new_manifest_inode = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.old_manifest_hash = "bad";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));

    changed = good;
    changed.version = 2;
    changed.old_artifact_hash = good.new_artifact_hash;
    changed.old_artifact_mode = 0o644;
    changed.old_artifact_device = 1;
    changed.old_artifact_inode = 4;
    changed.old_manifest_hash = good.new_manifest_hash;
    changed.old_manifest_device = 1;
    changed.old_manifest_inode = 6;
    try validate_journal_header(changed, good.transaction_id);
    changed.old_manifest_device = null;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_manifest_device = 1;
    changed.old_manifest_inode = null;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_manifest_inode = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_manifest_inode = 6;
    changed.old_manifest_device = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_manifest_device = 1;
    changed.old_artifact_hash = "bad";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_artifact_hash = good.new_artifact_hash;
    changed.old_artifact_mode = 0o600;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_artifact_mode = 0o644;
    changed.old_artifact_device = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_artifact_device = 1;
    changed.old_artifact_inode = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_artifact_inode = 4;
    changed.old_manifest_hash = null;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_manifest_hash = good.new_manifest_hash;
    changed.old_artifact_inode = null;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.old_artifact_inode = 4;
    changed.backup_hash = good.new_artifact_hash;
    changed.backup_mode = 0o644;
    changed.backup_device = 1;
    changed.backup_inode = 5;
    try validate_journal_header(changed, good.transaction_id);
    changed.backup_hash = "bad";
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_hash = good.new_artifact_hash;
    changed.backup_device = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_device = 1;
    changed.backup_inode = 0;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_inode = 5;
    changed.backup_mode = 0o600;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_hash = null;
    changed.backup_mode = 0;
    changed.backup_device = null;
    changed.backup_inode = null;
    try validate_journal_header(changed, good.transaction_id);
    changed.backup_mode = 0o644;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));

    // Version-one journals predate replacement metadata. Exercise each
    // forbidden v2-only field independently so recovery cannot accept a
    // partially downgraded or mixed-version record.
    changed = good;
    changed.old_artifact_hash = good.new_artifact_hash;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.old_artifact_device = 1;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.old_artifact_inode = 1;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.backup_hash = good.new_artifact_hash;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.backup_mode = 0o644;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.backup_device = 1;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed = good;
    changed.backup_inode = 1;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));

    // A v2 force-backup tuple is all-or-nothing and its mode is restricted to
    // the two supported artifact modes.
    changed = good;
    changed.version = 2;
    changed.old_artifact_hash = good.new_artifact_hash;
    changed.old_artifact_mode = 0o644;
    changed.old_artifact_device = 1;
    changed.old_artifact_inode = 4;
    changed.old_manifest_hash = good.new_manifest_hash;
    changed.old_manifest_device = 1;
    changed.old_manifest_inode = 6;
    changed.backup_hash = good.new_artifact_hash;
    changed.backup_mode = 0o644;
    changed.backup_device = 1;
    changed.backup_inode = 5;
    try validate_journal_header(changed, good.transaction_id);
    changed.backup_hash = null;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_hash = good.new_artifact_hash;
    changed.backup_mode = 0o600;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_mode = 0o644;
    changed.backup_device = null;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_device = 1;
    changed.backup_inode = null;
    try std.testing.expectError(error.RecoveryRequired, validate_journal_header(changed, good.transaction_id));
    changed.backup_hash = null;
    changed.backup_mode = 0;
    changed.backup_device = null;
    changed.backup_inode = null;
    try validate_journal_header(changed, good.transaction_id);
}

test "backup metadata codec rejects each invalid field and references compare every identity" {
    const txid = [_]u8{'a'} ** 32;
    const info = file_info_t{ .hash = hash_bytes("payload"), .mode = 0o644, .device = 7, .inode = 9 };
    const encoded = encode_backup_metadata(txid, "desktop:codec", info);
    const decoded = try decode_backup_metadata(txid, &encoded);
    const expected = backup_reference(txid, "desktop:codec", info);
    try std.testing.expect(same_backup_ref(decoded, expected));
    try std.testing.expectError(error.RecoveryRequired, decode_backup_metadata(txid, encoded[0 .. encoded.len - 1]));
    var changed = encoded;
    changed[0] = 'X';
    try std.testing.expectError(error.RecoveryRequired, decode_backup_metadata(txid, &changed));
    changed = encoded;
    std.mem.writeInt(u32, changed[4..8], 2, .little);
    try std.testing.expectError(error.RecoveryRequired, decode_backup_metadata(txid, &changed));
    changed = encoded;
    changed[8] = 'b';
    try std.testing.expectError(error.RecoveryRequired, decode_backup_metadata(txid, &changed));
    changed = encoded;
    std.mem.writeInt(u32, changed[104..108], 0o600, .little);
    try std.testing.expectError(error.RecoveryRequired, decode_backup_metadata(txid, &changed));
    changed = encoded;
    std.mem.writeInt(u64, changed[108..116], 0, .little);
    try std.testing.expectError(error.RecoveryRequired, decode_backup_metadata(txid, &changed));
    changed = encoded;
    std.mem.writeInt(u64, changed[116..124], 0, .little);
    try std.testing.expectError(error.RecoveryRequired, decode_backup_metadata(txid, &changed));

    var reference = expected;
    reference.transaction_id[0] = 'b';
    try std.testing.expect(!same_backup_ref(expected, reference));
    reference = expected;
    reference.record_id_sha256[0] ^= 1;
    try std.testing.expect(!same_backup_ref(expected, reference));
    reference = expected;
    reference.sha256[0] ^= 1;
    try std.testing.expect(!same_backup_ref(expected, reference));
    reference = expected;
    reference.artifact_mode = 0o755;
    try std.testing.expect(!same_backup_ref(expected, reference));
    reference = expected;
    reference.device += 1;
    try std.testing.expect(!same_backup_ref(expected, reference));
    reference = expected;
    reference.inode += 1;
    try std.testing.expect(!same_backup_ref(expected, reference));
}

test "strict path helpers cover byte limits, component boundaries, and root binding" {
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("/"));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("relative"));
    try std.testing.expectError(error.InvalidPath, validate_absolute_root("/tmp/"));
    try std.testing.expectError(error.InvalidPath, validate_relative("a\x00b"));
    try std.testing.expectError(error.InvalidPath, validate_relative("a/\xff"));
    try std.testing.expectError(error.InvalidPath, validate_relative(&([_]u8{'a'} ** 256)));
    var too_many_components: [129]u8 = undefined;
    for (0..65) |i| {
        too_many_components[i * 2] = 'a';
        if (i < 64) too_many_components[i * 2 + 1] = '/';
    }
    try std.testing.expectError(error.InvalidPath, validate_relative(&too_many_components));
    try std.testing.expect(paths_overlap("/data/a", "/data/a"));
    try std.testing.expect(paths_overlap("/data/a", "/data/a/b"));
    try std.testing.expect(paths_overlap("/data/a/b", "/data/a"));
    try std.testing.expect(!paths_overlap("/data/a", "/data/abc"));
    try std.testing.expect(!path_contains("/data/a", "/data/abc"));
    try std.testing.expect(!valid_record_id(""));
    try std.testing.expect(!valid_record_id("bad\x00id"));
    try std.testing.expect(!valid_record_id("\xff"));
    const long_record_id = [_]u8{'x'} ** (manifest.MaxManifestStringBytes + 1);
    try std.testing.expect(!valid_record_id(&long_record_id));
    try std.testing.expect(valid_record_id("ok:id"));
    try std.testing.expect(!valid_txid("x"));
    try std.testing.expect(!valid_txid("gggggggggggggggggggggggggggggggg"));
    try std.testing.expect(!valid_hash_text("0"));
    try std.testing.expect(!valid_hash_text("z" ** 64));
    try std.testing.expect(cleanup_txid(".cleanup-0123456789abcdef0123456789abcdef") != null);
    try std.testing.expect(cleanup_txid(".cleanup-xyz") == null);
    try std.testing.expect(cleanup_txid(".cleanup-gggggggggggggggggggggggggggggggg") == null);
    try std.testing.expect(first_component("applications/x", "applications"));
    try std.testing.expect(!first_component("applications", "applications"));
    try std.testing.expect(!icon_path_allowed("hicolor/"));
    try std.testing.expect(!icon_path_allowed("hicolor/48x48"));
    try std.testing.expect(!icon_path_allowed("hicolor/48x48/apps"));
    try std.testing.expect(!icon_path_allowed("hicolor//apps/icon.png"));
    try std.testing.expect(!icon_path_allowed("hicolor/48x48/apps/x/y"));
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const absolute = try temporary.dir.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(absolute);
    try validate_root_binding(absolute, temporary.dir.fd);
    try std.testing.expectError(error.InvalidScope, validate_root_binding("/tmp", temporary.dir.fd));
}

test "absolute root and global path validators cover independent scope outcomes" {
    for ([_][]const u8{ "", "/", "relative", "/trailing/", "/a//b", "/a/../b", "/a\x00b" }) |path| {
        try std.testing.expectError(error.InvalidPath, validate_absolute_root(path));
    }
    try validate_absolute_root("/safe/root");
    var overlong_root: [4096]u8 = undefined;
    @memset(&overlong_root, 'x');
    overlong_root[0] = '/';
    try std.testing.expectError(error.InvalidPath, validate_absolute_root(&overlong_root));

    const base = manifest.record_t{
        .id = "desktop:one",
        .kind = .DesktopEntry,
        .path = "/safe/apps/one.desktop",
        .sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
        .device = null,
        .app_key = null,
        .created_by = "0.2.0",
        .prior = .None,
        .mime = null,
    };
    var records = [_]manifest.record_t{ base, base };
    try std.testing.expectError(error.ManifestMismatch, validate_global_paths(&records));
    records[1].path = "/safe/apps/one.desktop/nested";
    try std.testing.expectError(error.ManifestMismatch, validate_global_paths(&records));
    records[1].path = "/safe/apps/one.desktopish";
    try validate_global_paths(&records);
    records[0].path = "relative/path";
    records[1].path = "";
    try validate_global_paths(&records);
}

test "manifest delta and exact record comparisons reject unrelated changes" {
    const old = [_]manifest.record_t{test_record("keep", "/data/applications/b.desktop", "other")};
    const valid = [_]manifest.record_t{ test_record("target", "/data/applications/a.desktop", "new"), test_record("keep", "/data/applications/b.desktop", "other") };
    try validate_delta(old[0..], valid[0..], "target");
    var missing = [_]manifest.record_t{valid[0]};
    try std.testing.expectError(error.ManifestMismatch, validate_delta(old[0..], missing[0..], "target"));
    var altered = valid;
    altered[1].created_by = "other-version";
    try std.testing.expectError(error.ManifestMismatch, validate_delta(old[0..], altered[0..], "target"));
    var changed_identity = valid;
    changed_identity[1].path = "/data/applications/changed.desktop";
    try std.testing.expectError(error.ManifestMismatch, validate_delta(old[0..], changed_identity[0..], "target"));
    try std.testing.expectError(error.ManifestMismatch, validate_delta(old[0..], valid[1..], "target"));
    try std.testing.expectError(error.ManifestMismatch, validate_delta(old[0..], old[0..], "missing"));
    var prior_a = [_]u8{'a'};
    var prior_b = [_]u8{'b'};
    try std.testing.expect(same_prior(.{ .Text = &prior_a }, .{ .Text = &prior_a }));
    try std.testing.expect(!same_prior(.{ .Text = &prior_a }, .{ .Text = &prior_b }));
    try std.testing.expect(same_prior(.{ .U32Value = 7 }, .{ .U32Value = 7 }));
    try std.testing.expect(!same_prior(.{ .U32Value = 7 }, .{ .U32Value = 8 }));
    try std.testing.expect(!same_prior(.None, .{ .Text = "a" }));
    try std.testing.expect(optional_text_equal(null, null));
    try std.testing.expect(!optional_text_equal(null, "x"));
    var optional_x = [_]u8{'x'};
    var optional_y = [_]u8{'y'};
    try std.testing.expect(optional_text_equal(&optional_x, &optional_x));
    try std.testing.expect(!optional_text_equal(&optional_x, &optional_y));
    try std.testing.expect(same_record(old[0], old[0]));
    try std.testing.expect(!same_target_identity(old[0], valid[0]));
    try std.testing.expect(find_record(old[0..], "keep") != null);
    try std.testing.expect(find_record(old[0..], "missing") == null);
    const duplicate_id = [_]manifest.record_t{ test_record("duplicate", "/a", "1"), test_record("duplicate", "/b", "2") };
    try std.testing.expect(find_record(&duplicate_id, "duplicate") == null);
    try std.testing.expectEqualStrings("desktop", root_name(.Desktop));
}

test "record equality checks every optional, identity, hash, and prior field" {
    try std.testing.expect(same_prior(.{ .Text = "same" }, .{ .Text = "same" }));
    try std.testing.expect(same_prior(.{ .U32Value = 9 }, .{ .U32Value = 9 }));
    try std.testing.expect(same_prior(.None, .None));
    try std.testing.expect(!same_prior(.{ .Text = "text" }, .{ .U32Value = 9 }));
    var base = manifest.record_t{
        .id = "id",
        .kind = .DesktopEntry,
        .path = "/data/applications/a.desktop",
        .sha256 = "digest",
        .device = "device",
        .app_key = "app",
        .created_by = "version",
        .prior = .{ .Text = "old" },
        .mime = "text/plain",
    };
    try std.testing.expect(same_record(base, base));
    try std.testing.expect(same_target_identity(base, base));
    var changed = base;
    changed.id = "other";
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed = base;
    changed.kind = .Icon;
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed = base;
    changed.path = "/data/applications/b.desktop";
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed = base;
    changed.sha256 = "other";
    try std.testing.expect(!same_record(base, changed));
    changed = base;
    changed.created_by = "other";
    try std.testing.expect(!same_record(base, changed));
    changed = base;
    changed.device = null;
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed = base;
    changed.app_key = null;
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed = base;
    changed.mime = null;
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed = base;
    changed.prior = .{ .Text = "different" };
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed.prior = .{ .U32Value = 4 };
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    base.prior = .{ .U32Value = 7 };
    changed = base;
    changed.prior = .{ .U32Value = 8 };
    try std.testing.expect(!same_record(base, changed));
    try std.testing.expect(!same_target_identity(base, changed));
    changed.prior = .None;
    try std.testing.expect(!same_record(base, changed));
}

test "init retries after every injected descriptor, lock, and root-walk error" {
    var observed_faults: usize = 0;
    var reached_success: bool = false;
    for (1..32) |fault_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        const state_path = try state.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        injected_io_count = 0;
        injected_io_at = fault_index;
        const result = store_t.init(std.testing.allocator, state.fd, &roots);
        const injected = injected_io_at == 0;
        injected_io_at = 0;
        if (!injected) {
            var store = try result;
            store.deinit();
            reached_success = true;
            break;
        }
        observed_faults += 1;
        if (result) |stored| {
            var store = stored;
            store.deinit();
            return error.InjectedIoFailureWasIgnored;
        } else |err| {
            switch (err) {
                error.IoError, error.AccessDenied, error.InvalidScope => {},
                else => return err,
            }
        }
        // Every error path must have released its lock and retained descriptors.
        var retry = try store_t.init(std.testing.allocator, state.fd, &roots);
        retry.deinit();
    }
    try std.testing.expect(reached_success);
    try std.testing.expect(observed_faults > 0);
}

test "native errno mapping and file info digest validation are deterministic" {
    try std.testing.expectError(error.FileNotFound, native_error_union(c.ENOENT));
    try std.testing.expectError(error.PathAlreadyExists, native_error_union(c.EEXIST));
    try std.testing.expectError(error.AccessDenied, native_error_union(c.EACCES));
    try std.testing.expectError(error.AccessDenied, native_error_union(c.EPERM));
    try std.testing.expectError(error.AccessDenied, native_error_union(c.ELOOP));
    try std.testing.expectError(error.AccessDenied, native_error_union(c.ENOTDIR));
    try std.testing.expectError(error.NoSpaceLeft, native_error_union(c.ENOSPC));
    try std.testing.expectError(error.Interrupted, native_error_union(c.EINTR));
    try std.testing.expectError(error.Busy, native_error_union(c.EBUSY));
    try std.testing.expectError(error.IoError, native_error_union(-1));
    for (1..256) |raw_code| {
        switch (native_error_code(@intCast(raw_code))) {
            error.FileNotFound, error.PathAlreadyExists, error.AccessDenied, error.NoSpaceLeft, error.Interrupted, error.Busy, error.IoError => {},
            else => return error.UnexpectedErrnoMapping,
        }
    }
    const hash = hash_bytes("x");
    const hex = hash_hex("x");
    try std.testing.expect(hash_text_equal(hash, &hex));
    try std.testing.expect(!hash_text_equal(hash, "0000000000000000000000000000000000000000000000000000000000000000"));
    try std.testing.expect(std.mem.eql(u8, &hash_from_text(&hex), &hash));
    try std.testing.expectEqual(@as(u8, 15), hex_digit('F'));
    try std.testing.expectEqual(@as(u8, 3), hex_digit('3'));
    try std.testing.expect(valid_hash_text(&hex));
    try std.testing.expect(same_file_info(.{ .hash = hash, .mode = 0o600, .device = 1, .inode = 2 }, .{ .hash = hash, .mode = 0o600, .device = 1, .inode = 2 }));
    try std.testing.expect(!same_file_info(.{ .hash = hash, .mode = 0o600, .device = 1, .inode = 2 }, .{ .hash = hash, .mode = 0o600, .device = 1, .inode = 3 }));
    try std.testing.expect(!same_file_info(.{ .hash = hash, .mode = 0o600, .device = 1, .inode = 2 }, .{ .hash = hash, .mode = 0o644, .device = 1, .inode = 2 }));
    try std.testing.expect(!same_file_info(.{ .hash = hash, .mode = 0o600, .device = 1, .inode = 2 }, .{ .hash = hash_bytes("different"), .mode = 0o600, .device = 1, .inode = 2 }));
    try std.testing.expect(!valid_hash_text("short"));
    var malformed_hash = hex;
    malformed_hash[0] = 'g';
    try std.testing.expect(!valid_hash_text(&malformed_hash));
    const base_info = file_info_t{ .hash = hash, .mode = 0o600, .device = 1, .inode = 2 };
    var changed_info = base_info;
    changed_info.device += 1;
    try std.testing.expect(!same_file_info(base_info, changed_info));
    const base_identity = identity_from_info(base_info);
    try std.testing.expect(same_identity(base_info, base_identity));
    var changed_identity = base_identity;
    changed_identity.device += 1;
    try std.testing.expect(!same_identity(base_info, changed_identity));
    changed_identity = base_identity;
    changed_identity.inode += 1;
    try std.testing.expect(!same_identity(base_info, changed_identity));
    changed_identity = base_identity;
    changed_identity.mode += 1;
    try std.testing.expect(!same_identity(base_info, changed_identity));
    changed_identity = base_identity;
    changed_identity.sha256[0] ^= 1;
    try std.testing.expect(!same_identity(base_info, changed_identity));
    const txid = [_]u8{'a'} ** 32;
    try std.testing.expectEqualStrings(".cleanup-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", cleanup_name(txid)[0..41]);
    try std.testing.expect(std.mem.startsWith(u8, &artifact_temp_name(txid), ".waddle-"));
    try std.testing.expect(std.mem.startsWith(u8, &manifest_temp_name(txid), ".manifest-"));
    try std.testing.expect(valid_txid("0123456789abcdef0123456789abcdef"));
    try std.testing.expect(!valid_txid("0123456789abcdef0123456789abcdeg"));
    try std.testing.expect(valid_backup_txid("0123456789abcdef0123456789abcdef"));
    try std.testing.expect(!valid_backup_txid("0123456789ABCDEF0123456789ABCDEF"));
    try std.testing.expect(!valid_backup_txid("short"));
    try std.testing.expect(cleanup_txid(".cleanup-0123456789abcdef0123456789abcdef") != null);
    try std.testing.expect(cleanup_txid(".cleanup-invalid") == null);
    try std.testing.expect(valid_record_id("desktop:valid"));
    try std.testing.expect(!valid_record_id(""));
    try std.testing.expect(!valid_hash_text("not-a-hash"));
    try std.testing.expect(icon_path_allowed("hicolor/48x48/apps/a.png"));
    try std.testing.expect(!icon_path_allowed("other/48x48/apps/a.png"));
    try std.testing.expect(!icon_path_allowed("hicolor/48x48/no-apps/a.png"));
    try std.testing.expect(!icon_path_allowed("hicolor/48x48/apps/"));
    try std.testing.expect(!icon_path_allowed("hicolor/48x48/apps/a.png/extra"));
}

test "configured root roles authorize only frozen host artifact path classes" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    try temporary.dir.makeDir("config");
    try temporary.dir.makeDir("desktop");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    var config = try temporary.dir.openDir("config", .{});
    defer config.close();
    var desktop = try temporary.dir.openDir("desktop", .{});
    defer desktop.close();
    const state_path = try state.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const config_path = try config.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(config_path);
    const desktop_path = try desktop.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(desktop_path);
    const roots = [_]root_t{
        .{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd },
        .{ .kind = .Config, .canonical_path = config_path, .dir_fd = config.fd },
        .{ .kind = .Desktop, .canonical_path = desktop_path, .dir_fd = desktop.fd },
    };
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const good = [_]manifest.record_t{
        test_record("desktop", try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/a.desktop", .{data_path}), ""),
        test_record("handler", try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/waddle-open.desktop", .{data_path}), ""),
        .{ .id = "icon", .kind = .Icon, .path = try std.fmt.allocPrint(std.testing.allocator, "{s}/hicolor/48x48/apps/a.png", .{data_path}), .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
        .{ .id = "menu", .kind = .Menu, .path = try std.fmt.allocPrint(std.testing.allocator, "{s}/menus/applications-merged/a.menu", .{config_path}), .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
        .{ .id = "directory", .kind = .Menu, .path = try std.fmt.allocPrint(std.testing.allocator, "{s}/desktop-directories/a.directory", .{data_path}), .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
        .{ .id = "autostart", .kind = .Autostart, .path = try std.fmt.allocPrint(std.testing.allocator, "{s}/autostart/a.desktop", .{config_path}), .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
        .{ .id = "shortcut", .kind = .DesktopShortcut, .path = try std.fmt.allocPrint(std.testing.allocator, "{s}/Shortcut.desktop", .{desktop_path}), .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null },
    };
    defer {
        for (good) |record| std.testing.allocator.free(record.path);
    }
    for (good) |record| {
        const resolved = try store.resolve_allowed(record);
        defer std.testing.allocator.free(resolved.relative);
        try std.testing.expect(resolved.relative.len > 0);
    }
    for ([_][]const u8{ "applications/a.desktop", "hicolor/48x48/apps/a.png", "desktop-directories/a.directory" }) |path| {
        const resolved = try store.resolve_from_journal(store.roots[0], path);
        defer std.testing.allocator.free(resolved);
        try std.testing.expectEqualStrings(path, resolved);
    }
    for ([_][]const u8{ "autostart/a.desktop", "menus/applications-merged/a.menu" }) |path| {
        const resolved = try store.resolve_from_journal(store.roots[1], path);
        defer std.testing.allocator.free(resolved);
        try std.testing.expectEqualStrings(path, resolved);
    }
    const desktop_journal_path = try store.resolve_from_journal(store.roots[2], "Shortcut.desktop");
    defer std.testing.allocator.free(desktop_journal_path);
    try std.testing.expectError(error.RecoveryRequired, store.resolve_from_journal(store.roots[0], "../outside"));
    try std.testing.expectError(error.RecoveryRequired, store.resolve_from_journal(store.roots[0], "autostart/a.desktop"));
    try std.testing.expectError(error.RecoveryRequired, store.resolve_from_journal(store.roots[1], "applications/a.desktop"));
    try std.testing.expectError(error.RecoveryRequired, store.resolve_from_journal(store.roots[2], "nested/Shortcut.desktop"));
    var outside = test_record("outside", "/elsewhere/applications/a.desktop", "");
    try std.testing.expectError(error.InvalidPath, store.resolve_allowed(outside));
    outside.path = data_path;
    try std.testing.expectError(error.InvalidPath, store.resolve_allowed(outside));
    const bad_prefix_path = try std.fmt.allocPrint(std.testing.allocator, "{s}suffix/applications/a.desktop", .{data_path});
    defer std.testing.allocator.free(bad_prefix_path);
    outside.path = bad_prefix_path;
    try std.testing.expectError(error.InvalidPath, store.resolve_allowed(outside));
    outside.path = try std.fmt.allocPrint(std.testing.allocator, "{s}/wrong/a.desktop", .{data_path});
    defer std.testing.allocator.free(outside.path);
    try std.testing.expectError(error.InvalidPath, store.resolve_allowed(outside));
    const malformed_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/a//b.desktop", .{data_path});
    defer std.testing.allocator.free(malformed_path);
    var malformed_record = outside;
    malformed_record.path = malformed_path;
    try std.testing.expectError(error.InvalidPath, store.resolve_allowed(malformed_record));
    outside.kind = .GuestPolicy;
    try std.testing.expectError(error.UnsupportedArtifactKind, store.resolve_allowed(outside));
    outside.kind = .Autostart;
    try std.testing.expectError(error.InvalidPath, store.resolve_allowed(outside));
    const menu_wrong_root = .{ .id = "m", .kind = .Menu, .path = try std.fmt.allocPrint(std.testing.allocator, "{s}/menus/applications-merged/a.menu", .{data_path}), .sha256 = "", .device = null, .app_key = null, .created_by = "", .prior = .None, .mime = null };
    defer std.testing.allocator.free(menu_wrong_root.path);
    try std.testing.expectError(error.InvalidPath, store.resolve_allowed(menu_wrong_root));
    var relative_records = [_]manifest.record_t{test_record("relative", "not-a-canonical-path", "")};
    try validate_global_paths(relative_records[0..]);
    const same_path = [_]manifest.record_t{ test_record("a", "/x", ""), test_record("b", "/x", "") };
    try std.testing.expectError(error.ManifestMismatch, validate_global_paths(&same_path));
    const relative_paths = [_]manifest.record_t{ test_record("empty", "", ""), test_record("r1", "relative/a", ""), test_record("r2", "relative/a", ""), test_record("absolute", "/x", "") };
    try validate_global_paths(&relative_paths);
    const nested_paths = [_]manifest.record_t{ test_record("parent", "/x", ""), test_record("child", "/x/y", "") };
    try std.testing.expectError(error.ManifestMismatch, validate_global_paths(&nested_paths));
}

test "commit validates request, schema, digest, and expected state before staging" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/preflight.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const payload = "preflight payload";
    const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:preflight", payload);
    defer std.testing.allocator.free(candidate);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();

    try std.testing.expectError(error.InvalidArgument, store.commit(.{ .record_id = "", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
    try std.testing.expectError(error.InvalidArgument, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .artifact_mode = 0o666, .candidate_manifest = candidate }));
    try std.testing.expectError(error.InvalidArgument, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = "" }));
    const oversized_artifact = try std.testing.allocator.alloc(u8, MaxArtifactBytes + 1);
    defer std.testing.allocator.free(oversized_artifact);
    try std.testing.expectError(error.InvalidArgument, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = oversized_artifact, .candidate_manifest = candidate }));
    const oversized_manifest = try std.testing.allocator.alloc(u8, manifest.MaxManifestBytes + 1);
    defer std.testing.allocator.free(oversized_manifest);
    try std.testing.expectError(error.InvalidArgument, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = oversized_manifest }));
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "missing", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
    try std.testing.expectError(error.InvalidManifest, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = "{}" }));
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = "changed", .candidate_manifest = candidate }));
    const prior_hash = [_]u8{0} ** 32;
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:preflight", .prior = .{ .Sha256 = prior_hash }, .artifact_bytes = payload, .candidate_manifest = candidate }));
    const extra_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/extra.desktop", .{data_path});
    defer std.testing.allocator.free(extra_path);
    const extra_digest = hash_hex("extra payload");
    const two_distinct_records = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:preflight\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:extra\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ target_path, hash_hex(payload), extra_path, extra_digest });
    defer std.testing.allocator.free(two_distinct_records);
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = two_distinct_records }));
    const same_path_records = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:preflight\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:extra\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ target_path, hash_hex(payload), target_path, extra_digest });
    defer std.testing.allocator.free(same_path_records);
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = same_path_records }));
    const duplicate_old_manifest = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:old-one\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:old-two\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ extra_path, extra_digest, extra_path, extra_digest });
    defer std.testing.allocator.free(duplicate_old_manifest);
    try state.writeFile(.{ .sub_path = "manifest.json", .data = duplicate_old_manifest });
    if (c.fchmodat(state.fd, "manifest.json", 0o600, 0) != 0) return error.IoError;
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
    try state.deleteFile("manifest.json");
    const existing_old_manifest = try test_manifest_bytes(std.testing.allocator, extra_path, "desktop:existing-old", "extra payload");
    defer std.testing.allocator.free(existing_old_manifest);
    try state.writeFile(.{ .sub_path = "manifest.json", .data = existing_old_manifest });
    if (c.fchmodat(state.fd, "manifest.json", 0o600, 0) != 0) return error.IoError;
    const candidate_with_new_record = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:existing-old\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:preflight\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ extra_path, extra_digest, target_path, hash_hex(payload) });
    defer std.testing.allocator.free(candidate_with_new_record);
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:preflight", .prior = .{ .Sha256 = hash_bytes("extra payload") }, .artifact_bytes = payload, .candidate_manifest = candidate_with_new_record }));
    const altered_existing_record = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:existing-old\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.1\",\"prior\":null}},{{\"id\":\"desktop:preflight\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ extra_path, extra_digest, target_path, hash_hex(payload) });
    defer std.testing.allocator.free(altered_existing_record);
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = altered_existing_record }));
    try state.deleteFile("manifest.json");
    try std.testing.expectError(error.UnsupportedReplacement, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate, .backup_modified = true }));
    try data.writeFile(.{ .sub_path = "applications/preflight.desktop", .data = payload });
    try std.testing.expectError(error.ArtifactConflict, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
    try std.testing.expectError(error.FileNotFound, state.access("manifest.json", .{}));
    store.deinit();
    try std.testing.expectError(error.Closed, store.recover());
    try std.testing.expectError(error.Closed, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
}

test "absent-only commit retries each selected real filesystem failure with old manifest present" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const kept_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/kept.desktop", .{data_path});
    defer std.testing.allocator.free(kept_path);
    const added_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/added.desktop", .{data_path});
    defer std.testing.allocator.free(added_path);
    const kept_bytes = "already installed artifact";
    const added_bytes = "fault-tested new artifact";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, kept_path, "desktop:kept", kept_bytes);
    defer std.testing.allocator.free(old_manifest);
    const kept_digest = hash_hex(kept_bytes);
    const added_digest = hash_hex(added_bytes);
    const new_manifest = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:kept\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:added\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ kept_path, kept_digest, added_path, added_digest });
    defer std.testing.allocator.free(new_manifest);
    try data.writeFile(.{ .sub_path = "applications/kept.desktop", .data = kept_bytes });
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();

    var reached_success = false;
    for (1..161) |failure_index| {
        injected_io_count = 0;
        injected_io_at = failure_index;
        const outcome = store.commit(.{ .record_id = "desktop:added", .prior = .Absent, .artifact_bytes = added_bytes, .candidate_manifest = new_manifest });
        const faulted = injected_io_at == 0;
        injected_io_at = 0;
        injected_io_count = 0;
        if (outcome) |_| {
            try std.testing.expect(!faulted);
            reached_success = true;
            break;
        } else |err| {
            try std.testing.expect(faulted);
            try std.testing.expect(err == error.IoError or err == error.AccessDenied or err == error.ArtifactConflict);
        }
        try store.recover();
        const manifest_bytes = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
        defer std.testing.allocator.free(manifest_bytes);
        if (std.mem.eql(u8, manifest_bytes, new_manifest)) {
            const installed = try data.readFileAlloc(std.testing.allocator, "applications/added.desktop", 4096);
            defer std.testing.allocator.free(installed);
            try std.testing.expectEqualStrings(added_bytes, installed);
            reached_success = true;
            break;
        }
        try std.testing.expectEqualStrings(old_manifest, manifest_bytes);
        try std.testing.expectError(error.FileNotFound, data.access("applications/added.desktop", .{}));
        const kept = try data.readFileAlloc(std.testing.allocator, "applications/kept.desktop", 4096);
        defer std.testing.allocator.free(kept);
        try std.testing.expectEqualStrings(kept_bytes, kept);
    }
    try std.testing.expect(reached_success);
}

test "recovery preserves malformed journal and missing staged-byte evidence" {
    for (0..7) |case_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const state_path = try state.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/journal.desktop", .{data_path});
        defer std.testing.allocator.free(target_path);
        const payload = "journal evidence bytes";
        const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:journal", payload);
        defer std.testing.allocator.free(candidate);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(8, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.commit(.{ .record_id = "desktop:journal", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
        store.fail_after.store(0, .monotonic);
        var transactions = try state.openDir("transactions", .{ .iterate = true });
        defer transactions.close();
        var iterator = transactions.iterate();
        const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
        var transaction = try transactions.openDir(entry.name, .{});
        defer transaction.close();
        if (case_index == 0) {
            const bytes = try transaction.readFileAlloc(std.testing.allocator, "journal.json", MaxJournalBytes);
            defer std.testing.allocator.free(bytes);
            const marker = std.mem.indexOf(u8, bytes, "prepared") orelse return error.TestExpectedEqual;
            var tampered = try std.testing.allocator.dupe(u8, bytes);
            defer std.testing.allocator.free(tampered);
            tampered[marker] = 'x';
            try transaction.writeFile(.{ .sub_path = "journal.json", .data = tampered });
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            const retained = try transaction.readFileAlloc(std.testing.allocator, "journal.json", MaxJournalBytes);
            defer std.testing.allocator.free(retained);
            try std.testing.expect(std.mem.indexOf(u8, retained, "xrepared") != null);
        } else if (case_index == 1) {
            try transaction.deleteFile("new-artifact");
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            try transaction.access("journal.json", .{});
        } else if (case_index == 2) {
            try transaction.writeFile(.{ .sub_path = "new-artifact", .data = "tampered staged artifact" });
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            try transaction.access("journal.json", .{});
        } else if (case_index == 3) {
            try transaction.writeFile(.{ .sub_path = "new-manifest", .data = "tampered staged manifest" });
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            try transaction.access("journal.json", .{});
        } else {
            const journal_bytes = try transaction.readFileAlloc(std.testing.allocator, "journal.json", MaxJournalBytes);
            defer std.testing.allocator.free(journal_bytes);
            const marker = if (case_index == 4) "\"new_artifact_hash\":\"" else "\"new_manifest_hash\":\"";
            const changed_byte: u8 = if (journal_bytes[(std.mem.indexOf(u8, journal_bytes, marker) orelse return error.TestExpectedEqual) + marker.len] == 'f') 'e' else 'f';
            const changed_journal = try change_json_value_byte(std.testing.allocator, journal_bytes, marker, changed_byte);
            defer std.testing.allocator.free(changed_journal);
            try transaction.writeFile(.{ .sub_path = "journal.json", .data = changed_journal });
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            try transaction.access("journal.json", .{});
        }
        try std.testing.expectError(error.FileNotFound, data.access("applications/journal.desktop", .{}));
        try std.testing.expectError(error.FileNotFound, state.access("manifest.json", .{}));
    }
}

test "journal manifest validator binds staged bytes record identity root and old-state evidence" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const state_path = try state.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/manifest-validation.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const payload = "validation evidence";
    const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:validation", payload);
    defer std.testing.allocator.free(candidate);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.commit(.{ .record_id = "desktop:validation", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
    store.fail_after.store(0, .monotonic);
    var transactions = try state.openDir("transactions", .{ .iterate = true });
    defer transactions.close();
    var iterator = transactions.iterate();
    const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
    const txid_slice = entry.name[0..32];
    const txid = try txid_array(txid_slice);
    var transaction = try transactions.openDir(entry.name, .{});
    defer transaction.close();
    const bytes = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(bytes);
    var parsed = try parse_journal(std.testing.allocator, bytes);
    defer parsed.deinit();
    try store.validate_journal_manifests(parsed.value, store.roots[0]);

    var invalid_json = parsed.value;
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = "{}" });
    const invalid_json_hash = hash_hex("{}");
    invalid_json.new_manifest_hash = &invalid_json_hash;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid_json, store.roots[0]));

    const wrong_record = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:other", payload);
    defer std.testing.allocator.free(wrong_record);
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = wrong_record });
    var wrong_record_journal = parsed.value;
    const wrong_record_hash = hash_hex(wrong_record);
    wrong_record_journal.new_manifest_hash = &wrong_record_hash;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(wrong_record_journal, store.roots[0]));

    const outside_record = try test_manifest_bytes(std.testing.allocator, "/outside/applications/manifest-validation.desktop", "desktop:validation", payload);
    defer std.testing.allocator.free(outside_record);
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = outside_record });
    var outside_journal = parsed.value;
    const outside_hash = hash_hex(outside_record);
    outside_journal.new_manifest_hash = &outside_hash;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(outside_journal, store.roots[0]));

    const duplicate_candidate = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:validation\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:duplicate\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ target_path, hash_hex(payload), target_path, hash_hex(payload) });
    defer std.testing.allocator.free(duplicate_candidate);
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = duplicate_candidate });
    var duplicate_journal = parsed.value;
    const duplicate_hash = hash_hex(duplicate_candidate);
    duplicate_journal.new_manifest_hash = &duplicate_hash;
    try std.testing.expectError(error.ManifestMismatch, store.validate_journal_manifests(duplicate_journal, store.roots[0]));

    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = candidate });

    var invalid = parsed.value;
    invalid.transaction_id = "bad";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid, store.roots[0]));
    invalid = parsed.value;
    invalid.new_artifact_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid, store.roots[0]));
    invalid = parsed.value;
    invalid.new_manifest_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid, store.roots[0]));
    invalid = parsed.value;
    invalid.record_id = "desktop:missing";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid, store.roots[0]));
    invalid = parsed.value;
    invalid.relative_path = "applications/other.desktop";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid, store.roots[0]));
    invalid = parsed.value;
    invalid.root = "config";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid, store.roots[0]));
    invalid = parsed.value;
    invalid.old_manifest_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(invalid, store.roots[0]));

    const kept_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/kept.desktop", .{data_path});
    defer std.testing.allocator.free(kept_path);
    const kept_digest = hash_hex("kept bytes");
    const old_manifest = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:kept\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ kept_path, kept_digest[0..] });
    defer std.testing.allocator.free(old_manifest);
    const payload_digest = hash_hex(payload);
    const extended_manifest = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:kept\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:validation\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ kept_path, kept_digest[0..], target_path, payload_digest[0..] });
    defer std.testing.allocator.free(extended_manifest);
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = old_manifest });
    if (c.fchmodat(transaction.fd, "old-manifest", 0o600, 0) != 0) return error.IoError;
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = extended_manifest });
    var extended_journal = parsed.value;
    const old_digest = hash_hex(old_manifest);
    const extended_digest = hash_hex(extended_manifest);
    extended_journal.old_manifest_hash = &old_digest;
    extended_journal.new_manifest_hash = &extended_digest;
    try store.validate_journal_manifests(extended_journal, store.roots[0]);
    var bad_old = extended_journal;
    bad_old.old_manifest_hash = &extended_digest;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(bad_old, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = extended_manifest });
    var same_old_new = extended_journal;
    same_old_new.old_manifest_hash = &extended_digest;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(same_old_new, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = "{}" });
    const malformed_old_hash = hash_hex("{}");
    var malformed_old = extended_journal;
    malformed_old.old_manifest_hash = &malformed_old_hash;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(malformed_old, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = old_manifest });
    var missing_old_claim = parsed.value;
    missing_old_claim.old_manifest_hash = null;
    missing_old_claim.new_manifest_hash = &extended_digest;
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = extended_manifest });
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(missing_old_claim, store.roots[0]));

    const existing_target_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:validation", "prior bytes");
    defer std.testing.allocator.free(existing_target_manifest);
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = existing_target_manifest });
    if (c.fchmodat(transaction.fd, "old-manifest", 0o600, 0) != 0) return error.IoError;
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = candidate });
    var replacement_journal = parsed.value;
    const existing_digest = hash_hex(existing_target_manifest);
    replacement_journal.old_manifest_hash = &existing_digest;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(replacement_journal, store.roots[0]));
    try transaction_evidence_exists(&transactions, entry.name);
    try std.testing.expectError(error.FileNotFound, transaction_evidence_exists(&transactions, "missing-transaction"));
    try transaction.deleteFile("journal.json");
    try std.testing.expectError(error.FileNotFound, transaction_evidence_exists(&transactions, entry.name));
    try transaction.writeFile(.{ .sub_path = "journal.json", .data = bytes });
    try transaction.deleteFile("new-artifact");
    try std.testing.expectError(error.FileNotFound, transaction_evidence_exists(&transactions, entry.name));
    try transaction.writeFile(.{ .sub_path = "new-artifact", .data = payload });
    try transaction.deleteFile("new-manifest");
    try std.testing.expectError(error.FileNotFound, transaction_evidence_exists(&transactions, entry.name));
    try transaction.writeFile(.{ .sub_path = "new-manifest", .data = candidate });
    try transaction_evidence_exists(&transactions, entry.name);
    try std.testing.expectError(error.FileNotFound, data.access("applications/manifest-validation.desktop", .{}));
    try std.testing.expectError(error.FileNotFound, state.access("manifest.json", .{}));
}

test "replacement journal validator checks version two old artifact and retained backup evidence" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/v2-validation.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const recorded = "recorded bytes";
    const modified = "modified prior bytes";
    const replacement = "replacement bytes";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:v2-validation", recorded);
    defer std.testing.allocator.free(old_manifest);
    const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:v2-validation", replacement);
    defer std.testing.allocator.free(new_manifest);
    try data.writeFile(.{ .sub_path = "applications/v2-validation.desktop", .data = modified });
    if (c.fchmodat(data.fd, "applications/v2-validation.desktop", 0o644, 0) != 0) return error.IoError;
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const expected = identity_from_info((try store.inspect(data.fd, "applications/v2-validation.desktop")).?);
    store.fail_after.store(12, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.replace(.{
        .record_id = "desktop:v2-validation",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = expected,
        .artifact_bytes = replacement,
        .candidate_manifest = new_manifest,
        .backup_modified = true,
    }));
    store.fail_after.store(0, .monotonic);

    var transactions = try state.openDir("transactions", .{ .iterate = true });
    defer transactions.close();
    var iterator = transactions.iterate();
    const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
    const txid = try txid_array(entry.name);
    const journal_bytes = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_bytes);
    var parsed = try parse_journal(std.testing.allocator, journal_bytes);
    defer parsed.deinit();
    try std.testing.expectEqual(@as(u32, 2), parsed.value.version);
    try store.validate_journal_manifests(parsed.value, store.roots[0]);
    defer {
        injected_renameat2_at = 0;
        injected_renameat2_errno_at = 0;
        injected_renameat2_errno = 0;
        injected_renameat2_count = 0;
    }
    try store.rollback_prepared_replace(parsed.value);
    for ([_]c_int{ c.EOPNOTSUPP, c.ENOSYS, c.EINVAL }) |unsupported_errno| {
        injected_renameat2_count = 0;
        injected_renameat2_errno = unsupported_errno;
        injected_renameat2_errno_at = 2; // Artifact exchange succeeds; manifest exchange is refused.
        try std.testing.expectError(error.UnsupportedAtomicReplace, store.apply_prepared_replace(parsed.value));
        try std.testing.expectEqual(@as(usize, 0), injected_renameat2_errno_at);
        const still_old_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
        defer std.testing.allocator.free(still_old_manifest);
        try std.testing.expectEqualStrings(old_manifest, still_old_manifest);
        const published_artifact = try data.readFileAlloc(std.testing.allocator, "applications/v2-validation.desktop", MaxArtifactBytes);
        defer std.testing.allocator.free(published_artifact);
        try std.testing.expectEqualStrings(replacement, published_artifact);
        try store.rollback_prepared_replace(parsed.value);
        const restored_artifact = try data.readFileAlloc(std.testing.allocator, "applications/v2-validation.desktop", MaxArtifactBytes);
        defer std.testing.allocator.free(restored_artifact);
        try std.testing.expectEqualStrings(modified, restored_artifact);
    }
    // Resume the real PREPARED transaction twice to exercise its idempotent
    // publication path before testing exact rollback of both exchanged files.
    try store.apply_prepared_replace(parsed.value);
    try store.apply_prepared_replace(parsed.value);
    if (c.renameat(data.fd, "applications/v2-validation.desktop", data.fd, "held-v2-validation") != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.rollback_prepared_replace(parsed.value));
    if (c.renameat(data.fd, "held-v2-validation", data.fd, "applications/v2-validation.desktop") != 0) return error.IoError;
    for (1..48) |failure_index| {
        injected_io_at = failure_index;
        injected_io_count = 0;
        const attempt = store.rollback_prepared_replace(parsed.value);
        injected_io_at = 0;
        injected_io_count = 0;
        if (attempt) |_| {} else |err| {
            try std.testing.expect(err == error.IoError or err == error.ArtifactConflict or err == error.RecoveryRequired or err == error.AccessDenied);
        }
        try store.apply_prepared_replace(parsed.value);
    }
    const target_parent = try store.open_parent(data.fd, parsed.value.relative_path);
    defer _ = c.close(target_parent.fd);
    const target_leaf = try std.testing.allocator.dupeZ(u8, target_parent.leaf);
    defer std.testing.allocator.free(target_leaf);
    const displaced_name = artifact_temp_name(txid);
    if (c.renameat(target_parent.fd, &displaced_name, target_parent.fd, "held-displaced") != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared_replace(parsed.value));
    if (c.renameat(target_parent.fd, "held-displaced", target_parent.fd, &displaced_name) != 0) return error.IoError;
    const wrong_displaced_fd = c.openat(target_parent.fd, &displaced_name, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (wrong_displaced_fd < 0) return error.IoError;
    try expect_rewrite_file(wrong_displaced_fd, "tampered prepared displaced inode");
    _ = c.close(wrong_displaced_fd);
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared_replace(parsed.value));
    const reset_displaced_fd = c.openat(target_parent.fd, &displaced_name, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (reset_displaced_fd < 0) return error.IoError;
    try expect_rewrite_file(reset_displaced_fd, modified);
    _ = c.close(reset_displaced_fd);
    if (c.renameat(state.fd, "manifest.json", state.fd, "held-manifest") != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared_replace(parsed.value));
    if (c.renameat(state.fd, "held-manifest", state.fd, "manifest.json") != 0) return error.IoError;
    const wrong_target_fd = c.openat(target_parent.fd, target_leaf, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (wrong_target_fd < 0) return error.IoError;
    try expect_rewrite_file(wrong_target_fd, "tampered prepared target");
    _ = c.close(wrong_target_fd);
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared_replace(parsed.value));
    const reset_target_fd = c.openat(target_parent.fd, target_leaf, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (reset_target_fd < 0) return error.IoError;
    try expect_rewrite_file(reset_target_fd, replacement);
    _ = c.close(reset_target_fd);
    try store.apply_prepared_replace(parsed.value);
    if (c.renameat(target_parent.fd, &displaced_name, target_parent.fd, "held-displaced") != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.rollback_prepared_replace(parsed.value));
    if (c.renameat(target_parent.fd, "held-displaced", target_parent.fd, &displaced_name) != 0) return error.IoError;
    if (c.renameat(state.fd, "manifest.json", state.fd, "held-manifest") != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.rollback_prepared_replace(parsed.value));
    if (c.renameat(state.fd, "held-manifest", state.fd, "manifest.json") != 0) return error.IoError;
    const old_state_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (old_state_fd < 0) return error.IoError;
    try expect_rewrite_file(old_state_fd, old_manifest);
    _ = c.close(old_state_fd);
    try std.testing.expectError(error.ArtifactConflict, store.rollback_prepared_replace(parsed.value));
    const reset_manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (reset_manifest_fd < 0) return error.IoError;
    try expect_rewrite_file(reset_manifest_fd, new_manifest);
    _ = c.close(reset_manifest_fd);
    if (c.renameat(state.fd, "manifest.json", state.fd, "held-manifest") != 0) return error.IoError;
    const foreign_manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (foreign_manifest_fd < 0) return error.IoError;
    try write_all(foreign_manifest_fd, old_manifest);
    if (c.fsync(foreign_manifest_fd) != 0 or c.close(foreign_manifest_fd) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.rollback_prepared_replace(parsed.value));
    const untouched_new_artifact = try data.readFileAlloc(std.testing.allocator, "applications/v2-validation.desktop", 4096);
    defer std.testing.allocator.free(untouched_new_artifact);
    try std.testing.expectEqualStrings(replacement, untouched_new_artifact);
    if (c.unlinkat(state.fd, "manifest.json", 0) != 0 or c.renameat(state.fd, "held-manifest", state.fd, "manifest.json") != 0) return error.IoError;
    try store.rollback_prepared_replace(parsed.value);
    try store.apply_prepared_replace(parsed.value);
    const displaced_fd = c.openat(target_parent.fd, &displaced_name, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (displaced_fd < 0) return error.IoError;
    try expect_rewrite_file(displaced_fd, "tampered displaced inode");
    _ = c.close(displaced_fd);
    try std.testing.expectError(error.ArtifactConflict, store.rollback_prepared_replace(parsed.value));
    const restored_displaced_fd = c.openat(target_parent.fd, &displaced_name, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (restored_displaced_fd < 0) return error.IoError;
    try expect_rewrite_file(restored_displaced_fd, modified);
    _ = c.close(restored_displaced_fd);
    const current_manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (current_manifest_fd < 0) return error.IoError;
    try expect_rewrite_file(current_manifest_fd, "foreign manifest bytes");
    _ = c.close(current_manifest_fd);
    try std.testing.expectError(error.ArtifactConflict, store.rollback_prepared_replace(parsed.value));
    const restored_manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (restored_manifest_fd < 0) return error.IoError;
    try expect_rewrite_file(restored_manifest_fd, new_manifest);
    _ = c.close(restored_manifest_fd);
    try store.rollback_prepared_replace(parsed.value);
    try store.rollback_prepared_replace(parsed.value);
    const rolled_back_bytes = try data.readFileAlloc(std.testing.allocator, "applications/v2-validation.desktop", 4096);
    defer std.testing.allocator.free(rolled_back_bytes);
    try std.testing.expectEqualStrings(modified, rolled_back_bytes);
    const rolled_back_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(rolled_back_manifest);
    try std.testing.expectEqualStrings(old_manifest, rolled_back_manifest);

    var transaction = try transactions.openDir(entry.name, .{});
    defer transaction.close();
    var changed = parsed.value;
    changed.root = "config";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.relative_path = "applications/other.desktop";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.new_artifact_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.old_artifact_device = null;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.old_artifact_inode = null;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.backup_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    try std.testing.expectError(error.ArtifactConflict, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.backup_device = 0;
    try std.testing.expectError(error.ArtifactConflict, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.backup_inode = 0;
    try std.testing.expectError(error.ArtifactConflict, store.validate_journal_manifests(changed, store.roots[0]));

    // A journal may not claim the just-published manifest as its old image,
    // even when the named old-manifest bytes have a matching digest.
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = new_manifest });
    changed = parsed.value;
    const candidate_digest = hash_hex(new_manifest);
    changed.old_manifest_hash = &candidate_digest;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = old_manifest });

    changed = parsed.value;
    changed.old_artifact_mode = 0o600;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.old_artifact_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    changed = parsed.value;
    changed.backup_hash = null;
    changed.backup_mode = 0;
    changed.backup_device = null;
    changed.backup_inode = null;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));

    try transaction.writeFile(.{ .sub_path = "old-artifact", .data = "tampered bytes" });
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(parsed.value, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-artifact", .data = modified });
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = new_manifest });
    changed = parsed.value;
    const new_digest = hash_hex(new_manifest);
    changed.old_manifest_hash = &new_digest;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = old_manifest });
    changed = parsed.value;
    changed.old_manifest_hash = null;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = old_manifest });
    try transaction.deleteFile("old-manifest");
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(parsed.value, store.roots[0]));
    const malformed_old_manifest = "not valid JSON";
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = malformed_old_manifest });
    if (c.fchmodat(transaction.fd, "old-manifest", 0o600, 0) != 0) return error.IoError;
    changed = parsed.value;
    const malformed_old_hash = hash_hex(malformed_old_manifest);
    changed.old_manifest_hash = &malformed_old_hash;
    try std.testing.expectError(error.RecoveryRequired, store.validate_journal_manifests(changed, store.roots[0]));
    try transaction.writeFile(.{ .sub_path = "old-manifest", .data = old_manifest });
    if (c.fchmodat(transaction.fd, "old-manifest", 0o600, 0) != 0) return error.IoError;

    // Corrupt the durable v2 journal one identity field at a time, asking the
    // normal startup recovery path to reject each state while preserving the
    // journal and transaction evidence. Restore the exact trusted bytes
    // between attempts so no mutation is based on a previous malformed one.
    var corruptions = [_]journal_t{parsed.value} ** 12;
    corruptions[0].old_artifact_hash = "bad";
    corruptions[1].old_artifact_mode = 0o600;
    corruptions[2].old_artifact_device = 0;
    corruptions[3].old_artifact_inode = 0;
    corruptions[4].old_manifest_hash = null;
    corruptions[5].backup_hash = null;
    corruptions[6].backup_hash = "bad";
    corruptions[7].backup_mode = 0o600;
    corruptions[8].backup_device = 0;
    corruptions[9].backup_inode = 0;
    corruptions[10].new_artifact_device = 0;
    corruptions[11].new_manifest_inode = 0;
    for (corruptions) |corrupted| {
        const malformed = try std.json.stringifyAlloc(std.testing.allocator, corrupted, .{});
        defer std.testing.allocator.free(malformed);
        try transaction.writeFile(.{ .sub_path = "journal.json", .data = malformed });
        try std.testing.expectError(error.RecoveryRequired, store.recover());
        try transaction.access("journal.json", .{});
        try transaction.writeFile(.{ .sub_path = "journal.json", .data = journal_bytes });
    }
    try store.recover();
}

fn transaction_evidence_exists(transactions: *std.fs.Dir, name: []const u8) !void {
    var transaction = try transactions.openDir(name, .{});
    defer transaction.close();
    try transaction.access("journal.json", .{});
    try transaction.access("new-artifact", .{});
    try transaction.access("new-manifest", .{});
}

test "replacement request guards reject stale, retargeted, malformed, and unforced states" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/request-guards.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const other_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/other.desktop", .{data_path});
    defer std.testing.allocator.free(other_path);
    const original = "original request-guard bytes";
    const edited = "externally edited request-guard bytes";
    const replacement = "replacement request-guard bytes";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:request-guards", original);
    defer std.testing.allocator.free(old_manifest);
    const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:request-guards", replacement);
    defer std.testing.allocator.free(new_manifest);
    const wrong_target_manifest = try test_manifest_bytes(std.testing.allocator, other_path, "desktop:request-guards", replacement);
    defer std.testing.allocator.free(wrong_target_manifest);
    const absent_record_manifest = try test_manifest_bytes(std.testing.allocator, other_path, "desktop:other", replacement);
    defer std.testing.allocator.free(absent_record_manifest);
    const payload_digest = hash_hex(replacement);
    const extra_manifest = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:request-guards\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:extra\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ target_path, payload_digest, other_path, hash_hex("extra") });
    defer std.testing.allocator.free(extra_manifest);
    try data.writeFile(.{ .sub_path = "applications/request-guards.desktop", .data = original });
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    defer _ = c.close(manifest_fd);
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const initial = try store.inspect_record("desktop:request-guards");
    const request = replace_request_t{
        .record_id = "desktop:request-guards",
        .expected_manifest_sha256 = initial.manifest_sha256,
        .expected_target = initial.target,
        .artifact_bytes = replacement,
        .candidate_manifest = new_manifest,
    };

    var invalid = request;
    invalid.record_id = "";
    try std.testing.expectError(error.InvalidArgument, store.replace(invalid));
    invalid = request;
    invalid.candidate_manifest = "";
    try std.testing.expectError(error.InvalidArgument, store.replace(invalid));
    const oversized_artifact = try std.testing.allocator.alloc(u8, MaxArtifactBytes + 1);
    defer std.testing.allocator.free(oversized_artifact);
    invalid = request;
    invalid.artifact_bytes = oversized_artifact;
    try std.testing.expectError(error.InvalidArgument, store.replace(invalid));
    const oversized_manifest = try std.testing.allocator.alloc(u8, manifest.MaxManifestBytes + 1);
    defer std.testing.allocator.free(oversized_manifest);
    invalid = request;
    invalid.candidate_manifest = oversized_manifest;
    try std.testing.expectError(error.InvalidArgument, store.replace(invalid));
    invalid = request;
    invalid.artifact_mode = 0o600;
    try std.testing.expectError(error.InvalidArgument, store.replace(invalid));
    invalid = request;
    invalid.expected_manifest_sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.replace(invalid));
    invalid = request;
    invalid.expected_target.inode +%= 1;
    try std.testing.expectError(error.ArtifactConflict, store.replace(invalid));
    invalid = request;
    invalid.expected_target.device +%= 1;
    try std.testing.expectError(error.ArtifactConflict, store.replace(invalid));
    invalid = request;
    invalid.expected_target.mode ^= 0o111;
    try std.testing.expectError(error.ArtifactConflict, store.replace(invalid));
    invalid = request;
    invalid.expected_target.sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.replace(invalid));
    const duplicate_old = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:request-guards\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:duplicate\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ target_path, hash_hex(original), target_path, hash_hex(original) });
    defer std.testing.allocator.free(duplicate_old);
    try state.writeFile(.{ .sub_path = "manifest.json", .data = duplicate_old });
    invalid = request;
    invalid.expected_manifest_sha256 = hash_bytes(duplicate_old);
    try std.testing.expectError(error.ManifestMismatch, store.replace(invalid));
    try state.writeFile(.{ .sub_path = "manifest.json", .data = old_manifest });
    invalid = request;
    invalid.artifact_bytes = "bytes do not match candidate digest";
    try std.testing.expectError(error.ManifestMismatch, store.replace(invalid));
    invalid = request;
    invalid.candidate_manifest = wrong_target_manifest;
    try std.testing.expectError(error.ManifestMismatch, store.replace(invalid));
    invalid = request;
    invalid.candidate_manifest = absent_record_manifest;
    invalid.record_id = "desktop:missing";
    try std.testing.expectError(error.ManifestMismatch, store.replace(invalid));
    invalid = request;
    invalid.record_id = "desktop:other";
    invalid.candidate_manifest = absent_record_manifest;
    try std.testing.expectError(error.UnsupportedReplacement, store.replace(invalid));
    invalid = request;
    invalid.candidate_manifest = extra_manifest;
    try std.testing.expectError(error.ManifestMismatch, store.replace(invalid));

    const old_with_unrelated = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:request-guards\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:extra\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ target_path, hash_hex(original), other_path, hash_hex("extra") });
    defer std.testing.allocator.free(old_with_unrelated);
    const changed_unrelated = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:request-guards\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:extra\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.1\",\"prior\":null}}]}}", .{ target_path, hash_hex(replacement), other_path, hash_hex("extra") });
    defer std.testing.allocator.free(changed_unrelated);
    try state.writeFile(.{ .sub_path = "manifest.json", .data = old_with_unrelated });
    const two_record_snapshot = try store.inspect_record("desktop:request-guards");
    invalid = request;
    invalid.expected_manifest_sha256 = two_record_snapshot.manifest_sha256;
    invalid.expected_target = two_record_snapshot.target;
    invalid.candidate_manifest = changed_unrelated;
    try std.testing.expectError(error.ManifestMismatch, store.replace(invalid));
    try state.writeFile(.{ .sub_path = "manifest.json", .data = old_manifest });

    const target_fd = c.openat(data.fd, "applications/request-guards.desktop", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (target_fd < 0) return error.IoError;
    defer _ = c.close(target_fd);
    if (c.ftruncate(target_fd, 0) != 0) return error.IoError;
    try write_all(target_fd, edited);
    if (c.fchmod(target_fd, 0o644) != 0 or c.fsync(target_fd) != 0) return error.IoError;
    const edited_snapshot = try store.inspect_record("desktop:request-guards");
    invalid = request;
    invalid.expected_target = edited_snapshot.target;
    try std.testing.expectError(error.UnsupportedReplacement, store.replace(invalid));
    if (c.fchmodat(data.fd, "applications/request-guards.desktop", 0o4755, 0) != 0) return error.IoError;
    try std.testing.expectError(error.AccessDenied, store.inspect_record("desktop:request-guards"));
    if (c.fchmodat(data.fd, "applications/request-guards.desktop", 0o644, 0) != 0) return error.IoError;
}

test "unmodified replacement publishes without allocating a backup" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/unmodified.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const original = "unmodified managed payload";
    const replacement = "new expected payload";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:unmodified", original);
    defer std.testing.allocator.free(old_manifest);
    const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:unmodified", replacement);
    defer std.testing.allocator.free(new_manifest);
    try data.writeFile(.{ .sub_path = "applications/unmodified.desktop", .data = original });
    if (c.fchmodat(data.fd, "applications/unmodified.desktop", 0o644, 0) != 0) return error.IoError;
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    defer _ = c.close(manifest_fd);
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const snapshot = try store.inspect_record("desktop:unmodified");
    const before_no_op = store.checkpoint_count.load(.monotonic);
    store.fail_after.store(before_no_op + 1, .monotonic);
    const no_op = try store.replace(.{
        .record_id = "desktop:unmodified",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = original,
        .candidate_manifest = old_manifest,
    });
    store.fail_after.store(0, .monotonic);
    try std.testing.expectEqual(before_no_op, store.checkpoint_count.load(.monotonic));
    try std.testing.expectEqual(snapshot.target, no_op.target);
    try std.testing.expect(no_op.backup == null);
    try std.testing.expectError(error.UnsupportedReplacement, store.replace(.{
        .record_id = "desktop:unmodified",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = original,
        .artifact_mode = 0o755,
        .candidate_manifest = old_manifest,
    }));
    try std.testing.expectError(error.ManifestMismatch, store.replace(.{
        .record_id = "desktop:unmodified",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = replacement,
        .candidate_manifest = old_manifest,
    }));
    const receipt = try store.replace(.{
        .record_id = "desktop:unmodified",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = replacement,
        .candidate_manifest = new_manifest,
    });
    try std.testing.expect(receipt.backup == null);
    try std.testing.expectEqual(hash_bytes(replacement), receipt.target.sha256);
    var backups = try store.list_backups(std.testing.allocator, "desktop:unmodified");
    defer backups.deinit();
    try std.testing.expectEqual(@as(usize, 0), backups.items.len);
}

test "committed replacement reapplies durable file and directory syncs after interruption" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/committed-resync.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const recorded = "recorded committed-resync bytes";
    const modified = "modified committed-resync bytes";
    const replacement = "new committed-resync bytes";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:committed-resync", recorded);
    defer std.testing.allocator.free(old_manifest);
    const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:committed-resync", replacement);
    defer std.testing.allocator.free(new_manifest);
    try data.writeFile(.{ .sub_path = "applications/committed-resync.desktop", .data = modified });
    if (c.fchmodat(data.fd, "applications/committed-resync.desktop", 0o644, 0) != 0) return error.IoError;
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const expected = identity_from_info((try store.inspect(data.fd, "applications/committed-resync.desktop")).?);
    store.fail_after.store(17, .monotonic);
    const replace_outcome = store.replace(.{
        .record_id = "desktop:committed-resync",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = expected,
        .artifact_bytes = replacement,
        .candidate_manifest = new_manifest,
        .backup_modified = true,
    });
    if (replace_outcome) |_| {
        return error.TestFailure;
    } else |err| {
        try std.testing.expectEqual(error.InjectedFailure, err);
    }
    store.fail_after.store(0, .monotonic);
    var transactions = try state.openDir("transactions", .{ .iterate = true });
    defer transactions.close();
    var iterator = transactions.iterate();
    var tx_name: ?[]const u8 = null;
    while (try iterator.next()) |entry| {
        if (valid_txid(entry.name)) {
            tx_name = entry.name;
            break;
        }
    }
    const selected_name = tx_name orelse return error.TestExpectedEqual;
    const txid = try txid_array(selected_name);
    var txid_z: [33]u8 = undefined;
    @memcpy(txid_z[0..32], &txid);
    txid_z[32] = 0;
    const bytes = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(bytes);
    var parsed = try parse_journal(std.testing.allocator, bytes);
    defer parsed.deinit();
    try std.testing.expectEqualStrings("committed", parsed.value.phase);
    var invalid_journal = parsed.value;
    invalid_journal.root = "config";
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(invalid_journal));
    invalid_journal = parsed.value;
    invalid_journal.relative_path = "applications/missing.desktop";
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(invalid_journal));
    invalid_journal = parsed.value;
    invalid_journal.new_artifact_inode +%= 1;
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(invalid_journal));
    invalid_journal = parsed.value;
    invalid_journal.new_manifest_inode +%= 1;
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(invalid_journal));
    invalid_journal = parsed.value;
    invalid_journal.transaction_id = "00000000000000000000000000000000";
    try std.testing.expectError(error.ArtifactConflict, store.apply_committed_replace(invalid_journal));
    const parent = try store.open_parent(data.fd, parsed.value.relative_path);
    defer _ = c.close(parent.fd);
    const displaced = artifact_temp_name(txid);
    const displaced_fd = c.openat(parent.fd, &displaced, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (displaced_fd < 0) return error.IoError;
    try expect_rewrite_file(displaced_fd, "tampered displaced committed inode");
    _ = c.close(displaced_fd);
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(parsed.value));
    const restored_displaced_fd = c.openat(parent.fd, &displaced, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (restored_displaced_fd < 0) return error.IoError;
    try expect_rewrite_file(restored_displaced_fd, modified);
    _ = c.close(restored_displaced_fd);
    if (c.renameat(state.fd, "manifest.json", state.fd, "held-manifest") != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(parsed.value));
    if (c.renameat(state.fd, "held-manifest", state.fd, "manifest.json") != 0) return error.IoError;
    var committed_transaction = try transactions.openDir(selected_name, .{});
    defer committed_transaction.close();
    if (c.renameat(transactions.fd, &txid_z, transactions.fd, "held-transaction") != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(parsed.value));
    if (c.renameat(transactions.fd, "held-transaction", transactions.fd, &txid_z) != 0) return error.IoError;
    if (c.renameat(committed_transaction.fd, "journal.json", committed_transaction.fd, "held-journal") != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, store.apply_committed_replace(parsed.value));
    if (c.renameat(committed_transaction.fd, "held-journal", committed_transaction.fd, "journal.json") != 0) return error.IoError;
    for (1..40) |failure_index| {
        injected_io_at = failure_index;
        injected_io_count = 0;
        const attempt = store.apply_committed_replace(parsed.value);
        injected_io_at = 0;
        injected_io_count = 0;
        if (attempt) |_| {} else |err| {
            try std.testing.expect(err == error.IoError or err == error.RecoveryRequired or err == error.AccessDenied or err == error.ArtifactConflict);
        }
    }
    try store.apply_committed_replace(parsed.value);
    try store.apply_committed_replace(parsed.value);
    try store.recover();
    const installed = try data.readFileAlloc(std.testing.allocator, "applications/committed-resync.desktop", 4096);
    defer std.testing.allocator.free(installed);
    try std.testing.expectEqualStrings(replacement, installed);
}

test "recovery validates every journal identity and phase field before rollback" {
    const tampering = [_][2][]const u8{
        .{ "\"version\":1", "\"version\":2" },
        .{ "\"transaction_id\":\"", "\"transaction_id\":\"f" },
        .{ "\"phase\":\"prepared\"", "\"phase\":\"phase-name-exceeds-limit\"" },
        .{ "\"record_id\":\"desktop:journal\"", "\"record_id\":\"\"" },
        .{ "\"root\":\"data\"", "\"root\":\"unknown\"" },
        .{ "\"relative_path\":\"applications/journal.desktop\"", "\"relative_path\":\"../outside\"" },
        .{ "\"relative_path\":\"applications/journal.desktop\"", "\"relative_path\":\"applications/other.desktop\"" },
        .{ "\"old_artifact_hash\":null", "\"old_artifact_hash\":\"0000000000000000000000000000000000000000000000000000000000000000\"" },
        .{ "\"old_artifact_mode\":0", "\"old_artifact_mode\":1" },
        .{ "\"new_artifact_device\":", "\"new_artifact_device\":0," },
        .{ "\"new_artifact_inode\":", "\"new_artifact_inode\":0," },
        .{ "\"old_manifest_hash\":null", "\"old_manifest_hash\":\"0000000000000000000000000000000000000000000000000000000000000000\"" },
    };
    for (tampering) |change| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const state_path = try state.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/journal.desktop", .{data_path});
        defer std.testing.allocator.free(target_path);
        const payload = "journal identity bytes";
        const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:journal", payload);
        defer std.testing.allocator.free(candidate);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(8, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.commit(.{ .record_id = "desktop:journal", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
        store.fail_after.store(0, .monotonic);
        var transactions = try state.openDir("transactions", .{ .iterate = true });
        defer transactions.close();
        var iterator = transactions.iterate();
        const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
        var transaction = try transactions.openDir(entry.name, .{});
        defer transaction.close();
        const bytes = try transaction.readFileAlloc(std.testing.allocator, "journal.json", MaxJournalBytes);
        defer std.testing.allocator.free(bytes);
        const changed = try replace_bytes_once(std.testing.allocator, bytes, change[0], change[1]);
        defer std.testing.allocator.free(changed);
        try transaction.writeFile(.{ .sub_path = "journal.json", .data = changed });
        try std.testing.expectError(error.RecoveryRequired, store.recover());
        try transaction.access("journal.json", .{});
        try std.testing.expectError(error.FileNotFound, data.access("applications/journal.desktop", .{}));
    }
}

test "adding a second manifest record preserves unrelated records and restores prior bytes" {
    for (0..9) |case_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const old_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/kept.desktop", .{data_path});
        defer std.testing.allocator.free(old_path);
        const new_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/added.desktop", .{data_path});
        defer std.testing.allocator.free(new_path);
        const old_payload = "kept artifact";
        const new_payload = "added artifact";
        const old_digest = hash_hex(old_payload);
        const old_manifest = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"desktop:kept\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ old_path, old_digest });
        defer std.testing.allocator.free(old_manifest);
        const new_manifest = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:01Z\",\"records\":[{{\"id\":\"desktop:kept\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}},{{\"id\":\"desktop:added\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ old_path, old_digest, new_path, hash_hex(new_payload) });
        defer std.testing.allocator.free(new_manifest);
        try data.writeFile(.{ .sub_path = "applications/kept.desktop", .data = old_payload });
        const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (manifest_fd < 0) return error.IoError;
        defer _ = c.close(manifest_fd);
        try write_all(manifest_fd, old_manifest);
        if (c.fsync(manifest_fd) != 0) return error.IoError;
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();
        const failure_step = if (case_index == 7 or case_index == 8) 9 else case_index + 7;
        if (case_index != 0) store.fail_after.store(failure_step, .monotonic);
        const committed = store.commit(.{ .record_id = "desktop:added", .prior = .Absent, .artifact_bytes = new_payload, .candidate_manifest = new_manifest });
        if (case_index != 0) {
            try std.testing.expectError(error.InjectedFailure, committed);
            store.fail_after.store(0, .monotonic);
            if (case_index == 8) {
                var transactions = try state.openDir("transactions", .{ .iterate = true });
                defer transactions.close();
                var iterator = transactions.iterate();
                const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
                const txid = try txid_array(entry.name);
                var transaction = try transactions.openDir(entry.name, .{});
                defer transaction.close();
                const original_journal = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
                defer std.testing.allocator.free(original_journal);
                var parsed = try parse_journal(std.testing.allocator, original_journal);
                defer parsed.deinit();
                try std.testing.expectEqual(@as(u32, 1), parsed.value.version);
                try store.apply_prepared(parsed.value);
                parsed.value.version = 1;
                parsed.value.old_artifact_hash = null;
                parsed.value.old_artifact_mode = 0;
                parsed.value.old_artifact_device = null;
                parsed.value.old_artifact_inode = null;
                parsed.value.backup_hash = null;
                parsed.value.backup_mode = 0;
                parsed.value.backup_device = null;
                parsed.value.backup_inode = null;
                parsed.value.old_manifest_device = null;
                parsed.value.old_manifest_inode = null;
                const legacy_journal = try std.json.stringifyAlloc(std.testing.allocator, parsed.value, .{});
                defer std.testing.allocator.free(legacy_journal);
                try transaction.writeFile(.{ .sub_path = "journal.json", .data = legacy_journal });
                const manifest_temp = manifest_temp_name(txid);
                try state.deleteFile(manifest_temp[0 .. ".manifest-".len + 32 + ".tmp".len]);
                try store.recover();
                const restored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
                defer std.testing.allocator.free(restored);
                try std.testing.expectEqualStrings(old_manifest, restored);
                try std.testing.expectError(error.FileNotFound, data.access("applications/added.desktop", .{}));
                continue;
            }
            if (case_index == 7) {
                if (c.unlinkat(state.fd, "manifest.json", 0) != 0) return error.IoError;
                try std.testing.expectError(error.ArtifactConflict, store.recover());
                try std.testing.expectError(error.FileNotFound, data.access("applications/added.desktop", .{}));
                try std.testing.expectError(error.FileNotFound, state.access("manifest.json", .{}));
                var transactions = try state.openDir("transactions", .{ .iterate = true });
                defer transactions.close();
                var iterator = transactions.iterate();
                const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
                try transaction_evidence_exists(&transactions, entry.name);
                continue;
            }
            try store.recover();
            if (failure_step < 13) {
                const restored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
                defer std.testing.allocator.free(restored);
                try std.testing.expectEqualStrings(old_manifest, restored);
                try std.testing.expectError(error.FileNotFound, data.access("applications/added.desktop", .{}));
            } else {
                const restored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
                defer std.testing.allocator.free(restored);
                try std.testing.expectEqualStrings(new_manifest, restored);
                const published = try data.readFileAlloc(std.testing.allocator, "applications/added.desktop", 4096);
                defer std.testing.allocator.free(published);
                try std.testing.expectEqualStrings(new_payload, published);
            }
        } else {
            try committed;
            const stored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
            defer std.testing.allocator.free(stored);
            try std.testing.expectEqualStrings(new_manifest, stored);
            const kept = try data.readFileAlloc(std.testing.allocator, "applications/kept.desktop", 4096);
            defer std.testing.allocator.free(kept);
            try std.testing.expectEqualStrings(old_payload, kept);
        }
    }
}

test "existing manifest target permits no silent replacement or identity retarget" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/existing.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const other_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/retargeted.desktop", .{data_path});
    defer std.testing.allocator.free(other_path);
    const payload = "already managed";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:existing", payload);
    defer std.testing.allocator.free(old_manifest);
    const unchanged_candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:existing", payload);
    defer std.testing.allocator.free(unchanged_candidate);
    const retargeted_candidate = try test_manifest_bytes(std.testing.allocator, other_path, "desktop:existing", payload);
    defer std.testing.allocator.free(retargeted_candidate);
    try data.writeFile(.{ .sub_path = "applications/existing.desktop", .data = payload });
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    defer _ = c.close(manifest_fd);
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    try std.testing.expectError(error.UnsupportedReplacement, store.commit(.{ .record_id = "desktop:existing", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = unchanged_candidate }));
    try std.testing.expectError(error.ManifestMismatch, store.commit(.{ .record_id = "desktop:existing", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = retargeted_candidate }));
    const current = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(current);
    try std.testing.expectEqualStrings(old_manifest, current);
}

test "forced replacement retains exact modified bytes and explicit restore keeps backup" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/replace.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);

    const manifest_bytes = "managed original";
    const user_modified_bytes = "user edit survives in backup";
    const replacement_bytes = "repaired managed artifact";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:replace", manifest_bytes);
    defer std.testing.allocator.free(old_manifest);
    const replacement_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:replace", replacement_bytes);
    defer std.testing.allocator.free(replacement_manifest);
    const restore_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:replace", user_modified_bytes);
    defer std.testing.allocator.free(restore_manifest);

    try data.writeFile(.{ .sub_path = "applications/replace.desktop", .data = user_modified_bytes });
    if (c.fchmodat(data.fd, "applications/replace.desktop", 0o644, 0) != 0) return error.IoError;
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0) return error.IoError;
    _ = c.close(manifest_fd);

    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    var old_info = (try store.inspect(data.fd, "applications/replace.desktop")).?;
    try std.testing.expectError(error.UnsupportedReplacement, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = replacement_manifest,
    }));
    var stale_info = identity_from_info(old_info);
    stale_info.inode +%= 1;
    try std.testing.expectError(error.ArtifactConflict, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = stale_info,
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    }));
    try std.testing.expectError(error.InvalidArgument, store.replace(.{
        .record_id = "",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    }));
    try std.testing.expectError(error.InvalidArgument, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = "",
        .backup_modified = true,
    }));
    try std.testing.expectError(error.InvalidArgument, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .artifact_mode = 0o600,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    }));
    const incorrect_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:replace", "different bytes");
    defer std.testing.allocator.free(incorrect_manifest);
    try std.testing.expectError(error.ManifestMismatch, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = incorrect_manifest,
        .backup_modified = true,
    }));
    var stale_manifest_digest = hash_bytes(old_manifest);
    stale_manifest_digest[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = stale_manifest_digest,
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    }));
    try data.deleteFile("applications/replace.desktop");
    try std.testing.expectError(error.ArtifactConflict, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    }));
    try data.writeFile(.{ .sub_path = "applications/replace.desktop", .data = user_modified_bytes });
    if (c.fchmodat(data.fd, "applications/replace.desktop", 0o600, 0) != 0) return error.IoError;
    const unsupported_mode_info = (try store.inspect(data.fd, "applications/replace.desktop")).?;
    try std.testing.expectError(error.AccessDenied, store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(unsupported_mode_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    }));
    if (c.fchmodat(data.fd, "applications/replace.desktop", 0o644, 0) != 0) return error.IoError;
    old_info = (try store.inspect(data.fd, "applications/replace.desktop")).?;
    const replacement = try store.replace(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(old_manifest),
        .expected_target = identity_from_info(old_info),
        .artifact_bytes = replacement_bytes,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    });
    const backup_ref = replacement.backup orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(@as(u32, 0o644), backup_ref.artifact_mode);
    try std.testing.expect(backup_ref.inode != 0 and backup_ref.device != 0);
    const backup_digest_text = hash_hex_from_array(backup_ref.sha256);
    var backup_journal = journal_t{
        .version = 2,
        .transaction_id = &backup_ref.transaction_id,
        .phase = "committed",
        .record_id = "desktop:replace",
        .root = "data",
        .relative_path = "applications/replace.desktop",
        .old_artifact_hash = &backup_digest_text,
        .old_artifact_mode = backup_ref.artifact_mode,
        .new_artifact_hash = &backup_digest_text,
        .new_artifact_mode = 0o644,
        .new_artifact_device = 1,
        .new_artifact_inode = 1,
        .old_manifest_hash = &backup_digest_text,
        .new_manifest_hash = &backup_digest_text,
        .new_manifest_device = 1,
        .new_manifest_inode = 2,
        .old_artifact_device = backup_ref.device,
        .old_artifact_inode = backup_ref.inode,
        .backup_hash = &backup_digest_text,
        .backup_mode = backup_ref.artifact_mode,
        .backup_device = backup_ref.device,
        .backup_inode = backup_ref.inode,
    };
    try store.verify_backup_for_journal(backup_journal);
    backup_journal.backup_mode = 0o600;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.backup_mode = backup_ref.artifact_mode;
    backup_journal.backup_device = null;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.backup_device = backup_ref.device;
    backup_journal.backup_inode = null;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.backup_inode = backup_ref.inode;
    backup_journal.version = 1;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.version = 2;
    backup_journal.old_artifact_hash = null;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.old_artifact_hash = &backup_digest_text;
    backup_journal.backup_hash = null;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.backup_mode = 0;
    backup_journal.backup_device = null;
    backup_journal.backup_inode = null;
    try store.verify_backup_for_journal(backup_journal);
    backup_journal.backup_mode = 0o644;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.backup_mode = 0;
    backup_journal.backup_inode = 9;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    backup_journal.backup_inode = null;
    backup_journal.backup_device = 9;
    try std.testing.expectError(error.RecoveryRequired, store.verify_backup_for_journal(backup_journal));
    var bad_backup_ref = backup_ref;
    bad_backup_ref.artifact_mode = 0o600;
    try std.testing.expectError(error.InvalidArgument, store.read_backup("desktop:replace", bad_backup_ref));
    bad_backup_ref = backup_ref;
    bad_backup_ref.transaction_id[0] = 'g';
    try std.testing.expectError(error.InvalidArgument, store.read_backup("desktop:replace", bad_backup_ref));
    bad_backup_ref = backup_ref;
    bad_backup_ref.record_id_sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", bad_backup_ref));
    bad_backup_ref = backup_ref;
    bad_backup_ref.inode +%= 1;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", bad_backup_ref));
    bad_backup_ref = backup_ref;
    bad_backup_ref.sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", bad_backup_ref));
    const backup = try store.read_backup("desktop:replace", backup_ref);
    defer std.testing.allocator.free(backup);
    try std.testing.expectEqualStrings(user_modified_bytes, backup);
    for (1..33) |failure_io| {
        injected_io_at = failure_io;
        injected_io_count = 0;
        const attempt = store.read_backup("desktop:replace", backup_ref);
        injected_io_at = 0;
        injected_io_count = 0;
        if (attempt) |bytes| {
            std.testing.allocator.free(bytes);
        } else |err| {
            try std.testing.expect(err == error.IoError or err == error.ArtifactConflict or err == error.RecoveryRequired or err == error.AccessDenied or err == error.OutOfMemory);
        }
    }
    const installed = try data.readFileAlloc(std.testing.allocator, "applications/replace.desktop", 4096);
    defer std.testing.allocator.free(installed);
    try std.testing.expectEqualStrings(replacement_bytes, installed);

    const installed_info = (try store.inspect(data.fd, "applications/replace.desktop")).?;
    const restored = try store.restore(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(replacement_manifest),
        .expected_target = identity_from_info(installed_info),
        .backup = backup_ref,
        .candidate_manifest = restore_manifest,
    });
    try std.testing.expectEqual(@as(u32, 0o644), restored.target.mode);
    const restored_bytes = try data.readFileAlloc(std.testing.allocator, "applications/replace.desktop", 4096);
    defer std.testing.allocator.free(restored_bytes);
    try std.testing.expectEqualStrings(user_modified_bytes, restored_bytes);
    const retained = try store.read_backup("desktop:replace", backup_ref);
    defer std.testing.allocator.free(retained);
    try std.testing.expectEqualStrings(user_modified_bytes, retained);
    try std.testing.expectError(error.InvalidArgument, store.restore(.{
        .record_id = "",
        .expected_manifest_sha256 = hash_bytes(replacement_manifest),
        .expected_target = identity_from_info(installed_info),
        .backup = backup_ref,
        .candidate_manifest = restore_manifest,
    }));

    var invalid_backup = backup_ref;
    invalid_backup.artifact_mode = 0o600;
    try std.testing.expectError(error.InvalidArgument, store.read_backup("desktop:replace", invalid_backup));
    invalid_backup = backup_ref;
    invalid_backup.transaction_id[0] = 'g';
    try std.testing.expectError(error.InvalidArgument, store.read_backup("desktop:replace", invalid_backup));
    try std.testing.expectError(error.InvalidArgument, store.read_backup("", backup_ref));
    invalid_backup = backup_ref;
    invalid_backup.record_id_sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", invalid_backup));
    invalid_backup = backup_ref;
    invalid_backup.transaction_id[0] = if (invalid_backup.transaction_id[0] == '0') '1' else '0';
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", invalid_backup));
    invalid_backup = backup_ref;
    invalid_backup.sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", invalid_backup));
    invalid_backup = backup_ref;
    invalid_backup.inode +%= 1;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", invalid_backup));
    try std.testing.expectError(error.ManifestMismatch, store.restore(.{
        .record_id = "desktop:replace",
        .expected_manifest_sha256 = hash_bytes(replacement_manifest),
        .expected_target = identity_from_info((try store.inspect(data.fd, "applications/replace.desktop")).?),
        .backup = backup_ref,
        .candidate_manifest = replacement_manifest,
    }));

    var backup_root = try state.openDir("backups", .{});
    defer backup_root.close();
    var backup_dir = try backup_root.openDir(&backup_ref.transaction_id, .{});
    defer backup_dir.close();
    const backup_txid_z = try std.testing.allocator.dupeZ(u8, &backup_ref.transaction_id);
    defer std.testing.allocator.free(backup_txid_z);
    const backup_leaf = hash_hex("desktop:replace");
    const backup_leaf_z = try std.testing.allocator.dupeZ(u8, &backup_leaf);
    defer std.testing.allocator.free(backup_leaf_z);
    if (c.fchmodat(backup_root.fd, backup_txid_z, 0o755, 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", backup_ref));
    if (c.fchmodat(backup_root.fd, backup_txid_z, 0o700, 0) != 0) return error.IoError;
    const metadata_fd = c.openat(backup_dir.fd, "metadata.bin", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (metadata_fd < 0) return error.IoError;
    defer _ = c.close(metadata_fd);
    if (c.fchmod(metadata_fd, 0o644) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", backup_ref));
    if (c.fchmod(metadata_fd, 0o600) != 0) return error.IoError;
    const payload_fd = c.openat(backup_dir.fd, backup_leaf_z, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (payload_fd < 0) return error.IoError;
    defer _ = c.close(payload_fd);
    if (c.fchmod(payload_fd, 0o644) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", backup_ref));
    if (c.fchmod(payload_fd, 0o600) != 0) return error.IoError;
    try backup_dir.deleteFile(&backup_leaf);
    try backup_dir.writeFile(.{ .sub_path = &backup_leaf, .data = user_modified_bytes });
    if (c.fchmodat(backup_dir.fd, backup_leaf_z, 0o600, 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.read_backup("desktop:replace", backup_ref));
}

test "replacement durable checkpoint failures recover old or committed state and preserve backup" {
    for (2..23) |failure_step| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/checkpoint.desktop", .{data_path});
        defer std.testing.allocator.free(target_path);
        const old_record_bytes = "old recorded content";
        const changed_bytes = "external edit before force repair";
        const new_bytes = "new installed content";
        const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:checkpoint", old_record_bytes);
        defer std.testing.allocator.free(old_manifest);
        const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:checkpoint", new_bytes);
        defer std.testing.allocator.free(new_manifest);
        try data.writeFile(.{ .sub_path = "applications/checkpoint.desktop", .data = changed_bytes });
        if (c.fchmodat(data.fd, "applications/checkpoint.desktop", 0o644, 0) != 0) return error.IoError;
        const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (manifest_fd < 0) return error.IoError;
        try write_all(manifest_fd, old_manifest);
        if (c.fsync(manifest_fd) != 0) return error.IoError;
        _ = c.close(manifest_fd);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();
        const old_info = (try store.inspect(data.fd, "applications/checkpoint.desktop")).?;
        store.fail_after.store(failure_step, .monotonic);
        const result = store.replace(.{
            .record_id = "desktop:checkpoint",
            .expected_manifest_sha256 = hash_bytes(old_manifest),
            .expected_target = identity_from_info(old_info),
            .artifact_bytes = new_bytes,
            .candidate_manifest = new_manifest,
            .backup_modified = true,
        });
        if (result) |_| {
            store.fail_after.store(0, .monotonic);
            if (failure_step <= 21) return error.ExpectedInjectedFailure;
        } else |err| {
            try std.testing.expectEqual(error.InjectedFailure, err);
            store.fail_after.store(0, .monotonic);
            try store.recover();
        }
        if (failure_step == 8) {
            try std.testing.expectError(error.RecoveryRequired, store.list_backups(std.testing.allocator, "desktop:checkpoint"));
            try std.testing.expectError(error.RecoveryRequired, store.list_backups(std.testing.allocator, "desktop:checkpoint"));
        } else {
            var retained = try store.list_backups(std.testing.allocator, "desktop:checkpoint");
            defer retained.deinit();
            const expected_backups: usize = if (failure_step < 8) 0 else 1;
            if (retained.items.len != expected_backups) {
                std.debug.print("checkpoint {d}: expected {d} backups, found {d}\n", .{ failure_step, expected_backups, retained.items.len });
                return error.TestFailure;
            }
        }
        const installed = try data.readFileAlloc(std.testing.allocator, "applications/checkpoint.desktop", 4096);
        defer std.testing.allocator.free(installed);
        const stored_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
        defer std.testing.allocator.free(stored_manifest);
        const old_state = std.mem.eql(u8, installed, changed_bytes);
        const new_state = std.mem.eql(u8, installed, new_bytes);
        try std.testing.expect(old_state or new_state);
        if (old_state) try std.testing.expectEqualStrings(old_manifest, stored_manifest) else try std.testing.expectEqualStrings(new_manifest, stored_manifest);
    }
}

test "prepared replacement recovery is retryable after artifact-temp cleanup was durable" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/retry-rollback.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const old_bytes = "old retry payload";
    const new_bytes = "new retry payload";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:retry-rollback", old_bytes);
    defer std.testing.allocator.free(old_manifest);
    const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:retry-rollback", new_bytes);
    defer std.testing.allocator.free(new_manifest);
    try data.writeFile(.{ .sub_path = "applications/retry-rollback.desktop", .data = old_bytes });
    if (c.fchmodat(data.fd, "applications/retry-rollback.desktop", 0o644, 0) != 0) return error.IoError;
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const snapshot = try store.inspect_record("desktop:retry-rollback");
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 12, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.replace(.{
        .record_id = "desktop:retry-rollback",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = new_bytes,
        .candidate_manifest = new_manifest,
    }));
    store.fail_after.store(0, .monotonic);

    var transactions = try state.openDir("transactions", .{ .iterate = true });
    defer transactions.close();
    var iterator = transactions.iterate();
    const entry = (try iterator.next()) orelse return error.TestExpectedEqual;
    const parsed = try txid_array(entry.name);
    const journal_bytes = try store.read_tx_file(parsed, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_bytes);
    var decoded = try parse_journal(std.testing.allocator, journal_bytes);
    defer decoded.deinit();
    try store.apply_prepared_replace(decoded.value);

    // Rollback exchanges both objects, then the third checkpoint removes the
    // displaced artifact temp. Failure there must leave a state that a second
    // recovery can recognize and finish without its inode being present.
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 3, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.recover());
    store.fail_after.store(0, .monotonic);
    try store.recover();
    const restored_artifact = try data.readFileAlloc(std.testing.allocator, "applications/retry-rollback.desktop", MaxArtifactBytes);
    defer std.testing.allocator.free(restored_artifact);
    const restored_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(restored_manifest);
    try std.testing.expectEqualStrings(old_bytes, restored_artifact);
    try std.testing.expectEqualStrings(old_manifest, restored_manifest);
}

test "prepared replacement recovery reuses the exact old-manifest temp after exchange failure" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/retry-manifest.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const old_bytes = "old manifest retry payload";
    const new_bytes = "new manifest retry payload";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:retry-manifest", old_bytes);
    defer std.testing.allocator.free(old_manifest);
    const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:retry-manifest", new_bytes);
    defer std.testing.allocator.free(new_manifest);
    try data.writeFile(.{ .sub_path = "applications/retry-manifest.desktop", .data = old_bytes });
    if (c.fchmodat(data.fd, "applications/retry-manifest.desktop", 0o644, 0) != 0) return error.IoError;
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const snapshot = try store.inspect_record("desktop:retry-manifest");
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 12, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.replace(.{
        .record_id = "desktop:retry-manifest",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = new_bytes,
        .candidate_manifest = new_manifest,
    }));
    store.fail_after.store(0, .monotonic);

    var transactions = try state.openDir("transactions", .{ .iterate = true });
    defer transactions.close();
    var iterator = transactions.iterate();
    const transaction = (try iterator.next()) orelse return error.TestExpectedEqual;
    const txid = try txid_array(transaction.name);
    const journal_bytes = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_bytes);
    var parsed = try parse_journal(std.testing.allocator, journal_bytes);
    defer parsed.deinit();
    try store.apply_prepared_replace(parsed.value);

    const manifest_temp = manifest_temp_name(try txid_array(parsed.value.transaction_id));
    const manifest_temp_path = manifest_temp[0 .. ".manifest-".len + 32 + ".tmp".len];
    if (c.renameat(state.fd, manifest_temp_path.ptr, state.fd, "held-old-manifest") != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, store.rollback_prepared_replace(parsed.value));
    const unchanged_new_artifact = try data.readFileAlloc(std.testing.allocator, "applications/retry-manifest.desktop", MaxArtifactBytes);
    defer std.testing.allocator.free(unchanged_new_artifact);
    try std.testing.expectEqualStrings(new_bytes, unchanged_new_artifact);
    if (c.renameat(state.fd, "held-old-manifest", state.fd, manifest_temp_path.ptr) != 0) return error.IoError;

    // Artifact rollback exchange succeeds first. Fail the manifest exchange
    // and then retry: recovery must validate and reuse the already-staged old
    // manifest inode instead of attempting an O_EXCL recreation.
    injected_renameat2_count = 0;
    injected_renameat2_at = 2;
    defer {
        injected_renameat2_at = 0;
        injected_renameat2_count = 0;
    }
    try std.testing.expectError(error.IoError, store.rollback_prepared_replace(parsed.value));
    try std.testing.expectEqual(@as(usize, 0), injected_renameat2_at);
    const retained_old_manifest = try store.inspect_named(store.dir_fd, manifest_temp[0..].ptr);
    try std.testing.expect(retained_old_manifest != null);
    try std.testing.expectEqual(parsed.value.old_manifest_device.?, retained_old_manifest.?.device);
    try std.testing.expectEqual(parsed.value.old_manifest_inode.?, retained_old_manifest.?.inode);
    try store.rollback_prepared_replace(parsed.value);
    const restored_artifact = try data.readFileAlloc(std.testing.allocator, "applications/retry-manifest.desktop", MaxArtifactBytes);
    defer std.testing.allocator.free(restored_artifact);
    const restored_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(restored_manifest);
    try std.testing.expectEqualStrings(old_bytes, restored_artifact);
    try std.testing.expectEqualStrings(old_manifest, restored_manifest);
}

test "manifest staging helpers verify real inode, digest, absence, and reuse states" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();

    const previous = "real authenticated manifest bytes";
    const fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (fd < 0) return error.IoError;
    try write_all(fd, previous);
    if (c.fsync(fd) != 0 or c.close(fd) != 0) return error.IoError;
    const actual = (try store.inspect_named(state.fd, "manifest.json")) orelse return error.TestExpectedEqual;
    try store.verify_named_file(state.fd, "manifest.json", actual);
    var wrong_identity = actual;
    wrong_identity.inode +%= 1;
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(state.fd, "manifest.json", wrong_identity));
    try store.validate_optional_temp(state.fd, "manifest.json", actual, true, false);
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(state.fd, "manifest.json", actual, false, true));
    try store.validate_optional_temp(state.fd, "missing-temp", actual, false, false);
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(state.fd, "missing-temp", actual, true, false));

    const old_hash = hash_hex_from_array(actual.hash);
    var journal = journal_t{
        .transaction_id = "0123456789abcdef0123456789abcdef",
        .phase = "prepared",
        .record_id = "desktop:staging",
        .root = "data",
        .relative_path = "applications/staging.desktop",
        .old_artifact_hash = "0000000000000000000000000000000000000000000000000000000000000000",
        .old_artifact_mode = 0o644,
        .new_artifact_hash = "1111111111111111111111111111111111111111111111111111111111111111",
        .new_artifact_mode = 0o644,
        .new_artifact_device = 1,
        .new_artifact_inode = 2,
        .old_manifest_hash = &old_hash,
        .old_manifest_device = actual.device,
        .old_manifest_inode = actual.inode,
        .new_manifest_hash = "2222222222222222222222222222222222222222222222222222222222222222",
        .new_manifest_device = 1,
        .new_manifest_inode = 3,
    };
    try store.validate_old_manifest_temp(journal, "manifest.json");
    var no_identity = journal;
    no_identity.old_manifest_device = null;
    no_identity.old_manifest_inode = null;
    try store.validate_old_manifest_temp(no_identity, "manifest.json");
    journal.old_manifest_inode.? += 1;
    try std.testing.expectError(error.ArtifactConflict, store.validate_old_manifest_temp(journal, "manifest.json"));
    journal.old_manifest_inode.? -= 1;
    try std.testing.expect(same_file_info(actual, journal_old_manifest_info(journal).?));
    var changed_hash = actual;
    changed_hash.hash[0] ^= 1;
    try std.testing.expect(!same_file_info(actual, changed_hash));

    const txid = try txid_array(journal.transaction_id);
    const temp_name = manifest_temp_name(txid);
    try state.writeFile(.{ .sub_path = temp_name[0 .. ".manifest-".len + 32 + ".tmp".len], .data = previous });
    if (c.fchmodat(state.fd, temp_name[0 .. ".manifest-".len + 32 + ".tmp".len], 0o600, 0) != 0) return error.IoError;
    const temp_info = (try store.inspect_named(state.fd, temp_name[0..].ptr)) orelse return error.TestExpectedEqual;
    journal.old_manifest_device = temp_info.device;
    journal.old_manifest_inode = temp_info.inode;
    const reused = try store.create_or_reuse_old_manifest_temp(txid, journal, previous);
    try std.testing.expect(same_file_info(temp_info, reused));
    no_identity.old_manifest_device = null;
    no_identity.old_manifest_inode = null;
    const reused_without_inode = try store.create_or_reuse_old_manifest_temp(txid, no_identity, previous);
    try std.testing.expectEqual(temp_info.hash, reused_without_inode.hash);
    const temp_name_slice = temp_name[0 .. ".manifest-".len + 32 + ".tmp".len];
    try state.rename(temp_name_slice, "held-authentic-old-manifest");
    try state.writeFile(.{ .sub_path = temp_name_slice, .data = previous });
    if (c.fchmodat(state.fd, temp_name_slice.ptr, 0o600, 0) != 0) return error.IoError;
    const same_bytes_foreign_inode = (try store.inspect_named(state.fd, temp_name[0..].ptr)) orelse return error.TestExpectedEqual;
    try std.testing.expect(!same_file_info(temp_info, same_bytes_foreign_inode));
    try std.testing.expectError(error.ArtifactConflict, store.remove_published_old_manifest_temp(txid, journal));
    try std.testing.expect(same_file_info(same_bytes_foreign_inode, (try store.inspect_named(state.fd, temp_name[0..].ptr)).?));
    try unlink_named_if_matches(std.testing.allocator, state.fd, temp_name[0..].ptr, same_bytes_foreign_inode);
    try state.rename("held-authentic-old-manifest", temp_name_slice);
    try store.remove_published_old_manifest_temp(txid, journal);
    try store.remove_published_old_manifest_temp(txid, journal);
    try std.testing.expectError(error.RecoveryRequired, store.create_or_reuse_old_manifest_temp(txid, journal, previous));
    try std.testing.expect((try store.inspect_named(state.fd, temp_name[0..].ptr)) == null);

    try state.writeFile(.{ .sub_path = temp_name_slice, .data = previous });
    if (c.fchmodat(state.fd, temp_name_slice.ptr, 0o600, 0) != 0) return error.IoError;
    try store.remove_published_old_manifest_temp(txid, no_identity);
    try std.testing.expect((try store.inspect_named(state.fd, temp_name[0..].ptr)) == null);

    var legacy_journal = journal;
    legacy_journal.version = 1;
    legacy_journal.old_manifest_device = null;
    legacy_journal.old_manifest_inode = null;
    injected_io_count = 0;
    injected_io_at = 2; // First openat in create_state_temp after absent-temp inspection.
    defer {
        injected_io_at = 0;
        injected_io_count = 0;
    }
    try std.testing.expectError(error.IoError, store.create_or_reuse_old_manifest_temp(txid, legacy_journal, previous));
    injected_io_at = 0;
    injected_io_count = 0;
    try std.testing.expect((try store.inspect_named(state.fd, temp_name[0..].ptr)) == null);
    const recreated_legacy = try store.create_or_reuse_old_manifest_temp(txid, legacy_journal, previous);
    try std.testing.expectEqual(hash_bytes(previous), recreated_legacy.hash);
    try unlink_named_if_matches(std.testing.allocator, state.fd, temp_name[0..].ptr, recreated_legacy);

    const next_bytes = "new candidate manifest bytes";
    const next_info = try store.create_state_temp(next_bytes, 0o600, txid);
    const next_hash = hash_hex(next_bytes);
    var first_install = journal;
    first_install.old_manifest_hash = null;
    first_install.old_manifest_device = null;
    first_install.old_manifest_inode = null;
    first_install.new_manifest_hash = &next_hash;
    first_install.new_manifest_device = next_info.device;
    first_install.new_manifest_inode = next_info.inode;
    try store.remove_published_old_manifest_temp(txid, first_install);
    try store.remove_published_old_manifest_temp(txid, first_install);

    try state.writeFile(.{ .sub_path = temp_name[0 .. ".manifest-".len + 32 + ".tmp".len], .data = "foreign same-name bytes" });
    if (c.fchmodat(state.fd, temp_name[0 .. ".manifest-".len + 32 + ".tmp".len], 0o600, 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.create_or_reuse_old_manifest_temp(txid, journal, previous));

    const publish_txid = [_]u8{'b'} ** 32;
    const publish_bytes = "candidate manifest for inode preflight";
    const publish_info = try store.create_state_temp(publish_bytes, 0o600, publish_txid);
    var wrong_manifest_identity = actual;
    wrong_manifest_identity.inode +%= 1;
    const current_hash_text = hash_hex_from_array(actual.hash);
    const incorrect_old_hash = hash_hex("not the manifest bytes");
    try std.testing.expectError(error.ArtifactConflict, store.publish_manifest(publish_txid, publish_info, &incorrect_old_hash, actual));
    try std.testing.expectError(error.ArtifactConflict, store.publish_manifest(publish_txid, publish_info, &current_hash_text, wrong_manifest_identity));
    try std.testing.expect(same_file_info(publish_info, (try store.inspect_named(state.fd, manifest_temp_name(publish_txid)[0..].ptr)).?));
    try store.verify_named_file(state.fd, "manifest.json", actual);
    try state.rename("manifest.json", "held-current-manifest");
    try std.testing.expectError(error.ArtifactConflict, store.publish_manifest(publish_txid, publish_info, &current_hash_text, actual));
    try store.verify_named_file(state.fd, manifest_temp_name(publish_txid)[0..].ptr, publish_info);
    try state.rename("held-current-manifest", "manifest.json");
    try store.remove_state_temp(publish_txid, publish_info);
}

test "replacement retries real filesystem operations after every selected injected failure" {
    for (1..161) |failure_io| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/io-retry.desktop", .{data_path});
        defer std.testing.allocator.free(target_path);
        const old_bytes = "old managed bytes";
        const modified_bytes = "modified bytes requiring backup";
        const new_bytes = "new committed bytes";
        const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:io-retry", old_bytes);
        defer std.testing.allocator.free(old_manifest);
        const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:io-retry", new_bytes);
        defer std.testing.allocator.free(new_manifest);
        try data.writeFile(.{ .sub_path = "applications/io-retry.desktop", .data = modified_bytes });
        if (c.fchmodat(data.fd, "applications/io-retry.desktop", 0o644, 0) != 0) return error.IoError;
        const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (manifest_fd < 0) return error.IoError;
        try write_all(manifest_fd, old_manifest);
        if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();
        const expected_target = identity_from_info((try store.inspect(data.fd, "applications/io-retry.desktop")).?);

        injected_io_at = failure_io;
        injected_io_count = 0;
        const outcome = store.replace(.{
            .record_id = "desktop:io-retry",
            .expected_manifest_sha256 = hash_bytes(old_manifest),
            .expected_target = expected_target,
            .artifact_bytes = new_bytes,
            .candidate_manifest = new_manifest,
            .backup_modified = true,
        });
        injected_io_at = 0;
        injected_io_count = 0;
        if (outcome) |_| {} else |err| {
            try std.testing.expect(err != error.InvalidArgument and err != error.OutOfMemory);
            store.recover() catch |recovery_error| try std.testing.expect(recovery_error == error.RecoveryRequired or recovery_error == error.IoError);
        }
        const installed = try data.readFileAlloc(std.testing.allocator, "applications/io-retry.desktop", 4096);
        defer std.testing.allocator.free(installed);
        try std.testing.expect(std.mem.eql(u8, installed, modified_bytes) or std.mem.eql(u8, installed, new_bytes));
        const current_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
        defer std.testing.allocator.free(current_manifest);
        if (std.mem.eql(u8, installed, modified_bytes)) {
            try std.testing.expectEqualStrings(old_manifest, current_manifest);
        } else {
            try std.testing.expectEqualStrings(new_manifest, current_manifest);
        }
    }
}

test "replacement allocation failures leave a recoverable old or committed state without leaks" {
    var observed_failures: usize = 0;
    for (0..256) |failure_offset| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/oom.desktop", .{data_path});
        defer std.testing.allocator.free(target_path);
        const record_bytes = "old manifest content";
        const changed_bytes = "modified target bytes";
        const new_bytes = "new target bytes";
        const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:oom", record_bytes);
        defer std.testing.allocator.free(old_manifest);
        const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:oom", new_bytes);
        defer std.testing.allocator.free(new_manifest);
        try data.writeFile(.{ .sub_path = "applications/oom.desktop", .data = changed_bytes });
        if (c.fchmodat(data.fd, "applications/oom.desktop", 0o644, 0) != 0) return error.IoError;
        const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (manifest_fd < 0) return error.IoError;
        try write_all(manifest_fd, old_manifest);
        if (c.fsync(manifest_fd) != 0) return error.IoError;
        _ = c.close(manifest_fd);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        var store = try store_t.init(failing.allocator(), state.fd, &roots);
        const info = (try store.inspect(data.fd, "applications/oom.desktop")).?;
        failing.fail_index = failing.alloc_index + failure_offset;
        const result = store.replace(.{
            .record_id = "desktop:oom",
            .expected_manifest_sha256 = hash_bytes(old_manifest),
            .expected_target = identity_from_info(info),
            .artifact_bytes = new_bytes,
            .candidate_manifest = new_manifest,
            .backup_modified = true,
        });
        if (!failing.has_induced_failure) {
            _ = try result;
            store.deinit();
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed_failures += 1;
        if (result) |_| {
            return error.ExpectedOutOfMemory;
        } else |err| if (err != error.OutOfMemory) return err;
        failing.fail_index = std.math.maxInt(usize);
        try store.recover();
        const actual = try data.readFileAlloc(std.testing.allocator, "applications/oom.desktop", 4096);
        defer std.testing.allocator.free(actual);
        const actual_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
        defer std.testing.allocator.free(actual_manifest);
        const is_old = std.mem.eql(u8, actual, changed_bytes);
        const is_new = std.mem.eql(u8, actual, new_bytes);
        try std.testing.expect(is_old or is_new);
        if (is_old) try std.testing.expectEqualStrings(old_manifest, actual_manifest) else try std.testing.expectEqualStrings(new_manifest, actual_manifest);
        store.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    try std.testing.expect(observed_failures > 0);
}

test "replacement refuses unsupported exchange and recovery rolls back safely" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/unsupported.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const old_bytes = "old exact bytes";
    const new_bytes = "new bytes";
    const old_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:unsupported", old_bytes);
    defer std.testing.allocator.free(old_manifest);
    const new_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:unsupported", new_bytes);
    defer std.testing.allocator.free(new_manifest);
    try data.writeFile(.{ .sub_path = "applications/unsupported.desktop", .data = old_bytes });
    if (c.fchmodat(data.fd, "applications/unsupported.desktop", 0o644, 0) != 0) return error.IoError;
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, old_manifest);
    if (c.fsync(manifest_fd) != 0) return error.IoError;
    _ = c.close(manifest_fd);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    const info = (try store.inspect(data.fd, "applications/unsupported.desktop")).?;
    for ([_]c_int{ c.EOPNOTSUPP, c.ENOSYS, c.EINVAL }) |unsupported_errno| {
        injected_renameat2_errno = unsupported_errno;
        try std.testing.expectError(error.UnsupportedAtomicReplace, store.replace(.{
            .record_id = "desktop:unsupported",
            .expected_manifest_sha256 = hash_bytes(old_manifest),
            .expected_target = identity_from_info(info),
            .artifact_bytes = new_bytes,
            .candidate_manifest = new_manifest,
        }));
        try store.recover();
        const restored = try data.readFileAlloc(std.testing.allocator, "applications/unsupported.desktop", 4096);
        defer std.testing.allocator.free(restored);
        try std.testing.expectEqualStrings(old_bytes, restored);
    }
    const stored_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(stored_manifest);
    try std.testing.expectEqualStrings(old_manifest, stored_manifest);
}

test "record snapshots and backup inventory survive reopen and reject damaged metadata" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const state_path = try state.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/inventory.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const original = "original";
    const modified = "modified user copy";
    const replacement = "replacement";
    const initial_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:inventory", original);
    defer std.testing.allocator.free(initial_manifest);
    const next_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:inventory", replacement);
    defer std.testing.allocator.free(next_manifest);
    const restore_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:inventory", modified);
    defer std.testing.allocator.free(restore_manifest);
    const unrelated_restore_manifest = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:other", modified);
    defer std.testing.allocator.free(unrelated_restore_manifest);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    const invalid_id = try std.testing.allocator.alloc(u8, 1);
    defer std.testing.allocator.free(invalid_id);
    invalid_id[0] = 0;
    var invalid_restore = std.mem.zeroes(restore_request_t);
    invalid_restore.record_id = invalid_id;
    try std.testing.expectError(error.InvalidArgument, store.restore(invalid_restore));
    try std.testing.expectError(error.InvalidArgument, store.inspect_record(invalid_id));
    invalid_id[0] = 0xff;
    invalid_restore.record_id = invalid_id;
    try std.testing.expectError(error.InvalidArgument, store.restore(invalid_restore));
    try std.testing.expectError(error.InvalidArgument, store.inspect_record(invalid_id));
    var no_backups = try store.list_backups(std.testing.allocator, "desktop:inventory");
    try std.testing.expectEqual(@as(usize, 0), no_backups.items.len);
    no_backups.deinit();
    try store.commit(.{ .record_id = "desktop:inventory", .prior = .Absent, .artifact_bytes = original, .candidate_manifest = initial_manifest });
    try data.deleteFile("applications/inventory.desktop");
    try std.testing.expectError(error.FileNotFound, store.inspect_record("desktop:inventory"));
    try data.writeFile(.{ .sub_path = "applications/inventory.desktop", .data = original });
    const manifest_write_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (manifest_write_fd < 0) return error.IoError;
    defer _ = c.close(manifest_write_fd);
    if (c.ftruncate(manifest_write_fd, 0) != 0) return error.IoError;
    try write_all(manifest_write_fd, "not a manifest");
    if (c.fsync(manifest_write_fd) != 0) return error.IoError;
    try std.testing.expectError(error.InvalidManifest, store.inspect_record("desktop:inventory"));
    const outside_path = try std.fmt.allocPrint(std.testing.allocator, "/tmp/{s}", .{"inventory.desktop"});
    defer std.testing.allocator.free(outside_path);
    const outside_manifest = try test_manifest_bytes(std.testing.allocator, outside_path, "desktop:inventory", original);
    defer std.testing.allocator.free(outside_manifest);
    if (c.ftruncate(manifest_write_fd, 0) != 0 or c.lseek(manifest_write_fd, 0, c.SEEK_SET) < 0) return error.IoError;
    try write_all(manifest_write_fd, outside_manifest);
    if (c.fsync(manifest_write_fd) != 0) return error.IoError;
    try std.testing.expectError(error.InvalidPath, store.inspect_record("desktop:inventory"));
    if (c.ftruncate(manifest_write_fd, 0) != 0 or c.lseek(manifest_write_fd, 0, c.SEEK_SET) < 0) return error.IoError;
    try write_all(manifest_write_fd, initial_manifest);
    if (c.fsync(manifest_write_fd) != 0) return error.IoError;
    for (1..20) |failure_io| {
        injected_io_at = failure_io;
        injected_io_count = 0;
        const attempt = store.inspect_record("desktop:inventory");
        injected_io_at = 0;
        injected_io_count = 0;
        if (attempt) |_| {} else |err| {
            try std.testing.expect(err == error.IoError or err == error.AccessDenied or err == error.RecoveryRequired or err == error.OutOfMemory);
        }
    }
    const target_fd = c.openat(data.fd, "applications/inventory.desktop", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (target_fd < 0) return error.IoError;
    defer _ = c.close(target_fd);
    if (c.ftruncate(target_fd, 0) != 0) return error.IoError;
    try write_all(target_fd, modified);
    if (c.fchmod(target_fd, 0o755) != 0 or c.fsync(target_fd) != 0) return error.IoError;
    const snapshot = try store.inspect_record("desktop:inventory");
    try std.testing.expectEqual(hash_bytes(initial_manifest), snapshot.manifest_sha256);
    try std.testing.expectEqual(@as(u32, 0o755), snapshot.target.mode);
    try std.testing.expectError(error.InvalidArgument, store.inspect_record(""));
    try std.testing.expectError(error.FileNotFound, store.inspect_record("desktop:absent"));
    const receipt = try store.replace(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = replacement,
        .candidate_manifest = next_manifest,
        .backup_modified = true,
    });
    const reference = receipt.backup orelse return error.TestExpectedEqual;
    var listing = try store.list_backups(std.testing.allocator, "desktop:inventory");
    try std.testing.expectEqual(@as(usize, 1), listing.items.len);
    try std.testing.expect(same_backup_ref(reference, listing.items[0]));
    listing.deinit();
    for (1..36) |fault_index| {
        injected_io_at = fault_index;
        injected_io_count = 0;
        const attempt = store.list_backups(std.testing.allocator, "desktop:inventory");
        injected_io_at = 0;
        injected_io_count = 0;
        if (attempt) |result| {
            var owned = result;
            owned.deinit();
        } else |err| {
            try std.testing.expect(err == error.IoError or err == error.RecoveryRequired or err == error.AccessDenied);
        }
    }
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    try std.testing.expectError(error.OutOfMemory, store.list_backups(failing.allocator(), "desktop:inventory"));
    try std.testing.expectError(error.InvalidArgument, store.list_backups(std.testing.allocator, ""));
    store.deinit();

    store = try store_t.init(std.testing.allocator, state.fd, &roots);
    var after_reopen = try store.list_backups(std.testing.allocator, "desktop:inventory");
    try std.testing.expectEqual(@as(usize, 1), after_reopen.items.len);
    try std.testing.expect(same_backup_ref(reference, after_reopen.items[0]));
    after_reopen.deinit();
    var current = try store.inspect_record("desktop:inventory");
    try std.testing.expectError(error.InvalidArgument, store.restore(.{
        .record_id = "",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = reference,
        .candidate_manifest = restore_manifest,
    }));
    try std.testing.expectError(error.InvalidManifest, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = reference,
        .candidate_manifest = "{",
    }));
    try std.testing.expectError(error.ManifestMismatch, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = reference,
        .candidate_manifest = unrelated_restore_manifest,
    }));
    var invalid_backup = reference;
    invalid_backup.transaction_id[0] = 'z';
    try std.testing.expectError(error.InvalidArgument, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = invalid_backup,
        .candidate_manifest = restore_manifest,
    }));
    invalid_backup = reference;
    invalid_backup.artifact_mode = 0o600;
    try std.testing.expectError(error.InvalidArgument, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = invalid_backup,
        .candidate_manifest = restore_manifest,
    }));
    invalid_backup = reference;
    invalid_backup.record_id_sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = invalid_backup,
        .candidate_manifest = restore_manifest,
    }));
    invalid_backup = reference;
    invalid_backup.inode +%= 1;
    try std.testing.expectError(error.ArtifactConflict, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = invalid_backup,
        .candidate_manifest = restore_manifest,
    }));
    try std.testing.expectError(error.ManifestMismatch, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = reference,
        .candidate_manifest = next_manifest,
    }));
    var stale_restore = current;
    stale_restore.manifest_sha256[0] ^= 1;
    try std.testing.expectError(error.ArtifactConflict, store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = stale_restore.manifest_sha256,
        .expected_target = current.target,
        .backup = reference,
        .candidate_manifest = restore_manifest,
    }));
    for (1..80) |failure_index| {
        injected_io_at = failure_index;
        injected_io_count = 0;
        const attempt = store.restore(.{
            .record_id = "desktop:inventory",
            .expected_manifest_sha256 = current.manifest_sha256,
            .expected_target = current.target,
            .backup = reference,
            .candidate_manifest = restore_manifest,
        });
        injected_io_at = 0;
        injected_io_count = 0;
        if (attempt) |_| {} else |err| {
            try std.testing.expect(err == error.IoError or err == error.ArtifactConflict or err == error.RecoveryRequired or err == error.AccessDenied or err == error.OutOfMemory);
        }
        try store.recover();
        const after_restore = try store.inspect_record("desktop:inventory");
        if (!std.mem.eql(u8, &after_restore.target.sha256, &hash_bytes(replacement))) {
            try std.testing.expectEqual(hash_bytes(modified), after_restore.target.sha256);
            _ = try store.replace(.{
                .record_id = "desktop:inventory",
                .expected_manifest_sha256 = after_restore.manifest_sha256,
                .expected_target = after_restore.target,
                .artifact_bytes = replacement,
                .candidate_manifest = next_manifest,
                .artifact_mode = 0o644,
            });
        }
        current = try store.inspect_record("desktop:inventory");
    }
    _ = try store.restore(.{
        .record_id = "desktop:inventory",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = reference,
        .candidate_manifest = restore_manifest,
    });
    try store.recover();
    try store.recover();
    store.deinit();

    var checked = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer checked.deinit();
    var backups = try state.openDir("backups", .{});
    defer backups.close();
    try backups.makeDir(".pending-test");
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    var retained_pending = try backups.openDir(".pending-test", .{});
    retained_pending.close();
    const blocked_id = [_]u8{'a'} ** 32;
    try std.testing.expectError(error.RecoveryRequired, checked.create_backup(blocked_id, "desktop:inventory", "blocked", 0o644));
    try backups.deleteDir(".pending-test");
    var tx_dir = try backups.openDir(&reference.transaction_id, .{});
    defer tx_dir.close();
    const backup_dir_name = try std.testing.allocator.dupeZ(u8, &reference.transaction_id);
    defer std.testing.allocator.free(backup_dir_name);
    try tx_dir.writeFile(.{ .sub_path = "unexpected", .data = "foreign" });
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    try tx_dir.deleteFile("unexpected");
    if (c.fchmodat(backups.fd, backup_dir_name, 0o755, 0) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    if (c.fchmodat(backups.fd, backup_dir_name, 0o700, 0) != 0) return error.IoError;
    const metadata_fd = c.openat(tx_dir.fd, "metadata.bin", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (metadata_fd < 0) return error.IoError;
    defer _ = c.close(metadata_fd);
    if (c.fchmod(metadata_fd, 0o644) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    if (c.fchmod(metadata_fd, 0o600) != 0) return error.IoError;
    if (c.pwrite(metadata_fd, "Z", 1, 0) != 1 or c.fsync(metadata_fd) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    const restored_metadata = encode_backup_metadata(reference.transaction_id, "desktop:inventory", .{
        .hash = reference.sha256,
        .mode = reference.artifact_mode,
        .device = reference.device,
        .inode = reference.inode,
    });
    var wrong_metadata = encode_backup_metadata(reference.transaction_id, "desktop:inventory", .{
        .hash = reference.sha256,
        .mode = reference.artifact_mode,
        .device = reference.device + 1,
        .inode = reference.inode,
    });
    if (c.pwrite(metadata_fd, &wrong_metadata, wrong_metadata.len, 0) != wrong_metadata.len or c.fsync(metadata_fd) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    wrong_metadata = encode_backup_metadata(reference.transaction_id, "desktop:inventory", .{
        .hash = reference.sha256,
        .mode = reference.artifact_mode,
        .device = reference.device,
        .inode = reference.inode + 1,
    });
    if (c.pwrite(metadata_fd, &wrong_metadata, wrong_metadata.len, 0) != wrong_metadata.len or c.fsync(metadata_fd) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    if (c.pwrite(metadata_fd, &restored_metadata, restored_metadata.len, 0) != restored_metadata.len or c.fsync(metadata_fd) != 0) return error.IoError;
    const payload_name = hash_hex("desktop:inventory");
    var payload_z: [65]u8 = undefined;
    @memcpy(payload_z[0..64], &payload_name);
    payload_z[64] = 0;
    const backup_payload_fd = c.openat(tx_dir.fd, &payload_z, c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (backup_payload_fd < 0) return error.IoError;
    defer _ = c.close(backup_payload_fd);
    if (c.fchmod(backup_payload_fd, 0o644) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
    if (c.fchmod(backup_payload_fd, 0o600) != 0) return error.IoError;
    try expect_rewrite_file(backup_payload_fd, "tampered retained backup bytes");
    try std.testing.expectError(error.RecoveryRequired, checked.list_backups(std.testing.allocator, "desktop:inventory"));
}

test "backup creation rejects a full authenticated inventory before staging" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    try store.ensure_private_dir("backups");
    var backups = try state.openDir("backups", .{});
    defer backups.close();
    const record_id = "desktop:quota";
    const record_hash = hash_hex(record_id);
    var payload_name: [65]u8 = undefined;
    @memcpy(payload_name[0..64], &record_hash);
    payload_name[64] = 0;
    const payload = "retained quota evidence";
    const payload_hash = hash_bytes(payload);
    try backups.makeDir(".pending-test");
    try std.testing.expectError(error.RecoveryRequired, store.create_backup([_]u8{'f'} ** 32, record_id, payload, 0o644));
    try std.testing.expectError(error.RecoveryRequired, store.list_backups(std.testing.allocator, record_id));
    try backups.deleteDir(".pending-test");
    const digits = "0123456789abcdef";
    for (0..MaxBackups) |index| {
        var txid = [_]u8{'0'} ** 32;
        txid[30] = digits[index >> 4];
        txid[31] = digits[index & 0x0f];
        var txname: [33]u8 = undefined;
        @memcpy(txname[0..32], &txid);
        txname[32] = 0;
        const entry_record_id = if (index == 0) "desktop:other" else record_id;
        const entry_record_hash = hash_hex(entry_record_id);
        var entry_payload_name: [65]u8 = undefined;
        @memcpy(entry_payload_name[0..64], &entry_record_hash);
        entry_payload_name[64] = 0;
        if (c.mkdirat(backups.fd, &txname, 0o700) != 0) return error.IoError;
        const backup_fd = c.openat(backups.fd, &txname, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
        if (backup_fd < 0) return error.IoError;
        const payload_fd = c.openat(backup_fd, &entry_payload_name, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (payload_fd < 0) return error.IoError;
        try write_all(payload_fd, payload);
        var payload_stat: c.struct_stat = undefined;
        if (c.fstat(payload_fd, &payload_stat) != 0 or c.fsync(payload_fd) != 0 or c.close(payload_fd) != 0) return error.IoError;
        const info = file_info_t{
            .hash = payload_hash,
            .mode = 0o644,
            .device = @intCast(payload_stat.st_dev),
            .inode = @intCast(payload_stat.st_ino),
        };
        const metadata = encode_backup_metadata(txid, entry_record_id, info);
        const metadata_fd = c.openat(backup_fd, "metadata.bin", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
        if (metadata_fd < 0) return error.IoError;
        try write_all(metadata_fd, &metadata);
        if (c.fsync(metadata_fd) != 0 or c.close(metadata_fd) != 0 or c.fsync(backup_fd) != 0 or c.close(backup_fd) != 0) return error.IoError;
    }
    const overflow_id = [_]u8{'f'} ** 32;
    try std.testing.expectError(error.RecoveryRequired, store.create_backup(overflow_id, record_id, "next backup", 0o644));
    const excess_id = [_]u8{'e'} ** 32;
    var excess_name: [33]u8 = undefined;
    @memcpy(excess_name[0..32], &excess_id);
    excess_name[32] = 0;
    if (c.mkdirat(backups.fd, &excess_name, 0o700) != 0) return error.IoError;
    const excess_fd = c.openat(backups.fd, &excess_name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (excess_fd < 0) return error.IoError;
    const excess_payload_fd = c.openat(excess_fd, &payload_name, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (excess_payload_fd < 0) return error.IoError;
    try write_all(excess_payload_fd, payload);
    var excess_stat: c.struct_stat = undefined;
    if (c.fstat(excess_payload_fd, &excess_stat) != 0 or c.fsync(excess_payload_fd) != 0 or c.close(excess_payload_fd) != 0) return error.IoError;
    const excess_info = file_info_t{
        .hash = payload_hash,
        .mode = 0o644,
        .device = @intCast(excess_stat.st_dev),
        .inode = @intCast(excess_stat.st_ino),
    };
    const excess_metadata = encode_backup_metadata(excess_id, record_id, excess_info);
    const excess_metadata_fd = c.openat(excess_fd, "metadata.bin", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (excess_metadata_fd < 0) return error.IoError;
    try write_all(excess_metadata_fd, &excess_metadata);
    if (c.fsync(excess_metadata_fd) != 0 or c.close(excess_metadata_fd) != 0 or c.fsync(excess_fd) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, store.create_backup(overflow_id, record_id, "another backup", 0o644));
    try std.testing.expectError(error.RecoveryRequired, store.list_backups(std.testing.allocator, record_id));
    if (c.unlinkat(excess_fd, &payload_name, 0) != 0) return error.IoError;
    if (c.unlinkat(excess_fd, "metadata.bin", 0) != 0) return error.IoError;
    if (c.close(excess_fd) != 0) return error.IoError;
    if (c.unlinkat(backups.fd, &excess_name, c.AT_REMOVEDIR) != 0) return error.IoError;
    var inventory = try store.list_backups(std.testing.allocator, record_id);
    defer inventory.deinit();
    try std.testing.expectEqual(MaxBackups - 1, inventory.items.len);
}

test "fixture write helpers report invalid descriptor failures" {
    try std.testing.expectError(error.IoError, write_test_file(-1, "invalid", "bytes", 0o600));
    try std.testing.expectError(error.IoError, expect_rewrite_file(-1, "bytes"));
}

test "replacement inspection and inventory reject a closed store owner" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    store.deinit();
    try std.testing.expectError(error.Closed, store.inspect_record("desktop:closed"));
    try std.testing.expectError(error.Closed, store.list_backups(std.testing.allocator, "desktop:closed"));
    try std.testing.expectError(error.Closed, store.replace(.{
        .record_id = "desktop:closed",
        .expected_manifest_sha256 = [_]u8{0} ** 32,
        .expected_target = .{ .device = 1, .inode = 1, .mode = 0o644, .sha256 = [_]u8{0} ** 32 },
        .artifact_bytes = "bytes",
        .candidate_manifest = "{}",
    }));
    try std.testing.expectError(error.Closed, store.restore(.{
        .record_id = "desktop:closed",
        .expected_manifest_sha256 = [_]u8{0} ** 32,
        .expected_target = .{ .device = 1, .inode = 1, .mode = 0o644, .sha256 = [_]u8{0} ** 32 },
        .backup = .{ .transaction_id = [_]u8{0} ** 32, .record_id_sha256 = [_]u8{0} ** 32, .sha256 = [_]u8{0} ** 32, .artifact_mode = 0o644, .device = 1, .inode = 1 },
        .candidate_manifest = "{}",
    }));
}

test "empty backup inventory and record lookup fail without creating state" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[]}");
    if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    try std.testing.expectError(error.InvalidArgument, store.inspect_record(""));
    try std.testing.expectError(error.InvalidArgument, store.list_backups(std.testing.allocator, ""));
    try std.testing.expectError(error.FileNotFound, store.inspect_record("desktop:missing"));
    var backups = try store.list_backups(std.testing.allocator, "desktop:missing");
    defer backups.deinit();
    try std.testing.expectEqual(@as(usize, 0), backups.items.len);
}

test "init and recovery reject insecure or ambiguous filesystem scopes" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    try temporary.dir.makeDir("other");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    var other = try temporary.dir.openDir("other", .{});
    defer other.close();
    const state_path = try state.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const other_path = try other.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(other_path);
    const duplicate_kind = [_]root_t{
        .{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd },
        .{ .kind = .Data, .canonical_path = other_path, .dir_fd = other.fd },
    };
    try std.testing.expectError(error.InvalidArgument, store_t.init(std.testing.allocator, state.fd, &duplicate_kind));
    const overlapping = [_]root_t{
        .{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd },
        .{ .kind = .Config, .canonical_path = data_path, .dir_fd = data.fd },
    };
    try std.testing.expectError(error.InvalidArgument, store_t.init(std.testing.allocator, state.fd, &overlapping));
    var mode_dir = try temporary.dir.openDir("state", .{});
    defer mode_dir.close();
    if (c.fchmodat(temporary.dir.fd, "state", 0o755, 0) != 0) return error.IoError;
    const single = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    try std.testing.expectError(error.AccessDenied, store_t.init(std.testing.allocator, mode_dir.fd, &single));
    if (c.fchmodat(temporary.dir.fd, "state", 0o700, 0) != 0) return error.IoError;
    var store = try store_t.init(std.testing.allocator, mode_dir.fd, &single);
    defer store.deinit();
    var transactions = try mode_dir.openDir("transactions", .{});
    defer transactions.close();
    if (c.fchmodat(mode_dir.fd, "transactions", 0o755, 0) != 0) return error.IoError;
    try std.testing.expectError(error.AccessDenied, store.recover());
}

test "selected real POSIX call failures unwind or recover without mixed publication" {
    var observed_faults: usize = 0;
    var reached_success: bool = false;
    for (1..180) |fault_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const state_path = try state.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/fault.desktop", .{data_path});
        defer std.testing.allocator.free(target_path);
        const payload = "fault-injected content";
        const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:fault", payload);
        defer std.testing.allocator.free(candidate);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();

        injected_io_count = 0;
        injected_io_at = fault_index;
        const result = store.commit(.{ .record_id = "desktop:fault", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate });
        const injected = injected_io_at == 0;
        injected_io_at = 0;
        if (!injected) {
            try result;
            reached_success = true;
            const published = try data.readFileAlloc(std.testing.allocator, "applications/fault.desktop", 4096);
            defer std.testing.allocator.free(published);
            try std.testing.expectEqualStrings(payload, published);
            break;
        }
        observed_faults += 1;
        if (result) |_| {
            return error.InjectedIoFailureWasIgnored;
        } else |err| {
            switch (err) {
                error.IoError, error.RecoveryRequired, error.AccessDenied, error.ArtifactConflict => {},
                else => return err,
            }
        }
        try store.recover();
        const artifact_exists = if (data.access("applications/fault.desktop", .{})) |_| true else |_| false;
        const manifest_exists = if (state.access("manifest.json", .{})) |_| true else |_| false;
        try std.testing.expectEqual(artifact_exists, manifest_exists);
    }
    try std.testing.expect(reached_success);
    try std.testing.expect(observed_faults > 0);
}

test "recovery retries every injected POSIX read and mutation failure in prepared and committed phases" {
    for ([_]usize{ 8, 12 }) |checkpoint_step| {
        var observed_faults: usize = 0;
        var reached_success: bool = false;
        for (1..120) |fault_index| {
            var temporary = std.testing.tmpDir(.{});
            defer temporary.cleanup();
            if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
            try temporary.dir.makeDir("data");
            var state = try temporary.dir.openDir("state", .{});
            defer state.close();
            var data = try temporary.dir.openDir("data", .{});
            defer data.close();
            try data.makeDir("applications");
            const state_path = try state.realpathAlloc(std.testing.allocator, ".");
            defer std.testing.allocator.free(state_path);
            const data_path = try data.realpathAlloc(std.testing.allocator, ".");
            defer std.testing.allocator.free(data_path);
            const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/recovery-fault.desktop", .{data_path});
            defer std.testing.allocator.free(target_path);
            const payload = "recovery fault payload";
            const candidate = try test_manifest_bytes(std.testing.allocator, target_path, "desktop:recovery-fault", payload);
            defer std.testing.allocator.free(candidate);
            const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
            var store = try store_t.init(std.testing.allocator, state.fd, &roots);
            defer store.deinit();
            store.fail_after.store(checkpoint_step, .monotonic);
            try std.testing.expectError(error.InjectedFailure, store.commit(.{ .record_id = "desktop:recovery-fault", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
            store.fail_after.store(0, .monotonic);

            injected_io_count = 0;
            injected_io_at = fault_index;
            const result = store.recover();
            const injected = injected_io_at == 0;
            injected_io_at = 0;
            if (!injected) {
                try result;
                reached_success = true;
                if (checkpoint_step == 8) {
                    try std.testing.expectError(error.FileNotFound, data.access("applications/recovery-fault.desktop", .{}));
                    try std.testing.expectError(error.FileNotFound, state.access("manifest.json", .{}));
                } else {
                    const published = try data.readFileAlloc(std.testing.allocator, "applications/recovery-fault.desktop", 4096);
                    defer std.testing.allocator.free(published);
                    try std.testing.expectEqualStrings(payload, published);
                    const published_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
                    defer std.testing.allocator.free(published_manifest);
                    try std.testing.expectEqualStrings(candidate, published_manifest);
                }
                break;
            }
            observed_faults += 1;
            if (result) |_| {
                return error.InjectedIoFailureWasIgnored;
            } else |err| {
                switch (err) {
                    error.IoError, error.RecoveryRequired, error.AccessDenied, error.ArtifactConflict => {},
                    else => return err,
                }
            }
            try store.recover();
            if (checkpoint_step == 8) {
                try std.testing.expectError(error.FileNotFound, data.access("applications/recovery-fault.desktop", .{}));
                try std.testing.expectError(error.FileNotFound, state.access("manifest.json", .{}));
            } else {
                const published = try data.readFileAlloc(std.testing.allocator, "applications/recovery-fault.desktop", 4096);
                defer std.testing.allocator.free(published);
                try std.testing.expectEqualStrings(payload, published);
                const published_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
                defer std.testing.allocator.free(published_manifest);
                try std.testing.expectEqualStrings(candidate, published_manifest);
            }
        }
        try std.testing.expect(reached_success);
        try std.testing.expect(observed_faults > 0);
    }
}

test "ownership helpers accept exact files and reject foreign inode mode link type and I/O states" {
    defer {
        injected_io_at = 0;
        injected_eintr_at = 0;
        injected_short_at = 0;
        injected_zero_at = 0;
        injected_io_count = 0;
        injected_replace_fstatat_at = 0;
        injected_replace_fstatat_count = 0;
        injected_replace_fstatat_fd = -1;
    }
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    var applications = try data.openDir("applications", .{});
    defer applications.close();
    const state_path = try state.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    try write_test_file(applications.fd, "owned.desktop", "owned artifact", 0o644);
    try write_test_file(applications.fd, "temp-owned", "owned artifact", 0o644);
    try write_test_file(state.fd, "manifest.json", "owned manifest", 0o600);
    const artifact = (try store.inspect(data.fd, "applications/owned.desktop")).?;
    const temp_artifact = (try store.inspect(data.fd, "applications/temp-owned")).?;
    const owned_manifest = (try store.inspect_manifest()).?;

    try store.verify_named_file(applications.fd, "owned.desktop", artifact);
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(applications.fd, "missing", artifact));
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(applications.fd, "temp-owned", artifact));
    if (c.fchmodat(applications.fd, "owned.desktop", 0o600, 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(applications.fd, "owned.desktop", artifact));
    if (c.fchmodat(applications.fd, "owned.desktop", 0o644, 0) != 0) return error.IoError;
    try applications.symLink("owned.desktop", "symlinked", .{});
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(applications.fd, "symlinked", artifact));
    if (c.linkat(applications.fd, "owned.desktop", applications.fd, "hardlinked", 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(applications.fd, "hardlinked", artifact));
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(applications.fd, "absent", artifact, true, false));
    try store.validate_optional_temp(applications.fd, "absent", artifact, false, false);
    try store.validate_optional_temp(applications.fd, "absent", artifact, false, true);
    try store.validate_optional_temp(applications.fd, "temp-owned", temp_artifact, true, false);
    try store.validate_optional_temp(applications.fd, "temp-owned", temp_artifact, false, false);
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(applications.fd, "temp-owned", temp_artifact, false, true));
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(applications.fd, "hardlinked", temp_artifact, true, false));
    var wrong_temp = temp_artifact;
    wrong_temp.hash = hash_bytes("wrong temp bytes");
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(applications.fd, "temp-owned", wrong_temp, true, false));
    wrong_temp = temp_artifact;
    wrong_temp.mode = 0o600;
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(applications.fd, "temp-owned", wrong_temp, true, false));
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(applications.fd, "temp-owned", artifact));

    injected_io_count = 0;
    injected_io_at = 1;
    try std.testing.expectError(error.ArtifactConflict, store.verify_named_file(applications.fd, "owned.desktop", artifact));
    injected_io_at = 0;
    injected_io_count = 0;
    injected_io_at = 1;
    try std.testing.expectError(error.ArtifactConflict, store.validate_optional_temp(applications.fd, "temp-owned", temp_artifact, true, false));
    injected_io_at = 0;
    injected_io_count = 0;
    for (1..5) |fault_index| {
        injected_io_count = 0;
        injected_io_at = fault_index;
        const verify = store.verify_named_file(applications.fd, "owned.desktop", artifact);
        injected_io_at = 0;
        injected_io_count = 0;
        if (verify) |_| return error.InjectedIoFailureWasIgnored else |err| {
            try std.testing.expect(err == error.ArtifactConflict or err == error.IoError);
        }

        injected_io_count = 0;
        injected_io_at = fault_index;
        const validate = store.validate_optional_temp(applications.fd, "temp-owned", temp_artifact, true, false);
        injected_io_at = 0;
        injected_io_count = 0;
        if (validate) |_| return error.InjectedIoFailureWasIgnored else |err| {
            try std.testing.expect(err == error.ArtifactConflict or err == error.IoError);
        }
    }

    const txid: [32]u8 = "0123456789abcdef0123456789abcdef".*;
    try std.testing.expect(std.meta.activeTag(try store.unlink_owned_artifact(data.fd, "applications/temp-owned", temp_artifact, txid)) == .Unlinked);
    try std.testing.expect(std.meta.activeTag(try store.unlink_owned_artifact(data.fd, "applications/temp-owned", temp_artifact, txid)) == .AlreadyAbsent);
    try std.testing.expect(std.meta.activeTag(try store.unlink_owned_artifact(data.fd, "applications/owned.desktop", temp_artifact, txid)) == .KeptUnchanged);
    try std.testing.expect(std.meta.activeTag(try store.unlink_owned_artifact(data.fd, "applications/missing", artifact, txid)) == .AlreadyAbsent);
    try store.unlink_owned_manifest(owned_manifest);
    try std.testing.expectError(error.ArtifactConflict, store.unlink_owned_manifest(owned_manifest));

    try write_test_file(applications.fd, "named", "named owned", 0o600);
    var named_stat: c.struct_stat = undefined;
    if (c.fstatat(applications.fd, "named", &named_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
    const named_info = file_info_t{ .hash = hash_bytes("named owned"), .mode = 0o600, .device = @intCast(named_stat.st_dev), .inode = @intCast(named_stat.st_ino) };
    try unlink_named_if_matches(std.testing.allocator, applications.fd, "named", named_info);
    try unlink_named_if_matches(std.testing.allocator, applications.fd, "named", named_info);
    try write_test_file(applications.fd, "named-link-target", "named link", 0o600);
    var link_stat: c.struct_stat = undefined;
    if (c.fstatat(applications.fd, "named-link-target", &link_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
    const link_target_info = file_info_t{ .hash = hash_bytes("named link"), .mode = 0o600, .device = @intCast(link_stat.st_dev), .inode = @intCast(link_stat.st_ino) };
    try applications.symLink("named-link-target", "named-link", .{});
    try std.testing.expectError(error.ArtifactConflict, unlink_named_if_matches(std.testing.allocator, applications.fd, "named-link", link_target_info));
    try write_test_file(applications.fd, "named-mode", "named mode", 0o600);
    var mode_stat: c.struct_stat = undefined;
    if (c.fstatat(applications.fd, "named-mode", &mode_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
    const named_mode_info = file_info_t{ .hash = hash_bytes("named mode"), .mode = 0o600, .device = @intCast(mode_stat.st_dev), .inode = @intCast(mode_stat.st_ino) };
    if (c.fchmodat(applications.fd, "named-mode", 0o644, 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, unlink_named_if_matches(std.testing.allocator, applications.fd, "named-mode", named_mode_info));
    if (c.fchmodat(applications.fd, "named-mode", 0o600, 0) != 0) return error.IoError;
    if (c.linkat(applications.fd, "named-mode", applications.fd, "named-mode-hardlink", 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, unlink_named_if_matches(std.testing.allocator, applications.fd, "named-mode", named_mode_info));
    if (c.unlinkat(applications.fd, "named-mode-hardlink", 0) != 0) return error.IoError;
    try unlink_named_if_matches(std.testing.allocator, applications.fd, "named-mode", named_mode_info);
    try write_test_file(applications.fd, "named-hash", "original", 0o600);
    var hash_stat: c.struct_stat = undefined;
    if (c.fstatat(applications.fd, "named-hash", &hash_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
    const named_hash_info = file_info_t{ .hash = hash_bytes("original"), .mode = 0o600, .device = @intCast(hash_stat.st_dev), .inode = @intCast(hash_stat.st_ino) };
    const replacement_fd = c.openat(applications.fd, "named-hash", c.O_WRONLY | c.O_TRUNC | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (replacement_fd < 0) return error.IoError;
    defer _ = c.close(replacement_fd);
    try write_all(replacement_fd, "modified");
    try std.testing.expectError(error.ArtifactConflict, unlink_named_if_matches(std.testing.allocator, applications.fd, "named-hash", named_hash_info));
    if (c.unlinkat(applications.fd, "named-hash", 0) != 0) return error.IoError;
    try write_test_file(applications.fd, "named-foreign", "foreign", 0o600);
    try std.testing.expectError(error.ArtifactConflict, unlink_named_if_matches(std.testing.allocator, applications.fd, "named-foreign", named_info));
    try write_test_file(applications.fd, "named-special-mode", "special mode", 0o600);
    var special_stat: c.struct_stat = undefined;
    if (c.fstatat(applications.fd, "named-special-mode", &special_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
    const special_info = file_info_t{ .hash = hash_bytes("special mode"), .mode = 0o600, .device = @intCast(special_stat.st_dev), .inode = @intCast(special_stat.st_ino) };
    if (c.fchmodat(applications.fd, "named-special-mode", 0o1600, 0) != 0) return error.IoError;
    try std.testing.expectError(error.ArtifactConflict, unlink_named_if_matches(std.testing.allocator, applications.fd, "named-special-mode", special_info));
    injected_io_count = 0;
    injected_io_at = 1;
    try std.testing.expectError(error.IoError, unlink_named_if_matches(std.testing.allocator, applications.fd, "named-foreign", named_info));
    injected_io_at = 0;
    injected_io_count = 0;
    for (1..9) |fault_index| {
        try write_test_file(applications.fd, "named-io", "named I/O", 0o600);
        var io_stat: c.struct_stat = undefined;
        if (c.fstatat(applications.fd, "named-io", &io_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
        const io_info = file_info_t{ .hash = hash_bytes("named I/O"), .mode = 0o600, .device = @intCast(io_stat.st_dev), .inode = @intCast(io_stat.st_ino) };
        injected_io_count = 0;
        injected_io_at = fault_index;
        const result = unlink_named_if_matches(std.testing.allocator, applications.fd, "named-io", io_info);
        injected_io_at = 0;
        injected_io_count = 0;
        if (result) |_| {
            try std.testing.expectEqual(@as(usize, 8), fault_index);
        } else |err| {
            try std.testing.expect(fault_index < 8);
            try std.testing.expect(err == error.IoError or err == error.ArtifactConflict);
            try applications.access("named-io", .{});
            if (c.unlinkat(applications.fd, "named-io", 0) != 0) return error.IoError;
        }
    }

    var artifact_fault_success = false;
    for (1..40) |fault_index| {
        try write_test_file(applications.fd, "unlink-fault.desktop", "unlink fault", 0o644);
        const expected = (try store.inspect(data.fd, "applications/unlink-fault.desktop")).?;
        injected_io_count = 0;
        injected_io_at = fault_index;
        const result = store.unlink_owned_artifact(data.fd, "applications/unlink-fault.desktop", expected, txid);
        const faulted = injected_io_at == 0;
        injected_io_at = 0;
        injected_io_count = 0;
        if (result) |disposition| {
            // A fault on the return rename is reported as a kept staging file, not an error.
            // An injected fault inside verification is reported as a changed file and the
            // file is returned to its name; an injected fault on the return rename keeps it
            // at the staging name. Neither deletes the inode.
            if (faulted) {
                switch (disposition) {
                    .KeptUnchanged => {},
                    .KeptAtTempName => |name| {
                        var staged_name: [45]u8 = [_]u8{0} ** 45;
                        @memcpy(staged_name[0..44], &name);
                        const staged: [:0]const u8 = staged_name[0..44 :0];
                        if (c.unlinkat(applications.fd, staged.ptr, 0) != 0) return error.IoError;
                    },
                    else => return error.TestUnexpectedResult,
                }
                if (applications.access("unlink-fault.desktop", .{})) |_| {
                    if (c.unlinkat(applications.fd, "unlink-fault.desktop", 0) != 0) return error.IoError;
                } else |_| {}
            } else {
                try std.testing.expect(std.meta.activeTag(disposition) == .Unlinked);
            }
            artifact_fault_success = true;
            break;
        } else |err| {
            try std.testing.expect(faulted);
            try std.testing.expect(err == error.IoError or err == error.ArtifactConflict);
            // The recorded inode may sit at the staging name after an I/O error; the next
            // attempt's residue rule removes it, and this test clears it for the next case.
            _ = c.unlinkat(applications.fd, ".waddle-0123456789abcdef0123456789abcdef.tmp", 0);
            if (applications.access("unlink-fault.desktop", .{})) |_| {
                if (c.unlinkat(applications.fd, "unlink-fault.desktop", 0) != 0) return error.IoError;
            } else |_| {}
        }
    }
    try std.testing.expect(artifact_fault_success);

    try write_test_file(applications.fd, "race-target.desktop", "owned before race", 0o644);
    const race_artifact = (try store.inspect(data.fd, "applications/race-target.desktop")).?;
    try write_test_file(applications.fd, "race-source.desktop", "foreign replacement", 0o644);
    if (c.renameat(applications.fd, "race-source.desktop", applications.fd, "race-target.desktop") != 0) return error.IoError;
    try std.testing.expect(std.meta.activeTag(try store.unlink_owned_artifact(data.fd, "applications/race-target.desktop", race_artifact, txid)) == .KeptUnchanged);
    const raced_artifact = try applications.readFileAlloc(std.testing.allocator, "race-target.desktop", 4096);
    defer std.testing.allocator.free(raced_artifact);
    try std.testing.expectEqualStrings("foreign replacement", raced_artifact);

    var manifest_fault_success = false;
    for (1..9) |fault_index| {
        try write_test_file(state.fd, "manifest.json", "manifest fault", 0o600);
        const expected = (try store.inspect_manifest()).?;
        injected_io_count = 0;
        injected_io_at = fault_index;
        const result = store.unlink_owned_manifest(expected);
        const faulted = injected_io_at == 0;
        injected_io_at = 0;
        injected_io_count = 0;
        if (result) |_| {
            try std.testing.expect(!faulted);
            manifest_fault_success = true;
            break;
        } else |err| {
            try std.testing.expect(faulted);
            try std.testing.expect(err == error.IoError or err == error.ArtifactConflict or err == error.AccessDenied);
            if (state.access("manifest.json", .{})) |_| {
                if (c.unlinkat(state.fd, "manifest.json", 0) != 0) return error.IoError;
            } else |_| {}
        }
    }
    try std.testing.expect(manifest_fault_success);

    try write_test_file(state.fd, "manifest.json", "owned manifest before race", 0o600);
    try write_test_file(state.fd, "manifest-source", "foreign manifest", 0o600);
    const race_manifest = (try store.inspect_manifest()).?;
    injected_replace_fstatat_fd = state.fd;
    injected_replace_fstatat_source = "manifest-source";
    injected_replace_fstatat_target = "manifest.json";
    injected_replace_fstatat_count = 0;
    injected_replace_fstatat_at = 1;
    try std.testing.expectError(error.ArtifactConflict, store.unlink_owned_manifest(race_manifest));
    injected_replace_fstatat_at = 0;
    const raced_manifest = try state.readFileAlloc(std.testing.allocator, "manifest.json", 4096);
    defer std.testing.allocator.free(raced_manifest);
    try std.testing.expectEqualStrings("foreign manifest", raced_manifest);
}

test "recovery only removes empty owned cleanup directories and rejects unknown transaction entries" {
    for (0..6) |case_index| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        const state_path = try state.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(state_path);
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var store = try store_t.init(std.testing.allocator, state.fd, &roots);
        defer store.deinit();
        var transactions = try state.openDir("transactions", .{});
        defer transactions.close();
        const txid = [_]u8{'a'} ** 32;
        const clean = cleanup_name(txid);
        if (case_index == 0) {
            try transactions.makeDir("not-a-transaction");
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            try transactions.access("not-a-transaction", .{});
        } else if (case_index == 1) {
            const fd = c.openat(transactions.fd, "not-a-directory", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
            if (fd < 0) return error.IoError;
            _ = c.close(fd);
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            try transactions.access("not-a-directory", .{});
        } else if (case_index == 2) {
            if (c.mkdirat(transactions.fd, &clean, 0o700) != 0) return error.IoError;
            try store.recover();
            try std.testing.expectError(error.FileNotFound, transactions.access(clean[0..41], .{}));
        } else if (case_index == 3) {
            if (c.mkdirat(transactions.fd, &clean, 0o700) != 0) return error.IoError;
            var empty_cleanup = try transactions.openDir(clean[0..41], .{});
            defer empty_cleanup.close();
            try empty_cleanup.writeFile(.{ .sub_path = "foreign", .data = "retain" });
            try std.testing.expectError(error.IoError, store.recover());
            try empty_cleanup.access("foreign", .{});
        } else if (case_index == 4) {
            if (c.mkdirat(transactions.fd, &clean, 0o700) != 0) return error.IoError;
            var cleanup = try transactions.openDir(clean[0..41], .{});
            defer cleanup.close();
            try write_test_file(cleanup.fd, "journal.json", "known evidence", 0o600);
            if (c.linkat(cleanup.fd, "journal.json", cleanup.fd, "journal.link", 0) != 0) return error.IoError;
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            try cleanup.access("journal.json", .{});
            try cleanup.access("journal.link", .{});
        } else {
            if (c.mkdirat(transactions.fd, &clean, 0o700) != 0) return error.IoError;
            var cleanup = try transactions.openDir(clean[0..41], .{});
            defer cleanup.close();
            try cleanup.symLink("missing-target", "journal.json", .{});
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            var symlink_stat: c.struct_stat = undefined;
            if (c.fstatat(cleanup.fd, "journal.json", &symlink_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
            try std.testing.expectEqual(@as(c_int, c.S_IFLNK), @as(c_int, @intCast(symlink_stat.st_mode & c.S_IFMT)));
        }
    }
}

test "recovery enforces the configured transaction-directory bound" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    const state_path = try state.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(state_path);
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    var transactions = try state.openDir("transactions", .{});
    defer transactions.close();
    for (0..MaxJournals + 1) |index| {
        var txid = [_]u8{'a'} ** 32;
        const digits = "0123456789abcdef";
        txid[0] = digits[index / 16];
        txid[1] = digits[index % 16];
        const name = cleanup_name(txid);
        if (c.mkdirat(transactions.fd, &name, 0o700) != 0) return error.IoError;
    }
    try std.testing.expectError(error.RecoveryRequired, store.recover());
}

/// Fixture record. `extra` holds raw members ending in a comma (for example a
/// guest device) and `prior` holds the raw JSON prior value.
const test_spec_t = struct {
    id: []const u8,
    kind: []const u8,
    path: []const u8,
    sha256: []const u8,
    extra: []const u8 = "",
    prior: []const u8 = "null",
};

/// Build a schema document with `null` priors for the requested records.
/// Fixture allocation and formatting failures are test bugs, so they panic
/// instead of adding error-only branches to the test-only coverage denominator.
fn test_document(allocator: std.mem.Allocator, schema: u32, specs: []const test_spec_t) []u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    out.writer().print("{{\"schema\":{d},\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[", .{schema}) catch @panic("test fixture");
    for (specs, 0..) |spec, index| {
        if (index != 0) out.append(',') catch @panic("test fixture");
        out.writer().print("{{\"id\":\"{s}\",\"kind\":\"{s}\",\"path\":\"{s}\",\"sha256\":\"{s}\",{s}\"created_by\":\"0.2.0\",\"prior\":{s}}}", .{ spec.id, spec.kind, spec.path, spec.sha256, spec.extra, spec.prior }) catch @panic("test fixture");
    }
    out.appendSlice("]}") catch @panic("test fixture");
    return out.toOwnedSlice() catch @panic("test fixture");
}

/// Create a regular file under `dir` and set its exact permission bits.
/// Failures panic because they indicate a broken test fixture, not a store result.
fn write_status_file(dir: std.fs.Dir, sub_path: [:0]const u8, bytes: []const u8, mode: c_uint) void {
    dir.writeFile(.{ .sub_path = sub_path, .data = bytes }) catch @panic("test fixture");
    if (c.fchmodat(dir.fd, sub_path.ptr, mode, 0) != 0) @panic("test fixture");
}

test "status classifies each recorded artifact class without mutating any file" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};

    write_status_file(data, "applications/ok.desktop", "ok bytes", 0o644);
    write_status_file(data, "applications/bin.desktop", "bin bytes", 0o755);
    write_status_file(data, "applications/changed.desktop", "changed bytes", 0o644);
    write_status_file(data, "applications/mode.desktop", "mode bytes", 0o600);
    write_status_file(data, "applications/group.desktop", "group bytes", 0o664);
    write_status_file(data, "applications/setuid.desktop", "setuid bytes", 0o4644);
    write_status_file(data, "applications/hard-source.desktop", "hard bytes", 0o644);
    if (c.linkat(data.fd, "applications/hard-source.desktop", data.fd, "applications/hard.desktop", 0) != 0) return error.IoError;
    if (c.symlinkat("ok.desktop", data.fd, "applications/link.desktop") != 0) return error.IoError;
    if (c.symlinkat(".", data.fd, "applications/linkdir") != 0) return error.IoError;
    write_status_file(data, "applications/plain", "not a directory", 0o644);
    {
        var huge = try data.createFile("applications/huge.desktop", .{});
        defer huge.close();
        try huge.setEndPos(MaxArtifactBytes + 1);
    }

    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const digest_ok = hash_hex("ok bytes");
    const digest_bin = hash_hex("bin bytes");
    const digest_original = hash_hex("original bytes");
    const digest_mode = hash_hex("mode bytes");
    const digest_group = hash_hex("group bytes");
    const digest_setuid = hash_hex("setuid bytes");
    const digest_hard = hash_hex("hard bytes");
    const digest_absent = hash_hex("never written");
    const specs = [_]test_spec_t{
        .{ .id = "desktop:ok", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/ok.desktop", .{data_path}), .sha256 = &digest_ok },
        .{ .id = "handler:bin", .kind = "handler_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/bin.desktop", .{data_path}), .sha256 = &digest_bin },
        .{ .id = "desktop:changed", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/changed.desktop", .{data_path}), .sha256 = &digest_original },
        .{ .id = "desktop:mode", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/mode.desktop", .{data_path}), .sha256 = &digest_mode },
        .{ .id = "desktop:group", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/group.desktop", .{data_path}), .sha256 = &digest_group },
        .{ .id = "desktop:setuid", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/setuid.desktop", .{data_path}), .sha256 = &digest_setuid },
        .{ .id = "desktop:hard", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/hard.desktop", .{data_path}), .sha256 = &digest_hard },
        .{ .id = "desktop:link", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/link.desktop", .{data_path}), .sha256 = &digest_ok },
        .{ .id = "desktop:parent-link", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/linkdir/ok.desktop", .{data_path}), .sha256 = &digest_ok },
        .{ .id = "desktop:parent-file", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/plain/x.desktop", .{data_path}), .sha256 = &digest_ok },
        .{ .id = "desktop:missing-parent", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/nodir/x.desktop", .{data_path}), .sha256 = &digest_ok },
        .{ .id = "desktop:missing", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/missing.desktop", .{data_path}), .sha256 = &digest_absent },
        .{ .id = "desktop:huge", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/applications/huge.desktop", .{data_path}), .sha256 = &digest_absent },
        .{ .id = "desktop:outside", .kind = "desktop_entry", .path = try std.fmt.allocPrint(scratch, "{s}/outside.desktop", .{data_path}), .sha256 = &digest_ok },
        .{ .id = "autostart:no-config", .kind = "autostart", .path = "/unused-config/autostart/x.desktop", .sha256 = &digest_ok },
        .{ .id = "binfmt:system", .kind = "binfmt", .path = "/etc/binfmt.d/waddle.conf", .sha256 = &digest_ok },
    };
    const expected = [_]record_state_t{ .Ok, .Ok, .Modified, .Foreign, .Foreign, .Foreign, .Foreign, .Foreign, .Foreign, .Foreign, .Missing, .Missing, .Modified, .Foreign, .Foreign, .Foreign };
    const manifest_bytes = test_document(scratch, 1, &specs);
    const manifest_fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (manifest_fd < 0) return error.IoError;
    try write_all(manifest_fd, manifest_bytes);
    if (c.fsync(manifest_fd) != 0 or c.close(manifest_fd) != 0) return error.IoError;

    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    for (0..2) |_| {
        var list = try store.status(std.testing.allocator);
        defer list.deinit();
        try std.testing.expectEqual(expected.len, list.items.len);
        for (expected, list.items, specs) |want, got, spec| {
            try std.testing.expectEqualStrings(spec.id, got.record_id);
            try std.testing.expectEqual(want, got.state);
        }
    }
    const stored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(stored);
    try std.testing.expectEqualStrings(manifest_bytes, stored);
    const changed = try data.readFileAlloc(std.testing.allocator, "applications/changed.desktop", 4096);
    defer std.testing.allocator.free(changed);
    try std.testing.expectEqualStrings("changed bytes", changed);
}

test "status returns an empty snapshot without a manifest and rejects unsafe manifests unchanged" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    {
        var empty = try store.status(std.testing.allocator);
        defer empty.deinit();
        try std.testing.expectEqual(@as(usize, 0), empty.items.len);
        empty.deinit();
        try std.testing.expectEqual(@as(usize, 0), empty.items.len);
    }
    const newer = "{\"schema\":2,\"waddle_version\":\"0.3.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[]}";
    try write_test_file(state.fd, "manifest.json", newer, 0o600);
    try std.testing.expectError(error.UnsupportedSchema, store.status(std.testing.allocator));
    const stored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
    defer std.testing.allocator.free(stored);
    try std.testing.expectEqualStrings(newer, stored);

    if (c.unlinkat(state.fd, "manifest.json", 0) != 0) return error.IoError;
    try write_test_file(state.fd, "manifest.json", "{}", 0o644);
    try std.testing.expectError(error.AccessDenied, store.status(std.testing.allocator));
    if (c.unlinkat(state.fd, "manifest.json", 0) != 0) return error.IoError;
    const duplicate_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/dup.desktop", .{data_path});
    defer std.testing.allocator.free(duplicate_path);
    const digest = hash_hex("dup");
    const duplicated = [_]test_spec_t{
        .{ .id = "desktop:one", .kind = "desktop_entry", .path = duplicate_path, .sha256 = &digest },
        .{ .id = "desktop:two", .kind = "desktop_entry", .path = duplicate_path, .sha256 = &digest },
    };
    const overlapping = test_document(std.testing.allocator, 1, &duplicated);
    defer std.testing.allocator.free(overlapping);
    try write_test_file(state.fd, "manifest.json", overlapping, 0o600);
    try std.testing.expectError(error.ManifestMismatch, store.status(std.testing.allocator));
    if (c.unlinkat(state.fd, "manifest.json", 0) != 0) return error.IoError;
    store.deinit();
    try std.testing.expectError(error.Closed, store.status(std.testing.allocator));
}

test "status retries after every selected injected I/O failure without mutating state" {
    defer {
        injected_io_at = 0;
        injected_io_count = 0;
    }
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    try data.makeDir("applications");
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    write_status_file(data, "applications/ok.desktop", "ok bytes", 0o644);
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/ok.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);
    const digest = hash_hex("ok bytes");
    const specs = [_]test_spec_t{.{ .id = "desktop:ok", .kind = "desktop_entry", .path = target_path, .sha256 = &digest }};
    const manifest_bytes = test_document(std.testing.allocator, 1, &specs);
    defer std.testing.allocator.free(manifest_bytes);
    try write_test_file(state.fd, "manifest.json", manifest_bytes, 0o600);
    const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();

    var observed_failures: usize = 0;
    for (1..200) |failure_io| {
        injected_io_at = failure_io;
        injected_io_count = 0;
        const outcome = store.status(std.testing.allocator);
        injected_io_at = 0;
        if (outcome) |list_value| {
            var list = list_value;
            defer list.deinit();
            try std.testing.expectEqual(@as(usize, 1), list.items.len);
            try std.testing.expectEqual(record_state_t.Ok, list.items[0].state);
            break;
        } else |err| {
            // An injected fstat fault is mapped to AccessDenied by the existing
            // read_manifest and open_private_dir fstat checks; every other
            // injected fault is reported as IoError. Both must fail closed.
            observed_failures += 1;
            try std.testing.expect(err == error.IoError or err == error.AccessDenied);
        }
        const stored = try state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
        defer std.testing.allocator.free(stored);
        try std.testing.expectEqualStrings(manifest_bytes, stored);
        const payload = try data.readFileAlloc(std.testing.allocator, "applications/ok.desktop", 4096);
        defer std.testing.allocator.free(payload);
        try std.testing.expectEqualStrings("ok bytes", payload);
    }
    try std.testing.expect(observed_failures > 0);
}

test "status allocation failures leave no leaks and every retry classifies the same records" {
    var observed_failures: usize = 0;
    for (0..512) |failure_offset| {
        var temporary = std.testing.tmpDir(.{});
        defer temporary.cleanup();
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
        try temporary.dir.makeDir("data");
        var state = try temporary.dir.openDir("state", .{});
        defer state.close();
        var data = try temporary.dir.openDir("data", .{});
        defer data.close();
        try data.makeDir("applications");
        const data_path = try data.realpathAlloc(std.testing.allocator, ".");
        defer std.testing.allocator.free(data_path);
        write_status_file(data, "applications/ok.desktop", "ok bytes", 0o644);
        write_status_file(data, "applications/changed.desktop", "changed bytes", 0o644);
        const ok_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/ok.desktop", .{data_path});
        defer std.testing.allocator.free(ok_path);
        const changed_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/changed.desktop", .{data_path});
        defer std.testing.allocator.free(changed_path);
        const ok_digest = hash_hex("ok bytes");
        const original_digest = hash_hex("original bytes");
        const specs = [_]test_spec_t{
            .{ .id = "desktop:ok", .kind = "desktop_entry", .path = ok_path, .sha256 = &ok_digest },
            .{ .id = "desktop:changed", .kind = "desktop_entry", .path = changed_path, .sha256 = &original_digest },
        };
        const manifest_bytes = test_document(std.testing.allocator, 1, &specs);
        defer std.testing.allocator.free(manifest_bytes);
        try write_test_file(state.fd, "manifest.json", manifest_bytes, 0o600);
        const roots = [_]root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        var store = try store_t.init(failing.allocator(), state.fd, &roots);
        failing.fail_index = failing.alloc_index + failure_offset;
        const outcome = store.status(failing.allocator());
        if (!failing.has_induced_failure) {
            var list = try outcome;
            list.deinit();
            store.deinit();
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed_failures += 1;
        if (outcome) |_| {
            return error.ExpectedOutOfMemory;
        } else |err| if (err != error.OutOfMemory) return err;
        failing.fail_index = std.math.maxInt(usize);
        var list = try store.status(failing.allocator());
        try std.testing.expectEqual(@as(usize, 2), list.items.len);
        try std.testing.expectEqual(record_state_t.Ok, list.items[0].state);
        try std.testing.expectEqual(record_state_t.Modified, list.items[1].state);
        list.deinit();
        store.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    try std.testing.expect(observed_failures > 0);
}

/// Temporary state and Data-root fixture for remove tests. Setup failures panic
/// because they are test bugs; store results are asserted by the tests.
const remove_fixture_t = struct {
    temporary: std.testing.TmpDir,
    state: std.fs.Dir,
    data: std.fs.Dir,
    data_path: []u8,

    fn create(allocator: std.mem.Allocator) remove_fixture_t {
        var temporary = std.testing.tmpDir(.{});
        if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) @panic("fixture state");
        temporary.dir.makeDir("data") catch @panic("fixture data");
        const state = temporary.dir.openDir("state", .{}) catch @panic("fixture state open");
        var data = temporary.dir.openDir("data", .{}) catch @panic("fixture data open");
        data.makeDir("applications") catch @panic("fixture applications");
        const data_path = data.realpathAlloc(allocator, ".") catch @panic("fixture path");
        return .{ .temporary = temporary, .state = state, .data = data, .data_path = data_path };
    }

    fn destroy(self: *remove_fixture_t, allocator: std.mem.Allocator) void {
        self.data.close();
        self.state.close();
        allocator.free(self.data_path);
        self.temporary.cleanup();
    }

    fn roots(self: *const remove_fixture_t) [1]root_t {
        return .{.{ .kind = .Data, .canonical_path = self.data_path, .dir_fd = self.data.fd }};
    }
};

/// Two managed artifacts and their documents. `original` holds target and keep;
/// `candidate` holds only keep, which is the required removal candidate.
const remove_docs_t = struct { original: []u8, candidate: []u8 };

fn remove_documents(arena: std.mem.Allocator, data_path: []const u8) remove_docs_t {
    const target_digest = hash_hex("target bytes");
    const keep_digest = hash_hex("keep bytes");
    const target_path = std.fmt.allocPrint(arena, "{s}/applications/target.desktop", .{data_path}) catch @panic("fixture path");
    const keep_path = std.fmt.allocPrint(arena, "{s}/applications/keep.desktop", .{data_path}) catch @panic("fixture path");
    const target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &target_digest };
    const keep = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
    const both = [_]test_spec_t{ target, keep };
    const only_keep = [_]test_spec_t{keep};
    return .{ .original = test_document(arena, 1, &both), .candidate = test_document(arena, 1, &only_keep) };
}

/// Install the standard artifacts (target and keep) with their exact modes.
fn install_remove_artifacts(fixture: *remove_fixture_t) void {
    write_status_file(fixture.data, "applications/target.desktop", "target bytes", 0o644);
    write_status_file(fixture.data, "applications/keep.desktop", "keep bytes", 0o644);
}

/// Owned copies of the files that a remove test classifies. `target` is null
/// when the recorded artifact is absent. Fixture reads panic on other errors.
const remove_snapshot_t = struct { manifest: []u8, keep: []u8, target: ?[]u8 };

fn take_remove_snapshot(fixture: *remove_fixture_t) remove_snapshot_t {
    const manifest_bytes = fixture.state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes) catch @panic("fixture manifest");
    const keep = fixture.data.readFileAlloc(std.testing.allocator, "applications/keep.desktop", 4096) catch @panic("fixture keep");
    const target: ?[]u8 = fixture.data.readFileAlloc(std.testing.allocator, "applications/target.desktop", 4096) catch null;
    return .{ .manifest = manifest_bytes, .keep = keep, .target = target };
}

fn free_remove_snapshot(snapshot: remove_snapshot_t) void {
    std.testing.allocator.free(snapshot.manifest);
    std.testing.allocator.free(snapshot.keep);
    if (snapshot.target) |target| std.testing.allocator.free(target);
}

/// Read the stored manifest bytes; a missing manifest is a fixture failure here.
fn read_stored_manifest(state: std.fs.Dir) []u8 {
    return state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes) catch @panic("fixture manifest");
}

/// Report whether `applications/target.desktop` exists under the Data root.
fn target_exists(data: std.fs.Dir) bool {
    data.access("applications/target.desktop", .{}) catch return false;
    return true;
}

test "remove deletes the matching artifact and record and repeats as already missing" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();

    const receipt = try store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
    try std.testing.expectEqual(remove_outcome_t.Removed, receipt.outcome);
    try std.testing.expect(receipt.record_found);
    try std.testing.expect(std.meta.activeTag(receipt.prior) == .None);
    try std.testing.expect(!target_exists(fixture.data));
    {
        const snapshot = take_remove_snapshot(&fixture);
        defer free_remove_snapshot(snapshot);
        try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
        if (snapshot.target) |target| {
            try std.testing.expectEqualStrings("target bytes", target);
            try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
        } else {
            try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
        }
    }

    const repeat = try store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
    try std.testing.expectEqual(remove_outcome_t.AlreadyMissing, repeat.outcome);
    try std.testing.expect(!repeat.record_found);
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(docs.candidate, on_disk);
    }
}

test "remove reports every non-removable record without changing any file or record" {
    for (0..6) |case_index| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        write_status_file(fixture.data, "applications/keep.desktop", "keep bytes", 0o644);
        switch (case_index) {
            0 => write_status_file(fixture.data, "applications/target.desktop", "edited bytes", 0o644),
            1 => if (c.symlinkat("keep.desktop", fixture.data.fd, "applications/target.desktop") != 0) @panic("fixture symlink"),
            2 => {},
            3 => write_status_file(fixture.data, "applications/target.desktop", "target bytes", 0o600),
            4 => {
                write_status_file(fixture.data, "applications/other.desktop", "target bytes", 0o644);
                if (c.linkat(fixture.data.fd, "applications/other.desktop", fixture.data.fd, "applications/target.desktop", 0) != 0) @panic("fixture hardlink");
            },
            else => write_status_file(fixture.data, "applications/target.desktop", "target bytes", 0o644),
        }
        const stored = if (case_index == 5) docs.candidate else docs.original;
        try write_test_file(fixture.state.fd, "manifest.json", stored, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();

        const receipt = try store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
        const expected: remove_outcome_t = switch (case_index) {
            0 => .KeptModified,
            1, 3, 4 => .KeptForeign,
            2 => .RecordRemovedArtifactMissing,
            else => .AlreadyMissing,
        };
        try std.testing.expectEqual(expected, receipt.outcome);
        try std.testing.expectEqual(case_index != 5, receipt.record_found);
        if (case_index == 2) {
            const removed = read_stored_manifest(fixture.state);
            defer std.testing.allocator.free(removed);
            try std.testing.expectEqualStrings(docs.candidate, removed);
            continue;
        }
        {
            const on_disk = read_stored_manifest(fixture.state);
            defer std.testing.allocator.free(on_disk);
            try std.testing.expectEqualStrings(stored, on_disk);
        }
        const keep = try fixture.data.readFileAlloc(std.testing.allocator, "applications/keep.desktop", 4096);
        defer std.testing.allocator.free(keep);
        try std.testing.expectEqualStrings("keep bytes", keep);
    }
}

test "remove without a manifest reports already missing and creates nothing" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    const receipt = try store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
    try std.testing.expectEqual(remove_outcome_t.AlreadyMissing, receipt.outcome);
    try std.testing.expect(!receipt.record_found);
    try std.testing.expectError(error.FileNotFound, fixture.state.access("manifest.json", .{}));
}

test "remove returns the record prior value as an owned receipt for every outcome" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const text_digest = hash_hex("binfmt bytes");
    const binfmt_prior = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const binfmt_path = "/etc/binfmt.d/waddle.conf";
    const policy_digest = hash_hex("policy bytes");
    const binfmt = test_spec_t{ .id = "binfmt:target", .kind = "binfmt", .path = binfmt_path, .sha256 = &text_digest, .prior = "\"" ++ binfmt_prior ++ "\"" };
    const policy = test_spec_t{ .id = "policy:target", .kind = "guest_policy", .path = "HKLM-Policy-Waddle", .sha256 = &policy_digest, .extra = "\"device\":\"default\",", .prior = "7" };
    const specs = [_]test_spec_t{ binfmt, policy };
    const document = test_document(scratch, 1, &specs);
    const policy_only = test_document(scratch, 1, &[_]test_spec_t{policy});
    const empty = test_document(scratch, 1, &[_]test_spec_t{});
    try write_test_file(fixture.state.fd, "manifest.json", document, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();

    const text_receipt = try store.remove(.{ .record_id = "binfmt:target", .candidate_manifest = policy_only });
    try std.testing.expectEqual(remove_outcome_t.RecordRemovedNonFile, text_receipt.outcome);
    try std.testing.expect(text_receipt.record_found);
    try std.testing.expectEqualStrings(binfmt_prior, text_receipt.prior.Text.bytes[0..text_receipt.prior.Text.len]);
    const policy_receipt = try store.remove(.{ .record_id = "policy:target", .candidate_manifest = empty });
    try std.testing.expectEqual(remove_outcome_t.RecordRemovedNonFile, policy_receipt.outcome);
    try std.testing.expectEqual(@as(u32, 7), policy_receipt.prior.U32Value);
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(empty, on_disk);
}

test "remove rejects every candidate mismatch and malformed request before mutation" {
    for (0..10) |case_index| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const scratch = arena.allocator();
        const docs = remove_documents(scratch, fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();

        const keep_digest = hash_hex("changed keep bytes");
        const keep_path = std.fmt.allocPrint(scratch, "{s}/applications/keep.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const other_digest = hash_hex("unrelated");
        const other_path = std.fmt.allocPrint(scratch, "{s}/applications/other.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const keep_changed = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
        const other = test_spec_t{ .id = "desktop:other", .kind = "desktop_entry", .path = other_path, .sha256 = &other_digest };
        const same_path = test_spec_t{ .id = "desktop:same", .kind = "desktop_entry", .path = keep_path, .sha256 = &other_digest };
        const keep_only = [_]test_spec_t{keep_changed};
        const other_only = [_]test_spec_t{other};
        const overlapping = [_]test_spec_t{ keep_changed, same_path };
        const oversized = scratch.alloc(u8, manifest.MaxManifestBytes + 1) catch @panic("fixture size");
        @memset(oversized, 'x');
        const candidate: []const u8 = switch (case_index) {
            0 => docs.original,
            1 => test_document(scratch, 1, &.{}),
            2 => test_document(scratch, 1, &keep_only),
            3 => test_document(scratch, 1, &other_only),
            4 => test_document(scratch, 2, &keep_only),
            5 => "{",
            6 => "",
            8 => test_document(scratch, 1, &overlapping),
            9 => oversized,
            else => docs.candidate,
        };
        const record_id: []const u8 = if (case_index == 7) "" else "desktop:target";
        const expected: anyerror = switch (case_index) {
            0, 1, 2, 3, 8 => error.ManifestMismatch,
            4 => error.UnsupportedSchema,
            5 => error.InvalidManifest,
            6, 7, 9 => error.InvalidArgument,
            else => unreachable,
        };
        try std.testing.expectError(expected, store.remove(.{ .record_id = record_id, .candidate_manifest = candidate }));
        {
            const on_disk = read_stored_manifest(fixture.state);
            defer std.testing.allocator.free(on_disk);
            try std.testing.expectEqualStrings(docs.original, on_disk);
        }
        const target = try fixture.data.readFileAlloc(std.testing.allocator, "applications/target.desktop", 4096);
        defer std.testing.allocator.free(target);
        try std.testing.expectEqualStrings("target bytes", target);
    }
}

test "remove recovers after every injected durable checkpoint and a second recovery is a no-op" {
    var completed = false;
    var observed_failures: usize = 0;
    for (1..48) |step| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + step, .monotonic);
        const outcome = store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
        store.fail_after.store(0, .monotonic);
        if (outcome) |receipt| {
            try std.testing.expectEqual(remove_outcome_t.Removed, receipt.outcome);
            {
                const snapshot = take_remove_snapshot(&fixture);
                defer free_remove_snapshot(snapshot);
                try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
                if (snapshot.target) |target| {
                    try std.testing.expectEqualStrings("target bytes", target);
                    try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
                } else {
                    try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
                }
            }
            completed = true;
            break;
        } else |err| {
            observed_failures += 1;
            try std.testing.expectEqual(error.InjectedFailure, err);
        }
        try store.recover();
        {
            const snapshot = take_remove_snapshot(&fixture);
            defer free_remove_snapshot(snapshot);
            try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
            if (snapshot.target) |target| {
                try std.testing.expectEqualStrings("target bytes", target);
                try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
            } else {
                try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
            }
        }
        try store.recover();
        {
            const snapshot = take_remove_snapshot(&fixture);
            defer free_remove_snapshot(snapshot);
            try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
            if (snapshot.target) |target| {
                try std.testing.expectEqualStrings("target bytes", target);
                try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
            } else {
                try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
            }
        }
    }
    try std.testing.expect(completed);
    try std.testing.expect(observed_failures > 0);
}

test "remove retries after every selected injected I/O failure and converges" {
    defer {
        injected_io_at = 0;
        injected_io_count = 0;
    }
    var completed = false;
    var observed_failures: usize = 0;
    for (1..320) |failure_io| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        injected_io_at = failure_io;
        injected_io_count = 0;
        const outcome = store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
        injected_io_at = 0;
        injected_io_count = 0;
        if (outcome) |receipt| {
            try std.testing.expectEqual(remove_outcome_t.Removed, receipt.outcome);
            completed = true;
            break;
        } else |err| {
            observed_failures += 1;
            try std.testing.expect(err != error.InvalidArgument and err != error.OutOfMemory and err != error.InjectedFailure);
        }
        try store.recover();
        {
            const snapshot = take_remove_snapshot(&fixture);
            defer free_remove_snapshot(snapshot);
            try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
            if (snapshot.target) |target| {
                try std.testing.expectEqualStrings("target bytes", target);
                try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
            } else {
                try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
            }
        }
        try store.recover();
        {
            const snapshot = take_remove_snapshot(&fixture);
            defer free_remove_snapshot(snapshot);
            try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
            if (snapshot.target) |target| {
                try std.testing.expectEqualStrings("target bytes", target);
                try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
            } else {
                try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
            }
        }
    }
    try std.testing.expect(completed);
    try std.testing.expect(observed_failures > 0);
}

test "remove allocation failures leave a recoverable state and no leaked allocations" {
    var observed_failures: usize = 0;
    for (0..512) |failure_offset| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        var store = try store_t.init(failing.allocator(), fixture.state.fd, &roots);
        failing.fail_index = failing.alloc_index + failure_offset;
        const outcome = store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
        if (!failing.has_induced_failure) {
            _ = try outcome;
            store.deinit();
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed_failures += 1;
        if (outcome) |_| {
            return error.ExpectedOutOfMemory;
        } else |err| if (err != error.OutOfMemory) return err;
        failing.fail_index = std.math.maxInt(usize);
        try store.recover();
        {
            const snapshot = take_remove_snapshot(&fixture);
            defer free_remove_snapshot(snapshot);
            try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
            if (snapshot.target) |target| {
                try std.testing.expectEqualStrings("target bytes", target);
                try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
            } else {
                try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
            }
        }
        store.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    try std.testing.expect(observed_failures > 0);
}

/// Return the single transaction identifier under `state/transactions`, or null
/// when none exists. More than one transaction is a fixture bug.
fn optional_transaction_id(state: std.fs.Dir) ?[32]u8 {
    var transactions = state.openDir("transactions", .{ .iterate = true }) catch @panic("fixture transactions");
    defer transactions.close();
    var iterator = transactions.iterate();
    const first = (iterator.next() catch @panic("fixture iterate")) orelse return null;
    if ((iterator.next() catch @panic("fixture iterate")) != null) @panic("fixture multiple transactions");
    var result: [32]u8 = undefined;
    @memcpy(&result, first.name[0..32]);
    return result;
}

/// Replace one private transaction file with exact bytes and mode. The file is
/// recreated exclusively so tampering never follows an existing link.
fn replace_transaction_file(state: std.fs.Dir, txid: [32]u8, leaf: [:0]const u8, bytes: []const u8, mode: c_uint) void {
    var transactions = state.openDir("transactions", .{}) catch @panic("fixture transactions");
    defer transactions.close();
    var tx = transactions.openDir(&txid, .{}) catch @panic("fixture transaction");
    defer tx.close();
    _ = c.unlinkat(tx.fd, leaf.ptr, 0);
    const fd = c.openat(tx.fd, leaf.ptr, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, mode);
    if (fd < 0) @panic("fixture transaction file");
    defer _ = c.close(fd);
    write_all(fd, bytes) catch @panic("fixture transaction write");
    if (c.fsync(fd) != 0) @panic("fixture transaction sync");
}

/// Build one tampered version-3 journal. Cases 0 through 18 mutate a single
/// immutable field, cases 19 through 21 change the raw text, and every case
/// must be rejected before any filesystem mutation.
fn tampered_journal(allocator: std.mem.Allocator, journal: journal_v3_t, case_index: usize, original_text: []const u8) []u8 {
    var mutated = journal;
    switch (case_index) {
        0 => mutated.version = 2,
        1 => mutated.transaction_id = "00000000000000000000000000000000",
        2 => mutated.phase = "bogus",
        3 => mutated.operation = "bogus",
        4 => mutated.record_id = "",
        5 => mutated.old_manifest_hash = "xyz",
        6 => mutated.new_manifest_hash = journal.old_manifest_hash,
        7 => mutated.old_manifest_device = 0,
        8 => mutated.new_manifest_inode = 0,
        9 => mutated.root = null,
        10 => mutated.relative_path = null,
        11 => mutated.old_artifact_hash = "xyz",
        12 => mutated.old_artifact_mode = 0o600,
        13 => mutated.old_artifact_device = null,
        14 => mutated.old_artifact_inode = 0,
        15 => mutated.root = "config",
        16 => mutated.relative_path = "applications/other.desktop",
        17 => mutated.operation = "migrate",
        18 => mutated.record_id = "desktop:unknown",
        19 => return std.fmt.allocPrint(allocator, "{s},\"unknown\":1}}", .{original_text[0 .. original_text.len - 1]}) catch @panic("fixture journal"),
        20 => return std.fmt.allocPrint(allocator, "{s},\"version\":3}}", .{original_text[0 .. original_text.len - 1]}) catch @panic("fixture journal"),
        21 => return allocator.dupe(u8, "{") catch @panic("fixture journal"),
        else => unreachable,
    }
    return std.json.stringifyAlloc(allocator, mutated, .{}) catch @panic("fixture journal");
}

test "remove prepared evidence rejects every tampered journal, staged manifest, and artifact before recovery" {
    var found = false;
    for (1..32) |step| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + step, .monotonic);
        const outcome = store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
        store.fail_after.store(0, .monotonic);
        if (outcome) |_| break else |err| try std.testing.expectEqual(error.InjectedFailure, err);
        const txid = optional_transaction_id(fixture.state) orelse continue;
        const original_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
        defer std.testing.allocator.free(original_text);
        var parsed = try parse_journal_v3(std.testing.allocator, original_text);
        defer parsed.deinit();
        if (!std.mem.eql(u8, parsed.value.phase, "prepared")) continue;
        found = true;

        const before_manifest = try fixture.state.readFileAlloc(std.testing.allocator, "manifest.json", manifest.MaxManifestBytes);
        defer std.testing.allocator.free(before_manifest);
        for (0..22) |case_index| {
            const tampered = tampered_journal(std.testing.allocator, parsed.value, case_index, original_text);
            defer std.testing.allocator.free(tampered);
            replace_transaction_file(fixture.state, txid, "journal.json", tampered, 0o600);
            try std.testing.expectError(error.RecoveryRequired, store.recover());
            {
                const on_disk = read_stored_manifest(fixture.state);
                defer std.testing.allocator.free(on_disk);
                try std.testing.expectEqualStrings(before_manifest, on_disk);
            }
            try std.testing.expect(target_exists(fixture.data));
        }
        replace_transaction_file(fixture.state, txid, "journal.json", original_text, 0o600);

        replace_transaction_file(fixture.state, txid, "new-manifest", "not the candidate", 0o600);
        try std.testing.expectError(error.RecoveryRequired, store.recover());
        replace_transaction_file(fixture.state, txid, "new-manifest", docs.candidate, 0o600);
        replace_transaction_file(fixture.state, txid, "old-manifest", "not the original", 0o600);
        try std.testing.expectError(error.RecoveryRequired, store.recover());
        replace_transaction_file(fixture.state, txid, "old-manifest", docs.original, 0o600);


        if (c.renameat(fixture.state.fd, "manifest.json", fixture.state.fd, "manifest.saved") != 0) @panic("fixture rename");
        write_status_file(fixture.state, "manifest.json", "foreign manifest", 0o600);
        try std.testing.expectError(error.ArtifactConflict, store.recover());
        if (c.unlinkat(fixture.state.fd, "manifest.json", 0) != 0) @panic("fixture unlink");
        if (c.renameat(fixture.state.fd, "manifest.saved", fixture.state.fd, "manifest.json") != 0) @panic("fixture rename");

        try store.apply_prepared_v3(parsed.value);
        try store.apply_prepared_v3(parsed.value);
        {
            const on_disk = read_stored_manifest(fixture.state);
            defer std.testing.allocator.free(on_disk);
            try std.testing.expectEqualStrings(docs.candidate, on_disk);
        }
        try store.recover();
        {
            const snapshot = take_remove_snapshot(&fixture);
            defer free_remove_snapshot(snapshot);
            try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
            if (snapshot.target) |target| {
                try std.testing.expectEqualStrings("target bytes", target);
                try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
            } else {
                try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
            }
        }
        break;
    }
    try std.testing.expect(found);
}

test "remove keeps text and numeric prior records intact in the candidate delta" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    install_remove_artifacts(&fixture);
    const binfmt_digest = hash_hex("binfmt bytes");
    const policy_digest = hash_hex("policy bytes");
    const binfmt_prior = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const binfmt = test_spec_t{ .id = "binfmt:kept", .kind = "binfmt", .path = "/etc/binfmt.d/waddle.conf", .sha256 = &binfmt_digest, .prior = "\"" ++ binfmt_prior ++ "\"" };
    const policy = test_spec_t{ .id = "policy:kept", .kind = "guest_policy", .path = "HKLM-Policy-Waddle", .sha256 = &policy_digest, .extra = "\"device\":\"default\",", .prior = "7" };
    const target_digest = hash_hex("target bytes");
    const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &target_digest };
    const original_specs = [_]test_spec_t{ target, binfmt, policy };
    const candidate_specs = [_]test_spec_t{ binfmt, policy };
    const original = test_document(scratch, 1, &original_specs);
    const candidate = test_document(scratch, 1, &candidate_specs);
    try write_test_file(fixture.state.fd, "manifest.json", original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    const receipt = try store.remove(.{ .record_id = "desktop:target", .candidate_manifest = candidate });
    try std.testing.expectEqual(remove_outcome_t.Removed, receipt.outcome);
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(candidate, on_disk);
    }
}

test "remove on a closed store returns Closed without reading the manifest" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    store.deinit();
    try std.testing.expectError(error.Closed, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
}

test "remove refuses an overlapping recorded manifest before classifying any artifact" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = remove_documents(scratch, fixture.data_path);
    install_remove_artifacts(&fixture);
    const shared_digest = hash_hex("target bytes");
    const shared_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const first = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = shared_path, .sha256 = &shared_digest };
    const second = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = shared_path, .sha256 = &shared_digest };
    const specs = [_]test_spec_t{ first, second };
    const overlapping = test_document(scratch, 1, &specs);
    try write_test_file(fixture.state.fd, "manifest.json", overlapping, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    try std.testing.expectError(error.ManifestMismatch, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(overlapping, on_disk);
    }
    const target = try fixture.data.readFileAlloc(std.testing.allocator, "applications/target.desktop", 4096);
    defer std.testing.allocator.free(target);
    try std.testing.expectEqualStrings("target bytes", target);
}

test "remove recovery retries after every injected I/O failure from prepared and committed phases" {
    defer {
        injected_io_at = 0;
        injected_io_count = 0;
    }
    for ([_]usize{ 6, 8, 9 }) |fail_step| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + fail_step, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
        store.fail_after.store(0, .monotonic);
        const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
        const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
        defer std.testing.allocator.free(journal_text);
        var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
        defer parsed.deinit();
        const committed = std.mem.eql(u8, parsed.value.phase, "committed");
        const on_disk_before = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk_before);
        if (fail_step == 6) try std.testing.expectEqualStrings(docs.original, on_disk_before);

        var observed_failures: usize = 0;
        for (1..256) |failure_io| {
            injected_io_at = failure_io;
            injected_io_count = 0;
            const outcome = store.recover();
            injected_io_at = 0;
            injected_io_count = 0;
            if (outcome) |_| break else |err| {
                observed_failures += 1;
                try std.testing.expect(err == error.IoError or err == error.AccessDenied or err == error.ArtifactConflict or err == error.RecoveryRequired);
            }
        }
        try store.recover();
        try std.testing.expect(observed_failures > 0);
        const snapshot = take_remove_snapshot(&fixture);
        defer free_remove_snapshot(snapshot);
        if (committed) {
            try std.testing.expect(snapshot.target == null);
            try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
        } else {
            try std.testing.expectEqualStrings("target bytes", snapshot.target orelse return error.TestExpectedEqual);
            try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
        }
        try std.testing.expectEqualStrings("keep bytes", snapshot.keep);
    }
}


test "committed remove evidence rejects tampered artifacts and staged manifests before any unlink" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 9, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    try std.testing.expectEqualStrings("committed", parsed.value.phase);
    const temp_name = manifest_temp_name(txid);
    const temp: [:0]const u8 = temp_name[0 .. std.mem.indexOfScalar(u8, &temp_name, 0) orelse unreachable :0];
    const saved_temp: [:0]const u8 = "displaced-saved.tmp";

    if (c.renameat(fixture.state.fd, temp.ptr, fixture.state.fd, saved_temp.ptr) != 0) @panic("fixture rename");
    write_status_file(fixture.state, temp, "foreign displaced manifest", 0o600);
    try std.testing.expectError(error.ArtifactConflict, store.recover());
    if (c.unlinkat(fixture.state.fd, temp.ptr, 0) != 0) @panic("fixture unlink");
    if (c.renameat(fixture.state.fd, saved_temp.ptr, fixture.state.fd, temp.ptr) != 0) @panic("fixture rename");

    if (c.renameat(fixture.state.fd, "manifest.json", fixture.state.fd, "manifest.saved") != 0) @panic("fixture rename");
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    write_status_file(fixture.state, "manifest.json", "foreign manifest", 0o600);
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    if (c.unlinkat(fixture.state.fd, "manifest.json", 0) != 0) @panic("fixture unlink");
    if (c.renameat(fixture.state.fd, "manifest.saved", fixture.state.fd, "manifest.json") != 0) @panic("fixture rename");

    try store.recover();
    const snapshot = take_remove_snapshot(&fixture);
    defer free_remove_snapshot(snapshot);
    try std.testing.expect(snapshot.target == null);
    try std.testing.expectEqualStrings(docs.candidate, snapshot.manifest);
}

test "prepared remove evidence rejects absent, relocated, or foreign live state before rollback" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    try std.testing.expectEqualStrings("prepared", parsed.value.phase);
    const temp_name = manifest_temp_name(txid);
    const temp: [:0]const u8 = temp_name[0 .. std.mem.indexOfScalar(u8, &temp_name, 0) orelse unreachable :0];

    if (c.renameat(fixture.state.fd, "manifest.json", fixture.state.fd, "manifest.saved") != 0) @panic("fixture rename");
    try std.testing.expectError(error.ArtifactConflict, store.recover());
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared_v3(parsed.value));
    write_status_file(fixture.state, "manifest.json", "foreign manifest", 0o600);
    try std.testing.expectError(error.ArtifactConflict, store.recover());
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared_v3(parsed.value));
    if (c.unlinkat(fixture.state.fd, "manifest.json", 0) != 0) @panic("fixture unlink");
    if (c.renameat(fixture.state.fd, "manifest.saved", fixture.state.fd, "manifest.json") != 0) @panic("fixture rename");

    try store.apply_prepared_v3(parsed.value);
    try store.apply_prepared_v3(parsed.value);
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(docs.candidate, on_disk);
    }
    if (c.unlinkat(fixture.state.fd, temp.ptr, 0) != 0) @panic("fixture unlink");
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    try std.testing.expect(target_exists(fixture.data));
}

/// Replace the staged manifests of a version-3 journal and update its digests so
/// that every hash check passes and only the semantic validation can reject them.
fn rewrite_manifest_evidence(state: std.fs.Dir, txid: [32]u8, journal: journal_v3_t, old_bytes: []const u8, new_bytes: []const u8) void {
    const old_hash = hash_hex(old_bytes);
    const new_hash = hash_hex(new_bytes);
    var updated = journal;
    updated.old_manifest_hash = &old_hash;
    updated.new_manifest_hash = &new_hash;
    const text = std.json.stringifyAlloc(std.testing.allocator, updated, .{}) catch @panic("fixture journal");
    defer std.testing.allocator.free(text);
    replace_transaction_file(state, txid, "old-manifest", old_bytes, 0o600);
    replace_transaction_file(state, txid, "new-manifest", new_bytes, 0o600);
    replace_transaction_file(state, txid, "journal.json", text, 0o600);
}

test "recovery rejects rewritten staged manifests that are digest-consistent but semantically invalid" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = remove_documents(scratch, fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();

    const target_digest = hash_hex("target bytes");
    const keep_digest = hash_hex("keep bytes");
    const other_digest = hash_hex("other bytes");
    const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const keep_path = std.fmt.allocPrint(scratch, "{s}/applications/keep.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &target_digest };
    const keep = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
    const outside = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = "/unused-root/applications/target.desktop", .sha256 = &target_digest };
    const wrong_digest = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &other_digest };
    const duplicate = test_spec_t{ .id = "desktop:duplicate", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
    const same_target_path = test_spec_t{ .id = "desktop:duplicate", .kind = "desktop_entry", .path = target_path, .sha256 = &keep_digest };
    const keep_list = [_]test_spec_t{keep};
    const target_list = [_]test_spec_t{target};
    const duplicate_list = [_]test_spec_t{ keep, duplicate };
    const outside_original = [_]test_spec_t{ outside, keep };
    const wrong_original = [_]test_spec_t{ wrong_digest, keep };
    const overlapping_original = [_]test_spec_t{ target, same_target_path };
    const sets = [_]struct { old: []const u8, new: []const u8 }{
        .{ .old = docs.original, .new = "{" },
        .{ .old = "{", .new = docs.candidate },
        .{ .old = docs.original, .new = test_document(scratch, 1, &duplicate_list) },
        .{ .old = test_document(scratch, 1, &overlapping_original), .new = docs.candidate },
        .{ .old = docs.original, .new = test_document(scratch, 1, &target_list) },
        .{ .old = test_document(scratch, 1, &outside_original), .new = test_document(scratch, 1, &keep_list) },
        .{ .old = test_document(scratch, 1, &wrong_original), .new = test_document(scratch, 1, &keep_list) },
    };
    for (sets) |set| {
        rewrite_manifest_evidence(fixture.state, txid, parsed.value, set.old, set.new);
        try std.testing.expectError(error.RecoveryRequired, store.recover());
        replace_transaction_file(fixture.state, txid, "old-manifest", docs.original, 0o600);
        replace_transaction_file(fixture.state, txid, "new-manifest", docs.candidate, 0o600);
        replace_transaction_file(fixture.state, txid, "journal.json", journal_text, 0o600);
    }
    try store.recover();
    try std.testing.expect(target_exists(fixture.data));
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(docs.original, on_disk);
    }
}


test "prepared published remove rejects a foreign or absent displaced manifest before restoring" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(docs.candidate, on_disk);
    }
    const temp_name = manifest_temp_name(txid);
    const temp: [:0]const u8 = temp_name[0 .. std.mem.indexOfScalar(u8, &temp_name, 0) orelse unreachable :0];
    const saved_temp: [:0]const u8 = "displaced-saved.tmp";

    if (c.renameat(fixture.state.fd, temp.ptr, fixture.state.fd, saved_temp.ptr) != 0) @panic("fixture rename");
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    write_status_file(fixture.state, temp, "foreign displaced manifest", 0o600);
    try std.testing.expectError(error.ArtifactConflict, store.recover());
    if (c.unlinkat(fixture.state.fd, temp.ptr, 0) != 0) @panic("fixture unlink");
    if (c.renameat(fixture.state.fd, saved_temp.ptr, fixture.state.fd, temp.ptr) != 0) @panic("fixture rename");

    try store.recover();
    const snapshot = take_remove_snapshot(&fixture);
    defer free_remove_snapshot(snapshot);
    try std.testing.expectEqualStrings("target bytes", snapshot.target orelse return error.TestExpectedEqual);
    try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
}

test "remove recovery allocation failures leave a retryable state without leaked allocations" {
    var observed_failures: usize = 0;
    for (0..256) |failure_offset| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        var store = try store_t.init(failing.allocator(), fixture.state.fd, &roots);
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
        store.fail_after.store(0, .monotonic);
        failing.fail_index = failing.alloc_index + failure_offset;
        const outcome = store.recover();
        if (!failing.has_induced_failure) {
            try outcome;
            store.deinit();
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed_failures += 1;
        if (outcome) |_| {
            return error.ExpectedOutOfMemory;
        } else |err| if (err != error.OutOfMemory) return err;
        failing.fail_index = std.math.maxInt(usize);
        try store.recover();
        const snapshot = take_remove_snapshot(&fixture);
        defer free_remove_snapshot(snapshot);
        try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
        store.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    try std.testing.expect(observed_failures > 0);
}

test "recovery rejects a journal root whose configured kind does not own the recorded artifact" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    fixture.temporary.dir.makeDir("config") catch @panic("fixture config");
    var config = fixture.temporary.dir.openDir("config", .{}) catch @panic("fixture config open");
    defer config.close();
    const config_path = config.realpathAlloc(std.testing.allocator, ".") catch @panic("fixture config path");
    defer std.testing.allocator.free(config_path);
    const roots = [_]root_t{
        .{ .kind = .Data, .canonical_path = fixture.data_path, .dir_fd = fixture.data.fd },
        .{ .kind = .Config, .canonical_path = config_path, .dir_fd = config.fd },
    };
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    var mismatched = parsed.value;
    mismatched.root = "config";
    const text = try std.json.stringifyAlloc(std.testing.allocator, mismatched, .{});
    defer std.testing.allocator.free(text);
    replace_transaction_file(fixture.state, txid, "journal.json", text, 0o600);
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    replace_transaction_file(fixture.state, txid, "journal.json", journal_text, 0o600);
    try store.recover();
    const snapshot = take_remove_snapshot(&fixture);
    defer free_remove_snapshot(snapshot);
    try std.testing.expectEqualStrings(docs.original, snapshot.manifest);
}

/// Candidate with a different root `waddle_version` and an unchanged record set.
fn migrated_document(arena: std.mem.Allocator, original: []const u8) []u8 {
    return std.mem.replaceOwned(u8, arena, original, "\"waddle_version\":\"0.2.0\"", "\"waddle_version\":\"0.3.0\"") catch @panic("fixture migrated");
}

test "migrate rewrites only root metadata and leaves the artifact and every record identical" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    const migrated = migrated_document(arena.allocator(), docs.original);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const before = try fixture.data.statFile("applications/target.desktop");
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();

    const receipt = try store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = migrated });
    try std.testing.expect(receipt.changed);
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(migrated, on_disk);
    }
    const after = try fixture.data.statFile("applications/target.desktop");
    try std.testing.expectEqual(before.inode, after.inode);
    const target = try fixture.data.readFileAlloc(std.testing.allocator, "applications/target.desktop", 4096);
    defer std.testing.allocator.free(target);
    try std.testing.expectEqualStrings("target bytes", target);

    const repeat = try store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = migrated });
    try std.testing.expect(!repeat.changed);
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(migrated, on_disk);
    }
}

test "migrate refuses changed, missing, extra, foreign-schema, and malformed requests without mutation" {
    for (0..11) |case_index| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const scratch = arena.allocator();
        const docs = remove_documents(scratch, fixture.data_path);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();

        const changed_digest = hash_hex("changed target bytes");
        const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const keep_path = std.fmt.allocPrint(scratch, "{s}/applications/keep.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const keep_digest = hash_hex("keep bytes");
        const extra_digest = hash_hex("extra bytes");
        const changed_target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &changed_digest };
        const keep = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
        const extra = test_spec_t{ .id = "desktop:extra", .kind = "desktop_entry", .path = "/unused-root/extra.desktop", .sha256 = &extra_digest };
        const changed_list = [_]test_spec_t{ changed_target, keep };
        const extra_list = [_]test_spec_t{ changed_target, keep, extra };
        const other_path = std.fmt.allocPrint(scratch, "{s}/applications/other.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const other = test_spec_t{ .id = "desktop:other", .kind = "desktop_entry", .path = other_path, .sha256 = &extra_digest };
        const original_target_digest = hash_hex("target bytes");
        const original_target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &original_target_digest };
        const swapped_list = [_]test_spec_t{ original_target, other };
        const duplicate_list = [_]test_spec_t{ keep, test_spec_t{ .id = "desktop:duplicate", .kind = "desktop_entry", .path = keep_path, .sha256 = &extra_digest } };
        const candidate: []const u8 = switch (case_index) {
            0 => test_document(scratch, 1, &changed_list),
            1 => docs.candidate,
            2 => test_document(scratch, 1, &extra_list),
            3 => test_document(scratch, 2, &extra_list),
            4 => "{",
            5 => "",
            8 => test_document(scratch, 1, &swapped_list),
            9 => test_document(scratch, 1, &duplicate_list),
            10 => blk: {
                const oversized = scratch.alloc(u8, manifest.MaxManifestBytes + 1) catch @panic("fixture size");
                @memset(oversized, ' ');
                break :blk oversized;
            },
            else => docs.original,
        };
        const record_id: []const u8 = switch (case_index) {
            6 => "desktop:missing",
            7 => "",
            else => "desktop:target",
        };
        const expected: anyerror = switch (case_index) {
            0, 1, 2, 8, 9 => error.ManifestMismatch,
            3 => error.UnsupportedSchema,
            4 => error.InvalidManifest,
            5, 7, 10 => error.InvalidArgument,
            6 => error.FileNotFound,
            else => unreachable,
        };
        try std.testing.expectError(expected, store.migrate_record(.{ .record_id = record_id, .candidate_manifest = candidate }));
        {
            const on_disk = read_stored_manifest(fixture.state);
            defer std.testing.allocator.free(on_disk);
            try std.testing.expectEqualStrings(docs.original, on_disk);
        }
        const target = try fixture.data.readFileAlloc(std.testing.allocator, "applications/target.desktop", 4096);
        defer std.testing.allocator.free(target);
        try std.testing.expectEqualStrings("target bytes", target);
    }
}

test "migrate rejects an overlapping recorded manifest and a closed store before any mutation" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = remove_documents(scratch, fixture.data_path);
    install_remove_artifacts(&fixture);
    const shared_digest = hash_hex("target bytes");
    const shared_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const first = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = shared_path, .sha256 = &shared_digest };
    const second = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = shared_path, .sha256 = &shared_digest };
    const overlapping = test_document(scratch, 1, &[_]test_spec_t{ first, second });
    try write_test_file(fixture.state.fd, "manifest.json", overlapping, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    try std.testing.expectError(error.ManifestMismatch, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = overlapping }));
    store.deinit();
    try std.testing.expectError(error.Closed, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = docs.original }));
    {
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(overlapping, on_disk);
    }
}

test "migrate without a manifest reports FileNotFound and creates nothing" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    try std.testing.expectError(error.FileNotFound, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = docs.original }));
    try std.testing.expectError(error.FileNotFound, fixture.state.access("manifest.json", .{}));
}

test "migrate recovers after every injected durable checkpoint and a second recovery is a no-op" {
    var completed = false;
    var observed_failures: usize = 0;
    for (1..48) |step| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        const migrated = migrated_document(arena.allocator(), docs.original);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + step, .monotonic);
        const outcome = store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = migrated });
        store.fail_after.store(0, .monotonic);
        if (outcome) |receipt| {
            try std.testing.expect(receipt.changed);
            completed = true;
        } else |err| {
            observed_failures += 1;
            try std.testing.expectEqual(error.InjectedFailure, err);
            try store.recover();
        }
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expect(std.mem.eql(u8, on_disk, docs.original) or std.mem.eql(u8, on_disk, migrated));
        if (completed) try std.testing.expectEqualStrings(migrated, on_disk);
        try store.recover();
        const again = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(again);
        try std.testing.expectEqualStrings(on_disk, again);
        const target = try fixture.data.readFileAlloc(std.testing.allocator, "applications/target.desktop", 4096);
        defer std.testing.allocator.free(target);
        try std.testing.expectEqualStrings("target bytes", target);
        if (completed) break;
    }
    try std.testing.expect(completed);
    try std.testing.expect(observed_failures > 0);
}

test "migrate retries after every selected injected I/O failure and allocation failure without leaks" {
    defer {
        injected_io_at = 0;
        injected_io_count = 0;
    }
    var observed_failures: usize = 0;
    for (1..240) |failure_io| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        const migrated = migrated_document(arena.allocator(), docs.original);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        injected_io_at = failure_io;
        injected_io_count = 0;
        const outcome = store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = migrated });
        injected_io_at = 0;
        injected_io_count = 0;
        if (outcome) |_| {
            const on_disk = read_stored_manifest(fixture.state);
            defer std.testing.allocator.free(on_disk);
            try std.testing.expectEqualStrings(migrated, on_disk);
            break;
        } else |err| {
            observed_failures += 1;
            try std.testing.expect(err != error.InvalidArgument and err != error.OutOfMemory and err != error.InjectedFailure);
        }
        try store.recover();
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expect(std.mem.eql(u8, on_disk, docs.original) or std.mem.eql(u8, on_disk, migrated));
        try store.recover();
    }
    try std.testing.expect(observed_failures > 0);
}

test "migrate allocation failures leave a recoverable state and no leaked allocations" {
    var observed_failures: usize = 0;
    for (0..512) |failure_offset| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = remove_documents(arena.allocator(), fixture.data_path);
        const migrated = migrated_document(arena.allocator(), docs.original);
        install_remove_artifacts(&fixture);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        var store = try store_t.init(failing.allocator(), fixture.state.fd, &roots);
        failing.fail_index = failing.alloc_index + failure_offset;
        const outcome = store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = migrated });
        if (!failing.has_induced_failure) {
            _ = try outcome;
            store.deinit();
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed_failures += 1;
        if (outcome) |_| {
            return error.ExpectedOutOfMemory;
        } else |err| if (err != error.OutOfMemory) return err;
        failing.fail_index = std.math.maxInt(usize);
        try store.recover();
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expect(std.mem.eql(u8, on_disk, docs.original) or std.mem.eql(u8, on_disk, migrated));
        store.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    try std.testing.expect(observed_failures > 0);
}

test "migrate journal rejects removal fields and rewritten metadata before recovery" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = remove_documents(scratch, fixture.data_path);
    const migrated = migrated_document(scratch, docs.original);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = migrated }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    try std.testing.expectEqualStrings("prepared", parsed.value.phase);
    try std.testing.expectEqualStrings("migrate", parsed.value.operation);

    var with_root = parsed.value;
    with_root.root = "data";
    var with_artifact = parsed.value;
    with_artifact.old_artifact_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    with_artifact.old_artifact_mode = 0o644;
    with_artifact.old_artifact_device = 1;
    with_artifact.old_artifact_inode = 1;
    var as_removal = parsed.value;
    as_removal.operation = "remove";
    const variants = [_]journal_v3_t{ with_root, with_artifact, as_removal };
    for (variants) |variant| {
        const text = try std.json.stringifyAlloc(std.testing.allocator, variant, .{});
        defer std.testing.allocator.free(text);
        replace_transaction_file(fixture.state, txid, "journal.json", text, 0o600);
        try std.testing.expectError(error.RecoveryRequired, store.recover());
    }
    replace_transaction_file(fixture.state, txid, "journal.json", journal_text, 0o600);

    const changed_digest = hash_hex("changed target bytes");
    const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const keep_path = std.fmt.allocPrint(scratch, "{s}/applications/keep.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const keep_digest = hash_hex("keep bytes");
    const changed = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &changed_digest };
    const keep = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
    const changed_list = [_]test_spec_t{ changed, keep };
    const semantic_change = test_document(scratch, 1, &changed_list);
    rewrite_manifest_evidence(fixture.state, txid, parsed.value, docs.original, semantic_change);
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    replace_transaction_file(fixture.state, txid, "old-manifest", docs.original, 0o600);
    replace_transaction_file(fixture.state, txid, "new-manifest", migrated, 0o600);
    replace_transaction_file(fixture.state, txid, "journal.json", journal_text, 0o600);

    try store.recover();
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(docs.original, on_disk);
    try std.testing.expect(target_exists(fixture.data));
}

/// Recreate fixture: one handler record whose artifact is absent, a candidate that
/// changes only the root `updated` value, and the artifact bytes it describes.
const recreate_docs_t = struct { original: []u8, candidate: []u8, bytes: []const u8 };

fn recreate_documents(arena: std.mem.Allocator, data_path: []const u8) recreate_docs_t {
    const bytes = "[Desktop Entry]\nType=Application\n";
    const digest = hash_hex(bytes);
    const path = std.fmt.allocPrint(arena, "{s}/applications/handler.desktop", .{data_path}) catch @panic("fixture path");
    const spec = test_spec_t{ .id = "handler:waddle-open", .kind = "handler_entry", .path = path, .sha256 = &digest };
    const specs = [_]test_spec_t{spec};
    const original = test_document(arena, 1, &specs);
    const candidate = std.mem.replaceOwned(u8, arena, original, "2026-10-25T00:00:00Z", "2026-10-26T00:00:00Z") catch @panic("fixture candidate");
    return .{ .original = original, .candidate = candidate, .bytes = bytes };
}

/// Assert that the artifact and manifest are in one of the two legal recreate states:
/// absent with the original manifest, or present with the candidate manifest.
fn expect_recreate_converged(fixture: *remove_fixture_t, docs: recreate_docs_t) !void {
    const present = blk: {
        fixture.data.access("applications/handler.desktop", .{}) catch break :blk false;
        break :blk true;
    };
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    if (present) {
        try std.testing.expectEqualStrings(docs.candidate, on_disk);
        const bytes = try fixture.data.readFileAlloc(std.testing.allocator, "applications/handler.desktop", 4096);
        defer std.testing.allocator.free(bytes);
        try std.testing.expectEqualStrings(docs.bytes, bytes);
    } else {
        try std.testing.expectEqualStrings(docs.original, on_disk);
    }
}

test "recreate publishes an absent artifact, keeps the record identity, and then refuses a live target" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = recreate_documents(arena.allocator(), fixture.data_path);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();

    const receipt = try store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes });
    try std.testing.expectEqual(@as(u32, 0o644), receipt.target.mode);
    try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
    var listed = try store.status(std.testing.allocator);
    defer listed.deinit();
    try std.testing.expectEqual(record_state_t.Ok, listed.items[0].state);

    try std.testing.expectError(error.InvalidArgument, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    const again = std.mem.replaceOwned(u8, arena.allocator(), docs.candidate, "2026-10-26T00:00:00Z", "2026-10-27T00:00:00Z") catch @panic("fixture candidate");
    try std.testing.expectError(error.ArtifactConflict, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = again, .artifact_bytes = docs.bytes }));
    try std.testing.expectError(error.InvalidArgument, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = "" }));
}

test "recreate rejects every invalid candidate and absent-state violation before mutation" {
    for (0..8) |case_index| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const scratch = arena.allocator();
        const docs = recreate_documents(scratch, fixture.data_path);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        const other_path = std.fmt.allocPrint(scratch, "{s}/applications/other.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const other_digest = hash_hex("other");
        const moved = test_spec_t{ .id = "handler:waddle-open", .kind = "handler_entry", .path = other_path, .sha256 = &other_digest };
        const moved_list = [_]test_spec_t{moved};
        const extra_list = [_]test_spec_t{ moved, .{ .id = "desktop:extra", .kind = "desktop_entry", .path = other_path, .sha256 = &other_digest } };
        const bad_digest = hash_hex("different bytes");
        const bad_path = std.fmt.allocPrint(scratch, "{s}/applications/handler.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const digest_changed = test_spec_t{ .id = "handler:waddle-open", .kind = "handler_entry", .path = bad_path, .sha256 = &bad_digest };
        const changed_list = [_]test_spec_t{digest_changed};
        const candidate: []const u8 = switch (case_index) {
            0 => test_document(scratch, 1, &moved_list),
            1 => test_document(scratch, 1, &extra_list),
            2 => docs.original,
            3 => test_document(scratch, 1, &changed_list),
            4 => "{",
            else => docs.candidate,
        };
        const record_id: []const u8 = if (case_index == 5) "handler:missing" else if (case_index == 6) "" else "handler:waddle-open";
        const expected: anyerror = switch (case_index) {
            0, 1, 3 => error.ManifestMismatch,
            2 => error.InvalidArgument,
            4 => error.InvalidManifest,
            5 => error.FileNotFound,
            6 => error.InvalidArgument,
            else => error.InvalidArgument,
        };
        const artifact: []const u8 = if (case_index == 7) "" else docs.bytes;
        try std.testing.expectError(expected, store.recreate(.{ .record_id = record_id, .candidate_manifest = candidate, .artifact_bytes = artifact }));
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expectEqualStrings(docs.original, on_disk);
        try std.testing.expectError(error.FileNotFound, fixture.data.access("applications/handler.desktop", .{}));
    }
}

test "recreate recovers after every injected durable checkpoint and a second recovery is a no-op" {
    var completed = false;
    var observed_failures: usize = 0;
    for (1..48) |step| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = recreate_documents(arena.allocator(), fixture.data_path);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + step, .monotonic);
        const outcome = store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes });
        store.fail_after.store(0, .monotonic);
        if (outcome) |_| {
            completed = true;
        } else |err| {
            observed_failures += 1;
            try std.testing.expectEqual(error.InjectedFailure, err);
            try store.recover();
        }
        try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
        try store.recover();
        try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
        if (completed) break;
    }
    try std.testing.expect(completed);
    try std.testing.expect(observed_failures > 0);
}

test "recreate retries after every selected injected I/O failure and allocation failure without leaks" {
    defer {
        injected_io_at = 0;
        injected_io_count = 0;
    }
    var observed_failures: usize = 0;
    for (1..200) |failure_io| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = recreate_documents(arena.allocator(), fixture.data_path);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        injected_io_at = failure_io;
        injected_io_count = 0;
        const outcome = store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes });
        injected_io_at = 0;
        injected_io_count = 0;
        if (outcome) |_| {
            try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
            break;
        } else |err| {
            observed_failures += 1;
            try std.testing.expect(err != error.InvalidArgument and err != error.OutOfMemory and err != error.InjectedFailure);
        }
        try store.recover();
        try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
    }
    try std.testing.expect(observed_failures > 0);
    var failure_offset: usize = 0;
    while (failure_offset < 512) : (failure_offset += 1) {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const docs = recreate_documents(arena.allocator(), fixture.data_path);
        try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
        const roots = fixture.roots();
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        var store = try store_t.init(failing.allocator(), fixture.state.fd, &roots);
        failing.fail_index = failing.alloc_index + failure_offset;
        const outcome = store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes });
        if (!failing.has_induced_failure) {
            _ = try outcome;
            store.deinit();
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        if (outcome) |_| return error.ExpectedOutOfMemory else |err| if (err != error.OutOfMemory) return err;
        failing.fail_index = std.math.maxInt(usize);
        try store.recover();
        try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
        store.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
}

test "recreate refuses a foreign file that appears during a prepared publish and rolls back cleanly" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = recreate_documents(arena.allocator(), fixture.data_path);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    try std.testing.expectEqualStrings("recreate", parsed.value.operation);
    try std.testing.expectEqualStrings("prepared", parsed.value.phase);

    write_status_file(fixture.data, "applications/handler.desktop", "foreign bytes", 0o644);
    try store.recover();
    const kept = try fixture.data.readFileAlloc(std.testing.allocator, "applications/handler.desktop", 4096);
    defer std.testing.allocator.free(kept);
    try std.testing.expectEqualStrings("foreign bytes", kept);
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(docs.original, on_disk);
}

test "ensure_directory creates missing components and refuses unsafe existing components" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    try store.ensure_directory(.Data, "nested/one/two");
    try fixture.data.access("nested/one/two", .{});
    try store.ensure_directory(.Data, "nested/one");
    try std.testing.expectError(error.InvalidScope, store.ensure_directory(.Config, "x"));
    try std.testing.expectError(error.InvalidPath, store.ensure_directory(.Data, "../escape"));
    if (c.symlinkat("nested", fixture.data.fd, "link") != 0) @panic("fixture symlink");
    try std.testing.expectError(error.AccessDenied, store.ensure_directory(.Data, "link/child"));
    if (c.mkdirat(fixture.data.fd, "loose", 0o775) != 0) @panic("fixture mkdir");
    if (c.fchmodat(fixture.data.fd, "loose", 0o775, 0) != 0) @panic("fixture chmod");
    try std.testing.expectError(error.AccessDenied, store.ensure_directory(.Data, "loose/child"));
    try std.testing.expectError(error.AccessDenied, store.ensure_directory(.Data, "loose"));
}

test "manifest_snapshot returns caller-owned bytes or null without writing" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    try std.testing.expect((try store.manifest_snapshot(std.testing.allocator)) == null);
    try write_test_file(fixture.state.fd, "manifest.json", "{\"schema\":1}", 0o600);
    const bytes = (try store.manifest_snapshot(std.testing.allocator)) orelse return error.TestExpectedEqual;
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqualStrings("{\"schema\":1}", bytes);
}

test "remove journals survive user edits of the artifact and init recovers without touching user bytes" {
    for (0..3) |edit| {
        var completed = false;
        for (1..24) |step| {
            var fixture = remove_fixture_t.create(std.testing.allocator);
            defer fixture.destroy(std.testing.allocator);
            var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
            defer arena.deinit();
            const docs = remove_documents(arena.allocator(), fixture.data_path);
            install_remove_artifacts(&fixture);
            try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
            const roots = fixture.roots();
            {
                var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
                store.fail_after.store(store.checkpoint_count.load(.monotonic) + step, .monotonic);
                const outcome = store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate });
                store.fail_after.store(0, .monotonic);
                store.deinit();
                if (outcome) |_| {
                    completed = true;
                } else |err| try std.testing.expectEqual(error.InjectedFailure, err);
            }
            // The user edits, deletes, or replaces the artifact between the interrupted
            // transaction and the next startup.
            switch (edit) {
                0 => write_status_file(fixture.data, "applications/target.desktop", "user edited bytes", 0o644),
                1 => _ = c.unlinkat(fixture.data.fd, "applications/target.desktop", 0),
                else => {
                    _ = c.unlinkat(fixture.data.fd, "applications/target.desktop", 0);
                    write_status_file(fixture.data, "applications/target.desktop", "user replacement", 0o644);
                },
            }
            var reopened = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
            defer reopened.deinit();
            const snapshot = take_remove_snapshot(&fixture);
            defer free_remove_snapshot(snapshot);
            switch (edit) {
                0 => try std.testing.expectEqualStrings("user edited bytes", snapshot.target orelse return error.TestExpectedEqual),
                1 => try std.testing.expect(snapshot.target == null),
                else => try std.testing.expectEqualStrings("user replacement", snapshot.target orelse return error.TestExpectedEqual),
            }
            try std.testing.expect(std.mem.eql(u8, snapshot.manifest, docs.original) or std.mem.eql(u8, snapshot.manifest, docs.candidate));
            if (completed) break;
        }
    }
}

test "remove deletes non-file and artifact-missing records through a journaled record-only transaction" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    install_remove_artifacts(&fixture);
    _ = fixture.data.deleteFile("applications/target.desktop") catch {};
    const binfmt_digest = hash_hex("binfmt bytes");
    const binfmt_prior = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const binfmt = test_spec_t{ .id = "binfmt:system", .kind = "binfmt", .path = "/etc/binfmt.d/waddle.conf", .sha256 = &binfmt_digest, .prior = "\"" ++ binfmt_prior ++ "\"" };
    const target_digest = hash_hex("target bytes");
    const keep_digest = hash_hex("keep bytes");
    const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const keep_path = std.fmt.allocPrint(scratch, "{s}/applications/keep.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &target_digest };
    const keep = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
    const all = [_]test_spec_t{ binfmt, target, keep };
    const without_binfmt = [_]test_spec_t{ target, keep };
    const only_keep = [_]test_spec_t{keep};
    const original = test_document(scratch, 1, &all);
    try write_test_file(fixture.state.fd, "manifest.json", original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();

    const first = try store.remove(.{ .record_id = "binfmt:system", .candidate_manifest = test_document(scratch, 1, &without_binfmt) });
    try std.testing.expectEqual(remove_outcome_t.RecordRemovedNonFile, first.outcome);
    try std.testing.expect(first.record_found);
    try std.testing.expectEqualStrings(binfmt_prior, first.prior.Text.bytes[0..first.prior.Text.len]);
    const second = try store.remove(.{ .record_id = "desktop:target", .candidate_manifest = test_document(scratch, 1, &only_keep) });
    try std.testing.expectEqual(remove_outcome_t.RecordRemovedArtifactMissing, second.outcome);
    const third = try store.remove(.{ .record_id = "desktop:target", .candidate_manifest = test_document(scratch, 1, &only_keep) });
    try std.testing.expectEqual(remove_outcome_t.AlreadyMissing, third.outcome);
    try std.testing.expect(!third.record_found);
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(test_document(scratch, 1, &only_keep), on_disk);
    const kept = try fixture.data.readFileAlloc(std.testing.allocator, "applications/keep.desktop", 4096);
    defer std.testing.allocator.free(kept);
    try std.testing.expectEqualStrings("keep bytes", kept);
}

test "forget recovers at every injected checkpoint and a second recovery is a no-op" {
    var completed = false;
    for (1..40) |step| {
        var fixture = remove_fixture_t.create(std.testing.allocator);
        defer fixture.destroy(std.testing.allocator);
        var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
        defer arena.deinit();
        const scratch = arena.allocator();
        const digest = hash_hex("binfmt bytes");
        const binfmt = test_spec_t{ .id = "binfmt:system", .kind = "binfmt", .path = "/etc/binfmt.d/waddle.conf", .sha256 = &digest };
        const target_digest = hash_hex("target bytes");
        const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
        const target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &target_digest };
        const original = test_document(scratch, 1, &[_]test_spec_t{ binfmt, target });
        const candidate = test_document(scratch, 1, &[_]test_spec_t{target});
        try write_test_file(fixture.state.fd, "manifest.json", original, 0o600);
        const roots = fixture.roots();
        var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
        defer store.deinit();
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + step, .monotonic);
        const outcome = store.remove(.{ .record_id = "binfmt:system", .candidate_manifest = candidate });
        store.fail_after.store(0, .monotonic);
        if (outcome) |receipt| {
            try std.testing.expectEqual(remove_outcome_t.RecordRemovedNonFile, receipt.outcome);
            completed = true;
        } else |err| {
            try std.testing.expectEqual(error.InjectedFailure, err);
            try store.recover();
        }
        const on_disk = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(on_disk);
        try std.testing.expect(std.mem.eql(u8, on_disk, original) or std.mem.eql(u8, on_disk, candidate));
        if (completed) try std.testing.expectEqualStrings(candidate, on_disk);
        try store.recover();
        const again = read_stored_manifest(fixture.state);
        defer std.testing.allocator.free(again);
        try std.testing.expectEqualStrings(on_disk, again);
        if (completed) break;
    }
    try std.testing.expect(completed);
}

test "artifact unlink moves the name aside, verifies the moved inode, and returns a changed file to its name" {
    defer {
        injected_renameat2_at = 0;
        injected_renameat2_count = 0;
    }
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    const txid: [32]u8 = "0123456789abcdef0123456789abcdef".*;
    const staging = "applications/.waddle-0123456789abcdef0123456789abcdef.tmp";

    install_remove_artifacts(&fixture);
    const original = (try store.inspect(fixture.data.fd, "applications/target.desktop")) orelse return error.TestExpectedEqual;
    write_status_file(fixture.data, "applications/target.desktop", "user edit", 0o644);
    const kept = try store.unlink_owned_artifact(fixture.data.fd, "applications/target.desktop", original, txid);
    try std.testing.expect(std.meta.activeTag(kept) == .KeptUnchanged);
    const still = try fixture.data.readFileAlloc(std.testing.allocator, "applications/target.desktop", 4096);
    defer std.testing.allocator.free(still);
    try std.testing.expectEqualStrings("user edit", still);
    try std.testing.expectError(error.FileNotFound, fixture.data.access(staging, .{}));

    injected_renameat2_count = 0;
    injected_renameat2_at = 2;
    const stranded = try store.unlink_owned_artifact(fixture.data.fd, "applications/target.desktop", original, txid);
    injected_renameat2_at = 0;
    injected_renameat2_count = 0;
    try std.testing.expect(std.meta.activeTag(stranded) == .KeptAtTempName);
    const moved = try fixture.data.readFileAlloc(std.testing.allocator, staging, 4096);
    defer std.testing.allocator.free(moved);
    try std.testing.expectEqualStrings("user edit", moved);
    try std.testing.expectError(error.FileNotFound, fixture.data.access("applications/target.desktop", .{}));
    if (c.unlinkat(fixture.data.fd, staging, 0) != 0) @panic("fixture unlink");

    write_status_file(fixture.data, "applications/target.desktop", "target bytes", 0o644);
    const current = (try store.inspect(fixture.data.fd, "applications/target.desktop")) orelse return error.TestExpectedEqual;
    const removed = try store.unlink_owned_artifact(fixture.data.fd, "applications/target.desktop", current, txid);
    try std.testing.expect(std.meta.activeTag(removed) == .Unlinked);
    try std.testing.expectError(error.FileNotFound, fixture.data.access("applications/target.desktop", .{}));
    try std.testing.expectError(error.FileNotFound, fixture.data.access(staging, .{}));
    const again = try store.unlink_owned_artifact(fixture.data.fd, "applications/target.desktop", current, txid);
    try std.testing.expect(std.meta.activeTag(again) == .AlreadyAbsent);
}

test "a socket at a recorded path is foreign, not an I/O failure, for status and remove" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const digest = hash_hex("socket bytes");
    const path = std.fmt.allocPrint(scratch, "{s}/applications/socket.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const spec = test_spec_t{ .id = "desktop:socket", .kind = "desktop_entry", .path = path, .sha256 = &digest };
    const specs = [_]test_spec_t{spec};
    const original = test_document(scratch, 1, &specs);
    const candidate = test_document(scratch, 1, &[_]test_spec_t{});
    try write_test_file(fixture.state.fd, "manifest.json", original, 0o600);
    const sock = std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0) catch return error.SkipZigTest;
    defer std.posix.close(sock);
    var addr = try std.net.Address.initUnix(path);
    std.posix.bind(sock, &addr.any, addr.getOsSockLen()) catch return error.SkipZigTest;
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    var listed = try store.status(std.testing.allocator);
    defer listed.deinit();
    try std.testing.expectEqual(record_state_t.Foreign, listed.items[0].state);
    const receipt = try store.remove(.{ .record_id = "desktop:socket", .candidate_manifest = candidate });
    try std.testing.expectEqual(remove_outcome_t.KeptForeign, receipt.outcome);
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(original, on_disk);
}

test "migrate refuses a candidate that only adds a record while keeping every existing record" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = remove_documents(scratch, fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    const extra_digest = hash_hex("extra bytes");
    const extra_path = std.fmt.allocPrint(scratch, "{s}/applications/extra.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const extra = test_spec_t{ .id = "desktop:extra", .kind = "desktop_entry", .path = extra_path, .sha256 = &extra_digest };
    const target_digest = hash_hex("target bytes");
    const keep_digest = hash_hex("keep bytes");
    const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const keep_path = std.fmt.allocPrint(scratch, "{s}/applications/keep.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &target_digest };
    const keep = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
    const grown = test_document(scratch, 1, &[_]test_spec_t{ target, keep, extra });
    try std.testing.expectError(error.ManifestMismatch, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = grown }));
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(docs.original, on_disk);
}

test "status recovers a pending journal before classifying any record" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    store.fail_after.store(0, .monotonic);
    try std.testing.expect(optional_transaction_id(fixture.state) != null);
    var listed = try store.status(std.testing.allocator);
    defer listed.deinit();
    try std.testing.expectEqual(@as(usize, 2), listed.items.len);
    try std.testing.expectEqual(record_state_t.Ok, listed.items[0].state);
    try std.testing.expect(optional_transaction_id(fixture.state) == null);
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(docs.original, on_disk);
}

test "committed remove refuses a displaced manifest with a foreign identity before any artifact change" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = remove_documents(arena.allocator(), fixture.data_path);
    install_remove_artifacts(&fixture);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 9, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = "desktop:target", .candidate_manifest = docs.candidate }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const temp_name = manifest_temp_name(txid);
    const temp: [:0]const u8 = temp_name[0 .. std.mem.indexOfScalar(u8, &temp_name, 0) orelse unreachable :0];
    const saved_temp: [:0]const u8 = "displaced-identity-saved.tmp";

    if (c.renameat(fixture.state.fd, temp.ptr, fixture.state.fd, saved_temp.ptr) != 0) @panic("fixture rename");
    write_status_file(fixture.state, temp, "foreign displaced manifest", 0o600);
    try std.testing.expectError(error.ArtifactConflict, store.recover());
    try std.testing.expect(target_exists(fixture.data));
    try std.testing.expect(optional_transaction_id(fixture.state) != null);
    if (c.unlinkat(fixture.state.fd, temp.ptr, 0) != 0) @panic("fixture unlink");
    if (c.renameat(fixture.state.fd, saved_temp.ptr, fixture.state.fd, temp.ptr) != 0) @panic("fixture rename");
    try store.recover();
    try std.testing.expect(!target_exists(fixture.data));
}

test "migrate refuses version downgrades, reordered records, and unparsable versions before mutation" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = remove_documents(scratch, fixture.data_path);
    install_remove_artifacts(&fixture);
    const current = std.mem.replaceOwned(u8, scratch, docs.original, "\"waddle_version\":\"0.2.0\"", "\"waddle_version\":\"0.3.0\"") catch @panic("fixture current");
    try write_test_file(fixture.state.fd, "manifest.json", current, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    try std.testing.expectError(error.VersionDowngrade, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = docs.original }));
    const reversed_text = std.mem.replaceOwned(u8, scratch, current, "\"waddle_version\":\"0.3.0\"", "\"waddle_version\":\"0.3.1\"") catch @panic("fixture");
    const target_digest = hash_hex("target bytes");
    const keep_digest = hash_hex("keep bytes");
    const target_path = std.fmt.allocPrint(scratch, "{s}/applications/target.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const keep_path = std.fmt.allocPrint(scratch, "{s}/applications/keep.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const target = test_spec_t{ .id = "desktop:target", .kind = "desktop_entry", .path = target_path, .sha256 = &target_digest };
    const keep = test_spec_t{ .id = "desktop:keep", .kind = "desktop_entry", .path = keep_path, .sha256 = &keep_digest };
    const reordered = test_document(scratch, 1, &[_]test_spec_t{ keep, target });
    const newer = std.mem.replaceOwned(u8, scratch, current, "\"waddle_version\":\"0.3.0\"", "\"waddle_version\":\"0.10.0\"") catch @panic("fixture");
    try std.testing.expectError(error.ManifestMismatch, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = reordered }));
    _ = reversed_text;
    const unparsable = std.mem.replaceOwned(u8, scratch, current, "\"waddle_version\":\"0.3.0\"", "\"waddle_version\":\"0.3.0-rc1\"") catch @panic("fixture");
    try std.testing.expectError(error.InvalidManifest, store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = unparsable }));
    const same_content = try store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = std.mem.replaceOwned(u8, scratch, current, "2026-10-25T00:00:00Z", "2026-10-26T00:00:00Z") catch @panic("fixture") });
    try std.testing.expect(same_content.changed);
    const upgraded = try store.migrate_record(.{ .record_id = "desktop:target", .candidate_manifest = newer });
    try std.testing.expect(upgraded.changed);
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(newer, on_disk);
}

test "version order compares numeric components and rejects unordered text" {
    try std.testing.expectEqual(std.math.Order.eq, try version_order("0.2", "0.2.0"));
    try std.testing.expectEqual(std.math.Order.gt, try version_order("1.10.0", "1.9.0"));
    try std.testing.expectEqual(std.math.Order.lt, try version_order("0.2.0", "0.2.1"));
    try std.testing.expectError(error.InvalidManifest, version_order("0.2.x", "0.2.0"));
    try std.testing.expectError(error.InvalidManifest, version_order("0.2.0", "0.2."));
    try std.testing.expectError(error.InvalidManifest, version_order("1234567890", "0"));
}

test "recreate delta keeps the target identity exact and every other record equal" {
    const digest_a = hash_hex("a");
    const digest_b = hash_hex("b");
    const digest_c = hash_hex("c");
    const old = [_]manifest.record_t{
        test_record("x:one", "/data/applications/one.desktop", &digest_a),
        test_record("x:two", "/data/applications/two.desktop", &digest_b),
    };
    var digest_moved = test_record("x:one", "/data/applications/one.desktop", &digest_c);
    digest_moved.created_by = "0.3.0";
    const changed_digest = [_]manifest.record_t{ digest_moved, old[1] };
    try validate_recreate_delta(&old, &changed_digest, "x:one");
    const moved = [_]manifest.record_t{ test_record("x:one", "/data/applications/moved.desktop", &digest_a), old[1] };
    try std.testing.expectError(error.ManifestMismatch, validate_recreate_delta(&old, &moved, "x:one"));
    const other_changed = [_]manifest.record_t{ old[0], test_record("x:two", "/data/applications/two.desktop", &digest_c) };
    try std.testing.expectError(error.ManifestMismatch, validate_recreate_delta(&old, &other_changed, "x:one"));
    try std.testing.expectError(error.ManifestMismatch, validate_recreate_delta(&old, old[0..1], "x:one"));
    const missing = [_]manifest.record_t{ old[0], test_record("x:three", "/data/applications/three.desktop", &digest_c) };
    try std.testing.expectError(error.ManifestMismatch, validate_recreate_delta(&old, &missing, "x:one"));
}

test "recreate refuses absent manifests, unsafe modes, and paths outside the roots" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = recreate_documents(scratch, fixture.data_path);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    try std.testing.expectError(error.FileNotFound, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    try std.testing.expectError(error.InvalidArgument, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes, .artifact_mode = 0o600 }));
    try std.testing.expectError(error.InvalidArgument, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = "", .artifact_bytes = docs.bytes }));
    const outside_digest = hash_hex(docs.bytes);
    const outside = test_spec_t{ .id = "handler:waddle-open", .kind = "handler_entry", .path = "/unused-root/applications/handler.desktop", .sha256 = &outside_digest };
    const outside_list = [_]test_spec_t{outside};
    const outside_original = test_document(scratch, 1, &outside_list);
    if (c.unlinkat(fixture.state.fd, "manifest.json", 0) != 0) @panic("fixture unlink");
    try write_test_file(fixture.state.fd, "manifest.json", outside_original, 0o600);
    const outside_candidate = std.mem.replaceOwned(u8, scratch, outside_original, "2026-10-25T00:00:00Z", "2026-10-26T00:00:00Z") catch @panic("fixture candidate");
    try std.testing.expectError(error.InvalidPath, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = outside_candidate, .artifact_bytes = docs.bytes }));
}

test "ensure_directory refuses a regular file in a directory component" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    write_status_file(fixture.data, "plain-file", "not a directory", 0o644);
    try std.testing.expectError(error.AccessDenied, store.ensure_directory(.Data, "plain-file/child"));
}

test "recreate journal evidence rejects digest, delta, and path changes after a crash" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();
    const docs = recreate_documents(scratch, fixture.data_path);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    const other_digest = hash_hex("different");
    const digest_path = std.fmt.allocPrint(scratch, "{s}/applications/handler.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const changed_digest = test_spec_t{ .id = "handler:waddle-open", .kind = "handler_entry", .path = digest_path, .sha256 = &other_digest };
    const changed_list = [_]test_spec_t{changed_digest};
    const candidate_digest = test_document(scratch, 1, &changed_list);
    rewrite_manifest_evidence(fixture.state, txid, parsed.value, docs.original, candidate_digest);
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    const moved_path = std.fmt.allocPrint(scratch, "{s}/applications/moved.desktop", .{fixture.data_path}) catch @panic("fixture path");
    const digest = hash_hex(docs.bytes);
    const moved = test_spec_t{ .id = "handler:waddle-open", .kind = "handler_entry", .path = moved_path, .sha256 = &digest };
    const moved_list = [_]test_spec_t{moved};
    rewrite_manifest_evidence(fixture.state, txid, parsed.value, docs.original, test_document(scratch, 1, &moved_list));
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    replace_transaction_file(fixture.state, txid, "old-manifest", docs.original, 0o600);
    replace_transaction_file(fixture.state, txid, "new-manifest", docs.candidate, 0o600);
    replace_transaction_file(fixture.state, txid, "journal.json", journal_text, 0o600);
    try store.recover();
    try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
}

test "closed stores and I/O failures in directory creation and snapshot reads fail without corruption" {
    defer {
        injected_io_at = 0;
        injected_io_count = 0;
    }
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = recreate_documents(arena.allocator(), fixture.data_path);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    try std.testing.expectError(error.InvalidArgument, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = "", .artifact_bytes = docs.bytes }));
    try std.testing.expectError(error.FileNotFound, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    store.deinit();
    try std.testing.expectError(error.Closed, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    try std.testing.expectError(error.Closed, store.ensure_directory(.Data, "x"));
    try std.testing.expectError(error.Closed, store.manifest_snapshot(std.testing.allocator));

    var live = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer live.deinit();
    var observed: usize = 0;
    for (1..24) |fault_io| {
        injected_io_at = fault_io;
        injected_io_count = 0;
        const created = live.ensure_directory(.Data, "fault/one/two");
        injected_io_at = 0;
        injected_io_count = 0;
        if (created) |_| {} else |err| {
            observed += 1;
            try std.testing.expect(err == error.IoError or err == error.AccessDenied);
        }
        try live.ensure_directory(.Data, "fault/one/two");
    }
    try std.testing.expect(observed > 0);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    for (1..12) |fault_io| {
        injected_io_at = fault_io;
        injected_io_count = 0;
        const snapshot = live.manifest_snapshot(std.testing.allocator);
        injected_io_at = 0;
        injected_io_count = 0;
        if (snapshot) |maybe| {
            if (maybe) |bytes| std.testing.allocator.free(bytes);
        } else |err| try std.testing.expect(err == error.IoError or err == error.AccessDenied or err == error.RecoveryRequired);
    }
    const final = (try live.manifest_snapshot(std.testing.allocator)) orelse return error.TestExpectedEqual;
    defer std.testing.allocator.free(final);
    try std.testing.expectEqualStrings(docs.original, final);
}

test "recreate publish is idempotent for an identical live artifact and refuses a different one" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = recreate_documents(arena.allocator(), fixture.data_path);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    try store.apply_prepared_v3(parsed.value);
    write_status_file(fixture.data, "applications/handler.desktop", "different bytes", 0o644);
    try std.testing.expectError(error.ArtifactConflict, store.apply_prepared_v3(parsed.value));
    try store.recover();
    const kept = try fixture.data.readFileAlloc(std.testing.allocator, "applications/handler.desktop", 4096);
    defer std.testing.allocator.free(kept);
    try std.testing.expectEqualStrings("different bytes", kept);
}

test "rollback keeps a recreated artifact that the user replaced with a symbolic link" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = recreate_documents(arena.allocator(), fixture.data_path);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    store.fail_after.store(0, .monotonic);
    if (c.unlinkat(fixture.data.fd, "applications/handler.desktop", 0) != 0) @panic("fixture unlink");
    if (c.symlinkat("elsewhere", fixture.data.fd, "applications/handler.desktop") != 0) @panic("fixture symlink");
    try store.recover();
    var link_stat: c.struct_stat = undefined;
    if (c.fstatat(fixture.data.fd, "applications/handler.desktop", &link_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.TestExpectedEqual;
    try std.testing.expect(link_stat.st_mode & c.S_IFMT == c.S_IFLNK);
    const on_disk = read_stored_manifest(fixture.state);
    defer std.testing.allocator.free(on_disk);
    try std.testing.expectEqualStrings(docs.original, on_disk);
}

test "recreate journal mutations of artifact fields are rejected before recovery" {
    var fixture = remove_fixture_t.create(std.testing.allocator);
    defer fixture.destroy(std.testing.allocator);
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const docs = recreate_documents(arena.allocator(), fixture.data_path);
    try write_test_file(fixture.state.fd, "manifest.json", docs.original, 0o600);
    const roots = fixture.roots();
    var store = try store_t.init(std.testing.allocator, fixture.state.fd, &roots);
    defer store.deinit();
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try std.testing.expectError(error.InjectedFailure, store.recreate(.{ .record_id = "handler:waddle-open", .candidate_manifest = docs.candidate, .artifact_bytes = docs.bytes }));
    store.fail_after.store(0, .monotonic);
    const txid = optional_transaction_id(fixture.state) orelse return error.TestExpectedEqual;
    const journal_text = try store.read_tx_file(txid, "journal.json", MaxJournalBytes);
    defer std.testing.allocator.free(journal_text);
    var parsed = try parse_journal_v3(std.testing.allocator, journal_text);
    defer parsed.deinit();
    for (0..7) |case_index| {
        var mutated = parsed.value;
        switch (case_index) {
            0 => mutated.new_artifact_hash = null,
            1 => mutated.new_artifact_mode = 0o600,
            2 => mutated.new_artifact_device = 0,
            3 => mutated.new_artifact_inode = null,
            4 => mutated.root = null,
            5 => mutated.relative_path = null,
            6 => mutated.old_artifact_hash = "0000000000000000000000000000000000000000000000000000000000000000",
            else => unreachable,
        }
        const text = try std.json.stringifyAlloc(std.testing.allocator, mutated, .{});
        defer std.testing.allocator.free(text);
        replace_transaction_file(fixture.state, txid, "journal.json", text, 0o600);
        try std.testing.expectError(error.RecoveryRequired, store.recover());
    }
    replace_transaction_file(fixture.state, txid, "journal.json", journal_text, 0o600);
    try store.recover();
    try expect_recreate_converged(&fixture, .{ .original = docs.original, .candidate = docs.candidate, .bytes = docs.bytes });
}

fn write_test_file(dir_fd: c_int, name: [*:0]const u8, bytes: []const u8, mode: c_uint) !void {
    const fd = c.openat(dir_fd, name, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, mode);
    if (fd < 0) return error.IoError;
    defer _ = c.close(fd);
    try write_all(fd, bytes);
    if (c.fsync(fd) != 0) return error.IoError;
}

fn expect_rewrite_file(fd: c_int, bytes: []const u8) !void {
    if (c.ftruncate(fd, 0) != 0) return error.IoError;
    try write_all(fd, bytes);
    if (c.fsync(fd) != 0) return error.IoError;
}
