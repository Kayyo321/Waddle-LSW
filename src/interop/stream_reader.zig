//! Fixed-capacity protocol stream framing backed by the frozen C codec.
const std_t = @import("std");
const c_t = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

/// Maximum complete frame storage owned by one stream reader.
pub const MaxFrameBytes: usize = c_t.InteropMaxFrameBytes;
/// Fixed protocol header size.
pub const HeaderBytes: usize = c_t.InteropHeaderBytes;

/// Result from one bounded feed attempt.
/// @ownership Contains only copied scalar values; no borrowed storage.
/// @errors Feed errors are returned by reader_t.feed.
pub const feed_result_t = struct {
    /// Number of bytes copied from the borrowed input.
    consumed: usize,
    /// Whether one complete frame can be peeked.
    frame_ready: bool,
    /// Exact codec error when header decoding failed; None otherwise.
    header_error: c_int,
};

/// Borrowed view of one complete frame.
/// @ownership Header is copied; frame and payload borrow reader storage until
/// consume, deinitialization, or reader mutation.
/// @thread_safety The owning event-loop thread must serialize all accesses.
pub const frame_view_t = struct {
    /// Decoded header copied from reader state.
    header: c_t.interop_header_t,
    /// Complete header and payload bytes, borrowed until consume.
    frame: []const u8,
    /// Payload bytes, borrowed until consume; empty for zero-length payloads.
    payload: []const u8,
    /// Codec validation result; interpret payload fields only when None.
    payload_validation: c_int,
};

/// Caller-owned fixed-capacity stream accumulator.
/// @ownership Owns its 65,536-byte frame buffer and decoded header by value;
/// retains no caller input, OS handle, or outbound state.
/// @thread_safety One serialized event-loop thread owns each instance.
pub const reader_t = struct {
    /// Fixed frame storage; accessible only through feed/peek.
    buffer: [MaxFrameBytes]u8 = undefined,
    /// Last decoded header, copied from the codec output.
    header: c_t.interop_header_t = undefined,
    /// Producer direction validated by the codec for each complete payload.
    direction: c_t.interop_direction_t,
    /// Bytes accumulated for the current frame.
    received: usize = 0,
    /// Complete frame size after a valid header; zero until header decode.
    frame_length: usize = 0,
    /// Sticky failed state after malformed header or truncated input.
    failed: bool = false,
    /// Clean EOF after all complete frames were consumed.
    input_closed: bool = false,
    /// Exact nonzero header codec result, retained for diagnostics.
    header_error: c_int = c_t.InteropErrorNone,

    /// Initialize a fresh endpoint reader for one supported producer direction.
    /// @param direction in Producer direction 1/2/3 from the frozen protocol ABI.
    /// @return A cleared fixed-buffer reader.
    /// @errors InvalidDirection when direction is outside Guest/Host/Local.
    /// @ownership Returned reader is caller-owned by value; no allocation occurs.
    /// @thread_safety Use from one serialized event-loop thread.
    pub fn init(direction: c_t.interop_direction_t) error{InvalidDirection}!reader_t {
        if (direction != c_t.InteropFromGuest and direction != c_t.InteropFromHost and direction != c_t.InteropFromLocal) return error.InvalidDirection;
        return .{
            .buffer = [_]u8{0} ** MaxFrameBytes,
            .header = std_t.mem.zeroes(c_t.interop_header_t),
            .direction = direction,
        };
    }

    /// Copy at most one frame from a borrowed input slice.
    /// @param self in/out Reader whose state advances by the returned consumed count.
    /// @param input in Accessible borrowed bytes; must not overlap any reader storage.
    /// @return Consumed count and ready flag; a header codec error is saved and
    /// returned in header_error while the reader becomes failed.
    /// @errors InputAliasesReader, ReaderFailed, or InputClosed.
    /// @ownership Input is never retained; the reader owns all copied bytes.
    /// @thread_safety Caller serializes calls with peek/consume/finish_input.
    pub fn feed(self: *reader_t, input: []const u8) error{ InputAliasesReader, ReaderFailed, InputClosed }!feed_result_t {
        if (ranges_overlap(@intFromPtr(self), @sizeOf(reader_t), @intFromPtr(input.ptr), input.len)) return error.InputAliasesReader;
        if (self.failed) return error.ReaderFailed;
        if (self.input_closed) return error.InputClosed;
        if (self.frame_ready()) return .{ .consumed = 0, .frame_ready = true, .header_error = self.header_error };
        if (input.len == 0) return .{ .consumed = 0, .frame_ready = false, .header_error = self.header_error };

        const header_count = if (self.frame_length == 0)
            @min(input.len, HeaderBytes - self.received)
        else
            0;
        if (header_count != 0) {
            @memcpy(self.buffer[self.received .. self.received + header_count], input[0..header_count]);
            self.received += header_count;
        }
        var consumed = header_count;
        if (self.frame_length == 0 and self.received == HeaderBytes) {
            const result = c_t.interop_header_decode(&self.buffer, HeaderBytes, &self.header);
            if (result != c_t.InteropErrorNone) {
                self.failed = true;
                self.header_error = result;
                return .{ .consumed = consumed, .frame_ready = false, .header_error = result };
            }
            self.frame_length = HeaderBytes + @as(usize, @intCast(self.header.length));
        }
        if (self.frame_length != 0 and self.received < self.frame_length and consumed < input.len) {
            const body_count = @min(input.len - consumed, self.frame_length - self.received);
            @memcpy(self.buffer[self.received .. self.received + body_count], input[consumed .. consumed + body_count]);
            self.received += body_count;
            consumed += body_count;
        }
        return .{ .consumed = consumed, .frame_ready = self.frame_ready(), .header_error = self.header_error };
    }

    /// Borrow a complete frame and report codec payload validation.
    /// @param self in Reader with one pending complete frame.
    /// @return Copied header and borrowed complete frame/payload views.
    /// @errors ReaderFailed, InputClosed, or FrameNotReady.
    /// @ownership Slices borrow reader storage and expire at consume/deinit/mutation.
    /// @thread_safety Caller serializes calls with feed/consume/finish_input.
    pub fn peek(self: *const reader_t) error{ ReaderFailed, InputClosed, FrameNotReady }!frame_view_t {
        if (self.failed) return error.ReaderFailed;
        if (self.input_closed) return error.InputClosed;
        if (!self.frame_ready()) return error.FrameNotReady;
        const payload = self.buffer[HeaderBytes..self.frame_length];
        const payload_ptr: [*c]const u8 = if (payload.len == 0) null else payload.ptr;
        const validation = c_t.interop_payload_validate(self.header.type, self.direction, payload_ptr, payload.len);
        return .{
            .header = self.header,
            .frame = self.buffer[0..self.frame_length],
            .payload = payload,
            .payload_validation = validation,
        };
    }

    /// Invalidate borrowed frame views and reset only the current accumulator.
    /// @param self in/out Reader with a complete pending frame.
    /// @return Void after the frame is consumed.
    /// @errors ReaderFailed, InputClosed, or FrameNotReady without mutation.
    /// @thread_safety Caller serializes calls with feed/peek/finish_input.
    pub fn consume(self: *reader_t) error{ ReaderFailed, InputClosed, FrameNotReady }!void {
        if (self.failed) return error.ReaderFailed;
        if (self.input_closed) return error.InputClosed;
        if (!self.frame_ready()) return error.FrameNotReady;
        self.received = 0;
        self.frame_length = 0;
    }

    /// Mark clean EOF, or fail closed when input ends mid-frame.
    /// @param self in/out Reader whose current frame state is inspected.
    /// @return Void on clean EOF with no pending frame.
    /// @errors ReaderFailed, FramePending, or TruncatedFrame.
    /// @thread_safety Caller serializes calls with feed/peek/consume.
    pub fn finish_input(self: *reader_t) error{ ReaderFailed, FramePending, TruncatedFrame }!void {
        if (self.failed) return error.ReaderFailed;
        if (self.frame_ready()) return error.FramePending;
        if (self.received != 0) {
            self.failed = true;
            return error.TruncatedFrame;
        }
        self.input_closed = true;
    }

    fn frame_ready(self: *const reader_t) bool {
        return self.frame_length != 0 and self.received == self.frame_length;
    }
};

fn ranges_overlap(left: usize, left_len: usize, right: usize, right_len: usize) bool {
    if (left_len == 0 or right_len == 0) return false;
    const left_end = std_t.math.add(usize, left, left_len) catch std_t.math.maxInt(usize);
    const right_end = std_t.math.add(usize, right, right_len) catch std_t.math.maxInt(usize);
    return left < right_end and right < left_end;
}

// Test-only fixtures.
fn make_ping_frame(frame: []u8) !usize {
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, frame.ptr, frame.len);
    var frame_length: usize = 0;
    const result = c_t.interop_writer_finish(&writer, c_t.InteropMsgPing, 0, c_t.InteropFromHost, &frame_length);
    if (result != c_t.InteropErrorNone) return error.CodecFixtureFailure;
    return frame_length;
}

fn encode_header_for_test(frame: []u8, payload_length: u32, message_type: u16) void {
    std_t.mem.writeInt(u32, frame[0..4], c_t.InteropMagic, .little);
    std_t.mem.writeInt(u16, frame[4..6], c_t.InteropVersion, .little);
    std_t.mem.writeInt(u16, frame[6..8], message_type, .little);
    std_t.mem.writeInt(u32, frame[8..12], payload_length, .little);
    std_t.mem.writeInt(u32, frame[12..16], 1, .little);
}

test "real codec ping frame is framed at every split and bytewise" {
    var frame: [64]u8 = undefined;
    const frame_length = try make_ping_frame(&frame);
    for (0..frame_length + 1) |split| {
        var reader = try reader_t.init(c_t.InteropFromHost);
        const first = try reader.feed(frame[0..split]);
        try std_t.testing.expectEqual(split, first.consumed);
        if (split < frame_length) try std_t.testing.expect(!first.frame_ready);
        const second = try reader.feed(frame[split..frame_length]);
        try std_t.testing.expectEqual(frame_length - split, second.consumed);
        try std_t.testing.expect(second.frame_ready);
        const view = try reader.peek();
        try std_t.testing.expectEqual(c_t.InteropErrorNone, view.payload_validation);
        try std_t.testing.expectEqualSlices(u8, frame[0..frame_length], view.frame);
        try reader.consume();
    }
    var byte_reader = try reader_t.init(c_t.InteropFromHost);
    for (frame[0..frame_length], 0..) |_, index| {
        const result = try byte_reader.feed(frame[index .. index + 1]);
        try std_t.testing.expectEqual(@as(usize, 1), result.consumed);
        try std_t.testing.expectEqual(index + 1 == frame_length, result.frame_ready);
    }
}

test "coalesced frames stop at one boundary and require explicit consume" {
    var frame: [64]u8 = undefined;
    const frame_length = try make_ping_frame(&frame);
    var stream: [128]u8 = undefined;
    @memcpy(stream[0..frame_length], frame[0..frame_length]);
    @memcpy(stream[frame_length .. frame_length * 2], frame[0..frame_length]);
    var reader = try reader_t.init(c_t.InteropFromHost);
    const first = try reader.feed(stream[0 .. frame_length * 2]);
    try std_t.testing.expectEqual(frame_length, first.consumed);
    try std_t.testing.expect(first.frame_ready);
    const blocked = try reader.feed(stream[frame_length .. frame_length * 2]);
    try std_t.testing.expectEqual(@as(usize, 0), blocked.consumed);
    try reader.consume();
    const second = try reader.feed(stream[frame_length .. frame_length * 2]);
    try std_t.testing.expectEqual(frame_length, second.consumed);
    try std_t.testing.expect(second.frame_ready);
}

test "maximum frame is bounded and payload validation remains visible" {
    var reader = try reader_t.init(c_t.InteropFromHost);
    var frame: [MaxFrameBytes]u8 = [_]u8{0} ** MaxFrameBytes;
    encode_header_for_test(&frame, c_t.InteropMaxPayloadBytes, c_t.InteropMsgPing);
    const result = try reader.feed(&frame);
    try std_t.testing.expectEqual(MaxFrameBytes, result.consumed);
    try std_t.testing.expect(result.frame_ready);
    const view = try reader.peek();
    try std_t.testing.expectEqual(@as(usize, c_t.InteropMaxPayloadBytes), view.payload.len);
    try std_t.testing.expectEqual(c_t.InteropErrorMalformedFrame, view.payload_validation);
}

test "bad header fails closed and retains exact codec error" {
    var frame: [64]u8 = undefined;
    const frame_length = try make_ping_frame(&frame);
    frame[0] ^= 0xff;
    var reader = try reader_t.init(c_t.InteropFromHost);
    const result = try reader.feed(frame[0..HeaderBytes]);
    try std_t.testing.expectEqual(HeaderBytes, result.consumed);
    try std_t.testing.expect(!result.frame_ready);
    try std_t.testing.expectEqual(c_t.InteropErrorProtocolViolation, result.header_error);
    try std_t.testing.expectEqual(c_t.InteropErrorProtocolViolation, reader.header_error);
    try std_t.testing.expectError(error.ReaderFailed, reader.feed(frame[HeaderBytes..frame_length]));
    try std_t.testing.expectError(error.ReaderFailed, reader.peek());
    try std_t.testing.expectError(error.ReaderFailed, reader.consume());
    var valid_frame: [64]u8 = undefined;
    const valid_length = try make_ping_frame(&valid_frame);
    var reconnected = try reader_t.init(c_t.InteropFromHost);
    const fresh = try reconnected.feed(valid_frame[0..valid_length]);
    try std_t.testing.expect(fresh.frame_ready);
    try std_t.testing.expectEqual(c_t.InteropErrorNone, (try reconnected.peek()).payload_validation);
}

test "initialization rejects unsupported producer directions" {
    try std_t.testing.expectError(error.InvalidDirection, reader_t.init(0));
    try std_t.testing.expectError(error.InvalidDirection, reader_t.init(4));
    const reader = try reader_t.init(c_t.InteropFromLocal);
    try std_t.testing.expectEqual(@as(usize, 0), reader.received);
    try std_t.testing.expectEqual(@as(usize, 0), reader.frame_length);
    try std_t.testing.expect(!reader.failed and !reader.input_closed);
    try std_t.testing.expectEqual(c_t.InteropErrorNone, reader.header_error);
    try std_t.testing.expectEqual(@as(u8, 0), reader.buffer[0]);
    try std_t.testing.expectEqual(@as(u8, 0), reader.buffer[MaxFrameBytes - 1]);
}

test "oversize and unaligned payload headers are rejected by codec" {
    for ([_]u32{ c_t.InteropMaxPayloadBytes + 4, 2 }) |bad_length| {
        var frame: [HeaderBytes]u8 = undefined;
        encode_header_for_test(&frame, bad_length, c_t.InteropMsgPing);
        var reader = try reader_t.init(c_t.InteropFromHost);
        const result = try reader.feed(&frame);
        try std_t.testing.expectEqual(c_t.InteropErrorProtocolViolation, result.header_error);
        try std_t.testing.expect(reader.failed);
    }
}

test "input alias is rejected before any reader state changes" {
    var reader = try reader_t.init(c_t.InteropFromHost);
    try std_t.testing.expectError(error.InputAliasesReader, reader.feed(reader.buffer[0..1]));
    try std_t.testing.expectEqual(@as(usize, 0), reader.received);
    const header_bytes = std_t.mem.asBytes(&reader.header);
    try std_t.testing.expectError(error.InputAliasesReader, reader.feed(header_bytes[0..1]));
    try std_t.testing.expectEqual(@as(usize, 0), reader.received);
}

test "EOF distinguishes partial header body and pending complete frame" {
    var frame: [64]u8 = undefined;
    const frame_length = try make_ping_frame(&frame);
    var header_reader = try reader_t.init(c_t.InteropFromHost);
    _ = try header_reader.feed(frame[0 .. HeaderBytes - 1]);
    try std_t.testing.expectError(error.TruncatedFrame, header_reader.finish_input());
    try std_t.testing.expectError(error.ReaderFailed, header_reader.feed(frame[HeaderBytes - 1 .. HeaderBytes]));

    var body_reader = try reader_t.init(c_t.InteropFromHost);
    var payload_frame: [HeaderBytes + 4]u8 = undefined;
    encode_header_for_test(&payload_frame, 4, c_t.InteropMsgPing);
    @memset(payload_frame[HeaderBytes..], 0);
    _ = try body_reader.feed(payload_frame[0 .. HeaderBytes + 2]);
    try std_t.testing.expectError(error.TruncatedFrame, body_reader.finish_input());

    var pending_reader = try reader_t.init(c_t.InteropFromHost);
    _ = try pending_reader.feed(frame[0..frame_length]);
    try std_t.testing.expectError(error.FramePending, pending_reader.finish_input());
    try std_t.testing.expectEqual(frame_length, pending_reader.received);
    try pending_reader.consume();
    try pending_reader.finish_input();
    try std_t.testing.expectError(error.InputClosed, pending_reader.feed(frame[0..1]));
}

test "payload schema and producer direction errors are returned without hiding frame" {
    var malformed: [HeaderBytes + 4]u8 = undefined;
    encode_header_for_test(&malformed, 4, c_t.InteropMsgPing);
    @memset(malformed[HeaderBytes..], 0);
    var host_reader = try reader_t.init(c_t.InteropFromHost);
    _ = try host_reader.feed(&malformed);
    try std_t.testing.expectEqual(c_t.InteropErrorMalformedFrame, (try host_reader.peek()).payload_validation);

    var valid: [64]u8 = undefined;
    const length = try make_ping_frame(&valid);
    var guest_reader = try reader_t.init(c_t.InteropFromGuest);
    _ = try guest_reader.feed(valid[0..length]);
    const view = try guest_reader.peek();
    try std_t.testing.expectEqual(c_t.InteropErrorUnsupportedType, view.payload_validation);
    try std_t.testing.expectEqual(length, view.frame.len);
}

test "deterministic header mutation corpus never exceeds fixed storage" {
    var base: [64]u8 = undefined;
    const length = try make_ping_frame(&base);
    var seed: u32 = 0x6d2b79f5;
    for (0..512) |iteration| {
        var mutated = base;
        seed = seed *% 1664525 +% 1013904223;
        const byte_index: usize = seed % HeaderBytes;
        seed = seed *% 1664525 +% 1013904223;
        mutated[byte_index] ^= @truncate(seed >> 24);
        var reader = try reader_t.init(c_t.InteropFromHost);
        const split = iteration % (HeaderBytes + 1);
        _ = try reader.feed(mutated[0..split]);
        if (!reader.failed) _ = try reader.feed(mutated[split..length]);
        try std_t.testing.expect(reader.received <= MaxFrameBytes);
        try std_t.testing.expect(reader.frame_length <= MaxFrameBytes);
        if (reader.frame_ready()) _ = try reader.peek();
    }
}

test "payload-only fragments preserve the boundary and clean EOF rejects later access" {
    // A real codec-valid optional extension on Ping gives a nonempty payload
    // without duplicating any production TLV parsing in this module.
    var frame: [HeaderBytes + 8]u8 = undefined;
    encode_header_for_test(&frame, 8, c_t.InteropMsgPing);
    std_t.mem.writeInt(u16, frame[16..18], 0x7ffe, .little);
    std_t.mem.writeInt(u16, frame[18..20], 4, .little);
    @memcpy(frame[20..24], "test");
    var reader = try reader_t.init(c_t.InteropFromHost);
    const header_result = try reader.feed(frame[0..HeaderBytes]);
    try std_t.testing.expectEqual(HeaderBytes, header_result.consumed);
    try std_t.testing.expect(!header_result.frame_ready);
    try std_t.testing.expectError(error.FrameNotReady, reader.peek());
    try std_t.testing.expectError(error.FrameNotReady, reader.consume());
    try std_t.testing.expectEqual(HeaderBytes, reader.received);
    const partial = try reader.feed(frame[HeaderBytes .. HeaderBytes + 2]);
    try std_t.testing.expectEqual(@as(usize, 2), partial.consumed);
    try std_t.testing.expect(!partial.frame_ready);
    const final = try reader.feed(frame[HeaderBytes + 2 ..]);
    try std_t.testing.expectEqual(@as(usize, 6), final.consumed);
    try std_t.testing.expect(final.frame_ready);
    const view = try reader.peek();
    try std_t.testing.expectEqual(c_t.InteropErrorNone, view.payload_validation);
    try std_t.testing.expectEqualSlices(u8, &frame, view.frame);
    try reader.consume();
    try reader.finish_input();
    try std_t.testing.expectError(error.InputClosed, reader.peek());
    try std_t.testing.expectError(error.InputClosed, reader.consume());
}

test "failed header refuses every consumer and EOF operation" {
    var frame: [HeaderBytes]u8 = undefined;
    encode_header_for_test(&frame, 0, c_t.InteropMsgPing);
    frame[0] ^= 1;
    var reader = try reader_t.init(c_t.InteropFromHost);
    const result = try reader.feed(&frame);
    try std_t.testing.expectEqual(c_t.InteropErrorProtocolViolation, result.header_error);
    try std_t.testing.expect(!result.frame_ready);
    try std_t.testing.expectError(error.ReaderFailed, reader.peek());
    try std_t.testing.expectError(error.ReaderFailed, reader.consume());
    try std_t.testing.expectError(error.ReaderFailed, reader.finish_input());
    try std_t.testing.expectEqual(c_t.InteropErrorProtocolViolation, reader.header_error);
}
