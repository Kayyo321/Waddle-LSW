//! Local session.sock frame scanner. Bounds the first frame of a client receive buffer to
//! MaxLocalFrameBytes before the C handler copies or dispatches it. Header decoding is the
//! production codec (interop_header_decode); this module adds the local size bound and the
//! completeness check. It owns no memory and performs no I/O.
const std_t = @import("std");
const c_t = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

/// Largest local frame in bytes, header included. Stricter than the codec limit.
pub const MaxLocalFrameBytes: usize = 4096;

/// Scanner result values; the C header session_local_handler.h repeats them.
pub const ScanNeedMore: c_int = 0;
pub const ScanReady: c_int = 1;
pub const ScanMalformed: c_int = 2;
pub const ScanTooLarge: c_int = 3;

/// Classify the first frame in a receive buffer.
/// @param bytes in Borrowed receive bytes; may be null only when length is zero.
/// @param length in Bytes available from the start of the buffer.
/// @param header_out out Writable header; written only for ScanReady.
/// @param frame_length_out out Writable size; set to the complete frame size only for
///        ScanReady, otherwise zero.
/// @return ScanNeedMore when fewer bytes than the frame need are present (the header is
///         not decoded before InteropHeaderBytes arrive), ScanReady for a complete frame,
///         ScanMalformed for a codec header rejection or null output, ScanTooLarge when the
///         announced frame exceeds MaxLocalFrameBytes (checked before completeness, so an
///         oversize announcement is refused without buffering it).
/// @ownership Borrows bytes for the call only; retains nothing.
/// @thread_safety Pure; safe from any thread.
export fn interop_local_frame_scan(
    bytes: ?[*]const u8,
    length: usize,
    header_out: ?*c_t.interop_header_t,
    frame_length_out: ?*usize,
) c_int {
    const out_header = header_out orelse return ScanMalformed;
    const out_length = frame_length_out orelse return ScanMalformed;
    out_length.* = 0;
    if (length < c_t.InteropHeaderBytes) return ScanNeedMore;
    const data = bytes orelse return ScanMalformed;
    var header: c_t.interop_header_t = undefined;
    if (c_t.interop_header_decode(data, length, &header) != c_t.InteropErrorNone) return ScanMalformed;
    const frame_size = c_t.InteropHeaderBytes + @as(usize, header.length);
    if (frame_size > MaxLocalFrameBytes) return ScanTooLarge;
    if (length < frame_size) return ScanNeedMore;
    out_header.* = header;
    out_length.* = frame_size;
    return ScanReady;
}

fn put_header(frame: []u8, magic: u32, version: u16, kind: u16, length: u32) void {
    std_t.mem.writeInt(u32, frame[0..4], magic, .little);
    std_t.mem.writeInt(u16, frame[4..6], version, .little);
    std_t.mem.writeInt(u16, frame[6..8], kind, .little);
    std_t.mem.writeInt(u32, frame[8..12], length, .little);
    std_t.mem.writeInt(u32, frame[12..16], 7, .little);
}

test "fewer than a header is need-more and writes nothing" {
    var header: c_t.interop_header_t = undefined;
    var length: usize = 99;
    const bytes = [_]u8{ 0x57, 0x49, 0x4F };
    try std_t.testing.expectEqual(ScanNeedMore, interop_local_frame_scan(&bytes, bytes.len, &header, &length));
    try std_t.testing.expectEqual(@as(usize, 0), length);
    try std_t.testing.expectEqual(ScanNeedMore, interop_local_frame_scan(null, 0, &header, &length));
}

test "a complete empty frame is ready and an incomplete payload is need-more" {
    var frame = [_]u8{0} ** 64;
    put_header(&frame, c_t.InteropMagic, c_t.InteropVersion, c_t.InteropMsgLocalStop, 0);
    var header: c_t.interop_header_t = undefined;
    var length: usize = 0;
    try std_t.testing.expectEqual(ScanReady, interop_local_frame_scan(&frame, 16, &header, &length));
    try std_t.testing.expectEqual(@as(usize, 16), length);
    try std_t.testing.expectEqual(@as(u16, c_t.InteropMsgLocalStop), header.type);
    try std_t.testing.expectEqual(@as(u32, 7), header.request_id);

    put_header(&frame, c_t.InteropMagic, c_t.InteropVersion, c_t.InteropMsgLocalLaunch, 8);
    try std_t.testing.expectEqual(ScanNeedMore, interop_local_frame_scan(&frame, 20, &header, &length));
    try std_t.testing.expectEqual(@as(usize, 0), length);
    try std_t.testing.expectEqual(ScanReady, interop_local_frame_scan(&frame, 24, &header, &length));
    try std_t.testing.expectEqual(@as(usize, 24), length);
}

test "bytes after the first frame are not consumed by the scan" {
    var frame = [_]u8{0} ** 64;
    put_header(&frame, c_t.InteropMagic, c_t.InteropVersion, c_t.InteropMsgLocalStop, 0);
    @memset(frame[16..], 0xAA);
    var header: c_t.interop_header_t = undefined;
    var length: usize = 0;
    try std_t.testing.expectEqual(ScanReady, interop_local_frame_scan(&frame, frame.len, &header, &length));
    try std_t.testing.expectEqual(@as(usize, 16), length);
}

test "a bad magic, a bad version, or an unaligned length is malformed" {
    var frame = [_]u8{0} ** 16;
    var header: c_t.interop_header_t = undefined;
    var length: usize = 0;
    put_header(&frame, c_t.InteropMagic + 1, c_t.InteropVersion, c_t.InteropMsgLocalStop, 0);
    try std_t.testing.expectEqual(ScanMalformed, interop_local_frame_scan(&frame, 16, &header, &length));
    put_header(&frame, c_t.InteropMagic, c_t.InteropVersion + 1, c_t.InteropMsgLocalStop, 0);
    try std_t.testing.expectEqual(ScanMalformed, interop_local_frame_scan(&frame, 16, &header, &length));
    put_header(&frame, c_t.InteropMagic, c_t.InteropVersion, c_t.InteropMsgLocalStop, 2);
    try std_t.testing.expectEqual(ScanMalformed, interop_local_frame_scan(&frame, 16, &header, &length));
    try std_t.testing.expectEqual(@as(usize, 0), length);
}

test "an announced frame above the local bound is too large before it is buffered" {
    var frame = [_]u8{0} ** 16;
    put_header(&frame, c_t.InteropMagic, c_t.InteropVersion, c_t.InteropMsgLocalLaunch, @intCast(MaxLocalFrameBytes - 16 + 4));
    var header: c_t.interop_header_t = undefined;
    var length: usize = 0;
    try std_t.testing.expectEqual(ScanTooLarge, interop_local_frame_scan(&frame, 16, &header, &length));
    try std_t.testing.expectEqual(@as(usize, 0), length);
}

test "a frame of exactly the local bound is accepted" {
    var frame = [_]u8{0} ** MaxLocalFrameBytes;
    put_header(&frame, c_t.InteropMagic, c_t.InteropVersion, c_t.InteropMsgLocalLaunch, @intCast(MaxLocalFrameBytes - 16));
    var header: c_t.interop_header_t = undefined;
    var length: usize = 0;
    try std_t.testing.expectEqual(ScanReady, interop_local_frame_scan(&frame, MaxLocalFrameBytes, &header, &length));
    try std_t.testing.expectEqual(MaxLocalFrameBytes, length);
}

test "null output pointers are malformed" {
    var length: usize = 0;
    var header: c_t.interop_header_t = undefined;
    const frame = [_]u8{0} ** 16;
    try std_t.testing.expectEqual(ScanMalformed, interop_local_frame_scan(&frame, 16, null, &length));
    try std_t.testing.expectEqual(ScanMalformed, interop_local_frame_scan(&frame, 16, &header, null));
    try std_t.testing.expectEqual(ScanMalformed, interop_local_frame_scan(null, 16, &header, &length));
}
