const std = @import("std");

/// Caller-owned header result; no allocation or shared state, C ABI layout.
const guest_frame_t = extern struct {
    length: u32,
    crc: u32,
    type: u16,
};

fn get16(bytes: []const u8) u16 {
    return std.mem.readInt(u16, bytes[0..2], .little);
}
fn get32(bytes: []const u8) u32 {
    return std.mem.readInt(u32, bytes[0..4], .little);
}
fn get64(bytes: []const u8) u64 {
    return std.mem.readInt(u64, bytes[0..8], .little);
}

/// in: borrowed nonnull header[length], expected sequence; out: caller-owned frame.
/// Returns 0 for valid 32-byte header, -1 otherwise. No allocation; thread-safe.
export fn guest_header_validate(header: [*]const u8, length: usize, sequence: u32,
    frame: *guest_frame_t) c_int {
    if (length != 32) return -1;
    const bytes = header[0..length];
    if (get32(bytes) != 0x57444c43 or get16(bytes[4..]) != 1 or
        get64(bytes[8..]) != 1 or get32(bytes[20..]) != 0 or
        get32(bytes[24..]) != sequence or get32(bytes[16..]) > 1048576) return -1;
    const kind = get16(bytes[6..]);
    switch (kind) {
        1, 3, 4, 5, 7, 9 => {},
        else => return -1,
    }
    frame.* = .{ .length = get32(bytes[16..]), .crc = get32(bytes[28..]), .type = kind };
    return 0;
}

/// in: borrowed nonnull data[length]; returns IEEE CRC-32. No mutation, allocation
/// or shared state; thread-safe. Empty input is allowed and returns zero.
export fn guest_crc32(data: [*]const u8, length: usize) u32 {
    return std.hash.Crc32.hash(data[0..length]);
}

test "headers reject corruption before payload allocation" {
    var header = [_]u8{0} ** 32;
    std.mem.writeInt(u32, header[0..4], 0x57444c43, .little);
    std.mem.writeInt(u16, header[4..6], 1, .little);
    std.mem.writeInt(u16, header[6..8], 1, .little);
    std.mem.writeInt(u64, header[8..16], 1, .little);
    std.mem.writeInt(u32, header[24..28], 1, .little);
    var frame: guest_frame_t = undefined;
    try std.testing.expectEqual(@as(c_int, 0), guest_header_validate(&header, 32, 1, &frame));
    try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 31, 1, &frame));
    try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 32, 2, &frame));
    for ([_]usize{ 0, 4, 8, 20, 24 }) |offset| {
        header[offset] ^= 2;
        try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 32, 1, &frame));
        header[offset] ^= 2;
    }
    std.mem.writeInt(u32, header[16..20], 1048577, .little);
    try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 32, 1, &frame));
    std.mem.writeInt(u32, header[16..20], 1048576, .little);
    for ([_]u16{ 1, 3, 4, 5, 7, 9, 2, 6, 8, 255, 0 }) |kind| {
        std.mem.writeInt(u16, header[6..8], kind, .little);
        try std.testing.expectEqual(@as(c_int, if (kind == 1 or kind == 3 or kind == 4 or kind == 5 or kind == 7 or kind == 9) 0 else -1), guest_header_validate(&header, 32, 1, &frame));
    }
    try std.testing.expectEqual(@as(u32, 0xcbf43926), guest_crc32("123456789", 9));
    try std.testing.expectEqual(@as(u32, 0), guest_crc32("", 0));
}

/// Borrowed UTF-8 views into one retained frame; no allocations or shared state.
const guest_spawn_t = extern struct {
    cwd: [*]const u8,
    command: [*]const u8,
    environment: [*]const u8,
    cwd_length: u32,
    command_length: u32,
    environment_length: u32,
    rows: u16,
    cols: u16,
    interactive: u32,
};

fn valid_text(bytes: []const u8) bool {
    return std.mem.indexOfScalar(u8, bytes, 0) == null and std.unicode.utf8ValidateSlice(bytes);
}

// Decode only argv[0]; validate the canonical quoted grammar for every argument.
// Workspace never grows: the C caller supplies command_length + 1 bytes.
fn command_validate(command: []const u8, executable: []u8) bool {
    var offset: usize = 0;
    var written: usize = 0;
    var first = true;
    while (offset < command.len) {
        if (command[offset] != '"') return false;
        offset += 1;
        while (true) {
            var slashes: usize = 0;
            while (offset < command.len and command[offset] == '\\') : (offset += 1) slashes += 1;
            if (offset == command.len) return false;
            const quote = command[offset] == '"';
            const count = if (quote) slashes / 2 else slashes;
            if (first) {
                if (count > executable.len -| written) return false;
                @memset(executable[written..][0..count], '\\');
                written += count;
            }
            if (quote and slashes % 2 == 0) {
                offset += 1;
                break;
            }
            if (first) {
                if (quote or written >= executable.len) return false;
                executable[written] = command[offset];
                written += 1;
            }
            offset += 1;
        }
        if (first) {
            if (written == 0 or written >= executable.len) return false;
            executable[written] = 0;
            first = false;
        }
        if (offset == command.len) return true;
        if (command[offset] != ' ' or offset + 1 == command.len) return false;
        offset += 1;
    }
    return false;
}

fn spawn_validate(bytes: []const u8, spawn: *guest_spawn_t, executable: []u8) bool {
    if (bytes.len < 26 or bytes.len > 1048576) return false;
    const flags = get32(bytes);
    if (flags & ~@as(u32, 11) != 0 or (flags & 3 != 1 and flags & 3 != 2)) return false;
    const cwd_len: usize = get32(bytes[12..]);
    const cmd_len: usize = get32(bytes[16..]);
    const env_len: usize = get32(bytes[20..]);
    if (@as(u64, cwd_len) + cmd_len + env_len + 26 != bytes.len or cmd_len == 0 or cwd_len == 0) return false;
    const cwd = bytes[24..][0..cwd_len];
    const command = bytes[25 + cwd_len ..][0..cmd_len];
    const environment = bytes[26 + cwd_len + cmd_len ..];
    if (bytes[24 + cwd_len] != 0 or bytes[25 + cwd_len + cmd_len] != 0 or
        !valid_text(cwd) or !valid_text(command) or !command_validate(command, executable)) return false;
    var offset: usize = 0;
    while (offset < environment.len) {
        const end = std.mem.indexOfScalarPos(u8, environment, offset, 0) orelse return false;
        const item = environment[offset..end];
        const equal = std.mem.indexOfScalar(u8, item, '=') orelse return false;
        if (equal == 0 or !valid_text(item)) return false;
        offset = end + 1;
    }
    const rows = get16(bytes[4..]);
    const cols = get16(bytes[6..]);
    if (flags & 1 != 0 and (rows == 0 or cols == 0 or rows > 32767 or cols > 32767)) return false;
    spawn.* = .{
        .cwd = cwd.ptr, .command = command.ptr, .environment = environment.ptr,
        .cwd_length = @intCast(cwd_len), .command_length = @intCast(cmd_len),
        .environment_length = @intCast(env_len), .rows = rows, .cols = cols,
        .interactive = flags & 1,
    };
    return true;
}

/// in: borrowed nonnull body[length], retained for spawn views. out: caller-owned
/// spawn and executable[capacity], disjoint from body. Returns 0 success, -1
/// malformed input/capacity. Output unspecified on failure; allocation-free and thread-safe.
export fn guest_spawn_validate(body: [*]const u8, length: usize, spawn: *guest_spawn_t,
    executable: [*]u8, capacity: usize) c_int {
    return if (spawn_validate(body[0..length], spawn, executable[0..capacity])) 0 else -1;
}

test "spawn canonical arguments and bounded views" {
    const cwd = "C:\\work";
    const command = "\"cmd.exe\" \"\" \"日本語\" \"a\\\"b\" \"C:\\x\\\\\"";
    const environment = "X=日本語\x00X=last\x00";
    const size = 26 + cwd.len + command.len + environment.len;
    const bytes = try std.testing.allocator.alloc(u8, size);
    defer std.testing.allocator.free(bytes);
    @memset(bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 1, .little);
    std.mem.writeInt(u16, bytes[4..6], 24, .little);
    std.mem.writeInt(u16, bytes[6..8], 80, .little);
    std.mem.writeInt(u32, bytes[12..16], cwd.len, .little);
    std.mem.writeInt(u32, bytes[16..20], command.len, .little);
    std.mem.writeInt(u32, bytes[20..24], environment.len, .little);
    @memcpy(bytes[24..][0..cwd.len], cwd);
    @memcpy(bytes[25 + cwd.len ..][0..command.len], command);
    @memcpy(bytes[26 + cwd.len + command.len ..], environment);
    var spawn: guest_spawn_t = undefined;
    var executable: [100]u8 = undefined;
    try std.testing.expect(spawn_validate(bytes, &spawn, &executable));
    try std.testing.expectEqualStrings("cmd.exe", std.mem.sliceTo(&executable, 0));
    for (0..size) |len| try std.testing.expect(!spawn_validate(bytes[0..len], &spawn, &executable));
    for ([_]usize{ 0, 5, 7, 12, 16, 20, 24 + cwd.len, 25 + cwd.len + command.len }) |offset| {
        const saved = bytes[offset];
        bytes[offset] = 255;
        try std.testing.expect(!spawn_validate(bytes, &spawn, &executable));
        bytes[offset] = saved;
    }
    bytes[24] = 255;
    try std.testing.expect(!spawn_validate(bytes, &spawn, &executable));
    bytes[24] = 0;
    try std.testing.expect(!spawn_validate(bytes, &spawn, &executable));
    bytes[24] = 'C';
    const env_start = 26 + cwd.len + command.len;
    bytes[env_start] = '=';
    try std.testing.expect(!spawn_validate(bytes, &spawn, &executable));
    bytes[env_start] = 'X';
    bytes[size - 1] = 'x';
    try std.testing.expect(!spawn_validate(bytes, &spawn, &executable));
    try std.testing.expect(!spawn_validate(bytes, &spawn, executable[0..2]));
}

test "reject noncanonical commands and preserve executable selection" {
    var executable: [100]u8 = undefined;
    for ([_][]const u8{ "", "cmd", "\"\"", "\"cmd\" ", "\"cmd\"  \"x\"", "\"cmd", "\"cmd\\\"", "\"cmd\"x", "\"c\\\"md\"", "\"cmd\" \"x" }) |command|
        try std.testing.expect(!command_validate(command, &executable));
    try std.testing.expect(command_validate("\"C:\\Program Files\\app.exe\" \"x\\\"y\"", &executable));
    try std.testing.expectEqualStrings("C:\\Program Files\\app.exe", std.mem.sliceTo(&executable, 0));
    try std.testing.expect(!command_validate("\"cmd\"", executable[0..0]));
}
