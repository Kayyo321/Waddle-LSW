//! Private, recoverable schema-1 manifest storage for supported user artifacts.
//! This module does not generate artifacts or start sessions. Callers supply
//! serialized manifests and explicit actions; all paths are reauthorized here.
const std = @import("std");
const manifest = @import("manifest.zig");
const c = @cImport({
    @cInclude("errno.h");
    @cInclude("fcntl.h");
    @cInclude("sys/file.h");
    @cInclude("sys/stat.h");
    @cInclude("sys/types.h");
    @cInclude("sys/wait.h");
    @cInclude("unistd.h");
    @cInclude("stdlib.h");
    @cInclude("stdio.h");
});

/// Maximum encoded transaction journal size.
pub const MaxJournalBytes = 4 * 1024 * 1024;
/// Maximum outstanding journals considered by one recovery pass.
pub const MaxJournals = 32;
/// Maximum artifact actions recorded by one transaction.
pub const MaxActions = manifest.MaxManifestRecords;
/// Maximum bytes read from one artifact or staged payload.
pub const MaxArtifactBytes = manifest.MaxManifestBytes;
/// Maximum names collected from one private state directory before recovery fails closed.
pub const MaxDirectoryEntries = MaxActions + MaxJournals * 3 + 16;

/// State namespace selected by the caller.
/// @param User accepts explicit private test/user state roots.
/// @param System requires euid 0 and the exact fixed system state root; host
/// artifact mutation remains unsupported in this implementation.
pub const scope_t = enum { User, System };

/// Caller-resolved absolute roots. This store does not resolve XDG environment
/// variables or xdg-user-dir; production root resolution belongs to its caller.
/// @ownership All slices are borrowed for `store_t.open` and copied on success.
/// @errors InvalidPath, AccessDenied, Busy, or I/O/allocation errors.
/// @thread_safety The resulting store holds an OS process lock; do not access
/// one store instance concurrently from multiple threads.
pub const store_config_t = struct {
    /// @in Scope discriminator.
    scope: scope_t,
    /// @in Absolute private state directory, caller-resolved.
    state_dir: []const u8,
    /// @in Absolute caller-resolved XDG data home.
    data_home: []const u8,
    /// @in Absolute caller-resolved XDG config home.
    config_home: []const u8,
    /// Optional validated xdg-user-dir Desktop root; required for shortcuts.
    desktop_root: ?[]const u8 = null,
};

const test_fixture_t = struct {
    allocator: std.mem.Allocator,
    path: [:0]u8,
    state_path: []u8,
    data_path: []u8,
    config_path: []u8,
    config: store_config_t,

    fn init(allocator: std.mem.Allocator) !test_fixture_t {
        const path = try std.fmt.allocPrintZ(allocator, "/tmp/waddle-manifest-test-{d}-XXXXXX", .{c.getpid()});
        errdefer allocator.free(path);
        if (c.mkdtemp(path.ptr) == null) return io_error();
        errdefer std.fs.cwd().deleteTree(path) catch {};
        const state = try std.fs.path.join(allocator, &.{ path, "state" });
        errdefer allocator.free(state);
        const data = try std.fs.path.join(allocator, &.{ path, "data" });
        errdefer allocator.free(data);
        const config = try std.fs.path.join(allocator, &.{ path, "config" });
        errdefer allocator.free(config);
        const apps = try std.fs.path.join(allocator, &.{ data, "applications" });
        defer allocator.free(apps);
        const merged = try std.fs.path.join(allocator, &.{ config, "menus", "applications-merged" });
        defer allocator.free(merged);
        const autostart = try std.fs.path.join(allocator, &.{ config, "autostart" });
        defer allocator.free(autostart);
        const desktop_directories = try std.fs.path.join(allocator, &.{ data, "desktop-directories" });
        defer allocator.free(desktop_directories);
        try std.fs.cwd().makePath(state);
        try std.fs.cwd().makePath(apps);
        try std.fs.cwd().makePath(merged);
        try std.fs.cwd().makePath(autostart);
        try std.fs.cwd().makePath(desktop_directories);
        try chmod_path(allocator, state, 0o700);
        return .{
            .allocator = allocator,
            .path = path,
            .state_path = state,
            .data_path = data,
            .config_path = config,
            .config = .{ .scope = .User, .state_dir = state, .data_home = data, .config_home = config },
        };
    }

    fn deinit(self: *test_fixture_t) void {
        std.fs.cwd().deleteTree(self.path) catch {};
        self.allocator.free(self.state_path);
        self.allocator.free(self.data_path);
        self.allocator.free(self.config_path);
        self.allocator.free(self.path);
    }

    fn target_path(self: test_fixture_t) ![]u8 {
        return std.fs.path.join(self.allocator, &.{ self.config.data_home, "applications", "waddle.device.app.desktop" });
    }

    fn artifact_mode(self: test_fixture_t) !u32 {
        const root = try std.fs.path.join(self.allocator, &.{ self.config.data_home, "applications" });
        defer self.allocator.free(root);
        return target_mode(self.allocator, .Applications, root, "waddle.device.app.desktop");
    }

    fn manifest_bytes(self: test_fixture_t, content: []const u8) ![]u8 {
        const target = try self.target_path();
        defer self.allocator.free(target);
        const digest = try hash_hex(self.allocator, content);
        defer self.allocator.free(digest);
        return std.fmt.allocPrint(self.allocator,
            "{{\"schema\":1,\"waddle_version\":\"test\",\"updated\":\"2026-10-24T00:00:00Z\",\"records\":[{{\"id\":\"entry-1\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"test\",\"prior\":null}}]}}",
            .{ target, digest });
    }

    fn write_initial(self: test_fixture_t) !struct { old_manifest: []u8, old_hash: [32]u8, target: []u8 } {
        const target = try self.target_path();
        errdefer self.allocator.free(target);
        try std.fs.cwd().writeFile(.{ .sub_path = target, .data = "old artifact" });
        try chmod_path(self.allocator, target, 0o644);
        const old_manifest = try self.manifest_bytes("old artifact");
        errdefer self.allocator.free(old_manifest);
        const state_manifest = try std.fs.path.join(self.allocator, &.{ self.config.state_dir, "manifest.json" });
        defer self.allocator.free(state_manifest);
        try std.fs.cwd().writeFile(.{ .sub_path = state_manifest, .data = old_manifest });
        try chmod_path(self.allocator, state_manifest, 0o600);
        return .{ .old_manifest = old_manifest, .old_hash = hash_raw(old_manifest), .target = target };
    }
};

fn test_action(bytes: []const u8, expected: ?[32]u8, operation: artifact_operation_t) artifact_action_t {
    return .{
        .record_id = "entry-1",
        .root = .Applications,
        .relative_path = "waddle.device.app.desktop",
        .operation = operation,
        .expected_current_sha256 = expected,
        .bytes = if (operation == .Write) bytes else null,
    };
}

fn decode_allocation_probe(allocator: std.mem.Allocator, bytes: []const u8) !void {
    var parsed = try decode_journal(allocator, bytes, "0123456789abcdef0123456789abcdef.json");
    defer parsed.deinit();
}

fn commit_allocation_probe(allocator: std.mem.Allocator) !void {
    var fixture = try test_fixture_t.init(allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer allocator.free(initial.old_manifest);
    defer allocator.free(initial.target);
    var store = try store_t.open(allocator, fixture.config);
    defer store.close();
    const candidate = try fixture.manifest_bytes("new artifact");
    defer allocator.free(candidate);
    const action = test_action("new artifact", hash_raw("old artifact"), .Write);
    store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} }) catch |err| {
        // A failed allocator may also prevent rollback bookkeeping; the
        // durable journal must then remain for a later recovery pass.
        if (err == error.RecoveryRequired) return;
        return err;
    };
}

test "commit publishes artifact before the manifest and verifies exact hashes" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer std.testing.allocator.free(initial.old_manifest);
    defer std.testing.allocator.free(initial.target);
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const candidate = try fixture.manifest_bytes("new artifact");
    defer std.testing.allocator.free(candidate);
    const action = test_action("new artifact", hash_raw("old artifact"), .Write);
    try store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} });
    const artifact = try std.fs.cwd().readFileAlloc(std.testing.allocator, initial.target, MaxArtifactBytes);
    defer std.testing.allocator.free(artifact);
    try std.testing.expectEqualStrings("new artifact", artifact);
    const manifest_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "manifest.json" });
    defer std.testing.allocator.free(manifest_path);
    const saved = try std.fs.cwd().readFileAlloc(std.testing.allocator, manifest_path, manifest.MaxManifestBytes);
    defer std.testing.allocator.free(saved);
    try std.testing.expectEqualSlices(u8, candidate, saved);
    try store.recover();
}

test "first install can create a new recorded artifact from an absent manifest" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const candidate = try fixture.manifest_bytes("first artifact");
    defer std.testing.allocator.free(candidate);
    const action = test_action("first artifact", null, .Write);
    try store.commit(.{ .expected_manifest_sha256 = null, .candidate_manifest = candidate, .actions = &.{action} });
    const target = try fixture.target_path();
    defer std.testing.allocator.free(target);
    const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, target, MaxArtifactBytes);
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqualStrings("first artifact", bytes);
}

test "preflight conflict and unsupported paths leave the artifact unchanged" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer std.testing.allocator.free(initial.old_manifest);
    defer std.testing.allocator.free(initial.target);
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const candidate = try fixture.manifest_bytes("new artifact");
    defer std.testing.allocator.free(candidate);
    const action = test_action("new artifact", hash_raw("wrong"), .Write);
    try std.testing.expectError(error.Conflict, store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} }));
    try std.testing.expectError(error.InvalidPath, validate_relative("../escape"));
    try std.testing.expectError(error.UnsupportedPolicy, validate_root_relative(.Applications, "other.desktop"));
    const artifact = try std.fs.cwd().readFileAlloc(std.testing.allocator, initial.target, MaxArtifactBytes);
    defer std.testing.allocator.free(artifact);
    try std.testing.expectEqualStrings("old artifact", artifact);
}

test "record transitions require explicit actions and unchanged records must match installed bytes" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer std.testing.allocator.free(initial.old_manifest);
    defer std.testing.allocator.free(initial.target);
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const changed = try fixture.manifest_bytes("new artifact");
    defer std.testing.allocator.free(changed);
    try std.testing.expectError(error.InvalidAction, store.commit(.{
        .expected_manifest_sha256 = initial.old_hash,
        .candidate_manifest = changed,
        .actions = &.{},
    }));
    try std.fs.cwd().writeFile(.{ .sub_path = initial.target, .data = "external edit" });
    try std.testing.expectError(error.Conflict, store.commit(.{
        .expected_manifest_sha256 = initial.old_hash,
        .candidate_manifest = initial.old_manifest,
        .actions = &.{},
    }));
}

test "manifest lock is nonblocking and released by close" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    var first = try store_t.open(std.testing.allocator, fixture.config);
    defer first.close();
    try std.testing.expectError(error.Busy, store_t.open(std.testing.allocator, fixture.config));
    first.close();
    var second = try store_t.open(std.testing.allocator, fixture.config);
    defer second.close();
}

test "force preserves exact modified artifact bytes in private retained backup" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer std.testing.allocator.free(initial.old_manifest);
    defer std.testing.allocator.free(initial.target);
    try std.fs.cwd().writeFile(.{ .sub_path = initial.target, .data = "user edit" });
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const candidate = try fixture.manifest_bytes("replacement");
    defer std.testing.allocator.free(candidate);
    // The expected hash can match the user's edit; force retention is based
    // on mismatch with the old manifest record, not this caller observation.
    var action = test_action("replacement", hash_raw("user edit"), .Write);
    action.force = true;
    try store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} });
    const backups_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "backups" });
    defer std.testing.allocator.free(backups_path);
    var backups = try std.fs.cwd().openDir(backups_path, .{ .iterate = true });
    defer backups.close();
    var it = backups.iterate();
    var backup_name: ?[]u8 = null;
    while (try it.next()) |entry| {
        if (!std.mem.endsWith(u8, entry.name, ".meta")) {
            backup_name = try std.testing.allocator.dupe(u8, entry.name);
            break;
        }
    }
    const owned_backup_name = backup_name orelse return error.TestExpectedBackup;
    defer std.testing.allocator.free(owned_backup_name);
    const backup_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "backups", owned_backup_name });
    defer std.testing.allocator.free(backup_path);
    const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, backup_path, MaxArtifactBytes);
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqualStrings("user edit", bytes);
    const metadata_name = try std.fmt.allocPrint(std.testing.allocator, "{s}.meta", .{owned_backup_name});
    defer std.testing.allocator.free(metadata_name);
    const metadata_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "backups", metadata_name });
    defer std.testing.allocator.free(metadata_path);
    const metadata = try std.fs.cwd().readFileAlloc(std.testing.allocator, metadata_path, 1024);
    defer std.testing.allocator.free(metadata);
    try std.testing.expect(std.mem.indexOf(u8, metadata, "\"mode\":420") != null);
}

test "incomplete retained backup pair is preserved and blocks recovery" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    try write_child(store.backups_fd, "crashed-payload", "retained bytes", 0o600, true);
    try std.testing.expectError(error.RecoveryRequired, store.recover());
    const backup_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "backups", "crashed-payload" });
    defer std.testing.allocator.free(backup_path);
    const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, backup_path, 1024);
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqualStrings("retained bytes", bytes);
}

test "retained backup metadata-only malformed and mismatched pairs block recovery" {
    const cases = [_]struct { name: []const u8, payload: ?[]const u8, metadata: []const u8 }{
        .{ .name = "metadata-only", .payload = null, .metadata = "{\"schema\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"mode\":420}" },
        .{ .name = "malformed", .payload = "payload", .metadata = "{invalid" },
        .{ .name = "mismatched", .payload = "payload", .metadata = "{\"schema\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"mode\":420}" },
    };
    for (cases) |case| {
        var fixture = try test_fixture_t.init(std.testing.allocator);
        defer fixture.deinit();
        var store = try store_t.open(std.testing.allocator, fixture.config);
        defer store.close();
        const payload_name = try std.testing.allocator.dupeZ(u8, case.name);
        defer std.testing.allocator.free(payload_name);
        if (case.payload) |payload| try write_child(store.backups_fd, payload_name, payload, 0o600, true);
        const metadata_name = try std.fmt.allocPrintZ(std.testing.allocator, "{s}.meta", .{case.name});
        defer std.testing.allocator.free(metadata_name);
        try write_child(store.backups_fd, metadata_name, case.metadata, 0o600, true);
        try std.testing.expectError(error.RecoveryRequired, store.recover());
        const saved_metadata = try read_child(std.testing.allocator, store.backups_fd, metadata_name, 1024, true);
        defer std.testing.allocator.free(saved_metadata);
        try std.testing.expectEqualStrings(case.metadata, saved_metadata);
        if (case.payload) |payload| {
            const saved_payload = try read_child(std.testing.allocator, store.backups_fd, case.name, 1024, true);
            defer std.testing.allocator.free(saved_payload);
            try std.testing.expectEqualStrings(payload, saved_payload);
        }
    }
}

test "crashes at durable boundaries recover to a complete old or new generation" {
    var completed = false;
    for (1..80) |point| {
        var fixture = try test_fixture_t.init(std.testing.allocator);
        defer fixture.deinit();
        const initial = try fixture.write_initial();
        defer std.testing.allocator.free(initial.old_manifest);
        defer std.testing.allocator.free(initial.target);
        const candidate = try fixture.manifest_bytes("new artifact");
        defer std.testing.allocator.free(candidate);
        var action = test_action("new artifact", hash_raw("old artifact"), .Write);
        action.mode = 0o755;
        test_boundary_count = 0;
        test_crash_at = point;
        const child = c.fork();
        if (child < 0) return io_error();
        if (child == 0) {
            var child_store = store_t.open(std.testing.allocator, fixture.config) catch c._exit(98);
            child_store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} }) catch c._exit(97);
            child_store.close();
            c._exit(0);
        }
        var status: c_int = 0;
        if (c.waitpid(child, &status, 0) != child) return io_error();
        test_crash_at = null;
        test_boundary_count = 0;
        const exit_status = (status >> 8) & 0xff;
        if (exit_status == 0) {
            completed = true;
        } else try std.testing.expectEqual(@as(c_int, 99), exit_status);
        var store = try store_t.open(std.testing.allocator, fixture.config);
        defer store.close();
        try store.recover();
        const manifest_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "manifest.json" });
        defer std.testing.allocator.free(manifest_path);
        const saved = try std.fs.cwd().readFileAlloc(std.testing.allocator, manifest_path, manifest.MaxManifestBytes);
        defer std.testing.allocator.free(saved);
        const artifact = try std.fs.cwd().readFileAlloc(std.testing.allocator, initial.target, MaxArtifactBytes);
        defer std.testing.allocator.free(artifact);
        const old_generation = std.mem.eql(u8, saved, initial.old_manifest) and std.mem.eql(u8, artifact, "old artifact");
        const new_generation = std.mem.eql(u8, saved, candidate) and std.mem.eql(u8, artifact, "new artifact");
        try std.testing.expect(old_generation or new_generation);
        try std.testing.expectEqual(if (new_generation) @as(u32, 0o755) else @as(u32, 0o644), try fixture.artifact_mode());
        if (completed) break;
    }
    test_crash_at = null;
    try std.testing.expect(completed);
}

test "injected durable I/O errors recover idempotently" {
    for (1..60) |point| {
        var fixture = try test_fixture_t.init(std.testing.allocator);
        defer fixture.deinit();
        const initial = try fixture.write_initial();
        defer std.testing.allocator.free(initial.old_manifest);
        defer std.testing.allocator.free(initial.target);
        const candidate = try fixture.manifest_bytes("new artifact");
        defer std.testing.allocator.free(candidate);
        var action = test_action("new artifact", hash_raw("old artifact"), .Write);
        action.mode = 0o755;
        var store = try store_t.open(std.testing.allocator, fixture.config);
        defer store.close();
        test_boundary_count = 0;
        test_fail_at = point;
        const result = store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} });
        test_fail_at = null;
        test_boundary_count = 0;
        if (result) |_| {} else |err| try std.testing.expect(err == error.InjectedIoFailure or err == error.RecoveryRequired);
        try store.recover();
        try store.recover();
        const manifest_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "manifest.json" });
        defer std.testing.allocator.free(manifest_path);
        const saved = try std.fs.cwd().readFileAlloc(std.testing.allocator, manifest_path, manifest.MaxManifestBytes);
        defer std.testing.allocator.free(saved);
        const artifact = try std.fs.cwd().readFileAlloc(std.testing.allocator, initial.target, MaxArtifactBytes);
        defer std.testing.allocator.free(artifact);
        const old_generation = std.mem.eql(u8, saved, initial.old_manifest) and std.mem.eql(u8, artifact, "old artifact");
        const new_generation = std.mem.eql(u8, saved, candidate) and std.mem.eql(u8, artifact, "new artifact");
        try std.testing.expect(old_generation or new_generation);
        try std.testing.expectEqual(if (new_generation) @as(u32, 0o755) else @as(u32, 0o644), try fixture.artifact_mode());
    }
    test_fail_at = null;
}

test "journal decoding and complete commits survive allocator failure sweeps" {
    const sample = journal_t{
        .schema = 1,
        .transaction_id = "0123456789abcdef0123456789abcdef",
        .phase = .Prepared,
        .old_manifest_exists = false,
        .old_manifest_sha256 = null,
        .new_manifest_sha256 = [_]u8{'0'} ** 64,
        .actions = &.{},
        .state_identity = .{ .dev = 1, .ino = 1 },
    };
    const encoded = try encode_journal(std.testing.allocator, sample);
    defer std.testing.allocator.free(encoded);
    try std.testing.checkAllAllocationFailures(std.testing.allocator, decode_allocation_probe, .{encoded});
    var reached_success = false;
    for (0..512) |fail_index| {
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = fail_index });
        commit_allocation_probe(failing.allocator()) catch |err| {
            if (err != error.OutOfMemory and err != error.RecoveryRequired) return err;
        };
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        if (!failing.has_induced_failure) {
            reached_success = true;
            break;
        }
    }
    try std.testing.expect(reached_success);
}

test "journal decoder rejects unknown keys and noncanonical phase and root names" {
    const unknown_key = "{\"schema\":1,\"transaction_id\":\"0123456789abcdef0123456789abcdef\",\"phase\":\"prepared\",\"old_manifest_exists\":false,\"old_manifest_sha256\":null,\"new_manifest_sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"actions\":[],\"extra\":true}";
    try std.testing.expectError(error.UnknownField, decode_journal(std.testing.allocator, unknown_key, "0123456789abcdef0123456789abcdef.json"));
    const bad_phase = "{\"schema\":1,\"transaction_id\":\"0123456789abcdef0123456789abcdef\",\"phase\":\"Prepared\",\"old_manifest_exists\":false,\"old_manifest_sha256\":null,\"new_manifest_sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"state_dev\":1,\"state_ino\":1,\"actions\":[]}";
    try std.testing.expectError(error.InvalidJournal, decode_journal(std.testing.allocator, bad_phase, "0123456789abcdef0123456789abcdef.json"));
    const bad_root = "{\"schema\":1,\"transaction_id\":\"0123456789abcdef0123456789abcdef\",\"phase\":\"prepared\",\"old_manifest_exists\":false,\"old_manifest_sha256\":null,\"new_manifest_sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"state_dev\":1,\"state_ino\":1,\"actions\":[{\"record_id\":\"r\",\"root\":\"Applications\",\"relative_path\":\"waddle.app.desktop\",\"before_exists\":false,\"before_sha256\":null,\"before_mode\":0,\"after_exists\":false,\"after_sha256\":null,\"after_mode\":0,\"root_dev\":1,\"root_ino\":1,\"parent_dev\":1,\"parent_ino\":1,\"force_backup\":false}]}";
    try std.testing.expectError(error.InvalidJournal, decode_journal(std.testing.allocator, bad_root, "0123456789abcdef0123456789abcdef.json"));
}

test "target root replacement is detected before publication through the held parent" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer std.testing.allocator.free(initial.old_manifest);
    defer std.testing.allocator.free(initial.target);
    const root = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.data_home, "applications" });
    defer std.testing.allocator.free(root);
    const saved_root = try std.fmt.allocPrint(std.testing.allocator, "{s}.saved", .{root});
    defer std.testing.allocator.free(saved_root);
    const root_fd = try open_absolute_dir(std.testing.allocator, root, false, true);
    defer _ = c.close(root_fd);
    const root_identity = try directory_identity(root_fd);
    const parent = try target_parent(std.testing.allocator, root, "waddle.device.app.desktop");
    defer {
        _ = c.close(parent.fd);
        std.testing.allocator.free(parent.leaf);
    }
    const parent_identity = try directory_identity(parent.fd);
    try std.fs.cwd().rename(root, saved_root);
    try std.fs.cwd().makeDir(root);
    const replacement_target = try std.fs.path.join(std.testing.allocator, &.{ root, "waddle.device.app.desktop" });
    defer std.testing.allocator.free(replacement_target);
    try std.fs.cwd().writeFile(.{ .sub_path = replacement_target, .data = "replacement root data" });
    try std.testing.expectError(error.Conflict, write_target_atomic_checked(
        std.testing.allocator,
        .Applications,
        root,
        "waddle.device.app.desktop",
        "new artifact",
        0o644,
        "0123456789abcdef0123456789abcdef",
        0,
        root_identity,
        parent_identity,
        hash_hex_array("old artifact"),
        0o644,
    ));
    const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, replacement_target, 1024);
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqualStrings("replacement root data", bytes);
}

test "target parent replacement is detected independently of an unchanged root" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const root = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.data_home, "icons" });
    defer std.testing.allocator.free(root);
    const parent_path = try std.fs.path.join(std.testing.allocator, &.{ root, "hicolor", "32x32", "apps" });
    defer std.testing.allocator.free(parent_path);
    const saved_parent = try std.fmt.allocPrint(std.testing.allocator, "{s}.saved", .{parent_path});
    defer std.testing.allocator.free(saved_parent);
    try std.fs.cwd().makePath(parent_path);
    const root_fd = try open_absolute_dir(std.testing.allocator, root, false, true);
    defer _ = c.close(root_fd);
    const root_identity = try directory_identity(root_fd);
    const parent = try target_parent(std.testing.allocator, root, "hicolor/32x32/apps/waddle.icon.png");
    defer {
        _ = c.close(parent.fd);
        std.testing.allocator.free(parent.leaf);
    }
    const parent_identity = try directory_identity(parent.fd);
    try std.fs.cwd().rename(parent_path, saved_parent);
    try std.fs.cwd().makeDir(parent_path);
    try std.testing.expectError(error.Conflict, write_target_atomic_checked(
        std.testing.allocator,
        .Icons,
        root,
        "hicolor/32x32/apps/waddle.icon.png",
        "new icon",
        0o644,
        "0123456789abcdef0123456789abcdef",
        0,
        root_identity,
        parent_identity,
        null,
        0,
    ));
    const target = try std.fs.path.join(std.testing.allocator, &.{ parent_path, "waddle.icon.png" });
    defer std.testing.allocator.free(target);
    try std.testing.expectError(error.FileNotFound, std.fs.cwd().access(target, .{}));
}

test "state directory replacement is detected against the held state descriptor" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const identity = try directory_identity(store.state_fd);
    const saved_state = try std.fmt.allocPrint(std.testing.allocator, "{s}.saved", .{fixture.config.state_dir});
    defer std.testing.allocator.free(saved_state);
    try std.fs.cwd().rename(fixture.config.state_dir, saved_state);
    try std.fs.cwd().makeDir(fixture.config.state_dir);
    try chmod_path(std.testing.allocator, fixture.config.state_dir, 0o700);
    try std.testing.expectError(error.Conflict, store.validate_state_identity(identity));
    try std.fs.cwd().deleteTree(fixture.config.state_dir);
    try std.fs.cwd().rename(saved_state, fixture.config.state_dir);
    try store.validate_state_identity(identity);
}

fn swap_state_path_for_test(allocator: std.mem.Allocator, state_path: []const u8) anyerror!void {
    const saved = try std.fmt.allocPrint(allocator, "{s}.publication-race", .{state_path});
    defer allocator.free(saved);
    try std.fs.cwd().rename(state_path, saved);
    try std.fs.cwd().makeDir(state_path);
    try chmod_path(allocator, state_path, 0o700);
}

test "state publication race remains on held descriptor and detects the swapped name" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer std.testing.allocator.free(initial.old_manifest);
    defer std.testing.allocator.free(initial.target);
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const identity = try directory_identity(store.state_fd);
    const saved_state = try std.fmt.allocPrint(std.testing.allocator, "{s}.publication-race", .{fixture.config.state_dir});
    defer std.testing.allocator.free(saved_state);
    test_state_mutation_hook = swap_state_path_for_test;
    defer test_state_mutation_hook = null;
    try std.testing.expectError(error.Conflict, store.write_state_manifest_atomic(
        "candidate manifest bytes",
        "0123456789abcdef0123456789abcdef",
        MaxActions + 1,
        identity,
    ));
    const replacement_manifest = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "manifest.json" });
    defer std.testing.allocator.free(replacement_manifest);
    try std.testing.expectError(error.FileNotFound, std.fs.cwd().access(replacement_manifest, .{}));
    const saved_manifest = try std.fs.path.join(std.testing.allocator, &.{ saved_state, "manifest.json" });
    defer std.testing.allocator.free(saved_manifest);
    const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, saved_manifest, 1024);
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqualStrings("candidate manifest bytes", bytes);
    try std.fs.cwd().deleteTree(fixture.config.state_dir);
    try std.fs.cwd().rename(saved_state, fixture.config.state_dir);
}

fn swap_state_path_for_cleanup_test(allocator: std.mem.Allocator, state_path: []const u8, temp_name: []const u8) anyerror!void {
    const saved = try std.fmt.allocPrint(allocator, "{s}.cleanup-race", .{state_path});
    defer allocator.free(saved);
    try std.fs.cwd().rename(state_path, saved);
    try std.fs.cwd().makeDir(state_path);
    try chmod_path(allocator, state_path, 0o700);
    const replacement_temp = try std.fs.path.join(allocator, &.{ state_path, temp_name });
    defer allocator.free(replacement_temp);
    try std.fs.cwd().writeFile(.{ .sub_path = replacement_temp, .data = "replacement temp" });
}

test "state temp cleanup remains on held descriptor after named path replacement" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const identity = try directory_identity(store.state_fd);
    const txid = "0123456789abcdef0123456789abcdef";
    const temp_name = try std.fmt.allocPrintZ(std.testing.allocator, ".waddle-{s}-{d}.tmp", .{ txid, MaxActions + 1 });
    defer std.testing.allocator.free(temp_name);
    try write_child(store.state_fd, temp_name, "old temp", 0o600, true);
    const saved_state = try std.fmt.allocPrint(std.testing.allocator, "{s}.cleanup-race", .{fixture.config.state_dir});
    defer std.testing.allocator.free(saved_state);
    test_state_cleanup_hook = swap_state_path_for_cleanup_test;
    defer test_state_cleanup_hook = null;
    try std.testing.expectError(error.Conflict, store.remove_state_temp(txid, MaxActions + 1, identity));
    const replacement_temp = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, temp_name });
    defer std.testing.allocator.free(replacement_temp);
    const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, replacement_temp, 1024);
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqualStrings("replacement temp", bytes);
    try std.fs.cwd().deleteTree(fixture.config.state_dir);
    try std.fs.cwd().rename(saved_state, fixture.config.state_dir);
}

test "menu commits publish both merged menu and desktop directory roots" {
    const roots = [_]artifact_root_t{ .MenuMerged, .DesktopDirectories };
    const relative = [_][]const u8{ "waddle-test.menu", "waddle-test.directory" };
    for (roots, relative) |root_kind, leaf| {
        var fixture = try test_fixture_t.init(std.testing.allocator);
        defer fixture.deinit();
        var store = try store_t.open(std.testing.allocator, fixture.config);
        defer store.close();
        const root = if (root_kind == .MenuMerged)
            try std.fs.path.join(std.testing.allocator, &.{ fixture.config.config_home, "menus", "applications-merged" })
        else
            try std.fs.path.join(std.testing.allocator, &.{ fixture.config.data_home, "desktop-directories" });
        defer std.testing.allocator.free(root);
        const target = try std.fs.path.join(std.testing.allocator, &.{ root, leaf });
        defer std.testing.allocator.free(target);
        const payload = "[Desktop Entry]\nName=Waddle\n";
        const digest = try hash_hex(std.testing.allocator, payload);
        defer std.testing.allocator.free(digest);
        const candidate = try std.fmt.allocPrint(std.testing.allocator,
            "{{\"schema\":1,\"waddle_version\":\"test\",\"updated\":\"2026-10-24T00:00:00Z\",\"records\":[{{\"id\":\"menu-1\",\"kind\":\"menu\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"test\",\"prior\":null}}]}}",
            .{ target, digest });
        defer std.testing.allocator.free(candidate);
        const action = artifact_action_t{
            .record_id = "menu-1",
            .root = root_kind,
            .relative_path = leaf,
            .operation = .Write,
            .expected_current_sha256 = null,
            .bytes = payload,
        };
        try store.commit(.{ .expected_manifest_sha256 = null, .candidate_manifest = candidate, .actions = &.{action} });
        const saved = try std.fs.cwd().readFileAlloc(std.testing.allocator, target, MaxArtifactBytes);
        defer std.testing.allocator.free(saved);
        try std.testing.expectEqualStrings(payload, saved);
    }
}

test "remove commit removes the recorded artifact and publishes its manifest transition" {
    var fixture = try test_fixture_t.init(std.testing.allocator);
    defer fixture.deinit();
    const initial = try fixture.write_initial();
    defer std.testing.allocator.free(initial.old_manifest);
    defer std.testing.allocator.free(initial.target);
    var store = try store_t.open(std.testing.allocator, fixture.config);
    defer store.close();
    const candidate = "{\"schema\":1,\"waddle_version\":\"test\",\"updated\":\"2026-10-24T00:00:00Z\",\"records\":[]}";
    const action = test_action("", hash_raw("old artifact"), .Remove);
    try store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} });
    try std.testing.expectError(error.FileNotFound, std.fs.cwd().access(initial.target, .{}));
    const manifest_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "manifest.json" });
    defer std.testing.allocator.free(manifest_path);
    const saved = try std.fs.cwd().readFileAlloc(std.testing.allocator, manifest_path, manifest.MaxManifestBytes);
    defer std.testing.allocator.free(saved);
    try std.testing.expectEqualStrings(candidate, saved);
}

test "prepared remove crash rolls the artifact back to the recorded generation" {
    var restored = false;
    for (1..80) |point| {
        {
            var fixture = try test_fixture_t.init(std.testing.allocator);
            defer fixture.deinit();
            const initial = try fixture.write_initial();
            defer std.testing.allocator.free(initial.old_manifest);
            defer std.testing.allocator.free(initial.target);
            const candidate = "{\"schema\":1,\"waddle_version\":\"test\",\"updated\":\"2026-10-24T00:00:00Z\",\"records\":[]}";
            const action = test_action("", hash_raw("old artifact"), .Remove);
            test_boundary_count = 0;
            test_crash_at = point;
            const child = c.fork();
            if (child < 0) return io_error();
            if (child == 0) {
                var child_store = store_t.open(std.testing.allocator, fixture.config) catch c._exit(98);
                child_store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} }) catch c._exit(97);
                child_store.close();
                c._exit(0);
            }
            var status: c_int = 0;
            if (c.waitpid(child, &status, 0) != child) return io_error();
            test_crash_at = null;
            test_boundary_count = 0;
            const exit_status = (status >> 8) & 0xff;
            if (exit_status == 99) {
                if (std.fs.cwd().access(initial.target, .{})) |_| {} else |err| switch (err) {
                    error.FileNotFound => {
                        var store = try store_t.open(std.testing.allocator, fixture.config);
                        defer store.close();
                        try store.recover();
                        const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, initial.target, MaxArtifactBytes);
                        defer std.testing.allocator.free(bytes);
                        try std.testing.expectEqualStrings("old artifact", bytes);
                        const manifest_path = try std.fs.path.join(std.testing.allocator, &.{ fixture.config.state_dir, "manifest.json" });
                        defer std.testing.allocator.free(manifest_path);
                        const saved = try std.fs.cwd().readFileAlloc(std.testing.allocator, manifest_path, manifest.MaxManifestBytes);
                        defer std.testing.allocator.free(saved);
                        try std.testing.expectEqualSlices(u8, initial.old_manifest, saved);
                        restored = true;
                    },
                    else => return err,
                }
            } else try std.testing.expectEqual(@as(c_int, 0), exit_status);
        }
        if (restored) break;
    }
    test_crash_at = null;
    try std.testing.expect(restored);
}

test "committed recovery preserves a journal and reports externally changed artifact" {
    var found_committed_journal = false;
    for (1..90) |point| {
        {
            var fixture = try test_fixture_t.init(std.testing.allocator);
            defer fixture.deinit();
            const initial = try fixture.write_initial();
            defer std.testing.allocator.free(initial.old_manifest);
            defer std.testing.allocator.free(initial.target);
            const candidate = try fixture.manifest_bytes("new artifact");
            defer std.testing.allocator.free(candidate);
            const action = test_action("new artifact", hash_raw("old artifact"), .Write);
            test_boundary_count = 0;
            test_crash_at = point;
            const child = c.fork();
            if (child < 0) return io_error();
            if (child == 0) {
                var child_store = store_t.open(std.testing.allocator, fixture.config) catch c._exit(98);
                child_store.commit(.{ .expected_manifest_sha256 = initial.old_hash, .candidate_manifest = candidate, .actions = &.{action} }) catch c._exit(97);
                child_store.close();
                c._exit(0);
            }
            var status: c_int = 0;
            if (c.waitpid(child, &status, 0) != child) return io_error();
            test_crash_at = null;
            test_boundary_count = 0;
            const exit_status = (status >> 8) & 0xff;
            if (exit_status == 99) {
                var inspector = try store_t.open(std.testing.allocator, fixture.config);
                const journal_names = try list_journals(std.testing.allocator, inspector.transactions_fd);
                var has_committed = false;
                for (journal_names) |journal_name| {
                    const raw = try read_child(std.testing.allocator, inspector.transactions_fd, journal_name, MaxJournalBytes, true);
                    defer std.testing.allocator.free(raw);
                    var parsed = try decode_journal(std.testing.allocator, raw, journal_name);
                    defer parsed.deinit();
                    if (parsed.value.phase == .Committed) has_committed = true;
                }
                for (journal_names) |journal_name| std.testing.allocator.free(journal_name);
                std.testing.allocator.free(journal_names);
                inspector.close();
                if (has_committed) {
                    try std.fs.cwd().writeFile(.{ .sub_path = initial.target, .data = "external committed edit" });
                    var store = try store_t.open(std.testing.allocator, fixture.config);
                    defer store.close();
                    try std.testing.expectError(error.Conflict, store.recover());
                    const bytes = try std.fs.cwd().readFileAlloc(std.testing.allocator, initial.target, MaxArtifactBytes);
                    defer std.testing.allocator.free(bytes);
                    try std.testing.expectEqualStrings("external committed edit", bytes);
                    found_committed_journal = true;
                }
            } else try std.testing.expectEqual(@as(c_int, 0), exit_status);
        }
        if (found_committed_journal) break;
    }
    test_crash_at = null;
    try std.testing.expect(found_committed_journal);
}


fn io_error() anyerror {
    return switch (c.__errno_location().*) {
        c.ENOENT => error.FileNotFound,
        c.EEXIST => error.PathAlreadyExists,
        c.EACCES, c.EPERM, c.ELOOP, c.ENOTDIR => error.AccessDenied,
        c.ENOSPC => error.NoSpaceLeft,
        c.EINTR => error.Interrupted,
        c.EAGAIN => error.Busy,
        else => error.IoFailure,
    };
}

fn chmod_path(allocator: std.mem.Allocator, path: []const u8, mode: u32) !void {
    const terminated = try allocator.dupeZ(u8, path);
    defer allocator.free(terminated);
    if (c.chmod(terminated, @intCast(mode)) != 0) return io_error();
}

fn verify_regular(fd: c_int, private: bool, one_link: bool) !void {
    var stat: c.struct_stat = undefined;
    if (c.fstat(fd, &stat) != 0) return io_error();
    if (stat.st_mode & c.S_IFMT != c.S_IFREG or stat.st_uid != c.geteuid() or
        (private and stat.st_mode & 0o077 != 0) or (one_link and stat.st_nlink != 1)) return error.AccessDenied;
}

fn verify_directory(fd: c_int, private: bool) !void {
    var stat: c.struct_stat = undefined;
    if (c.fstat(fd, &stat) != 0) return io_error();
    if (stat.st_mode & c.S_IFMT != c.S_IFDIR or stat.st_uid != c.geteuid() or (private and stat.st_mode & 0o077 != 0)) return error.AccessDenied;
}

fn open_absolute_dir(allocator: std.mem.Allocator, path: []const u8, private: bool, must_exist: bool) !c_int {
    if (path.len < 2 or path[0] != '/' or std.mem.indexOfScalar(u8, path, 0) != null) return error.InvalidPath;
    var fd = c.open("/", c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0));
    if (fd < 0) return io_error();
    errdefer _ = c.close(fd);
    var parts = std.mem.splitScalar(u8, path[1..], '/');
    while (parts.next()) |part| {
        if (part.len == 0 or std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return error.InvalidPath;
        const name = try allocator.dupeZ(u8, part);
        defer allocator.free(name);
        var next = c.openat(fd, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0));
        if (next < 0 and !must_exist and c.__errno_location().* == c.ENOENT) {
            if (c.mkdirat(fd, name, if (private) @as(c_uint, 0o700) else @as(c_uint, 0o755)) != 0 and c.__errno_location().* != c.EEXIST) return io_error();
            next = c.openat(fd, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0));
        }
        if (next < 0) return io_error();
        _ = c.close(fd);
        fd = next;
    }
    try verify_directory(fd, private);
    return fd;
}

fn open_or_create_child(parent: c_int, name: [:0]const u8, private: bool) !c_int {
    if (c.mkdirat(parent, name, if (private) @as(c_uint, 0o700) else @as(c_uint, 0o755)) != 0 and c.__errno_location().* != c.EEXIST) return io_error();
    const fd = c.openat(parent, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0));
    if (fd < 0) return io_error();
    errdefer _ = c.close(fd);
    try verify_directory(fd, private);
    return fd;
}

fn open_child(parent: c_int, name: [:0]const u8) !c_int {
    const fd = c.openat(parent, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0));
    if (fd < 0) return io_error();
    errdefer _ = c.close(fd);
    try verify_directory(fd, true);
    return fd;
}

fn fsync_fd(fd: c_int) !void {
    if (c.fsync(fd) != 0) return io_error();
    try durable_boundary();
}

fn read_child(allocator: std.mem.Allocator, parent: c_int, name: []const u8, limit: usize, private: bool) ![]u8 {
    const leaf = try allocator.dupeZ(u8, name);
    defer allocator.free(leaf);
    const fd = c.openat(parent, leaf, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK, @as(c_uint, 0));
    if (fd < 0) return io_error();
    defer _ = c.close(fd);
    try verify_regular(fd, private, private);
    var file = std.fs.File{ .handle = fd };
    const bytes = try file.readToEndAlloc(allocator, limit);
    if (bytes.len > limit) {
        allocator.free(bytes);
        return error.FileTooLarge;
    }
    return bytes;
}

fn write_child(parent: c_int, name: [:0]const u8, bytes: []const u8, mode: u32, exclusive: bool) !void {
    const flags = c.O_WRONLY | c.O_CREAT | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK | (if (exclusive) c.O_EXCL else c.O_TRUNC);
    const fd = c.openat(parent, name, flags, @as(c_uint, @intCast(mode)));
    if (fd < 0) return io_error();
    defer _ = c.close(fd);
    var file = std.fs.File{ .handle = fd };
    try file.writeAll(bytes);
    try durable_boundary();
    if (c.fchmod(fd, @intCast(mode)) != 0) return io_error();
    try durable_boundary();
    try fsync_fd(fd);
    try fsync_fd(parent);
}

fn rename_child(from_dir: c_int, from: [:0]const u8, to_dir: c_int, to: [:0]const u8, require_absent: bool) !void {
    var st: c.struct_stat = undefined;
    if (c.fstatat(to_dir, to, &st, c.AT_SYMLINK_NOFOLLOW) == 0) {
        if (require_absent) return error.PathAlreadyExists;
        if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.geteuid() or st.st_mode & 0o077 != 0 or st.st_nlink != 1) return error.AccessDenied;
    } else if (c.__errno_location().* != c.ENOENT) return io_error();
    if (c.renameat(from_dir, from, to_dir, to) != 0) return io_error();
    try durable_boundary();
    try fsync_fd(from_dir);
    if (to_dir != from_dir) try fsync_fd(to_dir);
}

fn unlink_child_if_present(parent: c_int, name: [:0]const u8) !void {
    var st: c.struct_stat = undefined;
    if (c.fstatat(parent, name, &st, c.AT_SYMLINK_NOFOLLOW) != 0) {
        if (c.__errno_location().* == c.ENOENT) return;
        return io_error();
    }
    if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.geteuid()) return error.AccessDenied;
    if (c.unlinkat(parent, name, 0) != 0) return io_error();
    try durable_boundary();
    try fsync_fd(parent);
}

fn delete_tree_child(parent: c_int, name: [:0]const u8) !void {
    const fd = c.openat(parent, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0));
    if (fd < 0) {
        if (c.__errno_location().* == c.ENOENT) return;
        return io_error();
    }
    defer _ = c.close(fd);
    try verify_directory(fd, true);
    var dir = std.fs.Dir{ .fd = c.dup(fd) };
    defer dir.close();
    var iter = dir.iterate();
    while (try iter.next()) |entry| {
        if (entry.kind != .file) return error.RecoveryRequired;
        const leaf = try std.heap.page_allocator.dupeZ(u8, entry.name);
        defer std.heap.page_allocator.free(leaf);
        try unlink_child_if_present(fd, leaf);
    }
    if (c.unlinkat(parent, name, c.AT_REMOVEDIR) != 0) return io_error();
    try fsync_fd(parent);
}

fn list_journals(allocator: std.mem.Allocator, transactions_fd: c_int) ![][]u8 {
    var dir = std.fs.Dir{ .fd = c.dup(transactions_fd) };
    defer dir.close();
    var names = std.ArrayList([]u8).init(allocator);
    errdefer {
        for (names.items) |name| allocator.free(name);
        names.deinit();
    }
    var iter = dir.iterate();
    while (try iter.next()) |entry| {
        if (std.mem.endsWith(u8, entry.name, ".json")) {
            if (entry.kind != .file) return error.RecoveryRequired;
            if (names.items.len >= MaxJournals) return error.RecoveryRequired;
            const name = try allocator.dupe(u8, entry.name);
            names.append(name) catch |err| {
                allocator.free(name);
                return err;
            };
        }
    }
    return names.toOwnedSlice();
}

const directory_entry_t = struct { name: []u8, kind: std.fs.File.Kind };

fn directory_has_file(entries: []const directory_entry_t, name: []const u8) bool {
    for (entries) |entry| if (entry.kind == .file and std.mem.eql(u8, entry.name, name)) return true;
    return false;
}

fn list_directory(allocator: std.mem.Allocator, fd: c_int) ![]directory_entry_t {
    var dir = std.fs.Dir{ .fd = c.dup(fd) };
    defer dir.close();
    var entries = std.ArrayList(directory_entry_t).init(allocator);
    errdefer {
        for (entries.items) |entry| allocator.free(entry.name);
        entries.deinit();
    }
    var iter = dir.iterate();
    while (try iter.next()) |entry| {
        if (entries.items.len >= MaxDirectoryEntries) return error.RecoveryRequired;
        const name = try allocator.dupe(u8, entry.name);
        entries.append(.{ .name = name, .kind = entry.kind }) catch |err| {
            allocator.free(name);
            return err;
        };
    }
    return entries.toOwnedSlice();
}

fn decode_journal(allocator: std.mem.Allocator, bytes: []const u8, filename: []const u8) !parsed_journal_t {
    if (bytes.len == 0 or bytes.len > MaxJournalBytes or !std.unicode.utf8ValidateSlice(bytes) or std.mem.indexOfScalar(u8, bytes, 0) != null) return error.InvalidJournal;
    var arena = std.heap.ArenaAllocator.init(allocator);
    errdefer arena.deinit();
    const parsed = try std.json.parseFromSlice(journal_wire_t, arena.allocator(), bytes, .{ .allocate = .alloc_always, .ignore_unknown_fields = false, .duplicate_field_behavior = .@"error" });
    const journal = try journal_from_wire(arena.allocator(), parsed.value);
    if (journal.schema != 1 or journal.actions.len > MaxActions or !valid_txid(journal.transaction_id)) return error.InvalidJournal;
    if (journal.state_identity.dev == 0 or journal.state_identity.ino == 0) return error.InvalidJournal;
    if (!valid_hash_array(journal.new_manifest_sha256) or (journal.old_manifest_sha256 != null and !valid_hash_array(journal.old_manifest_sha256.?))) return error.InvalidJournal;
    const expected_name = try std.fmt.allocPrint(arena.allocator(), "{s}.json", .{journal.transaction_id});
    if (!std.mem.eql(u8, filename, expected_name)) return error.InvalidJournal;
    for (journal.actions, 0..) |entry, i| {
        try validate_relative(entry.relative_path);
        try validate_root_relative(entry.root, entry.relative_path);
        if (entry.record_id.len == 0 or entry.record_id.len > manifest.MaxManifestStringBytes or entry.before_exists != (entry.before_sha256 != null) or entry.after_exists != (entry.after_sha256 != null) or (entry.before_exists and entry.before_mode != 0o644 and entry.before_mode != 0o755) or (entry.after_exists and entry.after_mode != 0o644 and entry.after_mode != 0o755)) return error.InvalidJournal;
        if (entry.root_identity.dev == 0 or entry.root_identity.ino == 0 or entry.parent_identity.dev == 0 or entry.parent_identity.ino == 0) return error.InvalidJournal;
        if ((entry.before_sha256 != null and !valid_hash_array(entry.before_sha256.?)) or (entry.after_sha256 != null and !valid_hash_array(entry.after_sha256.?))) return error.InvalidJournal;
        for (journal.actions[0..i]) |prior| {
            if (std.mem.eql(u8, prior.record_id, entry.record_id) or (prior.root == entry.root and std.mem.eql(u8, prior.relative_path, entry.relative_path))) return error.InvalidJournal;
        }
    }
    return .{ .arena = arena, .value = journal };
}

fn encode_journal(allocator: std.mem.Allocator, journal: journal_t) ![]u8 {
    const actions = try allocator.alloc(journal_wire_action_t, journal.actions.len);
    defer allocator.free(actions);
    for (journal.actions, 0..) |entry, index| actions[index] = .{
        .record_id = entry.record_id,
        .root = root_wire_name(entry.root),
        .relative_path = entry.relative_path,
        .before_exists = entry.before_exists,
        .before_sha256 = entry.before_sha256,
        .before_mode = entry.before_mode,
        .after_exists = entry.after_exists,
        .after_sha256 = entry.after_sha256,
        .after_mode = entry.after_mode,
        .force_backup = entry.force_backup,
        .root_dev = entry.root_identity.dev,
        .root_ino = entry.root_identity.ino,
        .parent_dev = entry.parent_identity.dev,
        .parent_ino = entry.parent_identity.ino,
    };
    const wire = journal_wire_t{
        .schema = journal.schema,
        .transaction_id = journal.transaction_id,
        .phase = if (journal.phase == .Prepared) "prepared" else "committed",
        .old_manifest_exists = journal.old_manifest_exists,
        .old_manifest_sha256 = journal.old_manifest_sha256,
        .new_manifest_sha256 = journal.new_manifest_sha256,
        .actions = actions,
        .state_dev = journal.state_identity.dev,
        .state_ino = journal.state_identity.ino,
    };
    return std.json.stringifyAlloc(allocator, wire, .{});
}

fn journal_from_wire(allocator: std.mem.Allocator, wire: journal_wire_t) !journal_t {
    const phase: journal_phase_t = if (std.mem.eql(u8, wire.phase, "prepared")) .Prepared else if (std.mem.eql(u8, wire.phase, "committed")) .Committed else return error.InvalidJournal;
    const actions = try allocator.alloc(journal_action_t, wire.actions.len);
    for (wire.actions, 0..) |entry, index| actions[index] = .{
        .record_id = entry.record_id,
        .root = try root_from_wire_name(entry.root),
        .relative_path = entry.relative_path,
        .before_exists = entry.before_exists,
        .before_sha256 = entry.before_sha256,
        .before_mode = entry.before_mode,
        .after_exists = entry.after_exists,
        .after_sha256 = entry.after_sha256,
        .after_mode = entry.after_mode,
        .force_backup = entry.force_backup,
        .root_identity = .{ .dev = entry.root_dev, .ino = entry.root_ino },
        .parent_identity = .{ .dev = entry.parent_dev, .ino = entry.parent_ino },
    };
    return .{
        .schema = wire.schema,
        .transaction_id = wire.transaction_id,
        .phase = phase,
        .old_manifest_exists = wire.old_manifest_exists,
        .old_manifest_sha256 = wire.old_manifest_sha256,
        .new_manifest_sha256 = wire.new_manifest_sha256,
        .actions = actions,
        .state_identity = .{ .dev = wire.state_dev, .ino = wire.state_ino },
    };
}

fn root_wire_name(root: artifact_root_t) []const u8 {
    return switch (root) {
        .Applications => "applications",
        .Icons => "icons",
        .MenuMerged => "menu_merged",
        .DesktopDirectories => "desktop_directories",
        .Autostart => "autostart",
        .Desktop => "desktop",
    };
}

fn root_from_wire_name(name: []const u8) !artifact_root_t {
    if (std.mem.eql(u8, name, "applications")) return .Applications;
    if (std.mem.eql(u8, name, "icons")) return .Icons;
    if (std.mem.eql(u8, name, "menu_merged")) return .MenuMerged;
    if (std.mem.eql(u8, name, "desktop_directories")) return .DesktopDirectories;
    if (std.mem.eql(u8, name, "autostart")) return .Autostart;
    if (std.mem.eql(u8, name, "desktop")) return .Desktop;
    return error.InvalidJournal;
}

fn valid_txid(value: []const u8) bool {
    if (value.len != 32) return false;
    for (value) |byte| if (!std.ascii.isDigit(byte) and (byte < 'a' or byte > 'f')) return false;
    return true;
}

fn valid_hash_array(value: [64]u8) bool {
    for (value) |byte| if (!std.ascii.isHex(byte)) return false;
    return true;
}

fn valid_hash_text(value: []const u8) bool {
    if (value.len != 64) return false;
    for (value) |byte| if (!std.ascii.isHex(byte)) return false;
    return true;
}

fn hash_text_equal(left: []const u8, right: []const u8) bool {
    if (left.len != right.len) return false;
    for (left, right) |a, b| if (std.ascii.toLower(a) != std.ascii.toLower(b)) return false;
    return true;
}

fn validate_action_shape(action: artifact_action_t) !void {
    try validate_relative(action.relative_path);
    try validate_root_relative(action.root, action.relative_path);
    if (action.record_id.len == 0 or action.record_id.len > manifest.MaxManifestStringBytes) return error.InvalidAction;
    if (action.mode != 0o644 and action.mode != 0o755) return error.InvalidAction;
    if ((action.operation == .Write) != (action.bytes != null)) return error.InvalidAction;
    if (action.bytes) |bytes| if (bytes.len > MaxArtifactBytes) return error.FileTooLarge;
}

fn validate_relative(path: []const u8) !void {
    if (path.len == 0 or path.len >= 1024 or path[0] == '/' or std.mem.indexOfScalar(u8, path, 0) != null) return error.InvalidPath;
    var parts = std.mem.splitScalar(u8, path, '/');
    while (parts.next()) |part| if (part.len == 0 or part.len > 255 or std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return error.InvalidPath;
}

fn path_relative_to_root(path: []const u8, root: []const u8) ?[]const u8 {
    if (path.len <= root.len or !std.mem.startsWith(u8, path, root) or path[root.len] != '/') return null;
    return path[root.len + 1 ..];
}

fn basename_is_waddle(path: []const u8, extension: []const u8) bool {
    const base = std.fs.path.basename(path);
    return std.mem.startsWith(u8, base, "waddle.") and std.mem.endsWith(u8, base, extension) and base.len > 7 + extension.len;
}

fn validate_root_relative(root: artifact_root_t, path: []const u8) !void {
    switch (root) {
        .Applications => if (std.mem.indexOfScalar(u8, path, '/') != null or !basename_is_waddle(path, ".desktop")) return error.UnsupportedPolicy,
        .Icons => {
            var parts = std.mem.splitScalar(u8, path, '/');
            const tree = parts.next() orelse return error.InvalidPath;
            const size = parts.next() orelse return error.InvalidPath;
            const apps = parts.next() orelse return error.InvalidPath;
            const file = parts.next() orelse return error.InvalidPath;
            if (parts.next() != null or !std.mem.eql(u8, tree, "hicolor") or !std.mem.eql(u8, apps, "apps") or !basename_is_waddle(file, ".png")) return error.UnsupportedPolicy;
            const x = std.mem.indexOfScalar(u8, size, 'x') orelse return error.UnsupportedPolicy;
            if (x == 0 or x + 1 == size.len or !std.mem.eql(u8, size[0..x], size[x + 1 ..])) return error.UnsupportedPolicy;
            const dim = std.fmt.parseInt(u16, size[0..x], 10) catch return error.UnsupportedPolicy;
            if (dim != 16 and dim != 24 and dim != 32 and dim != 48 and dim != 64 and dim != 128 and dim != 256) return error.UnsupportedPolicy;
        },
        .MenuMerged => if (std.mem.indexOfScalar(u8, path, '/') != null or !std.mem.startsWith(u8, path, "waddle-") or !std.mem.endsWith(u8, path, ".menu")) return error.UnsupportedPolicy,
        .DesktopDirectories => if (std.mem.indexOfScalar(u8, path, '/') != null or !std.mem.startsWith(u8, path, "waddle-") or !std.mem.endsWith(u8, path, ".directory")) return error.UnsupportedPolicy,
        .Autostart => if (std.mem.indexOfScalar(u8, path, '/') != null or !basename_is_waddle(path, ".desktop")) return error.UnsupportedPolicy,
        .Desktop => if (std.mem.indexOfScalar(u8, path, '/') != null or !basename_is_waddle(path, ".desktop")) return error.UnsupportedPolicy,
    }
}

fn kind_matches(kind: manifest.record_kind_t, root: artifact_root_t) bool {
    return switch (root) {
        .Applications => kind == .DesktopEntry or kind == .HandlerEntry,
        .Icons => kind == .Icon,
        .MenuMerged, .DesktopDirectories => kind == .Menu,
        .Autostart => kind == .Autostart,
        .Desktop => kind == .DesktopShortcut,
    };
}

fn find_record(records: []const manifest.record_t, id: []const u8) ?manifest.record_t {
    for (records) |record| if (std.mem.eql(u8, record.id, id)) return record;
    return null;
}

fn find_action(actions: []const artifact_action_t, id: []const u8) ?artifact_action_t {
    for (actions) |action| if (std.mem.eql(u8, action.record_id, id)) return action;
    return null;
}

fn hash_hex(allocator: std.mem.Allocator, bytes: []const u8) ![]u8 {
    return hex_encode(allocator, &hash_raw(bytes));
}

fn hash_raw(bytes: []const u8) [32]u8 {
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
    return digest;
}

fn hash_hex_array(bytes: []const u8) [64]u8 {
    const raw = hash_raw(bytes);
    var out: [64]u8 = undefined;
    for (raw, 0..) |byte, i| {
        out[i * 2] = "0123456789abcdef"[byte >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[byte & 0xf];
    }
    return out;
}

fn hex_encode(allocator: std.mem.Allocator, bytes: []const u8) ![]u8 {
    const out = try allocator.alloc(u8, bytes.len * 2);
    for (bytes, 0..) |byte, i| {
        out[i * 2] = "0123456789abcdef"[byte >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[byte & 0xf];
    }
    return out;
}

fn hex_to_array(value: []const u8) ![64]u8 {
    if (value.len != 64) return error.InvalidHash;
    var out: [64]u8 = undefined;
    @memcpy(&out, value);
    return out;
}

fn target_parent(allocator: std.mem.Allocator, root: []const u8, relative: []const u8) !struct { fd: c_int, leaf: [:0]u8 } {
    try validate_relative(relative);
    var fd = try open_absolute_dir(allocator, root, false, true);
    errdefer _ = c.close(fd);
    var parts = std.mem.splitScalar(u8, relative, '/');
    var leaf: []const u8 = undefined;
    while (parts.next()) |part| {
        leaf = part;
        if (parts.peek() != null) {
            const name = try allocator.dupeZ(u8, part);
            defer allocator.free(name);
            const next = c.openat(fd, name, c.O_RDONLY | c.O_DIRECTORY | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0));
            if (next < 0) return io_error();
            _ = c.close(fd);
            fd = next;
            try verify_directory(fd, false);
        }
    }
    return .{ .fd = fd, .leaf = try allocator.dupeZ(u8, leaf) };
}

fn directory_identity(fd: c_int) !directory_identity_t {
    var stat: c.struct_stat = undefined;
    if (c.fstat(fd, &stat) != 0) return io_error();
    if (stat.st_mode & c.S_IFMT != c.S_IFDIR) return error.AccessDenied;
    return .{ .dev = @intCast(stat.st_dev), .ino = @intCast(stat.st_ino) };
}

fn identity_equal(left: directory_identity_t, right: directory_identity_t) bool {
    return left.dev == right.dev and left.ino == right.ino;
}

fn read_target(allocator: std.mem.Allocator, root_kind: artifact_root_t, root: []const u8, relative: []const u8, limit: usize, private_file: bool) !?[]u8 {
    try validate_root_relative(root_kind, relative);
    const target = try target_parent(allocator, root, relative);
    defer {
        _ = c.close(target.fd);
        allocator.free(target.leaf);
    }
    const fd = c.openat(target.fd, target.leaf, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK, @as(c_uint, 0));
    if (fd < 0) {
        if (c.__errno_location().* == c.ENOENT) return null;
        return io_error();
    }
    defer _ = c.close(fd);
    try verify_regular(fd, private_file, false);
    var file = std.fs.File{ .handle = fd };
    const bytes = try file.readToEndAlloc(allocator, limit);
    if (bytes.len > limit) {
        allocator.free(bytes);
        return error.FileTooLarge;
    }
    return bytes;
}

fn target_mode(allocator: std.mem.Allocator, root_kind: artifact_root_t, root: []const u8, relative: []const u8) !u32 {
    try validate_root_relative(root_kind, relative);
    const target = try target_parent(allocator, root, relative);
    defer {
        _ = c.close(target.fd);
        allocator.free(target.leaf);
    }
    var stat: c.struct_stat = undefined;
    if (c.fstatat(target.fd, target.leaf, &stat, c.AT_SYMLINK_NOFOLLOW) != 0) {
        if (c.__errno_location().* == c.ENOENT) return 0;
        return io_error();
    }
    if (stat.st_mode & c.S_IFMT != c.S_IFREG or stat.st_uid != c.geteuid()) return error.AccessDenied;
    return @intCast(stat.st_mode & 0o777);
}

fn write_target_atomic(allocator: std.mem.Allocator, root: []const u8, relative: []const u8, bytes: []const u8, mode: u32, txid: []const u8, index: usize) !void {
    const target = try target_parent(allocator, root, relative);
    defer {
        _ = c.close(target.fd);
        allocator.free(target.leaf);
    }
    const temp = try std.fmt.allocPrintZ(allocator, ".waddle-{s}-{d}.tmp", .{ txid, index });
    defer allocator.free(temp);
    try write_child(target.fd, temp, bytes, mode, true);
    var st: c.struct_stat = undefined;
    if (c.fstatat(target.fd, target.leaf, &st, c.AT_SYMLINK_NOFOLLOW) == 0) {
        if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.geteuid()) return error.AccessDenied;
    } else if (c.__errno_location().* != c.ENOENT) return io_error();
    if (c.renameat(target.fd, temp, target.fd, target.leaf) != 0) return io_error();
    try durable_boundary();
    try fsync_fd(target.fd);
}

/// Recheck the expected target through the same held parent descriptor used
/// for publication, so an intervening path replacement cannot redirect it.
fn write_target_atomic_checked(allocator: std.mem.Allocator, root_kind: artifact_root_t, root: []const u8, relative: []const u8, bytes: []const u8, mode: u32, txid: []const u8, index: usize, expected_root: directory_identity_t, expected_parent: directory_identity_t, expected_hash: ?[64]u8, expected_mode: u32) !void {
    try validate_root_relative(root_kind, relative);
    const parent = try target_parent(allocator, root, relative);
    defer {
        _ = c.close(parent.fd);
        allocator.free(parent.leaf);
    }
    const root_fd = try open_absolute_dir(allocator, root, false, true);
    defer _ = c.close(root_fd);
    if (!identity_equal(try directory_identity(root_fd), expected_root) or
        !identity_equal(try directory_identity(parent.fd), expected_parent)) return error.Conflict;
    var stat: c.struct_stat = undefined;
    if (c.fstatat(parent.fd, parent.leaf, &stat, c.AT_SYMLINK_NOFOLLOW) == 0) {
        if (stat.st_mode & c.S_IFMT != c.S_IFREG or stat.st_uid != c.geteuid()) return error.AccessDenied;
        if (expected_hash == null or (stat.st_mode & 0o777) != expected_mode) return error.Conflict;
        const current_fd = c.openat(parent.fd, parent.leaf, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK, @as(c_uint, 0));
        if (current_fd < 0) return io_error();
        defer _ = c.close(current_fd);
        var file = std.fs.File{ .handle = current_fd };
        const current = try file.readToEndAlloc(allocator, MaxArtifactBytes);
        defer allocator.free(current);
        if (!std.mem.eql(u8, &hash_hex_array(current), &expected_hash.?)) return error.Conflict;
    } else if (c.__errno_location().* == c.ENOENT) {
        if (expected_hash != null) return error.Conflict;
    } else return io_error();
    const temp = try std.fmt.allocPrintZ(allocator, ".waddle-{s}-{d}.tmp", .{ txid, index });
    defer allocator.free(temp);
    try write_child(parent.fd, temp, bytes, mode, true);
    if (c.renameat(parent.fd, temp, parent.fd, parent.leaf) != 0) return io_error();
    try durable_boundary();
    try fsync_fd(parent.fd);
}

fn unlink_target(allocator: std.mem.Allocator, root: []const u8, relative: []const u8) !void {
    const target = try target_parent(allocator, root, relative);
    defer {
        _ = c.close(target.fd);
        allocator.free(target.leaf);
    }
    var st: c.struct_stat = undefined;
    if (c.fstatat(target.fd, target.leaf, &st, c.AT_SYMLINK_NOFOLLOW) != 0) {
        if (c.__errno_location().* == c.ENOENT) return;
        return io_error();
    }
    if (st.st_mode & c.S_IFMT != c.S_IFREG or st.st_uid != c.geteuid()) return error.AccessDenied;
    if (c.unlinkat(target.fd, target.leaf, 0) != 0) return io_error();
    try durable_boundary();
    try fsync_fd(target.fd);
}

fn unlink_target_checked(allocator: std.mem.Allocator, root_kind: artifact_root_t, root: []const u8, relative: []const u8, expected_root: directory_identity_t, expected_parent: directory_identity_t, expected_hash: [64]u8, expected_mode: u32) !void {
    try validate_root_relative(root_kind, relative);
    const parent = try target_parent(allocator, root, relative);
    defer {
        _ = c.close(parent.fd);
        allocator.free(parent.leaf);
    }
    const root_fd = try open_absolute_dir(allocator, root, false, true);
    defer _ = c.close(root_fd);
    if (!identity_equal(try directory_identity(root_fd), expected_root) or
        !identity_equal(try directory_identity(parent.fd), expected_parent)) return error.Conflict;
    var stat: c.struct_stat = undefined;
    if (c.fstatat(parent.fd, parent.leaf, &stat, c.AT_SYMLINK_NOFOLLOW) != 0) {
        if (c.__errno_location().* == c.ENOENT) return error.Conflict;
        return io_error();
    }
    if (stat.st_mode & c.S_IFMT != c.S_IFREG or stat.st_uid != c.geteuid() or (stat.st_mode & 0o777) != expected_mode) return error.AccessDenied;
    const fd = c.openat(parent.fd, parent.leaf, c.O_RDONLY | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK, @as(c_uint, 0));
    if (fd < 0) return io_error();
    defer _ = c.close(fd);
    var file = std.fs.File{ .handle = fd };
    const bytes = try file.readToEndAlloc(allocator, MaxArtifactBytes);
    defer allocator.free(bytes);
    if (!std.mem.eql(u8, &hash_hex_array(bytes), &expected_hash)) return error.Conflict;
    if (c.unlinkat(parent.fd, parent.leaf, 0) != 0) return io_error();
    try durable_boundary();
    try fsync_fd(parent.fd);
}

fn remove_sibling_temp(allocator: std.mem.Allocator, root: []const u8, relative: []const u8, txid: []const u8, index: usize) !void {
    const target = try target_parent(allocator, root, relative);
    defer {
        _ = c.close(target.fd);
        allocator.free(target.leaf);
    }
    const temp = try std.fmt.allocPrintZ(allocator, ".waddle-{s}-{d}.tmp", .{ txid, index });
    defer allocator.free(temp);
    try unlink_child_if_present(target.fd, temp);
}

/// Supported user artifact root. MIME, guest, binfmt and polkit actions are rejected.
/// @ownership Enum value is copied by value.
pub const artifact_root_t = enum { Applications, Icons, MenuMerged, DesktopDirectories, Autostart, Desktop };
/// Supported file operation; mutations are journaled with their manifest.
/// @ownership Enum value is copied by value.
pub const artifact_operation_t = enum { Write, Remove };

/// One caller-authorized artifact operation.
/// @ownership String and byte slices are borrowed through `commit`; the store
/// copies transaction data before returning.
/// @errors InvalidAction, InvalidPath, Conflict, UnsupportedPolicy, or I/O errors.
/// @thread_safety Use only under the owning locked `store_t` call sequence.
pub const artifact_action_t = struct {
    /// @in Stable manifest record identifier.
    record_id: []const u8,
    /// @in Explicit allowlisted root.
    root: artifact_root_t,
    /// Root-relative target, e.g. `waddle.device.app.desktop` or
    /// `hicolor/64x64/apps/waddle.device.app.png` for the `icons` root.
    relative_path: []const u8,
    /// @in Operation to apply to the exact target.
    operation: artifact_operation_t,
    /// Hash recorded by the prior manifest, or null if the path must be absent.
    expected_current_sha256: ?[32]u8,
    /// Required for writes; borrowed through the call only.
    bytes: ?[]const u8,
    /// Mode to publish, limited to 0644 or 0755.
    mode: u32 = 0o644,
    /// Caller authorization to replace/remove a modified *recorded* artifact.
    force: bool = false,
};

/// Caller-borrowed transaction request.
/// @ownership Candidate and action data are borrowed until `commit` returns.
/// @errors Conflict, InvalidAction, InvalidPath, UnsupportedPolicy, or I/O errors.
/// @thread_safety Commit requires the locked store and is not concurrently callable.
pub const commit_request_t = struct {
    /// Null means `manifest.json` must not exist.
    expected_manifest_sha256: ?[32]u8,
    /// @in Complete schema-1 candidate bytes; borrowed through `commit`.
    candidate_manifest: []const u8,
    /// @in Explicit allowlisted operations corresponding to candidate records.
    actions: []const artifact_action_t,
};

const journal_phase_t = enum { Prepared, Committed };
const directory_identity_t = struct { dev: u64, ino: u64 };
const journal_action_t = struct {
    record_id: []const u8,
    root: artifact_root_t,
    relative_path: []const u8,
    before_exists: bool,
    before_sha256: ?[64]u8,
    before_mode: u32,
    after_exists: bool,
    after_sha256: ?[64]u8,
    after_mode: u32,
    force_backup: bool,
    root_identity: directory_identity_t,
    parent_identity: directory_identity_t,
};
const journal_t = struct {
    schema: u32,
    transaction_id: []const u8,
    phase: journal_phase_t,
    old_manifest_exists: bool,
    old_manifest_sha256: ?[64]u8,
    new_manifest_sha256: [64]u8,
    actions: []const journal_action_t,
    state_identity: directory_identity_t,
};
const retained_backup_metadata_t = struct {
    schema: u32,
    sha256: []const u8,
    mode: u32,
};

// Journal wire names are frozen lowercase strings; runtime enum names follow
// the repository's PascalCase constant convention.
const journal_wire_action_t = struct {
    record_id: []const u8,
    root: []const u8,
    relative_path: []const u8,
    before_exists: bool,
    before_sha256: ?[64]u8,
    before_mode: u32,
    after_exists: bool,
    after_sha256: ?[64]u8,
    after_mode: u32,
    force_backup: bool,
    root_dev: u64,
    root_ino: u64,
    parent_dev: u64,
    parent_ino: u64,
};
const journal_wire_t = struct {
    schema: u32,
    transaction_id: []const u8,
    phase: []const u8,
    old_manifest_exists: bool,
    old_manifest_sha256: ?[64]u8,
    new_manifest_sha256: [64]u8,
    actions: []const journal_wire_action_t,
    state_dev: u64,
    state_ino: u64,
};
var test_crash_at: ?usize = null;
var test_fail_at: ?usize = null;
var test_boundary_count: usize = 0;
var test_state_mutation_hook: ?*const fn (std.mem.Allocator, []const u8) anyerror!void = null;
var test_state_cleanup_hook: ?*const fn (std.mem.Allocator, []const u8, []const u8) anyerror!void = null;

fn durable_boundary() !void {
    test_boundary_count += 1;
    if (test_crash_at) |point| if (test_boundary_count == point) c._exit(99);
    if (test_fail_at) |point| if (test_boundary_count == point) return error.InjectedIoFailure;
}
const parsed_journal_t = struct {
    arena: std.heap.ArenaAllocator,
    value: journal_t,
    fn deinit(self: *parsed_journal_t) void {
        self.arena.deinit();
    }
};

/// Owns validated root and lock descriptors. Open is exclusive and nonblocking;
/// keep this value alive through recovery/commit and close it once done.
pub const store_t = struct {
    /// @ownership Allocator used for all owned paths and temporary state.
    allocator: std.mem.Allocator,
    /// @in Copy of the caller-selected scope.
    scope: scope_t,
    /// @ownership Owned absolute path copies, freed by `close`.
    state_path: []u8,
    /// @ownership Owned absolute path copies, freed by `close`.
    data_path: []u8,
    /// @ownership Owned absolute path copies, freed by `close`.
    config_path: []u8,
    /// @ownership Owned allowlisted root path, freed by `close`.
    applications_path: []u8,
    /// @ownership Owned allowlisted root path, freed by `close`.
    icons_path: []u8,
    /// @ownership Owned allowlisted root path, freed by `close`.
    menus_path: []u8,
    /// @ownership Owned allowlisted root path, freed by `close`.
    desktop_directories_path: []u8,
    /// @ownership Owned allowlisted root path, freed by `close`.
    autostart_path: []u8,
    /// @ownership Optional owned path, freed by `close`.
    desktop_path: ?[]u8,
    /// @ownership Owned POSIX descriptors; `close` releases each live one.
    state_fd: c_int = -1,
    /// @ownership Private transaction directory descriptor; closed by `close`.
    transactions_fd: c_int = -1,
    /// @ownership Private retained-backup descriptor; closed by `close`.
    backups_fd: c_int = -1,
    /// @ownership Held nonblocking flock descriptor; closed by `close`.
    lock_fd: c_int = -1,
    /// @in/out Internal lifecycle bit; callers use `close`, not direct writes.
    is_live: bool = true,

    /// Open explicit roots, validate owner/mode/no-symlink properties, create
    /// only private transaction children, and acquire a nonblocking lock.
    /// @param allocator in Allocator that owns copied paths and transaction data.
    /// @param config in Caller-resolved absolute root configuration.
    /// @return A live store owning descriptors and copied paths; close it once.
    /// @errors AccessDenied, Busy, InvalidPath, OutOfMemory, or I/O errors.
    /// @thread_safety OS flock serializes independent processes. The returned
    /// store must be externally synchronized when shared between threads.
    pub fn open(allocator: std.mem.Allocator, config: store_config_t) !store_t {
        if (config.scope == .System and (!std.mem.eql(u8, config.state_dir, "/var/lib/waddle/integration") or c.geteuid() != 0)) return error.AccessDenied;
        var self = store_t{
            .allocator = allocator,
            .scope = config.scope,
            .state_path = &.{},
            .data_path = &.{},
            .config_path = &.{},
            .applications_path = &.{},
            .icons_path = &.{},
            .menus_path = &.{},
            .desktop_directories_path = &.{},
            .autostart_path = &.{},
            .desktop_path = null,
        };
        errdefer self.close();
        self.state_path = try allocator.dupe(u8, config.state_dir);
        self.data_path = try allocator.dupe(u8, config.data_home);
        self.config_path = try allocator.dupe(u8, config.config_home);
        self.applications_path = try std.fs.path.join(allocator, &.{ config.data_home, "applications" });
        self.icons_path = try std.fs.path.join(allocator, &.{ config.data_home, "icons" });
        self.menus_path = try std.fs.path.join(allocator, &.{ config.config_home, "menus", "applications-merged" });
        self.desktop_directories_path = try std.fs.path.join(allocator, &.{ config.data_home, "desktop-directories" });
        self.autostart_path = try std.fs.path.join(allocator, &.{ config.config_home, "autostart" });
        if (config.desktop_root) |path| self.desktop_path = try allocator.dupe(u8, path);
        const state_path = self.state_path;
        const data_path = self.data_path;
        const config_path = self.config_path;
        const desktop_path = self.desktop_path;
        self.state_fd = try open_absolute_dir(allocator, state_path, true, config.scope == .User);
        self.transactions_fd = try open_or_create_child(self.state_fd, "transactions", true);
        self.backups_fd = try open_or_create_child(self.state_fd, "backups", true);
        const data_fd = try open_absolute_dir(allocator, data_path, false, config.scope == .User);
        _ = c.close(data_fd);
        const config_fd = try open_absolute_dir(allocator, config_path, false, config.scope == .User);
        _ = c.close(config_fd);
        if (desktop_path) |path| {
            const desktop_fd = try open_absolute_dir(allocator, path, false, config.scope == .User);
            _ = c.close(desktop_fd);
        }
        const lock_name = try allocator.dupeZ(u8, "manifest.lock");
        defer allocator.free(lock_name);
        self.lock_fd = c.openat(self.state_fd, lock_name, c.O_RDWR | c.O_CREAT | c.O_CLOEXEC | c.O_NOFOLLOW | c.O_NONBLOCK, @as(c_uint, 0o600));
        if (self.lock_fd < 0) return io_error();
        try verify_regular(self.lock_fd, config.scope == .User, true);
        if (c.flock(self.lock_fd, c.LOCK_EX | c.LOCK_NB) != 0) return if (c.__errno_location().* == c.EWOULDBLOCK or c.__errno_location().* == c.EAGAIN) error.Busy else io_error();
        try fsync_fd(self.state_fd);
        return self;
    }

    /// Recover every outstanding transaction. A malformed/conflicting journal
    /// is retained and blocks subsequent commits.
    /// @param self in/out Live store holding the process lock.
    /// @return Void after all journals are verified and resolved.
    /// @errors RecoveryRequired, Conflict, InvalidJournal, or I/O/allocation errors.
    /// @thread_safety Not safe to call concurrently on one store instance.
    pub fn recover(self: *store_t) !void {
        if (!self.is_live or self.lock_fd < 0) return error.Closed;
        try self.validate_retained_backups();
        const names = try list_journals(self.allocator, self.transactions_fd);
        defer {
            for (names) |name| self.allocator.free(name);
            self.allocator.free(names);
        }
        if (names.len > MaxJournals) return error.RecoveryRequired;
        const journals = try self.allocator.alloc(parsed_journal_t, names.len);
        var journal_count: usize = 0;
        defer {
            for (journals[0..journal_count]) |*journal| journal.deinit();
            self.allocator.free(journals);
        }
        const raw = try self.allocator.alloc([]u8, names.len);
        var raw_count: usize = 0;
        defer {
            for (raw[0..raw_count]) |bytes| self.allocator.free(bytes);
            self.allocator.free(raw);
        }
        for (names, 0..) |name, i| {
            raw[i] = try read_child(self.allocator, self.transactions_fd, name, MaxJournalBytes, true);
            raw_count += 1;
            journals[i] = try decode_journal(self.allocator, raw[i], name);
            journal_count += 1;
        }
        for (journals) |journal| {
            if (journal.value.phase == .Prepared) try self.validate_recovery_state(journal.value) else try self.verify_committed(journal.value);
        }
        for (journals) |journal| try self.recover_one(journal.value);
        try self.cleanup_orphans();
    }

    /// Validate, stage, publish artifacts, publish the manifest last, then
    /// durably mark committed. On error after prepared, rollback is attempted.
    /// @param self in/out Live locked store.
    /// @param request in Borrowed manifest and actions; slices live through return.
    /// @return Void only after artifacts and manifest are durable and verified.
    /// @errors Conflict, UnsupportedPolicy, InvalidAction, RecoveryRequired, or I/O errors.
    /// @thread_safety Not safe to call concurrently on one store instance.
    pub fn commit(self: *store_t, request: commit_request_t) !void {
        if (!self.is_live or self.lock_fd < 0) return error.Closed;
        try self.recover();
        if (request.actions.len > MaxActions or self.scope != .User) return error.UnsupportedPolicy;
        const old_bytes = read_child(self.allocator, self.state_fd, "manifest.json", manifest.MaxManifestBytes, true) catch |err| switch (err) {
            error.FileNotFound => null,
            else => return err,
        };
        defer if (old_bytes) |bytes| self.allocator.free(bytes);
        if (old_bytes) |bytes| {
            if (request.expected_manifest_sha256 == null or !std.mem.eql(u8, &(request.expected_manifest_sha256 orelse unreachable), &hash_raw(bytes))) return error.Conflict;
        } else if (request.expected_manifest_sha256 != null) return error.Conflict;
        var old_doc: ?manifest.document_t = null;
        defer if (old_doc) |*doc| doc.deinit();
        if (old_bytes) |bytes| old_doc = try manifest.parse(self.allocator, bytes);
        var new_doc = try manifest.parse(self.allocator, request.candidate_manifest);
        defer new_doc.deinit();
        if (old_doc) |doc| try self.validate_document(doc);
        try self.validate_document(new_doc);
        try self.validate_complete_transition(old_doc, new_doc, request.actions);
        var txid_buf: [16]u8 = undefined;
        std.crypto.random.bytes(&txid_buf);
        const txid = try hex_encode(self.allocator, &txid_buf);
        defer self.allocator.free(txid);
        const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
        defer self.allocator.free(txdir_name);
        const tx_dir = try open_or_create_child(self.transactions_fd, txdir_name, true);
        defer _ = c.close(tx_dir);
        var entry_list = std.ArrayList(journal_action_t).init(self.allocator);
        defer {
            for (entry_list.items) |entry| {
                self.allocator.free(entry.record_id);
                self.allocator.free(entry.relative_path);
            }
            entry_list.deinit();
        }
        try entry_list.ensureTotalCapacity(request.actions.len);
        for (request.actions, 0..) |action, index| {
            const entry = try self.prepare_action(txid, index, action, old_doc, new_doc);
            for (entry_list.items) |prior| {
                if (prior.root == entry.root and std.mem.eql(u8, prior.relative_path, entry.relative_path)) {
                    self.allocator.free(entry.record_id);
                    self.allocator.free(entry.relative_path);
                    return error.InvalidAction;
                }
            }
            entry_list.appendAssumeCapacity(entry);
        }
        const entries = entry_list.items;
        try write_child(tx_dir, "manifest.new", request.candidate_manifest, 0o600, true);
        if (old_bytes) |bytes| try write_child(tx_dir, "manifest.old", bytes, 0o600, true);
        try fsync_fd(tx_dir);
        try fsync_fd(self.transactions_fd);
        var record = journal_t{
            .schema = 1,
            .transaction_id = txid,
            .phase = .Prepared,
            .old_manifest_exists = old_bytes != null,
            .old_manifest_sha256 = if (old_bytes) |bytes| hash_hex_array(bytes) else null,
            .new_manifest_sha256 = hash_hex_array(request.candidate_manifest),
            .actions = entries,
            .state_identity = try directory_identity(self.state_fd),
        };
        try self.save_journal(record, true);
        self.finish_transaction(txid, entries, &record) catch |err| {
            self.rollback_transaction(txid) catch return error.RecoveryRequired;
            return err;
        };
        try self.cleanup_committed(txid, entries, record.state_identity);
    }

    fn finish_transaction(self: *store_t, txid: []const u8, entries: []const journal_action_t, record: *journal_t) !void {
        for (entries, 0..) |entry, index| try self.apply_action(txid, index, entry);
        try self.publish_manifest(txid, record.new_manifest_sha256, record.state_identity);
        record.phase = .Committed;
        try self.save_journal(record.*, false);
    }

    /// Release the lock and all descriptors/owned paths. Safe to call twice.
    /// @param self in/out Store whose resources are released; pointer remains valid.
    /// @return Void; repeated calls are no-ops.
    /// @thread_safety Caller must ensure no operation is in progress.
    pub fn close(self: *store_t) void {
        if (!self.is_live) return;
        if (self.lock_fd >= 0) _ = c.close(self.lock_fd);
        if (self.backups_fd >= 0) _ = c.close(self.backups_fd);
        if (self.transactions_fd >= 0) _ = c.close(self.transactions_fd);
        if (self.state_fd >= 0) _ = c.close(self.state_fd);
        self.allocator.free(self.state_path);
        self.allocator.free(self.data_path);
        self.allocator.free(self.config_path);
        self.allocator.free(self.applications_path);
        self.allocator.free(self.icons_path);
        self.allocator.free(self.menus_path);
        self.allocator.free(self.desktop_directories_path);
        self.allocator.free(self.autostart_path);
        if (self.desktop_path) |path| self.allocator.free(path);
        self.state_fd = -1;
        self.transactions_fd = -1;
        self.backups_fd = -1;
        self.lock_fd = -1;
        self.is_live = false;
    }

    fn prepare_action(self: *store_t, txid: []const u8, index: usize, action: artifact_action_t, old_doc: ?manifest.document_t, new_doc: manifest.document_t) !journal_action_t {
        try validate_action_shape(action);
        const target = try self.resolve_target(action.root, action.relative_path);
        defer self.allocator.free(target);
        const old_record = if (old_doc) |doc| find_record(doc.records, action.record_id) else null;
        if (old_record) |record| {
            if (!std.mem.eql(u8, record.path, target) or !kind_matches(record.kind, action.root)) return error.UnsupportedPolicy;
        }
        const new_record = find_record(new_doc.records, action.record_id);
        if (action.operation == .Write) {
            const record = new_record orelse return error.Conflict;
            if (!std.mem.eql(u8, record.path, target) or !kind_matches(record.kind, action.root)) return error.UnsupportedPolicy;
            if (action.bytes == null) return error.Conflict;
            const new_hash = try hash_hex(self.allocator, action.bytes.?);
            defer self.allocator.free(new_hash);
            if (!hash_text_equal(record.sha256, new_hash)) return error.Conflict;
        } else if (new_record != null or old_record == null) return error.Conflict;
        const action_root_path = self.root_path(action.root) orelse return error.UnsupportedPolicy;
        const root_fd = try open_absolute_dir(self.allocator, action_root_path, false, true);
        defer _ = c.close(root_fd);
        const root_identity = try directory_identity(root_fd);
        const parent_target = try target_parent(self.allocator, action_root_path, action.relative_path);
        defer {
            _ = c.close(parent_target.fd);
            self.allocator.free(parent_target.leaf);
        }
        const parent_identity = try directory_identity(parent_target.fd);
        const current = try read_target(self.allocator, action.root, action_root_path, action.relative_path, MaxArtifactBytes, false);
        defer if (current) |bytes| self.allocator.free(bytes);
        const current_hash = if (current) |bytes| try hash_hex(self.allocator, bytes) else null;
        defer if (current_hash) |hash| self.allocator.free(hash);
        const current_mode = try target_mode(self.allocator, action.root, action_root_path, action.relative_path);
        const expected_hex = if (action.expected_current_sha256) |value| try hex_encode(self.allocator, &value) else null;
        defer if (expected_hex) |value| self.allocator.free(value);
        if (current_hash) |hash| {
            if (expected_hex == null) return error.Conflict;
            if (!hash_text_equal(hash, expected_hex.?)) {
                if (!action.force or old_record == null) return error.Conflict;
            }
            if (old_record) |record| {
                if (!hash_text_equal(record.sha256, hash) and !action.force) return error.Conflict;
                if (!hash_text_equal(record.sha256, hash) and action.force)
                    try self.copy_retained_backup(txid, action.record_id, current.?, current_mode);
            } else if (expected_hex != null) return error.Conflict;
        } else if (expected_hex != null) return error.Conflict;
        const before_fixed = if (current_hash) |value| try hex_to_array(value) else null;
        const after_hash = if (action.operation == .Write) try hash_hex(self.allocator, action.bytes.?) else null;
        defer if (after_hash) |value| self.allocator.free(value);
        const after_fixed = if (after_hash) |value| try hex_to_array(value) else null;
        const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
        defer self.allocator.free(txdir_name);
        const data_dir = try open_child(self.transactions_fd, txdir_name);
        defer _ = c.close(data_dir);
        if (current) |bytes| {
            const old_name = try std.fmt.allocPrintZ(self.allocator, "{d}.old", .{index});
            defer self.allocator.free(old_name);
            try write_child(data_dir, old_name, bytes, 0o600, true);
        }
        if (action.bytes) |bytes| {
            const new_name = try std.fmt.allocPrintZ(self.allocator, "{d}.new", .{index});
            defer self.allocator.free(new_name);
            try write_child(data_dir, new_name, bytes, 0o600, true);
        }
        try fsync_fd(data_dir);
        const record_id = try self.allocator.dupe(u8, action.record_id);
        errdefer self.allocator.free(record_id);
        const relative_path = try self.allocator.dupe(u8, action.relative_path);
        errdefer self.allocator.free(relative_path);
        return .{
            .record_id = record_id,
            .root = action.root,
            .relative_path = relative_path,
            .before_exists = current != null,
            .before_sha256 = before_fixed,
            .before_mode = current_mode,
            .after_exists = action.operation == .Write,
            .after_sha256 = after_fixed,
            .after_mode = if (action.operation == .Write) action.mode else 0,
            .force_backup = if (old_record) |record| action.force and current != null and !hash_text_equal(record.sha256, current_hash.?) else false,
            .root_identity = root_identity,
            .parent_identity = parent_identity,
        };
    }

    fn resolve_target(self: *store_t, root: artifact_root_t, relative: []const u8) ![]u8 {
        try validate_relative(relative);
        const root_base = self.root_path(root) orelse return error.UnsupportedPolicy;
        try validate_root_relative(root, relative);
        return std.fs.path.join(self.allocator, &.{ root_base, relative });
    }

    fn validate_document(self: *store_t, document: manifest.document_t) !void {
        for (document.records) |record| {
            const roots: []const artifact_root_t = switch (record.kind) {
                .DesktopEntry, .HandlerEntry => &.{.Applications},
                .Icon => &.{.Icons},
                .Autostart => &.{.Autostart},
                .DesktopShortcut => &.{.Desktop},
                .Menu => &.{ .MenuMerged, .DesktopDirectories },
                .MimeDefault, .Binfmt, .Polkit, .GuestPolicy, .GuestComponent, .GuestAutostart, .GuestCertificate, .GuestPrinter => return error.UnsupportedPolicy,
            };
            var matched = false;
            for (roots) |root| {
                const root_path_value = self.root_path(root) orelse continue;
                const relative = path_relative_to_root(record.path, root_path_value) orelse continue;
                validate_relative(relative) catch continue;
                validate_root_relative(root, relative) catch continue;
                matched = true;
                break;
            }
            if (!matched) return error.UnsupportedPolicy;
        }
    }

    fn records_equal(left: manifest.record_t, right: manifest.record_t) bool {
        if (!std.mem.eql(u8, left.id, right.id) or left.kind != right.kind or
            !std.mem.eql(u8, left.path, right.path) or !std.mem.eql(u8, left.sha256, right.sha256) or
            !std.mem.eql(u8, left.created_by, right.created_by) or
            !optional_text_equal(left.device, right.device) or !optional_text_equal(left.app_key, right.app_key) or
            !optional_text_equal(left.mime, right.mime)) return false;
        return switch (left.prior) {
            .None => right.prior == .None,
            .Text => |value| switch (right.prior) {
                .Text => |other| std.mem.eql(u8, value, other),
                else => false,
            },
            .U32Value => |value| switch (right.prior) {
                .U32Value => |other| value == other,
                else => false,
            },
        };
    }

    fn optional_text_equal(left: ?[]const u8, right: ?[]const u8) bool {
        if (left == null or right == null) return left == null and right == null;
        return std.mem.eql(u8, left.?, right.?);
    }

    /// Require a one-to-one action for every record change, and verify records
    /// that remain byte-for-byte unchanged still describe the installed file.
    fn validate_complete_transition(self: *store_t, old_doc: ?manifest.document_t, new_doc: manifest.document_t, actions: []const artifact_action_t) !void {
        for (actions, 0..) |action, i| {
            for (actions[0..i]) |prior| {
                if (std.mem.eql(u8, prior.record_id, action.record_id)) return error.InvalidAction;
            }
        }
        if (old_doc) |old| {
            for (old.records) |record| {
                const next = find_record(new_doc.records, record.id);
                const action = find_action(actions, record.id);
                if (next) |new_record| {
                    if (records_equal(record, new_record)) {
                        if (action != null) return error.InvalidAction;
                        try self.verify_record_on_disk(record);
                    } else {
                        if (action == null) return error.InvalidAction;
                        if (!std.mem.eql(u8, record.path, new_record.path) or record.kind != new_record.kind)
                            return error.UnsupportedPolicy;
                        if (action.?.operation != .Write) return error.InvalidAction;
                    }
                } else {
                    if (action == null or action.?.operation != .Remove) return error.InvalidAction;
                }
            }
        }
        for (new_doc.records) |record| {
            if (old_doc == null or find_record(old_doc.?.records, record.id) == null) {
                const action = find_action(actions, record.id) orelse return error.InvalidAction;
                if (action.operation != .Write) return error.InvalidAction;
            }
        }
        for (actions) |action| {
            const old_record = if (old_doc) |doc| find_record(doc.records, action.record_id) else null;
            const new_record = find_record(new_doc.records, action.record_id);
            if (old_record == null and new_record == null) return error.InvalidAction;
            const record = new_record orelse old_record.?;
            if (!kind_matches(record.kind, action.root)) return error.UnsupportedPolicy;
            const target = try self.resolve_target(action.root, action.relative_path);
            defer self.allocator.free(target);
            if (!std.mem.eql(u8, record.path, target)) return error.UnsupportedPolicy;
        }
    }

    fn verify_record_on_disk(self: *store_t, record: manifest.record_t) !void {
        const roots: []const artifact_root_t = switch (record.kind) {
            .DesktopEntry, .HandlerEntry => &.{.Applications},
            .Icon => &.{.Icons},
            .Autostart => &.{.Autostart},
            .DesktopShortcut => &.{.Desktop},
            .Menu => &.{ .MenuMerged, .DesktopDirectories },
            else => return error.UnsupportedPolicy,
        };
        for (roots) |root_kind| {
            const root = self.root_path(root_kind) orelse continue;
            const relative = path_relative_to_root(record.path, root) orelse continue;
            const bytes = try read_target(self.allocator, root_kind, root, relative, MaxArtifactBytes, false) orelse return error.Conflict;
            defer self.allocator.free(bytes);
            if (!hash_text_equal(record.sha256, &hash_hex_array(bytes))) return error.Conflict;
            return;
        }
        return error.UnsupportedPolicy;
    }

    fn root_path(self: *store_t, root: artifact_root_t) ?[]const u8 {
        return switch (root) {
            .Applications => self.applications_path,
            .Icons => self.icons_path,
            .MenuMerged => self.menus_path,
            .DesktopDirectories => self.desktop_directories_path,
            .Autostart => self.autostart_path,
            .Desktop => self.desktop_path,
        };
    }

    fn copy_retained_backup(self: *store_t, txid: []const u8, record_id: []const u8, bytes: []const u8, mode: u32) !void {
        const id_hash = try hash_hex(self.allocator, record_id);
        defer self.allocator.free(id_hash);
        const name = try std.fmt.allocPrintZ(self.allocator, "{s}-{s}", .{ txid, id_hash[0..16] });
        defer self.allocator.free(name);
        try write_child(self.backups_fd, name, bytes, 0o600, true);
        const metadata_name = try std.fmt.allocPrintZ(self.allocator, "{s}.meta", .{name});
        defer self.allocator.free(metadata_name);
        const backup_hash = try hash_hex(self.allocator, bytes);
        defer self.allocator.free(backup_hash);
        const metadata = try std.json.stringifyAlloc(self.allocator, retained_backup_metadata_t{
            .schema = 1,
            .sha256 = backup_hash,
            .mode = mode,
        }, .{});
        defer self.allocator.free(metadata);
        try write_child(self.backups_fd, metadata_name, metadata, 0o600, true);
        try fsync_fd(self.backups_fd);
    }

    /// Incomplete retained-backup pairs are preserved and block mutation. This
    /// makes a crash between payload and metadata publication visible instead
    /// of silently treating the payload as disposable.
    fn validate_retained_backups(self: *store_t) !void {
        const entries = try list_directory(self.allocator, self.backups_fd);
        defer {
            for (entries) |entry| self.allocator.free(entry.name);
            self.allocator.free(entries);
        }
        for (entries) |entry| {
            if (entry.kind != .file) return error.RecoveryRequired;
            if (std.mem.endsWith(u8, entry.name, ".meta")) {
                const base = entry.name[0 .. entry.name.len - ".meta".len];
                if (base.len == 0 or !directory_has_file(entries, base)) return error.RecoveryRequired;
                const raw = try read_child(self.allocator, self.backups_fd, base, MaxArtifactBytes, true);
                defer self.allocator.free(raw);
                const metadata = try read_child(self.allocator, self.backups_fd, entry.name, 1024, true);
                defer self.allocator.free(metadata);
                const parsed = std.json.parseFromSlice(retained_backup_metadata_t, self.allocator, metadata, .{ .allocate = .alloc_always, .ignore_unknown_fields = false, .duplicate_field_behavior = .@"error" }) catch return error.RecoveryRequired;
                defer parsed.deinit();
                if (parsed.value.schema != 1 or !valid_hash_text(parsed.value.sha256) or
                    (parsed.value.mode != 0o644 and parsed.value.mode != 0o755) or
                    !std.mem.eql(u8, parsed.value.sha256, &hash_hex_array(raw))) return error.RecoveryRequired;
            } else {
                const metadata_name = try std.fmt.allocPrint(self.allocator, "{s}.meta", .{entry.name});
                defer self.allocator.free(metadata_name);
                if (!directory_has_file(entries, metadata_name)) return error.RecoveryRequired;
            }
        }
    }

    fn save_journal(self: *store_t, journal: journal_t, first: bool) !void {
        const bytes = try encode_journal(self.allocator, journal);
        defer self.allocator.free(bytes);
        if (bytes.len > MaxJournalBytes) return error.JournalTooLarge;
        const filename = try std.fmt.allocPrintZ(self.allocator, "{s}.json", .{journal.transaction_id});
        defer self.allocator.free(filename);
        const temp = try std.fmt.allocPrintZ(self.allocator, "{s}.json.tmp", .{journal.transaction_id});
        defer self.allocator.free(temp);
        try write_child(self.transactions_fd, temp, bytes, 0o600, true);
        try rename_child(self.transactions_fd, temp, self.transactions_fd, filename, first);
        try fsync_fd(self.transactions_fd);
    }

    fn apply_action(self: *store_t, txid: []const u8, index: usize, entry: journal_action_t) !void {
        try self.validate_entry_identity(entry);
        const root = self.root_path(entry.root) orelse return error.UnsupportedPolicy;
        const current = try read_target(self.allocator, entry.root, root, entry.relative_path, MaxArtifactBytes, false);
        defer if (current) |bytes| self.allocator.free(bytes);
        const current_hash = if (current) |bytes| try hash_hex(self.allocator, bytes) else null;
        defer if (current_hash) |hash| self.allocator.free(hash);
        const current_mode = try target_mode(self.allocator, entry.root, root, entry.relative_path);
        if (current_hash) |hash| {
            if (entry.after_sha256 != null and std.mem.eql(u8, hash, &entry.after_sha256.?) and current_mode == entry.after_mode) return;
            if (!entry.before_exists or !std.mem.eql(u8, hash, &entry.before_sha256.?) or current_mode != entry.before_mode) return error.Conflict;
        } else if (entry.before_exists) return error.Conflict;
        if (entry.after_exists) {
            const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
            defer self.allocator.free(txdir_name);
            const tx_dir = try open_child(self.transactions_fd, txdir_name);
            defer _ = c.close(tx_dir);
            const payload_name = try std.fmt.allocPrintZ(self.allocator, "{d}.new", .{index});
            defer self.allocator.free(payload_name);
            const payload = try read_child(self.allocator, tx_dir, payload_name, MaxArtifactBytes, true);
            defer self.allocator.free(payload);
            try write_target_atomic_checked(self.allocator, entry.root, root, entry.relative_path, payload, entry.after_mode, txid, index, entry.root_identity, entry.parent_identity, entry.before_sha256, entry.before_mode);
        } else try unlink_target_checked(self.allocator, entry.root, root, entry.relative_path, entry.root_identity, entry.parent_identity, entry.before_sha256 orelse return error.RecoveryRequired, entry.before_mode);
    }

    fn publish_manifest(self: *store_t, txid: []const u8, expected_hash: [64]u8, state_identity: directory_identity_t) !void {
        try self.validate_state_identity(state_identity);
        const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
        defer self.allocator.free(txdir_name);
        const tx_dir = try open_child(self.transactions_fd, txdir_name);
        defer _ = c.close(tx_dir);
        const payload = try read_child(self.allocator, tx_dir, "manifest.new", manifest.MaxManifestBytes, true);
        defer self.allocator.free(payload);
        const actual = try hash_hex(self.allocator, payload);
        defer self.allocator.free(actual);
        if (!std.mem.eql(u8, actual, &expected_hash)) return error.RecoveryRequired;
        try self.write_state_manifest_atomic(payload, txid, MaxActions + 1, state_identity);
    }

    /// Publish the state manifest relative to the already-held state directory
    /// descriptor. The named path is checked around publication; a path swap
    /// can never redirect the rename into an attacker-selected directory.
    fn write_state_manifest_atomic(self: *store_t, payload: []const u8, txid: []const u8, index: usize, state_identity: directory_identity_t) !void {
        try self.validate_state_identity(state_identity);
        const temp = try std.fmt.allocPrintZ(self.allocator, ".waddle-{s}-{d}.tmp", .{ txid, index });
        defer self.allocator.free(temp);
        try write_child(self.state_fd, temp, payload, 0o600, true);
        errdefer unlink_child_if_present(self.state_fd, temp) catch {};
        try self.validate_state_identity(state_identity);
        if (test_state_mutation_hook) |hook| {
            test_state_mutation_hook = null;
            try hook(self.allocator, self.state_path);
        }
        try rename_child(self.state_fd, temp, self.state_fd, "manifest.json", false);
        try self.validate_state_identity(state_identity);
    }

    /// Remove the state manifest only through the held directory descriptor.
    fn remove_state_manifest(self: *store_t, state_identity: directory_identity_t) !void {
        try self.validate_state_identity(state_identity);
        try unlink_child_if_present(self.state_fd, "manifest.json");
        try self.validate_state_identity(state_identity);
    }

    fn remove_state_temp(self: *store_t, txid: []const u8, index: usize, state_identity: directory_identity_t) !void {
        try self.validate_state_identity(state_identity);
        const temp = try std.fmt.allocPrintZ(self.allocator, ".waddle-{s}-{d}.tmp", .{ txid, index });
        defer self.allocator.free(temp);
        if (test_state_cleanup_hook) |hook| {
            test_state_cleanup_hook = null;
            try hook(self.allocator, self.state_path, temp);
        }
        try unlink_child_if_present(self.state_fd, temp);
        try self.validate_state_identity(state_identity);
    }

    fn recover_one(self: *store_t, journal: journal_t) !void {
        if (journal.phase == .Committed) {
            try self.verify_committed(journal);
            try self.cleanup_committed(journal.transaction_id, journal.actions, journal.state_identity);
        } else {
            try self.validate_recovery_state(journal);
            try self.rollback_transaction(journal.transaction_id);
        }
    }

    fn cleanup_orphans(self: *store_t) !void {
        const entries = try list_directory(self.allocator, self.transactions_fd);
        defer {
            for (entries) |entry| self.allocator.free(entry.name);
            self.allocator.free(entries);
        }
        for (entries) |entry| {
            if (std.mem.endsWith(u8, entry.name, ".json")) continue;
            if (std.mem.endsWith(u8, entry.name, ".json.tmp")) {
                const id = entry.name[0 .. entry.name.len - ".json.tmp".len];
                if (!valid_txid(id) or entry.kind != .file) return error.RecoveryRequired;
                const temp = try self.allocator.dupeZ(u8, entry.name);
                defer self.allocator.free(temp);
                try unlink_child_if_present(self.transactions_fd, temp);
            } else if (std.mem.endsWith(u8, entry.name, ".data")) {
                const id = entry.name[0 .. entry.name.len - ".data".len];
                if (!valid_txid(id) or entry.kind != .directory) return error.RecoveryRequired;
                const journal_name = try std.fmt.allocPrintZ(self.allocator, "{s}.json", .{id});
                defer self.allocator.free(journal_name);
                var stat: c.struct_stat = undefined;
                if (c.fstatat(self.transactions_fd, journal_name, &stat, c.AT_SYMLINK_NOFOLLOW) == 0) continue;
                if (c.__errno_location().* != c.ENOENT) return io_error();
                const data_name = try self.allocator.dupeZ(u8, entry.name);
                defer self.allocator.free(data_name);
                try delete_tree_child(self.transactions_fd, data_name);
            } else return error.RecoveryRequired;
        }
    }

    fn validate_recovery_state(self: *store_t, journal: journal_t) !void {
        try self.validate_journal_identities(journal);
        const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{journal.transaction_id});
        defer self.allocator.free(txdir_name);
        const tx_dir = try open_child(self.transactions_fd, txdir_name);
        defer _ = c.close(tx_dir);
        const new_manifest = try read_child(self.allocator, tx_dir, "manifest.new", manifest.MaxManifestBytes, true);
        defer self.allocator.free(new_manifest);
        if (!std.mem.eql(u8, &hash_hex_array(new_manifest), &journal.new_manifest_sha256)) return error.RecoveryRequired;
        var new_document = try manifest.parse(self.allocator, new_manifest);
        defer new_document.deinit();
        try self.validate_document(new_document);
        var old_document: ?manifest.document_t = null;
        defer if (old_document) |*doc| doc.deinit();
        if (journal.old_manifest_exists) {
            const old_manifest = try read_child(self.allocator, tx_dir, "manifest.old", manifest.MaxManifestBytes, true);
            defer self.allocator.free(old_manifest);
            if (!std.mem.eql(u8, &hash_hex_array(old_manifest), &journal.old_manifest_sha256.?)) return error.RecoveryRequired;
            old_document = try manifest.parse(self.allocator, old_manifest);
            try self.validate_document(old_document.?);
        }
        try self.validate_journal_transition(old_document, new_document, journal.actions);
        const current_manifest = read_child(self.allocator, self.state_fd, "manifest.json", manifest.MaxManifestBytes, true) catch |err| switch (err) {
            error.FileNotFound => null,
            else => return err,
        };
        defer if (current_manifest) |bytes| self.allocator.free(bytes);
        if (current_manifest) |bytes| {
            const current_hash = hash_hex_array(bytes);
            const is_old = journal.old_manifest_sha256 != null and std.mem.eql(u8, &current_hash, &journal.old_manifest_sha256.?);
            const is_new = std.mem.eql(u8, &current_hash, &journal.new_manifest_sha256);
            if (!is_old and !is_new) return error.Conflict;
        } else if (journal.old_manifest_exists or journal.phase == .Committed) return error.Conflict;
        for (journal.actions, 0..) |entry, index| {
            try self.validate_action_manifest(old_document, new_document, entry, true);
            if (entry.before_exists) {
                const old_name = try std.fmt.allocPrintZ(self.allocator, "{d}.old", .{index});
                defer self.allocator.free(old_name);
                const old = try read_child(self.allocator, tx_dir, old_name, MaxArtifactBytes, true);
                defer self.allocator.free(old);
                if (!std.mem.eql(u8, &hash_hex_array(old), &entry.before_sha256.?)) return error.RecoveryRequired;
            }
            if (entry.after_exists) {
                const new_name = try std.fmt.allocPrintZ(self.allocator, "{d}.new", .{index});
                defer self.allocator.free(new_name);
                const new_bytes = try read_child(self.allocator, tx_dir, new_name, MaxArtifactBytes, true);
                defer self.allocator.free(new_bytes);
                if (!std.mem.eql(u8, &hash_hex_array(new_bytes), &entry.after_sha256.?)) return error.RecoveryRequired;
            }
            const root = self.root_path(entry.root) orelse return error.UnsupportedPolicy;
            const current = try read_target(self.allocator, entry.root, root, entry.relative_path, MaxArtifactBytes, false);
            defer if (current) |bytes| self.allocator.free(bytes);
            const current_mode = try target_mode(self.allocator, entry.root, root, entry.relative_path);
            if (current) |bytes| {
                const hash = hash_hex_array(bytes);
                const before = entry.before_sha256 != null and std.mem.eql(u8, &hash, &entry.before_sha256.?) and current_mode == entry.before_mode;
                const after = entry.after_sha256 != null and std.mem.eql(u8, &hash, &entry.after_sha256.?) and current_mode == entry.after_mode;
                if (!before and !after) return error.Conflict;
            } else if (entry.before_exists and entry.after_exists) return error.Conflict;
        }
    }

    fn validate_action_manifest(self: *store_t, old_document: ?manifest.document_t, new_document: manifest.document_t, entry: journal_action_t, validate_before: bool) !void {
        const old_record = if (old_document) |doc| find_record(doc.records, entry.record_id) else null;
        const new_record = find_record(new_document.records, entry.record_id);
        const target = try self.resolve_target(entry.root, entry.relative_path);
        defer self.allocator.free(target);
        if (validate_before) {
            if (entry.before_exists) {
                const record = old_record orelse return error.InvalidJournal;
                if (!kind_matches(record.kind, entry.root) or !std.mem.eql(u8, record.path, target)) return error.InvalidJournal;
                if (!hash_text_equal(record.sha256, &entry.before_sha256.?) and !entry.force_backup) return error.InvalidJournal;
            } else if (old_record != null) return error.InvalidJournal;
        }
        if (entry.after_exists) {
            const record = new_record orelse return error.InvalidJournal;
            if (!kind_matches(record.kind, entry.root) or !std.mem.eql(u8, record.path, target) or !hash_text_equal(record.sha256, &entry.after_sha256.?)) return error.InvalidJournal;
        } else if (new_record != null) return error.InvalidJournal;
    }

    fn validate_journal_transition(self: *store_t, old_doc: ?manifest.document_t, new_doc: manifest.document_t, entries: []const journal_action_t) !void {
        const actions = try self.allocator.alloc(artifact_action_t, entries.len);
        defer self.allocator.free(actions);
        for (entries, 0..) |entry, index| actions[index] = .{
            .record_id = entry.record_id,
            .root = entry.root,
            .relative_path = entry.relative_path,
            .operation = if (entry.after_exists) .Write else .Remove,
            .expected_current_sha256 = null,
            .bytes = null,
            .mode = if (entry.after_exists) entry.after_mode else 0o644,
            .force = entry.force_backup,
        };
        try self.validate_complete_transition(old_doc, new_doc, actions);
    }

    fn validate_journal_identities(self: *store_t, journal: journal_t) !void {
        try self.validate_state_identity(journal.state_identity);
        for (journal.actions) |entry| {
            const root = self.root_path(entry.root) orelse return error.UnsupportedPolicy;
            const root_fd = try open_absolute_dir(self.allocator, root, false, true);
            defer _ = c.close(root_fd);
            if (!identity_equal(try directory_identity(root_fd), entry.root_identity)) return error.Conflict;
            const parent = try target_parent(self.allocator, root, entry.relative_path);
            defer {
                _ = c.close(parent.fd);
                self.allocator.free(parent.leaf);
            }
            if (!identity_equal(try directory_identity(parent.fd), entry.parent_identity)) return error.Conflict;
        }
    }

    fn validate_state_identity(self: *store_t, expected: directory_identity_t) !void {
        if (!identity_equal(try directory_identity(self.state_fd), expected)) return error.Conflict;
        const named_state = try open_absolute_dir(self.allocator, self.state_path, false, true);
        defer _ = c.close(named_state);
        if (!identity_equal(try directory_identity(named_state), expected)) return error.Conflict;
    }

    fn validate_entry_identity(self: *store_t, entry: journal_action_t) !void {
        const root = self.root_path(entry.root) orelse return error.UnsupportedPolicy;
        const root_fd = try open_absolute_dir(self.allocator, root, false, true);
        defer _ = c.close(root_fd);
        if (!identity_equal(try directory_identity(root_fd), entry.root_identity)) return error.Conflict;
        const parent = try target_parent(self.allocator, root, entry.relative_path);
        defer {
            _ = c.close(parent.fd);
            self.allocator.free(parent.leaf);
        }
        if (!identity_equal(try directory_identity(parent.fd), entry.parent_identity)) return error.Conflict;
    }

    fn rollback_transaction(self: *store_t, txid: []const u8) !void {
        const name = try std.fmt.allocPrintZ(self.allocator, "{s}.json", .{txid});
        defer self.allocator.free(name);
        const raw = try read_child(self.allocator, self.transactions_fd, name, MaxJournalBytes, true);
        defer self.allocator.free(raw);
        var parsed = try decode_journal(self.allocator, raw, name);
        defer parsed.deinit();
        const journal = parsed.value;
        if (journal.phase == .Committed) return error.Conflict;
        var i = journal.actions.len;
        while (i > 0) {
            i -= 1;
            try self.rollback_action(txid, i, journal.actions[i]);
        }
        const manifest_current = read_child(self.allocator, self.state_fd, "manifest.json", manifest.MaxManifestBytes, true) catch |err| switch (err) {
            error.FileNotFound => null,
            else => return err,
        };
        defer if (manifest_current) |bytes| self.allocator.free(bytes);
        if (manifest_current) |bytes| {
            const hash = try hash_hex(self.allocator, bytes);
            defer self.allocator.free(hash);
            if (journal.old_manifest_sha256) |expected| {
                if (!std.mem.eql(u8, hash, &expected) and !std.mem.eql(u8, hash, &journal.new_manifest_sha256)) return error.Conflict;
            } else if (!std.mem.eql(u8, hash, &journal.new_manifest_sha256)) return error.Conflict;
        } else if (journal.old_manifest_exists) return error.Conflict;
        if (journal.old_manifest_exists) {
            const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
            defer self.allocator.free(txdir_name);
            const tx_dir = try open_child(self.transactions_fd, txdir_name);
            defer _ = c.close(tx_dir);
            const old = try read_child(self.allocator, tx_dir, "manifest.old", manifest.MaxManifestBytes, true);
            defer self.allocator.free(old);
            try self.write_state_manifest_atomic(old, txid, MaxActions + 2, journal.state_identity);
        } else try self.remove_state_manifest(journal.state_identity);
            try self.cleanup_prepared(txid, journal.actions, journal.state_identity);
    }

    fn rollback_action(self: *store_t, txid: []const u8, index: usize, entry: journal_action_t) !void {
        try self.validate_entry_identity(entry);
        const root = self.root_path(entry.root) orelse return error.UnsupportedPolicy;
        const current = try read_target(self.allocator, entry.root, root, entry.relative_path, MaxArtifactBytes, false);
        defer if (current) |bytes| self.allocator.free(bytes);
        const hash = if (current) |bytes| try hash_hex(self.allocator, bytes) else null;
        defer if (hash) |value| self.allocator.free(value);
        const current_mode = try target_mode(self.allocator, entry.root, root, entry.relative_path);
        if (hash) |value| {
            if (entry.before_sha256) |before| if (std.mem.eql(u8, value, &before) and current_mode == entry.before_mode) return;
            if (entry.after_sha256) |after| if (!std.mem.eql(u8, value, &after) or current_mode != entry.after_mode) return error.Conflict;
            if (entry.after_sha256 == null) return error.Conflict;
        } else if (!entry.before_exists) return;
        if (entry.before_exists) {
            const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
            defer self.allocator.free(txdir_name);
            const tx_dir = try open_child(self.transactions_fd, txdir_name);
            defer _ = c.close(tx_dir);
            const payload_name = try std.fmt.allocPrintZ(self.allocator, "{d}.old", .{index});
            defer self.allocator.free(payload_name);
            const old = try read_child(self.allocator, tx_dir, payload_name, MaxArtifactBytes, true);
            defer self.allocator.free(old);
            const old_hash = try hash_hex(self.allocator, old);
            defer self.allocator.free(old_hash);
            if (!std.mem.eql(u8, old_hash, &entry.before_sha256.?)) return error.RecoveryRequired;
            try write_target_atomic_checked(self.allocator, entry.root, root, entry.relative_path, old, entry.before_mode, txid, index + MaxActions + 3, entry.root_identity, entry.parent_identity, entry.after_sha256, entry.after_mode);
        } else try unlink_target_checked(self.allocator, entry.root, root, entry.relative_path, entry.root_identity, entry.parent_identity, entry.after_sha256 orelse return error.RecoveryRequired, entry.after_mode);
    }

    fn verify_committed(self: *store_t, journal: journal_t) !void {
        try self.validate_journal_identities(journal);
        const txdir_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{journal.transaction_id});
        defer self.allocator.free(txdir_name);
        const tx_dir = try open_child(self.transactions_fd, txdir_name);
        defer _ = c.close(tx_dir);
        const bytes = try read_child(self.allocator, self.state_fd, "manifest.json", manifest.MaxManifestBytes, true);
        defer self.allocator.free(bytes);
        const hash = try hash_hex(self.allocator, bytes);
        defer self.allocator.free(hash);
        if (!std.mem.eql(u8, hash, &journal.new_manifest_sha256)) return error.Conflict;
        var document = try manifest.parse(self.allocator, bytes);
        defer document.deinit();
        try self.validate_document(document);
        var old_document: ?manifest.document_t = null;
        defer if (old_document) |*doc| doc.deinit();
        if (journal.old_manifest_exists) {
            const old_bytes = try read_child(self.allocator, tx_dir, "manifest.old", manifest.MaxManifestBytes, true);
            defer self.allocator.free(old_bytes);
            if (!std.mem.eql(u8, &hash_hex_array(old_bytes), &journal.old_manifest_sha256.?)) return error.RecoveryRequired;
            old_document = try manifest.parse(self.allocator, old_bytes);
            try self.validate_document(old_document.?);
        }
        try self.validate_journal_transition(old_document, document, journal.actions);
        for (journal.actions) |entry| {
            try self.validate_action_manifest(null, document, entry, false);
            const root = self.root_path(entry.root) orelse return error.UnsupportedPolicy;
            const current = try read_target(self.allocator, entry.root, root, entry.relative_path, MaxArtifactBytes, false);
            defer if (current) |value| self.allocator.free(value);
            const current_mode = try target_mode(self.allocator, entry.root, root, entry.relative_path);
            if (entry.after_exists) {
                if (current == null) return error.Conflict;
                const current_hash = try hash_hex(self.allocator, current.?);
                defer self.allocator.free(current_hash);
                if (!std.mem.eql(u8, current_hash, &entry.after_sha256.?) or current_mode != entry.after_mode) return error.Conflict;
            } else if (current != null) return error.Conflict;
        }
        for (document.records) |record| try self.verify_record_on_disk(record);
    }

    fn cleanup_prepared(self: *store_t, txid: []const u8, actions: []const journal_action_t, state_identity: directory_identity_t) !void {
        for (actions, 0..) |entry, index| try remove_sibling_temp(self.allocator, self.root_path(entry.root).?, entry.relative_path, txid, index);
        try self.remove_state_temp(txid, MaxActions + 1, state_identity);
        try self.remove_state_temp(txid, MaxActions + 2, state_identity);
        const journal_name = try std.fmt.allocPrintZ(self.allocator, "{s}.json", .{txid});
        defer self.allocator.free(journal_name);
        try unlink_child_if_present(self.transactions_fd, journal_name);
        const txname = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
        defer self.allocator.free(txname);
        try delete_tree_child(self.transactions_fd, txname);
        try fsync_fd(self.transactions_fd);
    }

    fn cleanup_committed(self: *store_t, txid: []const u8, actions: []const journal_action_t, state_identity: directory_identity_t) !void {
        for (actions, 0..) |entry, index| try remove_sibling_temp(self.allocator, self.root_path(entry.root).?, entry.relative_path, txid, index);
        try self.remove_state_temp(txid, MaxActions + 1, state_identity);
        try self.remove_state_temp(txid, MaxActions + 2, state_identity);
        const journal_name = try std.fmt.allocPrintZ(self.allocator, "{s}.json", .{txid});
        defer self.allocator.free(journal_name);
        try unlink_child_if_present(self.transactions_fd, journal_name);
        try fsync_fd(self.transactions_fd);
        // Once the committed journal is durably gone, leftover staged data is
        // an unambiguous orphan and can be removed by cleanup_orphans.
        const data_name = try std.fmt.allocPrintZ(self.allocator, "{s}.data", .{txid});
        defer self.allocator.free(data_name);
        try delete_tree_child(self.transactions_fd, data_name);
    }
};
