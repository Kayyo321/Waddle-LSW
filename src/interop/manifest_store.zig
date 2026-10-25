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
    new_manifest_hash: []const u8,
    new_manifest_device: u64,
    new_manifest_inode: u64,
};
const owned_root_t = struct { kind: root_kind_t, canonical_path: []u8, fd: c_int };
const parent_t = struct { fd: c_int, leaf: []const u8 };
const file_info_t = struct { hash: [32]u8, mode: u32, device: u64, inode: u64 };

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
        if (old_manifest) |bytes| {
            old_manifest_digest = hash_bytes(bytes);
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
            .new_manifest_hash = &hash_hex(request.candidate_manifest),
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
        self.remove_tx_tree(transaction_id) catch |err| {
            return err;
        };
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
            try self.remove_target_temp(root.fd, journal.relative_path, entry.name[0..32].*, journal_artifact_info(journal));
            try self.remove_state_temp(entry.name[0..32].*, journal_manifest_info(journal));
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
        if (journal.old_manifest_hash) |old_hash| {
            const bytes = old_bytes orelse return error.RecoveryRequired;
            if (!hash_text_equal(hash_bytes(bytes), old_hash)) return error.RecoveryRequired;
            if (hash_text_equal(hash_bytes(bytes), journal.new_manifest_hash)) return error.RecoveryRequired;
            var old = manifest.parse(self.allocator, bytes) catch |err| return if (err == error.OutOfMemory) error.OutOfMemory else error.RecoveryRequired;
            defer old.deinit();
            try validate_global_paths(old.records);
            try validate_delta(old.records, candidate.records, journal.record_id);
            if (find_record(old.records, journal.record_id) != null) return error.RecoveryRequired;
        } else {
            if (old_bytes != null or candidate.records.len != 1) return error.RecoveryRequired;
        }
    }

    fn apply_prepared(self: *store_t, journal: journal_t) !void {
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
        try self.validate_optional_temp(self.dir_fd, manifest_temp[0..].ptr, manifest_info, !manifest_published, manifest_published);
        if (!artifact_published) try self.publish_file(root.fd, journal.relative_path, txid, artifact_info);
        if (!manifest_published) try self.publish_manifest(txid, manifest_info, journal.old_manifest_hash);
    }

    fn rollback_prepared(self: *store_t, journal: journal_t) !void {
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
                if (hash_text_equal(found.hash, old_hash)) break :blk false;
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
        try self.validate_optional_temp(self.dir_fd, manifest_temp[0..].ptr, manifest_info, false, manifest_published);
        if (journal.old_manifest_hash) |old_hash| {
            const previous = old_manifest orelse return error.RecoveryRequired;
            if (!hash_text_equal(hash_bytes(previous), old_hash)) return error.RecoveryRequired;
        }
        if (artifact_published) try self.unlink_owned_artifact(root.fd, journal.relative_path, artifact_info);
        if (manifest_published) {
            if (old_manifest) |previous| {
                const restored = try self.create_state_temp(previous, 0o600, txid);
                try self.publish_manifest(txid, restored, journal.new_manifest_hash);
            } else try self.unlink_owned_manifest(manifest_info);
        }
    }

    fn apply_committed(self: *store_t, journal: journal_t) !void {
        const root = self.find_root_by_name(journal.root) orelse return error.RecoveryRequired;
        const current = (try self.inspect(root.fd, journal.relative_path)) orelse return error.RecoveryRequired;
        if (!same_file_info(current, journal_artifact_info(journal))) return error.RecoveryRequired;
        const current_manifest = (try self.inspect_manifest()) orelse return error.RecoveryRequired;
        if (!same_file_info(current_manifest, journal_manifest_info(journal))) return error.RecoveryRequired;

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
            return native_error();
        }
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0) return native_error();
        if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1) return error.AccessDenied;
        const bytes = try read_all(self.allocator, fd, MaxArtifactBytes);
        defer self.allocator.free(bytes);
        return .{ .hash = hash_bytes(bytes), .mode = @intCast(st.st_mode & 0o777), .device = @intCast(st.st_dev), .inode = @intCast(st.st_ino) };
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
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o022 != 0) return error.AccessDenied;
        const mode: u32 = @intCast(st.st_mode & 0o777);
        if ((std.mem.eql(u8, leaf, "journal.json") or std.mem.eql(u8, leaf, "journal.next") or
            std.mem.eql(u8, leaf, "new-manifest") or std.mem.eql(u8, leaf, "old-manifest")) and mode != 0o600) return error.AccessDenied;
        if (std.mem.eql(u8, leaf, "new-artifact") and mode != 0o644 and mode != 0o755) return error.AccessDenied;
        return read_all(self.allocator, fd, limit);
    }

    fn write_journal(self: *store_t, txid: [32]u8, journal: journal_t) !void {
        const bytes = try std.json.stringifyAlloc(self.allocator, journal, .{});
        defer self.allocator.free(bytes);
        if (bytes.len > MaxJournalBytes) return error.JournalTooLarge;
        try self.write_tx_file(txid, "journal.json", bytes, 0o600);
        const parent = try self.open_private_dir("transactions");
        defer _ = c.close(parent);
        if (io_fsync(parent) != 0) return native_error();
    }

    fn publish_phase(self: *store_t, txid: [32]u8, journal: journal_t, phase: []const u8) !void {
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
        const fd = io_openat3(self.dir_fd, "manifest.json", c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
        if (fd < 0) return native_error();
        defer _ = c.close(fd);
        var st: c.struct_stat = undefined;
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o777 != 0o600) return error.AccessDenied;
        return read_all(self.allocator, fd, manifest.MaxManifestBytes);
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

    fn unlink_owned_artifact(self: *store_t, root_fd: c_int, relative: []const u8, expected: file_info_t) !void {
        const current = (try self.inspect(root_fd, relative)) orelse return error.ArtifactConflict;
        if (!same_file_info(current, expected)) return error.ArtifactConflict;
        const parent = try self.open_parent(root_fd, relative);
        defer _ = c.close(parent.fd);
        var leaf: [256]u8 = undefined;
        @memcpy(leaf[0..parent.leaf.len], parent.leaf);
        leaf[parent.leaf.len] = 0;
        var st: c.struct_stat = undefined;
        if (io_fstatat(parent.fd, &leaf, &st, c.AT_SYMLINK_NOFOLLOW) != 0 or
            @as(u64, @intCast(st.st_dev)) != expected.device or @as(u64, @intCast(st.st_ino)) != expected.inode) return error.ArtifactConflict;
        if (io_unlinkat(parent.fd, &leaf, 0) != 0 or io_fsync(parent.fd) != 0) return native_error();
        try self.checkpoint();
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

    fn publish_manifest(self: *store_t, txid: [32]u8, expected: file_info_t, old_hash: ?[]const u8) !void {
        const temp = manifest_temp_name(txid);
        try verify_named_file(self, self.dir_fd, temp[0..].ptr, expected);
        if (old_hash) |digest| {
            const current = try self.inspect_manifest();
            const found = current orelse return error.ArtifactConflict;
            if (!hash_text_equal(found.hash, digest)) return error.ArtifactConflict;
            if (io_renameat(self.dir_fd, temp[0..].ptr, self.dir_fd, "manifest.json") != 0) return native_error();
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
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1) return error.ArtifactConflict;
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
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1) return error.ArtifactConflict;
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
        if (io_fstat(fd, &st) != 0 or st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or st.st_mode & 0o777 != 0o600) return error.AccessDenied;
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
    if (journal.version != 1 or !std.mem.eql(u8, journal.transaction_id, entry_name) or
        journal.phase.len > 16 or journal.record_id.len > manifest.MaxManifestStringBytes or
        journal.root.len > 16 or journal.relative_path.len >= 4096 or
        !valid_record_id(journal.record_id) or !valid_hash_text(journal.new_artifact_hash) or
        !valid_hash_text(journal.new_manifest_hash) or
        journal.old_artifact_hash != null or journal.old_artifact_mode != 0 or
        journal.new_artifact_device == 0 or journal.new_artifact_inode == 0 or
        journal.new_manifest_device == 0 or journal.new_manifest_inode == 0 or
        (journal.old_manifest_hash != null and !valid_hash_text(journal.old_manifest_hash.?))) return error.RecoveryRequired;
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
    if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.getuid() or st.st_nlink != 1 or
        @as(u64, @intCast(st.st_dev)) != expected.device or @as(u64, @intCast(st.st_ino)) != expected.inode or
        @as(u32, @intCast(st.st_mode & 0o777)) != expected.mode) return error.ArtifactConflict;
    const fd = io_openat3(parent_fd, name, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK);
    if (fd < 0) return error.ArtifactConflict;
    defer _ = c.close(fd);
    var opened: c.struct_stat = undefined;
    if (io_fstat(fd, &opened) != 0 or opened.st_mode & c.S_IFMT != c.S_IFREG or opened.st_uid != c.getuid() or opened.st_nlink != 1 or
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
    return std.fmt.bytesToHex(hash_bytes(bytes), .lower);
}
fn hash_hex_from_array(hash: [32]u8) [64]u8 {
    return std.fmt.bytesToHex(hash, .lower);
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
fn journal_manifest_info(journal: journal_t) file_info_t {
    return .{ .hash = hash_from_text(journal.new_manifest_hash), .mode = 0o600, .device = journal.new_manifest_device, .inode = journal.new_manifest_inode };
}
fn same_file_info(a: file_info_t, b: file_info_t) bool {
    return a.device == b.device and a.inode == b.inode and a.mode == b.mode and std.mem.eql(u8, &a.hash, &b.hash);
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
    return std.fmt.bytesToHex(bytes, .lower);
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
    const txid = [_]u8{'a'} ** 32;
    try std.testing.expectEqualStrings(".cleanup-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", cleanup_name(txid)[0..41]);
    try std.testing.expect(std.mem.startsWith(u8, &artifact_temp_name(txid), ".waddle-"));
    try std.testing.expect(std.mem.startsWith(u8, &manifest_temp_name(txid), ".manifest-"));
    try std.testing.expect(valid_txid("0123456789abcdef0123456789abcdef"));
    try std.testing.expect(!valid_txid("0123456789abcdef0123456789abcdeg"));
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
    try std.testing.expectError(error.UnsupportedReplacement, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate, .backup_modified = true }));
    try data.writeFile(.{ .sub_path = "applications/preflight.desktop", .data = payload });
    try std.testing.expectError(error.ArtifactConflict, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
    try std.testing.expectError(error.FileNotFound, state.access("manifest.json", .{}));
    store.deinit();
    try std.testing.expectError(error.Closed, store.recover());
    try std.testing.expectError(error.Closed, store.commit(.{ .record_id = "desktop:preflight", .prior = .Absent, .artifact_bytes = payload, .candidate_manifest = candidate }));
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
    if (c.mkdirat(temporary.dir.fd, "state", 0o700) != 0) return error.IoError;
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

fn transaction_evidence_exists(transactions: *std.fs.Dir, name: []const u8) !void {
    var transaction = try transactions.openDir(name, .{});
    defer transaction.close();
    try transaction.access("journal.json", .{});
    try transaction.access("new-artifact", .{});
    try transaction.access("new-manifest", .{});
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
    for (0..8) |case_index| {
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
        const failure_step = if (case_index == 7) 9 else case_index + 7;
        if (case_index != 0) store.fail_after.store(failure_step, .monotonic);
        const committed = store.commit(.{ .record_id = "desktop:added", .prior = .Absent, .artifact_bytes = new_payload, .candidate_manifest = new_manifest });
        if (case_index != 0) {
            try std.testing.expectError(error.InjectedFailure, committed);
            store.fail_after.store(0, .monotonic);
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

    try store.unlink_owned_artifact(data.fd, "applications/temp-owned", temp_artifact);
    try std.testing.expectError(error.ArtifactConflict, store.unlink_owned_artifact(data.fd, "applications/temp-owned", temp_artifact));
    try std.testing.expectError(error.AccessDenied, store.unlink_owned_artifact(data.fd, "applications/owned.desktop", temp_artifact));
    try std.testing.expectError(error.ArtifactConflict, store.unlink_owned_artifact(data.fd, "applications/missing", artifact));
    try store.unlink_owned_manifest(owned_manifest);
    try std.testing.expectError(error.ArtifactConflict, store.unlink_owned_manifest(owned_manifest));

    try write_test_file(applications.fd, "named", "named owned", 0o600);
    var named_stat: c.struct_stat = undefined;
    if (c.fstatat(applications.fd, "named", &named_stat, c.AT_SYMLINK_NOFOLLOW) != 0) return error.IoError;
    const named_info = file_info_t{ .hash = hash_bytes("named owned"), .mode = 0o600, .device = @intCast(named_stat.st_dev), .inode = @intCast(named_stat.st_ino) };
    try unlink_named_if_matches(std.testing.allocator, applications.fd, "named", named_info);
    try unlink_named_if_matches(std.testing.allocator, applications.fd, "named", named_info);
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
    for (1..13) |fault_index| {
        try write_test_file(applications.fd, "unlink-fault.desktop", "unlink fault", 0o644);
        const expected = (try store.inspect(data.fd, "applications/unlink-fault.desktop")).?;
        injected_io_count = 0;
        injected_io_at = fault_index;
        const result = store.unlink_owned_artifact(data.fd, "applications/unlink-fault.desktop", expected);
        const faulted = injected_io_at == 0;
        injected_io_at = 0;
        injected_io_count = 0;
        if (result) |_| {
            try std.testing.expect(!faulted);
            artifact_fault_success = true;
            break;
        } else |err| {
            try std.testing.expect(faulted);
            try std.testing.expect(err == error.IoError or err == error.ArtifactConflict);
            if (applications.access("unlink-fault.desktop", .{})) |_| {
                if (c.unlinkat(applications.fd, "unlink-fault.desktop", 0) != 0) return error.IoError;
            } else |_| {}
        }
    }
    try std.testing.expect(artifact_fault_success);

    try write_test_file(applications.fd, "race-target.desktop", "owned before race", 0o644);
    try write_test_file(applications.fd, "race-source.desktop", "foreign replacement", 0o644);
    const race_artifact = (try store.inspect(data.fd, "applications/race-target.desktop")).?;
    injected_replace_fstatat_fd = applications.fd;
    injected_replace_fstatat_source = "race-source.desktop";
    injected_replace_fstatat_target = "race-target.desktop";
    injected_replace_fstatat_count = 0;
    injected_replace_fstatat_at = 1;
    try std.testing.expectError(error.ArtifactConflict, store.unlink_owned_artifact(data.fd, "applications/race-target.desktop", race_artifact));
    injected_replace_fstatat_at = 0;
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

fn write_test_file(dir_fd: c_int, name: [*:0]const u8, bytes: []const u8, mode: c_uint) !void {
    const fd = c.openat(dir_fd, name, c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, mode);
    if (fd < 0) return error.IoError;
    defer _ = c.close(fd);
    try write_all(fd, bytes);
    if (c.fsync(fd) != 0) return error.IoError;
}
