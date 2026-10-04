const std = @import("std");

fn valid_source(source: []const u8) bool {
    if (source.len == 0 or source[0] != '/' or source.len > 1048576) return false;
    if (source.len == 1) return true;
    var parts = std.mem.splitScalar(u8, std.mem.trimRight(u8, source[1..], "/"), '/');
    while (parts.next()) |part| {
        if (part.len == 0 or std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return false;
        for (part) |ch| if (ch == '\\' or ch == '=') return false;
    }
    return std.unicode.utf8ValidateSlice(source);
}

fn valid_target(target: []const u8) bool {
    if (target.len < 3 or target.len > 1048576 or !std.ascii.isAlphabetic(target[0]) or
        target[1] != ':' or target[2] != '\\') return false;
    var parts = std.mem.splitScalar(u8, target[3..], '\\');
    while (parts.next()) |part| {
        if (part.len > 0 and (part[part.len - 1] == '.' or part[part.len - 1] == ' ')) return false;
        if (std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return false;
        for (part) |ch| if (ch < 32 or std.mem.indexOfScalar(u8, "/:*?\"<>|", ch) != null) return false;
    }
    return std.unicode.utf8ValidateSlice(target);
}

/// Validate borrowed UTF-8 rule strings; no allocation, mutation, or shared state.
/// Returns 1 if valid, 0 otherwise. Pointers must be nonnull NUL-terminated strings.
export fn waddle_path_rule_valid(source: [*:0]const u8, target: [*:0]const u8) c_int {
    return @intFromBool(valid_source(std.mem.span(source)) and valid_target(std.mem.span(target)));
}

fn translate(path: []const u8, source_arg: []const u8, target: []const u8, output: []u8) !usize {
    if (!valid_source(source_arg) or !valid_target(target) or !std.unicode.utf8ValidateSlice(path)) return error.Invalid;
    const source = if (source_arg.len == 1) source_arg else std.mem.trimRight(u8, source_arg, "/");
    if (!std.mem.startsWith(u8, path, source) or
        (source.len > 1 and path.len > source.len and path[source.len] != '/')) return error.Unmapped;
    var used: usize = 0;
    const root = std.mem.trimRight(u8, target, "\\");
    if (root.len >= output.len) return error.Capacity;
    @memcpy(output[0..root.len], root);
    used = root.len;
    var parts = std.mem.splitScalar(u8, path[source.len..], '/');
    while (parts.next()) |part| {
        if (std.mem.eql(u8, part, "..")) return error.Invalid;
        if (part.len == 0 or std.mem.eql(u8, part, ".")) continue;
        for (part) |ch| if (ch < 32 or std.mem.indexOfScalar(u8, "\\:*?\"<>|", ch) != null) return error.Invalid;
        if (part[part.len - 1] == '.' or part[part.len - 1] == ' ') return error.Invalid;
        if (used + 1 + part.len >= output.len) return error.Capacity;
        output[used] = '\\';
        used += 1;
        @memcpy(output[used..][0..part.len], part);
        used += part.len;
    }
    if (used == 2) {
        if (used + 1 >= output.len) return error.Capacity;
        output[used] = '\\';
        used += 1;
    }
    output[used] = 0;
    return used;
}

/// Translate borrowed NUL-terminated path/rule into caller-owned output[capacity].
/// Returns 0 on success, -1 for invalid/unmapped input, -2 for capacity failure.
/// No allocation or shared state; output may be partial on failure.
export fn waddle_path_rule_apply(path: [*:0]const u8, source: [*:0]const u8, target: [*:0]const u8, output: [*]u8, capacity: usize) c_int {
    _ = translate(std.mem.span(path), std.mem.span(source), std.mem.span(target), output[0..capacity]) catch |err| {
        return if (err == error.Capacity) -2 else -1;
    };
    return 0;
}

test "rule validation, components and drive roots" {
    for ([_][]const u8{ "", "relative", "/a//b", "/../b", "/a/./b", "/a\\b", "/a=b" }) |source|
        try std.testing.expect(!valid_source(source));
    for ([_][]const u8{ "", "C:relative", "\\\\server\\share", "C:\\..", "C:\\x/y", "C:\\x?" }) |target|
        try std.testing.expect(!valid_target(target));
    try std.testing.expect(valid_source("/home/dev/"));
    try std.testing.expect(valid_source("/"));
    try std.testing.expect(valid_target("X:\\export"));
}

test "normalization, Unicode, boundaries, capacity and traversal" {
    var output: [100]u8 = undefined;
    const n = try translate("/home/dev//./日本語", "/home/dev/", "X:\\export\\", &output);
    try std.testing.expectEqualStrings("X:\\export\\日本語", output[0..n]);
    const root_len = try translate("/", "/", "Z:\\", &output);
    try std.testing.expectEqualStrings("Z:\\", output[0..root_len]);
    try std.testing.expectError(error.Unmapped, translate("/home/device", "/home/dev", "X:\\", &output));
    for ([_][]const u8{ "/home/dev/../x", "/home/dev/x\\y", "/home/dev/x?", "/home/dev/x.", "/home/dev/x " }) |path|
        try std.testing.expectError(error.Invalid, translate(path, "/home/dev", "X:\\", &output));
    try std.testing.expectError(error.Capacity, translate("/x", "/", "Z:\\", output[0..4]));
    try std.testing.expectEqual(@as(c_int, 0), waddle_path_rule_valid("bad", "Z:\\"));
    try std.testing.expectEqual(@as(c_int, -1), waddle_path_rule_apply("/../x", "/", "Z:\\", &output, output.len));
    try std.testing.expectEqual(@as(c_int, -2), waddle_path_rule_apply("/x", "/", "Z:\\", &output, 1));
    try std.testing.expectEqual(@as(c_int, 0), waddle_path_rule_apply("/x", "/", "Z:\\", &output, output.len));
}
