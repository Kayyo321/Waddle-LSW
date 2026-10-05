//! Offline storage transactions. Zig owns bounded metadata and temporary memory;
//! C owns registry/runtime locks. All exported calls retain no borrowed pointers.
const std = @import("std");
const c = @cImport({
    @cInclude("daemon_device.h");
    @cInclude("daemon_config.h");
    @cInclude("errno.h");
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
    @cInclude("sys/stat.h");
});
/// Portable settings; host exports and identity are deliberately absent.
const settings_t = struct {
    memory_mb: u32 = 4096,
    vcpus: u32 = 4,
    default_shell: []const u8 = "powershell.exe",
    vsock_port: u32 = 5242,
    start_timeout: u32 = 60,
    stop_timeout: u32 = 15,
};
/// Backup schema, owned by one bounded parse arena.
const manifest_t = struct {
    schema_version: u32,
    source_name: []const u8,
    disk_file: []const u8,
    disk_size_bytes: u64,
    disk_sha256: []const u8,
    config: settings_t,
};
/// Root-relative reference; external roots include durable parent inode identity.
const location_t = struct { root: []const u8, path: []const u8 };
/// A reversible rename with the exact moved object's identity.
const move_t = struct { old: location_t, final: location_t, device: u64, inode: u64 };
/// Durable intent. New objects live only in unpredictable private staging trees.
const journal_t = struct {
    schema_version: u32 = 1,
    transaction_id: []const u8,
    operation: []const u8,
    phase: []const u8 = "prepared",
    source_name: ?[]const u8,
    destination_name: ?[]const u8,
    moves: []const move_t = &.{},
    replacements: []const move_t = &.{},
    external_parent: ?[]const u8 = null,
    external_device: u64 = 0,
    external_inode: u64 = 0,
};
/// One command arena owns every allocated path and parsed string through return.
const context_t = struct {
    allocator: std.mem.Allocator,
    config: []const u8,
    state: []const u8,
    external: ?[]const u8 = null,

    fn join(self: context_t, root: []const u8, tail: []const u8) ![]const u8 {
        const result = try std.fmt.allocPrint(self.allocator, "{s}/{s}", .{ root, tail });
        if (result.len >= 1024) return error.InvalidConfig;
        return result;
    }
    fn resolve(self: context_t, location: location_t) ![]const u8 {
        try relative_valid(location.path);
        const root = if (std.mem.eql(u8, location.root, "config")) self.config else if (std.mem.eql(u8, location.root, "state")) self.state else if (std.mem.eql(u8, location.root, "external")) self.external orelse return error.InvalidConfig else return error.InvalidConfig;
        return self.join(root, location.path);
    }
};
/// Reject escape paths, NULs and ambiguous path components before any I/O.
fn relative_valid(path: []const u8) !void {
    if (path.len == 0 or path[0] == '/' or path.len >= 1024 or std.mem.indexOfScalar(u8, path, 0) != null) return error.InvalidConfig;
    var parts = std.mem.splitScalar(u8, path, '/');
    while (parts.next()) |part| if (part.len == 0 or std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return error.InvalidConfig;
}
fn name_valid(name: []const u8) bool {
    if (name.len == 0 or name.len >= 64) return false;
    for (name) |ch| if (!std.ascii.isAlphanumeric(ch) and ch != '-' and ch != '_') return false;
    return true;
}
fn text(buffer: anytype) []const u8 {
    const bytes = buffer[0..];
    return bytes[0 .. std.mem.indexOfScalar(u8, bytes, 0) orelse bytes.len];
}
fn native_error() anyerror {
    return switch (c.__errno_location().*) {
        c.ENOENT => error.FileNotFound,
        c.EEXIST => error.PathAlreadyExists,
        c.EBUSY => error.Busy,
        c.EACCES, c.EPERM, c.ELOOP => error.AccessDenied,
        c.ENOSPC => error.NoSpaceLeft,
        c.EINTR => error.Interrupted,
        c.EUCLEAN => error.RecoveryRequired,
        else => error.InvalidConfig,
    };
}
fn context(allocator: std.mem.Allocator) !context_t {
    var cfg: [1024]u8 = undefined;
    var state: [1024]u8 = undefined;
    if (c.daemon_device_get_config_dir(&cfg, cfg.len) != 0 or c.daemon_device_get_state_dir("probe", &state, state.len) != 0) return native_error();
    const cfg_text = text(&cfg);
    const state_text = text(&state);
    const state_parent = std.fs.path.dirname(state_text).?;
    return .{ .allocator = allocator, .config = try allocator.dupe(u8, std.fs.path.dirname(cfg_text).?), .state = try allocator.dupe(u8, std.fs.path.dirname(state_parent).?) };
}
/// Open a directory by no-follow traversal; verify private owned roots when requested.
fn directory(ctx: context_t, path: []const u8, create: bool, private: bool) !std.fs.Dir {
    const terminated = try ctx.allocator.dupeZ(u8, path);
    const fd = c.daemon_device_open_directory(terminated, @intFromBool(create));
    if (fd < 0) return native_error();
    var dir = std.fs.Dir{ .fd = fd };
    errdefer dir.close();
    var st: c.struct_stat = undefined;
    if (c.fstat(fd, &st) != 0) return native_error();
    if (private and (st.st_uid != c.getuid() or st.st_mode & 0o077 != 0)) return error.AccessDenied;
    return dir;
}
/// Open a regular file without following the final symlink; caller closes.
fn regular(ctx: context_t, path: []const u8, private: bool) !std.fs.File {
    var parent = try directory(ctx, std.fs.path.dirname(path) orelse return error.InvalidConfig, false, false);
    defer parent.close();
    const leaf = try ctx.allocator.dupeZ(u8, std.fs.path.basename(path));
    const fd = c.openat(parent.fd, leaf, c.O_RDONLY | c.O_NOFOLLOW | c.O_CLOEXEC | c.O_NONBLOCK);
    if (fd < 0) return native_error();
    var file = std.fs.File{ .handle = fd };
    errdefer file.close();
    var st: c.struct_stat = undefined;
    if (c.fstat(fd, &st) != 0) return native_error();
    if (st.st_mode & c.S_IFMT != c.S_IFREG or (private and (st.st_uid != c.getuid() or st.st_mode & 0o077 != 0 or st.st_nlink != 1))) return error.AccessDenied;
    return file;
}
fn read_metadata(ctx: context_t, path: []const u8) ![]const u8 {
    var file = try regular(ctx, path, true);
    defer file.close();
    const data = try file.readToEndAlloc(ctx.allocator, 65536);
    if (!std.unicode.utf8ValidateSlice(data) or std.mem.indexOfScalar(u8, data, 0) != null) return error.InvalidConfig;
    return data;
}
/// Reject nesting beyond eight levels before the typed parser allocates structures.
fn parse_json(comptime T: type, ctx: context_t, bytes: []const u8) !T {
    var scanner = std.json.Scanner.initCompleteInput(ctx.allocator, bytes);
    defer scanner.deinit();
    var depth: usize = 0;
    while (true) {
        const token = try scanner.next();
        switch (token) {
            .object_begin, .array_begin => { depth += 1; if (depth > 8) return error.InvalidConfig; },
            .object_end, .array_end => depth -= 1,
            .end_of_document => break,
            else => {},
        }
    }
    return (try std.json.parseFromSlice(T, ctx.allocator, bytes, .{ .allocate = .alloc_always, .ignore_unknown_fields = false, .duplicate_field_behavior = .@"error" })).value;
}
fn create_file(ctx: context_t, path: []const u8, bytes: []const u8) !void {
    var parent = try directory(ctx, std.fs.path.dirname(path).?, false, true);
    defer parent.close();
    var file = try parent.createFile(std.fs.path.basename(path), .{ .exclusive = true, .mode = 0o600 });
    defer file.close();
    try file.writeAll(bytes);
    try file.sync();
    try std.posix.fsync(parent.fd);
}
fn absent(ctx: context_t, path: []const u8) !void {
    var parent = directory(ctx, std.fs.path.dirname(path).?, false, false) catch |err| {
        if (err == error.FileNotFound) return;
        return err;
    };
    defer parent.close();
    const leaf = try ctx.allocator.dupeZ(u8, std.fs.path.basename(path));
    var st: c.struct_stat = undefined;
    if (c.fstatat(parent.fd, leaf, &st, c.AT_SYMLINK_NOFOLLOW) == 0) return error.PathAlreadyExists;
    if (c.__errno_location().* != c.ENOENT) return native_error();
}
/// Capture no-follow identity for recovery; caller owns all reference strings.
fn identity(ctx: context_t, path: []const u8) !c.struct_stat {
    var parent = try directory(ctx, std.fs.path.dirname(path).?, false, false);
    defer parent.close();
    const leaf = try ctx.allocator.dupeZ(u8, std.fs.path.basename(path));
    var st: c.struct_stat = undefined;
    if (c.fstatat(parent.fd, leaf, &st, c.AT_SYMLINK_NOFOLLOW) != 0) return native_error();
    if (st.st_uid != c.getuid() or st.st_mode & 0o077 != 0 or (st.st_mode & c.S_IFMT != c.S_IFREG and st.st_mode & c.S_IFMT != c.S_IFDIR)) return error.AccessDenied;
    return st;
}
fn rename_path(ctx: context_t, old: []const u8, final: []const u8) !void {
    var source = try directory(ctx, std.fs.path.dirname(old).?, false, true);
    defer source.close();
    var dest = try directory(ctx, std.fs.path.dirname(final).?, false, true);
    defer dest.close();
    try std.posix.renameat(source.fd, std.fs.path.basename(old), dest.fd, std.fs.path.basename(final));
    try std.posix.fsync(source.fd);
    try std.posix.fsync(dest.fd);
}
fn move(ctx: context_t, old: location_t, final: location_t) !move_t {
    const st = try identity(ctx, try ctx.resolve(old));
    return .{ .old = old, .final = final, .device = @intCast(st.st_dev), .inode = st.st_ino };
}
/// Verify exact object identity before moving forward/rolling back; idempotent.
fn apply_move(ctx: context_t, item: move_t, forward: bool) !void {
    const from = try ctx.resolve(if (forward) item.old else item.final);
    const to = try ctx.resolve(if (forward) item.final else item.old);
    if (identity(ctx, to)) |done| {
        if (done.st_dev == item.device and done.st_ino == item.inode) return;
    } else |err| if (err != error.FileNotFound) return err;
    const st = try identity(ctx, from);
    if (st.st_dev != item.device or st.st_ino != item.inode) return error.RecoveryRequired;
    try absent(ctx, to);
    try rename_path(ctx, from, to);
}
fn stage_path(ctx: context_t, root: []const u8, id: []const u8, file: []const u8) !location_t {
    return .{ .root = root, .path = try std.fmt.allocPrint(ctx.allocator, "transactions/{s}/{s}", .{ id, file }) };
}
fn journal_path(ctx: context_t, id: []const u8) ![]const u8 {
    return ctx.join(ctx.config, try std.fmt.allocPrint(ctx.allocator, "transactions/{s}.json", .{id}));
}
fn save_journal(ctx: context_t, record: journal_t, first: bool) !void {
    const bytes = try std.json.stringifyAlloc(ctx.allocator, record, .{});
    if (bytes.len > 65536) return error.InvalidConfig;
    const path = try journal_path(ctx, record.transaction_id);
    if (first) return create_file(ctx, path, bytes);
    const temporary = try std.fmt.allocPrint(ctx.allocator, "{s}.tmp", .{path});
    // A failed previous atomic phase write may leave only this owned temp file.
    var parent = try directory(ctx, std.fs.path.dirname(path).?, false, true);
    defer parent.close();
    parent.deleteFile(std.fs.path.basename(temporary)) catch |err| if (err != error.FileNotFound) return err;
    try create_file(ctx, temporary, bytes);
    try rename_path(ctx, temporary, path);
}
/// Delete only transaction-owned trees, without following child symlinks.
fn purge(ctx: context_t, root: []const u8, path: []const u8) !void {
    var parent = directory(ctx, root, false, true) catch |err| {
        if (err == error.FileNotFound) return;
        return err;
    };
    defer parent.close();
    try parent.deleteTree(path);
    try std.posix.fsync(parent.fd);
}
fn cleanup_journal(ctx: context_t, record: journal_t) !void {
    const staging = try std.fmt.allocPrint(ctx.allocator, "transactions/{s}", .{record.transaction_id});
    try purge(ctx, ctx.config, staging);
    try purge(ctx, ctx.state, staging);
    if (ctx.external) |root| try purge(ctx, root, try std.fmt.allocPrint(ctx.allocator, ".waddle-{s}", .{record.transaction_id}));
    var parent = try directory(ctx, try ctx.join(ctx.config, "transactions"), false, true);
    defer parent.close();
    const leaf = try std.fmt.allocPrint(ctx.allocator, "{s}.json", .{record.transaction_id});
    parent.deleteFile(try std.fmt.allocPrint(ctx.allocator, "{s}.tmp", .{leaf})) catch |err| if (err != error.FileNotFound) return err;
    try parent.deleteFile(leaf);
    try std.posix.fsync(parent.fd);
}
fn recover_one(initial: context_t, record: journal_t) !void {
    if (record.schema_version != 1 or record.transaction_id.len != 32 or record.moves.len > 16 or record.replacements.len != 0) return error.RecoveryRequired;
    for (record.transaction_id) |ch| if (!(ch >= '0' and ch <= '9') and !(ch >= 'a' and ch <= 'f')) return error.RecoveryRequired;
    if (record.source_name) |name| if (!name_valid(name)) return error.RecoveryRequired;
    if (record.destination_name) |name| if (!name_valid(name)) return error.RecoveryRequired;
    var ctx = initial;
    if (record.external_parent) |root| {
        var dir = try directory(ctx, root, false, true);
        defer dir.close();
        var st: c.struct_stat = undefined;
        if (c.fstat(dir.fd, &st) != 0 or st.st_dev != record.external_device or st.st_ino != record.external_inode) return error.RecoveryRequired;
        ctx.external = root;
    }
    // Validate every path before making any changes, including absent references.
    for (record.moves) |item| { _ = try ctx.resolve(item.old); _ = try ctx.resolve(item.final); }
    if (std.mem.eql(u8, record.phase, "prepared")) {
        var index = record.moves.len;
        while (index > 0) { index -= 1; try apply_move(ctx, record.moves[index], false); }
    } else if (!std.mem.eql(u8, record.phase, "committed")) return error.RecoveryRequired;
    try cleanup_journal(ctx, record);
}
fn recover(ctx: context_t, repair: bool) !void {
    var dir = directory(ctx, try ctx.join(ctx.config, "transactions"), false, true) catch |err| {
        if (err == error.FileNotFound) return;
        return err;
    };
    defer dir.close();
    var iterator_dir = try dir.openDir(".", .{ .iterate = true });
    defer iterator_dir.close();
    var iterator = iterator_dir.iterate();
    var paths = std.ArrayList([]const u8).init(ctx.allocator);
    while (try iterator.next()) |entry| {
        if (!std.mem.endsWith(u8, entry.name, ".json")) continue;
        if (!repair) return error.RecoveryRequired;
        if (paths.items.len >= 32) return error.RecoveryRequired;
        try paths.append(try ctx.allocator.dupe(u8, entry.name));
    }
    for (paths.items) |leaf| {
        const record = try parse_json(journal_t, ctx, try read_metadata(ctx, try ctx.join(try ctx.join(ctx.config, "transactions"), leaf)));
        const expected = try std.fmt.allocPrint(ctx.allocator, "{s}.json", .{record.transaction_id});
        if (!std.mem.eql(u8, leaf, expected)) return error.RecoveryRequired;
        try recover_one(ctx, record);
    }
}
fn set_errno(err: anyerror) void {
    c.__errno_location().* = switch (err) {
        error.FileNotFound => c.ENOENT,
        error.PathAlreadyExists => c.EEXIST,
        error.Busy => c.EBUSY,
        error.AccessDenied, error.SymLinkLoop => c.EACCES,
        error.NoSpaceLeft => c.ENOSPC,
        error.Interrupted => c.EINTR,
        error.RecoveryRequired => c.EUCLEAN,
        error.OutOfMemory => c.ENOMEM,
        error.InvalidConfig, error.SyntaxError, error.UnexpectedToken, error.DuplicateField, error.UnknownField, error.MissingField, error.Overflow, error.InvalidCharacter => c.EINVAL,
        else => c.EIO,
    };
}
/// C ABI: caller holds registry lock; all allocations freed on every return.
export fn daemon_device_recover(repair: c_int) c_int {
    var arena = std.heap.ArenaAllocator.init(std.heap.c_allocator);
    defer arena.deinit();
    const ctx = context(arena.allocator()) catch |err| { set_errno(err); return -1; };
    recover(ctx, repair != 0) catch |err| { set_errno(err); return -1; };
    return 0;
}
/// Run fixed argv without a shell, with bounded captured metadata and inherited stderr.
fn tool(ctx: context_t, argv: []const []const u8) ![]const u8 {
    const result = try std.process.Child.run(.{ .allocator = ctx.allocator, .argv = argv, .max_output_bytes = 65536 });
    switch (result.term) { .Exited => |status| if (status != 0) {
        std.io.getStdErr().writer().writeAll(result.stderr) catch {};
        return error.ToolFailed;
    }, else => return error.Interrupted }
    return result.stdout;
}
/// Validate qemu-img's trusted tool output via bounded JSON and every backing member.
fn image(ctx: context_t, path: []const u8, standalone: bool) !std.json.Value {
    var file = try regular(ctx, path, false);
    file.close();
    const bytes = try tool(ctx, &.{ "qemu-img", "info", "--output=json", "--backing-chain", path });
    if (bytes.len > 65536) return error.InvalidConfig;
    const value = try parse_json(std.json.Value, ctx, bytes);
    if (value != .array or value.array.items.len == 0 or value.array.items.len > 8) return error.InvalidConfig;
    if (standalone and value.array.items.len != 1) return error.InvalidConfig;
    for (value.array.items) |entry| {
        if (entry != .object) return error.InvalidConfig;
        const format = entry.object.get("format") orelse return error.InvalidConfig;
        if (format != .string or !std.mem.eql(u8, format.string, "qcow2")) return error.InvalidConfig;
        const filename = entry.object.get("filename") orelse return error.InvalidConfig;
        if (filename != .string) return error.InvalidConfig;
        var backing = try regular(ctx, filename.string, false);
        backing.close();
    }
    return value;
}
fn hash_disk(ctx: context_t, path: []const u8) !struct { size: u64, hash: [64]u8 } {
    var file = try regular(ctx, path, false);
    defer file.close();
    var hash = std.crypto.hash.sha2.Sha256.init(.{});
    var buffer: [65536]u8 = undefined;
    var size: u64 = 0;
    while (true) {
        const amount = try file.read(&buffer);
        if (amount == 0) break;
        hash.update(buffer[0..amount]);
        size = try std.math.add(u64, size, amount);
    }
    var digest: [32]u8 = undefined;
    hash.final(&digest);
    return .{ .size = size, .hash = std.fmt.bytesToHex(digest, .lower) };
}
fn settings(cfg: *const c.daemon_config_t) settings_t {
    return .{ .memory_mb = cfg.memory_mb, .vcpus = cfg.vcpus, .default_shell = text(&cfg.default_shell), .vsock_port = cfg.vsock_port, .start_timeout = cfg.start_timeout_sec, .stop_timeout = cfg.stop_timeout_sec };
}
fn settings_valid(value: settings_t) !void {
    if (value.memory_mb < 512 or value.memory_mb > 65536 or value.vcpus < 1 or value.vcpus > 128 or value.vsock_port == 0 or value.start_timeout < 1 or value.start_timeout > 600 or value.stop_timeout < 1 or value.stop_timeout > 300 or value.default_shell.len == 0 or value.default_shell.len > 255 or !std.unicode.utf8ValidateSlice(value.default_shell) or std.mem.indexOfAny(u8, value.default_shell, "\x00\r\n") != null) return error.InvalidConfig;
}
fn blank_size(value: []const u8) !u64 {
    if (value.len < 2) return error.InvalidConfig;
    const factor: u64 = switch (value[value.len - 1]) { 'M' => 1024 * 1024, 'G' => 1024 * 1024 * 1024, else => return error.InvalidConfig };
    for (value[0 .. value.len - 1]) |ch| if (!std.ascii.isDigit(ch)) return error.InvalidConfig;
    const number = try std.fmt.parseInt(u64, value[0 .. value.len - 1], 10);
    const bytes = try std.math.mul(u64, number, factor);
    if (bytes < 1024 * 1024 or bytes > 2 * 1024 * 1024 * 1024 * 1024) return error.InvalidConfig;
    return bytes;
}
fn load_config(ctx: context_t, info: *const c.device_info_t) !c.daemon_config_t {
    const bytes = try read_metadata(ctx, text(&info.config_path));
    var cfg: c.daemon_config_t = undefined;
    c.daemon_config_init_defaults(&cfg);
    cfg.vsock_cid = 0;
    cfg.disk_image[0] = 0;
    if (c.daemon_config_parse_string(&cfg, bytes.ptr, bytes.len) != 0 or cfg.disk_image[0] != '/') return error.InvalidConfig;
    return cfg;
}
/// Preserve every original line; identity changes append explicit last-wins fields.
fn profile(ctx: context_t, name: []const u8, disk: []const u8, cid: u32, value: settings_t, original: ?[]const u8, mounts: []const u8) ![]const u8 {
    try settings_valid(value);
    if (std.mem.indexOfAny(u8, disk, "\r\n\x00") != null) return error.InvalidConfig;
    const prefix = original orelse "";
    const bytes = try std.fmt.allocPrint(ctx.allocator,
        "{s}\n[subsystem]\nname = {s}\ndisk_image = {s}\nvsock_cid = {d}\nmemory_mb = {d}\nvcpus = {d}\nvsock_port = {d}\ndefault_shell = {s}\n[timeouts]\nstart_timeout = {d}\nstop_timeout = {d}\n{s}",
        .{ prefix, name, disk, cid, value.memory_mb, value.vcpus, value.vsock_port, value.default_shell, value.start_timeout, value.stop_timeout, mounts });
    if (bytes.len > 65536) return error.InvalidConfig;
    var candidate: c.daemon_config_t = undefined;
    c.daemon_config_init_defaults(&candidate);
    if (c.daemon_config_parse_string(&candidate, bytes.ptr, bytes.len) != 0) return error.InvalidConfig;
    return bytes;
}
fn selected_default(ctx: context_t) !?[]const u8 {
    const data = read_metadata(ctx, try ctx.join(ctx.config, "default_device")) catch |err| {
        if (err == error.FileNotFound) return null;
        return err;
    };
    if (data.len < 2 or data[data.len - 1] != '\n' or !name_valid(data[0 .. data.len - 1])) return error.InvalidConfig;
    return data[0 .. data.len - 1];
}
fn registration(ctx: context_t, name: []const u8) !location_t {
    return .{ .root = "config", .path = try std.fmt.allocPrint(ctx.allocator, "devices/{s}.ini", .{name}) };
}
fn state_location(ctx: context_t, name: []const u8) !location_t {
    return .{ .root = "state", .path = try std.fmt.allocPrint(ctx.allocator, "devices/{s}", .{name}) };
}
/// Refuse rename/removal if any registered chain depends on the managed disk.
fn dependencies(ctx: context_t, registry: *const c.device_list_t, source: *const c.device_info_t) !void {
    var file = try regular(ctx, text(&source.disk_image), true);
    const target = try file.stat();
    file.close();
    for (registry.devices[0..registry.count]) |*info| {
        if (std.mem.eql(u8, text(&info.name), text(&source.name))) continue;
        if (info.config_valid == 0) return error.InvalidConfig;
        const chain = try image(ctx, text(&info.disk_image), false);
        for (chain.array.items) |entry| {
            const path = entry.object.get("filename").?.string;
            var member = try regular(ctx, path, false);
            const st = try member.stat();
            member.close();
            if (st.inode == target.inode) {
                const first = chain.array.items[0].object.get("filename").?.string;
                if (std.mem.eql(u8, path, first) or chain.array.items.len > 1) return error.Busy;
            }
        }
    }
}
fn canonical(ctx: context_t, path: []const u8) ![]const u8 {
    if (std.mem.indexOfAny(u8, path, "\n\r\x00") != null) return error.InvalidConfig;
    return std.fs.cwd().realpathAlloc(ctx.allocator, path);
}
/// Build and execute one transaction; no name/CID reservation during dry-run.
fn mutate(ctx_initial: context_t, request: *const c.device_request_t, output: []u8) !void {
    output[0] = 0;
    if (request.name == null) return error.InvalidConfig;
    const name = std.mem.span(request.name);
    if (!name_valid(name)) return error.InvalidConfig;
    const arg = if (request.argument) |pointer| std.mem.span(pointer) else "";
    const operation = request.operation;
    const creates = operation == c.DeviceInit or operation == c.DeviceClone or operation == c.DeviceImport;
    const source_required = operation == c.DeviceRename or operation == c.DeviceRemove or operation == c.DeviceClone or operation == c.DeviceExport;
    const destination = if (operation == c.DeviceClone or operation == c.DeviceRename) arg else name;
    if ((creates or operation == c.DeviceRename) and !name_valid(destination)) return error.InvalidConfig;
    var ctx = ctx_initial;
    const lock = c.daemon_device_registry_lock(@intFromBool(request.dry_run == 0));
    if (lock < 0) {
        if (!(request.dry_run != 0 and c.__errno_location().* == c.ENOENT)) return native_error();
    }
    defer if (lock >= 0) { _ = c.close(lock); };
    try recover(ctx, request.dry_run == 0);
    var registry: c.device_list_t = undefined;
    if (c.daemon_device_scan_locked(&registry) < 0) return native_error();
    var source: ?*const c.device_info_t = null;
    for (registry.devices[0..registry.count]) |*info| {
        if (std.mem.eql(u8, name, text(&info.name))) source = info;
        if (creates and info.config_valid == 0) return error.InvalidConfig;
    }
    if (source_required and source == null) return error.FileNotFound;
    if (operation == c.DeviceRename and std.mem.eql(u8, name, destination)) {
        if (source.?.config_valid == 0) return error.InvalidConfig;
        return;
    }
    if (creates or operation == c.DeviceRename) {
        try absent(ctx, try ctx.resolve(try registration(ctx, destination)));
        try absent(ctx, try ctx.resolve(try state_location(ctx, destination)));
        if (creates and registry.count >= 32) return error.NoSpaceLeft;
    }
    var lease: c_int = -1;
    defer if (lease >= 0) { _ = c.close(lease); };
    var cfg: c.daemon_config_t = undefined;
    var value = settings_t{};
    var original: ?[]const u8 = null;
    if (source_required) {
        cfg = try load_config(ctx, source.?);
        if (c.daemon_device_lock_quiescent(source.?, &lease) != 0) return native_error();
        _ = try image(ctx, text(&source.?.disk_image), false);
        value = settings(&cfg);
        original = try read_metadata(ctx, text(&source.?.config_path));
        if (operation == c.DeviceRename or operation == c.DeviceRemove) {
            const owned = try ctx.join(try ctx.resolve(try state_location(ctx, name)), "disk.qcow2");
            if (!std.mem.eql(u8, owned, text(&source.?.disk_image))) return error.AccessDenied;
            try dependencies(ctx, &registry, source.?);
        }
    }
    var base: []const u8 = "";
    var backup_disk: []const u8 = "";
    if (operation == c.DeviceInit) {
        if (request.memory_mb != 0) value.memory_mb = request.memory_mb;
        if (request.vcpus != 0) value.vcpus = request.vcpus;
        if (request.shell) |shell| value.default_shell = std.mem.span(shell);
        try settings_valid(value);
        if (request.blank != 0) { _ = try blank_size(arg); } else {
            if (arg.len > 0) { base = try canonical(ctx, arg); } else {
                var found: [1024]u8 = undefined;
                if (c.daemon_device_find_base_disk(&found, found.len) != 0) return native_error();
                base = try canonical(ctx, text(&found));
            }
            _ = try image(ctx, base, false);
        }
    }
    if (operation == c.DeviceExport) {
        const absolute = if (std.fs.path.isAbsolute(arg)) arg else try ctx.join(try std.fs.cwd().realpathAlloc(ctx.allocator, "."), arg);
        const parent = try canonical(ctx, std.fs.path.dirname(absolute) orelse return error.InvalidConfig);
        var dir = try directory(ctx, parent, false, true);
        dir.close();
        ctx.external = parent;
        try absent(ctx, try ctx.join(parent, std.fs.path.basename(absolute)));
    }
    if (operation == c.DeviceImport) {
        const input = try canonical(ctx, arg);
        // Canonicalization cannot legitimize a symlink backup supplied by the caller.
        if (std.fs.path.isAbsolute(arg) and !std.mem.eql(u8, input, arg)) return error.AccessDenied;
        var dir = try directory(ctx, input, false, true);
        defer dir.close();
        var entries_dir = try dir.openDir(".", .{ .iterate = true });
        defer entries_dir.close();
        var iterator = entries_dir.iterate();
        var count: usize = 0;
        while (try iterator.next()) |entry| {
            if (entry.kind != .file or (!std.mem.eql(u8, entry.name, "manifest.json") and !std.mem.eql(u8, entry.name, "disk.qcow2"))) return error.InvalidConfig;
            count += 1;
        }
        if (count != 2) return error.InvalidConfig;
        const manifest = try parse_json(manifest_t, ctx, try read_metadata(ctx, try ctx.join(input, "manifest.json")));
        if (manifest.schema_version != 1 or !name_valid(manifest.source_name) or !std.mem.eql(u8, manifest.disk_file, "disk.qcow2") or manifest.disk_sha256.len != 64) return error.InvalidConfig;
        value = manifest.config;
        try settings_valid(value);
        backup_disk = try ctx.join(input, "disk.qcow2");
        _ = try image(ctx, backup_disk, true);
        const hash = try hash_disk(ctx, backup_disk);
        if (hash.size != manifest.disk_size_bytes or !std.mem.eql(u8, &hash.hash, manifest.disk_sha256)) return error.InvalidConfig;
    }
    const cid = if (creates) c.daemon_device_allocate_cid(&registry) else if (source) |info| info.vsock_cid else 0;
    if (creates and cid == 0) return native_error();
    if (request.dry_run != 0) return;
    // Verify the utility exists before intent; tools never inherit registry leases.
    if (creates or operation == c.DeviceExport) _ = try tool(ctx, &.{ "qemu-img", "--version" });
    var random: [16]u8 = undefined;
    std.crypto.random.bytes(&random);
    const id = try ctx.allocator.dupe(u8, &std.fmt.bytesToHex(random, .lower));
    var record = journal_t{ .transaction_id = id, .operation = switch (operation) { c.DeviceInit => "init", c.DeviceRename => "rename", c.DeviceRemove => "remove", c.DeviceClone => "clone", c.DeviceExport => "export", c.DeviceImport => "import", else => return error.InvalidConfig }, .source_name = if (source_required) name else null, .destination_name = if (creates or operation == c.DeviceRename) destination else null };
    if (ctx.external) |parent| {
        const st = try identity(ctx, parent);
        record.external_parent = parent;
        record.external_device = @intCast(st.st_dev);
        record.external_inode = st.st_ino;
    }
    for ([_][]const u8{ ctx.config, ctx.state }) |root| {
        var dir = try directory(ctx, try ctx.join(root, "transactions"), true, true);
        dir.close();
    }
    try save_journal(ctx, record, true);
    fault("prepared");
    errdefer recover_one(ctx, record) catch {};
    const staging = try std.fmt.allocPrint(ctx.allocator, "transactions/{s}", .{id});
    for ([_][]const u8{ ctx.config, ctx.state }) |root| {
        const path = try ctx.join(root, staging);
        try absent(ctx, path);
        var dir = try directory(ctx, path, true, true);
        dir.close();
    }
    var moves = std.ArrayList(move_t).init(ctx.allocator);
    const config_final = try registration(ctx, destination);
    const state_final = try state_location(ctx, destination);
    if (creates) {
        const staged_state = try stage_path(ctx, "state", id, "new_state");
        const staged_root = try ctx.resolve(staged_state);
        var dir = try directory(ctx, staged_root, true, true);
        dir.close();
        const disk = try ctx.join(staged_root, "disk.qcow2");
        if (operation == c.DeviceInit) {
            if (request.blank != 0) { _ = try tool(ctx, &.{ "qemu-img", "create", "-f", "qcow2", disk, arg }); } else { _ = try tool(ctx, &.{ "qemu-img", "create", "-f", "qcow2", "-b", base, "-F", "qcow2", disk }); }
        } else { _ = try tool(ctx, &.{ "qemu-img", "convert", "-O", "qcow2", if (operation == c.DeviceImport) backup_disk else text(&source.?.disk_image), disk }); }
        var disk_file = try regular(ctx, disk, false);
        try disk_file.chmod(0o600);
        try disk_file.sync();
        disk_file.close();
        _ = try image(ctx, disk, operation != c.DeviceInit or request.blank != 0);
        const staged_cfg = try stage_path(ctx, "config", id, "new.ini");
        const mounts = if (operation == c.DeviceImport) "[filesystem]\n" else if (operation == c.DeviceClone) "" else try std.fmt.allocPrint(ctx.allocator, "[filesystem]\nmount = {s}:Z:\\:rw\n", .{std.posix.getenv("HOME") orelse return error.InvalidConfig});
        const bytes = try profile(ctx, destination, try ctx.join(try ctx.resolve(state_final), "disk.qcow2"), cid, value, if (operation == c.DeviceClone) original else null, mounts);
        try create_file(ctx, try ctx.resolve(staged_cfg), bytes);
        var config_dir = try directory(ctx, try ctx.join(ctx.config, "devices"), true, true);
        config_dir.close();
        var state_dir = try directory(ctx, try ctx.join(ctx.state, "devices"), true, true);
        state_dir.close();
        try moves.append(try move(ctx, staged_state, state_final));
        try moves.append(try move(ctx, staged_cfg, config_final));
    } else if (operation == c.DeviceRename or operation == c.DeviceRemove) {
        const old_cfg = try registration(ctx, name);
        const rollback_cfg = try stage_path(ctx, "config", id, "old.ini");
        try moves.append(try move(ctx, old_cfg, rollback_cfg));
        const old_state = try state_location(ctx, name);
        var new_state = if (operation == c.DeviceRename) state_final else try stage_path(ctx, "state", id, "old_state");
        if (operation == c.DeviceRemove and request.keep_data != 0) {
            var retained = try directory(ctx, try ctx.join(ctx.state, "retained"), true, true);
            retained.close();
            new_state = .{ .root = "state", .path = try std.fmt.allocPrint(ctx.allocator, "retained/{s}", .{id}) };
            const saved_cfg = try ctx.join(text(&source.?.state_dir), "original.ini");
            try absent(ctx, saved_cfg);
            // Keep a recoverable copy in the state staging tree, then publish after state move.
            const copy = try stage_path(ctx, "state", id, "original.ini");
            try create_file(ctx, try ctx.resolve(copy), original.?);
            try moves.append(try move(ctx, copy, .{ .root = "state", .path = try std.fmt.allocPrint(ctx.allocator, "retained/{s}/original.ini", .{id}) }));
            const retention = try ctx.resolve(new_state);
            @memcpy(output[0..retention.len], retention); output[retention.len] = 0;
        }
        // State move must precede the optional retained config publication.
        try moves.insert(1, try move(ctx, old_state, new_state));
        if (operation == c.DeviceRename) {
            const staged_cfg = try stage_path(ctx, "config", id, "new.ini");
            try create_file(ctx, try ctx.resolve(staged_cfg), try profile(ctx, destination, try ctx.join(try ctx.resolve(state_final), "disk.qcow2"), cid, value, original, ""));
            try moves.append(try move(ctx, staged_cfg, config_final));
        }
        if (try selected_default(ctx)) |selected| {
            if (std.mem.eql(u8, selected, name)) {
                try moves.append(try move(ctx, .{ .root = "config", .path = "default_device" }, try stage_path(ctx, "config", id, "old_default")));
                if (operation == c.DeviceRename) {
                    const staged_default = try stage_path(ctx, "config", id, "new_default");
                    try create_file(ctx, try ctx.resolve(staged_default), try std.fmt.allocPrint(ctx.allocator, "{s}\n", .{destination}));
                    try moves.append(try move(ctx, staged_default, .{ .root = "config", .path = "default_device" }));
                }
            }
        }
    } else if (operation == c.DeviceExport) {
        const staged = location_t{ .root = "external", .path = try std.fmt.allocPrint(ctx.allocator, ".waddle-{s}", .{id}) };
        const path = try ctx.resolve(staged);
        try absent(ctx, path);
        var dir = try directory(ctx, path, true, true);
        dir.close();
        const disk = try ctx.join(path, "disk.qcow2");
        _ = try tool(ctx, &.{ "qemu-img", "convert", "-O", "qcow2", text(&source.?.disk_image), disk });
        var file = try regular(ctx, disk, false);
        try file.chmod(0o600); try file.sync(); file.close();
        _ = try image(ctx, disk, true);
        const hash = try hash_disk(ctx, disk);
        const manifest = manifest_t{ .schema_version = 1, .source_name = name, .disk_file = "disk.qcow2", .disk_size_bytes = hash.size, .disk_sha256 = &hash.hash, .config = value };
        try create_file(ctx, try ctx.join(path, "manifest.json"), try std.json.stringifyAlloc(ctx.allocator, manifest, .{}));
        const final = location_t{ .root = "external", .path = std.fs.path.basename(arg) };
        try moves.append(try move(ctx, staged, final));
        const published = try ctx.resolve(final);
        @memcpy(output[0..published.len], published); output[published.len] = 0;
    }
    fault("staged");
    record.moves = moves.items;
    try save_journal(ctx, record, false);
    for (record.moves, 0..) |item, index| {
        try apply_move(ctx, item, true);
        fault(try std.fmt.allocPrint(ctx.allocator, "move_{d}", .{index}));
    }
    var committed = record;
    committed.phase = "committed";
    try save_journal(ctx, committed, false);
    record = committed;
    fault("committed");
    try cleanup_journal(ctx, record);
}
/// Deterministic subprocess crash seam used only by isolated acceptance fixtures.
fn fault(point: []const u8) void {
    const configured = std.posix.getenv("WADDLE_DEVICE_TEST_CRASH") orelse return;
    if (std.mem.eql(u8, configured, point)) c._exit(99);
}

/// C ABI mutation entry: owned arena and locks are deterministically released.
export fn daemon_device_mutate(request: ?*const c.device_request_t, output: ?[*]u8) c_int {
    const input = request orelse { c.__errno_location().* = c.EINVAL; return -1; };
    const result = output orelse { c.__errno_location().* = c.EINVAL; return -1; };
    const previous_mask = c.umask(0o077);
    defer _ = c.umask(previous_mask);
    var arena = std.heap.ArenaAllocator.init(std.heap.c_allocator);
    defer arena.deinit();
    const ctx = context(arena.allocator()) catch |err| { set_errno(err); return -1; };
    mutate(ctx, input, result[0..1024]) catch |err| { set_errno(err); return -1; };
    return 0;
}

test "portable settings, sizes and root containment reject unsafe input" {
    try std.testing.expectError(error.InvalidConfig, blank_size("0M"));
    try std.testing.expectError(error.Overflow, blank_size("999999999999999999999G"));
    try std.testing.expectEqual(@as(u64, 1024 * 1024), try blank_size("1M"));
    try std.testing.expectError(error.InvalidConfig, relative_valid("devices/../outside"));
    try std.testing.expectError(error.InvalidConfig, settings_valid(.{ .vcpus = 0 }));
    try std.testing.expectError(error.InvalidConfig, settings_valid(.{ .default_shell = "bad\n" }));
    try settings_valid(.{});
}

test "bounded journal parsing rejects duplicates unknown fields and nesting" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const ctx = try context(arena.allocator());
    try std.testing.expectError(error.DuplicateField, parse_json(settings_t, ctx, "{\"vcpus\":4,\"vcpus\":8}"));
    try std.testing.expectError(error.UnknownField, parse_json(settings_t, ctx, "{\"unexpected\":4}"));
    try std.testing.expectError(error.InvalidConfig, parse_json(std.json.Value, ctx, "[[[[[[[[[0]]]]]]]]]"));
}
