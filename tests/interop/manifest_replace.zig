//! Public API integration tests for replacement, backup inventory, and restore.
const std = @import("std");
const manifest_store = @import("manifest_store");
const c = @cImport({
    @cInclude("fcntl.h");
    @cInclude("sys/stat.h");
    @cInclude("sys/types.h");
    @cInclude("unistd.h");
});

fn digest(bytes: []const u8) [32]u8 {
    var result: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &result, .{});
    return result;
}

fn manifest_bytes(allocator: std.mem.Allocator, path: []const u8, id: []const u8, bytes: []const u8) ![]u8 {
    const hex = std.fmt.bytesToHex(digest(bytes), .lower);
    return std.fmt.allocPrint(allocator, "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T00:00:00Z\",\"records\":[{{\"id\":\"{s}\",\"kind\":\"desktop_entry\",\"path\":\"{s}\",\"sha256\":\"{s}\",\"created_by\":\"0.2.0\",\"prior\":null}}]}}", .{ id, path, hex[0..] });
}

test "public store flow commits, inspects, replaces, inventories, restores, and recovers" {
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
    const target_path = try std.fmt.allocPrint(std.testing.allocator, "{s}/applications/entry.desktop", .{data_path});
    defer std.testing.allocator.free(target_path);

    const initial = "managed bytes";
    const changed = "local edit to preserve";
    const replacement = "repaired bytes";
    const initial_manifest = try manifest_bytes(std.testing.allocator, target_path, "desktop:test", initial);
    defer std.testing.allocator.free(initial_manifest);
    const replacement_manifest = try manifest_bytes(std.testing.allocator, target_path, "desktop:test", replacement);
    defer std.testing.allocator.free(replacement_manifest);
    const restore_manifest = try manifest_bytes(std.testing.allocator, target_path, "desktop:test", changed);
    defer std.testing.allocator.free(restore_manifest);

    const roots = [_]manifest_store.root_t{.{ .kind = .Data, .canonical_path = data_path, .dir_fd = data.fd }};
    var store = try manifest_store.store_t.init(std.testing.allocator, state.fd, &roots);
    defer store.deinit();
    try store.commit(.{
        .record_id = "desktop:test",
        .prior = .Absent,
        .artifact_bytes = initial,
        .candidate_manifest = initial_manifest,
    });

    const target_fd = c.openat(data.fd, "applications/entry.desktop", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (target_fd < 0) return error.IoError;
    defer _ = c.close(target_fd);
    if (c.ftruncate(target_fd, 0) != 0) return error.IoError;
    var written: usize = 0;
    while (written < changed.len) {
        const amount = c.write(target_fd, changed[written..].ptr, changed.len - written);
        if (amount <= 0) return error.IoError;
        written += @intCast(amount);
    }
    if (c.fsync(target_fd) != 0) return error.IoError;
    if (c.fchmod(target_fd, 0o755) != 0) return error.IoError;

    const snapshot = try store.inspect_record("desktop:test");
    try std.testing.expectEqual(digest(initial_manifest), snapshot.manifest_sha256);
    try std.testing.expectEqual(@as(u32, 0o755), snapshot.target.mode);
    const replaced = try store.replace(.{
        .record_id = "desktop:test",
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = replacement,
        .candidate_manifest = replacement_manifest,
        .backup_modified = true,
    });
    const backup = replaced.backup orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(@as(u32, 0o755), backup.artifact_mode);

    var inventory = try store.list_backups(std.testing.allocator, "desktop:test");
    defer inventory.deinit();
    try std.testing.expectEqual(@as(usize, 1), inventory.items.len);
    try std.testing.expectEqual(backup, inventory.items[0]);
    var empty_inventory = try store.list_backups(std.testing.allocator, "desktop:other");
    defer empty_inventory.deinit();
    try std.testing.expectEqual(@as(usize, 0), empty_inventory.items.len);

    store.deinit();
    store = try manifest_store.store_t.init(std.testing.allocator, state.fd, &roots);
    var durable_inventory = try store.list_backups(std.testing.allocator, "desktop:test");
    defer durable_inventory.deinit();
    try std.testing.expectEqual(@as(usize, 1), durable_inventory.items.len);
    try std.testing.expectEqual(backup, durable_inventory.items[0]);

    const current = try store.inspect_record("desktop:test");
    const restored = try store.restore(.{
        .record_id = "desktop:test",
        .expected_manifest_sha256 = current.manifest_sha256,
        .expected_target = current.target,
        .backup = backup,
        .candidate_manifest = restore_manifest,
    });
    try std.testing.expectEqual(@as(u32, 0o755), restored.target.mode);
    try store.recover();
    try store.recover();
    var retained = try store.list_backups(std.testing.allocator, "desktop:test");
    defer retained.deinit();
    try std.testing.expectEqual(@as(usize, 1), retained.items.len);
    try std.testing.expectEqual(backup, retained.items[0]);
    const final_bytes = try data.readFileAlloc(std.testing.allocator, "applications/entry.desktop", 4096);
    defer std.testing.allocator.free(final_bytes);
    try std.testing.expectEqualStrings(changed, final_bytes);

    var backup_root = try state.openDir("backups", .{});
    defer backup_root.close();
    var backup_dir = try backup_root.openDir(&backup.transaction_id, .{});
    defer backup_dir.close();
    const metadata_fd = c.openat(backup_dir.fd, "metadata.bin", c.O_WRONLY | c.O_CLOEXEC | c.O_NOFOLLOW);
    if (metadata_fd < 0) return error.IoError;
    defer _ = c.close(metadata_fd);
    const corrupt_magic = [_]u8{'X'};
    if (c.pwrite(metadata_fd, &corrupt_magic, 1, 0) != 1 or c.fsync(metadata_fd) != 0) return error.IoError;
    try std.testing.expectError(error.RecoveryRequired, store.list_backups(std.testing.allocator, "desktop:test"));
}

test "replacement public symbols expose by-value snapshots and caller-owned inventory" {
    try std.testing.expect(@hasDecl(manifest_store.store_t, "replace"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "restore"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "inspect_record"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "list_backups"));
    try std.testing.expect(@hasDecl(manifest_store, "replace_request_t"));
    try std.testing.expect(@hasDecl(manifest_store, "restore_request_t"));
    try std.testing.expect(@hasDecl(manifest_store, "replace_receipt_t"));
    try std.testing.expect(@hasDecl(manifest_store, "restore_receipt_t"));
    try std.testing.expect(@hasDecl(manifest_store, "artifact_identity_t"));
    try std.testing.expect(@hasDecl(manifest_store, "record_snapshot_t"));
    try std.testing.expect(@hasDecl(manifest_store, "backup_ref_t"));
    try std.testing.expect(@hasDecl(manifest_store, "backup_list_t"));
    try std.testing.expectEqual(@as(usize, 32), @sizeOf(@TypeOf(@as(manifest_store.backup_ref_t, undefined).sha256)));
    try std.testing.expectEqual(@as(usize, 32), @sizeOf(@TypeOf(@as(manifest_store.backup_ref_t, undefined).record_id_sha256)));
}
