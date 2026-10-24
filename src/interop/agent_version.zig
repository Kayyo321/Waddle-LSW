//! Bounded Semantic Version 2 parser for peer-advertised agent versions.
//!
//! Parsing is allocation-free and borrows the input only for the duration of
//! the call. The module retains no pointers and has no shared mutable state.

const std_t = @import("std");

const MinVersionBytes = 5;
const MaxVersionBytes = 32;
const MaxMajor = std_t.math.maxInt(u32);

/// Parse a bounded SemVer 2 value and return its numeric major component.
///
/// @param[in] version Borrowed, readable byte slice. It must contain exactly
/// 5..32 ASCII bytes; it is not NUL-terminated and is not modified.
/// @return The copied major value, including zero, or `error.InvalidVersion`
/// for malformed syntax, an out-of-range major, or a value outside the bound.
/// @note Performs no allocation, retains no pointer, and is thread-safe when
/// callers provide independently valid readable slices.
pub fn parse_major(version: []const u8) error{InvalidVersion}!u32 {
    if (version.len < MinVersionBytes or version.len > MaxVersionBytes)
        return error.InvalidVersion;

    var cursor: usize = 0;
    var major: u32 = 0;
    var major_digits: usize = 0;
    while (cursor < version.len and is_digit(version[cursor])) : (cursor += 1) {
        const digit: u32 = version[cursor] - '0';
        if (major > (MaxMajor - digit) / 10) return error.InvalidVersion;
        major = major * 10 + digit;
        major_digits += 1;
    }
    if (major_digits == 0 or !valid_numeric_end(version, 0, cursor))
        return error.InvalidVersion;
    if (cursor >= version.len or version[cursor] != '.') return error.InvalidVersion;
    cursor += 1;

    cursor = try parse_core_number(version, cursor);
    if (cursor >= version.len or version[cursor] != '.') return error.InvalidVersion;
    cursor = try parse_core_number(version, cursor + 1);

    if (cursor < version.len and version[cursor] == '-') {
        cursor = try parse_identifiers(version, cursor + 1, true, true);
    }
    if (cursor < version.len and version[cursor] == '+') {
        cursor = try parse_identifiers(version, cursor + 1, false, false);
    }
    if (cursor != version.len) return error.InvalidVersion;
    return major;
}

fn parse_core_number(version: []const u8, start: usize) error{InvalidVersion}!usize {
    var cursor = start;
    while (cursor < version.len and is_digit(version[cursor])) : (cursor += 1) {}
    if (cursor == start or !valid_numeric_end(version, start, cursor))
        return error.InvalidVersion;
    return cursor;
}

fn valid_numeric_end(version: []const u8, start: usize, end: usize) bool {
    return end > start and (end - start == 1 or version[start] != '0');
}

/// Parse nonempty dot-separated prerelease or build identifiers.
/// `allow_plus` permits the prerelease-to-build separator at the end.
fn parse_identifiers(version: []const u8, start: usize, prerelease: bool, allow_plus: bool) error{InvalidVersion}!usize {
    var cursor = start;
    while (true) {
        const identifier_start = cursor;
        var numeric_only = true;
        while (cursor < version.len and is_identifier_byte(version[cursor])) : (cursor += 1) {
            if (!is_digit(version[cursor])) numeric_only = false;
        }
        if (cursor == identifier_start) return error.InvalidVersion;
        if (prerelease and numeric_only and cursor - identifier_start > 1 and version[identifier_start] == '0')
            return error.InvalidVersion;
        if (cursor == version.len) return cursor;
        if (version[cursor] == '.') {
            cursor += 1;
            continue;
        }
        if (allow_plus and version[cursor] == '+') return cursor;
        return error.InvalidVersion;
    }
}

fn is_digit(byte: u8) bool {
    return byte >= '0' and byte <= '9';
}

fn is_identifier_byte(byte: u8) bool {
    return is_digit(byte) or (byte >= 'A' and byte <= 'Z') or
        (byte >= 'a' and byte <= 'z') or byte == '-';
}

// Test-only fixtures.
const testing_t = std_t.testing;

fn expect_invalid(version: []const u8) !void {
    try testing_t.expectError(error.InvalidVersion, parse_major(version));
}

test "SemVer major parser accepts core and metadata grammar" {
    try testing_t.expectEqual(@as(u32, 0), try parse_major("0.0.0"));
    try testing_t.expectEqual(std_t.math.maxInt(u32), try parse_major("4294967295.1.2"));
    try testing_t.expectEqual(@as(u32, 7), try parse_major("7.123456789012.123456789012"));
    try testing_t.expectEqual(@as(u32, 1), try parse_major("1.2.3-alpha.1-x+build.001"));
    try testing_t.expectEqual(@as(u32, 1), try parse_major("1.2.3+001.alpha-2"));
    try testing_t.expectEqual(@as(u32, 1), try parse_major("1.2.3-0"));
    try testing_t.expectEqual(@as(u32, 1), try parse_major("1.2.3-0a"));
    try testing_t.expectEqual(@as(u32, 1), try parse_major("1.2.3-00a"));
}

test "SemVer parser rejects malformed core and metadata" {
    const invalid = [_][]const u8{
        "",               "1.2",                          "1..2",                    "1.2.",         ".....",                   "12345",    "1x2.3",
        "1.234567",       "1.222.",                       "01.2.3",                  "1.02.3",       "1.2.03",                  "1.2.3-",   "1.2.3+",
        "1.2.3-alpha.",   "1.2.3-.alpha",                 "1.2.3+alpha..b",          "1.2.3-alpha+", "1.2.3-alpha+build+again", "1.2.3-01", "1.2.3-00.alpha",
        "1.2.3_foo",      "1.2.3+has space",              "1.2.3+nonascii-\xC3\xA9", "1.2.3\x00",    "1.2.3 ",                  " 1.2.3",   "1.2.3.4",
        "4294967296.0.0", "999999999999999999999999.0.0",
    };
    for (invalid) |version| try expect_invalid(version);
    try expect_invalid("0.0.0"[0..4]);
    const too_long = "1.2.3+123456789012345678901234567890";
    try expect_invalid(too_long);
}

test "exact maximum size and repeated parsing stay allocation-free" {
    const exact_max = "1.2.3+12345678901234567890123456";
    try testing_t.expectEqual(MaxVersionBytes, exact_max.len);
    var iteration: usize = 0;
    while (iteration < 1000) : (iteration += 1) {
        try testing_t.expectEqual(@as(u32, 1), try parse_major(exact_max));
    }
}
