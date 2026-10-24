//! Fixed-capacity, owning FIFO for validated protocol frames.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

/// Maximum number of queued frames in one direction.
pub const QueueCapacity: usize = c.InteropPeerQueueFrames;
/// Slots reserved for admission of required control traffic.
pub const ReservedControlSlots: usize = 8;
/// Maximum queued data-class frames.
pub const DataCapacity: usize = QueueCapacity - ReservedControlSlots;
/// Maximum bytes in one owned protocol frame.
pub const MaxFrameBytes: usize = c.InteropMaxFrameBytes;
/// Fixed protocol header length.
pub const HeaderBytes: usize = c.InteropHeaderBytes;

/// Admission class chosen by the session caller.
/// @ownership Enum value is copied by value.
pub const frame_class_t = enum { Data, Control };

/// One owned queued frame record.
/// @ownership Queue owns bytes and frees them on full acknowledgement/deinit.
const frame_record_t = struct {
    bytes: []u8,
};

/// Caller-owned serialized FIFO for one producer direction.
/// @ownership Borrows allocator until deinit; owns each copied accepted frame.
/// @thread_safety All calls require the same serialized event-loop thread.
pub const queue_t = struct {
    /// Allocator borrowed until deinit; it must outlive every queued frame.
    allocator: std.mem.Allocator,
    /// Producer direction used for codec payload validation.
    direction: c.interop_direction_t,
    /// Fixed ring of optional owned frame records.
    frames: [QueueCapacity]?frame_record_t = [_]?frame_record_t{null} ** QueueCapacity,
    /// Index of oldest outstanding frame.
    head: usize = 0,
    /// Number of outstanding frames.
    count: usize = 0,
    /// Bytes already acknowledged from the current front frame.
    front_write_offset: usize = 0,
    /// Total unsent bytes across all frames.
    pending_bytes: usize = 0,
    /// False after idempotent deinit.
    live: bool = true,

    /// Initialize an empty queue for one frozen producer direction.
    /// @param allocator in Allocator borrowed until deinit; it must remain valid.
    /// @param direction in Producer direction Guest/Host/Local.
    /// @return Empty queue with fixed records and no heap allocation.
    /// @errors InvalidDirection for a value outside the frozen direction set.
    /// @ownership Returned queue is caller-owned by value.
    /// @thread_safety Use and deinit from one serialized event-loop thread.
    pub fn init(allocator: std.mem.Allocator, direction: c.interop_direction_t) error{InvalidDirection}!queue_t {
        if (direction != c.InteropFromGuest and direction != c.InteropFromHost and direction != c.InteropFromLocal) return error.InvalidDirection;
        return .{ .allocator = allocator, .direction = direction };
    }

    /// Validate and copy exactly one complete frame into the FIFO.
    /// @param self in/out Live queue; unchanged on every failure.
    /// @param frame in Accessible borrowed bytes, never retained by reference.
    /// @param class in Caller-selected Data or Control admission class.
    /// @return Void after the queue owns a validated byte-for-byte copy.
    /// @errors QueueClosed, InvalidFrame, InvalidPayload, UnsupportedFrame,
    /// DataCapacityReserved, QueueFull, or OutOfMemory.
    /// @ownership One allocation transfers to the queue only after full copy.
    /// @thread_safety Caller serializes calls with peek/consume_written/deinit.
    pub fn enqueue(self: *queue_t, frame: []const u8, class: frame_class_t) error{
        QueueClosed,
        InvalidFrame,
        InvalidPayload,
        UnsupportedFrame,
        DataCapacityReserved,
        QueueFull,
        OutOfMemory,
    }!void {
        if (!self.live) return error.QueueClosed;
        if (frame.len < HeaderBytes or frame.len > MaxFrameBytes) return error.InvalidFrame;
        var header: c.interop_header_t = undefined;
        const header_result = c.interop_header_decode(frame.ptr, HeaderBytes, &header);
        if (header_result != c.InteropErrorNone) return error.InvalidFrame;
        const complete_length = HeaderBytes + @as(usize, @intCast(header.length));
        if (complete_length != frame.len) return error.InvalidFrame;
        const payload = frame[HeaderBytes..];
        const payload_ptr: [*c]const u8 = if (payload.len == 0) null else payload.ptr;
        const payload_result = c.interop_payload_validate(header.type, self.direction, payload_ptr, payload.len);
        if (payload_result == c.InteropErrorUnsupportedType or payload_result == c.InteropErrorUnsupportedField) return error.UnsupportedFrame;
        if (payload_result != c.InteropErrorNone) return error.InvalidPayload;

        if (self.count == QueueCapacity) return error.QueueFull;
        if (class == .Data and self.count >= DataCapacity) return error.DataCapacityReserved;
        const copy = self.allocator.alloc(u8, frame.len) catch return error.OutOfMemory;
        @memcpy(copy, frame);
        const index = (self.head + self.count) % QueueCapacity;
        self.frames[index] = .{ .bytes = copy };
        self.count += 1;
        self.pending_bytes += frame.len;
    }

    /// Borrow only the remaining unsent bytes of the front frame.
    /// @param self in Live queue.
    /// @return Borrowed remaining front bytes, invalidated by any mutation.
    /// @errors QueueClosed when deinitialized, Empty when no frame is queued.
    /// @ownership Slice borrows queue-owned storage and must not outlive mutation.
    /// @thread_safety Caller serializes calls with all queue operations.
    pub fn peek(self: *const queue_t) error{ QueueClosed, Empty }![]const u8 {
        if (!self.live) return error.QueueClosed;
        if (self.count == 0) return error.Empty;
        const record = self.frames[self.head] orelse return error.Empty;
        return record.bytes[self.front_write_offset..];
    }

    /// Acknowledge bytes confirmed written by the transport.
    /// @param self in/out Live queue whose front frame is advanced.
    /// @param count in Number of confirmed bytes, at most remaining front bytes.
    /// @return Void; full acknowledgement frees/clears the record and advances FIFO.
    /// @errors QueueClosed, Empty, or InvalidCount without mutation.
    /// @ownership Frees a completed frame allocation exactly once.
    /// @thread_safety Caller serializes calls with enqueue/peek/deinit.
    pub fn consume_written(self: *queue_t, count: usize) error{ QueueClosed, Empty, InvalidCount }!void {
        if (!self.live) return error.QueueClosed;
        if (self.count == 0) return error.Empty;
        const record = self.frames[self.head] orelse return error.Empty;
        const remaining = record.bytes.len - self.front_write_offset;
        if (count > remaining) return error.InvalidCount;
        if (count == 0) return;
        self.pending_bytes -= count;
        self.front_write_offset += count;
        if (self.front_write_offset == record.bytes.len) {
            self.allocator.free(record.bytes);
            self.frames[self.head] = null;
            self.head = (self.head + 1) % QueueCapacity;
            self.count -= 1;
            self.front_write_offset = 0;
        }
    }

    /// Release every live frame and close the queue; safe to call repeatedly.
    /// @param self in/out Queue whose owned frames are released and records cleared.
    /// @return Void; all counters are zero and later mutating calls report QueueClosed.
    /// @ownership Frees all remaining frame allocations using the borrowed allocator.
    /// @thread_safety Caller ensures no operation or borrowed peek slice is active.
    pub fn deinit(self: *queue_t) void {
        if (!self.live) return;
        for (&self.frames) |*record| {
            if (record.*) |owned| self.allocator.free(owned.bytes);
            record.* = null;
        }
        self.head = 0;
        self.count = 0;
        self.front_write_offset = 0;
        self.pending_bytes = 0;
        self.live = false;
    }
};

// Test-only fixtures.
fn make_ping_frame(frame: []u8, request_id: u32) !usize {
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, frame.ptr, frame.len);
    var frame_length: usize = 0;
    const result = c.interop_writer_finish(&writer, c.InteropMsgPing, request_id, c.InteropFromHost, &frame_length);
    if (result != c.InteropErrorNone) return error.CodecFixtureFailure;
    return frame_length;
}

fn malformed_frame(frame: []u8, payload_length: u32) void {
    std.mem.writeInt(u32, frame[0..4], c.InteropMagic, .little);
    std.mem.writeInt(u16, frame[4..6], c.InteropVersion, .little);
    std.mem.writeInt(u16, frame[6..8], c.InteropMsgPing, .little);
    std.mem.writeInt(u32, frame[8..12], payload_length, .little);
    std.mem.writeInt(u32, frame[12..16], 1, .little);
}

fn allocation_sweep_probe(allocator: std.mem.Allocator, frame: []const u8) !void {
    var queue = try queue_t.init(allocator, c.InteropFromHost);
    defer queue.deinit();
    try queue.enqueue(frame, .Data);
}

test "enqueue owns copied frame and partial acknowledgements preserve exact FIFO bytes" {
    var queue = try queue_t.init(std.testing.allocator, c.InteropFromHost);
    defer queue.deinit();
    var first_frame: [64]u8 = undefined;
    const first_length = try make_ping_frame(&first_frame, 11);
    var second_frame: [64]u8 = undefined;
    const second_length = try make_ping_frame(&second_frame, 22);
    try queue.enqueue(first_frame[0..first_length], .Data);
    try queue.enqueue(second_frame[0..second_length], .Control);
    @memset(first_frame[0..first_length], 0xaa);
    const original = try make_ping_frame(&first_frame, 11);
    try std.testing.expectEqual(first_length, original);
    try std.testing.expectEqual(@as(usize, first_length + second_length), queue.pending_bytes);
    const first = try queue.peek();
    try std.testing.expectEqualSlices(u8, first_frame[0..first_length], first);
    try queue.consume_written(0);
    try std.testing.expectEqualSlices(u8, first_frame[0..first_length], try queue.peek());
    try queue.consume_written(5);
    try std.testing.expectEqual(@as(usize, first_length + second_length - 5), queue.pending_bytes);
    try std.testing.expectEqualSlices(u8, first_frame[5..first_length], try queue.peek());
    const remaining_before = try queue.peek();
    try std.testing.expectError(error.InvalidCount, queue.consume_written(remaining_before.len + 1));
    try std.testing.expectEqualSlices(u8, first_frame[5..first_length], try queue.peek());
    try queue.consume_written(remaining_before.len);
    try std.testing.expectEqualSlices(u8, second_frame[0..second_length], try queue.peek());
    try queue.consume_written(second_length);
    try std.testing.expectEqual(@as(usize, 0), queue.pending_bytes);
    try std.testing.expectError(error.Empty, queue.peek());
}

test "data reservation control admission full capacity and ring wrap preserve FIFO" {
    var queue = try queue_t.init(std.testing.allocator, c.InteropFromHost);
    defer queue.deinit();
    var frame: [64]u8 = undefined;
    for (0..DataCapacity) |index| {
        const length = try make_ping_frame(&frame, @intCast(index));
        try queue.enqueue(frame[0..length], .Data);
    }
    const before_reserved = queue.pending_bytes;
    const length = try make_ping_frame(&frame, 9000);
    try std.testing.expectError(error.DataCapacityReserved, queue.enqueue(frame[0..length], .Data));
    try std.testing.expectEqual(DataCapacity, queue.count);
    try std.testing.expectEqual(before_reserved, queue.pending_bytes);
    for (DataCapacity..QueueCapacity) |index| {
        const control_length = try make_ping_frame(&frame, @intCast(index));
        try queue.enqueue(frame[0..control_length], .Control);
    }
    try std.testing.expectEqual(QueueCapacity, queue.count);
    const before_full = queue.pending_bytes;
    try std.testing.expectError(error.QueueFull, queue.enqueue(frame[0..length], .Control));
    try std.testing.expectEqual(QueueCapacity, queue.count);
    try std.testing.expectEqual(before_full, queue.pending_bytes);
    for (0..300) |index| {
        const current = try queue.peek();
        const decoded_id = std.mem.readInt(u32, current[12..16], .little);
        try std.testing.expectEqual(@as(u32, @intCast(index)), decoded_id);
        try queue.consume_written(current.len);
    }
    for (QueueCapacity..QueueCapacity + 300) |index| {
        const control_length = try make_ping_frame(&frame, @intCast(index));
        try queue.enqueue(frame[0..control_length], .Control);
    }
    for (300..QueueCapacity + 300) |index| {
        const current = try queue.peek();
        const decoded_id = std.mem.readInt(u32, current[12..16], .little);
        try std.testing.expectEqual(@as(u32, @intCast(index)), decoded_id);
        try queue.consume_written(current.len);
    }
    try std.testing.expectEqual(@as(usize, 0), queue.count);
}

test "codec rejects malformed maximum-size and wrong-direction frames before allocation" {
    var invalid: [MaxFrameBytes]u8 = [_]u8{0} ** MaxFrameBytes;
    malformed_frame(&invalid, c.InteropMaxPayloadBytes);
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 0 });
    var queue = try queue_t.init(failing.allocator(), c.InteropFromHost);
    defer queue.deinit();
    try std.testing.expectError(error.InvalidPayload, queue.enqueue(&invalid, .Data));
    try std.testing.expectEqual(@as(usize, 0), queue.count);
    try std.testing.expectEqual(@as(usize, 0), queue.pending_bytes);
    try std.testing.expect(!failing.has_induced_failure);

    var ping: [64]u8 = undefined;
    const ping_length = try make_ping_frame(&ping, 1);
    var guest_queue = try queue_t.init(failing.allocator(), c.InteropFromGuest);
    defer guest_queue.deinit();
    try std.testing.expectError(error.UnsupportedFrame, guest_queue.enqueue(ping[0..ping_length], .Data));
    try std.testing.expect(!failing.has_induced_failure);
}

test "maximum valid optional extension frame is copied and acknowledged without truncation" {
    var queue = try queue_t.init(std.testing.allocator, c.InteropFromHost);
    defer queue.deinit();
    var frame: [MaxFrameBytes]u8 = [_]u8{0x5a} ** MaxFrameBytes;
    const extension_length = MaxFrameBytes - HeaderBytes - 4;
    // The writer deliberately only emits known tags. Construct a hypothetical
    // newer peer's unknown optional extension and validate it through enqueue.
    malformed_frame(&frame, c.InteropMaxPayloadBytes);
    std.mem.writeInt(u16, frame[HeaderBytes..][0..2], 0x7ffe, .little);
    std.mem.writeInt(u16, frame[HeaderBytes + 2 ..][0..2], extension_length, .little);
    @memset(frame[HeaderBytes + 4 ..], 0xa7);
    try queue.enqueue(&frame, .Control);
    try std.testing.expectEqual(MaxFrameBytes, queue.pending_bytes);
    try std.testing.expectEqualSlices(u8, &frame, try queue.peek());
    // Mutating the caller buffer must not change either queued boundary byte.
    @memset(&frame, 0);
    const copied = try queue.peek();
    try std.testing.expectEqual(@as(u8, 0xa7), copied[HeaderBytes + 4]);
    try std.testing.expectEqual(@as(u8, 0xa7), copied[MaxFrameBytes - 1]);
    try queue.consume_written(MaxFrameBytes - 1);
    try std.testing.expectEqual(@as(usize, 1), queue.pending_bytes);
    try std.testing.expectEqualSlices(u8, &[_]u8{0xa7}, try queue.peek());
    try queue.consume_written(1);
    try std.testing.expectEqual(@as(usize, 0), queue.count);
    try std.testing.expectEqual(@as(usize, 0), queue.pending_bytes);
}

test "enqueue requires one exact frame and rejects invalid header lengths" {
    var queue = try queue_t.init(std.testing.allocator, c.InteropFromHost);
    defer queue.deinit();
    var frame: [64]u8 = undefined;
    const length = try make_ping_frame(&frame, 7);
    try std.testing.expectError(error.InvalidFrame, queue.enqueue(frame[0 .. length - 1], .Data));
    try std.testing.expectError(error.InvalidFrame, queue.enqueue(frame[0 .. length + 1], .Data));
    malformed_frame(&frame, 4);
    try std.testing.expectError(error.InvalidFrame, queue.enqueue(frame[0..HeaderBytes], .Data));
    try std.testing.expectEqual(@as(usize, 0), queue.count);
}

test "allocation failure leaves existing frames offsets and counters unchanged" {
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 1 });
    var queue = try queue_t.init(failing.allocator(), c.InteropFromHost);
    defer queue.deinit();
    var first_frame: [64]u8 = undefined;
    const first_length = try make_ping_frame(&first_frame, 11);
    var second_frame: [64]u8 = undefined;
    const second_length = try make_ping_frame(&second_frame, 22);
    try queue.enqueue(first_frame[0..first_length], .Data);
    try queue.consume_written(4);
    const pending_before = queue.pending_bytes;
    const offset_before = queue.front_write_offset;
    try std.testing.expectError(error.OutOfMemory, queue.enqueue(second_frame[0..second_length], .Data));
    try std.testing.expectEqual(@as(usize, 1), queue.count);
    try std.testing.expectEqual(pending_before, queue.pending_bytes);
    try std.testing.expectEqual(offset_before, queue.front_write_offset);
    try std.testing.expectEqualSlices(u8, first_frame[4..first_length], try queue.peek());
}

test "single frame allocation is covered by an allocator failure sweep" {
    var frame: [64]u8 = undefined;
    const length = try make_ping_frame(&frame, 88);
    try std.testing.checkAllAllocationFailures(std.testing.allocator, allocation_sweep_probe, .{frame[0..length]});
}

test "deinit frees partial frames idempotently and closes future operations" {
    var queue = try queue_t.init(std.testing.allocator, c.InteropFromHost);
    var frame: [64]u8 = undefined;
    const length = try make_ping_frame(&frame, 3);
    try queue.enqueue(frame[0..length], .Data);
    try queue.consume_written(3);
    queue.deinit();
    queue.deinit();
    try std.testing.expectEqual(@as(usize, 0), queue.count);
    try std.testing.expectEqual(@as(usize, 0), queue.pending_bytes);
    try std.testing.expectError(error.QueueClosed, queue.enqueue(frame[0..length], .Control));
    try std.testing.expectError(error.QueueClosed, queue.peek());
    try std.testing.expectError(error.QueueClosed, queue.consume_written(0));
}

test "enqueue may borrow an already queued frame while copying another record" {
    var queue = try queue_t.init(std.testing.allocator, c.InteropFromHost);
    defer queue.deinit();
    var frame: [64]u8 = undefined;
    const length = try make_ping_frame(&frame, 41);
    try queue.enqueue(frame[0..length], .Data);
    const borrowed = try queue.peek();
    var expected: [64]u8 = undefined;
    const expected_length = try make_ping_frame(&expected, 41);
    try queue.enqueue(borrowed, .Control);
    try std.testing.expectEqual(@as(usize, 2), queue.count);
    try std.testing.expectEqualSlices(u8, expected[0..expected_length], try queue.peek());
    try queue.consume_written(length);
    try std.testing.expectEqualSlices(u8, expected[0..expected_length], try queue.peek());
}

test "deterministic queue stress preserves acknowledgement byte totals" {
    var queue = try queue_t.init(std.testing.allocator, c.InteropFromHost);
    defer queue.deinit();
    var frame: [64]u8 = undefined;
    var expected_pending: usize = 0;
    var seed: u32 = 0x9e3779b9;
    for (0..4096) |iteration| {
        seed = seed *% 1664525 +% 1013904223;
        if (queue.count == 0 or (queue.count < DataCapacity and seed & 1 == 0)) {
            const length = try make_ping_frame(&frame, @intCast(iteration));
            try queue.enqueue(frame[0..length], if (seed & 4 == 0) .Control else .Data);
            expected_pending += length;
        } else {
            const remaining = try queue.peek();
            const count = @min(remaining.len, @as(usize, (seed >> 8) % 19));
            try queue.consume_written(count);
            expected_pending -= count;
        }
        try std.testing.expectEqual(expected_pending, queue.pending_bytes);
        try std.testing.expect(queue.count <= QueueCapacity);
    }
}
