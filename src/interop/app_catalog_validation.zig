const std = @import("std");
const MaxUtf8Bytes = 4096;
const MaxPackageFamilyBytes = 50;
const MaxPathUnits = 32767;

fn pointer_end_valid(pointer: [*]const u8, length: usize) bool {
    return length <= std.math.maxInt(usize) - @intFromPtr(pointer);
}

/// Validate one borrowed UTF-8 string for a C catalog call.
///
/// `data` is borrowed and is never retained. `maximum` is at most 4096. A null
/// pointer is valid only for an empty string when `allow_empty` is true.
/// Embedded NUL and malformed, overlong, surrogate, or out-of-range code
/// points are rejected. End-pointer overflow is checked before slicing.
export fn app_catalog_utf8_valid(
    data: ?[*]const u8,
    length: usize,
    maximum: usize,
    allow_empty: bool,
) bool {
    if (maximum > MaxUtf8Bytes or length > maximum or (!allow_empty and length == 0)) return false;
    if (length == 0) return true;
    const pointer = data orelse return false;
    if (!pointer_end_valid(pointer, length)) return false;
    const bytes = pointer[0..length];
    var index: usize = 0;
    while (index < bytes.len) {
        const first = bytes[index];
        index += 1;
        if (first == 0) return false;
        if (first < 0x80) continue;

        var codepoint: u32 = undefined;
        var continuation_count: usize = undefined;
        if (first >= 0xC2 and first <= 0xDF) {
            codepoint = first & 0x1F;
            continuation_count = 1;
        } else if (first >= 0xE0 and first <= 0xEF) {
            codepoint = first & 0x0F;
            continuation_count = 2;
        } else if (first >= 0xF0 and first <= 0xF4) {
            codepoint = first & 0x07;
            continuation_count = 3;
        } else return false;
        if (bytes.len - index < continuation_count) return false;
        var continuation: usize = 0;
        while (continuation < continuation_count) : (continuation += 1) {
            const next = bytes[index];
            index += 1;
            if ((next & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if ((continuation_count == 2 and codepoint < 0x800) or
            (continuation_count == 3 and codepoint < 0x10000) or
            (codepoint >= 0xD800 and codepoint <= 0xDFFF) or codepoint > 0x10FFFF)
            return false;
    }
    return true;
}

/// Validate the bounded package-family syntax used only to derive Packaged.
///
/// `data` is borrowed for the call and is not retained. Empty input means the
/// observation had no family name and is valid. Input is capped at 50 bytes;
/// nonempty input must be
/// `name_publisher` with a 1..36 byte ASCII name and 13 lowercase ASCII
/// alphanumeric publisher bytes.
export fn app_catalog_package_family_valid(data: ?[*]const u8, length: usize) bool {
    if (length == 0) return true;
    if (length > MaxPackageFamilyBytes) return false;
    const pointer = data orelse return false;
    if (!pointer_end_valid(pointer, length)) return false;
    const bytes = pointer[0..length];
    var separator = length;
    for (bytes, 0..) |byte, index| {
        if (byte == '_') {
            if (separator != length) return false;
            separator = index;
        }
    }
    if (separator == length or separator < 1 or separator > 36) return false;
    if (length - separator - 1 != 13) return false;
    for (bytes[0..separator]) |byte| {
        const valid = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
            (byte >= '0' and byte <= '9') or byte == '.' or byte == '-';
        if (!valid) return false;
    }
    for (bytes[separator + 1 ..]) |byte| {
        if (!((byte >= 'a' and byte <= 'z') or (byte >= '0' and byte <= '9'))) return false;
    }
    return true;
}

fn path_unit(bytes: []const u8, index: usize) u16 {
    return @as(u16, bytes[index * 2]) | (@as(u16, bytes[index * 2 + 1]) << 8);
}

/// Validate uppercase UTF-16LE canonical drive/UNC path bytes.
///
/// `data` is borrowed for the call and is never retained. The maximum is capped
/// at 32767 units. The byte slice must be nonempty, even length, NUL-free,
/// absolute, without ASCII lowercase code units, well-formed UTF-16, and
/// contain no repeated, `.` or `..` path components. This checks byte syntax
/// only; Windows must derive Unicode invariant uppercase identity from the
/// target's final path by handle. End-pointer overflow is rejected before slice.
export fn app_catalog_path_identity_valid(data: ?[*]const u8, length: usize, maximum_units: usize) bool {
    if (maximum_units > MaxPathUnits) return false;
    if (length == 0 or length > maximum_units * 2 or (length & 1) != 0) return false;
    const pointer = data orelse return false;
    if (!pointer_end_valid(pointer, length)) return false;
    const bytes = pointer[0..length];
    const units = length / 2;
    if (units < 3) return false;
    const first = path_unit(bytes, 0);
    const second = path_unit(bytes, 1);
    const drive_path = first >= 'A' and first <= 'Z' and second == ':';
    const unc_path = first == '\\' and second == '\\';
    if (!drive_path and !unc_path) return false;
    if (drive_path and path_unit(bytes, 2) != '\\') return false;

    var component_start: usize = if (drive_path) 3 else 2;
    var component_count: usize = 0;
    var index: usize = 0;
    while (index < units) : (index += 1) {
        const unit = path_unit(bytes, index);
        if (unit == 0 or unit == '/' or (unit >= 'a' and unit <= 'z')) return false;
        if (unit >= 0xD800 and unit <= 0xDBFF) {
            if (index + 1 >= units) return false;
            const low = path_unit(bytes, index + 1);
            if (low < 0xDC00 or low > 0xDFFF) return false;
            index += 1;
            continue;
        }
        if (unit >= 0xDC00 and unit <= 0xDFFF) return false;
        if (unit == '\\' and index >= component_start) {
            const component_length = index - component_start;
            if (component_length == 0) return false;
            if ((component_length == 1 and path_unit(bytes, component_start) == '.') or
                (component_length == 2 and path_unit(bytes, component_start) == '.' and
                path_unit(bytes, component_start + 1) == '.')) return false;
            component_count += 1;
            component_start = index + 1;
        }
    }
    const final_component = units - component_start;
    if ((final_component == 1 and path_unit(bytes, component_start) == '.') or
        (final_component == 2 and path_unit(bytes, component_start) == '.' and
        path_unit(bytes, component_start + 1) == '.')) return false;
    if (final_component != 0) {
        component_count += 1;
    } else if (!drive_path) return false;
    if (unc_path and component_count < 2) return false;
    return true;
}

// Test-only fixtures.
noinline fn invoke_utf8(data: ?[*]const u8, length: usize, maximum: usize, allow_empty: bool) bool {
    return app_catalog_utf8_valid(data, length, maximum, allow_empty);
}

noinline fn invoke_package_family(data: ?[*]const u8, length: usize) bool {
    return app_catalog_package_family_valid(data, length);
}

noinline fn invoke_path_identity(data: ?[*]const u8, length: usize, maximum_units: usize) bool {
    return app_catalog_path_identity_valid(data, length, maximum_units);
}

test "UTF-8 validator rejects boundaries and accepts valid scalars" {
    const valid = [_][]const u8{ "A", "\xC2\x80", "\xE0\xA0\x80", "\xF0\x9F\x98\x80", "\xF4\x8F\xBF\xBF" };
    for (valid) |bytes| try std.testing.expect(app_catalog_utf8_valid(bytes.ptr, bytes.len, 16, false));
    const invalid = [_][]const u8{
        "\x00", "\xC0\xAF", "\xE2\x82", "\xC2x", "\xE0\x80\x80",
        "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
    };
    for (invalid) |bytes| try std.testing.expect(!app_catalog_utf8_valid(bytes.ptr, bytes.len, 16, true));
    try std.testing.expect(app_catalog_utf8_valid(null, 0, 0, true));
    try std.testing.expect(!app_catalog_utf8_valid(null, 0, 0, false));
    try std.testing.expect(!app_catalog_utf8_valid(null, 1, 16, true));
    try std.testing.expect(!app_catalog_utf8_valid("A", 1, 0, true));
}

test "package-family validator checks all syntax classes" {
    try std.testing.expect(app_catalog_package_family_valid(null, 0));
    try std.testing.expect(app_catalog_package_family_valid("Aa1.-_abcdefg123456", 19));
    try std.testing.expect(app_catalog_package_family_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_abcdefg123456", 50));
    const invalid = [_][]const u8{
        "A", "_abcdefg123456", "A_abcdefg12345", "A__abcdefg123456",
        "A_ABCDEFGHIJKLM", "A_abcdefg12345!", "A!_abcdefg123456",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789x_abcdefg123456",
    };
    for (invalid) |bytes| try std.testing.expect(!app_catalog_package_family_valid(bytes.ptr, bytes.len));
    try std.testing.expect(!app_catalog_package_family_valid(null, 1));
}

fn utf16le(comptime text: []const u16) [text.len * 2]u8 {
    var bytes: [text.len * 2]u8 = undefined;
    for (text, 0..) |unit, index| {
        bytes[index * 2] = @truncate(unit);
        bytes[index * 2 + 1] = @truncate(unit >> 8);
    }
    return bytes;
}

test "path validator handles drive, UNC and UTF-16 edge cases" {
    const valid_drive = utf16le(&.{ 'C', ':', '\\', 'A', '\\', 'B' });
    const valid_root = utf16le(&.{ 'C', ':', '\\' });
    const valid_unc = utf16le(&.{ '\\', '\\', 'S', 'R', 'V', '\\', 'S', 'H', 'R' });
    const valid_surrogate = utf16le(&.{ 'C', ':', '\\', 0xD83D, 0xDE00 });
    try std.testing.expect(app_catalog_path_identity_valid(&valid_drive, valid_drive.len, 32767));
    try std.testing.expect(app_catalog_path_identity_valid(&valid_root, valid_root.len, 32767));
    try std.testing.expect(app_catalog_path_identity_valid(&valid_unc, valid_unc.len, 32767));
    try std.testing.expect(app_catalog_path_identity_valid(&valid_surrogate, valid_surrogate.len, 32767));
    const dot = utf16le(&.{ 'C', ':', '\\', '.', '\\', 'X' });
    const parent = utf16le(&.{ 'C', ':', '\\', 'X', '\\', '.', '.' });
    const short_unc = utf16le(&.{ '\\', '\\', 'S' });
    const trailing_unc = utf16le(&.{ '\\', '\\', 'S', '\\', 'H', '\\' });
    const repeated = utf16le(&.{ 'C', ':', '\\', '\\', 'X' });
    const bad_high = utf16le(&.{ 'C', ':', '\\', 0xD800, 'X' });
    const lone_low = utf16le(&.{ 'C', ':', '\\', 0xDC00 });
    const invalid = [_][]const u8{ &dot, &parent, &short_unc, &trailing_unc, &repeated, &bad_high, &lone_low };
    for (invalid) |bytes| try std.testing.expect(!app_catalog_path_identity_valid(bytes.ptr, bytes.len, 32767));
    try std.testing.expect(!app_catalog_path_identity_valid(null, 0, 32767));
    try std.testing.expect(!app_catalog_path_identity_valid("C:", 3, 32767));
    try std.testing.expect(!app_catalog_path_identity_valid(&valid_drive, valid_drive.len, 1));
}

test "validators exercise null, boundary and alternate decision edges" {
    try std.testing.expect(invoke_utf8(null, 0, 0, true));
    try std.testing.expect(!invoke_utf8(null, 1, 1, true));
    try std.testing.expect(invoke_utf8("x", 1, 1, false));
    try std.testing.expect(!invoke_utf8("x", 1, 0, true));
    try std.testing.expect(!invoke_package_family(null, 1));
    try std.testing.expect(invoke_package_family(null, 0));
    const long_family = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789xx_abcdefg123456";
    try std.testing.expect(!invoke_package_family(long_family.ptr, long_family.len));

    const too_large_units = std.math.maxInt(usize);
    const minimum_path = utf16le(&.{ 'C', ':', '\\' });
    try std.testing.expect(!invoke_path_identity(&minimum_path, minimum_path.len, too_large_units));
    try std.testing.expect(!invoke_path_identity(&minimum_path, minimum_path.len, 32768));
    try std.testing.expect(!invoke_path_identity("C:", 2, 32767));
    try std.testing.expect(!invoke_path_identity("abc", 6, 32767));
    const non_absolute = utf16le(&.{ 'X', ':', 'X' });
    try std.testing.expect(!invoke_path_identity(&non_absolute, non_absolute.len, 32767));
    const drive_missing_root = utf16le(&.{ 'C', ':', 'X' });
    try std.testing.expect(!invoke_path_identity(&drive_missing_root, drive_missing_root.len, 32767));
    const nul_unit = utf16le(&.{ 'C', ':', '\\', 0 });
    const slash_unit = utf16le(&.{ 'C', ':', '\\', '/' });
    const lower_unit = utf16le(&.{ 'C', ':', '\\', 'a' });
    try std.testing.expect(!invoke_path_identity(&nul_unit, nul_unit.len, 32767));
    try std.testing.expect(!invoke_path_identity(&slash_unit, slash_unit.len, 32767));
    try std.testing.expect(!invoke_path_identity(&lower_unit, lower_unit.len, 32767));
    const terminal_high = utf16le(&.{ 'C', ':', '\\', 0xD800 });
    try std.testing.expect(!invoke_path_identity(&terminal_high, terminal_high.len, 32767));
    const intermediate_dotdot = utf16le(&.{ 'C', ':', '\\', '.', '.', '\\', 'X' });
    const intermediate_dotx = utf16le(&.{ 'C', ':', '\\', '.', '.', 'X', '\\', 'Y' });
    const final_dotx = utf16le(&.{ 'C', ':', '\\', '.', '.', 'X' });
    try std.testing.expect(!invoke_path_identity(&intermediate_dotdot, intermediate_dotdot.len, 32767));
    try std.testing.expect(invoke_path_identity(&intermediate_dotx, intermediate_dotx.len, 32767));
    try std.testing.expect(invoke_path_identity(&final_dotx, final_dotx.len, 32767));
    const near_end = @as([*]const u8, @ptrFromInt(std.math.maxInt(usize) - 1));
    try std.testing.expect(!invoke_utf8(near_end, 4, 4, false));
    try std.testing.expect(!invoke_package_family(near_end, 4));
    try std.testing.expect(!invoke_path_identity(near_end, 4, 2));
}
