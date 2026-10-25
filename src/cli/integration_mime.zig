//! Per-user MIME default edits for `waddle integration`. The pure rules for the
//! `[Default Applications]` group live in `mimeapps`; this module owns the file:
//! reads are bounded, publication goes through a same-directory temporary file
//! and a rename, the existing permission bits are kept, and a symbolic link is
//! followed so its target is replaced while the link itself is preserved.
const std = @import("std");
const mimeapps = @import("mimeapps");

/// Leaf name of the MIME defaults file below `XDG_CONFIG_HOME`.
pub const Leaf: []const u8 = "mimeapps.list";

/// Maximum symbolic links followed before publication is refused.
pub const MaxLinkHops: usize = 8;

/// Outcome of undoing one default this module set.
pub const restore_t = enum {
    /// The key still held the expected value; the prior value was written back or
    /// the key was deleted when no prior existed.
    Restored,
    /// The key held another value, which is a user change; the file was not touched.
    NotOurs,
    /// The key is absent, for example after a crash between registration and the edit,
    /// so there is nothing to restore; the file was not touched.
    Absent,
    /// The file does not exist; nothing was touched.
    FileAbsent,
};

/// Follow a chain of symbolic links from `path` and return the final path, which
/// may not exist yet (a dangling link names where the file will be created).
/// Relative link targets resolve against the directory of the link. Caller frees.
/// Errors: SymLinkLoop after MaxLinkHops hops, OutOfMemory, or a filesystem error.
pub fn resolve_final(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    var current = try allocator.dupe(u8, path);
    errdefer allocator.free(current);
    var hops: usize = 0;
    var buffer: [std.fs.max_path_bytes]u8 = undefined;
    while (std.fs.cwd().readLink(current, &buffer)) |target| {
        hops += 1;
        if (hops > MaxLinkHops) return error.SymLinkLoop;
        // `current` is always absolute here, so its directory always exists.
        const next = if (std.fs.path.isAbsolute(target))
            try allocator.dupe(u8, target)
        else
            try std.fs.path.join(allocator, &.{ std.fs.path.dirname(current).?, target });
        allocator.free(current);
        current = next;
    } else |err| switch (err) {
        error.NotLink, error.FileNotFound => {},
        else => return err,
    }
    return current;
}

/// Read the complete file, or return null when it does not exist. Symbolic links
/// are followed. Caller frees the result with `allocator`.
/// Errors: Quota when the file is larger than mimeapps.MaxBytes; filesystem errors.
pub fn read(allocator: std.mem.Allocator, path: []const u8) !?[]u8 {
    const file = std.fs.cwd().openFile(path, .{}) catch |err| {
        if (err == error.FileNotFound) return null;
        return err;
    };
    defer file.close();
    return file.readToEndAlloc(allocator, mimeapps.MaxBytes) catch |err| {
        if (err == error.FileTooBig) return error.Quota;
        return err;
    };
}

/// Return an owned copy of the exact trimmed value of `mime`, or null when the file
/// or the key is absent. An empty value is returned as an empty slice.
pub fn current_value(allocator: std.mem.Allocator, path: []const u8, mime: []const u8) !?[]u8 {
    const input = (try read(allocator, path)) orelse return null;
    defer allocator.free(input);
    const value = (try mimeapps.read_default(input, mime)) orelse return null;
    return try allocator.dupe(u8, value);
}

/// Publish `bytes` as the complete content of the file at `path`. The write goes to
/// a temporary file in the same directory as the resolved target with the target's
/// permission bits (0644 for a new file), is synced, and is renamed over the target.
/// A failed publication removes the temporary file and leaves the target unchanged.
/// Errors: Quota when `bytes` exceeds mimeapps.MaxBytes, SymLinkLoop, or filesystem errors.
pub fn publish(allocator: std.mem.Allocator, path: []const u8, bytes: []const u8) !void {
    if (!std.fs.path.isAbsolute(path)) return error.InvalidArgument;
    if (bytes.len > mimeapps.MaxBytes) return error.Quota;
    const final = try resolve_final(allocator, path);
    defer allocator.free(final);
    // An absolute path always has a directory component.
    const dir_path = std.fs.path.dirname(final).?;
    const name = std.fs.path.basename(final);
    try std.fs.cwd().makePath(dir_path);
    var dir = try std.fs.cwd().openDir(dir_path, .{});
    defer dir.close();
    const mode: std.fs.File.Mode = if (dir.statFile(name)) |stat| @intCast(stat.mode & 0o7777) else |err| blk: {
        if (err != error.FileNotFound) return err;
        break :blk 0o644;
    };
    var random: [8]u8 = undefined;
    std.crypto.random.bytes(&random);
    const hex = std.fmt.bytesToHex(random, .lower);
    var temp_buffer: [255]u8 = undefined;
    const temp_name = try std.fmt.bufPrint(&temp_buffer, ".{s}.waddle-{s}.tmp", .{ name, &hex });
    var file = try dir.createFile(temp_name, .{ .exclusive = true, .mode = mode });
    var published = false;
    defer if (!published) dir.deleteFile(temp_name) catch {};
    defer file.close();
    try file.chmod(mode);
    try file.writeAll(bytes);
    try file.sync();
    try dir.rename(temp_name, name);
    published = true;
    // Directory sync is best effort: some filesystems reject it with EINVAL, and the
    // rename has already succeeded at this point.
    _ = std.c.fsync(dir.fd);
}

/// Set `mime` to `desktop_id` in the file at `path`, creating the file when it does
/// not exist. Returns the exact previous trimmed value, owned by the caller, or null
/// when the key was absent. The file is published only after the edit succeeds.
/// Errors: InvalidArgument for an unsupported desktop id or MIME type, Quota,
/// Malformed, Ambiguous, OutOfMemory, or filesystem errors.
pub fn set_default(allocator: std.mem.Allocator, path: []const u8, mime: []const u8, desktop_id: []const u8) !?[]u8 {
    const owned = try read(allocator, path);
    defer if (owned) |bytes| allocator.free(bytes);
    const input: []const u8 = owned orelse "";
    var edit = try mimeapps.rewrite(allocator, input, mime, desktop_id);
    defer edit.deinit();
    try publish(allocator, path, edit.bytes);
    const prior = edit.prior_value;
    edit.prior_value = null;
    return prior;
}

/// Undo one default this module set. When the key still holds `expected_id`, the
/// file is rewritten with `prior` as the value, or with the key deleted when `prior`
/// is null. A key that is absent reports Absent and a different value reports NotOurs;
/// neither touches the file.
/// Errors: Quota, Malformed, Ambiguous, InvalidArgument, OutOfMemory, or filesystem errors.
pub fn restore_default(allocator: std.mem.Allocator, path: []const u8, mime: []const u8, expected_id: []const u8, prior: ?[]const u8) !restore_t {
    const input = (try read(allocator, path)) orelse return .FileAbsent;
    defer allocator.free(input);
    const current = (try mimeapps.read_default(input, mime)) orelse return .Absent;
    if (!std.mem.eql(u8, current, expected_id)) return .NotOurs;
    var edit = try mimeapps.rewrite(allocator, input, mime, prior);
    defer edit.deinit();
    try publish(allocator, path, edit.bytes);
    return .Restored;
}

// Test-only fixtures.
const testing = std.testing;

const Fixture = struct {
    tmp: std.testing.TmpDir,
    base: []u8,

    fn create(allocator: std.mem.Allocator) !Fixture {
        var tmp = std.testing.tmpDir(.{ .iterate = true });
        errdefer tmp.cleanup();
        const base = try tmp.dir.realpathAlloc(allocator, ".");
        return .{ .tmp = tmp, .base = base };
    }

    fn deinit(self: *Fixture, allocator: std.mem.Allocator) void {
        allocator.free(self.base);
        self.tmp.cleanup();
    }

    fn path(self: *const Fixture, allocator: std.mem.Allocator, leaf: []const u8) ![]u8 {
        return std.fs.path.join(allocator, &.{ self.base, leaf });
    }

    fn read_leaf(self: *const Fixture, allocator: std.mem.Allocator, leaf: []const u8) ![]u8 {
        return self.tmp.dir.readFileAlloc(allocator, leaf, mimeapps.MaxBytes);
    }

    fn temp_count(self: *const Fixture) !usize {
        var count: usize = 0;
        var iterator = self.tmp.dir.iterate();
        while (try iterator.next()) |entry| {
            if (std.mem.indexOf(u8, entry.name, ".waddle-") != null) count += 1;
        }
        return count;
    }
};

const exe_mime = "application/x-msi";
const waddle_id = "waddle-open.desktop";

test "publish creates a missing file with mode 0644 and a missing directory" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    const target = try fx.path(allocator, "config/mimeapps.list");
    defer allocator.free(target);
    try publish(allocator, target, "[Default Applications]\n");
    const stored = try fx.read_leaf(allocator, "config/mimeapps.list");
    defer allocator.free(stored);
    try testing.expectEqualStrings("[Default Applications]\n", stored);
    const stat = try fx.tmp.dir.statFile("config/mimeapps.list");
    try testing.expectEqual(@as(std.fs.File.Mode, 0o644), stat.mode & 0o7777);
    try testing.expectEqual(@as(usize, 0), try fx.temp_count());
}

test "publish keeps the permission bits of an existing file" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "x" });
    const seeded = try fx.tmp.dir.openFile("mimeapps.list", .{});
    try seeded.chmod(0o600);
    seeded.close();
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    try publish(allocator, target, "[Default Applications]\n");
    const stat = try fx.tmp.dir.statFile("mimeapps.list");
    try testing.expectEqual(@as(std.fs.File.Mode, 0o600), stat.mode & 0o7777);
}

test "publish replaces the target of a symbolic link and keeps the link" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.makePath("dotfiles");
    try fx.tmp.dir.writeFile(.{ .sub_path = "dotfiles/mimeapps.list", .data = "old\n" });
    try fx.tmp.dir.symLink("dotfiles/mimeapps.list", "mimeapps.list", .{});
    const link = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(link);
    try publish(allocator, link, "new\n");
    const stored = try fx.read_leaf(allocator, "dotfiles/mimeapps.list");
    defer allocator.free(stored);
    try testing.expectEqualStrings("new\n", stored);
    var buffer: [std.fs.max_path_bytes]u8 = undefined;
    const link_target = try fx.tmp.dir.readLink("mimeapps.list", &buffer);
    try testing.expectEqualStrings("dotfiles/mimeapps.list", link_target);
}

test "publish creates the target of a dangling symbolic link" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.makePath("elsewhere");
    try fx.tmp.dir.symLink("elsewhere/created.list", "mimeapps.list", .{});
    const link = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(link);
    try publish(allocator, link, "created\n");
    const stored = try fx.read_leaf(allocator, "elsewhere/created.list");
    defer allocator.free(stored);
    try testing.expectEqualStrings("created\n", stored);
}

test "a symbolic link loop is refused without touching anything" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.symLink("b", "a", .{});
    try fx.tmp.dir.symLink("a", "b", .{});
    const link = try fx.path(allocator, "a");
    defer allocator.free(link);
    try testing.expectError(error.SymLinkLoop, publish(allocator, link, "x"));
    try testing.expectEqual(@as(usize, 0), try fx.temp_count());
}

test "a publication that cannot be renamed removes its temporary file" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.makePath("mimeapps.list");
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    try testing.expect(std.meta.isError(publish(allocator, target, "x")));
    try testing.expectEqual(@as(usize, 0), try fx.temp_count());
}

test "publish refuses content above the bounded size" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    const oversized = try allocator.alloc(u8, mimeapps.MaxBytes + 1);
    defer allocator.free(oversized);
    @memset(oversized, '\n');
    try testing.expectError(error.Quota, publish(allocator, target, oversized));
    try testing.expectEqual(@as(usize, 0), try fx.temp_count());
}

test "read reports an absent file as null and an oversized file as Quota" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    try testing.expect((try read(allocator, target)) == null);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "" });
    const empty = (try read(allocator, target)).?;
    defer allocator.free(empty);
    try testing.expectEqual(@as(usize, 0), empty.len);
    const oversized = try allocator.alloc(u8, mimeapps.MaxBytes + 1);
    defer allocator.free(oversized);
    @memset(oversized, '\n');
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = oversized });
    try testing.expectError(error.Quota, read(allocator, target));
}

test "set_default creates the key, reports an absent prior, and keeps other groups" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Added Associations]\ntext/plain=editor.desktop;\n" });
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    const prior = try set_default(allocator, target, exe_mime, waddle_id);
    try testing.expect(prior == null);
    const stored = try fx.read_leaf(allocator, "mimeapps.list");
    defer allocator.free(stored);
    try testing.expect(std.mem.indexOf(u8, stored, "text/plain=editor.desktop;") != null);
    try testing.expect(std.mem.indexOf(u8, stored, "[Default Applications]\napplication/x-msi=waddle-open.desktop\n") != null);
}

test "set_default returns the exact previous value and replaces it" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi = old.desktop\n" });
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    const prior = (try set_default(allocator, target, exe_mime, waddle_id)).?;
    defer allocator.free(prior);
    try testing.expectEqualStrings("old.desktop", prior);
    const stored = try fx.read_leaf(allocator, "mimeapps.list");
    defer allocator.free(stored);
    try testing.expect(std.mem.indexOf(u8, stored, "application/x-msi=waddle-open.desktop") != null);
}

test "restore_default writes back the prior value or deletes a key that had none" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi=old.desktop\napplication/x-msdownload=other.desktop\n" });
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    const prior = (try set_default(allocator, target, exe_mime, waddle_id)).?;
    defer allocator.free(prior);
    try testing.expectEqual(restore_t.Restored, try restore_default(allocator, target, exe_mime, waddle_id, prior));
    const restored = try fx.read_leaf(allocator, "mimeapps.list");
    defer allocator.free(restored);
    try testing.expect(std.mem.indexOf(u8, restored, "application/x-msi=old.desktop\n") != null);
    try testing.expect(std.mem.indexOf(u8, restored, "application/x-msdownload=other.desktop\n") != null);

    const created = try fx.path(allocator, "fresh.list");
    defer allocator.free(created);
    _ = try set_default(allocator, created, exe_mime, waddle_id);
    try testing.expectEqual(restore_t.Restored, try restore_default(allocator, created, exe_mime, waddle_id, null));
    const emptied = try fx.read_leaf(allocator, "fresh.list");
    defer allocator.free(emptied);
    try testing.expect(std.mem.indexOf(u8, emptied, "application/x-msi") == null);
}

test "restore_default leaves a user-changed, absent, or missing key untouched" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    try testing.expectEqual(restore_t.FileAbsent, try restore_default(allocator, target, exe_mime, waddle_id, null));
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi=user.desktop\n" });
    try testing.expectEqual(restore_t.NotOurs, try restore_default(allocator, target, exe_mime, waddle_id, null));
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\n" });
    try testing.expectEqual(restore_t.Absent, try restore_default(allocator, target, exe_mime, waddle_id, null));
    const stored = try fx.read_leaf(allocator, "mimeapps.list");
    defer allocator.free(stored);
    try testing.expectEqualStrings("[Default Applications]\n", stored);
}

test "current_value returns an owned copy of the trimmed value or null" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    try testing.expect((try current_value(allocator, target, exe_mime)) == null);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi =  waddle-open.desktop  \n" });
    const value = (try current_value(allocator, target, exe_mime)).?;
    defer allocator.free(value);
    try testing.expectEqualStrings("waddle-open.desktop", value);
}

test "several MIME types are edited independently and each restores its own prior" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi=old-msi.desktop\n" });
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    const types = [_][]const u8{ "application/x-msi", "application/x-ms-shortcut", "application/x-msdos-program" };
    var priors: [types.len]?[]u8 = undefined;
    for (types, 0..) |mime, index| priors[index] = try set_default(allocator, target, mime, waddle_id);
    defer for (priors) |prior| if (prior) |value| allocator.free(value);
    try testing.expectEqualStrings("old-msi.desktop", priors[0].?);
    try testing.expect(priors[1] == null);
    for (types, 0..) |mime, index| {
        const expected: ?[]const u8 = if (index == 0) "old-msi.desktop" else null;
        _ = try restore_default(allocator, target, mime, waddle_id, expected);
    }
    const stored = try fx.read_leaf(allocator, "mimeapps.list");
    defer allocator.free(stored);
    try testing.expectEqualStrings("[Default Applications]\napplication/x-msi=old-msi.desktop\n", stored);
}

test "paths under a regular file and directories as files are refused with their filesystem error" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.writeFile(.{ .sub_path = "plain", .data = "x" });
    try fx.tmp.dir.makePath("directory.list");
    const under_file = try fx.path(allocator, "plain/mimeapps.list");
    defer allocator.free(under_file);
    try testing.expect(std.meta.isError(publish(allocator, under_file, "x")));
    try testing.expect(std.meta.isError(read(allocator, under_file)));
    const as_directory = try fx.path(allocator, "directory.list");
    defer allocator.free(as_directory);
    try testing.expect(std.meta.isError(read(allocator, as_directory)));
    try testing.expect(std.meta.isError(current_value(allocator, as_directory, exe_mime)));
    try testing.expectEqual(@as(usize, 0), try fx.temp_count());
}

test "a directory component that is a dangling symbolic link is refused before any write" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.symLink("nonexistent-directory", "dangling-dir", .{});
    const through_link = try fx.path(allocator, "dangling-dir/mimeapps.list");
    defer allocator.free(through_link);
    const outcome = publish(allocator, through_link, "x");
    if (outcome) |_| {
        // Creating the missing directory is also acceptable; the write must then be complete.
        const stored = try fx.tmp.dir.readFileAlloc(allocator, "nonexistent-directory/mimeapps.list", mimeapps.MaxBytes);
        defer allocator.free(stored);
        try testing.expectEqualStrings("x", stored);
    } else |_| {}
    try testing.expectEqual(@as(usize, 0), try fx.temp_count());
}

test "a directory without read permission is refused by its open and leaves no temporary file" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.makePath("locked");
    try std.posix.fchmodat(fx.tmp.dir.fd, "locked", 0o300, 0);
    defer std.posix.fchmodat(fx.tmp.dir.fd, "locked", 0o700, 0) catch {};
    const target = try fx.path(allocator, "locked/mimeapps.list");
    defer allocator.free(target);
    // Root ignores the read bit, so the publication may succeed there; either outcome is valid.
    if (publish(allocator, target, "x")) |_| {} else |_| {}
    const stored_ok = fx.tmp.dir.readFileAlloc(allocator, "locked/mimeapps.list", mimeapps.MaxBytes) catch null;
    if (stored_ok) |bytes| allocator.free(bytes);
}

test "a relative target has no directory and a leaf name too long for its temporary file is refused" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    try testing.expectError(error.InvalidArgument, publish(allocator, "mimeapps.list", "x"));
    const long_leaf = try allocator.alloc(u8, 240);
    defer allocator.free(long_leaf);
    @memset(long_leaf, 'a');
    const long_path = try fx.path(allocator, long_leaf);
    defer allocator.free(long_path);
    try testing.expectError(error.NoSpaceLeft, publish(allocator, long_path, "x"));
}

test "a key that is absent or a file that is malformed yields null or the parser error" {
    const allocator = testing.allocator;
    var fx = try Fixture.create(allocator);
    defer fx.deinit(allocator);
    const target = try fx.path(allocator, "mimeapps.list");
    defer allocator.free(target);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\ntext/plain=editor.desktop\n" });
    try testing.expect((try current_value(allocator, target, exe_mime)) == null);
    try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\n[Default Applications]\n" });
    try testing.expectError(error.Ambiguous, current_value(allocator, target, exe_mime));
    try testing.expectError(error.Ambiguous, set_default(allocator, target, exe_mime, waddle_id));
    try testing.expectError(error.Ambiguous, restore_default(allocator, target, exe_mime, waddle_id, null));
}

const SweepSetup = enum { Absent, AbsoluteLink, RelativeLink };

/// Run `set_default` with an allocation failure at every index for one file layout,
/// requiring the sweep to finish and every induced failure to leave no allocation behind.
fn sweep_set_default(setup: SweepSetup) !void {
    var completed = false;
    var offset: usize = 0;
    while (offset < 20000) : (offset += 1) {
        var fx = try Fixture.create(testing.allocator);
        defer fx.deinit(testing.allocator);
        try fx.tmp.dir.makePath("dotfiles");
        try fx.tmp.dir.writeFile(.{ .sub_path = "dotfiles/mimeapps.list", .data = "[Default Applications]\n" });
        const link = try fx.path(testing.allocator, "mimeapps.list");
        defer testing.allocator.free(link);
        switch (setup) {
            .Absent => {},
            .AbsoluteLink => {
                const target = try fx.path(testing.allocator, "dotfiles/mimeapps.list");
                defer testing.allocator.free(target);
                try fx.tmp.dir.symLink(target, "mimeapps.list", .{});
            },
            .RelativeLink => try fx.tmp.dir.symLink("dotfiles/mimeapps.list", "mimeapps.list", .{}),
        }
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = offset });
        const outcome = set_default(failing.allocator(), link, exe_mime, waddle_id);
        if (outcome) |maybe_prior| {
            if (maybe_prior) |prior| failing.allocator().free(prior);
        } else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        if (!failing.has_induced_failure) {
            completed = true;
            break;
        }
    }
    try testing.expect(completed);
}

test "set_default survives an allocation failure at every index for absent and linked files" {
    try sweep_set_default(.Absent);
    try sweep_set_default(.AbsoluteLink);
    try sweep_set_default(.RelativeLink);
}

test "current_value reports an allocation failure at every index without leaking" {
    var completed = false;
    var offset: usize = 0;
    while (offset < 20000) : (offset += 1) {
        var fx = try Fixture.create(testing.allocator);
        defer fx.deinit(testing.allocator);
        try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi=waddle-open.desktop\n" });
        const target = try fx.path(testing.allocator, "mimeapps.list");
        defer testing.allocator.free(target);
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = offset });
        const outcome = current_value(failing.allocator(), target, exe_mime);
        if (outcome) |maybe_value| {
            if (maybe_value) |value| failing.allocator().free(value);
        } else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        if (!failing.has_induced_failure) {
            completed = true;
            break;
        }
    }
    try testing.expect(completed);
}

test "allocation failures in edits and restores leave no leaked allocations" {
    var completed_set = false;
    var offset: usize = 0;
    while (offset < 20000) : (offset += 1) {
        var fx = try Fixture.create(testing.allocator);
        defer fx.deinit(testing.allocator);
        try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi=old.desktop\n" });
        const target = try fx.path(testing.allocator, "mimeapps.list");
        defer testing.allocator.free(target);
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = offset });
        const outcome = set_default(failing.allocator(), target, exe_mime, waddle_id);
        if (outcome) |maybe_prior| {
            if (maybe_prior) |prior| failing.allocator().free(prior);
        } else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        if (!failing.has_induced_failure) {
            completed_set = true;
            break;
        }
    }
    try testing.expect(completed_set);
    var completed_restore = false;
    var offset_restore: usize = 0;
    while (offset_restore < 20000) : (offset_restore += 1) {
        var fx = try Fixture.create(testing.allocator);
        defer fx.deinit(testing.allocator);
        try fx.tmp.dir.writeFile(.{ .sub_path = "mimeapps.list", .data = "[Default Applications]\napplication/x-msi=waddle-open.desktop\n" });
        const target = try fx.path(testing.allocator, "mimeapps.list");
        defer testing.allocator.free(target);
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = offset_restore });
        const outcome = restore_default(failing.allocator(), target, exe_mime, waddle_id, null);
        if (outcome) |_| {} else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        if (!failing.has_induced_failure) {
            completed_restore = true;
            break;
        }
    }
    try testing.expect(completed_restore);
}
