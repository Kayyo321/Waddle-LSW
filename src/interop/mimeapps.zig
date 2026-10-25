//! Preserving pure editor for one MIME default; filesystem publication is external.
const std = @import("std");
/// Maximum serialized mimeapps.list size accepted and produced.
pub const MaxBytes: usize = 1024 * 1024;
/// Maximum physical lines, including a final unterminated line.
pub const MaxLines: usize = 4096;
/// Maximum physical line payload bytes excluding LF.
pub const MaxLineBytes: usize = 16384;

/// Owned result; allocator must outlive this value. No input slice is retained.
pub const edit_result_t = struct {
    /// Allocator owning bytes and prior_value.
    allocator: std.mem.Allocator,
    /// Complete new file; publish only under the integration transaction owner.
    bytes: []u8,
    /// Exact trimmed previous list; null means no key, empty means an empty key.
    prior_value: ?[]u8,
    /// Free both owned allocations. Repeat calls are safe; caller serializes.
    pub fn deinit(self: *edit_result_t) void {
        self.allocator.free(self.bytes);
        if (self.prior_value) |prior| self.allocator.free(prior);
        self.bytes = &.{};
        self.prior_value = null;
    }
};
const range_t = struct { start: usize, end: usize };
const plan_t = struct {
    target: ?range_t = null,
    prior_value: ?[]const u8 = null,
    insert: usize,
    have_group: bool = false,
    newline: []const u8 = "\n",
};

fn trim(text: []const u8) []const u8 {
    return std.mem.trim(u8, text, " \t");
}
fn mime_valid(mime: []const u8) bool {
    if (mime.len < 3 or mime.len > 255) return false;
    var slash: ?usize = null;
    for (mime, 0..) |byte, index| {
        if (byte == '/') {
            if (slash != null or index == 0 or index + 1 == mime.len) return false;
            slash = index;
        } else if (!(std.ascii.isAlphanumeric(byte) or byte == '.' or byte == '+' or byte == '-' or byte == '_')) return false;
    }
    return slash != null;
}
fn defaults_valid(value: []const u8) bool {
    if (value.len > MaxLineBytes or !std.unicode.utf8ValidateSlice(value)) return false;
    if (value.len == 0) return true;
    var iterator = std.mem.splitScalar(u8, value, ';');
    var consumed: usize = 0;
    while (iterator.next()) |id| {
        consumed += id.len;
        if (id.len == 0) return consumed == value.len;
        if (id.len > 255 or !std.mem.endsWith(u8, id, ".desktop") or id.len == 8) return false;
        for (id) |byte| if (byte <= 0x20 or byte == 0x7f or byte == '/' or byte == '\\' or byte == '[' or byte == ']') return false;
        consumed += 1;
    }
    return true;
}
fn scan(input: []const u8, mime: []const u8) !plan_t {
    if (!mime_valid(mime)) return error.InvalidArgument;
    if (input.len > MaxBytes) return error.Quota;
    if (!std.unicode.utf8ValidateSlice(input) or std.mem.indexOfScalar(u8, input, 0) != null) return error.Malformed;
    var plan = plan_t{ .insert = input.len };
    var offset: usize = 0;
    var lines: usize = 0;
    var in_default = false;
    while (offset < input.len) {
        lines += 1;
        if (lines > MaxLines) return error.Quota;
        const lf = std.mem.indexOfScalarPos(u8, input, offset, '\n');
        const end = if (lf) |position| position + 1 else input.len;
        var content_end = if (lf) |position| position else input.len;
        if (content_end - offset > MaxLineBytes) return error.Quota;
        const crlf = content_end > offset and input[content_end - 1] == '\r';
        if (crlf) content_end -= 1;
        const content = trim(input[offset..content_end]);
        if (content.len != 0 and content[0] == '[') {
            if (content.len < 3 or content[content.len - 1] != ']') return error.Malformed;
            if (in_default) plan.insert = offset;
            in_default = std.mem.eql(u8, content, "[Default Applications]");
            if (in_default) {
                if (plan.have_group) return error.Ambiguous;
                plan.have_group = true;
                plan.newline = if (crlf) "\r\n" else "\n";
            }
        } else if (in_default and content.len != 0 and content[0] != '#' and content[0] != ';') {
            if (std.mem.indexOfScalar(u8, content, '=')) |equal| {
                if (std.mem.eql(u8, trim(content[0..equal]), mime)) {
                    if (plan.target != null) return error.Ambiguous;
                    const value = trim(content[equal + 1 ..]);
                    if (!defaults_valid(value)) return error.Malformed;
                    plan.target = .{ .start = offset, .end = end };
                    plan.prior_value = value;
                    plan.newline = if (crlf) "\r\n" else "\n";
                }
            }
        }
        offset = end;
    }
    return plan;
}

/// Inspect one target's complete default list without allocation or I/O.
/// Input and mime are borrowed immutable UTF-8 bytes for the call; the optional
/// result borrows input until it is modified/freed. Returns null for absence,
/// empty for an empty value; InvalidArgument/Quota/Malformed/Ambiguous reject
/// unsupported input. Pure and thread-safe for stable independent buffers.
pub fn read_default(input: []const u8, mime: []const u8) !?[]const u8 {
    return (try scan(input, mime)).prior_value;
}

/// Produce a complete owned edit and exact owned prior value. All slices are
/// borrowed only during this call, never mutated/retained. Null replacement
/// removes the key; nonnull is a complete validated desktop-ID list. Caller
/// frees the result with deinit; allocator must outlive it. Errors preserve all
/// input and free partial allocations. Thread-safe for independent state;
/// caller supplies transaction/lock/filesystem ownership externally.
pub fn rewrite(allocator: std.mem.Allocator, input: []const u8, mime: []const u8, replacement: ?[]const u8) !edit_result_t {
    const plan = try scan(input, mime);
    if (replacement) |value| if (!defaults_valid(value)) return error.InvalidArgument;
    const prior = if (plan.prior_value) |value| try allocator.dupe(u8, value) else null;
    errdefer if (prior) |value| allocator.free(value);
    var output = std.ArrayList(u8).init(allocator);
    errdefer output.deinit();
    if (plan.target) |target| {
        try output.appendSlice(input[0..target.start]);
        if (replacement) |value| {
            try output.appendSlice(mime);
            try output.append('=');
            try output.appendSlice(value);
            // An existing unterminated target remains unterminated.
            if (target.end != input.len or input[target.end - 1] == '\n') try output.appendSlice(plan.newline);
        }
        try output.appendSlice(input[target.end..]);
    } else if (replacement) |value| {
        try output.appendSlice(input[0..plan.insert]);
        if (plan.insert != 0 and input[plan.insert - 1] != '\n') try output.appendSlice(plan.newline);
        if (!plan.have_group) try output.appendSlice("[Default Applications]\n");
        try output.appendSlice(mime);
        try output.append('=');
        try output.appendSlice(value);
        try output.appendSlice(plan.newline);
        try output.appendSlice(input[plan.insert..]);
    } else try output.appendSlice(input);
    // Revalidate produced quotas so a returned file always remains editable.
    _ = try scan(output.items, mime);
    return .{ .allocator = allocator, .bytes = try output.toOwnedSlice(), .prior_value = prior };
}

// Test-only fixtures.
test "target edits preserve every other group line comment and complete prior list" {
    const input = "# keep\n[Added Associations]\ntext/plain=else.desktop;\n[Default Applications]\n# inside\n text/plain = old.desktop;second.desktop; \nimage/png=image.desktop;\n[Other]\nX=untouched";
    var result = try rewrite(std.testing.allocator, input, "text/plain", "waddle-open.desktop;");
    defer result.deinit();
    try std.testing.expectEqualStrings("old.desktop;second.desktop;", result.prior_value.?);
    try std.testing.expectEqualStrings("# keep\n[Added Associations]\ntext/plain=else.desktop;\n[Default Applications]\n# inside\ntext/plain=waddle-open.desktop;\nimage/png=image.desktop;\n[Other]\nX=untouched", result.bytes);
    var restored = try rewrite(std.testing.allocator, result.bytes, "text/plain", result.prior_value);
    defer restored.deinit();
    try std.testing.expectEqualStrings("old.desktop;second.desktop;", (try read_default(restored.bytes, "text/plain")).?);
    var removed = try rewrite(std.testing.allocator, result.bytes, "text/plain", null);
    defer removed.deinit();
    try std.testing.expect(std.mem.indexOf(u8, removed.bytes, "waddle-open.desktop") == null);
    try std.testing.expectEqualStrings("image.desktop;", (try read_default(removed.bytes, "image/png")).?);
}
test "insertion removal empty defaults and newline styles are explicit" {
    const cases = [_]struct { input: []const u8, expected: []const u8 }{
        .{ .input = "", .expected = "[Default Applications]\ntext/plain=a.desktop;\n" },
        .{ .input = "# x", .expected = "# x\n[Default Applications]\ntext/plain=a.desktop;\n" },
        .{ .input = "[Default Applications]\r\n# x\r\n[Other]\r\nX=1", .expected = "[Default Applications]\r\n# x\r\ntext/plain=a.desktop;\r\n[Other]\r\nX=1" },
        .{ .input = "[Default Applications]", .expected = "[Default Applications]\ntext/plain=a.desktop;\n" },
        .{ .input = "[Default Applications]\ntext/plain=old.desktop", .expected = "[Default Applications]\ntext/plain=a.desktop;" },
    };
    for (cases) |item| {
        var result = try rewrite(std.testing.allocator, item.input, "text/plain", "a.desktop;");
        defer result.deinit();
        try std.testing.expectEqualStrings(item.expected, result.bytes);
    }
    var absent = try rewrite(std.testing.allocator, "[Other]\nX=1", "text/plain", null);
    defer absent.deinit();
    try std.testing.expectEqualStrings("[Other]\nX=1", absent.bytes);
    var empty = try rewrite(std.testing.allocator, "[Default Applications]\ntext/plain=\n", "text/plain", "");
    defer empty.deinit();
    try std.testing.expectEqualStrings("", empty.prior_value.?);
    try std.testing.expectEqualStrings("", (try read_default(empty.bytes, "text/plain")).?);
    try std.testing.expect((try read_default(empty.bytes, "image/png")) == null);
    empty.deinit();
}
test "ambiguous malformed and injection values fail closed" {
    for ([_][]const u8{ "[Default Applications]\n[Default Applications]\n", "[Default Applications]\ntext/plain=a.desktop\ntext/plain=b.desktop\n" }) |input|
        try std.testing.expectError(error.Ambiguous, rewrite(std.testing.allocator, input, "text/plain", "a.desktop"));
    for ([_][]const u8{ "[broken\n", "[Default Applications]\ntext/plain=/x.desktop\n", "[Default Applications]\ntext/plain=a.desktop;;b.desktop\n", "bad\x00", "bad\xff" }) |input|
        try std.testing.expectError(error.Malformed, rewrite(std.testing.allocator, input, "text/plain", null));
    for ([_][]const u8{ "", "a", "/x", "x/", "x/y/z", "a b/c", "x/\n" }) |mime|
        try std.testing.expectError(error.InvalidArgument, rewrite(std.testing.allocator, "", mime, "a.desktop"));
    for ([_][]const u8{ "x", ".desktop", "a b.desktop", "a\\b.desktop", "a/b.desktop", "x\n.desktop", "[x].desktop", ";", "a.desktop;;", "a\xff.desktop" }) |value|
        try std.testing.expectError(error.InvalidArgument, rewrite(std.testing.allocator, "", "text/plain", value));
    var unicode = try rewrite(std.testing.allocator, "", "x-custom/type+suffix_1.2", "é.desktop;another.desktop");
    defer unicode.deinit();
    try std.testing.expectEqualStrings("é.desktop;another.desktop", (try read_default(unicode.bytes, "x-custom/type+suffix_1.2")).?);
}
fn allocation_case(allocator: std.mem.Allocator) !void {
    var result = try rewrite(allocator, "[Default Applications]\ntext/plain=old.desktop;", "text/plain", "new.desktop;");
    defer result.deinit();
}
test "allocation failures and file line value quotas preserve ownership" {
    try std.testing.checkAllAllocationFailures(std.testing.allocator, allocation_case, .{});
    const huge = try std.testing.allocator.alloc(u8, MaxBytes + 1);
    defer std.testing.allocator.free(huge);
    @memset(huge, 'x');
    try std.testing.expectError(error.Quota, rewrite(std.testing.allocator, huge, "text/plain", null));
    try std.testing.expectError(error.Quota, rewrite(std.testing.allocator, huge[0 .. MaxLineBytes + 1], "text/plain", null));
    @memset(huge, '\n');
    try std.testing.expectError(error.Quota, rewrite(std.testing.allocator, huge[0 .. MaxLines + 1], "text/plain", null));
    @memset(huge, 'a');
    try std.testing.expectError(error.InvalidArgument, rewrite(std.testing.allocator, "", huge[0..256], "a.desktop"));
    try std.testing.expectError(error.InvalidArgument, rewrite(std.testing.allocator, "", "text/plain", huge[0..256]));
    try std.testing.expectError(error.InvalidArgument, rewrite(std.testing.allocator, "", "text/plain", huge[0 .. MaxLineBytes + 1]));
}

test "produced documents obey the same quotas and unrelated bytes stay stable" {
    const input = try std.testing.allocator.alloc(u8, MaxBytes);
    defer std.testing.allocator.free(input);
    for (input, 0..) |*byte, index| byte.* = if (index % MaxLineBytes == MaxLineBytes - 1) '\n' else '#';
    try std.testing.expectError(error.Quota, rewrite(std.testing.allocator, input, "text/plain", "a.desktop;"));
    @memset(input, '\n');
    try std.testing.expectError(error.Quota, rewrite(std.testing.allocator, input[0..MaxLines], "text/plain", "a.desktop;"));
    try std.testing.expectError(error.Malformed, read_default("[broken", "text/plain"));
    var random_state = std.Random.DefaultPrng.init(0x6d696d652d656469);
    const random = random_state.random();
    var bytes: [512]u8 = undefined;
    for (0..2000) |_| {
        random.bytes(&bytes);
        const length = random.uintLessThan(usize, bytes.len + 1);
        var result = rewrite(std.testing.allocator, bytes[0..length], "text/plain", "a.desktop;") catch continue;
        defer result.deinit();
        try std.testing.expectEqualStrings("a.desktop;", (try read_default(result.bytes, "text/plain")).?);
    }
}

fn allocation_variants(allocator: std.mem.Allocator, input: []const u8, replacement: ?[]const u8) !void {
    var result = try rewrite(allocator, input, "text/plain", replacement);
    defer result.deinit();
}
test "all publication shapes release allocations on every failure" {
    const inputs = [_][]const u8{
        "",                                                                "# long unrelated preserved comment to allocate prefix storage\n",
        "[Other]\nX=kept\n",                                               "[Default Applications]",
        "[Default Applications]\n; comment\nnot-a-key\n[Other]\nX=kept\n", "[Default Applications]\ntext/plain=old.desktop;\n[Other]\n" ++ ("# long unrelated preserved comment\n" ** 40),
        "[Default Applications]\ntext/plain=old.desktop;",
    };
    for (inputs) |input| {
        try std.testing.checkAllAllocationFailures(std.testing.allocator, allocation_variants, .{ input, @as(?[]const u8, "new.desktop;") });
        try std.testing.checkAllAllocationFailures(std.testing.allocator, allocation_variants, .{ input, @as(?[]const u8, null) });
        try std.testing.checkAllAllocationFailures(std.testing.allocator, allocation_variants, .{ input, @as(?[]const u8, "long-new-default-application.desktop;" ** 20) });
    }
    try std.testing.expectError(error.InvalidArgument, read_default("", "/ab"));
    try std.testing.expectError(error.InvalidArgument, read_default("", "ab/"));
    try std.testing.expectError(error.Malformed, read_default("[]", "text/plain"));
}
