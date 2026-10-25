//! Keyboard layout sync: XKB layout and variant names to Windows KLIDs, plus a
//! send-on-change reducer for KeyboardLayout. Pure, allocation-free, owner-thread
//! state. No keymap parsing, no LoadKeyboardLayoutW and no wire encoding here.
const std = @import("std");

/// Maximum bytes accepted for an XKB layout or variant name.
pub const MaxNameBytes: usize = 64;
/// Exact length of a Windows KLID string: eight hexadecimal digits.
pub const KlidBytes: usize = 8;

/// Explicit rejections. No state is changed when one is returned.
/// InvalidName: layout is empty, a name is longer than MaxNameBytes, or a byte is outside the XKB charset.
/// InvalidKlid: text is not exactly eight hexadecimal digits, or every digit is zero.
pub const error_t = error{ InvalidName, InvalidKlid };

/// Windows keyboard layout identifier in canonical uppercase ASCII hexadecimal.
pub const klid_t = extern struct {
    /// Digits 0 through 9 and A through F. Never all zero in a value returned by this module.
    bytes: [KlidBytes]u8,
};

/// One mapping row. `layout` and `variant` are XKB names; an empty variant is the layout default.
pub const entry_t = struct {
    /// XKB layout name, valid under validate_name.
    layout: []const u8,
    /// XKB variant name, valid under validate_variant; empty for the default variant.
    variant: []const u8,
    /// Windows KLID for this row, canonical uppercase.
    klid: [KlidBytes]u8,
};

/// Fixed mapping table. Each KLID is the keyboard that the Microsoft "Default input
/// profiles (input locales) in Windows" page lists for a locale whose physical layout
/// equals the xkb layout (verified against the 2024-09-02 revision). Rows whose Windows
/// default is an IME, or whose KLID the page does not confirm, are absent. Pairs are
/// unique. Lookup is exact: a variant without a row is unmapped, never its layout default.
pub const KeyboardLayoutTable = [_]entry_t{
    .{ .layout = "us", .variant = "", .klid = "00000409".* },
    .{ .layout = "us", .variant = "intl", .klid = "00020409".* },
    .{ .layout = "gb", .variant = "", .klid = "00000809".* },
    .{ .layout = "ie", .variant = "", .klid = "00001809".* },
    .{ .layout = "au", .variant = "", .klid = "00000409".* },
    .{ .layout = "ara", .variant = "", .klid = "00000401".* },
    .{ .layout = "de", .variant = "", .klid = "00000407".* },
    .{ .layout = "ch", .variant = "", .klid = "00000807".* },
    .{ .layout = "at", .variant = "", .klid = "00000407".* },
    .{ .layout = "fr", .variant = "", .klid = "0000040C".* },
    .{ .layout = "es", .variant = "", .klid = "0000040A".* },
    .{ .layout = "it", .variant = "", .klid = "00000410".* },
    .{ .layout = "pt", .variant = "", .klid = "00000816".* },
    .{ .layout = "br", .variant = "", .klid = "00000416".* },
    .{ .layout = "se", .variant = "", .klid = "0000041D".* },
    .{ .layout = "no", .variant = "", .klid = "00000414".* },
    .{ .layout = "dk", .variant = "", .klid = "00000406".* },
    .{ .layout = "fi", .variant = "", .klid = "0000040B".* },
    .{ .layout = "is", .variant = "", .klid = "0000040F".* },
    .{ .layout = "pl", .variant = "", .klid = "00000415".* },
    .{ .layout = "cz", .variant = "", .klid = "00000405".* },
    .{ .layout = "sk", .variant = "", .klid = "0000041B".* },
    .{ .layout = "hu", .variant = "", .klid = "0000040E".* },
    .{ .layout = "ro", .variant = "", .klid = "00010418".* },
    .{ .layout = "hr", .variant = "", .klid = "0000041A".* },
    .{ .layout = "si", .variant = "", .klid = "00000424".* },
    .{ .layout = "ee", .variant = "", .klid = "00000425".* },
    .{ .layout = "lv", .variant = "", .klid = "00020426".* },
    .{ .layout = "ru", .variant = "", .klid = "00000419".* },
    .{ .layout = "ua", .variant = "", .klid = "00020422".* },
    .{ .layout = "by", .variant = "", .klid = "00000423".* },
    .{ .layout = "gr", .variant = "", .klid = "00000408".* },
    .{ .layout = "tr", .variant = "", .klid = "0000041F".* },
    .{ .layout = "il", .variant = "", .klid = "0002040D".* },
    .{ .layout = "th", .variant = "", .klid = "0000041E".* },
};

/// Send-on-change classification for one observation.
pub const outcome_kind_t = enum(u8) {
    /// The mapped KLID differs from the last sent value. Queue exactly one KeyboardLayout.
    Send = 0,
    /// The mapped KLID equals the last sent value. Queue nothing.
    Unchanged = 1,
    /// No table row matches. Queue nothing; the guest layout stays as it is.
    NoMapping = 2,
};

/// Result of one observation. `klid` is zero when `kind` is NoMapping.
pub const outcome_t = struct {
    /// Classification of this observation.
    kind: outcome_kind_t,
    /// Mapped KLID when kind is not NoMapping.
    klid: klid_t,
};

/// Host owner state for the last KeyboardLayout sent on the current connection.
/// Zero is the valid initial value (nothing sent). Owned by the host event-loop thread.
pub const state_t = extern struct {
    /// True once a KeyboardLayout has been sent on this connection.
    has_applied: bool,
    /// KLID of the last sent KeyboardLayout; meaningful only when `has_applied`.
    applied: klid_t,
};

/// C ABI status. Ok is 0; every other value names one rejection.
pub const status_t = enum(c_int) {
    /// Operation succeeded; output arguments were written.
    Ok = 0,
    /// A required pointer argument was NULL.
    NullArgument = 1,
    /// A layout or variant name is empty, too long, or outside the XKB charset.
    InvalidName = 2,
    /// KLID text is not eight non-zero hexadecimal digits.
    InvalidKlid = 3,
};

/// C ABI copy of outcome_t with the kind as a fixed-width byte.
pub const outcome_abi_t = extern struct {
    /// outcome_kind_t value: 0 Send, 1 Unchanged, 2 NoMapping.
    kind: u8,
    /// Mapped KLID, or zero for NoMapping.
    klid: klid_t,
};

const abi_error_t = error{ NullArgument, InvalidName, InvalidKlid };

/// Accept an XKB layout name: 1 through MaxNameBytes bytes from [a-z0-9_].
/// Parameter is a borrowed slice (in). Returns nothing on success.
/// Errors: InvalidName for an empty name, a longer name, or any other byte.
/// Thread safety: pure.
pub fn validate_name(name: []const u8) error_t!void {
    if (name.len == 0 or name.len > MaxNameBytes) return error.InvalidName;
    for (name) |byte| {
        if (!is_layout_byte(byte)) return error.InvalidName;
    }
}

/// Accept an XKB variant name: 0 through MaxNameBytes bytes from [a-z0-9_-].
/// Parameter is a borrowed slice (in). Empty means the layout default.
/// Errors: InvalidName for a longer name or any other byte.
/// Thread safety: pure.
pub fn validate_variant(variant: []const u8) error_t!void {
    if (variant.len > MaxNameBytes) return error.InvalidName;
    for (variant) |byte| {
        if (!is_variant_byte(byte)) return error.InvalidName;
    }
}

/// Parse eight hexadecimal digits into the canonical uppercase KLID form.
/// `text` is a borrowed slice (in) and is not retained.
/// Returns the KLID by value.
/// Errors: InvalidKlid when the length is not eight, a byte is not hexadecimal,
/// or all digits are zero (the all-zero value is not a layout).
/// Thread safety: pure.
pub fn validate_klid(text: []const u8) error_t!klid_t {
    if (text.len != KlidBytes) return error.InvalidKlid;
    var out = std.mem.zeroes(klid_t);
    var nonzero = false;
    for (text, 0..) |byte, index| {
        const digit = hex_value(byte) orelse return error.InvalidKlid;
        if (digit != 0) nonzero = true;
        out.bytes[index] = std.ascii.toUpper(byte);
    }
    if (!nonzero) return error.InvalidKlid;
    return out;
}

/// Find the KLID for an exact (layout, variant) pair in KeyboardLayoutTable.
/// Both names are borrowed slices (in). Returns the KLID, or null when the pair
/// is syntactically valid but has no row.
/// Errors: InvalidName when either name fails validate_name or validate_variant.
/// Thread safety: pure; the table is read-only.
pub fn lookup(layout: []const u8, variant: []const u8) error_t!?klid_t {
    try validate_name(layout);
    try validate_variant(variant);
    for (KeyboardLayoutTable) |entry| {
        if (std.mem.eql(u8, entry.layout, layout) and std.mem.eql(u8, entry.variant, variant)) {
            return klid_t{ .bytes = entry.klid };
        }
    }
    return null;
}

/// Observe the host keyboard layout after a keymap change or on connect.
/// Sends only on change: a mapped KLID equal to the last sent value yields Unchanged.
/// An unmapped pair yields NoMapping and leaves state unchanged, so returning to the
/// previously sent layout is also Unchanged.
/// `state` is in/out. Name slices are borrowed (in) for the call only.
/// Returns the outcome by value.
/// Errors: InvalidName; state is not changed on error.
/// Thread safety: caller serializes calls on one state.
pub fn observe(state: *state_t, layout: []const u8, variant: []const u8) error_t!outcome_t {
    const mapped = (try lookup(layout, variant)) orelse {
        return .{ .kind = .NoMapping, .klid = std.mem.zeroes(klid_t) };
    };
    if (state.has_applied and std.mem.eql(u8, &state.applied.bytes, &mapped.bytes)) {
        return .{ .kind = .Unchanged, .klid = mapped };
    }
    state.* = .{ .has_applied = true, .applied = mapped };
    return .{ .kind = .Send, .klid = mapped };
}

/// Forget the last sent KLID, for example after a disconnect or a new connection.
/// The next mapped observation is Send. `state` is in/out. No error.
/// Thread safety: caller serializes calls on one state.
pub fn reset(state: *state_t) void {
    state.* = std.mem.zeroes(state_t);
}

fn is_layout_byte(byte: u8) bool {
    return (byte >= 'a' and byte <= 'z') or (byte >= '0' and byte <= '9') or byte == '_';
}

fn is_variant_byte(byte: u8) bool {
    return is_layout_byte(byte) or byte == '-';
}

fn hex_value(byte: u8) ?u8 {
    return switch (byte) {
        '0'...'9' => byte - '0',
        'a'...'f' => byte - 'a' + 10,
        'A'...'F' => byte - 'A' + 10,
        else => null,
    };
}

fn status_of(err: abi_error_t) status_t {
    return switch (err) {
        error.NullArgument => .NullArgument,
        error.InvalidName => .InvalidName,
        error.InvalidKlid => .InvalidKlid,
    };
}

/// Borrow `len` bytes from `ptr` for validation, never reading more than max + 1 bytes.
/// Zero length is the empty string and needs no pointer. A longer request is truncated to
/// max + 1 so validation rejects it without reading past the caller's declared length.
fn bytes_arg(ptr: ?[*]const u8, len: usize, max: usize) abi_error_t![]const u8 {
    if (len == 0) return "";
    const p = ptr orelse return error.NullArgument;
    return p[0..@min(len, max + 1)];
}

/// C ABI: validate KLID text of `len` bytes and write its canonical form to `out`.
/// `text` and `out` must be non-NULL when len is nonzero (`out` always).
/// Returns Ok, NullArgument, or InvalidKlid. Writes `out` only on Ok.
/// Thread safety: pure.
export fn waddle_keyboard_layout_validate_klid(text: ?[*]const u8, len: usize, out: ?*klid_t) callconv(.C) status_t {
    const o = out orelse return .NullArgument;
    const bytes = bytes_arg(text, len, KlidBytes) catch |err| return status_of(err);
    o.* = validate_klid(bytes) catch return .InvalidKlid;
    return .Ok;
}

/// C ABI: observe the host layout given XKB layout and variant byte ranges.
/// `layout` must be non-NULL when layout_len is nonzero. `variant` may be NULL
/// with variant_len 0 for the default variant. `state` is inout, `out` is out.
/// Returns Ok, NullArgument, or InvalidName. State and `out` change only on Ok.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_layout_observe(state: ?*state_t, layout: ?[*]const u8, layout_len: usize, variant: ?[*]const u8, variant_len: usize, out: ?*outcome_abi_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    const o = out orelse return .NullArgument;
    const layout_name = bytes_arg(layout, layout_len, MaxNameBytes) catch |err| return status_of(err);
    const variant_name = bytes_arg(variant, variant_len, MaxNameBytes) catch |err| return status_of(err);
    const result = observe(s, layout_name, variant_name) catch |err| return status_of(err);
    o.* = .{ .kind = @intFromEnum(result.kind), .klid = result.klid };
    return .Ok;
}

/// C ABI: clear the last-sent KLID. `state` is inout. Returns Ok or NullArgument.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_layout_reset(state: ?*state_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    reset(s);
    return .Ok;
}

test "layout names accept the XKB charset and reject everything else" {
    try validate_name(rt([]const u8, "us"));
    try validate_name(rt([]const u8, "ara"));
    try validate_name(rt([]const u8, "a_1"));
    try validate_name(rt([]const u8, &[_]u8{'a'} ** MaxNameBytes));
    try std.testing.expectError(error.InvalidName, validate_name(rt([]const u8, "")));
    try std.testing.expectError(error.InvalidName, validate_name(rt([]const u8, &[_]u8{'a'} ** (MaxNameBytes + 1))));
    try std.testing.expectError(error.InvalidName, validate_name(rt([]const u8, "US")));
    try std.testing.expectError(error.InvalidName, validate_name(rt([]const u8, "u-s")));
    try std.testing.expectError(error.InvalidName, validate_name(rt([]const u8, "u s")));
    try std.testing.expectError(error.InvalidName, validate_name(rt([]const u8, "u.s")));
    try std.testing.expectError(error.InvalidName, validate_name("\x00"));
    try std.testing.expectError(error.InvalidName, validate_name("\xff"));
}

test "variant names allow a dash and the empty default" {
    try validate_variant(rt([]const u8, ""));
    try validate_variant(rt([]const u8, "altgr-intl"));
    try validate_variant(rt([]const u8, "nodeadkeys"));
    try validate_variant(rt([]const u8, &[_]u8{'-'} ** MaxNameBytes));
    try std.testing.expectError(error.InvalidName, validate_variant(rt([]const u8, &[_]u8{'-'} ** (MaxNameBytes + 1))));
    try std.testing.expectError(error.InvalidName, validate_variant(rt([]const u8, "Dvorak")));
    try std.testing.expectError(error.InvalidName, validate_variant(rt([]const u8, "a.b")));
    try std.testing.expectError(error.InvalidName, validate_variant(rt([]const u8, "a b")));
}

test "klid text must be eight non-zero hexadecimal digits in canonical uppercase" {
    try std.testing.expectEqualSlices(u8, "00000409", &(try validate_klid(rt([]const u8, "00000409"))).bytes);
    try std.testing.expectEqualSlices(u8, "0000ABCD", &(try validate_klid(rt([]const u8, "0000abcd"))).bytes);
    try std.testing.expectEqualSlices(u8, "00000001", &(try validate_klid(rt([]const u8, "00000001"))).bytes);
    try std.testing.expectError(error.InvalidKlid, validate_klid(rt([]const u8, "0000040")));
    try std.testing.expectError(error.InvalidKlid, validate_klid(rt([]const u8, "000004090")));
    try std.testing.expectError(error.InvalidKlid, validate_klid(rt([]const u8, "")));
    try std.testing.expectError(error.InvalidKlid, validate_klid(rt([]const u8, "0000040g")));
    try std.testing.expectError(error.InvalidKlid, validate_klid(rt([]const u8, "0000 409")));
    try std.testing.expectError(error.InvalidKlid, validate_klid(rt([]const u8, "00000000")));
}

test "every table row is a valid name pair with a canonical KLID and pairs are unique" {
    for (KeyboardLayoutTable, 0..) |entry, index| {
        try validate_name(entry.layout);
        try validate_variant(entry.variant);
        const klid = try validate_klid(&entry.klid);
        try std.testing.expectEqualSlices(u8, &entry.klid, &klid.bytes);
        for (KeyboardLayoutTable[0..index]) |earlier| {
            const same_pair = std.mem.eql(u8, earlier.layout, entry.layout) and std.mem.eql(u8, earlier.variant, entry.variant);
            try std.testing.expect(!same_pair);
        }
    }
    try std.testing.expect(KeyboardLayoutTable.len <= 128);
}

test "lookup maps exact pairs and reports unmapped pairs as null" {
    try std.testing.expectEqualSlices(u8, "00000409", &(try lookup(rt([]const u8, "us"), "")).?.bytes);
    try std.testing.expectEqualSlices(u8, "00020409", &(try lookup(rt([]const u8, "us"), "intl")).?.bytes);
    try std.testing.expectEqualSlices(u8, "00020426", &(try lookup(rt([]const u8, "lv"), "")).?.bytes);
    try std.testing.expect((try lookup(rt([]const u8, "us"), "dvorak")) == null);
    try std.testing.expect((try lookup(rt([]const u8, "de"), "nodeadkeys")) == null);
    try std.testing.expectEqualSlices(u8, "00000416", &(try lookup(rt([]const u8, "br"), "")).?.bytes);
    try std.testing.expect((try lookup(rt([]const u8, "xx"), "")) == null);
    try std.testing.expect((try lookup(rt([]const u8, "de"), "neo")) == null);
    try std.testing.expect((try lookup(rt([]const u8, "us"), "dvorak-x")) == null);
    try std.testing.expectError(error.InvalidName, lookup(rt([]const u8, ""), ""));
    try std.testing.expectError(error.InvalidName, lookup(rt([]const u8, "US"), ""));
    try std.testing.expectError(error.InvalidName, lookup(rt([]const u8, "us"), "Intl"));
}

test "observe sends on first mapping and on change, never on duplicates" {
    var state = state_t{ .has_applied = false, .applied = std.mem.zeroes(klid_t) };
    const first = try observe(&state, rt([]const u8, "us"), "");
    try std.testing.expectEqual(outcome_kind_t.Send, first.kind);
    try std.testing.expectEqualSlices(u8, "00000409", &first.klid.bytes);

    const duplicate = try observe(&state, rt([]const u8, "us"), "");
    try std.testing.expectEqual(outcome_kind_t.Unchanged, duplicate.kind);
    try std.testing.expectEqualSlices(u8, "00000409", &duplicate.klid.bytes);

    const changed = try observe(&state, rt([]const u8, "de"), "");
    try std.testing.expectEqual(outcome_kind_t.Send, changed.kind);
    try std.testing.expectEqualSlices(u8, "00000407", &state.applied.bytes);
}

test "unmapped layouts keep the guest layout and do not reset duplicate suppression" {
    var state = state_t{ .has_applied = false, .applied = std.mem.zeroes(klid_t) };
    _ = try observe(&state, rt([]const u8, "gb"), "");
    const unknown = try observe(&state, rt([]const u8, "xx"), "");
    try std.testing.expectEqual(outcome_kind_t.NoMapping, unknown.kind);
    try std.testing.expectEqualSlices(u8, &[_]u8{0} ** KlidBytes, &unknown.klid.bytes);
    try std.testing.expectEqualSlices(u8, "00000809", &state.applied.bytes);

    const back = try observe(&state, rt([]const u8, "gb"), "");
    try std.testing.expectEqual(outcome_kind_t.Unchanged, back.kind);
    const unknown_variant = try observe(&state, rt([]const u8, "gb"), "dvorak");
    try std.testing.expectEqual(outcome_kind_t.NoMapping, unknown_variant.kind);
}

test "invalid names fail without changing state and reset forces the next send" {
    var state = state_t{ .has_applied = false, .applied = std.mem.zeroes(klid_t) };
    try std.testing.expectError(error.InvalidName, observe(&state, rt([]const u8, ""), ""));
    try std.testing.expectError(error.InvalidName, observe(&state, rt([]const u8, "us"), "BAD"));
    try std.testing.expectEqualDeep(state_t{ .has_applied = false, .applied = std.mem.zeroes(klid_t) }, state);

    _ = try observe(&state, rt([]const u8, "fr"), "");
    reset(&state);
    try std.testing.expect(!state.has_applied);
    try std.testing.expectEqual(outcome_kind_t.Send, (try observe(&state, rt([]const u8, "fr"), "")).kind);
}

test "C ABI validates arguments, maps outcomes and reports every status" {
    var state = std.mem.zeroes(state_t);
    var outcome = outcome_abi_t{ .kind = 0xEE, .klid = .{ .bytes = [_]u8{'X'} ** KlidBytes } };
    const us = "us";
    const null_state = rt(?*state_t, null);
    const null_text = rt(?[*]const u8, null);
    const null_outcome = rt(?*outcome_abi_t, null);
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_layout_observe(null_state, us, us.len, null_text, 0, &outcome));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_layout_observe(&state, us, us.len, null_text, 0, null_outcome));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_layout_observe(&state, null_text, rt(usize, 2), null_text, 0, &outcome));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_layout_observe(&state, us, us.len, null_text, rt(usize, 3), &outcome));
    try std.testing.expectEqual(@as(u8, 0xEE), outcome.kind);

    try std.testing.expectEqual(status_t.InvalidName, waddle_keyboard_layout_observe(&state, us, rt(usize, 0), null_text, 0, &outcome));
    try std.testing.expectEqual(status_t.InvalidName, waddle_keyboard_layout_observe(&state, "U", 1, null_text, 0, &outcome));
    const long = [_]u8{'a'} ** (MaxNameBytes + 1);
    try std.testing.expectEqual(status_t.InvalidName, waddle_keyboard_layout_observe(&state, &long, rt(usize, long.len), null_text, 0, &outcome));
    try std.testing.expectEqual(status_t.InvalidName, waddle_keyboard_layout_observe(&state, us, us.len, "B", 1, &outcome));
    try std.testing.expectEqual(@as(u8, 0xEE), outcome.kind);
    try std.testing.expect(!state.has_applied);

    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_layout_observe(&state, us, us.len, null_text, 0, &outcome));
    try std.testing.expectEqual(@as(u8, @intFromEnum(outcome_kind_t.Send)), outcome.kind);
    try std.testing.expectEqualSlices(u8, "00000409", &outcome.klid.bytes);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_layout_observe(&state, us, us.len, "intl", 4, &outcome));
    try std.testing.expectEqual(@as(u8, @intFromEnum(outcome_kind_t.Send)), outcome.kind);
    try std.testing.expectEqualSlices(u8, "00020409", &outcome.klid.bytes);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_layout_observe(&state, "zz", 2, null_text, 0, &outcome));
    try std.testing.expectEqual(@as(u8, @intFromEnum(outcome_kind_t.NoMapping)), outcome.kind);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_layout_observe(&state, us, us.len, "intl", 4, &outcome));
    try std.testing.expectEqual(@as(u8, @intFromEnum(outcome_kind_t.Unchanged)), outcome.kind);

    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_layout_reset(null_state));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_layout_reset(&state));
    try std.testing.expect(!state.has_applied);

    var klid = std.mem.zeroes(klid_t);
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_layout_validate_klid("00000409", 8, rt(?*klid_t, null)));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_layout_validate_klid(null_text, 8, &klid));
    try std.testing.expectEqual(status_t.InvalidKlid, waddle_keyboard_layout_validate_klid("0000040g", 8, &klid));
    try std.testing.expectEqual(status_t.InvalidKlid, waddle_keyboard_layout_validate_klid("00000409", 7, &klid));
    try std.testing.expectEqual(status_t.InvalidKlid, waddle_keyboard_layout_validate_klid("000004090", 9, &klid));
    try std.testing.expectEqual(status_t.InvalidKlid, waddle_keyboard_layout_validate_klid(null_text, rt(usize, 0), &klid));
    try std.testing.expectEqualSlices(u8, &[_]u8{0} ** KlidBytes, &klid.bytes);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_layout_validate_klid("0000abcd", 8, &klid));
    try std.testing.expectEqualSlices(u8, "0000ABCD", &klid.bytes);
}

// Test-only fixtures.
/// Launders a value through memory the optimizer cannot see, so every error and
/// null path stays a live runtime branch instead of being folded out of the test.
fn rt(comptime T: type, value: T) T {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

/// Reference lookup written as a key comparison, independent of lookup's field-by-field match.
fn reference_key(buffer: []u8, layout: []const u8, variant: []const u8) []const u8 {
    return std.fmt.bufPrint(buffer, "{s}/{s}", .{ layout, variant }) catch unreachable;
}

fn reference_lookup(layout: []const u8, variant: []const u8) ?[KlidBytes]u8 {
    var wanted_buffer: [2 * MaxNameBytes + 1]u8 = undefined;
    const wanted = reference_key(&wanted_buffer, layout, variant);
    for (KeyboardLayoutTable) |entry| {
        var row_buffer: [2 * MaxNameBytes + 1]u8 = undefined;
        if (std.mem.eql(u8, wanted, reference_key(&row_buffer, entry.layout, entry.variant))) return entry.klid;
    }
    return null;
}

fn accepts(layout: []const u8, variant: []const u8) bool {
    validate_name(layout) catch return false;
    validate_variant(variant) catch return false;
    return true;
}

const random_names = [_][]const u8{ "us", "gb", "de", "fr", "xx", "zz", "", "US", "ch", "pt", "ara" };
const random_variants = [_][]const u8{ "", "intl", "dvorak", "nodeadkeys", "neo", "BAD", "a b" };

test "randomized observations match a reference model over long sequences" {
    var prng = std.Random.DefaultPrng.init(0x6b6c6179);
    const random = prng.random();
    var state = state_t{ .has_applied = false, .applied = std.mem.zeroes(klid_t) };
    var reference_applied: ?[KlidBytes]u8 = null;
    for (0..20000) |_| {
        if (random.uintLessThan(u8, 64) == 0) {
            reset(&state);
            reference_applied = null;
            continue;
        }
        const layout = random_names[random.uintLessThan(usize, random_names.len)];
        const variant = random_variants[random.uintLessThan(usize, random_variants.len)];
        const valid = accepts(layout, variant);
        if (!valid) {
            const before = state;
            try std.testing.expectError(error.InvalidName, observe(&state, layout, variant));
            try std.testing.expectEqualDeep(before, state);
            continue;
        }
        const mapped = reference_lookup(layout, variant);
        const outcome = try observe(&state, layout, variant);
        if (mapped) |bytes| {
            const expected: outcome_kind_t = if (reference_applied) |applied|
                (if (std.mem.eql(u8, &applied, &bytes)) .Unchanged else .Send)
            else
                .Send;
            try std.testing.expectEqual(expected, outcome.kind);
            try std.testing.expectEqualSlices(u8, &bytes, &outcome.klid.bytes);
            reference_applied = bytes;
        } else {
            try std.testing.expectEqual(outcome_kind_t.NoMapping, outcome.kind);
        }
        if (reference_applied) |applied| {
            try std.testing.expectEqualSlices(u8, &applied, &state.applied.bytes);
        } else {
            try std.testing.expect(!state.has_applied);
        }
    }
}

test "ABI struct sizes match the C header layouts" {
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(klid_t));
    try std.testing.expectEqual(@as(usize, 9), @sizeOf(state_t));
    try std.testing.expectEqual(@as(usize, 9), @sizeOf(outcome_abi_t));
}
