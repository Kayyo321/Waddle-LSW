//! Interop protocol v1 codec (impl/seamless-interop/IMPL_DESC.md section 3.2).
//!
//! Untrusted frames from either peer are validated here before any field is
//! interpreted. Every exported function is allocation-free, never retains a
//! caller pointer beyond the call (except the writer's borrowed buffer, which the
//! caller owns), and leaves outputs unchanged on failure.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

const Sha256 = std.crypto.hash.sha2.Sha256;

const Error = struct {
    const none: c_int = c.InteropErrorNone;
    const protocol: c_int = c.InteropErrorProtocolViolation;
    const malformed: c_int = c.InteropErrorMalformedFrame;
    const unsupported_field: c_int = c.InteropErrorUnsupportedField;
    const unsupported_type: c_int = c.InteropErrorUnsupportedType;
    const quota: c_int = c.InteropErrorQuotaExceeded;
};

fn get(comptime T: type, data: []const u8, offset: usize) T {
    return std.mem.readInt(T, data[offset..][0..@sizeOf(T)], .little);
}
fn put(comptime T: type, data: []u8, offset: usize, value: T) void {
    std.mem.writeInt(T, data[offset..][0..@sizeOf(T)], value, .little);
}

// ---------------------------------------------------------------------------
// Field registry
// ---------------------------------------------------------------------------

/// Registry entry: criticality and inclusive value length bounds.
const FieldSpec = struct { critical: bool, min: u16, max: u16 };

const MaxTag = 0x4A;

/// Indexed by base tag; null entries are unknown tags.
const registry = blk: {
    var table = [_]?FieldSpec{null} ** (MaxTag + 1);
    const fixed = struct {
        fn f(critical: bool, size: u16) FieldSpec {
            return .{ .critical = critical, .min = size, .max = size };
        }
        fn v(critical: bool, min: u16, max: u16) FieldSpec {
            return .{ .critical = critical, .min = min, .max = max };
        }
    };
    table[c.InteropTagWindow] = fixed.f(true, 8);
    table[c.InteropTagGeneration] = fixed.f(true, 8);
    table[c.InteropTagParent] = fixed.f(true, 8);
    table[c.InteropTagParentGeneration] = fixed.f(true, 8);
    table[c.InteropTagRole] = fixed.f(true, 1);
    table[c.InteropTagRect] = fixed.f(true, 16);
    table[c.InteropTagTitle] = fixed.v(false, 0, c.InteropMaxTitleBytes);
    table[c.InteropTagAppKey] = fixed.v(true, 3, c.InteropMaxAppKeyBytes);
    table[c.InteropTagProcessId] = fixed.f(false, 4);
    table[c.InteropTagState] = fixed.f(true, 4);
    table[c.InteropTagDpi] = fixed.f(true, 4);
    table[c.InteropTagSlot] = fixed.f(true, 4);
    table[c.InteropTagFrameSequence] = fixed.f(true, 8);
    table[c.InteropTagDamage] = fixed.v(false, 0, 256);
    table[c.InteropTagEdge] = fixed.f(true, 1);
    table[c.InteropTagSerial] = fixed.f(true, 4);
    table[c.InteropTagPoint] = fixed.f(true, 8);
    table[c.InteropTagButton] = fixed.f(true, 2);
    table[c.InteropTagPressed] = fixed.f(true, 1);
    table[c.InteropTagKey] = fixed.f(true, 2);
    table[c.InteropTagAxisV120] = fixed.f(true, 4);
    table[c.InteropTagAxisH120] = fixed.f(true, 4);
    table[c.InteropTagDelta] = fixed.f(true, 8);
    table[c.InteropTagTouchId] = fixed.f(true, 4);
    table[c.InteropTagPressure] = fixed.f(true, 2);
    table[c.InteropTagTilt] = fixed.f(true, 4);
    table[c.InteropTagRotation] = fixed.f(false, 2);
    table[c.InteropTagTool] = fixed.f(true, 1);
    table[c.InteropTagText] = fixed.v(true, 0, c.InteropMaxStringBytes);
    table[c.InteropTagCursorBegin] = fixed.f(false, 4);
    table[c.InteropTagCursorEnd] = fixed.f(false, 4);
    table[c.InteropTagLayout] = fixed.f(true, 8);
    table[c.InteropTagLocks] = fixed.f(true, 1);
    table[c.InteropTagCursorShape] = fixed.f(true, 2);
    table[c.InteropTagHotspot] = fixed.f(true, 8);
    table[c.InteropTagImageSize] = fixed.f(true, 8);
    table[c.InteropTagContentHash] = fixed.f(true, c.InteropHashBytes);
    table[c.InteropTagTransfer] = fixed.f(true, 4);
    table[c.InteropTagTotalBytes] = fixed.f(true, 8);
    table[c.InteropTagOffset] = fixed.f(true, 8);
    table[c.InteropTagData] = fixed.v(true, 1, c.InteropChunkBytes);
    table[c.InteropTagMime] = fixed.v(true, 1, 255);
    table[c.InteropTagMimeList] = fixed.v(true, 2, 32 * 256);
    table[c.InteropTagClipboardEpoch] = fixed.f(true, 8);
    table[c.InteropTagDropEffect] = fixed.f(true, 1);
    table[c.InteropTagDisplayName] = fixed.v(true, 1, 255);
    table[c.InteropTagTargetPath] = fixed.v(false, 1, c.InteropMaxStringBytes);
    table[c.InteropTagArguments] = fixed.v(false, 0, c.InteropMaxStringBytes);
    table[c.InteropTagWorkingDir] = fixed.v(false, 1, c.InteropMaxStringBytes);
    table[c.InteropTagAumid] = fixed.v(false, 1, 512);
    table[c.InteropTagAppFlags] = fixed.f(true, 4);
    table[c.InteropTagIconSizes] = fixed.v(false, 2, 14);
    table[c.InteropTagIconSize] = fixed.f(true, 2);
    table[c.InteropTagLaunchKind] = fixed.f(true, 1);
    table[c.InteropTagActivationToken] = fixed.v(false, 1, 255);
    table[c.InteropTagStatus] = fixed.f(true, 4);
    table[c.InteropTagMessage] = fixed.v(false, 0, 1024);
    table[c.InteropTagCapabilities] = fixed.f(true, 8);
    table[c.InteropTagAgentVersion] = fixed.v(true, 5, 32);
    table[c.InteropTagLayoutVersion] = fixed.f(true, 4);
    table[c.InteropTagSessionEpoch] = fixed.f(true, 8);
    table[c.InteropTagOutput] = fixed.f(true, 16);
    table[c.InteropTagStream] = fixed.f(true, 4);
    table[c.InteropTagPrinterName] = fixed.v(true, 1, 127);
    table[c.InteropTagPrinterUri] = fixed.v(true, 1, 255);
    table[c.InteropTagPublisher] = fixed.v(false, 0, 255);
    table[c.InteropTagDecision] = fixed.f(true, 1);
    table[c.InteropTagUsbId] = fixed.f(true, 6);
    table[c.InteropTagLeaseEpoch] = fixed.f(true, 8);
    table[c.InteropTagErrorCode] = fixed.f(true, 4);
    table[c.InteropTagRefusedType] = fixed.f(false, 2);
    table[c.InteropTagUriList] = fixed.v(true, 1, 60000);
    table[c.InteropTagIntervalMs] = fixed.f(true, 4);
    table[c.InteropTagRate] = fixed.f(true, 4);
    break :blk table;
};

fn spec_for(base: u16) ?FieldSpec {
    if (base == 0 or base > MaxTag) return null;
    return registry[base];
}

fn utf8_text(value: []const u8) bool {
    return std.unicode.utf8ValidateSlice(value) and std.mem.indexOfScalar(u8, value, 0) == null;
}
fn printable(value: []const u8) bool {
    for (value) |byte| if (byte < '!' or byte > '~') return false;
    return true;
}
fn in_range(comptime T: type, value: T, low: T, high: T) bool {
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
fn printer_name_valid(value: []const u8) bool {
    if (!printable(value)) return false;
    return std.mem.indexOfAny(u8, value, "/#") == null;
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
    if (base <= c.InteropTagButton) return window_value_valid(base, value);
    if (base <= c.InteropTagContentHash) return input_value_valid(base, value);
    if (base <= c.InteropTagMessage) return data_value_valid(base, value);
    return session_value_valid(base, value);
}

/// Window identity, geometry and pointer fields (tags 0x01..0x12).
fn window_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c.InteropTagWindow, c.InteropTagGeneration, c.InteropTagFrameSequence => get(u64, value, 0) != 0,
        c.InteropTagParent, c.InteropTagParentGeneration, c.InteropTagProcessId => true,
        c.InteropTagRole => in_range(u8, value[0], 1, 6),
        c.InteropTagRect => rect_valid(value),
        c.InteropTagTitle => utf8_text(value),
        c.InteropTagAppKey => app_key_valid(value),
        c.InteropTagState => get(u32, value, 0) <= c.InteropStateMask,
        c.InteropTagDpi => in_range(u32, get(u32, value, 0), 96, 960),
        c.InteropTagSlot => get(u32, value, 0) < c.InteropMaxWindows * 3,
        c.InteropTagDamage => damage_valid(value),
        c.InteropTagEdge => switch (value[0]) {
            1, 2, 4, 5, 6, 8, 9, 10 => true,
            else => false,
        },
        c.InteropTagSerial => get(u32, value, 0) != 0,
        c.InteropTagPoint => point_valid(value, -16384, 16384),
        c.InteropTagButton => in_range(u16, get(u16, value, 0), 1, 5),
        else => unreachable, // spec_for admits only registered tags
    };
}

/// Keyboard, touch, tablet, text and cursor fields (tags 0x13..0x25).
fn input_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c.InteropTagPressed => value[0] <= 1,
        c.InteropTagKey => in_range(u16, get(u16, value, 0), 1, 767),
        c.InteropTagAxisV120, c.InteropTagAxisH120 => in_range(i32, get(i32, value, 0), -15360, 15360),
        c.InteropTagDelta => point_valid(value, -(16384 << 8), 16384 << 8),
        c.InteropTagTouchId => get(u32, value, 0) <= 9,
        c.InteropTagPressure => get(u16, value, 0) <= 1024,
        c.InteropTagTilt => in_range(i16, get(i16, value, 0), -9000, 9000) and
            in_range(i16, get(i16, value, 2), -9000, 9000),
        c.InteropTagRotation => get(u16, value, 0) <= 35999,
        c.InteropTagTool => in_range(u8, value[0], 1, 2),
        c.InteropTagText => utf8_text(value),
        c.InteropTagCursorBegin, c.InteropTagCursorEnd => in_range(i32, get(i32, value, 0), -1, c.InteropMaxStringBytes),
        c.InteropTagLayout => hex_valid(value),
        c.InteropTagLocks => value[0] <= 7,
        c.InteropTagCursorShape => cursor_shape_valid(get(u16, value, 0)),
        c.InteropTagHotspot => point_valid(value, 0, 512),
        c.InteropTagImageSize => in_range(u32, get(u32, value, 0), 1, 512) and in_range(u32, get(u32, value, 4), 1, 512),
        c.InteropTagContentHash => true,
        else => unreachable, // spec_for admits only registered tags
    };
}

/// Transfer, clipboard, catalog and launch fields (tags 0x26..0x39).
fn data_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c.InteropTagTransfer => get(u32, value, 0) != 0,
        c.InteropTagTotalBytes => get(u64, value, 0) <= c.InteropMaxTransferBytes,
        c.InteropTagOffset => get(u64, value, 0) < c.InteropMaxTransferBytes,
        c.InteropTagData => true,
        c.InteropTagMime => mime_valid(value),
        c.InteropTagMimeList => mime_list_valid(value),
        c.InteropTagClipboardEpoch => get(u64, value, 0) != 0,
        c.InteropTagDropEffect => value[0] <= 7,
        c.InteropTagDisplayName, c.InteropTagTargetPath, c.InteropTagArguments, c.InteropTagWorkingDir,
        c.InteropTagAumid, c.InteropTagMessage,
        => utf8_text(value),
        c.InteropTagAppFlags => get(u32, value, 0) <= c.InteropAppFlagMask,
        c.InteropTagIconSizes => icon_sizes_valid(value),
        c.InteropTagIconSize => icon_size_valid(get(u16, value, 0)),
        c.InteropTagLaunchKind => in_range(u8, value[0], 1, 5),
        c.InteropTagActivationToken => printable(value),
        c.InteropTagStatus => get(u32, value, 0) <= c.InteropGuestError,
        else => unreachable, // spec_for admits only registered tags
    };
}

/// Session, display, audio, printer, USB and error fields (tags 0x3A..0x4A).
fn session_value_valid(base: u16, value: []const u8) bool {
    return switch (base) {
        c.InteropTagCapabilities => get(u64, value, 0) <= c.InteropCapabilityMask,
        c.InteropTagAgentVersion => version_valid(value),
        c.InteropTagLayoutVersion => get(u32, value, 0) != 0,
        c.InteropTagSessionEpoch, c.InteropTagLeaseEpoch => get(u64, value, 0) != 0,
        c.InteropTagOutput => in_range(u32, get(u32, value, 4), 1, 16384) and
            in_range(u32, get(u32, value, 8), 1, 16384) and in_range(u32, get(u32, value, 12), 120, 480),
        c.InteropTagStream => get(u32, value, 0) < c.InteropMaxAudioStreams,
        c.InteropTagPrinterName => printer_name_valid(value),
        c.InteropTagPrinterUri => printable(value),
        c.InteropTagPublisher, c.InteropTagUriList => utf8_text(value),
        c.InteropTagDecision => value[0] <= 1,
        c.InteropTagUsbId, c.InteropTagRefusedType => true,
        c.InteropTagErrorCode => in_range(u32, get(u32, value, 0), 1, c.InteropErrorUnexpected),
        c.InteropTagIntervalMs => get(u32, value, 0) <= 10000,
        c.InteropTagRate => get(u32, value, 0) <= 1000,
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
    for (value) |byte| if (!std.ascii.isHex(byte)) return false;
    return true;
}
fn cursor_shape_valid(shape: u16) bool {
    return shape <= c.InteropCursorLast or shape == c.InteropCursorCustom;
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

const MessageSpec = struct {
    type: u16,
    directions: u8,
    required: []const u16,
    optional: []const u16 = &.{},
};

const Tags = struct {
    const window = c.InteropTagWindow;
    const generation = c.InteropTagGeneration;
    const lease = c.InteropTagLeaseEpoch;
    const serial = c.InteropTagSerial;
};
const input_head = [_]u16{ Tags.window, Tags.generation, Tags.serial, Tags.lease };
fn with(comptime base: []const u16, comptime extra: []const u16) []const u16 {
    return base ++ extra;
}
const launch_fields = [_]u16{ c.InteropTagTargetPath, c.InteropTagArguments, c.InteropTagWorkingDir, c.InteropTagAppKey, c.InteropTagActivationToken };
const catalog_optional = [_]u16{ c.InteropTagTargetPath, c.InteropTagArguments, c.InteropTagWorkingDir, c.InteropTagAumid, c.InteropTagIconSizes, c.InteropTagContentHash };

const messages = [_]MessageSpec{
    .{ .type = c.InteropMsgHello, .directions = DirGuest, .required = &.{ c.InteropTagAgentVersion, c.InteropTagCapabilities, c.InteropTagLayoutVersion, c.InteropTagSessionEpoch }, .optional = &.{c.InteropTagMessage} },
    .{ .type = c.InteropMsgHelloAck, .directions = DirHost, .required = &.{ c.InteropTagAgentVersion, c.InteropTagCapabilities, c.InteropTagLayoutVersion } },
    .{ .type = c.InteropMsgGoodbye, .directions = DirBoth, .required = &.{c.InteropTagStatus}, .optional = &.{c.InteropTagMessage} },
    .{ .type = c.InteropMsgPing, .directions = DirHost, .required = &.{} },
    .{ .type = c.InteropMsgPong, .directions = DirGuest, .required = &.{} },
    .{ .type = c.InteropMsgError, .directions = DirBoth, .required = &.{c.InteropTagErrorCode}, .optional = &.{ c.InteropTagRefusedType, c.InteropTagMessage } },
    .{ .type = c.InteropMsgWindowAnnounce, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagRole, c.InteropTagRect, c.InteropTagAppKey, c.InteropTagState, c.InteropTagDpi }, .optional = &.{ c.InteropTagTitle, c.InteropTagParent, c.InteropTagParentGeneration, c.InteropTagProcessId } },
    .{ .type = c.InteropMsgWindowUpdate, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagRect, c.InteropTagState, c.InteropTagDpi }, .optional = &.{ c.InteropTagTitle, c.InteropTagParent, c.InteropTagParentGeneration } },
    .{ .type = c.InteropMsgWindowRetire, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation } },
    .{ .type = c.InteropMsgWindowConfigure, .directions = DirHost, .required = &.{ Tags.window, Tags.generation, c.InteropTagState }, .optional = &.{c.InteropTagRect} },
    .{ .type = c.InteropMsgWindowClose, .directions = DirHost, .required = &.{ Tags.window, Tags.generation } },
    .{ .type = c.InteropMsgWindowMoveRequest, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, Tags.serial } },
    .{ .type = c.InteropMsgWindowResizeRequest, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagEdge, Tags.serial } },
    .{ .type = c.InteropMsgWindowStateRequest, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagState } },
    .{ .type = c.InteropMsgFrameReady, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagSlot, c.InteropTagFrameSequence }, .optional = &.{c.InteropTagDamage} },
    .{ .type = c.InteropMsgFrameRelease, .directions = DirHost, .required = &.{ Tags.window, Tags.generation, c.InteropTagSlot, c.InteropTagFrameSequence } },
    .{ .type = c.InteropMsgWindowIconChanged, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagContentHash } },
    .{ .type = c.InteropMsgPopupRepositioned, .directions = DirHost, .required = &.{ Tags.window, Tags.generation, c.InteropTagRect } },
    .{ .type = c.InteropMsgPopupDismissed, .directions = DirHost, .required = &.{ Tags.window, Tags.generation } },
    .{ .type = c.InteropMsgInputFocus, .directions = DirHost, .required = with(&input_head, &.{c.InteropTagPressed}) },
    .{ .type = c.InteropMsgInputKey, .directions = DirHost, .required = with(&input_head, &.{ c.InteropTagPressed, c.InteropTagKey }) },
    .{ .type = c.InteropMsgInputPointerMotion, .directions = DirHost, .required = with(&input_head, &.{c.InteropTagPoint}) },
    .{ .type = c.InteropMsgInputPointerRelative, .directions = DirHost, .required = with(&input_head, &.{c.InteropTagDelta}) },
    .{ .type = c.InteropMsgInputPointerButton, .directions = DirHost, .required = with(&input_head, &.{ c.InteropTagPoint, c.InteropTagButton, c.InteropTagPressed }) },
    .{ .type = c.InteropMsgInputPointerAxis, .directions = DirHost, .required = with(&input_head, &.{ c.InteropTagAxisV120, c.InteropTagAxisH120 }) },
    .{ .type = c.InteropMsgInputTouch, .directions = DirHost, .required = with(&input_head, &.{ c.InteropTagPoint, c.InteropTagPressed, c.InteropTagTouchId }) },
    .{ .type = c.InteropMsgInputTouchFrame, .directions = DirHost, .required = &input_head },
    .{ .type = c.InteropMsgInputTouchCancel, .directions = DirHost, .required = &input_head },
    .{ .type = c.InteropMsgInputTablet, .directions = DirHost, .required = with(&input_head, &.{ c.InteropTagPoint, c.InteropTagPressed, c.InteropTagPressure, c.InteropTagTilt, c.InteropTagTool }), .optional = &.{ c.InteropTagButton, c.InteropTagRotation } },
    .{ .type = c.InteropMsgInputTabletProximity, .directions = DirHost, .required = with(&input_head, &.{ c.InteropTagPressed, c.InteropTagTool }) },
    .{ .type = c.InteropMsgInputGesturePinch, .directions = DirHost, .required = with(&input_head, &.{ c.InteropTagPoint, c.InteropTagAxisV120 }) },
    .{ .type = c.InteropMsgInputTextCommit, .directions = DirHost, .required = with(&input_head, &.{c.InteropTagText}) },
    .{ .type = c.InteropMsgInputTextPreedit, .directions = DirHost, .required = with(&input_head, &.{c.InteropTagText}), .optional = &.{ c.InteropTagCursorBegin, c.InteropTagCursorEnd } },
    .{ .type = c.InteropMsgInputRelease, .directions = DirHost, .required = &input_head },
    .{ .type = c.InteropMsgInputLockState, .directions = DirHost, .required = &.{ Tags.serial, c.InteropTagLocks, Tags.lease } },
    .{ .type = c.InteropMsgInputRepeatInfo, .directions = DirHost, .required = &.{ c.InteropTagIntervalMs, c.InteropTagRate } },
    .{ .type = c.InteropMsgKeyboardLayout, .directions = DirHost, .required = &.{c.InteropTagLayout} },
    .{ .type = c.InteropMsgCaretRect, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagRect } },
    .{ .type = c.InteropMsgLeaseRevoked, .directions = DirGuest, .required = &.{Tags.lease} },
    .{ .type = c.InteropMsgLeaseAck, .directions = DirHost, .required = &.{ Tags.serial, Tags.lease } },
    .{ .type = c.InteropMsgLeaseReady, .directions = DirGuest, .required = &.{ Tags.serial, Tags.lease } },
    .{ .type = c.InteropMsgCursorShape, .directions = DirGuest, .required = &.{c.InteropTagCursorShape}, .optional = &.{ c.InteropTagHotspot, c.InteropTagContentHash } },
    .{ .type = c.InteropMsgCursorConstraint, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagPressed } },
    .{ .type = c.InteropMsgClipboardOffer, .directions = DirBoth, .required = &.{ c.InteropTagMimeList, c.InteropTagClipboardEpoch }, .optional = &.{c.InteropTagDropEffect} },
    .{ .type = c.InteropMsgClipboardRequest, .directions = DirBoth, .required = &.{ c.InteropTagTransfer, c.InteropTagMime, c.InteropTagClipboardEpoch } },
    .{ .type = c.InteropMsgClipboardClear, .directions = DirBoth, .required = &.{c.InteropTagClipboardEpoch} },
    .{ .type = c.InteropMsgDndEnter, .directions = DirHost, .required = &.{ Tags.window, Tags.generation, c.InteropTagPoint, c.InteropTagMimeList, c.InteropTagDropEffect } },
    .{ .type = c.InteropMsgDndMotion, .directions = DirHost, .required = &.{ Tags.window, Tags.generation, c.InteropTagPoint } },
    .{ .type = c.InteropMsgDndLeave, .directions = DirHost, .required = &.{ Tags.window, Tags.generation } },
    .{ .type = c.InteropMsgDndDrop, .directions = DirHost, .required = &.{ Tags.window, Tags.generation, c.InteropTagPoint, c.InteropTagTransfer, c.InteropTagMime } },
    .{ .type = c.InteropMsgDndResult, .directions = DirGuest, .required = &.{ c.InteropTagDropEffect, c.InteropTagStatus } },
    .{ .type = c.InteropMsgDndSourceStart, .directions = DirGuest, .required = &.{ Tags.window, Tags.generation, c.InteropTagMimeList, c.InteropTagDropEffect } },
    .{ .type = c.InteropMsgDndSourceRequest, .directions = DirHost, .required = &.{ c.InteropTagTransfer, c.InteropTagMime } },
    .{ .type = c.InteropMsgDndSourceFinished, .directions = DirHost, .required = &.{ c.InteropTagDropEffect, c.InteropTagStatus } },
    .{ .type = c.InteropMsgCatalogBegin, .directions = DirGuest, .required = &.{} },
    .{ .type = c.InteropMsgCatalogAdd, .directions = DirGuest, .required = &.{ c.InteropTagAppKey, c.InteropTagDisplayName, c.InteropTagAppFlags }, .optional = &catalog_optional },
    .{ .type = c.InteropMsgCatalogUpdate, .directions = DirGuest, .required = &.{ c.InteropTagAppKey, c.InteropTagDisplayName, c.InteropTagAppFlags }, .optional = &catalog_optional },
    .{ .type = c.InteropMsgCatalogRemove, .directions = DirGuest, .required = &.{c.InteropTagAppKey} },
    .{ .type = c.InteropMsgCatalogEnd, .directions = DirGuest, .required = &.{} },
    .{ .type = c.InteropMsgIconRequest, .directions = DirHost, .required = &.{ c.InteropTagAppKey, c.InteropTagTransfer, c.InteropTagIconSize }, .optional = &.{ Tags.window, Tags.generation } },
    .{ .type = c.InteropMsgLaunchRequest, .directions = DirHost, .required = &.{c.InteropTagLaunchKind}, .optional = &launch_fields },
    .{ .type = c.InteropMsgLaunchResult, .directions = DirGuest, .required = &.{ c.InteropTagProcessId, c.InteropTagStatus }, .optional = &.{ c.InteropTagActivationToken, c.InteropTagMessage } },
    .{ .type = c.InteropMsgElevationNotice, .directions = DirGuest, .required = &.{ c.InteropTagProcessId, c.InteropTagDisplayName }, .optional = &.{ c.InteropTagTargetPath, c.InteropTagPublisher } },
    .{ .type = c.InteropMsgUacPrompt, .directions = DirGuest, .required = &.{ c.InteropTagTransfer, c.InteropTagDisplayName }, .optional = &.{ c.InteropTagTargetPath, c.InteropTagPublisher } },
    .{ .type = c.InteropMsgUacDecision, .directions = DirHost, .required = &.{ c.InteropTagTransfer, c.InteropTagDecision } },
    .{ .type = c.InteropMsgDesktopSwitch, .directions = DirGuest, .required = &.{c.InteropTagPressed} },
    .{ .type = c.InteropMsgAudioStreamOpen, .directions = DirGuest, .required = &.{ c.InteropTagAppKey, c.InteropTagProcessId, c.InteropTagDisplayName, c.InteropTagStream } },
    .{ .type = c.InteropMsgAudioStreamClose, .directions = DirGuest, .required = &.{c.InteropTagStream} },
    .{ .type = c.InteropMsgPrinterSync, .directions = DirHost, .required = &.{ c.InteropTagPressed, c.InteropTagPrinterName, c.InteropTagPrinterUri }, .optional = &.{c.InteropTagDisplayName} },
    .{ .type = c.InteropMsgPrinterResult, .directions = DirGuest, .required = &.{ c.InteropTagStatus, c.InteropTagPrinterName }, .optional = &.{c.InteropTagMessage} },
    .{ .type = c.InteropMsgDisplayConfig, .directions = DirHost, .required = &.{c.InteropTagOutput} },
    .{ .type = c.InteropMsgDisplayApplied, .directions = DirGuest, .required = &.{ c.InteropTagRect, c.InteropTagStatus }, .optional = &.{c.InteropTagMessage} },
    .{ .type = c.InteropMsgTransferBegin, .directions = DirBoth, .required = &.{ c.InteropTagTransfer, c.InteropTagTotalBytes, c.InteropTagMime }, .optional = &.{c.InteropTagContentHash} },
    .{ .type = c.InteropMsgTransferChunk, .directions = DirBoth, .required = &.{ c.InteropTagTransfer, c.InteropTagOffset, c.InteropTagData } },
    .{ .type = c.InteropMsgTransferEnd, .directions = DirBoth, .required = &.{ c.InteropTagContentHash, c.InteropTagTransfer } },
    .{ .type = c.InteropMsgTransferCancel, .directions = DirBoth, .required = &.{ c.InteropTagTransfer, c.InteropTagStatus } },
    .{ .type = c.InteropMsgLocalLaunch, .directions = DirLocal, .required = &.{c.InteropTagLaunchKind}, .optional = &launch_fields },
};

const TagSet = u128;
fn tag_bit(base: u16) TagSet {
    return @as(TagSet, 1) << @as(u7, @intCast(base));
}
fn set_of(tags: []const u16) TagSet {
    var result: TagSet = 0;
    for (tags) |tag| result |= tag_bit(tag);
    return result;
}

/// Precomputed required/allowed sets, one per message table entry.
const message_sets = blk: {
    var sets: [messages.len]struct { required: TagSet, allowed: TagSet } = undefined;
    for (messages, 0..) |message, index| {
        const required = set_of(message.required);
        sets[index] = .{ .required = required, .allowed = required | set_of(message.optional) };
    }
    break :blk sets;
};

fn message_index(kind: u16) ?usize {
    for (messages, 0..) |message, index| if (message.type == kind) return index;
    return null;
}

fn direction_bit(direction: c.interop_direction_t) u8 {
    return switch (direction) {
        c.InteropFromGuest => DirGuest,
        c.InteropFromHost => DirHost,
        c.InteropFromLocal => DirLocal,
        else => 0,
    };
}

// ---------------------------------------------------------------------------
// Payload walking
// ---------------------------------------------------------------------------

const Field = struct { tag: u16, value: []const u8 };

/// Iterates TLV fields, checking structure and padding only.
const Walker = struct {
    data: []const u8,
    offset: usize = 0,

    /// Returns the next field, null at the end, or error.Malformed.
    fn next(self: *Walker) error{Malformed}!?Field {
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
    var walker = Walker{ .data = payload };
    while (walker.next() catch return null) |field| {
        if (field.tag & 0x7FFF == base) return field.value;
    }
    return null;
}

/// Cross-field rules that the per-field table cannot express.
fn cross_valid(kind: u16, payload: []const u8) bool {
    if (kind == c.InteropMsgLaunchRequest or kind == c.InteropMsgLocalLaunch) {
        // Required-field checking ran first, so launch_kind is present.
        const launch = optional_field(payload, c.InteropTagLaunchKind).?;
        const has_app = optional_field(payload, c.InteropTagAppKey) != null;
        const has_path = optional_field(payload, c.InteropTagTargetPath) != null;
        return if (launch[0] == c.InteropLaunchApp) has_app and !has_path else has_path and !has_app;
    }
    if (kind == c.InteropMsgWindowAnnounce or kind == c.InteropMsgWindowUpdate) {
        const has_parent = optional_field(payload, c.InteropTagParent) != null;
        const has_generation = optional_field(payload, c.InteropTagParentGeneration) != null;
        if (has_parent != has_generation) return false;
    }
    if (kind == c.InteropMsgIconRequest) {
        const has_window = optional_field(payload, c.InteropTagWindow) != null;
        if (has_window != (optional_field(payload, c.InteropTagGeneration) != null)) return false;
    }
    return true;
}

fn validate(kind: u16, direction: c.interop_direction_t, payload: []const u8) c_int {
    if (payload.len > c.InteropMaxPayloadBytes or payload.len % 4 != 0) return Error.malformed;
    const index = message_index(kind) orelse return Error.unsupported_type;
    const bit = direction_bit(direction);
    if (bit == 0 or messages[index].directions & bit == 0) return Error.unsupported_type;
    const sets = message_sets[index];
    var walker = Walker{ .data = payload };
    var seen: TagSet = 0;
    var last: u16 = 0;
    var fields: usize = 0;
    var outputs: usize = 0;
    while (walker.next() catch return Error.malformed) |field| {
        fields += 1;
        if (fields > c.InteropMaxFields) return Error.malformed;
        const base = field.tag & 0x7FFF;
        const critical = field.tag & c.InteropCriticalTag != 0;
        if (base == 0) return Error.malformed;
        const repeat = base == last and base == c.InteropTagOutput and kind == c.InteropMsgDisplayConfig;
        if (base <= last and !repeat) return Error.malformed;
        last = base;
        const spec = spec_for(base) orelse {
            if (critical) return Error.unsupported_field;
            continue;
        };
        if (spec.critical != critical) return Error.malformed;
        if (field.value.len < spec.min or field.value.len > spec.max) return Error.malformed;
        if (!value_valid(base, field.value)) return Error.malformed;
        if (sets.allowed & tag_bit(base) == 0) return Error.malformed;
        if (base == c.InteropTagOutput) {
            outputs += 1;
            if (outputs > 8) return Error.malformed;
        }
        seen |= tag_bit(base);
    }
    if (seen & sets.required != sets.required) return Error.malformed;
    if (!cross_valid(kind, payload)) return Error.malformed;
    return Error.none;
}

// ---------------------------------------------------------------------------
// Exported C ABI
// ---------------------------------------------------------------------------

/// in: borrowed untrusted bytes[length]; out: header unchanged on failure.
export fn interop_header_decode(bytes: ?[*]const u8, length: usize, header: *c.interop_header_t) c_int {
    if (length < c.InteropHeaderBytes) return Error.malformed;
    const data = (bytes orelse return Error.malformed)[0..c.InteropHeaderBytes];
    const magic = get(u32, data, 0);
    const version = get(u16, data, 4);
    const size = get(u32, data, 8);
    if (magic != c.InteropMagic or version != c.InteropVersion or size > c.InteropMaxPayloadBytes or size % 4 != 0)
        return Error.protocol;
    header.* = .{ .magic = magic, .version = version, .type = get(u16, data, 6), .length = size, .request_id = get(u32, data, 12) };
    return Error.none;
}

/// in: type/direction and borrowed untrusted payload[length]. Pure.
export fn interop_payload_validate(kind: u16, direction: c.interop_direction_t, payload: ?[*]const u8, length: usize) c_int {
    if (length == 0) return validate(kind, direction, &.{});
    const data = payload orelse return Error.malformed;
    return validate(kind, direction, data[0..length]);
}

/// out: borrowed value pointer into payload; unchanged when absent.
export fn interop_field_get(payload: ?[*]const u8, length: usize, tag: u16, index: u32, value: ?*?[*]const u8, value_length: ?*u16) c_int {
    const value_out = value orelse return -1;
    const length_out = value_length orelse return -1;
    if (length > c.InteropMaxPayloadBytes) return -1;
    const data: []const u8 = if (length == 0) &.{} else (payload orelse return -1)[0..length];
    var walker = Walker{ .data = data };
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

/// out: little-endian field widened to u64; unchanged when absent or mismatched.
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

/// out: signed 32-bit field; unchanged when absent or mismatched.
export fn interop_field_i32(payload: ?[*]const u8, length: usize, tag: u16, value: *i32) c_int {
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    const found = interop_field_get(payload, length, tag, 0, &pointer, &size);
    if (found != 0) return found;
    if (size != 4) return -1;
    value.* = get(i32, pointer.?[0..4], 0);
    return 0;
}

export fn interop_writer_init(writer: *c.interop_writer_t, frame: ?[*]u8, capacity: usize) void {
    const usable = @min(capacity, c.InteropMaxFrameBytes);
    writer.* = .{ .frame = frame, .capacity = usable, .length = c.InteropHeaderBytes, .last_tag = 0, .fields = 0, .failed = 0 };
    if (frame == null or capacity < c.InteropHeaderBytes) writer.failed = 1;
}

fn writer_fail(writer: *c.interop_writer_t) c_int {
    writer.failed = 1;
    return Error.malformed;
}

export fn interop_put_bytes(writer: *c.interop_writer_t, tag: u16, value: ?*const anyopaque, length: u16) c_int {
    if (writer.failed != 0) return Error.malformed;
    const spec = spec_for(tag) orelse return writer_fail(writer);
    const repeat = tag == writer.last_tag and tag == c.InteropTagOutput;
    if (tag <= writer.last_tag and !repeat) return writer_fail(writer);
    if (writer.fields >= c.InteropMaxFields) return writer_fail(writer);
    if (length < spec.min or length > spec.max) return writer_fail(writer);
    const bytes: []const u8 = if (length == 0) &.{} else @as([*]const u8, @ptrCast(value orelse return writer_fail(writer)))[0..length];
    if (!value_valid(tag, bytes)) return writer_fail(writer);
    const padded = (@as(usize, length) + 3) & ~@as(usize, 3);
    if (writer.length + 4 + padded > writer.capacity or writer.length + 4 + padded > c.InteropMaxFrameBytes)
        return writer_fail(writer);
    const frame = writer.frame[0..writer.capacity];
    const critical_bit: u16 = c.InteropCriticalTag;
    const wire_tag: u16 = if (spec.critical) tag | critical_bit else tag;
    put(u16, frame, writer.length, wire_tag);
    put(u16, frame, writer.length + 2, length);
    @memcpy(frame[writer.length + 4 ..][0..length], bytes);
    @memset(frame[writer.length + 4 + length .. writer.length + 4 + padded], 0);
    writer.length += 4 + padded;
    writer.last_tag = tag;
    writer.fields += 1;
    return Error.none;
}

export fn interop_put_uint(writer: *c.interop_writer_t, tag: u16, value: u64) c_int {
    if (writer.failed != 0) return Error.malformed;
    const spec = spec_for(tag) orelse return writer_fail(writer);
    if (spec.min != spec.max) return writer_fail(writer);
    var bytes: [8]u8 = undefined;
    switch (spec.min) {
        1 => {
            if (value > std.math.maxInt(u8)) return writer_fail(writer);
            bytes[0] = @intCast(value);
        },
        2 => {
            if (value > std.math.maxInt(u16)) return writer_fail(writer);
            put(u16, &bytes, 0, @intCast(value));
        },
        4 => {
            if (value > std.math.maxInt(u32)) return writer_fail(writer);
            put(u32, &bytes, 0, @intCast(value));
        },
        8 => put(u64, &bytes, 0, value),
        else => return writer_fail(writer),
    }
    return interop_put_bytes(writer, tag, &bytes, spec.min);
}

export fn interop_put_i32(writer: *c.interop_writer_t, tag: u16, value: i32) c_int {
    var bytes: [4]u8 = undefined;
    put(i32, &bytes, 0, value);
    return interop_put_bytes(writer, tag, &bytes, 4);
}

export fn interop_put_rect(writer: *c.interop_writer_t, tag: u16, x: i32, y: i32, width: u32, height: u32) c_int {
    var bytes: [16]u8 = undefined;
    put(i32, &bytes, 0, x);
    put(i32, &bytes, 4, y);
    put(u32, &bytes, 8, width);
    put(u32, &bytes, 12, height);
    return interop_put_bytes(writer, tag, &bytes, 16);
}

export fn interop_put_point(writer: *c.interop_writer_t, tag: u16, x: i32, y: i32) c_int {
    var bytes: [8]u8 = undefined;
    put(i32, &bytes, 0, x);
    put(i32, &bytes, 4, y);
    return interop_put_bytes(writer, tag, &bytes, 8);
}

export fn interop_writer_finish(writer: *c.interop_writer_t, kind: u16, request_id: u32, direction: c.interop_direction_t, frame_length: *usize) c_int {
    if (writer.failed != 0) return Error.malformed;
    const frame = writer.frame[0..writer.length];
    const result = validate(kind, direction, frame[c.InteropHeaderBytes..]);
    if (result != Error.none) {
        writer.failed = 1;
        return result;
    }
    put(u32, frame, 0, c.InteropMagic);
    put(u16, frame, 4, c.InteropVersion);
    put(u16, frame, 6, kind);
    put(u32, frame, 8, @intCast(writer.length - c.InteropHeaderBytes));
    put(u32, frame, 12, request_id);
    frame_length.* = writer.length;
    return Error.none;
}

comptime {
    std.debug.assert(@sizeOf(Sha256) <= 128);
}
/// The C struct only guarantees 8-byte alignment, so the context is copied in
/// and out of its opaque bytes instead of being referenced in place.
fn load_hash(transfer: *const c.interop_transfer_t) Sha256 {
    return std.mem.bytesToValue(Sha256, transfer.hash_state[0..@sizeOf(Sha256)]);
}
fn store_hash(transfer: *c.interop_transfer_t, state: *const Sha256) void {
    @memcpy(transfer.hash_state[0..@sizeOf(Sha256)], std.mem.asBytes(state));
}

export fn interop_transfer_begin(transfer: *c.interop_transfer_t, id: u32, total_bytes: u64, cap: u64) c_int {
    if (id == 0) return Error.malformed;
    if (total_bytes > cap or total_bytes > c.InteropMaxTransferBytes) return Error.quota;
    transfer.* = .{ .id = id, .state = 1, .total_bytes = total_bytes, .received = 0, .hash_state = std.mem.zeroes([128]u8) };
    const state = Sha256.init(.{});
    store_hash(transfer, &state);
    return Error.none;
}

export fn interop_transfer_chunk(transfer: *c.interop_transfer_t, offset: u64, data: ?[*]const u8, length: usize) c_int {
    if (transfer.state != 1 or length == 0 or length > c.InteropChunkBytes or offset != transfer.received or
        length > transfer.total_bytes - transfer.received) return Error.protocol;
    const bytes = (data orelse return Error.protocol)[0..length];
    var state = load_hash(transfer);
    state.update(bytes);
    store_hash(transfer, &state);
    transfer.received += length;
    return Error.none;
}

export fn interop_transfer_end(transfer: *c.interop_transfer_t, hash: ?[*]const u8) c_int {
    const expected = (hash orelse return Error.protocol)[0..c.InteropHashBytes];
    if (transfer.state != 1 or transfer.received != transfer.total_bytes) return Error.protocol;
    var copy = load_hash(transfer);
    var digest: [c.InteropHashBytes]u8 = undefined;
    copy.final(&digest);
    if (!std.crypto.utils.timingSafeEql([c.InteropHashBytes]u8, digest, expected.*)) return Error.protocol;
    transfer.state = 2;
    return Error.none;
}

export fn interop_transfer_cancel(transfer: *c.interop_transfer_t) void {
    transfer.* = std.mem.zeroes(c.interop_transfer_t);
}

export fn interop_sha256(data: ?[*]const u8, length: usize, hash: [*]u8) void {
    const bytes: []const u8 = if (length == 0) &.{} else data.?[0..length];
    Sha256.hash(bytes, hash[0..c.InteropHashBytes], .{});
}

// ---------------------------------------------------------------------------
// Test-only fixtures.
// ---------------------------------------------------------------------------

/// Keeps test inputs opaque to the optimizer so exported functions run their
/// out-of-line branches instead of constant-folded inlined copies.
fn opaque_value(comptime V: type, value: V) V {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

const testing = std.testing;

fn frame_buffer() [c.InteropMaxFrameBytes]u8 {
    return std.mem.zeroes([c.InteropMaxFrameBytes]u8);
}

fn writer_for(buffer: []u8) c.interop_writer_t {
    var writer: c.interop_writer_t = undefined;
    interop_writer_init(&writer, buffer.ptr, buffer.len);
    return writer;
}

fn hello(buffer: []u8, epoch: u64) !usize {
    var writer = writer_for(buffer);
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagCapabilities, c.InteropCapWindows | c.InteropCapInput));
    try testing.expectEqual(Error.none, interop_put_bytes(&writer, c.InteropTagAgentVersion, "0.2.0", 5));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagLayoutVersion, 2));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagSessionEpoch, epoch));
    var length: usize = 0;
    try testing.expectEqual(Error.none, interop_writer_finish(&writer, c.InteropMsgHello, 0, c.InteropFromGuest, &length));
    return length;
}

test "hello round trip, header decode and typed field access" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 0x1122334455667788);
    var header: c.interop_header_t = undefined;
    try testing.expectEqual(Error.none, interop_header_decode(&buffer, length, &header));
    try testing.expectEqual(@as(u16, c.InteropMsgHello), header.type);
    try testing.expectEqual(length - 16, header.length);
    const payload = buffer[16..length];
    try testing.expectEqual(Error.none, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, payload.ptr, payload.len));
    var epoch: u64 = 0;
    try testing.expectEqual(@as(c_int, 0), interop_field_uint(payload.ptr, payload.len, c.InteropTagSessionEpoch, 8, &epoch));
    try testing.expectEqual(@as(u64, 0x1122334455667788), epoch);
    try testing.expectEqual(@as(c_int, 1), interop_field_uint(payload.ptr, payload.len, c.InteropTagMessage, 4, &epoch));
    try testing.expectEqual(@as(c_int, -1), interop_field_uint(payload.ptr, payload.len, c.InteropTagSessionEpoch, 4, &epoch));
    // Direction is enforced: a guest Hello cannot come from the host.
    try testing.expectEqual(Error.unsupported_type, interop_payload_validate(c.InteropMsgHello, c.InteropFromHost, payload.ptr, payload.len));
    try testing.expectEqual(Error.unsupported_type, interop_payload_validate(0x0FFF, c.InteropFromGuest, payload.ptr, payload.len));
    try testing.expectEqual(Error.unsupported_type, interop_payload_validate(c.InteropMsgHello, 9, payload.ptr, payload.len));
}

test "header rejects magic, version, length and short input" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 1);
    var header = std.mem.zeroes(c.interop_header_t);
    try testing.expectEqual(Error.malformed, interop_header_decode(&buffer, 15, &header));
    try testing.expectEqual(Error.malformed, interop_header_decode(null, 16, &header));
    var bad = buffer;
    bad[0] ^= 1;
    try testing.expectEqual(Error.protocol, interop_header_decode(&bad, length, &header));
    bad = buffer;
    bad[4] = 2;
    try testing.expectEqual(Error.protocol, interop_header_decode(&bad, length, &header));
    bad = buffer;
    put(u32, &bad, 8, 65524);
    try testing.expectEqual(Error.protocol, interop_header_decode(&bad, length, &header));
    put(u32, &bad, 8, 6);
    try testing.expectEqual(Error.protocol, interop_header_decode(&bad, length, &header));
    try testing.expectEqual(@as(u32, 0), header.magic); // unchanged on every failure
}

test "structural defects: order, duplicates, padding, criticality, unknown tags" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 7);
    const payload = buffer[16..length];
    // Swap the first two fields (capabilities 0x3A, agent_version 0x3B) -> out of order.
    var swapped: [256]u8 = undefined;
    const first = 4 + 8; // capabilities: header + 8 bytes
    const second = 4 + 8; // "0.2.0" padded to 8
    @memcpy(swapped[0..second], payload[first .. first + second]);
    @memcpy(swapped[second .. second + first], payload[0..first]);
    @memcpy(swapped[first + second .. payload.len], payload[first + second ..]);
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, &swapped, payload.len));
    // Nonzero padding after the 5-byte version string.
    var padded: [256]u8 = undefined;
    @memcpy(padded[0..payload.len], payload);
    padded[first + 4 + 5] = 1;
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, &padded, payload.len));
    // Criticality bit flipped on a known tag.
    @memcpy(padded[0..payload.len], payload);
    padded[1] &= 0x7F;
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, &padded, payload.len));
    // Unknown optional tag is skipped; unknown critical tag is refused.
    var extended: [256]u8 = undefined;
    @memcpy(extended[0..payload.len], payload);
    put(u16, &extended, payload.len, 0x0100);
    put(u16, &extended, payload.len + 2, 3);
    @memcpy(extended[payload.len + 4 ..][0..4], &[_]u8{ 1, 2, 3, 0 });
    try testing.expectEqual(Error.none, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, &extended, payload.len + 8));
    put(u16, &extended, payload.len, 0x8100);
    try testing.expectEqual(Error.unsupported_field, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, &extended, payload.len + 8));
    // Truncated field header and truncated value.
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, payload.ptr, payload.len - 4));
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, payload.ptr, 2));
    // Missing required field (drop the trailing session epoch).
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgHello, c.InteropFromGuest, payload.ptr, payload.len - 12));
    // Zero tag.
    var zero = [_]u8{ 0, 0, 0, 0 };
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, &zero, 4));
    try testing.expectEqual(Error.none, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, null, 0));
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, null, 4));
}

test "field present but not allowed for the message type is malformed" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagWindow, 5));
    var length: usize = 0;
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgPing, 0, c.InteropFromHost, &length));
    try testing.expectEqual(@as(usize, 0), length);
    // The writer stays poisoned after a failed finish.
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagGeneration, 1));
}

test "writer rejects order, values, capacity and poisons itself" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagGeneration, 3));
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagWindow, 3));
    try testing.expect(writer.failed != 0);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagRole, 1));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagWindow, 0)); // nonzero rule
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagRole, 300)); // width overflow
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, 0x7000, 1)); // unknown tag
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagTitle, 1)); // variable-size via uint
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagTitle, null, 3));
    var small: [20]u8 = undefined;
    writer = writer_for(&small);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagWindow, 1));
    writer = writer_for(small[0..10]);
    try testing.expect(writer.failed != 0);
    var length: usize = 0;
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgPing, 0, c.InteropFromHost, &length));
    interop_writer_init(&writer, null, 100);
    try testing.expect(writer.failed != 0);
}

test "window announce with every field, parent pairing and string rules" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagWindow, 0x10024));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagGeneration, 1));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagParent, 0x10020));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagParentGeneration, 1));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagRole, c.InteropRoleDialog));
    try testing.expectEqual(Error.none, interop_put_rect(&writer, c.InteropTagRect, -10, 20, 640, 480));
    const title = "Save As — Notepad";
    try testing.expectEqual(Error.none, interop_put_bytes(&writer, c.InteropTagTitle, title, title.len));
    try testing.expectEqual(Error.none, interop_put_bytes(&writer, c.InteropTagAppKey, "a-Microsoft_WindowsNotepad", 26));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagProcessId, 4242));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagState, c.InteropStateResizable));
    try testing.expectEqual(Error.none, interop_put_uint(&writer, c.InteropTagDpi, 144));
    var length: usize = 0;
    try testing.expectEqual(Error.none, interop_writer_finish(&writer, c.InteropMsgWindowAnnounce, 0, c.InteropFromGuest, &length));
    const payload = buffer[16..length];
    var x: i32 = 0;
    try testing.expectEqual(@as(c_int, 1), interop_field_i32(payload.ptr, payload.len, c.InteropTagAxisV120, &x));
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    try testing.expectEqual(@as(c_int, 0), interop_field_get(payload.ptr, payload.len, c.InteropTagTitle, 0, &pointer, &size));
    try testing.expectEqualStrings(title, pointer.?[0..size]);
    try testing.expectEqual(@as(c_int, -1), interop_field_get(payload.ptr, payload.len, c.InteropTagTitle, 0, null, &size));
    try testing.expectEqual(@as(c_int, -1), interop_field_get(null, 8, c.InteropTagTitle, 0, &pointer, &size));
    try testing.expectEqual(@as(c_int, -1), interop_field_i32(payload.ptr, payload.len, c.InteropTagWindow, &x));

    // Parent without its generation is rejected by the cross-field rule.
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c.InteropTagWindow, 1);
    _ = interop_put_uint(&writer, c.InteropTagGeneration, 1);
    _ = interop_put_uint(&writer, c.InteropTagParent, 2);
    _ = interop_put_uint(&writer, c.InteropTagRole, 1);
    _ = interop_put_rect(&writer, c.InteropTagRect, 0, 0, 1, 1);
    _ = interop_put_bytes(&writer, c.InteropTagAppKey, "p-00", 4);
    _ = interop_put_uint(&writer, c.InteropTagState, 0);
    _ = interop_put_uint(&writer, c.InteropTagDpi, 96);
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgWindowAnnounce, 0, c.InteropFromGuest, &length));

    // String and key rules.
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagAppKey, "x-bad", 5));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagAppKey, "a-dot.ted", 9));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagTitle, "a\x00b", 3));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagTitle, "\xff\xfe", 2));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_rect(&writer, c.InteropTagRect, 0, 0, 16385, 1));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagDpi, 95));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagState, 0x80));
}

const BoundaryCase = struct { tag: u16, good: []const u8, bad: []const u8 };
const le = struct {
    fn u16_(comptime v: u16) [2]u8 {
        var b: [2]u8 = undefined;
        std.mem.writeInt(u16, &b, v, .little);
        return b;
    }
    fn u32_(comptime v: u32) [4]u8 {
        var b: [4]u8 = undefined;
        std.mem.writeInt(u32, &b, v, .little);
        return b;
    }
    fn i32_(comptime v: i32) [4]u8 {
        var b: [4]u8 = undefined;
        std.mem.writeInt(i32, &b, v, .little);
        return b;
    }
    fn pair(comptime a: i32, comptime b2: i32) [8]u8 {
        return i32_(a) ++ i32_(b2);
    }
    fn u64_(comptime v: u64) [8]u8 {
        var b: [8]u8 = undefined;
        std.mem.writeInt(u64, &b, v, .little);
        return b;
    }
};
const boundary_cases = [_]BoundaryCase{
    .{ .tag = c.InteropTagRole, .good = &.{6}, .bad = &.{7} },
    .{ .tag = c.InteropTagSlot, .good = &le.u32_(191), .bad = &le.u32_(192) },
    .{ .tag = c.InteropTagDamage, .good = &(le.pair(0, 0) ++ le.u32_(16384) ++ le.u32_(1)), .bad = &(le.pair(0, 0) ++ le.u32_(16385) ++ le.u32_(1)) },
    .{ .tag = c.InteropTagEdge, .good = &.{10}, .bad = &.{3} },
    .{ .tag = c.InteropTagSerial, .good = &le.u32_(1), .bad = &le.u32_(0) },
    .{ .tag = c.InteropTagPoint, .good = &le.pair(-16384, 16384), .bad = &le.pair(0, 16385) },
    .{ .tag = c.InteropTagButton, .good = &le.u16_(5), .bad = &le.u16_(6) },
    .{ .tag = c.InteropTagPressed, .good = &.{1}, .bad = &.{2} },
    .{ .tag = c.InteropTagKey, .good = &le.u16_(767), .bad = &le.u16_(768) },
    .{ .tag = c.InteropTagAxisV120, .good = &le.i32_(-15360), .bad = &le.i32_(15361) },
    .{ .tag = c.InteropTagAxisH120, .good = &le.i32_(15360), .bad = &le.i32_(-15361) },
    .{ .tag = c.InteropTagDelta, .good = &le.pair(16384 << 8, 0), .bad = &le.pair(0, -(16384 << 8) - 1) },
    .{ .tag = c.InteropTagTouchId, .good = &le.u32_(9), .bad = &le.u32_(10) },
    .{ .tag = c.InteropTagPressure, .good = &le.u16_(1024), .bad = &le.u16_(1025) },
    .{ .tag = c.InteropTagTilt, .good = &.{ 0x28, 0x23, 0xD8, 0xDC }, .bad = &.{ 0x29, 0x23, 0, 0 } },
    .{ .tag = c.InteropTagRotation, .good = &le.u16_(35999), .bad = &le.u16_(36000) },
    .{ .tag = c.InteropTagTool, .good = &.{2}, .bad = &.{0} },
    .{ .tag = c.InteropTagCursorBegin, .good = &le.i32_(-1), .bad = &le.i32_(-2) },
    .{ .tag = c.InteropTagCursorEnd, .good = &le.i32_(4096), .bad = &le.i32_(4097) },
    .{ .tag = c.InteropTagLayout, .good = "0000040c", .bad = "0000040g" },
    .{ .tag = c.InteropTagLocks, .good = &.{7}, .bad = &.{8} },
    .{ .tag = c.InteropTagCursorShape, .good = &le.u16_(0xFFFF), .bad = &le.u16_(35) },
    .{ .tag = c.InteropTagHotspot, .good = &le.pair(512, 0), .bad = &le.pair(-1, 0) },
    .{ .tag = c.InteropTagImageSize, .good = &(le.u32_(512) ++ le.u32_(1)), .bad = &(le.u32_(0) ++ le.u32_(1)) },
    .{ .tag = c.InteropTagTransfer, .good = &le.u32_(1), .bad = &le.u32_(0) },
    .{ .tag = c.InteropTagTotalBytes, .good = &le.u64_(c.InteropMaxTransferBytes), .bad = &le.u64_(c.InteropMaxTransferBytes + 1) },
    .{ .tag = c.InteropTagOffset, .good = &le.u64_(c.InteropMaxTransferBytes - 1), .bad = &le.u64_(c.InteropMaxTransferBytes) },
    .{ .tag = c.InteropTagMime, .good = "text/plain;charset=utf-8", .bad = "text/plain; charset" },
    .{ .tag = c.InteropTagMimeList, .good = &([_]u8{9} ++ "image/png".*), .bad = &([_]u8{10} ++ "image/png".*) },
    .{ .tag = c.InteropTagDropEffect, .good = &.{7}, .bad = &.{8} },
    .{ .tag = c.InteropTagAppFlags, .good = &le.u32_(0x1F), .bad = &le.u32_(0x20) },
    .{ .tag = c.InteropTagIconSizes, .good = &(le.u16_(16) ++ le.u16_(256)), .bad = &(le.u16_(32) ++ le.u16_(16)) },
    .{ .tag = c.InteropTagIconSize, .good = &le.u16_(48), .bad = &le.u16_(40) },
    .{ .tag = c.InteropTagLaunchKind, .good = &.{5}, .bad = &.{6} },
    .{ .tag = c.InteropTagActivationToken, .good = "abc_123", .bad = "abc 123" },
    .{ .tag = c.InteropTagStatus, .good = &le.u32_(9), .bad = &le.u32_(10) },
    .{ .tag = c.InteropTagCapabilities, .good = &le.u64_(c.InteropCapabilityMask), .bad = &le.u64_(c.InteropCapabilityMask + 1) },
    .{ .tag = c.InteropTagAgentVersion, .good = "10.20.30", .bad = "1.2.3.4" },
    .{ .tag = c.InteropTagLayoutVersion, .good = &le.u32_(2), .bad = &le.u32_(0) },
    .{ .tag = c.InteropTagOutput, .good = &(le.u32_(0) ++ le.u32_(3840) ++ le.u32_(2160) ++ le.u32_(480)), .bad = &(le.u32_(0) ++ le.u32_(3840) ++ le.u32_(2160) ++ le.u32_(119)) },
    .{ .tag = c.InteropTagStream, .good = &le.u32_(7), .bad = &le.u32_(8) },
    .{ .tag = c.InteropTagPrinterName, .good = "Office_Laser", .bad = "Office/Laser" },
    .{ .tag = c.InteropTagPrinterUri, .good = "http://10.0.2.100:631/printers/x", .bad = "http://a b" },
    .{ .tag = c.InteropTagDecision, .good = &.{1}, .bad = &.{2} },
    .{ .tag = c.InteropTagErrorCode, .good = &le.u32_(8), .bad = &le.u32_(9) },
    .{ .tag = c.InteropTagIntervalMs, .good = &le.u32_(10000), .bad = &le.u32_(10001) },
    .{ .tag = c.InteropTagRate, .good = &le.u32_(1000), .bad = &le.u32_(1001) },
    .{ .tag = c.InteropTagGeneration, .good = &le.u64_(1), .bad = &le.u64_(0) },
    .{ .tag = c.InteropTagUriList, .good = "file:///home/u/a.txt\r\n", .bad = "\x00" },
};

test "every field value rule accepts its boundary and rejects one past it" {
    var buffer: [1024]u8 = undefined;
    for (boundary_cases) |case| {
        var writer = writer_for(&buffer);
        testing.expectEqual(Error.none, interop_put_bytes(&writer, case.tag, case.good.ptr, @intCast(case.good.len))) catch |err| {
            std.debug.print("tag 0x{x} rejected boundary value\n", .{case.tag});
            return err;
        };
        writer = writer_for(&buffer);
        testing.expectEqual(Error.malformed, interop_put_bytes(&writer, case.tag, case.bad.ptr, @intCast(case.bad.len))) catch |err| {
            std.debug.print("tag 0x{x} accepted out-of-range value\n", .{case.tag});
            return err;
        };
    }
    // Mime lists: empty entry, 33 entries.
    var writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagMimeList, &[_]u8{ 0, 0 }, 2));
    var many: [33 * 2]u8 = undefined;
    for (0..33) |i| {
        many[i * 2] = 1;
        many[i * 2 + 1] = 'a';
    }
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagMimeList, &many, many.len));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.none, interop_put_bytes(&writer, c.InteropTagMimeList, &many, 64));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagAgentVersion, "1..2", 4));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagAgentVersion, "1.2.", 4));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagAgentVersion, "1.2.3333333333", 14));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagDamage, "0123456789", 10));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagIconSizes, "abc", 3));
}

test "launch requests require exactly one target kind" {
    var buffer = frame_buffer();
    var length: usize = 0;
    var writer = writer_for(&buffer);
    _ = interop_put_bytes(&writer, c.InteropTagAppKey, "a-Microsoft_Windows_Explorer", 28);
    _ = interop_put_uint(&writer, c.InteropTagLaunchKind, c.InteropLaunchApp);
    _ = interop_put_bytes(&writer, c.InteropTagActivationToken, "token-1", 7);
    try testing.expectEqual(Error.none, interop_writer_finish(&writer, c.InteropMsgLaunchRequest, 3, c.InteropFromHost, &length));
    try testing.expectEqual(@as(u32, 3), get(u32, &buffer, 12));
    writer = writer_for(&buffer);
    _ = interop_put_bytes(&writer, c.InteropTagTargetPath, "Z:\\Downloads\\setup.exe", 22);
    _ = interop_put_uint(&writer, c.InteropTagLaunchKind, c.InteropLaunchPath);
    try testing.expectEqual(Error.none, interop_writer_finish(&writer, c.InteropMsgLocalLaunch, 0, c.InteropFromLocal, &length));
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c.InteropTagLaunchKind, c.InteropLaunchApp);
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgLaunchRequest, 0, c.InteropFromHost, &length));
    writer = writer_for(&buffer);
    _ = interop_put_bytes(&writer, c.InteropTagTargetPath, "C:\\x.exe", 8);
    _ = interop_put_bytes(&writer, c.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c.InteropTagLaunchKind, c.InteropLaunchPath);
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgLaunchRequest, 0, c.InteropFromHost, &length));
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c.InteropTagLaunchKind, c.InteropLaunchPath);
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgLocalLaunch, 0, c.InteropFromLocal, &length));
}

test "icon requests pair window and generation" {
    var buffer = frame_buffer();
    var length: usize = 0;
    var writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c.InteropTagWindow, 9);
    _ = interop_put_bytes(&writer, c.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c.InteropTagTransfer, 1);
    _ = interop_put_uint(&writer, c.InteropTagIconSize, 32);
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgIconRequest, 0, c.InteropFromHost, &length));
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c.InteropTagWindow, 9);
    _ = interop_put_uint(&writer, c.InteropTagGeneration, 2);
    _ = interop_put_bytes(&writer, c.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c.InteropTagTransfer, 1);
    _ = interop_put_uint(&writer, c.InteropTagIconSize, 32);
    try testing.expectEqual(Error.none, interop_writer_finish(&writer, c.InteropMsgIconRequest, 0, c.InteropFromHost, &length));
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
        try testing.expectEqual(Error.none, interop_put_bytes(&writer, c.InteropTagOutput, &output, 16));
    }
    try testing.expectEqual(Error.none, interop_writer_finish(&writer, c.InteropMsgDisplayConfig, 0, c.InteropFromHost, &length));
    const payload = buffer[16..length];
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    try testing.expectEqual(@as(c_int, 0), interop_field_get(payload.ptr, payload.len, c.InteropTagOutput, 7, &pointer, &size));
    try testing.expectEqual(@as(u32, 7), get(u32, pointer.?[0..size], 0));
    try testing.expectEqual(@as(c_int, 1), interop_field_get(payload.ptr, payload.len, c.InteropTagOutput, 8, &pointer, &size));
    // Nine outputs are rejected.
    try testing.expectEqual(Error.none, interop_put_bytes(&writer, c.InteropTagOutput, &output, 16));
    writer.failed = 0;
    try testing.expectEqual(Error.malformed, interop_writer_finish(&writer, c.InteropMsgDisplayConfig, 0, c.InteropFromHost, &length));
    // A repeated tag in any other message is out of order.
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c.InteropTagStatus, 0);
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagStatus, 0));
    // Hand-built repeat of a non-output tag in DisplayConfig position is refused by the validator.
    var raw: [24]u8 = std.mem.zeroes([24]u8);
    put(u16, &raw, 0, c.InteropTagStatus | c.InteropCriticalTag);
    put(u16, &raw, 2, 4);
    put(u16, &raw, 8, c.InteropTagStatus | c.InteropCriticalTag);
    put(u16, &raw, 10, 4);
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgTransferCancel, c.InteropFromHost, &raw, 16));
}

test "field count limit is enforced by writer and validator" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    writer.fields = c.InteropMaxFields;
    try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagWindow, 1));
    // 65 unknown optional fields fail validation even though each is skippable.
    var raw: [65 * 4]u8 = undefined;
    for (0..65) |index| {
        put(u16, &raw, index * 4, @intCast(0x100 + index));
        put(u16, &raw, index * 4 + 2, 0);
    }
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, &raw, raw.len));
    try testing.expectEqual(Error.none, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, &raw, 64 * 4));
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, &raw, 65530));
}

test "transfers: contiguity, bounds, digest and cancellation" {
    const content = "The quick brown fox jumps over the lazy dog";
    var digest: [32]u8 = undefined;
    interop_sha256(content, content.len, &digest);
    var transfer = std.mem.zeroes(c.interop_transfer_t);
    try testing.expectEqual(Error.malformed, interop_transfer_begin(&transfer, 0, 4, 64));
    try testing.expectEqual(Error.quota, interop_transfer_begin(&transfer, 1, 65, 64));
    try testing.expectEqual(Error.quota, interop_transfer_begin(&transfer, 1, c.InteropMaxTransferBytes + 1, std.math.maxInt(u64)));
    try testing.expectEqual(@as(u32, 0), transfer.id);
    try testing.expectEqual(Error.protocol, interop_transfer_chunk(&transfer, 0, content, 4)); // not begun
    try testing.expectEqual(Error.none, interop_transfer_begin(&transfer, 7, content.len, 64));
    try testing.expectEqual(Error.protocol, interop_transfer_chunk(&transfer, 1, content, 4)); // gap
    try testing.expectEqual(Error.protocol, interop_transfer_chunk(&transfer, 0, content, 0)); // empty
    try testing.expectEqual(Error.protocol, interop_transfer_chunk(&transfer, 0, null, 4));
    try testing.expectEqual(Error.none, interop_transfer_chunk(&transfer, 0, content, 10));
    try testing.expectEqual(Error.protocol, interop_transfer_chunk(&transfer, 0, content, 10)); // overlap
    try testing.expectEqual(Error.protocol, interop_transfer_end(&transfer, &digest)); // incomplete
    try testing.expectEqual(Error.protocol, interop_transfer_chunk(&transfer, 10, content[10..].ptr, content.len)); // overflow
    try testing.expectEqual(Error.none, interop_transfer_chunk(&transfer, 10, content[10..].ptr, content.len - 10));
    var wrong = digest;
    wrong[0] ^= 0xFF;
    try testing.expectEqual(Error.protocol, interop_transfer_end(&transfer, &wrong));
    try testing.expectEqual(Error.protocol, interop_transfer_end(&transfer, null));
    try testing.expectEqual(Error.none, interop_transfer_end(&transfer, &digest));
    try testing.expectEqual(@as(u32, 2), transfer.state);
    try testing.expectEqual(Error.protocol, interop_transfer_end(&transfer, &digest)); // already complete
    interop_transfer_cancel(&transfer);
    try testing.expectEqual(@as(u32, 0), transfer.state);
    // Oversize chunk and empty-transfer digest.
    var big = std.mem.zeroes([c.InteropChunkBytes + 1]u8);
    try testing.expectEqual(Error.none, interop_transfer_begin(&transfer, 2, big.len, big.len));
    try testing.expectEqual(Error.protocol, interop_transfer_chunk(&transfer, 0, &big, big.len));
    try testing.expectEqual(Error.none, interop_transfer_begin(&transfer, 3, 0, 0));
    interop_sha256(null, 0, &digest);
    try testing.expectEqual(Error.none, interop_transfer_end(&transfer, &digest));
}

test "every message type has a buildable minimal frame and rejects the wrong direction" {
    var buffer = frame_buffer();
    for (messages) |message| {
        var writer = writer_for(&buffer);
        var tags: [16]u16 = undefined;
        var count = message.required.len;
        @memcpy(tags[0..count], message.required);
        // Launch messages need one target to satisfy the cross-field rule.
        if (message.type == c.InteropMsgLaunchRequest or message.type == c.InteropMsgLocalLaunch) {
            tags[count] = c.InteropTagTargetPath;
            count += 1;
        }
        std.mem.sort(u16, tags[0..count], {}, std.sort.asc(u16));
        for (tags[0..count]) |tag| {
            const value = if (tag == c.InteropTagTargetPath) "C:\\a" else sample_value(tag);
            try testing.expectEqual(Error.none, interop_put_bytes(&writer, tag, value.ptr, @intCast(value.len)));
        }
        const direction: c.interop_direction_t = if (message.directions & DirGuest != 0) c.InteropFromGuest else if (message.directions & DirHost != 0) c.InteropFromHost else c.InteropFromLocal;
        var length: usize = 0;
        testing.expectEqual(Error.none, interop_writer_finish(&writer, message.type, 0, direction, &length)) catch |err| {
            std.debug.print("message 0x{x} minimal frame rejected\n", .{message.type});
            return err;
        };
        const payload = buffer[16..length];
        const other: c.interop_direction_t = if (message.directions & DirLocal != 0) c.InteropFromGuest else if (message.directions == DirBoth) c.InteropFromLocal else if (direction == c.InteropFromGuest) c.InteropFromHost else c.InteropFromGuest;
        try testing.expectEqual(Error.unsupported_type, interop_payload_validate(message.type, other, payload.ptr, payload.len));
    }
}

fn sample_value(tag: u16) []const u8 {
    const S = struct {
        const one8 = [_]u8{ 1, 0, 0, 0, 0, 0, 0, 0 };
        const one4 = [_]u8{ 1, 0, 0, 0 };
        const one2 = [_]u8{ 16, 0 };
        const one1 = [_]u8{1};
        const rect = [_]u8{0} ** 8 ++ [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0 };
        const point = [_]u8{0} ** 8;
        const dpi = [_]u8{ 96, 0, 0, 0 };
        const output = [_]u8{0} ** 4 ++ [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0, 120, 0, 0, 0 };
        const hash = [_]u8{0} ** 32;
        const usb = [_]u8{ 1, 0, 2, 0, 1, 1 };
        const zero4 = [_]u8{ 0, 0, 0, 0 };
        const image = [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0 };
        const mimes = [_]u8{4} ++ "text".*;
    };
    return switch (tag) {
        c.InteropTagWindow, c.InteropTagGeneration, c.InteropTagFrameSequence, c.InteropTagClipboardEpoch, c.InteropTagSessionEpoch, c.InteropTagLeaseEpoch, c.InteropTagCapabilities, c.InteropTagTotalBytes, c.InteropTagOffset, c.InteropTagParent, c.InteropTagParentGeneration => &S.one8,
        c.InteropTagRect => &S.rect,
        c.InteropTagPoint, c.InteropTagHotspot, c.InteropTagDelta => &S.point,
        c.InteropTagImageSize => &S.image,
        c.InteropTagDpi => &S.dpi,
        c.InteropTagOutput => &S.output,
        c.InteropTagContentHash => &S.hash,
        c.InteropTagUsbId => &S.usb,
        c.InteropTagStatus, c.InteropTagState, c.InteropTagAppFlags, c.InteropTagTouchId, c.InteropTagStream, c.InteropTagIntervalMs, c.InteropTagRate => &S.zero4,
        c.InteropTagButton => &[_]u8{ 1, 0 },
        c.InteropTagLaunchKind => &[_]u8{c.InteropLaunchPath},
        c.InteropTagKey, c.InteropTagIconSize, c.InteropTagCursorShape, c.InteropTagRefusedType, c.InteropTagPressure => &S.one2,
        c.InteropTagRole, c.InteropTagPressed, c.InteropTagTool, c.InteropTagDropEffect, c.InteropTagDecision, c.InteropTagLocks => &S.one1,
        c.InteropTagEdge => &[_]u8{1},
        c.InteropTagAppKey => "p-ab",
        c.InteropTagAgentVersion => "0.2.0",
        c.InteropTagLayout => "00000409",
        c.InteropTagMime => "text/plain",
        c.InteropTagMimeList => &S.mimes,
        c.InteropTagDisplayName, c.InteropTagPrinterName, c.InteropTagPrinterUri, c.InteropTagUriList => "x",
        c.InteropTagData => "d",
        c.InteropTagText => "",
        c.InteropTagTilt, c.InteropTagAxisV120, c.InteropTagAxisH120, c.InteropTagSlot, c.InteropTagProcessId => &S.zero4,
        c.InteropTagSerial, c.InteropTagTransfer, c.InteropTagLayoutVersion, c.InteropTagErrorCode => &S.one4,
        else => unreachable,
    };
}

test "deterministic mutation fuzz never crashes and keeps outputs unchanged on error" {
    var buffer = frame_buffer();
    const length = try hello(&buffer, 99);
    var prng = std.Random.DefaultPrng.init(0x5741444C);
    const random = prng.random();
    var mutated: [256]u8 = undefined;
    var accepted: usize = 0;
    for (0..20000) |_| {
        @memcpy(mutated[0..length], buffer[0..length]);
        const flips = random.intRangeAtMost(usize, 1, 4);
        for (0..flips) |_| mutated[random.uintLessThan(usize, length)] ^= random.int(u8) | 1;
        const cut = if (random.boolean()) length else random.uintLessThan(usize, length + 1);
        var header = std.mem.zeroes(c.interop_header_t);
        if (interop_header_decode(&mutated, cut, &header) != Error.none) {
            try testing.expectEqual(@as(u32, 0), header.magic);
            continue;
        }
        if (header.length > cut - 16) continue;
        const payload = mutated[16 .. 16 + header.length];
        if (interop_payload_validate(header.type, c.InteropFromGuest, payload.ptr, payload.len) == Error.none) {
            accepted += 1;
            var pointer: ?[*]const u8 = null;
            var size: u16 = 0;
            _ = interop_field_get(payload.ptr, payload.len, c.InteropTagAgentVersion, 0, &pointer, &size);
        }
    }
    try testing.expect(accepted < 20000);
}

test "every registered tag reaches a value rule without hitting the unreachable default" {
    var zeros = std.mem.zeroes([c.InteropChunkBytes]u8);
    for (1..MaxTag + 1) |index| {
        const base: u16 = @intCast(index);
        const spec = spec_for(base) orelse continue;
        _ = value_valid(base, zeros[0..spec.max]);
        _ = value_valid(base, zeros[0..spec.min]);
    }
    try testing.expect(spec_for(0) == null and spec_for(MaxTag + 1) == null);
}

test "runtime inputs exercise defensive branches of every export" {
    var buffer = frame_buffer();
    var writer = writer_for(&buffer);
    // Fields of widths 1, 2, 4 and 8 plus a 3-byte string read through field_uint.
    _ = interop_put_uint(&writer, c.InteropTagRole, 1);
    _ = interop_put_rect(&writer, c.InteropTagRect, 0, 0, 1, 1);
    _ = interop_put_bytes(&writer, c.InteropTagTitle, "abc", 3);
    _ = interop_put_uint(&writer, c.InteropTagState, 3);
    _ = interop_put_uint(&writer, c.InteropTagKey, 30);
    _ = interop_put_uint(&writer, c.InteropTagClipboardEpoch, 9);
    const payload = buffer[16..writer.length];
    const Read = struct { tag: u16, width: u32, result: c_int, value: u64 };
    const reads = [_]Read{
        .{ .tag = c.InteropTagRole, .width = 1, .result = 0, .value = 1 },
        .{ .tag = c.InteropTagKey, .width = 2, .result = 0, .value = 30 },
        .{ .tag = c.InteropTagState, .width = 4, .result = 0, .value = 3 },
        .{ .tag = c.InteropTagClipboardEpoch, .width = 8, .result = 0, .value = 9 },
        .{ .tag = c.InteropTagTitle, .width = 3, .result = -1, .value = 0 },
        .{ .tag = c.InteropTagRect, .width = 16, .result = -1, .value = 0 },
        .{ .tag = c.InteropTagWindow, .width = 8, .result = 1, .value = 0 },
    };
    for (reads) |read| {
        var value: u64 = 0;
        try testing.expectEqual(read.result, interop_field_uint(payload.ptr, opaque_value(usize, payload.len), opaque_value(u16, read.tag), opaque_value(u32, read.width), &value));
        try testing.expectEqual(read.value, value);
        var signed: i32 = 0;
        const expected: c_int = if (read.result == 1) 1 else if (read.width == 4) 0 else -1;
        try testing.expectEqual(expected, interop_field_i32(payload.ptr, opaque_value(usize, payload.len), opaque_value(u16, read.tag), &signed));
    }
    // field_get on malformed, short and NULL inputs.
    var pointer: ?[*]const u8 = null;
    var size: u16 = 0;
    const bad = [_]u8{ 1, 0, 9, 0, 0, 0 };
    try testing.expectEqual(@as(c_int, -1), interop_field_get(&bad, opaque_value(usize, 6), 1, 0, &pointer, &size));
    try testing.expectEqual(@as(c_int, -1), interop_field_get(&bad, opaque_value(usize, 2), 1, 0, &pointer, &size));
    try testing.expectEqual(@as(c_int, 1), interop_field_get(null, opaque_value(usize, 0), 1, 0, &pointer, &size));
    try testing.expectEqual(@as(c_int, -1), interop_field_get(null, opaque_value(usize, 4), 1, 0, &pointer, &size));
    try testing.expectEqual(@as(c_int, -1), interop_field_get(&bad, opaque_value(usize, 65524), 1, 0, &pointer, &size));
    try testing.expectEqual(@as(c_int, -1), interop_field_get(&bad, 4, 1, 0, opaque_value(?*?[*]const u8, null), &size));
    try testing.expectEqual(@as(c_int, -1), interop_field_get(&bad, 4, 1, 0, &pointer, opaque_value(?*u16, null)));
    // payload_validate on NULL, unaligned and oversize inputs.
    const lengths = [_]usize{ 4, 6, 65524 };
    for (lengths) |length| try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, opaque_value(?[*]const u8, null), opaque_value(usize, length)));
    try testing.expectEqual(Error.malformed, interop_payload_validate(c.InteropMsgPing, c.InteropFromHost, &bad, opaque_value(usize, 6)));
    // Writer: unknown tag, poisoned writer, width overflows, non-integer registry widths.
    const Put = struct { tag: u16, value: u64 };
    const puts = [_]Put{
        .{ .tag = 0x7000, .value = 1 },
        .{ .tag = c.InteropTagKey, .value = 70000 },
        .{ .tag = c.InteropTagStatus, .value = 1 << 33 },
        .{ .tag = c.InteropTagRole, .value = 256 },
        .{ .tag = c.InteropTagRect, .value = 1 },
        .{ .tag = c.InteropTagUsbId, .value = 1 },
        .{ .tag = c.InteropTagContentHash, .value = 1 },
    };
    for (puts) |item| {
        writer = writer_for(&buffer);
        try testing.expectEqual(Error.malformed, interop_put_uint(&writer, opaque_value(u16, item.tag), opaque_value(u64, item.value)));
        try testing.expectEqual(Error.malformed, interop_put_uint(&writer, c.InteropTagWindow, 1)); // poisoned
        try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, c.InteropTagWindow, "12345678", 8));
    }
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, opaque_value(u16, 0x7000), "x", 1));
    writer = writer_for(&buffer);
    try testing.expectEqual(Error.malformed, interop_put_bytes(&writer, opaque_value(u16, 0), "x", 1));
    // Transfers with runtime ids, sizes and digests.
    const Begin = struct { id: u32, total: u64, cap: u64, result: c_int };
    const begins = [_]Begin{
        .{ .id = 0, .total = 1, .cap = 8, .result = Error.malformed },
        .{ .id = 1, .total = 9, .cap = 8, .result = Error.quota },
        .{ .id = 1, .total = c.InteropMaxTransferBytes + 1, .cap = std.math.maxInt(u64), .result = Error.quota },
        .{ .id = 5, .total = 1, .cap = 8, .result = Error.none },
    };
    var transfer = std.mem.zeroes(c.interop_transfer_t);
    for (begins) |begin| try testing.expectEqual(begin.result, interop_transfer_begin(&transfer, opaque_value(u32, begin.id), opaque_value(u64, begin.total), opaque_value(u64, begin.cap)));
    try testing.expectEqual(Error.protocol, interop_transfer_end(&transfer, opaque_value(?[*]const u8, null)));
    try testing.expectEqual(Error.none, interop_transfer_chunk(&transfer, opaque_value(u64, 0), "z", opaque_value(usize, 1)));
    var digest: [32]u8 = undefined;
    interop_sha256(opaque_value(?[*]const u8, "z"), opaque_value(usize, 1), &digest);
    try testing.expectEqual(Error.none, interop_transfer_end(&transfer, &digest));
    interop_sha256(opaque_value(?[*]const u8, null), opaque_value(usize, 0), &digest);
    // Value rules reached with runtime data: app keys, damage, outputs, printer names, mimes.
    const Value = struct { tag: u16, bytes: []const u8, ok: bool };
    const output_ok = [_]u8{0} ** 4 ++ [_]u8{ 0, 15, 0, 0, 0, 8, 0, 0, 120, 0, 0, 0 };
    const output_tall = [_]u8{0} ** 4 ++ [_]u8{ 0, 15, 0, 0, 0, 0, 0, 0, 120, 0, 0, 0 };
    const output_wide = [_]u8{0} ** 4 ++ [_]u8{ 0, 0, 0, 0, 0, 8, 0, 0, 120, 0, 0, 0 };
    const damage_two = [_]u8{0} ** 8 ++ [_]u8{ 1, 0, 0, 0, 1, 0, 0, 0 } ++ [_]u8{0} ** 8 ++ [_]u8{ 1, 0, 0, 0, 1, 0x80, 0, 0 };
    const values = [_]Value{
        .{ .tag = c.InteropTagAppKey, .bytes = "a-x", .ok = true },
        .{ .tag = c.InteropTagAppKey, .bytes = "ab-", .ok = false },
        .{ .tag = c.InteropTagAppKey, .bytes = "p_x", .ok = false },
        .{ .tag = c.InteropTagAppKey, .bytes = "q-x", .ok = false },
        .{ .tag = c.InteropTagAppKey, .bytes = "a-ÿ", .ok = false },
        .{ .tag = c.InteropTagDamage, .bytes = &damage_two, .ok = false },
        .{ .tag = c.InteropTagDamage, .bytes = damage_two[0..16], .ok = true },
        .{ .tag = c.InteropTagOutput, .bytes = &output_ok, .ok = true },
        .{ .tag = c.InteropTagOutput, .bytes = &output_tall, .ok = false },
        .{ .tag = c.InteropTagOutput, .bytes = &output_wide, .ok = false },
        .{ .tag = c.InteropTagPrinterName, .bytes = "a b", .ok = false },
        .{ .tag = c.InteropTagPrinterName, .bytes = "a#b", .ok = false },
        .{ .tag = c.InteropTagMime, .bytes = "", .ok = false },
        .{ .tag = c.InteropTagEdge, .bytes = &.{2}, .ok = true },
        .{ .tag = c.InteropTagEdge, .bytes = &.{7}, .ok = false },
    };
    for (values) |item| try testing.expectEqual(item.ok, value_valid(opaque_value(u16, item.tag), opaque_value([]const u8, item.bytes)));
    // Cross rules with runtime payloads: icon request with generation but no window.
    writer = writer_for(&buffer);
    _ = interop_put_uint(&writer, c.InteropTagGeneration, 2);
    _ = interop_put_bytes(&writer, c.InteropTagAppKey, "p-ab", 4);
    _ = interop_put_uint(&writer, c.InteropTagTransfer, 1);
    _ = interop_put_uint(&writer, c.InteropTagIconSize, 32);
    const icon = buffer[16..writer.length];
    try testing.expectEqual(Error.malformed, interop_payload_validate(opaque_value(u16, c.InteropMsgIconRequest), c.InteropFromHost, icon.ptr, opaque_value(usize, icon.len)));
    try testing.expectEqual(Error.malformed, interop_payload_validate(opaque_value(u16, c.InteropMsgWindowRetire), c.InteropFromGuest, icon.ptr, opaque_value(usize, icon.len)));
}
