//! Interop protocol v2 codec (impl/seamless-interop/IMPL_DESC.md section 3.2).
//!
//! Untrusted frames from either peer are validated here before any field is
//! interpreted. Every exported function is allocation-free, never retains a
//! caller pointer beyond the call (except the writer's borrowed buffer, which the
//! caller owns). Readers, AUMID keys and transfers preserve outputs on failure;
//! writer failure sets its sticky failed flag and prevents frame publication.
//! Public ownership, nullability, return values and threading contracts are
//! defined per export in include/waddle/interop_protocol.h.
const std_t = @import("std");
const c_t = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

const sha256_t = std_t.crypto.hash.sha2.Sha256;

const error_codes_t = struct {
    const None: c_int = c_t.InteropErrorNone;
    const Protocol: c_int = c_t.InteropErrorProtocolViolation;
    const Malformed: c_int = c_t.InteropErrorMalformedFrame;
    const UnsupportedField: c_int = c_t.InteropErrorUnsupportedField;
    const UnsupportedType: c_int = c_t.InteropErrorUnsupportedType;
    const Quota: c_int = c_t.InteropErrorQuotaExceeded;
};

fn get(comptime value_t: type, data: []const u8, offset: usize) value_t {
    return std_t.mem.readInt(value_t, data[offset..][0..@sizeOf(value_t)], .little);
}
fn put(comptime value_t: type, data: []u8, offset: usize, value: value_t) void {
    std_t.mem.writeInt(value_t, data[offset..][0..@sizeOf(value_t)], value, .little);
}

// ---------------------------------------------------------------------------
// Field registry
// ---------------------------------------------------------------------------

/// Registry entry: criticality and inclusive value length bounds.
const field_spec_t = struct { critical: bool, min: u16, max: u16 };

const MaxTag = 0x4B;

/// Indexed by base tag; null entries are unknown tags.
const Registry = blk: {
    var table = [_]?field_spec_t{null} ** (MaxTag + 1);
    const field_factory_t = struct {
        fn f(critical: bool, size: u16) field_spec_t {
            return .{ .critical = critical, .min = size, .max = size };
        }
        fn v(critical: bool, min: u16, max: u16) field_spec_t {
            return .{ .critical = critical, .min = min, .max = max };
        }
    };
    table[c_t.InteropTagWindow] = field_factory_t.f(true, 8);
    table[c_t.InteropTagGeneration] = field_factory_t.f(true, 8);
    table[c_t.InteropTagParent] = field_factory_t.f(true, 8);
    table[c_t.InteropTagParentGeneration] = field_factory_t.f(true, 8);
    table[c_t.InteropTagRole] = field_factory_t.f(true, 1);
    table[c_t.InteropTagRect] = field_factory_t.f(true, 16);
    table[c_t.InteropTagTitle] = field_factory_t.v(false, 0, c_t.InteropMaxTitleBytes);
    table[c_t.InteropTagAppKey] = field_factory_t.v(true, 3, c_t.InteropMaxAppKeyBytes);
    table[c_t.InteropTagProcessId] = field_factory_t.f(false, 4);
    table[c_t.InteropTagState] = field_factory_t.f(true, 4);
    table[c_t.InteropTagDpi] = field_factory_t.f(true, 4);
    table[c_t.InteropTagSlot] = field_factory_t.f(true, 4);
    table[c_t.InteropTagFrameSequence] = field_factory_t.f(true, 8);
    table[c_t.InteropTagDamage] = field_factory_t.v(false, 0, 256);
    table[c_t.InteropTagEdge] = field_factory_t.f(true, 1);
    table[c_t.InteropTagSerial] = field_factory_t.f(true, 4);
    table[c_t.InteropTagPoint] = field_factory_t.f(true, 8);
    table[c_t.InteropTagButton] = field_factory_t.f(true, 2);
    table[c_t.InteropTagPressed] = field_factory_t.f(true, 1);
    table[c_t.InteropTagKey] = field_factory_t.f(true, 2);
    table[c_t.InteropTagAxisV120] = field_factory_t.f(true, 4);
    table[c_t.InteropTagAxisH120] = field_factory_t.f(true, 4);
    table[c_t.InteropTagDelta] = field_factory_t.f(true, 8);
    table[c_t.InteropTagTouchId] = field_factory_t.f(true, 4);
    table[c_t.InteropTagPressure] = field_factory_t.f(true, 2);
    table[c_t.InteropTagTilt] = field_factory_t.f(true, 4);
    table[c_t.InteropTagRotation] = field_factory_t.f(false, 2);
    table[c_t.InteropTagTool] = field_factory_t.f(true, 1);
    table[c_t.InteropTagText] = field_factory_t.v(true, 0, c_t.InteropMaxStringBytes);
    table[c_t.InteropTagCursorBegin] = field_factory_t.f(false, 4);
    table[c_t.InteropTagCursorEnd] = field_factory_t.f(false, 4);
    table[c_t.InteropTagLayout] = field_factory_t.f(true, 8);
    table[c_t.InteropTagLocks] = field_factory_t.f(true, 1);
    table[c_t.InteropTagCursorShape] = field_factory_t.f(true, 2);
    table[c_t.InteropTagHotspot] = field_factory_t.f(true, 8);
    table[c_t.InteropTagImageSize] = field_factory_t.f(true, 8);
    table[c_t.InteropTagContentHash] = field_factory_t.f(true, c_t.InteropHashBytes);
    table[c_t.InteropTagTransfer] = field_factory_t.f(true, 4);
    table[c_t.InteropTagTotalBytes] = field_factory_t.f(true, 8);
    table[c_t.InteropTagOffset] = field_factory_t.f(true, 8);
    table[c_t.InteropTagData] = field_factory_t.v(true, 1, c_t.InteropChunkBytes);
    table[c_t.InteropTagMime] = field_factory_t.v(true, 1, 255);
    table[c_t.InteropTagMimeList] = field_factory_t.v(true, 2, 32 * 256);
    table[c_t.InteropTagClipboardEpoch] = field_factory_t.f(true, 8);
    table[c_t.InteropTagDropEffect] = field_factory_t.f(true, 1);
    table[c_t.InteropTagDisplayName] = field_factory_t.v(true, 1, 255);
    table[c_t.InteropTagTargetPath] = field_factory_t.v(false, 1, c_t.InteropMaxStringBytes);
    table[c_t.InteropTagArguments] = field_factory_t.v(false, 0, c_t.InteropMaxStringBytes);
    table[c_t.InteropTagWorkingDir] = field_factory_t.v(false, 1, c_t.InteropMaxStringBytes);
    table[c_t.InteropTagAumid] = field_factory_t.v(false, 1, 512);
    table[c_t.InteropTagAppFlags] = field_factory_t.f(true, 4);
    table[c_t.InteropTagIconSizes] = field_factory_t.v(false, 2, 14);
    table[c_t.InteropTagIconSize] = field_factory_t.f(true, 2);
    table[c_t.InteropTagLaunchKind] = field_factory_t.f(true, 1);
    table[c_t.InteropTagActivationToken] = field_factory_t.v(false, 1, 255);
    table[c_t.InteropTagStatus] = field_factory_t.f(true, 4);
    table[c_t.InteropTagMessage] = field_factory_t.v(false, 0, 1024);
    table[c_t.InteropTagCapabilities] = field_factory_t.f(true, 8);
    table[c_t.InteropTagAgentVersion] = field_factory_t.v(true, 5, 32);
    table[c_t.InteropTagLayoutVersion] = field_factory_t.f(true, 4);
    table[c_t.InteropTagSessionEpoch] = field_factory_t.f(true, 8);
    table[c_t.InteropTagOutput] = field_factory_t.f(true, 16);
    table[c_t.InteropTagStream] = field_factory_t.f(true, 4);
    table[c_t.InteropTagPrinterName] = field_factory_t.v(true, 1, 127);
    table[c_t.InteropTagPrinterUri] = field_factory_t.v(true, 1, 255);
    table[c_t.InteropTagPublisher] = field_factory_t.v(false, 0, 255);
    table[c_t.InteropTagDecision] = field_factory_t.f(true, 1);
    table[c_t.InteropTagUsbId] = field_factory_t.f(true, 6);
    table[c_t.InteropTagLeaseEpoch] = field_factory_t.f(true, 8);
    table[c_t.InteropTagErrorCode] = field_factory_t.f(true, 4);
    table[c_t.InteropTagRefusedType] = field_factory_t.f(false, 2);
    table[c_t.InteropTagUriList] = field_factory_t.v(true, 1, 60000);
    table[c_t.InteropTagIntervalMs] = field_factory_t.f(true, 4);
    table[c_t.InteropTagRate] = field_factory_t.f(true, 4);
    table[c_t.InteropTagMinSize] = field_factory_t.f(false, 8);
    break :blk table;
};

fn spec_for(base: u16) ?field_spec_t {
    if (base == 0 or base > MaxTag) return null;
    return Registry[base];
}

fn utf8_text(value: []const u8) bool {
    return std_t.unicode.utf8ValidateSlice(value) and std_t.mem.indexOfScalar(u8, value, 0) == null;
}
fn printable(value: []const u8) bool {
    for (value) |byte| if (byte < '!' or byte > '~') return false;
    return true;
}
fn in_range(comptime value_t: type, value: value_t, low: value_t, high: value_t) bool {
    return value >= low and value <= high;
}
fn icon_size_valid(size: u16) bool {
    return switch (size) {
        16, 24, 32, 48, 64, 128, 256 => true,
        else => false,
    };
}
fn mime_valid(value: []const u8) bool {
    return value.len >= 1 and value.len <= 255 and printable(value);
}
fn mime_list_valid(value: []const u8) bool {
    var offset: usize = 0;
    var entries: usize = 0;
    while (offset < value.len) {
        const size = value[offset];
        offset += 1;
        if (size == 0 or offset + size > value.len or !mime_valid(value[offset..][0..size])) return false;
        offset += size;
        entries += 1;
        if (entries > 32) return false;
    }
    return entries >= 1;
}
fn version_valid(value: []const u8) bool {
    var parts: usize = 0;
    var digits: usize = 0;
    for (value) |byte| {
        if (byte == '.') {
            if (digits == 0) return false;
            parts += 1;
            digits = 0;
        } else if (byte >= '0' and byte <= '9') {
            digits += 1;
            if (digits > 9) return false;
        } else return false;
    }
    return parts == 2 and digits > 0;
}
fn app_key_valid(value: []const u8) bool {
    if (value.len < 3 or value[1] != '-' or (value[0] != 'a' and value[0] != 'p')) return false;
    for (value) |byte| {
        const ok = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
            (byte >= '0' and byte <= '9') or byte == '_' or byte == '-';
        if (!ok) return false;
    }
    return true;
}

fn app_key_byte(byte: u8) u8 {
    return if ((byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
        (byte >= '0' and byte <= '9') or byte == '_' or byte == '-') byte else '_';
}

/// Build a bounded protocol-v2 application key from UTF-8.
/// @param[in] aumid Borrowed UTF-8 AUMID without NUL. NULL is rejected. Entire input is
/// consumed before output; overlapping byte buffers are safe.
/// @param[in] length Input bytes, 1..InteropMaxStringBytes; no normalization or case folding.
/// @param[out] key Non-NULL caller-owned writable buffer of capacity bytes; not NUL-terminated.
/// Unchanged on failure.
/// @param[in] capacity Output capacity; at most InteropMaxAppKeyBytes are written.
/// @param[out] key_length Non-NULL caller-owned length, separate from both byte buffers;
/// unchanged on failure.
/// @return InteropErrorNone, MalformedFrame for invalid input, or QuotaExceeded for
/// insufficient output space.
/// @note No allocation or retained pointers. Thread-safe for distinct outputs. Hash suffix is
/// eight lowercase hex digits of SHA-256 over original UTF-8.
export fn interop_app_key_from_aumid(aumid: ?[*]const u8, length: usize, key: [*]u8, capacity: usize, key_length: *usize) c_int {
    if (length == 0 or length > c_t.InteropMaxStringBytes) return error_codes_t.Malformed;
    const input = (aumid orelse return error_codes_t.Malformed)[0..length];
    if (!utf8_text(input)) return error_codes_t.Malformed;
    var changed = false;
    for (input) |byte| changed = changed or app_key_byte(byte) != byte;
    const suffixed = changed or length > c_t.InteropMaxAppKeyBytes - 2;
    const suffix_bytes: usize = if (suffixed) 9 else 0;
    const body_bytes = @min(length, c_t.InteropMaxAppKeyBytes - 2 - suffix_bytes);
    const total = 2 + body_bytes + suffix_bytes;
    if (capacity < total) return error_codes_t.Quota;
    var result: [c_t.InteropMaxAppKeyBytes]u8 = undefined;
    result[0] = 'a';
    result[1] = '-';
    for (input[0..body_bytes], 0..) |byte, index| result[2 + index] = app_key_byte(byte);
    if (suffixed) {
        var digest: [32]u8 = undefined;
        sha256_t.hash(input, &digest, .{});
        const Hex = "0123456789abcdef";
        result[2 + body_bytes] = '-';
        for (digest[0..4], 0..) |byte, index| {
            result[3 + body_bytes + index * 2] = Hex[byte >> 4];
            result[4 + body_bytes + index * 2] = Hex[byte & 15];
        }
    }
    @memcpy(key[0..total], result[0..total]);
    key_length.* = total;
    return error_codes_t.None;
}
fn printer_name_valid(value: []const u8) bool {
    if (!printable(value)) return false;
    return std_t.mem.indexOfAny(u8, value, "/#") == null;
}
fn point_valid(value: []const u8, low: i32, high: i32) bool {
    return in_range(i32, get(i32, value, 0), low, high) and in_range(i32, get(i32, value, 4), low, high);
}
fn rect_valid(value: []const u8) bool {
    return get(u32, value, 8) <= 16384 and get(u32, value, 12) <= 16384;
}

/// Semantic validation of one known field value whose length already matches
/// the registry bounds. Split by tag range so each switch stays small.
fn value_valid(base: u16, value: []const u8) bool {
    if (base <= c_t.InteropTagButton) return window_value_valid(base, value);
    if (base <= c_t.InteropTagContentHash) return input_value_valid(base, value);
    if (base <= c_t.InteropTagMessage) return data_value_valid(base, value);
    return session_value_valid(base, value);
}

/// Window identity, geometry and pointer fields (tags 0x01..0x12).
fn window_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c_t.InteropTagWindow, c_t.InteropTagGeneration, c_t.InteropTagFrameSequence => get(u64, value, 0) != 0,
        c_t.InteropTagParent, c_t.InteropTagParentGeneration, c_t.InteropTagProcessId => true,
        c_t.InteropTagRole => in_range(u8, value[0], 1, 6),
        c_t.InteropTagRect => rect_valid(value),
        c_t.InteropTagTitle => utf8_text(value),
        c_t.InteropTagAppKey => app_key_valid(value),
        c_t.InteropTagState => get(u32, value, 0) <= c_t.InteropStateMask,
        c_t.InteropTagDpi => in_range(u32, get(u32, value, 0), 96, 960),
        c_t.InteropTagSlot => get(u32, value, 0) < c_t.InteropMaxWindows * 3,
        c_t.InteropTagDamage => damage_valid(value),
        c_t.InteropTagEdge => switch (value[0]) {
            1, 2, 4, 5, 6, 8, 9, 10 => true,
            else => false,
        },
        c_t.InteropTagSerial => get(u32, value, 0) != 0,
        c_t.InteropTagPoint => point_valid(value, -16384, 16384),
        c_t.InteropTagButton => in_range(u16, get(u16, value, 0), 1, 5),
        else => unreachable, // spec_for admits only registered tags
    };
}

/// Keyboard, touch, tablet, text and cursor fields (tags 0x13..0x25).
fn input_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c_t.InteropTagPressed => value[0] <= 1,
        c_t.InteropTagKey => in_range(u16, get(u16, value, 0), 1, 767),
        c_t.InteropTagAxisV120, c_t.InteropTagAxisH120 => in_range(i32, get(i32, value, 0), -15360, 15360),
        c_t.InteropTagDelta => point_valid(value, -(16384 << 8), 16384 << 8),
        c_t.InteropTagTouchId => get(u32, value, 0) <= 9,
        c_t.InteropTagPressure => get(u16, value, 0) <= 1024,
        c_t.InteropTagTilt => in_range(i16, get(i16, value, 0), -9000, 9000) and
            in_range(i16, get(i16, value, 2), -9000, 9000),
        c_t.InteropTagRotation => get(u16, value, 0) <= 35999,
        c_t.InteropTagTool => in_range(u8, value[0], 1, 2),
        c_t.InteropTagText => utf8_text(value),
        c_t.InteropTagCursorBegin, c_t.InteropTagCursorEnd => in_range(i32, get(i32, value, 0), -1, c_t.InteropMaxStringBytes),
        c_t.InteropTagLayout => hex_valid(value),
        c_t.InteropTagLocks => value[0] <= 7,
        c_t.InteropTagCursorShape => cursor_shape_valid(get(u16, value, 0)),
        c_t.InteropTagHotspot => point_valid(value, 0, 512),
        c_t.InteropTagImageSize => in_range(u32, get(u32, value, 0), 1, 512) and in_range(u32, get(u32, value, 4), 1, 512),
        c_t.InteropTagContentHash => true,
        else => unreachable, // spec_for admits only registered tags
    };
}

/// Transfer, clipboard, catalog and launch fields (tags 0x26..0x39).
fn data_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c_t.InteropTagTransfer => get(u32, value, 0) != 0,
        c_t.InteropTagTotalBytes => get(u64, value, 0) <= c_t.InteropMaxTransferBytes,
        c_t.InteropTagOffset => get(u64, value, 0) < c_t.InteropMaxTransferBytes,
        c_t.InteropTagData => true,
        c_t.InteropTagMime => mime_valid(value),
        c_t.InteropTagMimeList => mime_list_valid(value),
        c_t.InteropTagClipboardEpoch => get(u64, value, 0) != 0,
        c_t.InteropTagDropEffect => value[0] <= 7,
        c_t.InteropTagDisplayName,
        c_t.InteropTagTargetPath,
        c_t.InteropTagArguments,
        c_t.InteropTagWorkingDir,
        c_t.InteropTagAumid,
        c_t.InteropTagMessage,
        => utf8_text(value),
        c_t.InteropTagAppFlags => get(u32, value, 0) <= c_t.InteropAppFlagMask,
        c_t.InteropTagIconSizes => icon_sizes_valid(value),
        c_t.InteropTagIconSize => icon_size_valid(get(u16, value, 0)),
        c_t.InteropTagLaunchKind => in_range(u8, value[0], 1, 5),
        c_t.InteropTagActivationToken => printable(value),
        c_t.InteropTagStatus => get(u32, value, 0) <= c_t.InteropGuestError,
        else => unreachable, // spec_for admits only registered tags
    };
}

/// Session, display, audio, printer, USB and error fields (tags 0x3A..0x4B).
fn session_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c_t.InteropTagCapabilities => get(u64, value, 0) <= c_t.InteropCapabilityMask,
        c_t.InteropTagAgentVersion => version_valid(value),
        c_t.InteropTagLayoutVersion => get(u32, value, 0) != 0,
        c_t.InteropTagSessionEpoch, c_t.InteropTagLeaseEpoch => get(u64, value, 0) != 0,
        c_t.InteropTagOutput => in_range(u32, get(u32, value, 4), 1, 16384) and
            in_range(u32, get(u32, value, 8), 1, 16384) and in_range(u32, get(u32, value, 12), 120, 480),
        c_t.InteropTagStream => get(u32, value, 0) < c_t.InteropMaxAudioStreams,
        c_t.InteropTagPrinterName => printer_name_valid(value),
        c_t.InteropTagPrinterUri => printable(value),
        c_t.InteropTagPublisher, c_t.InteropTagUriList => utf8_text(value),
        c_t.InteropTagDecision => value[0] <= 1,
        c_t.InteropTagUsbId, c_t.InteropTagRefusedType => true,
        c_t.InteropTagErrorCode => in_range(u32, get(u32, value, 0), 1, c_t.InteropErrorUnexpected),
        c_t.InteropTagIntervalMs => get(u32, value, 0) <= 10000,
        c_t.InteropTagRate => get(u32, value, 0) <= 1000,
        c_t.InteropTagMinSize => get(u32, value, 0) <= 16384 and get(u32, value, 4) <= 16384,
        else => unreachable, // spec_for admits only registered tags
    };
}

fn damage_valid(value: []const u8) bool {
    if (value.len % 16 != 0) return false;
    var offset: usize = 0;
    while (offset < value.len) : (offset += 16) {
        if (!rect_valid(value[offset..][0..16])) return false;
    }
    return true;
}
fn hex_valid(value: []const u8) bool {
    for (value) |byte| if (!std_t.ascii.isHex(byte)) return false;
    return true;
}
fn cursor_shape_valid(shape: u16) bool {
    return shape <= c_t.InteropCursorLast or shape == c_t.InteropCursorCustom;
}
fn icon_sizes_valid(value: []const u8) bool {
    if (value.len % 2 != 0) return false;
    var previous: u16 = 0;
    var offset: usize = 0;
    while (offset < value.len) : (offset += 2) {
        const size = get(u16, value, offset);
        if (!icon_size_valid(size) or size <= previous) return false;
        previous = size;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Message table
// ---------------------------------------------------------------------------

const DirGuest: u8 = 1;
const DirHost: u8 = 2;
const DirBoth: u8 = DirGuest | DirHost;
const DirLocal: u8 = 4;

const message_spec_t = struct {
    type: u16,
    directions: u8,
    required: []const u16,
    optional: []const u16 = &.{},
};

const field_tags_t = struct {
    const Window = c_t.InteropTagWindow;
    const Generation = c_t.InteropTagGeneration;
    const Lease = c_t.InteropTagLeaseEpoch;
    const Serial = c_t.InteropTagSerial;
};
const InputHead = [_]u16{ field_tags_t.Window, field_tags_t.Generation, field_tags_t.Serial, field_tags_t.Lease };
fn with(comptime base: []const u16, comptime extra: []const u16) []const u16 {
    return base ++ extra;
}
const LaunchFields = [_]u16{ c_t.InteropTagTargetPath, c_t.InteropTagArguments, c_t.InteropTagWorkingDir, c_t.InteropTagAppKey, c_t.InteropTagActivationToken };
const CatalogOptional = [_]u16{ c_t.InteropTagTargetPath, c_t.InteropTagArguments, c_t.InteropTagWorkingDir, c_t.InteropTagAumid, c_t.InteropTagIconSizes, c_t.InteropTagContentHash };

const Messages = [_]message_spec_t{
    .{ .type = c_t.InteropMsgHello, .directions = DirGuest, .required = &.{ c_t.InteropTagAgentVersion, c_t.InteropTagCapabilities, c_t.InteropTagLayoutVersion, c_t.InteropTagSessionEpoch }, .optional = &.{c_t.InteropTagMessage} },
    .{ .type = c_t.InteropMsgHelloAck, .directions = DirHost, .required = &.{ c_t.InteropTagAgentVersion, c_t.InteropTagCapabilities, c_t.InteropTagLayoutVersion } },
    .{ .type = c_t.InteropMsgGoodbye, .directions = DirBoth, .required = &.{c_t.InteropTagStatus}, .optional = &.{c_t.InteropTagMessage} },
    .{ .type = c_t.InteropMsgPing, .directions = DirHost, .required = &.{} },
    .{ .type = c_t.InteropMsgPong, .directions = DirGuest, .required = &.{} },
    .{ .type = c_t.InteropMsgError, .directions = DirBoth, .required = &.{c_t.InteropTagErrorCode}, .optional = &.{ c_t.InteropTagRefusedType, c_t.InteropTagMessage } },
    .{ .type = c_t.InteropMsgWindowAnnounce, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagRole, c_t.InteropTagRect, c_t.InteropTagAppKey, c_t.InteropTagState, c_t.InteropTagDpi }, .optional = &.{ c_t.InteropTagTitle, c_t.InteropTagParent, c_t.InteropTagParentGeneration, c_t.InteropTagProcessId, c_t.InteropTagMinSize, c_t.InteropTagSerial } },
    .{ .type = c_t.InteropMsgWindowUpdate, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagRect, c_t.InteropTagState, c_t.InteropTagDpi }, .optional = &.{ c_t.InteropTagTitle, c_t.InteropTagParent, c_t.InteropTagParentGeneration, c_t.InteropTagMinSize } },
    .{ .type = c_t.InteropMsgWindowRetire, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation } },
    .{ .type = c_t.InteropMsgWindowConfigure, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagState }, .optional = &.{c_t.InteropTagRect} },
    .{ .type = c_t.InteropMsgWindowClose, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation } },
    .{ .type = c_t.InteropMsgWindowMoveRequest, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, field_tags_t.Serial } },
    .{ .type = c_t.InteropMsgWindowResizeRequest, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagEdge, field_tags_t.Serial } },
    .{ .type = c_t.InteropMsgWindowStateRequest, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagState } },
    .{ .type = c_t.InteropMsgFrameReady, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagSlot, c_t.InteropTagFrameSequence }, .optional = &.{c_t.InteropTagDamage} },
    .{ .type = c_t.InteropMsgFrameRelease, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagSlot, c_t.InteropTagFrameSequence } },
    .{ .type = c_t.InteropMsgWindowIconChanged, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagContentHash } },
    .{ .type = c_t.InteropMsgPopupRepositioned, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagRect } },
    .{ .type = c_t.InteropMsgPopupDismissed, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation } },
    .{ .type = c_t.InteropMsgInputFocus, .directions = DirHost, .required = with(&InputHead, &.{c_t.InteropTagPressed}) },
    .{ .type = c_t.InteropMsgInputKey, .directions = DirHost, .required = with(&InputHead, &.{ c_t.InteropTagPressed, c_t.InteropTagKey }) },
    .{ .type = c_t.InteropMsgInputPointerMotion, .directions = DirHost, .required = with(&InputHead, &.{c_t.InteropTagPoint}) },
    .{ .type = c_t.InteropMsgInputPointerRelative, .directions = DirHost, .required = with(&InputHead, &.{c_t.InteropTagDelta}) },
    .{ .type = c_t.InteropMsgInputPointerButton, .directions = DirHost, .required = with(&InputHead, &.{ c_t.InteropTagPoint, c_t.InteropTagButton, c_t.InteropTagPressed }) },
    .{ .type = c_t.InteropMsgInputPointerAxis, .directions = DirHost, .required = with(&InputHead, &.{ c_t.InteropTagAxisV120, c_t.InteropTagAxisH120 }) },
    .{ .type = c_t.InteropMsgInputTouch, .directions = DirHost, .required = with(&InputHead, &.{ c_t.InteropTagPoint, c_t.InteropTagPressed, c_t.InteropTagTouchId }) },
    .{ .type = c_t.InteropMsgInputTouchFrame, .directions = DirHost, .required = &InputHead },
    .{ .type = c_t.InteropMsgInputTouchCancel, .directions = DirHost, .required = &InputHead },
    .{ .type = c_t.InteropMsgInputTablet, .directions = DirHost, .required = with(&InputHead, &.{ c_t.InteropTagPoint, c_t.InteropTagPressed, c_t.InteropTagPressure, c_t.InteropTagTilt, c_t.InteropTagTool }), .optional = &.{ c_t.InteropTagButton, c_t.InteropTagRotation } },
    .{ .type = c_t.InteropMsgInputTabletProximity, .directions = DirHost, .required = with(&InputHead, &.{ c_t.InteropTagPressed, c_t.InteropTagTool }) },
    .{ .type = c_t.InteropMsgInputGesturePinch, .directions = DirHost, .required = with(&InputHead, &.{ c_t.InteropTagPoint, c_t.InteropTagAxisV120 }) },
    .{ .type = c_t.InteropMsgInputTextCommit, .directions = DirHost, .required = with(&InputHead, &.{c_t.InteropTagText}) },
    .{ .type = c_t.InteropMsgInputTextPreedit, .directions = DirHost, .required = with(&InputHead, &.{c_t.InteropTagText}), .optional = &.{ c_t.InteropTagCursorBegin, c_t.InteropTagCursorEnd } },
    .{ .type = c_t.InteropMsgInputRelease, .directions = DirHost, .required = &InputHead },
    .{ .type = c_t.InteropMsgInputLockState, .directions = DirHost, .required = &.{ field_tags_t.Serial, c_t.InteropTagLocks, field_tags_t.Lease } },
    .{ .type = c_t.InteropMsgInputRepeatInfo, .directions = DirHost, .required = &.{ c_t.InteropTagIntervalMs, c_t.InteropTagRate } },
    .{ .type = c_t.InteropMsgKeyboardLayout, .directions = DirHost, .required = &.{c_t.InteropTagLayout} },
    .{ .type = c_t.InteropMsgCaretRect, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagRect } },
    .{ .type = c_t.InteropMsgLeaseRevoked, .directions = DirGuest, .required = &.{field_tags_t.Lease} },
    .{ .type = c_t.InteropMsgLeaseAck, .directions = DirHost, .required = &.{ field_tags_t.Serial, field_tags_t.Lease } },
    .{ .type = c_t.InteropMsgLeaseReady, .directions = DirGuest, .required = &.{ field_tags_t.Serial, field_tags_t.Lease } },
    .{ .type = c_t.InteropMsgCursorShape, .directions = DirGuest, .required = &.{c_t.InteropTagCursorShape}, .optional = &.{ c_t.InteropTagHotspot, c_t.InteropTagContentHash } },
    .{ .type = c_t.InteropMsgCursorConstraint, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagPressed } },
    .{ .type = c_t.InteropMsgClipboardOffer, .directions = DirBoth, .required = &.{ c_t.InteropTagMimeList, c_t.InteropTagClipboardEpoch }, .optional = &.{c_t.InteropTagDropEffect} },
    .{ .type = c_t.InteropMsgClipboardRequest, .directions = DirBoth, .required = &.{ c_t.InteropTagTransfer, c_t.InteropTagMime, c_t.InteropTagClipboardEpoch } },
    .{ .type = c_t.InteropMsgClipboardClear, .directions = DirBoth, .required = &.{c_t.InteropTagClipboardEpoch} },
    .{ .type = c_t.InteropMsgDndEnter, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagPoint, c_t.InteropTagMimeList, c_t.InteropTagDropEffect } },
    .{ .type = c_t.InteropMsgDndMotion, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagPoint } },
    .{ .type = c_t.InteropMsgDndLeave, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation } },
    .{ .type = c_t.InteropMsgDndDrop, .directions = DirHost, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagPoint, c_t.InteropTagTransfer, c_t.InteropTagMime } },
    .{ .type = c_t.InteropMsgDndResult, .directions = DirGuest, .required = &.{ c_t.InteropTagDropEffect, c_t.InteropTagStatus } },
    .{ .type = c_t.InteropMsgDndSourceStart, .directions = DirGuest, .required = &.{ field_tags_t.Window, field_tags_t.Generation, c_t.InteropTagMimeList, c_t.InteropTagDropEffect } },
    .{ .type = c_t.InteropMsgDndSourceRequest, .directions = DirHost, .required = &.{ c_t.InteropTagTransfer, c_t.InteropTagMime } },
    .{ .type = c_t.InteropMsgDndSourceFinished, .directions = DirHost, .required = &.{ c_t.InteropTagDropEffect, c_t.InteropTagStatus } },
    .{ .type = c_t.InteropMsgCatalogBegin, .directions = DirGuest, .required = &.{} },
    .{ .type = c_t.InteropMsgCatalogAdd, .directions = DirGuest, .required = &.{ c_t.InteropTagAppKey, c_t.InteropTagDisplayName, c_t.InteropTagAppFlags }, .optional = &CatalogOptional },
    .{ .type = c_t.InteropMsgCatalogUpdate, .directions = DirGuest, .required = &.{ c_t.InteropTagAppKey, c_t.InteropTagDisplayName, c_t.InteropTagAppFlags }, .optional = &CatalogOptional },
    .{ .type = c_t.InteropMsgCatalogRemove, .directions = DirGuest, .required = &.{c_t.InteropTagAppKey} },
    .{ .type = c_t.InteropMsgCatalogEnd, .directions = DirGuest, .required = &.{} },
    .{ .type = c_t.InteropMsgIconRequest, .directions = DirHost, .required = &.{ c_t.InteropTagAppKey, c_t.InteropTagTransfer, c_t.InteropTagIconSize }, .optional = &.{ field_tags_t.Window, field_tags_t.Generation } },
    .{ .type = c_t.InteropMsgLaunchRequest, .directions = DirHost, .required = &.{c_t.InteropTagLaunchKind}, .optional = &LaunchFields },
    .{ .type = c_t.InteropMsgLaunchResult, .directions = DirGuest, .required = &.{ c_t.InteropTagProcessId, c_t.InteropTagStatus }, .optional = &.{ c_t.InteropTagActivationToken, c_t.InteropTagMessage } },
    .{ .type = c_t.InteropMsgElevationNotice, .directions = DirGuest, .required = &.{ c_t.InteropTagProcessId, c_t.InteropTagDisplayName }, .optional = &.{ c_t.InteropTagTargetPath, c_t.InteropTagPublisher } },
    .{ .type = c_t.InteropMsgUacPrompt, .directions = DirGuest, .required = &.{ c_t.InteropTagTransfer, c_t.InteropTagDisplayName }, .optional = &.{ c_t.InteropTagTargetPath, c_t.InteropTagPublisher } },
    .{ .type = c_t.InteropMsgUacDecision, .directions = DirHost, .required = &.{ c_t.InteropTagTransfer, c_t.InteropTagDecision } },
    .{ .type = c_t.InteropMsgDesktopSwitch, .directions = DirGuest, .required = &.{c_t.InteropTagPressed} },
    .{ .type = c_t.InteropMsgAudioStreamOpen, .directions = DirGuest, .required = &.{ c_t.InteropTagAppKey, c_t.InteropTagProcessId, c_t.InteropTagDisplayName, c_t.InteropTagStream } },
    .{ .type = c_t.InteropMsgAudioStreamClose, .directions = DirGuest, .required = &.{c_t.InteropTagStream} },
    .{ .type = c_t.InteropMsgPrinterSync, .directions = DirHost, .required = &.{ c_t.InteropTagPressed, c_t.InteropTagPrinterName, c_t.InteropTagPrinterUri }, .optional = &.{c_t.InteropTagDisplayName} },
    .{ .type = c_t.InteropMsgPrinterResult, .directions = DirGuest, .required = &.{ c_t.InteropTagStatus, c_t.InteropTagPrinterName }, .optional = &.{c_t.InteropTagMessage} },
    .{ .type = c_t.InteropMsgDisplayConfig, .directions = DirHost, .required = &.{c_t.InteropTagOutput} },
    .{ .type = c_t.InteropMsgDisplayApplied, .directions = DirGuest, .required = &.{ c_t.InteropTagRect, c_t.InteropTagStatus }, .optional = &.{c_t.InteropTagMessage} },
    .{ .type = c_t.InteropMsgTransferBegin, .directions = DirBoth, .required = &.{ c_t.InteropTagTransfer, c_t.InteropTagTotalBytes, c_t.InteropTagMime }, .optional = &.{c_t.InteropTagContentHash} },
    .{ .type = c_t.InteropMsgTransferChunk, .directions = DirBoth, .required = &.{ c_t.InteropTagTransfer, c_t.InteropTagOffset, c_t.InteropTagData } },
    .{ .type = c_t.InteropMsgTransferEnd, .directions = DirBoth, .required = &.{ c_t.InteropTagContentHash, c_t.InteropTagTransfer } },
    .{ .type = c_t.InteropMsgTransferCancel, .directions = DirBoth, .required = &.{ c_t.InteropTagTransfer, c_t.InteropTagStatus } },
    .{ .type = c_t.InteropMsgLocalLaunch, .directions = DirLocal, .required = &.{c_t.InteropTagLaunchKind}, .optional = &LaunchFields },
    .{ .type = c_t.InteropMsgLocalStop, .directions = DirLocal, .required = &.{} },
    .{ .type = c_t.InteropMsgLocalStopResult, .directions = DirLocal, .required = &.{c_t.InteropTagStatus}, .optional = &.{c_t.InteropTagMessage} },
};

const tag_set_t = u128;
fn tag_bit(base: u16) tag_set_t {
    return @as(tag_set_t, 1) << @as(u7, @intCast(base));
}
fn set_of(tags: []const u16) tag_set_t {
    var result: tag_set_t = 0;
    for (tags) |tag| result |= tag_bit(tag);
    return result;
}

/// Precomputed required/allowed sets, one per message table entry.
const MessageSets = blk: {
    var sets: [Messages.len]struct { required: tag_set_t, allowed: tag_set_t } = undefined;
    for (Messages, 0..) |message, index| {
        const Required = set_of(message.required);
        sets[index] = .{ .required = Required, .allowed = Required | set_of(message.optional) };
    }
    break :blk sets;
};

fn message_index(kind: u16) ?usize {
    for (Messages, 0..) |message, index| if (message.type == kind) return index;
    return null;
}

fn direction_bit(direction: c_t.interop_direction_t) u8 {
    return switch (direction) {
        c_t.InteropFromGuest => DirGuest,
        c_t.InteropFromHost => DirHost,
        c_t.InteropFromLocal => DirLocal,
        else => 0,
    };
}

// ---------------------------------------------------------------------------
// Payload walking
// ---------------------------------------------------------------------------

const field_t = struct { tag: u16, value: []const u8 };

/// Iterates TLV fields, checking structure and padding only.
const field_walker_t = struct {
    data: []const u8,
    offset: usize = 0,

    /// Returns the next field, null at the end, or error.Malformed.
    fn next(self: *field_walker_t) error{Malformed}!?field_t {
        if (self.offset == self.data.len) return null;
        if (self.data.len - self.offset < 4) return error.Malformed;
        const tag = get(u16, self.data, self.offset);
        const size: usize = get(u16, self.data, self.offset + 2);
        const start = self.offset + 4;
        const padded = (size + 3) & ~@as(usize, 3);
        if (self.data.len - start < padded) return error.Malformed;
        for (self.data[start + size .. start + padded]) |byte| if (byte != 0) return error.Malformed;
        self.offset = start + padded;
        return .{ .tag = tag, .value = self.data[start .. start + size] };
    }
};

fn optional_field(payload: []const u8, base: u16) ?[]const u8 {
    var walker = field_walker_t{ .data = payload };
    while (walker.next() catch return null) |field| {
        if (field.tag & 0x7FFF == base) return field.value;
    }
    return null;
}

/// Cross-field rules that the per-field table cannot express.
fn cross_valid(kind: u16, payload: []const u8) bool {
    if (kind == c_t.InteropMsgLaunchRequest or kind == c_t.InteropMsgLocalLaunch) {
        // Required-field checking ran first, so launch_kind is present.
        const launch = optional_field(payload, c_t.InteropTagLaunchKind).?;
        const has_app = optional_field(payload, c_t.InteropTagAppKey) != null;
        const has_path = optional_field(payload, c_t.InteropTagTargetPath) != null;
        return if (launch[0] == c_t.InteropLaunchApp) has_app and !has_path else has_path and !has_app;
    }
    if (kind == c_t.InteropMsgWindowAnnounce or kind == c_t.InteropMsgWindowUpdate) {
        const has_parent = optional_field(payload, c_t.InteropTagParent) != null;
        const has_generation = optional_field(payload, c_t.InteropTagParentGeneration) != null;
        if (has_parent != has_generation) return false;
        if (kind == c_t.InteropMsgWindowAnnounce and optional_field(payload, c_t.InteropTagSerial) != null and
            optional_field(payload, c_t.InteropTagRole).?[0] != c_t.InteropRoleMenu) return false;
    }
    if (kind == c_t.InteropMsgIconRequest) {
        const has_window = optional_field(payload, c_t.InteropTagWindow) != null;
        if (has_window != (optional_field(payload, c_t.InteropTagGeneration) != null)) return false;
    }
    return true;
}

fn validate(kind: u16, direction: c_t.interop_direction_t, payload: []const u8) c_int {
    if (payload.len > c_t.InteropMaxPayloadBytes or payload.len % 4 != 0) return error_codes_t.Malformed;
    const index = message_index(kind) orelse return error_codes_t.UnsupportedType;
    const bit = direction_bit(direction);
    if (bit == 0 or Messages[index].directions & bit == 0) return error_codes_t.UnsupportedType;
    const sets = MessageSets[index];
    var walker = field_walker_t{ .data = payload };
    var seen: tag_set_t = 0;
    var last: u16 = 0;
    var fields: usize = 0;
    var outputs: usize = 0;
    while (walker.next() catch return error_codes_t.Malformed) |field| {
        fields += 1;
        if (fields > c_t.InteropMaxFields) return error_codes_t.Malformed;
        const base = field.tag & 0x7FFF;
        const critical = field.tag & c_t.InteropCriticalTag != 0;
        if (base == 0) return error_codes_t.Malformed;
        const repeat = base == last and base == c_t.InteropTagOutput and kind == c_t.InteropMsgDisplayConfig;
        if (base <= last and !repeat) return error_codes_t.Malformed;
        last = base;
        const spec = spec_for(base) orelse {
            if (critical) return error_codes_t.UnsupportedField;
            continue;
        };
        if (spec.critical != critical) return error_codes_t.Malformed;
        if (field.value.len < spec.min or field.value.len > spec.max) return error_codes_t.Malformed;
        if (!value_valid(base, field.value)) return error_codes_t.Malformed;
        if (sets.allowed & tag_bit(base) == 0) return error_codes_t.Malformed;
        if (base == c_t.InteropTagOutput) {
            outputs += 1;
            if (outputs > 8) return error_codes_t.Malformed;
        }
        seen |= tag_bit(base);
    }
    if (seen & sets.required != sets.required) return error_codes_t.Malformed;
    if (!cross_valid(kind, payload)) return error_codes_t.Malformed;
    return error_codes_t.None;
}

// ---------------------------------------------------------------------------
// Exported C ABI
// ---------------------------------------------------------------------------

/// Decode a protocol-v2 header into host byte order.
/// @param[in] bytes Borrowed untrusted bytes, readable for length bytes; NULL is rejected.
/// @param[in] length Available bytes, at least InteropHeaderBytes; following payload is not
/// consumed.
/// @param[out] header Non-NULL caller-owned output; unchanged on every error.
/// @return InteropErrorNone; MalformedFrame for short/NULL input; ProtocolViolation for bad
/// magic, non-v2 version, oversize or unaligned payload length.
/// @note No allocation or retained pointers. Thread-safe for distinct outputs.
export fn interop_header_decode(bytes: ?[*]const u8, length: usize, header: *c_t.interop_header_t) c_int {
    if (length < c_t.InteropHeaderBytes) return error_codes_t.Malformed;
    const data = (bytes orelse return error_codes_t.Malformed)[0..c_t.InteropHeaderBytes];
    const magic = get(u32, data, 0);
    const version = get(u16, data, 4);
    const size = get(u32, data, 8);
    if (magic != c_t.InteropMagic or version != c_t.InteropVersion or size > c_t.InteropMaxPayloadBytes or size % 4 != 0)
        return error_codes_t.Protocol;
    header.* = .{ .magic = magic, .version = version, .type = get(u16, data, 6), .length = size, .request_id = get(u32, data, 12) };
    return error_codes_t.None;
}

/// Validate a complete payload for a message type and producer.
/// @param[in] kind Message type from a successfully decoded header.
/// @param[in] direction VSOCK producer or InteropFromLocal for either session.sock peer.
/// @param[in] payload Borrowed untrusted bytes; may be NULL when length is zero.
/// @param[in] length Complete payload size from the header; must be a multiple of four.
/// @return InteropErrorNone; UnsupportedType for unknown/wrong-direction messages;
/// UnsupportedField for unknown critical tags; MalformedFrame for structure, required-field,
/// ordering, size, value or cross-field defects.
/// @note No allocation or retained pointers. Thread-safe for distinct outputs. Does not enforce
/// runtime capabilities, request IDs, leases, or connection state.
export fn interop_payload_validate(kind: u16, direction: c_t.interop_direction_t, payload: ?[*]const u8, length: usize) c_int {
    if (length == 0) return validate(kind, direction, &.{});
    const data = payload orelse return error_codes_t.Malformed;
    return validate(kind, direction, data[0..length]);
}

/// Borrow the indexed occurrence of a field in a validated payload.
/// @param[in] payload Borrowed previously validated bytes; may be NULL only when length is zero.
/// @param[in] length Payload bytes, at most InteropMaxPayloadBytes.
/// @param[in] tag Base field tag without the critical bit.
/// @param[in] index Zero-based occurrence; only Output may repeat.
/// @param[out] value Non-NULL caller-owned pointer slot; result borrows payload until that
/// payload is modified/freed. Unchanged on error/absence.
/// @param[out] value_length Non-NULL caller-owned size slot; unchanged on error/absence.
/// @return 0 found, 1 absent, -1 for malformed bounds/padding, oversize payload or NULL output
/// slots.
/// @note No allocation or retained pointers. Thread-safe for distinct outputs.
export fn interop_field_get(payload: ?[*]const u8, length: usize, tag: u16, index: u32, value: ?*?[*]const u8, value_length: ?*u16) c_int {
    const value_out = value orelse return -1;
    const length_out = value_length orelse return -1;
    if (length > c_t.InteropMaxPayloadBytes) return -1;
    const data: []const u8 = if (length == 0) &.{} else (payload orelse return -1)[0..length];
    var walker = field_walker_t{ .data = data };
    var occurrence: u32 = 0;
    while (walker.next() catch return -1) |field| {
        if (field.tag & 0x7FFF != tag) continue;
        if (occurrence == index) {
            value_out.* = field.value.ptr;
            length_out.* = @intCast(field.value.len);
            return 0;
        }
        occurrence += 1;
    }
    return 1;
}

/// Read a validated unsigned field, widened to uint64_t.
/// @param[in] payload Borrowed previously validated bytes; may be NULL only when length is zero.
/// @param[in] length Payload bytes, at most InteropMaxPayloadBytes.
/// @param[in] tag Base field tag without the critical bit.
/// @param[in] width Expected width in bytes: 1, 2, 4 or 8.
/// @param[out] value Non-NULL caller-owned scalar, unchanged on error/absence.
/// @return 0 found, 1 absent, -1 for malformed access, invalid width or width mismatch.
/// @note No allocation or retained pointers. Thread-safe for distinct outputs.
export fn interop_field_uint(payload: ?[*]const u8, length: usize, tag: u16, width: u32, value: *u64) c_int {
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    const found = interop_field_get(payload, length, tag, 0, &pointer, &size);
    if (found != 0) return found;
    if (size != width) return -1;
    const bytes = pointer.?[0..size];
    value.* = switch (size) {
        1 => bytes[0],
        2 => get(u16, bytes, 0),
        4 => get(u32, bytes, 0),
        8 => get(u64, bytes, 0),
        else => return -1,
    };
    return 0;
}

/// Read a validated signed 32-bit little-endian field.
/// @param[in] payload Borrowed previously validated bytes; may be NULL only when length is zero.
/// @param[in] length Payload bytes, at most InteropMaxPayloadBytes.
/// @param[in] tag Base field tag without the critical bit.
/// @param[out] value Non-NULL caller-owned scalar, unchanged on error/absence.
/// @return 0 found, 1 absent, -1 for malformed access or a field not four bytes long.
/// @note No allocation or retained pointers. Thread-safe for distinct outputs.
export fn interop_field_i32(payload: ?[*]const u8, length: usize, tag: u16, value: *i32) c_int {
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    const found = interop_field_get(payload, length, tag, 0, &pointer, &size);
    if (found != 0) return found;
    if (size != 4) return -1;
    value.* = get(i32, pointer.?[0..4], 0);
    return 0;
}

/// Reset a writer to borrow caller-owned frame storage.
/// @param[out] writer Non-NULL caller-owned state, fully reset; may not overlap frame storage.
/// @param[in,out] frame Caller-owned writable bytes retained by writer until its
/// reset/disposal; NULL creates a failed writer. No frame bytes are changed by init.
/// @param[in] capacity Readable/writable frame capacity, clamped to InteropMaxFrameBytes.
/// @return No value. NULL frame or capacity below InteropHeaderBytes sets the failed flag.
/// @note No allocation. The caller owns state/storage; serialize calls sharing the writer or
/// its frame.
export fn interop_writer_init(writer: *c_t.interop_writer_t, frame: ?[*]u8, capacity: usize) void {
    const usable = @min(capacity, c_t.InteropMaxFrameBytes);
    writer.* = .{ .frame = frame, .capacity = usable, .length = c_t.InteropHeaderBytes, .last_tag = 0, .fields = 0, .failed = 0 };
    if (frame == null or capacity < c_t.InteropHeaderBytes) writer.failed = 1;
}

fn writer_fail(writer: *c_t.interop_writer_t) c_int {
    writer.failed = 1;
    return error_codes_t.Malformed;
}

/// Append a validated raw field; add its registry critical bit.
/// @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains
/// borrowed until reset or state disposal.
/// @param[in] tag Known base tag, in ascending order; Output repetition is validated by finish.
/// @param[in] value Borrowed input for length bytes; NULL allowed only when length is zero.
/// Must not overlap frame storage.
/// @param[in] length Input bytes, within that tag registry entry limits.
/// @return InteropErrorNone or MalformedFrame for a failed writer, unknown tag, bad
/// order/value/size, field count or capacity. Failure preserves prior frame bytes and poisons
/// the writer.
/// @note No allocation. The caller owns state/storage; serialize calls sharing the writer or
/// its frame.
export fn interop_put_bytes(writer: *c_t.interop_writer_t, tag: u16, value: ?*const anyopaque, length: u16) c_int {
    if (writer.failed != 0) return error_codes_t.Malformed;
    const spec = spec_for(tag) orelse return writer_fail(writer);
    const repeat = tag == writer.last_tag and tag == c_t.InteropTagOutput;
    if (tag <= writer.last_tag and !repeat) return writer_fail(writer);
    if (writer.fields >= c_t.InteropMaxFields) return writer_fail(writer);
    if (length < spec.min or length > spec.max) return writer_fail(writer);
    const bytes: []const u8 = if (length == 0) &.{} else @as([*]const u8, @ptrCast(value orelse return writer_fail(writer)))[0..length];
    if (!value_valid(tag, bytes)) return writer_fail(writer);
    const padded = (@as(usize, length) + 3) & ~@as(usize, 3);
    if (writer.length + 4 + padded > writer.capacity or writer.length + 4 + padded > c_t.InteropMaxFrameBytes)
        return writer_fail(writer);
    const frame = writer.frame[0..writer.capacity];
    const critical_bit: u16 = c_t.InteropCriticalTag;
    const wire_tag: u16 = if (spec.critical) tag | critical_bit else tag;
    put(u16, frame, writer.length, wire_tag);
    put(u16, frame, writer.length + 2, length);
    @memcpy(frame[writer.length + 4 ..][0..length], bytes);
    @memset(frame[writer.length + 4 + length .. writer.length + 4 + padded], 0);
    writer.length += 4 + padded;
    writer.last_tag = tag;
    writer.fields += 1;
    return error_codes_t.None;
}

/// Append an unsigned field of its registry width.
/// @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains
/// borrowed until reset or state disposal.
/// @param[in] tag Known base tag with a fixed 1/2/4/8-byte integer encoding.
/// @param[in] value Integer to encode; narrowing overflow is rejected.
/// @return InteropErrorNone or MalformedFrame for registry, width, range, writer or capacity
/// errors; failure poisons the writer.
/// @note No allocation. The caller owns state/storage; serialize calls sharing the writer or
/// its frame.
export fn interop_put_uint(writer: *c_t.interop_writer_t, tag: u16, value: u64) c_int {
    if (writer.failed != 0) return error_codes_t.Malformed;
    const spec = spec_for(tag) orelse return writer_fail(writer);
    if (spec.min != spec.max) return writer_fail(writer);
    var bytes: [8]u8 = undefined;
    switch (spec.min) {
        1 => {
            if (value > std_t.math.maxInt(u8)) return writer_fail(writer);
            bytes[0] = @intCast(value);
        },
        2 => {
            if (value > std_t.math.maxInt(u16)) return writer_fail(writer);
            put(u16, &bytes, 0, @intCast(value));
        },
        4 => {
            if (value > std_t.math.maxInt(u32)) return writer_fail(writer);
            put(u32, &bytes, 0, @intCast(value));
        },
        8 => put(u64, &bytes, 0, value),
        else => return writer_fail(writer),
    }
    return interop_put_bytes(writer, tag, &bytes, spec.min);
}

/// Append a signed 32-bit little-endian field.
/// @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains
/// borrowed until reset or state disposal.
/// @param[in] tag Known base tag accepting exactly four bytes.
/// @param[in] value Signed value, also checked against tag-specific limits.
/// @return InteropErrorNone or MalformedFrame; failure poisons the writer.
/// @note No allocation. The caller owns state/storage; serialize calls sharing the writer or
/// its frame.
export fn interop_put_i32(writer: *c_t.interop_writer_t, tag: u16, value: i32) c_int {
    var bytes: [4]u8 = undefined;
    put(i32, &bytes, 0, value);
    return interop_put_bytes(writer, tag, &bytes, 4);
}

/// Append a 16-byte rect field.
/// @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains
/// borrowed until reset or state disposal.
/// @param[in] tag Known base tag accepting the rect encoding.
/// @param[in] x Signed guest-pixel x origin.
/// @param[in] y Signed guest-pixel y origin.
/// @param[in] width Physical pixel width, 0..16384.
/// @param[in] height Physical pixel height, 0..16384.
/// @return InteropErrorNone or MalformedFrame; failure poisons the writer.
/// @note No allocation. The caller owns state/storage; serialize calls sharing the writer or
/// its frame.
export fn interop_put_rect(writer: *c_t.interop_writer_t, tag: u16, x: i32, y: i32, width: u32, height: u32) c_int {
    var bytes: [16]u8 = undefined;
    put(i32, &bytes, 0, x);
    put(i32, &bytes, 4, y);
    put(u32, &bytes, 8, width);
    put(u32, &bytes, 12, height);
    return interop_put_bytes(writer, tag, &bytes, 16);
}

/// Append an eight-byte signed point field.
/// @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains
/// borrowed until reset or state disposal.
/// @param[in] tag Known base tag accepting an x/y pair.
/// @param[in] x Signed coordinate subject to tag-specific bounds.
/// @param[in] y Signed coordinate subject to tag-specific bounds.
/// @return InteropErrorNone or MalformedFrame; failure poisons the writer.
/// @note No allocation. The caller owns state/storage; serialize calls sharing the writer or
/// its frame.
export fn interop_put_point(writer: *c_t.interop_writer_t, tag: u16, x: i32, y: i32) c_int {
    var bytes: [8]u8 = undefined;
    put(i32, &bytes, 0, x);
    put(i32, &bytes, 4, y);
    return interop_put_bytes(writer, tag, &bytes, 8);
}

/// Validate payload and publish its protocol-v2 header.
/// @param[in,out] writer Non-NULL caller-owned initialized state; frame storage remains
/// borrowed until reset or state disposal.
/// @param[in] kind Message type for required/allowed field validation.
/// @param[in] request_id Request correlation; zero for unsolicited messages. Runtime enforces
/// outstanding-ID and local stop rules.
/// @param[in] direction Producer checked against the message table.
/// @param[out] frame_length Non-NULL caller-owned total byte count; unchanged on failure.
/// @return InteropErrorNone or payload validation error. Header bytes are written only on
/// success; failure poisons the writer. The caller must not send a frame from a failed writer.
/// @note No allocation. The caller owns state/storage; serialize calls sharing the writer or
/// its frame.
export fn interop_writer_finish(writer: *c_t.interop_writer_t, kind: u16, request_id: u32, direction: c_t.interop_direction_t, frame_length: *usize) c_int {
    if (writer.failed != 0) return error_codes_t.Malformed;
    const frame = writer.frame[0..writer.length];
    const result = validate(kind, direction, frame[c_t.InteropHeaderBytes..]);
    if (result != error_codes_t.None) {
        writer.failed = 1;
        return result;
    }
    put(u32, frame, 0, c_t.InteropMagic);
    put(u16, frame, 4, c_t.InteropVersion);
    put(u16, frame, 6, kind);
    put(u32, frame, 8, @intCast(writer.length - c_t.InteropHeaderBytes));
    put(u32, frame, 12, request_id);
    frame_length.* = writer.length;
    return error_codes_t.None;
}

comptime {
    std_t.debug.assert(@sizeOf(sha256_t) <= 128);
}
/// The c_t struct only guarantees 8-byte alignment, so the context is copied in
/// and out of its opaque bytes instead of being referenced in place.
fn load_hash(transfer: *const c_t.interop_transfer_t) sha256_t {
    return std_t.mem.bytesToValue(sha256_t, transfer.hash_state[0..@sizeOf(sha256_t)]);
}
fn store_hash(transfer: *c_t.interop_transfer_t, state: *const sha256_t) void {
    @memcpy(transfer.hash_state[0..@sizeOf(sha256_t)], std_t.mem.asBytes(state));
}

/// Initialize one receiver transfer accounting state.
/// @param[out] transfer Non-NULL caller-owned state, reset on success only; begin replaces
/// previous state. Caller handles table admission/cancel first.
/// @param[in] id Nonzero transfer ID.
/// @param[in] total_bytes Announced payload bytes, including zero for empty data.
/// @param[in] cap Per-kind receiver limit; the global InteropMaxTransferBytes limit applies too.
/// @return InteropErrorNone, MalformedFrame for zero ID, QuotaExceeded for size over either
/// cap; state unchanged on error.
/// @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
/// transfer. Does not allocate or store payload bytes.
export fn interop_transfer_begin(transfer: *c_t.interop_transfer_t, id: u32, total_bytes: u64, cap: u64) c_int {
    if (id == 0) return error_codes_t.Malformed;
    if (total_bytes > cap or total_bytes > c_t.InteropMaxTransferBytes) return error_codes_t.Quota;
    transfer.* = .{ .id = id, .state = 1, .total_bytes = total_bytes, .received = 0, .hash_state = std_t.mem.zeroes([128]u8) };
    const state = sha256_t.init(.{});
    store_hash(transfer, &state);
    return error_codes_t.None;
}

/// Account and hash one contiguous borrowed chunk.
/// @param[in,out] transfer Non-NULL caller-owned receiving state created by begin; unchanged on
/// error.
/// @param[in] offset Must equal the number of bytes already received.
/// @param[in] data Borrowed chunk bytes, non-NULL on success; NULL is rejected.
/// @param[in] length Chunk bytes, 1..InteropChunkBytes, fitting the remaining total.
/// @return InteropErrorNone or ProtocolViolation for wrong state, gap/overlap, overflow,
/// empty/oversize chunk or NULL data.
/// @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
/// transfer. Caller separately copies/streams accepted bytes to owned payload storage.
export fn interop_transfer_chunk(transfer: *c_t.interop_transfer_t, offset: u64, data: ?[*]const u8, length: usize) c_int {
    if (transfer.state != 1 or length == 0 or length > c_t.InteropChunkBytes or offset != transfer.received or
        length > transfer.total_bytes - transfer.received) return error_codes_t.Protocol;
    const bytes = (data orelse return error_codes_t.Protocol)[0..length];
    var state = load_hash(transfer);
    state.update(bytes);
    store_hash(transfer, &state);
    transfer.received += length;
    return error_codes_t.None;
}

/// Verify the final digest and mark a transfer complete.
/// @param[in,out] transfer Non-NULL caller-owned receiving state; unchanged on error.
/// @param[in] hash Borrowed InteropHashBytes-byte SHA-256 digest; NULL is rejected.
/// @return InteropErrorNone or ProtocolViolation for NULL hash, wrong state, incomplete bytes
/// or digest mismatch.
/// @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
/// transfer.
export fn interop_transfer_end(transfer: *c_t.interop_transfer_t, hash: ?[*]const u8) c_int {
    const expected = (hash orelse return error_codes_t.Protocol)[0..c_t.InteropHashBytes];
    if (transfer.state != 1 or transfer.received != transfer.total_bytes) return error_codes_t.Protocol;
    var copy = load_hash(transfer);
    var digest: [c_t.InteropHashBytes]u8 = undefined;
    copy.final(&digest);
    if (!std_t.crypto.utils.timingSafeEql([c_t.InteropHashBytes]u8, digest, expected.*)) return error_codes_t.Protocol;
    transfer.state = 2;
    return error_codes_t.None;
}

/// Reset a caller-owned transfer to idle.
/// @param[out] transfer Non-NULL caller-owned state; all bytes, including opaque hash context,
/// are zeroed. Valid in any initialized state.
/// @return No value and no error. Does not free payload storage, which belongs to the caller.
/// @note No allocation or retained pointers. Caller owns the state; serialize calls sharing a
/// transfer.
export fn interop_transfer_cancel(transfer: *c_t.interop_transfer_t) void {
    transfer.* = std_t.mem.zeroes(c_t.interop_transfer_t);
}

/// Hash borrowed bytes for a sender.
/// @param[in] data Borrowed readable bytes; NULL allowed only when length is zero.
/// @param[in] length Readable input byte count.
/// @param[out] hash Non-NULL caller-owned output of InteropHashBytes bytes; must not overlap input.
/// @return No value or recoverable error; invalid pointer/length arguments violate the
/// precondition.
/// @note No allocation or retained pointers. Thread-safe for distinct outputs.
export fn interop_sha256(data: ?[*]const u8, length: usize, hash: [*]u8) void {
    const bytes: []const u8 = if (length == 0) &.{} else data.?[0..length];
    sha256_t.hash(bytes, hash[0..c_t.InteropHashBytes], .{});
}

// ---------------------------------------------------------------------------
// Test-only fixtures.
// ---------------------------------------------------------------------------

test "AUMID keys reserve suffix space and preserve outputs on every error" {
    var key = [_]u8{0xA5} ** c_t.InteropMaxAppKeyBytes;
    var length: usize = 777;
    const case_t = struct { input: []const u8, expected: []const u8 };
    const Cases = [_]case_t{
        .{ .input = "Explorer", .expected = "a-Explorer" },
        .{ .input = "A.B", .expected = "a-A_B-4b861d8b" },
        .{ .input = "A B", .expected = "a-A_B-fea4c5ce" },
        .{ .input = "ÿ", .expected = "a-__-ea47fa96" },
        .{ .input = "x" ** 198, .expected = "a-" ++ "x" ** 198 },
        .{ .input = "x" ** 199, .expected = "a-" ++ "x" ** 189 ++ "-f18029ae" },
        .{ .input = "x" ** 4096, .expected = "a-" ++ "x" ** 189 ++ "-a2e659da" },
    };
    for (Cases) |case| {
        @memset(&key, 0xA5);
        try testing_t.expectEqual(error_codes_t.None, interop_app_key_from_aumid(opaque_value(?[*]const u8, case.input.ptr), opaque_value(usize, case.input.len), &key, opaque_value(usize, key.len), &length));
        try testing_t.expectEqualStrings(case.expected, key[0..length]);
        try testing_t.expect(length <= c_t.InteropMaxAppKeyBytes);
        try testing_t.expect(app_key_valid(key[0..length]));
        for (key[length..]) |byte| try testing_t.expectEqual(@as(u8, 0xA5), byte);
        // Exactly one byte too small must leave the whole output and length intact.
        @memset(&key, 0xA5);
        const previous = length;
        try testing_t.expectEqual(error_codes_t.Quota, interop_app_key_from_aumid(case.input.ptr, case.input.len, &key, opaque_value(usize, length - 1), &length));
        try testing_t.expectEqual(previous, length);
        for (key) |byte| try testing_t.expectEqual(@as(u8, 0xA5), byte);
    }
    const Invalid = [_][]const u8{ "", "a\x00b", "\xff", "x" ** 4097 };
    for (Invalid) |input| {
        try testing_t.expectEqual(error_codes_t.Malformed, interop_app_key_from_aumid(opaque_value(?[*]const u8, input.ptr), opaque_value(usize, input.len), &key, key.len, &length));
    }
    try testing_t.expectEqual(error_codes_t.Malformed, interop_app_key_from_aumid(opaque_value(?[*]const u8, null), opaque_value(usize, 1), &key, key.len, &length));
    for (key) |byte| try testing_t.expectEqual(@as(u8, 0xA5), byte);
    // Full input consumption before copying permits overlapping caller storage.
    @memcpy(key[0..3], "A.B");
    try testing_t.expectEqual(error_codes_t.None, interop_app_key_from_aumid(&key, 3, &key, key.len, &length));
    try testing_t.expectEqualStrings("a-A_B-4b861d8b", key[0..length]);
}

test "window minimum sizes are independent of current geometry and bounded per axis" {
    var buffer = frame_buffer();
    for ([_]u16{ c_t.InteropMsgWindowAnnounce, c_t.InteropMsgWindowUpdate }) |kind| {
        var writer = writer_for(&buffer);
        _ = interop_put_uint(&writer, c_t.InteropTagWindow, 1);
        _ = interop_put_uint(&writer, c_t.InteropTagGeneration, 2);
        if (kind == c_t.InteropMsgWindowAnnounce) _ = interop_put_uint(&writer, c_t.InteropTagRole, 1);
        _ = interop_put_rect(&writer, c_t.InteropTagRect, 10, 20, 640, 480);
        if (kind == c_t.InteropMsgWindowAnnounce) _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "a-App", 5);
        _ = interop_put_uint(&writer, c_t.InteropTagState, 0);
        _ = interop_put_uint(&writer, c_t.InteropTagDpi, 96);
        var minimum: [8]u8 = undefined;
        put(u32, &minimum, 0, 320);
        put(u32, &minimum, 4, 200);
        try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagMinSize, &minimum, 8));
        var size: usize = 0;
        try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, kind, 0, c_t.InteropFromGuest, &size));
        var value: ?[*]const u8 = null;
        var value_length: u16 = 0;
        const payload = buffer[c_t.InteropHeaderBytes..size];
        try testing_t.expectEqual(@as(c_int, 0), interop_field_get(payload.ptr, payload.len, c_t.InteropTagMinSize, 0, &value, &value_length));
        try testing_t.expectEqual(@as(u16, 8), value_length);
        try testing_t.expectEqual(@as(u32, 320), get(u32, value.?[0..8], 0));
        try testing_t.expectEqual(@as(c_int, 0), interop_field_get(payload.ptr, payload.len, c_t.InteropTagRect, 0, &value, &value_length));
        try testing_t.expectEqual(@as(u32, 640), get(u32, value.?[0..16], 8));
    }
    var writer = writer_for(&buffer);
    const Zero = [_]u8{0} ** 8;
    try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagMinSize, &Zero, 8));
    var size: usize = 0;
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgPing, 0, c_t.InteropFromHost, &size));
    var minimum = Zero;
    for ([_]usize{ 0, 4 }) |axis| {
        writer = writer_for(&buffer);
        @memset(&minimum, 0);
        put(u32, &minimum, axis, 16385);
        try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagMinSize, &minimum, 8));
    }
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagMinSize, &Zero, 4));
}

test "menu trigger serials are protocol references and local stop frames never cross VSOCK" {
    var buffer = frame_buffer();
    for ([_]u8{ c_t.InteropRoleMenu, c_t.InteropRoleToplevel }) |role| {
        var writer = writer_for(&buffer);
        _ = interop_put_uint(&writer, c_t.InteropTagWindow, 1);
        _ = interop_put_uint(&writer, c_t.InteropTagGeneration, 1);
        _ = interop_put_uint(&writer, c_t.InteropTagRole, role);
        _ = interop_put_rect(&writer, c_t.InteropTagRect, 0, 0, 32, 32);
        _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "a-App", 5);
        _ = interop_put_uint(&writer, c_t.InteropTagState, 0);
        _ = interop_put_uint(&writer, c_t.InteropTagDpi, 96);
        _ = interop_put_uint(&writer, c_t.InteropTagSerial, 42);
        var size: usize = 0;
        const expected = if (role == c_t.InteropRoleMenu) error_codes_t.None else error_codes_t.Malformed;
        try testing_t.expectEqual(expected, interop_writer_finish(&writer, c_t.InteropMsgWindowAnnounce, 0, c_t.InteropFromGuest, &size));
        if (role == c_t.InteropRoleMenu) {
            var serial: u64 = 0;
            try testing_t.expectEqual(@as(c_int, 0), interop_field_uint(buffer[16..].ptr, size - 16, c_t.InteropTagSerial, 4, &serial));
            try testing_t.expectEqual(@as(u64, 42), serial);
        }
    }
    for ([_]u16{ c_t.InteropMsgLocalStop, c_t.InteropMsgLocalStopResult }) |kind| {
        var writer = writer_for(&buffer);
        if (kind == c_t.InteropMsgLocalStopResult) _ = interop_put_uint(&writer, c_t.InteropTagStatus, c_t.InteropOk);
        var size: usize = 0;
        try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, kind, 19, c_t.InteropFromLocal, &size));
        var header: c_t.interop_header_t = undefined;
        try testing_t.expectEqual(error_codes_t.None, interop_header_decode(&buffer, size, &header));
        try testing_t.expectEqual(@as(u32, 19), header.request_id);
        for ([_]c_t.interop_direction_t{ c_t.InteropFromHost, c_t.InteropFromGuest }) |direction|
            try testing_t.expectEqual(error_codes_t.UnsupportedType, interop_payload_validate(kind, direction, buffer[16..].ptr, size - 16));
    }
}

/// Keeps test inputs opaque to the optimizer so exported functions run their
/// out-of-line branches instead of constant-folded inlined copies.
fn opaque_value(comptime value_t: type, value: value_t) value_t {
    var copy = value;
    std_t.mem.doNotOptimizeAway(&copy);
    return copy;
}

const testing_t = std_t.testing;

fn frame_buffer() [c_t.InteropMaxFrameBytes]u8 {
    return std_t.mem.zeroes([c_t.InteropMaxFrameBytes]u8);
}

fn writer_for(buffer: []u8) c_t.interop_writer_t {
    var writer: c_t.interop_writer_t = undefined;
    interop_writer_init(&writer, buffer.ptr, buffer.len);
    return writer;
}

fn hello(buffer: []u8, epoch: u64) !usize {
    var writer = writer_for(buffer);
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagCapabilities, c_t.InteropCapWindows | c_t.InteropCapInput));
    try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagAgentVersion, "0.2.0", 5));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagLayoutVersion, c_t.InteropLayoutVersion));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagSessionEpoch, epoch));
    var length: usize = 0;
    try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, c_t.InteropMsgHello, 0, c_t.InteropFromGuest, &length));
    return length;
}

test "hello round trip, header decode and typed field access" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 0x1122334455667788);
    var header: c_t.interop_header_t = undefined;
    try testing_t.expectEqual(error_codes_t.None, interop_header_decode(&buffer, length, &header));
    try testing_t.expectEqual(@as(u16, c_t.InteropMsgHello), header.type);
    try testing_t.expectEqual(length - 16, header.length);
    const payload = buffer[16..length];
    try testing_t.expectEqual(error_codes_t.None, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, payload.ptr, payload.len));
    var epoch: u64 = 0;
    try testing_t.expectEqual(@as(c_int, 0), interop_field_uint(payload.ptr, payload.len, c_t.InteropTagSessionEpoch, 8, &epoch));
    try testing_t.expectEqual(@as(u64, 0x1122334455667788), epoch);
    try testing_t.expectEqual(@as(c_int, 1), interop_field_uint(payload.ptr, payload.len, c_t.InteropTagMessage, 4, &epoch));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_uint(payload.ptr, payload.len, c_t.InteropTagSessionEpoch, 4, &epoch));
    // Direction is enforced: a guest Hello cannot come from the host.
    try testing_t.expectEqual(error_codes_t.UnsupportedType, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromHost, payload.ptr, payload.len));
    try testing_t.expectEqual(error_codes_t.UnsupportedType, interop_payload_validate(0x0FFF, c_t.InteropFromGuest, payload.ptr, payload.len));
    try testing_t.expectEqual(error_codes_t.UnsupportedType, interop_payload_validate(c_t.InteropMsgHello, 9, payload.ptr, payload.len));
}

test "header rejects magic, version, length and short input" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 1);
    var header = std_t.mem.zeroes(c_t.interop_header_t);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_header_decode(&buffer, 15, &header));
    try testing_t.expectEqual(error_codes_t.Malformed, interop_header_decode(null, 16, &header));
    var bad = buffer;
    bad[0] ^= 1;
    try testing_t.expectEqual(error_codes_t.Protocol, interop_header_decode(&bad, length, &header));
    bad = buffer;
    put(u16, &bad, 4, c_t.InteropVersion + 1);
    try testing_t.expectEqual(error_codes_t.Protocol, interop_header_decode(&bad, length, &header));
    bad = buffer;
    bad[4] = 1; // The superseded protocol v1 must not be accepted as v2.
    try testing_t.expectEqual(error_codes_t.Protocol, interop_header_decode(&bad, length, &header));
    bad = buffer;
    put(u32, &bad, 8, 65524);
    try testing_t.expectEqual(error_codes_t.Protocol, interop_header_decode(&bad, length, &header));
    put(u32, &bad, 8, 6);
    try testing_t.expectEqual(error_codes_t.Protocol, interop_header_decode(&bad, length, &header));
    try testing_t.expectEqual(@as(u32, 0), header.magic); // unchanged on every failure
}

test "structural defects: order, duplicates, padding, criticality, unknown tags" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 7);
    const payload = buffer[16..length];
    // Swap the first two fields (capabilities 0x3A, agent_version 0x3B) -> out of order.
    var swapped: [256]u8 = undefined;
    const First = 4 + 8; // capabilities: header + 8 bytes
    const Second = 4 + 8; // "0.2.0" padded to 8
    @memcpy(swapped[0..Second], payload[First .. First + Second]);
    @memcpy(swapped[Second .. Second + First], payload[0..First]);
    @memcpy(swapped[First + Second .. payload.len], payload[First + Second ..]);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, &swapped, payload.len));
    // Nonzero padding after the 5-byte version string.
    var padded: [256]u8 = undefined;
    @memcpy(padded[0..payload.len], payload);
    padded[First + 4 + 5] = 1;
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, &padded, payload.len));
    // Criticality bit flipped on a known tag.
    @memcpy(padded[0..payload.len], payload);
    padded[1] &= 0x7F;
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, &padded, payload.len));
    // Unknown optional tag is skipped; unknown critical tag is refused.
    var extended: [256]u8 = undefined;
    @memcpy(extended[0..payload.len], payload);
    put(u16, &extended, payload.len, 0x0100);
    put(u16, &extended, payload.len + 2, 3);
    @memcpy(extended[payload.len + 4 ..][0..4], &[_]u8{ 1, 2, 3, 0 });
    try testing_t.expectEqual(error_codes_t.None, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, &extended, payload.len + 8));
    put(u16, &extended, payload.len, 0x8100);
    try testing_t.expectEqual(error_codes_t.UnsupportedField, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, &extended, payload.len + 8));
    // Truncated field header and truncated value.
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, payload.ptr, payload.len - 4));
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, payload.ptr, 2));
    // Missing required field (drop the trailing session epoch).
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgHello, c_t.InteropFromGuest, payload.ptr, payload.len - 12));
    // Zero tag.
    var zero = [_]u8{ 0, 0, 0, 0 };
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, &zero, 4));
    try testing_t.expectEqual(error_codes_t.None, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, null, 0));
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, null, 4));
}

test "field present but not allowed for the message type is malformed" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagWindow, 5));
    var length: usize = 0;
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgPing, 0, c_t.InteropFromHost, &length));
    try testing_t.expectEqual(@as(usize, 0), length);
    // The writer stays poisoned after a failed finish.
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagGeneration, 1));
}

test "writer rejects order, values, capacity and poisons itself" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagGeneration, 3));
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagWindow, 3));
    try testing_t.expect(writer.failed != 0);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagRole, 1));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagWindow, 0)); // nonzero rule
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagRole, 300)); // width overflow
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, 0x7000, 1)); // unknown tag
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagTitle, 1)); // variable-size via uint
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagTitle, null, 3));
    var small: [20]u8 = undefined;
    writer = writer_for(&small);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagWindow, 1));
    writer = writer_for(small[0..10]);
    try testing_t.expect(writer.failed != 0);
    var length: usize = 0;
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgPing, 0, c_t.InteropFromHost, &length));
    interop_writer_init(&writer, null, 100);
    try testing_t.expect(writer.failed != 0);
}

test "window announce with every field, parent pairing and string rules" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagWindow, 0x10024));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagGeneration, 1));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagParent, 0x10020));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagParentGeneration, 1));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagRole, c_t.InteropRoleDialog));
    try testing_t.expectEqual(error_codes_t.None, interop_put_rect(&writer, c_t.InteropTagRect, -10, 20, 640, 480));
    const Title = "Save As — Notepad";
    try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagTitle, Title, Title.len));
    try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagAppKey, "a-Microsoft_WindowsNotepad", 26));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagProcessId, 4242));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagState, c_t.InteropStateResizable));
    try testing_t.expectEqual(error_codes_t.None, interop_put_uint(&writer, c_t.InteropTagDpi, 144));
    var length: usize = 0;
    try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, c_t.InteropMsgWindowAnnounce, 0, c_t.InteropFromGuest, &length));
    const payload = buffer[16..length];
    var x: i32 = 0;
    try testing_t.expectEqual(@as(c_int, 1), interop_field_i32(payload.ptr, payload.len, c_t.InteropTagAxisV120, &x));
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    try testing_t.expectEqual(@as(c_int, 0), interop_field_get(payload.ptr, payload.len, c_t.InteropTagTitle, 0, &pointer, &size));
    try testing_t.expectEqualStrings(Title, pointer.?[0..size]);
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(payload.ptr, payload.len, c_t.InteropTagTitle, 0, null, &size));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(null, 8, c_t.InteropTagTitle, 0, &pointer, &size));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_i32(payload.ptr, payload.len, c_t.InteropTagWindow, &x));

    // Parent without its generation is rejected by the cross-field rule.
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c_t.InteropTagWindow, 1);
    _ = interop_put_uint(&writer, c_t.InteropTagGeneration, 1);
    _ = interop_put_uint(&writer, c_t.InteropTagParent, 2);
    _ = interop_put_uint(&writer, c_t.InteropTagRole, 1);
    _ = interop_put_rect(&writer, c_t.InteropTagRect, 0, 0, 1, 1);
    _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "p-00", 4);
    _ = interop_put_uint(&writer, c_t.InteropTagState, 0);
    _ = interop_put_uint(&writer, c_t.InteropTagDpi, 96);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgWindowAnnounce, 0, c_t.InteropFromGuest, &length));

    // String and key rules.
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagAppKey, "x-bad", 5));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagAppKey, "a-dot.ted", 9));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagTitle, "a\x00b", 3));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagTitle, "\xff\xfe", 2));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_rect(&writer, c_t.InteropTagRect, 0, 0, 16385, 1));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagDpi, 95));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagState, 0x80));
}

const boundary_case_t = struct { tag: u16, good: []const u8, bad: []const u8 };
const little_endian_t = struct {
    fn u16_(comptime v: u16) [2]u8 {
        var b: [2]u8 = undefined;
        std_t.mem.writeInt(u16, &b, v, .little);
        return b;
    }
    fn u32_(comptime v: u32) [4]u8 {
        var b: [4]u8 = undefined;
        std_t.mem.writeInt(u32, &b, v, .little);
        return b;
    }
    fn i32_(comptime v: i32) [4]u8 {
        var b: [4]u8 = undefined;
        std_t.mem.writeInt(i32, &b, v, .little);
        return b;
    }
    fn pair(comptime a: i32, comptime b2: i32) [8]u8 {
        return i32_(a) ++ i32_(b2);
    }
    fn u64_(comptime v: u64) [8]u8 {
        var b: [8]u8 = undefined;
        std_t.mem.writeInt(u64, &b, v, .little);
        return b;
    }
};
const BoundaryCases = [_]boundary_case_t{
    .{ .tag = c_t.InteropTagMinSize, .good = &(little_endian_t.u32_(16384) ++ little_endian_t.u32_(0)), .bad = &(little_endian_t.u32_(0) ++ little_endian_t.u32_(16385)) },
    .{ .tag = c_t.InteropTagRole, .good = &.{6}, .bad = &.{7} },
    .{ .tag = c_t.InteropTagSlot, .good = &little_endian_t.u32_(191), .bad = &little_endian_t.u32_(192) },
    .{ .tag = c_t.InteropTagDamage, .good = &(little_endian_t.pair(0, 0) ++ little_endian_t.u32_(16384) ++ little_endian_t.u32_(1)), .bad = &(little_endian_t.pair(0, 0) ++ little_endian_t.u32_(16385) ++ little_endian_t.u32_(1)) },
    .{ .tag = c_t.InteropTagEdge, .good = &.{10}, .bad = &.{3} },
    .{ .tag = c_t.InteropTagSerial, .good = &little_endian_t.u32_(1), .bad = &little_endian_t.u32_(0) },
    .{ .tag = c_t.InteropTagPoint, .good = &little_endian_t.pair(-16384, 16384), .bad = &little_endian_t.pair(0, 16385) },
    .{ .tag = c_t.InteropTagButton, .good = &little_endian_t.u16_(5), .bad = &little_endian_t.u16_(6) },
    .{ .tag = c_t.InteropTagPressed, .good = &.{1}, .bad = &.{2} },
    .{ .tag = c_t.InteropTagKey, .good = &little_endian_t.u16_(767), .bad = &little_endian_t.u16_(768) },
    .{ .tag = c_t.InteropTagAxisV120, .good = &little_endian_t.i32_(-15360), .bad = &little_endian_t.i32_(15361) },
    .{ .tag = c_t.InteropTagAxisH120, .good = &little_endian_t.i32_(15360), .bad = &little_endian_t.i32_(-15361) },
    .{ .tag = c_t.InteropTagDelta, .good = &little_endian_t.pair(16384 << 8, 0), .bad = &little_endian_t.pair(0, -(16384 << 8) - 1) },
    .{ .tag = c_t.InteropTagTouchId, .good = &little_endian_t.u32_(9), .bad = &little_endian_t.u32_(10) },
    .{ .tag = c_t.InteropTagPressure, .good = &little_endian_t.u16_(1024), .bad = &little_endian_t.u16_(1025) },
    .{ .tag = c_t.InteropTagTilt, .good = &.{ 0x28, 0x23, 0xD8, 0xDC }, .bad = &.{ 0x29, 0x23, 0, 0 } },
    .{ .tag = c_t.InteropTagRotation, .good = &little_endian_t.u16_(35999), .bad = &little_endian_t.u16_(36000) },
    .{ .tag = c_t.InteropTagTool, .good = &.{2}, .bad = &.{0} },
    .{ .tag = c_t.InteropTagCursorBegin, .good = &little_endian_t.i32_(-1), .bad = &little_endian_t.i32_(-2) },
    .{ .tag = c_t.InteropTagCursorEnd, .good = &little_endian_t.i32_(4096), .bad = &little_endian_t.i32_(4097) },
    .{ .tag = c_t.InteropTagLayout, .good = "0000040c", .bad = "0000040g" },
    .{ .tag = c_t.InteropTagLocks, .good = &.{7}, .bad = &.{8} },
    .{ .tag = c_t.InteropTagCursorShape, .good = &little_endian_t.u16_(0xFFFF), .bad = &little_endian_t.u16_(35) },
    .{ .tag = c_t.InteropTagHotspot, .good = &little_endian_t.pair(512, 0), .bad = &little_endian_t.pair(-1, 0) },
    .{ .tag = c_t.InteropTagImageSize, .good = &(little_endian_t.u32_(512) ++ little_endian_t.u32_(1)), .bad = &(little_endian_t.u32_(0) ++ little_endian_t.u32_(1)) },
    .{ .tag = c_t.InteropTagTransfer, .good = &little_endian_t.u32_(1), .bad = &little_endian_t.u32_(0) },
    .{ .tag = c_t.InteropTagTotalBytes, .good = &little_endian_t.u64_(c_t.InteropMaxTransferBytes), .bad = &little_endian_t.u64_(c_t.InteropMaxTransferBytes + 1) },
    .{ .tag = c_t.InteropTagOffset, .good = &little_endian_t.u64_(c_t.InteropMaxTransferBytes - 1), .bad = &little_endian_t.u64_(c_t.InteropMaxTransferBytes) },
    .{ .tag = c_t.InteropTagMime, .good = "text/plain;charset=utf-8", .bad = "text/plain; charset" },
    .{ .tag = c_t.InteropTagMimeList, .good = &([_]u8{9} ++ "image/png".*), .bad = &([_]u8{10} ++ "image/png".*) },
    .{ .tag = c_t.InteropTagDropEffect, .good = &.{7}, .bad = &.{8} },
    .{ .tag = c_t.InteropTagAppFlags, .good = &little_endian_t.u32_(0x1F), .bad = &little_endian_t.u32_(0x20) },
    .{ .tag = c_t.InteropTagIconSizes, .good = &(little_endian_t.u16_(16) ++ little_endian_t.u16_(256)), .bad = &(little_endian_t.u16_(32) ++ little_endian_t.u16_(16)) },
    .{ .tag = c_t.InteropTagIconSize, .good = &little_endian_t.u16_(48), .bad = &little_endian_t.u16_(40) },
    .{ .tag = c_t.InteropTagLaunchKind, .good = &.{5}, .bad = &.{6} },
    .{ .tag = c_t.InteropTagActivationToken, .good = "abc_123", .bad = "abc 123" },
    .{ .tag = c_t.InteropTagStatus, .good = &little_endian_t.u32_(9), .bad = &little_endian_t.u32_(10) },
    .{ .tag = c_t.InteropTagCapabilities, .good = &little_endian_t.u64_(c_t.InteropCapabilityMask), .bad = &little_endian_t.u64_(c_t.InteropCapabilityMask + 1) },
    .{ .tag = c_t.InteropTagAgentVersion, .good = "10.20.30", .bad = "1.2.3.4" },
    .{ .tag = c_t.InteropTagLayoutVersion, .good = &little_endian_t.u32_(2), .bad = &little_endian_t.u32_(0) },
    .{ .tag = c_t.InteropTagOutput, .good = &(little_endian_t.u32_(0) ++ little_endian_t.u32_(3840) ++ little_endian_t.u32_(2160) ++ little_endian_t.u32_(480)), .bad = &(little_endian_t.u32_(0) ++ little_endian_t.u32_(3840) ++ little_endian_t.u32_(2160) ++ little_endian_t.u32_(119)) },
    .{ .tag = c_t.InteropTagStream, .good = &little_endian_t.u32_(7), .bad = &little_endian_t.u32_(8) },
    .{ .tag = c_t.InteropTagPrinterName, .good = "Office_Laser", .bad = "Office/Laser" },
    .{ .tag = c_t.InteropTagPrinterUri, .good = "http://10.0.2.100:631/printers/x", .bad = "http://a b" },
    .{ .tag = c_t.InteropTagDecision, .good = &.{1}, .bad = &.{2} },
    .{ .tag = c_t.InteropTagErrorCode, .good = &little_endian_t.u32_(8), .bad = &little_endian_t.u32_(9) },
    .{ .tag = c_t.InteropTagIntervalMs, .good = &little_endian_t.u32_(10000), .bad = &little_endian_t.u32_(10001) },
    .{ .tag = c_t.InteropTagRate, .good = &little_endian_t.u32_(1000), .bad = &little_endian_t.u32_(1001) },
    .{ .tag = c_t.InteropTagGeneration, .good = &little_endian_t.u64_(1), .bad = &little_endian_t.u64_(0) },
    .{ .tag = c_t.InteropTagUriList, .good = "file:///home/u/a.txt\r\n", .bad = "\x00" },
};

test "every field value rule accepts its boundary and rejects one past it" {
    var buffer: [1024]u8 = undefined;
    for (BoundaryCases) |case| {
        var writer = writer_for(&buffer);
        testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, case.tag, case.good.ptr, @intCast(case.good.len))) catch |err| {
            std_t.debug.print("tag 0x{x} rejected boundary value\n", .{case.tag});
            return err;
        };
        writer = writer_for(&buffer);
        testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, case.tag, case.bad.ptr, @intCast(case.bad.len))) catch |err| {
            std_t.debug.print("tag 0x{x} accepted out-of-range value\n", .{case.tag});
            return err;
        };
    }
    // Mime lists: empty entry, 33 entries.
    var writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagMimeList, &[_]u8{ 0, 0 }, 2));
    var many: [33 * 2]u8 = undefined;
    for (0..33) |i| {
        many[i * 2] = 1;
        many[i * 2 + 1] = 'a';
    }
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagMimeList, &many, many.len));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagMimeList, &many, 64));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagAgentVersion, "1..2", 4));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagAgentVersion, "1.2.", 4));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagAgentVersion, "1.2.3333333333", 14));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagDamage, "0123456789", 10));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagIconSizes, "abc", 3));
}

test "launch requests require exactly one target kind" {
    var buffer = frame_buffer();
    var length: usize = 0;
    var writer = writer_for(&buffer);
    _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "a-Microsoft_Windows_Explorer", 28);
    _ = interop_put_uint(&writer, c_t.InteropTagLaunchKind, c_t.InteropLaunchApp);
    _ = interop_put_bytes(&writer, c_t.InteropTagActivationToken, "token-1", 7);
    try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, c_t.InteropMsgLaunchRequest, 3, c_t.InteropFromHost, &length));
    try testing_t.expectEqual(@as(u32, 3), get(u32, &buffer, 12));
    writer = writer_for(&buffer);
    _ = interop_put_bytes(&writer, c_t.InteropTagTargetPath, "Z:\\Downloads\\setup.exe", 22);
    _ = interop_put_uint(&writer, c_t.InteropTagLaunchKind, c_t.InteropLaunchPath);
    try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, c_t.InteropMsgLocalLaunch, 0, c_t.InteropFromLocal, &length));
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c_t.InteropTagLaunchKind, c_t.InteropLaunchApp);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgLaunchRequest, 0, c_t.InteropFromHost, &length));
    writer = writer_for(&buffer);
    _ = interop_put_bytes(&writer, c_t.InteropTagTargetPath, "C:\\x.exe", 8);
    _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c_t.InteropTagLaunchKind, c_t.InteropLaunchPath);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgLaunchRequest, 0, c_t.InteropFromHost, &length));
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c_t.InteropTagLaunchKind, c_t.InteropLaunchPath);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgLocalLaunch, 0, c_t.InteropFromLocal, &length));
}

test "icon requests pair window and generation" {
    var buffer = frame_buffer();
    var length: usize = 0;
    var writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c_t.InteropTagWindow, 9);
    _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c_t.InteropTagTransfer, 1);
    _ = interop_put_uint(&writer, c_t.InteropTagIconSize, 32);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgIconRequest, 0, c_t.InteropFromHost, &length));
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c_t.InteropTagWindow, 9);
    _ = interop_put_uint(&writer, c_t.InteropTagGeneration, 2);
    _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c_t.InteropTagTransfer, 1);
    _ = interop_put_uint(&writer, c_t.InteropTagIconSize, 32);
    try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, c_t.InteropMsgIconRequest, 0, c_t.InteropFromHost, &length));
}

test "display config repeats outputs 1..8 times and nowhere else" {
    var buffer = frame_buffer();
    var output = [_]u8{0} ** 16;
    put(u32, &output, 4, 2560);
    put(u32, &output, 8, 1440);
    put(u32, &output, 12, 150);
    var writer = writer_for(&buffer);
    var length: usize = 0;
    for (0..8) |index| {
        put(u32, &output, 0, @intCast(index));
        try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagOutput, &output, 16));
    }
    try testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, c_t.InteropMsgDisplayConfig, 0, c_t.InteropFromHost, &length));
    const payload = buffer[16..length];
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    try testing_t.expectEqual(@as(c_int, 0), interop_field_get(payload.ptr, payload.len, c_t.InteropTagOutput, 7, &pointer, &size));
    try testing_t.expectEqual(@as(u32, 7), get(u32, pointer.?[0..size], 0));
    try testing_t.expectEqual(@as(c_int, 1), interop_field_get(payload.ptr, payload.len, c_t.InteropTagOutput, 8, &pointer, &size));
    // Nine outputs are rejected.
    try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, c_t.InteropTagOutput, &output, 16));
    writer.failed = 0;
    try testing_t.expectEqual(error_codes_t.Malformed, interop_writer_finish(&writer, c_t.InteropMsgDisplayConfig, 0, c_t.InteropFromHost, &length));
    // A repeated tag in any other message is out of order.
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c_t.InteropTagStatus, 0);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagStatus, 0));
    // Hand-built repeat of a non-output tag in DisplayConfig position is refused by the validator.
    var raw: [24]u8 = std_t.mem.zeroes([24]u8);
    put(u16, &raw, 0, c_t.InteropTagStatus | c_t.InteropCriticalTag);
    put(u16, &raw, 2, 4);
    put(u16, &raw, 8, c_t.InteropTagStatus | c_t.InteropCriticalTag);
    put(u16, &raw, 10, 4);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgTransferCancel, c_t.InteropFromHost, &raw, 16));
}

test "field count limit is enforced by writer and validator" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    writer.fields = c_t.InteropMaxFields;
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagWindow, 1));
    // 65 unknown optional fields fail validation even though each is skippable.
    var raw: [65 * 4]u8 = undefined;
    for (0..65) |index| {
        put(u16, &raw, index * 4, @intCast(0x100 + index));
        put(u16, &raw, index * 4 + 2, 0);
    }
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, &raw, raw.len));
    try testing_t.expectEqual(error_codes_t.None, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, &raw, 64 * 4));
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, &raw, 65530));
}

test "transfers: contiguity, bounds, digest and cancellation" {
    const Content = "The quick brown fox jumps over the lazy dog";
    var digest: [32]u8 = undefined;
    interop_sha256(Content, Content.len, &digest);
    var transfer = std_t.mem.zeroes(c_t.interop_transfer_t);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_transfer_begin(&transfer, 0, 4, 64));
    try testing_t.expectEqual(error_codes_t.Quota, interop_transfer_begin(&transfer, 1, 65, 64));
    try testing_t.expectEqual(error_codes_t.Quota, interop_transfer_begin(&transfer, 1, c_t.InteropMaxTransferBytes + 1, std_t.math.maxInt(u64)));
    try testing_t.expectEqual(@as(u32, 0), transfer.id);
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_chunk(&transfer, 0, Content, 4)); // not begun
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_begin(&transfer, 7, Content.len, 64));
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_chunk(&transfer, 1, Content, 4)); // gap
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_chunk(&transfer, 0, Content, 0)); // empty
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_chunk(&transfer, 0, null, 4));
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_chunk(&transfer, 0, Content, 10));
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_chunk(&transfer, 0, Content, 10)); // overlap
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_end(&transfer, &digest)); // incomplete
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_chunk(&transfer, 10, Content[10..].ptr, Content.len)); // overflow
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_chunk(&transfer, 10, Content[10..].ptr, Content.len - 10));
    var wrong = digest;
    wrong[0] ^= 0xFF;
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_end(&transfer, &wrong));
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_end(&transfer, null));
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_end(&transfer, &digest));
    try testing_t.expectEqual(@as(u32, 2), transfer.state);
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_end(&transfer, &digest)); // already complete
    interop_transfer_cancel(&transfer);
    try testing_t.expectEqual(@as(u32, 0), transfer.state);
    // Oversize chunk and empty-transfer digest.
    var big = std_t.mem.zeroes([c_t.InteropChunkBytes + 1]u8);
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_begin(&transfer, 2, big.len, big.len));
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_chunk(&transfer, 0, &big, big.len));
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_begin(&transfer, 3, 0, 0));
    interop_sha256(null, 0, &digest);
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_end(&transfer, &digest));
}

test "every message type has a buildable minimal frame and rejects the wrong direction" {
    var buffer = frame_buffer();
    for (Messages) |message| {
        var writer = writer_for(&buffer);
        var tags: [16]u16 = undefined;
        var count = message.required.len;
        @memcpy(tags[0..count], message.required);
        // Launch messages need one target to satisfy the cross-field rule.
        if (message.type == c_t.InteropMsgLaunchRequest or message.type == c_t.InteropMsgLocalLaunch) {
            tags[count] = c_t.InteropTagTargetPath;
            count += 1;
        }
        std_t.mem.sort(u16, tags[0..count], {}, std_t.sort.asc(u16));
        for (tags[0..count]) |tag| {
            const value = if (tag == c_t.InteropTagTargetPath) "C:\\a" else sample_value(tag);
            try testing_t.expectEqual(error_codes_t.None, interop_put_bytes(&writer, tag, value.ptr, @intCast(value.len)));
        }
        const direction: c_t.interop_direction_t = if (message.directions & DirGuest != 0) c_t.InteropFromGuest else if (message.directions & DirHost != 0) c_t.InteropFromHost else c_t.InteropFromLocal;
        var length: usize = 0;
        testing_t.expectEqual(error_codes_t.None, interop_writer_finish(&writer, message.type, 0, direction, &length)) catch |err| {
            std_t.debug.print("message 0x{x} minimal frame rejected\n", .{message.type});
            return err;
        };
        const payload = buffer[16..length];
        const other: c_t.interop_direction_t = if (message.directions & DirLocal != 0) c_t.InteropFromGuest else if (message.directions == DirBoth) c_t.InteropFromLocal else if (direction == c_t.InteropFromGuest) c_t.InteropFromHost else c_t.InteropFromGuest;
        try testing_t.expectEqual(error_codes_t.UnsupportedType, interop_payload_validate(message.type, other, payload.ptr, payload.len));
    }
}

fn sample_value(tag: u16) []const u8 {
    const sample_values_t = struct {
        const One8 = [_]u8{ 1, 0, 0, 0, 0, 0, 0, 0 };
        const One4 = [_]u8{ 1, 0, 0, 0 };
        const One2 = [_]u8{ 16, 0 };
        const One1 = [_]u8{1};
        const Rect = [_]u8{0} ** 8 ++ [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0 };
        const Point = [_]u8{0} ** 8;
        const Dpi = [_]u8{ 96, 0, 0, 0 };
        const Output = [_]u8{0} ** 4 ++ [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0, 120, 0, 0, 0 };
        const Hash = [_]u8{0} ** 32;
        const Usb = [_]u8{ 1, 0, 2, 0, 1, 1 };
        const Zero4 = [_]u8{ 0, 0, 0, 0 };
        const Image = [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0 };
        const Mimes = [_]u8{4} ++ "text".*;
    };
    return switch (tag) {
        c_t.InteropTagWindow, c_t.InteropTagGeneration, c_t.InteropTagFrameSequence, c_t.InteropTagClipboardEpoch, c_t.InteropTagSessionEpoch, c_t.InteropTagLeaseEpoch, c_t.InteropTagCapabilities, c_t.InteropTagTotalBytes, c_t.InteropTagOffset, c_t.InteropTagParent, c_t.InteropTagParentGeneration => &sample_values_t.One8,
        c_t.InteropTagRect => &sample_values_t.Rect,
        c_t.InteropTagPoint, c_t.InteropTagHotspot, c_t.InteropTagDelta => &sample_values_t.Point,
        c_t.InteropTagImageSize, c_t.InteropTagMinSize => &sample_values_t.Image,
        c_t.InteropTagDpi => &sample_values_t.Dpi,
        c_t.InteropTagOutput => &sample_values_t.Output,
        c_t.InteropTagContentHash => &sample_values_t.Hash,
        c_t.InteropTagUsbId => &sample_values_t.Usb,
        c_t.InteropTagStatus, c_t.InteropTagState, c_t.InteropTagAppFlags, c_t.InteropTagTouchId, c_t.InteropTagStream, c_t.InteropTagIntervalMs, c_t.InteropTagRate => &sample_values_t.Zero4,
        c_t.InteropTagButton => &[_]u8{ 1, 0 },
        c_t.InteropTagLaunchKind => &[_]u8{c_t.InteropLaunchPath},
        c_t.InteropTagKey, c_t.InteropTagIconSize, c_t.InteropTagCursorShape, c_t.InteropTagRefusedType, c_t.InteropTagPressure => &sample_values_t.One2,
        c_t.InteropTagRole, c_t.InteropTagPressed, c_t.InteropTagTool, c_t.InteropTagDropEffect, c_t.InteropTagDecision, c_t.InteropTagLocks => &sample_values_t.One1,
        c_t.InteropTagEdge => &[_]u8{1},
        c_t.InteropTagAppKey => "p-ab",
        c_t.InteropTagAgentVersion => "0.2.0",
        c_t.InteropTagLayout => "00000409",
        c_t.InteropTagMime => "text/plain",
        c_t.InteropTagMimeList => &sample_values_t.Mimes,
        c_t.InteropTagDisplayName, c_t.InteropTagPrinterName, c_t.InteropTagPrinterUri, c_t.InteropTagUriList => "x",
        c_t.InteropTagData => "d",
        c_t.InteropTagText => "",
        c_t.InteropTagTilt, c_t.InteropTagAxisV120, c_t.InteropTagAxisH120, c_t.InteropTagSlot, c_t.InteropTagProcessId => &sample_values_t.Zero4,
        c_t.InteropTagSerial, c_t.InteropTagTransfer, c_t.InteropTagLayoutVersion, c_t.InteropTagErrorCode => &sample_values_t.One4,
        else => unreachable,
    };
}

test "deterministic mutation fuzz never crashes and keeps outputs unchanged on error" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 99);
    var prng = std_t.Random.DefaultPrng.init(0x5741444C);
    const random = prng.random();
    var mutated: [256]u8 = undefined;
    var accepted: usize = 0;
    for (0..20000) |_| {
        @memcpy(mutated[0..length], buffer[0..length]);
        const flips = random.intRangeAtMost(usize, 1, 4);
        for (0..flips) |_| mutated[random.uintLessThan(usize, length)] ^= random.int(u8) | 1;
        const cut = if (random.boolean()) length else random.uintLessThan(usize, length + 1);
        var header = std_t.mem.zeroes(c_t.interop_header_t);
        if (interop_header_decode(&mutated, cut, &header) != error_codes_t.None) {
            try testing_t.expectEqual(@as(u32, 0), header.magic);
            continue;
        }
        if (header.length > cut - 16) continue;
        const payload = mutated[16 .. 16 + header.length];
        if (interop_payload_validate(header.type, c_t.InteropFromGuest, payload.ptr, payload.len) == error_codes_t.None) {
            accepted += 1;
            var pointer: ?[*]const u8 = null;
            var size: u16 = 0;
            _ = interop_field_get(payload.ptr, payload.len, c_t.InteropTagAgentVersion, 0, &pointer, &size);
        }
    }
    try testing_t.expect(accepted < 20000);
}

test "every registered tag reaches a value rule without hitting the unreachable default" {
    var zeros = std_t.mem.zeroes([c_t.InteropChunkBytes]u8);
    for (1..MaxTag + 1) |index| {
        const base: u16 = @intCast(index);
        const spec = spec_for(base) orelse continue;
        _ = value_valid(base, zeros[0..spec.max]);
        _ = value_valid(base, zeros[0..spec.min]);
    }
    try testing_t.expect(spec_for(0) == null and spec_for(MaxTag + 1) == null);
}

test "runtime inputs exercise defensive branches of every export" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    // Fields of widths 1, 2, 4 and 8 plus a 3-byte string read through field_uint.
    _ = interop_put_uint(&writer, c_t.InteropTagRole, 1);
    _ = interop_put_rect(&writer, c_t.InteropTagRect, 0, 0, 1, 1);
    _ = interop_put_bytes(&writer, c_t.InteropTagTitle, "abc", 3);
    _ = interop_put_uint(&writer, c_t.InteropTagState, 3);
    _ = interop_put_uint(&writer, c_t.InteropTagKey, 30);
    _ = interop_put_uint(&writer, c_t.InteropTagClipboardEpoch, 9);
    const payload = buffer[16..writer.length];
    const read_case_t = struct { tag: u16, width: u32, result: c_int, value: u64 };
    const Reads = [_]read_case_t{
        .{ .tag = c_t.InteropTagRole, .width = 1, .result = 0, .value = 1 },
        .{ .tag = c_t.InteropTagKey, .width = 2, .result = 0, .value = 30 },
        .{ .tag = c_t.InteropTagState, .width = 4, .result = 0, .value = 3 },
        .{ .tag = c_t.InteropTagClipboardEpoch, .width = 8, .result = 0, .value = 9 },
        .{ .tag = c_t.InteropTagTitle, .width = 3, .result = -1, .value = 0 },
        .{ .tag = c_t.InteropTagRect, .width = 16, .result = -1, .value = 0 },
        .{ .tag = c_t.InteropTagWindow, .width = 8, .result = 1, .value = 0 },
    };
    for (Reads) |read| {
        var value: u64 = 0;
        try testing_t.expectEqual(read.result, interop_field_uint(payload.ptr, opaque_value(usize, payload.len), opaque_value(u16, read.tag), opaque_value(u32, read.width), &value));
        try testing_t.expectEqual(read.value, value);
        var signed: i32 = 0;
        const expected: c_int = if (read.result == 1) 1 else if (read.width == 4) 0 else -1;
        try testing_t.expectEqual(expected, interop_field_i32(payload.ptr, opaque_value(usize, payload.len), opaque_value(u16, read.tag), &signed));
    }
    // field_get on malformed, short and NULL inputs.
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    const Bad = [_]u8{ 1, 0, 9, 0, 0, 0 };
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(&Bad, opaque_value(usize, 6), 1, 0, &pointer, &size));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(&Bad, opaque_value(usize, 2), 1, 0, &pointer, &size));
    try testing_t.expectEqual(@as(c_int, 1), interop_field_get(null, opaque_value(usize, 0), 1, 0, &pointer, &size));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(null, opaque_value(usize, 4), 1, 0, &pointer, &size));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(&Bad, opaque_value(usize, 65524), 1, 0, &pointer, &size));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(&Bad, 4, 1, 0, opaque_value(?*?[*]const u8, null), &size));
    try testing_t.expectEqual(@as(c_int, -1), interop_field_get(&Bad, 4, 1, 0, &pointer, opaque_value(?*u16, null)));
    // payload_validate on NULL, unaligned and oversize inputs.
    const Lengths = [_]usize{ 4, 6, 65524 };
    for (Lengths) |length| try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, opaque_value(?[*]const u8, null), opaque_value(usize, length)));
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(c_t.InteropMsgPing, c_t.InteropFromHost, &Bad, opaque_value(usize, 6)));
    // Writer: unknown tag, poisoned writer, width overflows, non-integer registry widths.
    const put_case_t = struct { tag: u16, value: u64 };
    const Puts = [_]put_case_t{
        .{ .tag = 0x7000, .value = 1 },
        .{ .tag = c_t.InteropTagKey, .value = 70000 },
        .{ .tag = c_t.InteropTagStatus, .value = 1 << 33 },
        .{ .tag = c_t.InteropTagRole, .value = 256 },
        .{ .tag = c_t.InteropTagRect, .value = 1 },
        .{ .tag = c_t.InteropTagUsbId, .value = 1 },
        .{ .tag = c_t.InteropTagContentHash, .value = 1 },
    };
    for (Puts) |item| {
        writer = writer_for(&buffer);
        try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, opaque_value(u16, item.tag), opaque_value(u64, item.value)));
        try testing_t.expectEqual(error_codes_t.Malformed, interop_put_uint(&writer, c_t.InteropTagWindow, 1)); // poisoned
        try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, c_t.InteropTagWindow, "12345678", 8));
    }
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, opaque_value(u16, 0x7000), "x", 1));
    writer = writer_for(&buffer);
    try testing_t.expectEqual(error_codes_t.Malformed, interop_put_bytes(&writer, opaque_value(u16, 0), "x", 1));
    // Transfers with runtime ids, sizes and digests.
    const begin_case_t = struct { id: u32, total: u64, cap: u64, result: c_int };
    const Begins = [_]begin_case_t{
        .{ .id = 0, .total = 1, .cap = 8, .result = error_codes_t.Malformed },
        .{ .id = 1, .total = 9, .cap = 8, .result = error_codes_t.Quota },
        .{ .id = 1, .total = c_t.InteropMaxTransferBytes + 1, .cap = std_t.math.maxInt(u64), .result = error_codes_t.Quota },
        .{ .id = 5, .total = 1, .cap = 8, .result = error_codes_t.None },
    };
    var transfer = std_t.mem.zeroes(c_t.interop_transfer_t);
    for (Begins) |begin| try testing_t.expectEqual(begin.result, interop_transfer_begin(&transfer, opaque_value(u32, begin.id), opaque_value(u64, begin.total), opaque_value(u64, begin.cap)));
    try testing_t.expectEqual(error_codes_t.Protocol, interop_transfer_end(&transfer, opaque_value(?[*]const u8, null)));
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_chunk(&transfer, opaque_value(u64, 0), "z", opaque_value(usize, 1)));
    var digest: [32]u8 = undefined;
    interop_sha256(opaque_value(?[*]const u8, "z"), opaque_value(usize, 1), &digest);
    try testing_t.expectEqual(error_codes_t.None, interop_transfer_end(&transfer, &digest));
    interop_sha256(opaque_value(?[*]const u8, null), opaque_value(usize, 0), &digest);
    // Value rules reached with runtime data: app keys, damage, outputs, printer names, mimes.
    const value_case_t = struct { tag: u16, bytes: []const u8, ok: bool };
    const OutputOk = [_]u8{0} ** 4 ++ [_]u8{ 0, 15, 0, 0, 0, 8, 0, 0, 120, 0, 0, 0 };
    const OutputTall = [_]u8{0} ** 4 ++ [_]u8{ 0, 15, 0, 0, 0, 0, 0, 0, 120, 0, 0, 0 };
    const OutputWide = [_]u8{0} ** 4 ++ [_]u8{ 0, 0, 0, 0, 0, 8, 0, 0, 120, 0, 0, 0 };
    const DamageTwo = [_]u8{0} ** 8 ++ [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0 } ++ [_]u8{0} ** 8 ++ [_]u8{ 1, 0, 0, 0, 1, 0x80, 0, 0 };
    const Values = [_]value_case_t{
        .{ .tag = c_t.InteropTagAppKey, .bytes = "a-x", .ok = true },
        .{ .tag = c_t.InteropTagAppKey, .bytes = "ab-", .ok = false },
        .{ .tag = c_t.InteropTagAppKey, .bytes = "p_x", .ok = false },
        .{ .tag = c_t.InteropTagAppKey, .bytes = "q-x", .ok = false },
        .{ .tag = c_t.InteropTagAppKey, .bytes = "a-ÿ", .ok = false },
        .{ .tag = c_t.InteropTagDamage, .bytes = &DamageTwo, .ok = false },
        .{ .tag = c_t.InteropTagDamage, .bytes = DamageTwo[0..16], .ok = true },
        .{ .tag = c_t.InteropTagOutput, .bytes = &OutputOk, .ok = true },
        .{ .tag = c_t.InteropTagOutput, .bytes = &OutputTall, .ok = false },
        .{ .tag = c_t.InteropTagOutput, .bytes = &OutputWide, .ok = false },
        .{ .tag = c_t.InteropTagPrinterName, .bytes = "a b", .ok = false },
        .{ .tag = c_t.InteropTagPrinterName, .bytes = "a#b", .ok = false },
        .{ .tag = c_t.InteropTagMime, .bytes = "", .ok = false },
        .{ .tag = c_t.InteropTagEdge, .bytes = &.{2}, .ok = true },
        .{ .tag = c_t.InteropTagEdge, .bytes = &.{7}, .ok = false },
    };
    for (Values) |item| try testing_t.expectEqual(item.ok, value_valid(opaque_value(u16, item.tag), opaque_value([]const u8, item.bytes)));
    // Cross rules with runtime payloads: icon request with generation but no window.
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c_t.InteropTagGeneration, 2);
    _ = interop_put_bytes(&writer, c_t.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c_t.InteropTagTransfer, 1);
    _ = interop_put_uint(&writer, c_t.InteropTagIconSize, 32);
    const icon = buffer[16..writer.length];
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(opaque_value(u16, c_t.InteropMsgIconRequest), c_t.InteropFromHost, icon.ptr, opaque_value(usize, icon.len)));
    try testing_t.expectEqual(error_codes_t.Malformed, interop_payload_validate(opaque_value(u16, c_t.InteropMsgWindowRetire), c_t.InteropFromGuest, icon.ptr, opaque_value(usize, icon.len)));
}
