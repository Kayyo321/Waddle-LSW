//! Public API integration tests for status, removal, and metadata migration.
//! Each test drives the store only through exported declarations, so the
//! journal internals are exercised by the module's own in-file tests.
const std = @import("std");
const manifest_store = @import("manifest_store");
const c = @cImport({
    @cInclude("fcntl.h");
    @cInclude("sys/stat.h");
    @cInclude("sys/types.h");
    @cInclude("unistd.h");
});

fn digest(bytes: []const u8) [64]u8 {
    var hash: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &hash, .{});
    return std.fmt.bytesToHex(hash, .lower);
}

fn document(allocator: std.mem.Allocator, path: []const u8, id: []const u8, hex: []const u8, version: []const u8) ![]u8 {
    return std.fmt.allocPrint(allocator, "{{\"schema\":1,\"waddle_version\":\"{s}\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"{s}\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ version, id, path, hex });
}

/// Assert that the stored manifest equals `expected` byte for byte.
fn expect_manifest_bytes(state: std.fs.Dir, expected: []const u8) !void {
    const stored = try state.readFileAlloc(std.testing.allocator, "manifest.json", 4 * 1024 * 1024);
    defer std.testing.allocator.free(stored);
    try std.testing.expectEqualStrings(expected, stored);
}

fn write_manifest(state: std.fs.Dir, bytes: []const u8) !void {
    const fd = c.openat(state.fd, "manifest.json", c.O_WRONLY | c.O_CREAT | c.O_EXCL | c.O_CLOEXEC | c.O_NOFOLLOW, @as(c_uint, 0o600));
    if (fd < 0) return error.IoError;
    defer _ = c.close(fd);
    var offset: usize = 0;
    while (offset < bytes.len) {
        const written = c.write(fd, bytes[offset..].ptr, bytes.len - offset);
        if (written <= 0) return error.IoError;
        offset += @intCast(written);
    }
    if (c.fsync(fd) != 0) return error.IoError;
}

test "public status, migrate, and remove drive one recorded artifact through its outcomes" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    if (c.mkdirat(temporary.dir.fd, "state", @as(c_uint, 0o700)) != 0) return error.IoError;
    try temporary.dir.makeDir("data");
    try temporary.dir.makeDir("data/applications");
    var state = try temporary.dir.openDir("state", .{});
    defer state.close();
    var data = try temporary.dir.openDir("data", .{});
    defer data.close();
    const data_path = try data.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(data_path);
    const roots = [_]manifest_store.root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try manifest_store.store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();

    var empty = try store.status(std.testing.allocator);
    defer empty.deinit();
    try std.testing.expectEqual(@as(usize, 0), empty.items.len);

    const path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/public.desktop", .{data_path});
    defer std.testing.allocator.free(path);
    const bytes = "public artifact\n";
    try data.writeFile(.{ .sub_path = "applications/public.desktop", .data = bytes });
    if (c.fchmodat(data.fd, "applications/public.desktop", 0o644, 0) != 0) return error.IoError;
    const hex = digest(bytes);
    const original = try document(std.testing.allocator, path, "desktop:public", &hex, "0.2.0");
    defer std.testing.allocator.free(original);
    try write_manifest(state, original);

    var listed = try store.status(std.testing.allocator);
    defer listed.deinit();
    try std.testing.expectEqual(@as(usize, 1), listed.items.len);
    try std.testing.expectEqualStrings("desktop:public", listed.items[0].record_id);
    try std.testing.expectEqual(manifest_store.record_state_t.Ok, listed.items[0].state);

    const migrated = try document(std.testing.allocator, path, "desktop:public", &hex, "0.3.0");
    defer std.testing.allocator.free(migrated);
    const migration = try store.migrate_record(.{ .record_id = "desktop:public", .candidate_manifest = migrated });
    try std.testing.expect(migration.changed);
    try expect_manifest_bytes(state, migrated);

    const emptied = "{\"schema\":1,\"waddle_version\":\"0.3.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[]}";
    const removal = try store.remove(.{ .record_id = "desktop:public", .candidate_manifest = emptied });
    try std.testing.expectEqual(manifest_store.remove_outcome_t.Removed, removal.outcome);
    try std.testing.expect(removal.record_found);
    try std.testing.expect(std.meta.activeTag(removal.prior) == .None);
    try std.testing.expectError(error.FileNotFound, data.access("applications/public.desktop", .{}));
    try expect_manifest_bytes(state, emptied);

    const repeat = try store.remove(.{ .record_id = "desktop:public", .candidate_manifest = "{\"schema\":1,\"waddle_version\":\"0.3.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[]}" });
    try std.testing.expectEqual(manifest_store.remove_outcome_t.AlreadyMissing, repeat.outcome);
    try std.testing.expect(!repeat.record_found);
}

test "public API exposes the removal, migration, and status declarations" {
    try std.testing.expect(@hasDecl(manifest_store.store_t, "status"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "remove"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "migrate_record"));
    try std.testing.expect(@hasDecl(manifest_store, "status_list_t"));
    try std.testing.expect(@hasDecl(manifest_store, "remove_receipt_t"));
    try std.testing.expect(@hasDecl(manifest_store, "migrate_receipt_t"));
    try std.testing.expect(@hasDecl(manifest_store, "prior_copy_t"));
}
