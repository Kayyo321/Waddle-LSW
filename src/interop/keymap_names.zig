//! XKB keymap name extraction for keyboard lock sync. Reads keymap text in the
//! xkb_v1 format that a Wayland compositor sends in wl_keyboard.keymap and returns
//! the first group's layout and variant (xkb_symbols include line) and the Caps,
//! Num and Scroll lock modifier indices (xkb_compat modifier_map statements).
//! Pure and allocation-free: no heap, no regex, no recursion. Every scan cursor
//! only moves forward, so work is linear in the input length.
const std = @import("std");

/// Maximum keymap text length accepted by parse, in bytes.
pub const MaxKeymapBytes: usize = 262144;
/// Maximum layout or variant name length, in bytes.
pub const MaxNameBytes: usize = 64;
/// Modifier index meaning the keymap did not resolve a lock modifier.
pub const ModifierAbsent: u8 = 0xFF;

/// Explicit rejections. `out` is unchanged when one is returned.
/// TooLarge: text is longer than MaxKeymapBytes.
/// MalformedName: a present layout or variant breaks its charset or length rule,
/// or a variant's opening '(' has no matching ')' inside its segment.
pub const error_t = error{ TooLarge, MalformedName };

/// Extracted keymap names. Plain data with no pointers. Zero-filled fields mean absent.
pub const names_t = extern struct {
    /// 1 when a layout was found, else 0.
    has_layout: u8,
    /// Byte length of `layout`; 0 when absent.
    layout_len: u8,
    /// Layout bytes, [a-z][a-z0-9_]*; zero past layout_len.
    layout: [MaxNameBytes]u8,
    /// 1 when a non-empty variant was found, else 0.
    has_variant: u8,
    /// Byte length of `variant`; 0 when absent.
    variant_len: u8,
    /// Variant bytes, [A-Za-z0-9_-]*; zero past variant_len.
    variant: [MaxNameBytes]u8,
    /// Core modifier index of Caps (Shift=0, Lock=1, Control=2, Mod1=3 .. Mod5=7), or ModifierAbsent.
    caps: u8,
    /// Core modifier index of Num lock, or ModifierAbsent.
    num: u8,
    /// Core modifier index of Scroll lock, or ModifierAbsent.
    scroll: u8,
    /// 1 when no modifier_map statement was recognized and the fixed fallback
    /// (caps=1, num=4, scroll=ModifierAbsent) was written; else 0.
    fallback_used: u8,
};

/// Core modifier names in index order; the array position is the XKB core index.
const ModifierNames = [_][]const u8{ "Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5" };
/// Include-string segment names that are options, not layouts.
const OptionNames = [_][]const u8{
    "inet",   "ctrl",    "terminate", "compose", "lv3",     "lv5",  "group",   "caps",
    "kpdl",   "altwin",  "srvr",      "misc",    "numpad",  "nbsp", "eurosign", "keypad",
    "shift",  "level3",  "level5",    "japan",   "korean",
};
const SymbolsToken = "xkb_symbols";
const IncludeToken = "include";
const ModifierMapToken = "modifier_map";
const CapsKey = "<CAPS>";
const NumKey = "<NMLK>";
const ScrollKey = "<SCLK>";
/// Fallback indices used only when the keymap has no recognized modifier_map statement.
const FallbackCaps: u8 = 1;
const FallbackNum: u8 = 4;
const FallbackScroll: u8 = ModifierAbsent;

/// Status of one lock key across all modifier_map statements.
const key_state_t = struct {
    /// True once a resolvable statement names the key.
    seen: bool = false,
    /// Core index from the first resolvable statement naming the key.
    index: u8 = ModifierAbsent,
    /// True when two resolvable statements name the key with different indices.
    conflict: bool = false,
};

/// Parses keymap text and writes the extracted names to `out` on success.
/// @param text in, borrowed for the call only; may be empty; must be at most MaxKeymapBytes.
/// @param out out, non-null, written only on success and never read; caller owns it.
/// @return void on success with every field of out written (zero-filled when absent).
/// error.TooLarge when text is longer than MaxKeymapBytes (checked before any scan).
/// error.MalformedName when a present layout or variant name is invalid.
/// Allocates nothing. Thread safe: a pure function of its arguments.
pub fn parse(text: []const u8, out: *names_t) error_t!void {
    if (text.len > MaxKeymapBytes) return error.TooLarge;
    var result = std.mem.zeroes(names_t);
    try parse_layout(text, &result);
    parse_modifiers(text, &result);
    out.* = result;
}

/// C ABI entry point over parse. Stateless; safe to call concurrently with distinct `out`.
/// @param text in, may be NULL only when length is 0; otherwise length bytes readable for the call.
/// @param length in, byte count of text.
/// @param out out, non-NULL, writable for sizeof(names_t) bytes; unchanged on any nonzero return.
/// @return 0 success; 1 NULL argument (out NULL, or text NULL with length > 0); 2 length above
/// MaxKeymapBytes; 3 malformed name. Allocates nothing.
export fn waddle_keymap_names_parse(text: ?[*]const u8, length: usize, out: ?*names_t) callconv(.C) c_int {
    const out_ptr = out orelse return 1;
    if (length > 0 and text == null) return 1;
    if (length > MaxKeymapBytes) return 2;
    // The length check above precedes slice formation, so parse cannot report TooLarge here.
    const bytes: []const u8 = if (length == 0) &.{} else text.?[0..length];
    parse(bytes, out_ptr) catch return 3;
    return 0;
}

/// Fills the layout fields from the first non-option segment after the base segment.
/// A segment's name ends at the first ':' or '('. Errors leave `result` partial; parse discards it.
fn parse_layout(text: []const u8, result: *names_t) error_t!void {
    const include = find_symbols_include(text) orelse return;
    var segments = std.mem.splitScalar(u8, include, '+');
    // Segment 0 is the base keymap (for example "pc"), never a layout.
    _ = segments.next();
    while (segments.next()) |segment| {
        const name_end = std.mem.indexOfAny(u8, segment, ":(") orelse segment.len;
        const name = segment[0..name_end];
        if (is_option(name)) continue;
        if (!is_layout_name(name)) return error.MalformedName;
        result.has_layout = 1;
        result.layout_len = @intCast(name.len);
        @memcpy(result.layout[0..name.len], name);
        return parse_variant(segment, result);
    }
    return;
}

/// Returns the bytes between the quotes of the first include directive that follows the
/// xkb_symbols token. Returns null when the token is absent, no quoted include follows it,
/// or the quote is never closed. Scanning does not allocate.
fn find_symbols_include(text: []const u8) ?[]const u8 {
    const section = std.mem.indexOf(u8, text, SymbolsToken) orelse return null;
    var search = section + SymbolsToken.len;
    while (std.mem.indexOfPos(u8, text, search, IncludeToken)) |found| {
        const cursor = skip_space(text, found + IncludeToken.len);
        if (cursor < text.len and text[cursor] == '"') {
            const close = std.mem.indexOfScalarPos(u8, text, cursor + 1, '"') orelse return null;
            return text[cursor + 1 .. close];
        }
        search = found + 1;
    }
    return null;
}

/// Fills the variant fields from the parenthesized text of one layout segment.
/// An empty "()" is treated as absent. Text after ')' and any group index are ignored.
fn parse_variant(segment: []const u8, result: *names_t) error_t!void {
    const open = std.mem.indexOfScalar(u8, segment, '(') orelse return;
    const close = std.mem.indexOfScalarPos(u8, segment, open + 1, ')') orelse return error.MalformedName;
    const variant = segment[open + 1 .. close];
    if (variant.len == 0) return;
    if (!is_variant_name(variant)) return error.MalformedName;
    result.has_variant = 1;
    result.variant_len = @intCast(variant.len);
    @memcpy(result.variant[0..variant.len], variant);
}

/// Resolves caps, num and scroll from `modifier_map <Name> { ... };` statements.
/// A statement is recognized when the token has no identifier byte before it, is followed by
/// whitespace and an identifier, then '{' and a closing '}'. Statements naming a modifier
/// outside ModifierNames are recognized but resolve no key. Lock keys are then resolved
/// per key: ABSENT when unseen or ambiguous. Fallback: when no statement is recognized,
/// caps=1, num=4, scroll=255 and fallback_used=1 (the keymap is then assumed to be a
/// standard evdev/xfree86 layout).
fn parse_modifiers(text: []const u8, result: *names_t) void {
    var caps: key_state_t = .{};
    var num: key_state_t = .{};
    var scroll: key_state_t = .{};
    var statements: usize = 0;
    var search: usize = 0;
    while (std.mem.indexOfPos(u8, text, search, ModifierMapToken)) |found| {
        search = found + ModifierMapToken.len;
        if (found > 0 and is_identifier_byte(text[found - 1])) continue;
        if (search >= text.len or !is_space(text[search])) continue;
        const name_start = skip_space(text, search);
        const name_end = identifier_end(text, name_start);
        if (name_end == name_start) continue;
        const open = skip_space(text, name_end);
        if (open >= text.len or text[open] != '{') continue;
        // With no '}' after this point, no later statement can close either.
        const close = std.mem.indexOfScalarPos(u8, text, open + 1, '}') orelse break;
        statements += 1;
        if (modifier_index(text[name_start..name_end])) |index| {
            const body = text[open + 1 .. close];
            if (std.mem.indexOf(u8, body, CapsKey) != null) record_key(&caps, index);
            if (std.mem.indexOf(u8, body, NumKey) != null) record_key(&num, index);
            if (std.mem.indexOf(u8, body, ScrollKey) != null) record_key(&scroll, index);
        }
        search = close + 1;
    }
    if (statements == 0) {
        result.caps = FallbackCaps;
        result.num = FallbackNum;
        result.scroll = FallbackScroll;
        result.fallback_used = 1;
        return;
    }
    result.caps = resolved_index(caps);
    result.num = resolved_index(num);
    result.scroll = resolved_index(scroll);
    result.fallback_used = 0;
}

/// Records one resolvable statement's index for a key. Repeats of the same index are not conflicts.
fn record_key(state: *key_state_t, index: u8) void {
    if (!state.seen) {
        state.seen = true;
        state.index = index;
    } else if (state.index != index) {
        state.conflict = true;
    }
}

/// Returns the key's index, or ModifierAbsent when the key is unseen or ambiguous.
fn resolved_index(state: key_state_t) u8 {
    if (!state.seen or state.conflict) return ModifierAbsent;
    return state.index;
}

/// Returns the XKB core index of a modifier name in ModifierNames, or null for any other name.
fn modifier_index(name: []const u8) ?u8 {
    for (ModifierNames, 0..) |candidate, index| {
        if (std.mem.eql(u8, candidate, name)) return @as(u8, @intCast(index));
    }
    return null;
}

/// True when `name` is one of OptionNames.
fn is_option(name: []const u8) bool {
    for (OptionNames) |option| {
        if (std.mem.eql(u8, option, name)) return true;
    }
    return false;
}

/// True when `name` matches [a-z][a-z0-9_]* and is at most MaxNameBytes long.
fn is_layout_name(name: []const u8) bool {
    if (name.len == 0 or name.len > MaxNameBytes) return false;
    if (!is_lower(name[0])) return false;
    for (name[1..]) |byte| {
        if (!(is_lower(byte) or is_digit(byte) or byte == '_')) return false;
    }
    return true;
}

/// True when `variant` matches [A-Za-z0-9_-]* and is at most MaxNameBytes long.
fn is_variant_name(variant: []const u8) bool {
    if (variant.len > MaxNameBytes) return false;
    for (variant) |byte| {
        if (!(is_lower(byte) or is_upper(byte) or is_digit(byte) or byte == '_' or byte == '-')) return false;
    }
    return true;
}

/// Returns the index of the first byte at or after `start` that is not whitespace, or text.len.
fn skip_space(text: []const u8, start: usize) usize {
    var index = start;
    while (index < text.len and is_space(text[index])) index += 1;
    return index;
}

/// Returns the index one past the identifier run ([A-Za-z0-9_]*) that begins at `start`.
fn identifier_end(text: []const u8, start: usize) usize {
    var index = start;
    while (index < text.len and is_identifier_byte(text[index])) index += 1;
    return index;
}

fn is_space(byte: u8) bool {
    return byte == ' ' or byte == '\t' or byte == '\n' or byte == '\r';
}

fn is_lower(byte: u8) bool {
    return byte >= 'a' and byte <= 'z';
}

fn is_upper(byte: u8) bool {
    return byte >= 'A' and byte <= 'Z';
}

fn is_digit(byte: u8) bool {
    return byte >= '0' and byte <= '9';
}

fn is_identifier_byte(byte: u8) bool {
    return is_lower(byte) or is_upper(byte) or is_digit(byte) or byte == '_';
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const FixtureText = "xkb_symbols \"x\" {\n    include \"pc+us+de:2+inet(evdev)\"\n};\n" ++
    "xkb_compat \"x\" {\n    modifier_map Mod2 { <NMLK> };\n    modifier_map Mod5 { <SCLK> };\n    modifier_map Lock { <CAPS> };\n};\n";

var large_storage: [MaxKeymapBytes + 1]u8 = undefined;

fn fresh_out() names_t {
    var out: names_t = undefined;
    @memset(std.mem.asBytes(&out), 0xA5);
    return out;
}

fn snapshot(value: *const names_t) [@sizeOf(names_t)]u8 {
    return std.mem.asBytes(value).*;
}

fn symbols_with(comptime include: []const u8) []const u8 {
    return "xkb_symbols \"(unnamed)\" {\n    include \"" ++ include ++ "\"\n};\n";
}

fn abi_call(text: []const u8, out: ?*names_t) c_int {
    return waddle_keymap_names_parse(text.ptr, text.len, out);
}

fn expect_layout(text: []const u8, layout: ?[]const u8, variant: ?[]const u8) !void {
    var out = fresh_out();
    try parse(text, &out);
    if (layout) |expected| {
        try std.testing.expectEqual(@as(u8, 1), out.has_layout);
        try std.testing.expectEqualStrings(expected, out.layout[0..out.layout_len]);
    } else {
        try std.testing.expectEqual(@as(u8, 0), out.has_layout);
        try std.testing.expectEqual(@as(u8, 0), out.layout_len);
        try std.testing.expect(std.mem.allEqual(u8, &out.layout, 0));
    }
    if (variant) |expected| {
        try std.testing.expectEqual(@as(u8, 1), out.has_variant);
        try std.testing.expectEqualStrings(expected, out.variant[0..out.variant_len]);
    } else {
        try std.testing.expectEqual(@as(u8, 0), out.has_variant);
        try std.testing.expectEqual(@as(u8, 0), out.variant_len);
        try std.testing.expect(std.mem.allEqual(u8, &out.variant, 0));
    }
}

fn expect_modifiers(text: []const u8, caps: u8, num: u8, scroll: u8, fallback: u8) !void {
    var out = fresh_out();
    try parse(text, &out);
    try std.testing.expectEqual(caps, out.caps);
    try std.testing.expectEqual(num, out.num);
    try std.testing.expectEqual(scroll, out.scroll);
    try std.testing.expectEqual(fallback, out.fallback_used);
}

fn expect_malformed(text: []const u8) !void {
    var out = fresh_out();
    const before = snapshot(&out);
    try std.testing.expectError(error.MalformedName, parse(text, &out));
    try std.testing.expect(std.mem.eql(u8, &before, std.mem.asBytes(&out)));
}

/// Runs parse on arbitrary bytes and enforces the contract: success, or an error with out unchanged.
fn check_contract(input: []const u8) !void {
    var out = fresh_out();
    const before = snapshot(&out);
    parse(input, &out) catch |err| {
        try std.testing.expect(err == error.MalformedName or err == error.TooLarge);
        try std.testing.expect(std.mem.eql(u8, &before, std.mem.asBytes(&out)));
    };
}

fn random_byte(random: std.Random) u8 {
    const alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+:()\"{};<> \n";
    if (random.uintLessThan(u8, 4) == 0) return random.int(u8);
    return alphabet[random.uintLessThan(usize, alphabet.len)];
}

test "layout and variant come from the first non-option layout segment" {
    try expect_layout(symbols_with("pc+us+de:2+inet(evdev)"), "us", null);
    try expect_layout(symbols_with("pc+de(nodeadkeys)+inet(evdev)"), "de", "nodeadkeys");
    try expect_layout(symbols_with("pc+inet(evdev)+gb"), "gb", null);
    try expect_layout(symbols_with("pc+ctrl(nocaps)+group(x)+us:2(intl)"), "us", "intl");
    try expect_layout(symbols_with("pc+us()"), "us", null);
    try expect_layout(symbols_with("pc+us(a-b_9)"), "us", "a-b_9");
    try expect_layout(FixtureText, "us", null);
}

test "every option name is skipped" {
    try expect_layout(symbols_with("pc+inet+ctrl+terminate+compose+lv3+lv5+group+caps+kpdl+altwin+srvr+misc+numpad+nbsp+eurosign+keypad+shift+level3+level5+japan+korean"), null, null);
}

test "absent include line is not an error" {
    try expect_layout("", null, null);
    try expect_layout("xkb_symbols \"x\" { };", null, null);
    try expect_layout("xkb_symbols { }", null, null);
    try expect_layout(symbols_with("pc"), null, null);
    try expect_layout(symbols_with("pc+inet(evdev)+ctrl"), null, null);
    try expect_layout("xkb_symbols { include \"pc+us", null, null);
    try expect_layout("xkb_symbols { include pc; }", null, null);
    try expect_layout("include \"pc+us\" xkb_symbols { }", null, null);
    try expect_layout("xkb_symbols { include pc; include \"pc+fr\" };", "fr", null);
}

test "layout name length boundary and malformed names are errors" {
    try expect_layout(symbols_with("pc+" ++ ("a" ** 64)), "a" ** 64, null);
    try expect_malformed(symbols_with("pc+" ++ ("a" ** 65)));
    try expect_malformed(symbols_with("pc+US"));
    try expect_malformed(symbols_with("pc+1us"));
    try expect_malformed(symbols_with("pc+u.s"));
    try expect_malformed(symbols_with("pc+:2"));
    try expect_malformed(symbols_with("pc+us(a b)"));
    try expect_malformed(symbols_with("pc+us(abc"));
    try expect_malformed(symbols_with("pc+us(" ++ ("v" ** 65) ++ ")"));
}

test "modifier discovery resolves caps, num and scroll" {
    try expect_modifiers(FixtureText, 1, 4, 7, 0);
    try expect_modifiers("modifier_map Mod2 { <NMLK> }; modifier_map Mod2 { <NMLK> };", 255, 4, 255, 0);
    try expect_modifiers("modifier_map Foo { <SCLK> }; modifier_map Mod4 { <NMLK> };", 255, 6, 255, 0);
    try expect_modifiers("modifier_map Lock { <CAPS> };", 1, 255, 255, 0);
}

test "a key mapped to two different indices is ABSENT" {
    try expect_modifiers("modifier_map Mod2 { <NMLK> }; modifier_map Mod3 { <NMLK> };", 255, 255, 255, 0);
}

test "no recognized modifier_map falls back to fixed indices" {
    try expect_modifiers("xkb_compat { };", 1, 4, 255, 1);
    try expect_modifiers("modifier_map Mod2 { <NMLK> };\nmodifier_map Mod2 { <NMLK> }", 255, 4, 255, 0);
    try expect_modifiers("xmodifier_map Mod2 { <NMLK> };", 1, 4, 255, 1);
    try expect_modifiers("modifier_map_x Mod2 { <NMLK> };", 1, 4, 255, 1);
    try expect_modifiers("modifier_map{ <NMLK> };", 1, 4, 255, 1);
    try expect_modifiers("modifier_map { <NMLK> };", 1, 4, 255, 1);
    try expect_modifiers("modifier_map", 1, 4, 255, 1);
    try expect_modifiers("modifier_map Mod2 <NMLK>;", 1, 4, 255, 1);
    try expect_modifiers("modifier_map Mod2", 1, 4, 255, 1);
    try expect_modifiers("modifier_map Mod2 { <NMLK> ;", 1, 4, 255, 1);
}

test "TooLarge at MaxKeymapBytes + 1 and acceptance at MaxKeymapBytes" {
    @memset(&large_storage, ' ');
    const prefix = "xkb_symbols { include \"pc+gb\" };";
    std.mem.copyForwards(u8, &large_storage, prefix);
    var out = fresh_out();
    const before = snapshot(&out);
    try std.testing.expectError(error.TooLarge, parse(large_storage[0 .. MaxKeymapBytes + 1], &out));
    try std.testing.expect(std.mem.eql(u8, &before, std.mem.asBytes(&out)));
    try parse(large_storage[0..MaxKeymapBytes], &out);
    try std.testing.expectEqualStrings("gb", out.layout[0..out.layout_len]);
}

test "C ABI maps every failure to its status code" {
    var out = fresh_out();
    const before = snapshot(&out);
    try std.testing.expectEqual(@as(c_int, 1), waddle_keymap_names_parse(null, 3, &out));
    try std.testing.expectEqual(@as(c_int, 1), abi_call("", null));
    try std.testing.expectEqual(@as(c_int, 2), abi_call(large_storage[0 .. MaxKeymapBytes + 1], &out));
    try std.testing.expectEqual(@as(c_int, 3), abi_call(symbols_with("pc+US"), &out));
    try std.testing.expect(std.mem.eql(u8, &before, std.mem.asBytes(&out)));
    try std.testing.expectEqual(@as(c_int, 0), waddle_keymap_names_parse(null, 0, &out));
    try std.testing.expectEqual(@as(u8, 0), out.has_layout);
    try std.testing.expectEqual(@as(c_int, 0), abi_call("xkb_symbols { include \"pc+gb\" };", &out));
    try std.testing.expectEqualStrings("gb", out.layout[0..out.layout_len]);
}

test "fuzz: random inputs never panic and honor the error contract" {
    var prng = std.Random.DefaultPrng.init(0x6b657968_6d617073);
    const random = prng.random();
    var buf: [512]u8 = undefined;
    var iteration: usize = 0;
    while (iteration < 20000) : (iteration += 1) {
        const len = random.intRangeAtMost(usize, 0, buf.len);
        for (buf[0..len]) |*byte| byte.* = random_byte(random);
        try check_contract(buf[0..len]);
    }
}

test "mutation: each byte of a valid keymap set to 8 random values never panics" {
    var prng = std.Random.DefaultPrng.init(0x6d757461_74696f6e);
    const random = prng.random();
    var buf: [FixtureText.len]u8 = undefined;
    std.mem.copyForwards(u8, &buf, FixtureText);
    try expect_layout(&buf, "us", null);
    for (0..FixtureText.len) |position| {
        var round: usize = 0;
        while (round < 8) : (round += 1) {
            var mutated = buf;
            mutated[position] = random.int(u8);
            try check_contract(&mutated);
        }
    }
}
