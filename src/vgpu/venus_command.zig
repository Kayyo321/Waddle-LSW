//! Allocation-free exclusive private command/reply owner; see venus_command.h.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_command.h");
});
const PrefixBytes: usize = 36;
const MaxBytes: usize = 16777216;
const MaxChunkBytes: usize = 4096;
const exchange_t = *const fn (
    ?*anyopaque,
    [*c]const c.venus_request_t,
    ?*const anyopaque,
    usize,
    [*c]c.venus_request_t,
    ?*anyopaque,
    usize,
) callconv(.C) c_int;

fn overlaps(a: usize, a_bytes: usize, b: usize, b_bytes: usize) bool {
    if (a_bytes > std.math.maxInt(usize) - a or b_bytes > std.math.maxInt(usize) - b)
        return true;
    return a < b + b_bytes and b < a + a_bytes;
}
fn live(owner: *const c.venus_command_t) bool {
    return owner.exchange != null and owner.context != null;
}
fn fail(owner: *c.venus_command_t, status: c_int) c_int {
    owner.state = c.CommandLost;
    owner.lost = status;
    return status;
}
fn request(
    owner: *c.venus_command_t,
    kind: u32,
    offset: usize,
    input: ?[]const u8,
    output: ?[]u8,
    fence: ?*u64,
) c_int {
    var offered = std.mem.zeroes(c.venus_request_t);
    offered.kind = kind;
    offered.argument_zero = offset;
    offered.payload_bytes = if (input) |bytes| @intCast(bytes.len) else 0;
    if (output) |bytes| offered.argument_one = bytes.len;
    var response = std.mem.zeroes(c.venus_request_t);
    const status = owner.exchange.?(
        owner.context,
        &offered,
        if (input) |bytes| bytes.ptr else null,
        offered.payload_bytes,
        &response,
        if (output) |bytes| bytes.ptr else null,
        if (output) |bytes| bytes.len else 0,
    );
    if (status != c.RingOk) return status;
    if (response.kind != kind or response.direction != 1 or response.status != 0 or
        response.resource_id != 0 or response.flags != 0 or response.argument_one != 0 or
        response.payload_bytes != (if (output) |bytes| bytes.len else 0) or
        (fence == null and response.argument_zero != 0)) return c.RingCorrupt;
    if (fence) |value| value.* = response.argument_zero;
    return c.RingOk;
}

/// Initialize borrowed owner; exact C ABI lifetime/bounds/threads documented in header.
export fn venus_command_init(
    optional_owner: ?*c.venus_command_t,
    exchange: ?exchange_t,
    context: ?*anyopaque,
    tx: ?[*]u8,
    tx_bytes: usize,
    rx: ?[*]u8,
    rx_bytes: usize,
) c_int {
    const owner = optional_owner orelse return c.RingInvalid;
    const empty = std.mem.zeroes(c.venus_command_t);
    if (!std.meta.eql(owner.*, empty) or exchange == null or context == null or
        tx == null or rx == null or tx_bytes < 44 or tx_bytes > MaxBytes or
        rx_bytes < 4 or rx_bytes > MaxBytes or tx_bytes % 4 != 0 or
        rx_bytes % 4 != 0) return c.RingInvalid;
    const address = @intFromPtr(owner);
    if (overlaps(address, @sizeOf(c.venus_command_t), @intFromPtr(tx.?), tx_bytes) or
        overlaps(address, @sizeOf(c.venus_command_t), @intFromPtr(rx.?), rx_bytes) or
        overlaps(@intFromPtr(tx.?), tx_bytes, @intFromPtr(rx.?), rx_bytes)) return c.RingInvalid;
    owner.* = empty;
    owner.exchange = exchange;
    owner.context = context;
    owner.tx = tx.?;
    owner.rx = rx.?;
    owner.tx_bytes = tx_bytes;
    owner.rx_bytes = rx_bytes;
    return c.RingOk;
}

/// Publish one private serialized command; header defines ownership/errors/threads.
export fn venus_command_start(
    optional_owner: ?*c.venus_command_t,
    optional_bytes: ?[*]const u8,
    length: usize,
) c_int {
    const owner = optional_owner orelse return c.RingInvalid;
    if (!live(owner)) return c.RingInvalid;
    if (owner.state == c.CommandLost) return owner.lost;
    if (owner.state != c.CommandIdle) return c.RingAgain;
    const bytes = optional_bytes orelse return c.RingInvalid;
    if (length < 8 or length % 4 != 0 or length > owner.tx_bytes - PrefixBytes or
        overlaps(@intFromPtr(bytes), length, @intFromPtr(owner), @sizeOf(c.venus_command_t)) or
        overlaps(@intFromPtr(bytes), length, @intFromPtr(owner.tx), owner.tx_bytes) or
        overlaps(@intFromPtr(bytes), length, @intFromPtr(owner.rx), owner.rx_bytes))
        return c.RingInvalid;
    if (std.mem.readInt(u32, bytes[4..8], .little) != 1) return c.RingInvalid;
    const tx = owner.tx[0..owner.tx_bytes];
    std.mem.writeInt(u32, tx[0..4], 178, .little);
    std.mem.writeInt(u32, tx[4..8], 0, .little);
    std.mem.writeInt(u64, tx[8..16], 1, .little);
    std.mem.writeInt(u32, tx[16..20], 1, .little);
    std.mem.writeInt(u64, tx[20..28], 0, .little);
    std.mem.writeInt(u64, tx[28..36], owner.rx_bytes, .little);
    @memcpy(tx[PrefixBytes..][0..length], bytes[0..length]);
    var fence: u64 = 0;
    const status = request(owner, c.RequestSubmit, 0, tx[0 .. PrefixBytes + length], null, &fence);
    if (status != c.RingOk) {
        if (status == c.RingAgain or status == c.RingInvalid or status == c.RingLimit)
            return status;
        return fail(owner, status);
    }
    if (fence == 0) return fail(owner, c.RingCorrupt);
    owner.reply_offset = 0;
    owner.cpu_fence = fence;
    owner.command_id = std.mem.readInt(u32, bytes[0..4], .little);
    owner.state = c.CommandSubmitted;
    return c.RingOk;
}

/// Acquire one CPU reply; header defines retry/terminal behavior and GPU boundary.
export fn venus_command_poll(optional_owner: ?*c.venus_command_t) c_int {
    const owner = optional_owner orelse return c.RingInvalid;
    if (!live(owner)) return c.RingInvalid;
    if (owner.state == c.CommandLost) return owner.lost;
    if (owner.state == c.CommandSubmitted) {
        const status = request(owner, c.RequestPoll, 0, null, null, null);
        if (status == c.RingAgain) return status;
        if (status != c.RingOk) return fail(owner, status);
        owner.state = c.CommandReading;
    } else if (owner.state != c.CommandReading) return c.RingInvalid;
    // Sole-owner transitions keep Reading's cursor strictly below the extent.
    std.debug.assert(owner.reply_offset < owner.rx_bytes);
    const length = @min(MaxChunkBytes, owner.rx_bytes - owner.reply_offset);
    const status = request(owner, c.RequestReply, owner.reply_offset, null, owner.rx[owner.reply_offset..][0..length], null);
    if (status == c.RingAgain) return status;
    if (status != c.RingOk) return fail(owner, status);
    owner.reply_offset += length;
    if (owner.reply_offset < owner.rx_bytes) return c.RingAgain;
    if (std.mem.readInt(u32, owner.rx[0..4], .little) != owner.command_id)
        return fail(owner, c.RingCorrupt);
    owner.state = c.CommandReady;
    return c.RingOk;
}

/// Consume borrowed private view once; header defines nullable output/error lifetime.
export fn venus_command_take(
    optional_owner: ?*c.venus_command_t,
    bytes: ?*?*const anyopaque,
    length: ?*usize,
) c_int {
    // Output arguments must be disjoint from owner, its buffers and each other.
    if (bytes == null or length == null) return c.RingInvalid;
    bytes.?.* = null;
    length.?.* = 0;
    const owner = optional_owner orelse return c.RingInvalid;
    if (!live(owner)) return c.RingInvalid;
    if (owner.state == c.CommandLost) return owner.lost;
    if (owner.state == c.CommandSubmitted or owner.state == c.CommandReading) return c.RingAgain;
    if (owner.state != c.CommandReady) return c.RingInvalid;
    bytes.?.* = owner.rx;
    length.?.* = owner.rx_bytes;
    owner.state = c.CommandIdle;
    owner.reply_offset = 0;
    owner.cpu_fence = 0;
    owner.command_id = 0;
    return c.RingOk;
}

/// Reset without transport calls/allocation; pending session must already be abandoned.
export fn venus_command_free(owner: ?*c.venus_command_t) void {
    if (owner) |value| value.* = std.mem.zeroes(c.venus_command_t);
}

const fixture_t = struct {
    kind: u32 = 0,
    calls: usize = 0,
    status: c_int = 0,
    fence: u64 = 1,
    reply_id: u32 = 137,
    corrupt: u32 = 0,
    fault_kind: u32 = 0,
    payload: [128]u8 = undefined,
    payload_bytes: usize = 0,
    fn exchange(
        context: ?*anyopaque,
        offered: [*c]const c.venus_request_t,
        input: ?*const anyopaque,
        length: usize,
        response: [*c]c.venus_request_t,
        output: ?*anyopaque,
        capacity: usize,
    ) callconv(.C) c_int {
        const self: *fixture_t = @ptrCast(@alignCast(context.?));
        self.calls += 1;
        self.kind = offered.*.kind;
        if (length != 0) {
            const bytes: [*]const u8 = @ptrCast(input.?);
            @memcpy(self.payload[0..length], bytes[0..length]);
            self.payload_bytes = length;
        }
        const inject = self.fault_kind == 0 or self.fault_kind == offered.*.kind;
        if (self.status != 0 and inject) return self.status;
        response.* = std.mem.zeroes(c.venus_request_t);
        response.*.kind = offered.*.kind;
        response.*.direction = 1;
        response.*.payload_bytes = @intCast(capacity);
        if (offered.*.kind == c.RequestSubmit) response.*.argument_zero = self.fence;
        if (offered.*.kind == c.RequestReply) {
            const bytes: [*]u8 = @ptrCast(output.?);
            @memset(bytes[0..capacity], 0);
            std.mem.writeInt(u32, bytes[0..4], self.reply_id, .little);
        }
        switch (if (inject) self.corrupt else 0) {
            1 => response.*.kind += 1,
            2 => response.*.direction = 0,
            3 => response.*.status = 1,
            4 => response.*.resource_id = 2,
            5 => response.*.flags = 1,
            6 => response.*.argument_one = 1,
            7 => response.*.payload_bytes += 1,
            8 => response.*.argument_zero = 999,
            else => {},
        }
        return 0;
    }
};
const VersionCommand = [_]u8{ 137, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 };

test "private prefix, exact-once reply and 128 allocation-free state cycles" {
    var tx: [128]u8 = undefined;
    var rx: [32]u8 = undefined;
    var owner = std.mem.zeroes(c.venus_command_t);
    var fixture = fixture_t{};
    try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
    }));
    var view: ?*const anyopaque = null;
    var length: usize = 1;
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_poll, .{&owner}));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_take, .{
        &owner, &view, &length,
    }));
    for (0..128) |index| {
        fixture.fence = index + 1;
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, VersionCommand.len,
        }));
        try std.testing.expectEqual(@as(u32, c.CommandSubmitted), owner.state);
        try std.testing.expectEqual(index + 1, owner.cpu_fence);
        try std.testing.expectEqual(@as(usize, 52), fixture.payload_bytes);
        try std.testing.expectEqualSlices(u8, &VersionCommand, fixture.payload[36..52]);
        try std.testing.expectEqual(@as(u32, 178), std.mem.readInt(
            u32,
            fixture.payload[0..4],
            .little,
        ));
        try std.testing.expectEqual(@as(u64, 1), std.mem.readInt(
            u64,
            fixture.payload[8..16],
            .little,
        ));
        try std.testing.expectEqual(@as(u64, rx.len), std.mem.readInt(
            u64,
            fixture.payload[28..36],
            .little,
        ));
        try std.testing.expectEqual(c.RingAgain, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, 16,
        }));
        try std.testing.expectEqual(c.RingAgain, @call(.never_inline, venus_command_take, .{
            &owner, &view, &length,
        }));
        fixture.status = c.RingAgain;
        try std.testing.expectEqual(
            c.RingAgain,
            @call(.never_inline, venus_command_poll, .{&owner}),
        );
        try std.testing.expectEqual(@as(u32, c.RequestPoll), fixture.kind);
        fixture.status = 0;
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_poll, .{&owner}));
        try std.testing.expectEqual(@as(u32, c.CommandReady), owner.state);
        try std.testing.expectEqual(
            c.RingInvalid,
            @call(.never_inline, venus_command_poll, .{&owner}),
        );
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_take, .{
            &owner, &view, &length,
        }));
        try std.testing.expectEqual(@as(usize, rx.len), length);
        try std.testing.expectEqual(@intFromPtr(&rx), @intFromPtr(view.?));
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_take, .{
            &owner, &view, &length,
        }));
        try std.testing.expectEqual(@as(usize, 0), length);
        try std.testing.expect(view == null);
    }
    @call(.never_inline, venus_command_free, .{&owner});
    @call(.never_inline, venus_command_free, .{&owner});
    @call(.never_inline, venus_command_free, .{null});
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_start, .{
        &owner, &VersionCommand, 16,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_poll, .{&owner}));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_take, .{
        &owner, &view, &length,
    }));
}

test "initialization bounds and every pairwise alias reject without changes" {
    var tx: [128]u8 = undefined;
    var rx: [32]u8 = undefined;
    var owner = std.mem.zeroes(c.venus_command_t);
    var fixture = fixture_t{};
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        null, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
    }));
    const extents = [_][2]usize{
        .{ 0, 32 },  .{ 40, 32 }, .{ 45, 32 }, .{ MaxBytes + 4, 32 },
        .{ 128, 0 }, .{ 128, 3 }, .{ 128, 5 }, .{ 128, MaxBytes + 4 },
    };
    for (extents) |extent| try std.testing.expectEqual(
        c.RingInvalid,
        @call(.never_inline, venus_command_init, .{
            &owner, fixture_t.exchange, &fixture, &tx, extent[0], &rx, extent[1],
        }),
    );
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, null, &fixture, &tx, tx.len, &rx, rx.len,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, null, &tx, tx.len, &rx, rx.len,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, null, tx.len, &rx, rx.len,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, &tx, tx.len, null, rx.len,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, @as([*]u8, @ptrCast(&owner)), 80, &rx, rx.len,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, &tx, tx.len, @as([*]u8, @ptrCast(&owner)), 80,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, &tx, tx.len, &tx, 32,
    }));
    const overflow: [*]u8 = @ptrFromInt(std.math.maxInt(usize) - 3);
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, &tx, tx.len, overflow, 32,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, overflow, 128, &rx, rx.len,
    }));
    try std.testing.expect(std.meta.eql(owner, std.mem.zeroes(c.venus_command_t)));
    owner.state = c.CommandSubmitted;
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
    }));
}

test "invalid start buffers and missing output arguments do not publish" {
    var tx: [128]u8 = undefined;
    var rx: [32]u8 = undefined;
    var owner = std.mem.zeroes(c.venus_command_t);
    var fixture = fixture_t{};
    try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{
        &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_start, .{
        null, &VersionCommand, 16,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_start, .{
        &owner, null, 16,
    }));
    for ([_]usize{
        0, 4, 9, 96,
    }) |extent|
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, extent,
        }));
    for ([_][*]const u8{
        &tx, &rx, @as([*]u8, @ptrCast(&owner)),
    }) |bytes|
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_start, .{
            &owner, bytes, 8,
        }));
    var bad = VersionCommand;
    bad[4] = 0;
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_start, .{
        &owner, &bad, bad.len,
    }));
    var view: ?*const anyopaque = null;
    var length: usize = 1;
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_poll, .{null}));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_take, .{
        &owner, null, &length,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_take, .{
        &owner, &view, null,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_take, .{
        null, &view, &length,
    }));
    try std.testing.expectEqual(@as(usize, 0), fixture.calls);
}

test "submission retry, terminal statuses, zero fence and malformed reply shapes" {
    var tx: [128]u8 = undefined;
    var rx: [32]u8 = undefined;
    var owner = std.mem.zeroes(c.venus_command_t);
    var fixture = fixture_t{};
    const Statuses = [_]c_int{
        c.RingAgain,  c.RingInvalid,   c.RingLimit,   c.RingCorrupt,
        c.RingClosed, c.RingCancelled, c.RingTimeout,
    };
    for (Statuses) |status| {
        @call(.never_inline, venus_command_free, .{&owner});
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{
            &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
        }));
        fixture.status = status;
        try std.testing.expectEqual(status, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, 16,
        }));
        if (status == c.RingAgain or status == c.RingInvalid or status == c.RingLimit) {
            try std.testing.expectEqual(@as(u32, c.CommandIdle), owner.state);
        } else {
            try std.testing.expectEqual(@as(u32, c.CommandLost), owner.state);
            try std.testing.expectEqual(status, @call(.never_inline, venus_command_start, .{
                &owner, &VersionCommand, 16,
            }));
            try std.testing.expectEqual(
                status,
                @call(.never_inline, venus_command_poll, .{&owner}),
            );
            var view: ?*const anyopaque = null;
            var length: usize = 0;
            try std.testing.expectEqual(status, @call(.never_inline, venus_command_take, .{
                &owner, &view, &length,
            }));
        }
    }
    fixture.status = 0;
    for (0..8) |corrupt| {
        @call(.never_inline, venus_command_free, .{&owner});
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{
            &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
        }));
        fixture.corrupt = @intCast(corrupt);
        fixture.fence = 0;
        try std.testing.expectEqual(c.RingCorrupt, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, 16,
        }));
    }
    fixture.corrupt = 0;
    fixture.fence = 1;
    for (0..10) |corrupt| {
        @call(.never_inline, venus_command_free, .{&owner});
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{
            &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
        }));
        fixture.corrupt = 0;
        fixture.reply_id = 137;
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, 16,
        }));
        if (corrupt < 8) fixture.corrupt = @intCast(corrupt + 1) else {
            fixture.reply_id = 138;
            if (corrupt == 9) fixture.status = c.RingClosed;
        }
        const expected = if (corrupt == 9) c.RingClosed else c.RingCorrupt;
        try std.testing.expectEqual(expected, @call(.never_inline, venus_command_poll, .{&owner}));
        try std.testing.expectEqual(@as(u32, c.CommandLost), owner.state);
    }
}

test "reply retries retain accepted CPU fence and every reply failure is sticky" {
    var tx: [128]u8 = undefined;
    var rx: [32]u8 = undefined;
    var owner = std.mem.zeroes(c.venus_command_t);
    var fixture = fixture_t{ .fault_kind = c.RequestReply };
    const Statuses = [_]c_int{
        c.RingAgain,  c.RingInvalid,   c.RingLimit,   c.RingCorrupt,
        c.RingClosed, c.RingCancelled, c.RingTimeout,
    };
    for (Statuses) |status| {
        @call(.never_inline, venus_command_free, .{&owner});
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{
            &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
        }));
        fixture.status = 0;
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, 16,
        }));
        fixture.status = status;
        try std.testing.expectEqual(
            status,
            @call(.never_inline, venus_command_poll, .{&owner}),
        );
        try std.testing.expectEqual(@as(u32, c.RequestReply), fixture.kind);
        try std.testing.expectEqual(@as(u64, 1), owner.cpu_fence);
        if (status == c.RingAgain) {
            try std.testing.expectEqual(@as(u32, c.CommandReading), owner.state);
            fixture.status = 0;
            try std.testing.expectEqual(
                c.RingOk,
                @call(.never_inline, venus_command_poll, .{&owner}),
            );
        } else try std.testing.expectEqual(@as(u32, c.CommandLost), owner.state);
    }
    fixture.status = 0;
    for (1..9) |corrupt| {
        @call(.never_inline, venus_command_free, .{&owner});
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{
            &owner, fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len,
        }));
        fixture.corrupt = 0;
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_start, .{
            &owner, &VersionCommand, 16,
        }));
        fixture.corrupt = @intCast(corrupt);
        try std.testing.expectEqual(
            c.RingCorrupt,
            @call(.never_inline, venus_command_poll, .{&owner}),
        );
    }
}

test "callback without its borrowed context rejects before owner or transport mutation" {
    var owner = std.mem.zeroes(c.venus_command_t);
    owner.exchange = fixture_t.exchange;
    const before = owner;
    var view: ?*const anyopaque = &VersionCommand;
    var length: usize = VersionCommand.len;
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_start, .{
        &owner, &VersionCommand, VersionCommand.len,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_poll, .{&owner}));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_command_take, .{
        &owner, &view, &length,
    }));
    try std.testing.expectEqualDeep(before, owner);
    try std.testing.expect(view == null);
    try std.testing.expectEqual(@as(usize, 0), length);
    @call(.never_inline, venus_command_free, .{&owner});
    try std.testing.expectEqualDeep(std.mem.zeroes(c.venus_command_t), owner);
}

const chunk_fixture_t = struct {
    extent: usize,
    accepted: usize = 0,
    polls: usize = 0,
    attempts: usize = 0,
    pause_offset: ?usize = null,
    pause_pending: bool = true,
    fault_offset: ?usize = null,
    fault_status: c_int = c.RingOk,
    corrupt: u8 = 0,
    wrong_identity: bool = false,

    fn byte_at(index: usize) u8 {
        return if (index < 4) (if (index == 0) 137 else 0) else @truncate(index * 7 + 3);
    }
    fn exchange(context: ?*anyopaque, offered: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
        const self: *chunk_fixture_t = @ptrCast(@alignCast(context.?));
        response.* = std.mem.zeroes(c.venus_request_t);
        response.*.kind = offered.*.kind;
        response.*.direction = 1;
        if (offered.*.kind == c.RequestSubmit) {
            std.debug.assert(length == 52 and output == null and capacity == 0);
            const bytes: [*]const u8 = @ptrCast(input.?);
            std.debug.assert(std.mem.readInt(u64, bytes[28..36], .little) == self.extent);
            response.*.argument_zero = 7;
            return c.RingOk;
        }
        std.debug.assert(input == null and length == 0);
        if (offered.*.kind == c.RequestPoll) {
            self.polls += 1;
            std.debug.assert(output == null and capacity == 0 and offered.*.argument_zero == 0);
            return c.RingOk;
        }
        std.debug.assert(offered.*.kind == c.RequestReply and output != null and
            offered.*.argument_zero == self.accepted and offered.*.argument_one == capacity and
            capacity == @min(@as(usize, 4096), self.extent - self.accepted));
        self.attempts += 1;
        const bytes: [*]u8 = @ptrCast(output.?);
        if (self.pause_offset == self.accepted and self.pause_pending) {
            self.pause_pending = false;
            @memset(bytes[0..capacity], 0xa5);
            return c.RingAgain;
        }
        if (self.fault_offset == self.accepted and self.fault_status != c.RingOk) {
            @memset(bytes[0..capacity], 0xa5);
            return self.fault_status;
        }
        for (bytes[0..capacity], self.accepted..) |*byte, index| byte.* = byte_at(index);
        if (self.wrong_identity and self.accepted == 0) bytes[0] = 99;
        response.*.payload_bytes = @intCast(capacity);
        switch (if (self.fault_offset == self.accepted) self.corrupt else 0) {
            1 => response.*.kind += 1,
            2 => response.*.direction = 0,
            3 => response.*.status = 1,
            4 => response.*.resource_id = 2,
            5 => response.*.flags = 1,
            6 => response.*.argument_one = 1,
            7 => response.*.payload_bytes += 1,
            8 => response.*.argument_zero = 999,
            else => {},
        }
        self.accepted += capacity;
        return c.RingOk;
    }
};

test "full reply byte ranges cover exact small and maximum extents without adjacent writes" {
    for ([_]usize{ 4, 4096, 4100, 8192, 274460, MaxBytes }) |extent| {
        const storage = try std.testing.allocator.alloc(u8, extent + 16);
        defer std.testing.allocator.free(storage);
        @memset(storage, 0xcc);
        var tx: [128]u8 = undefined;
        var owner = std.mem.zeroes(c.venus_command_t);
        var fixture = chunk_fixture_t{ .extent = extent };
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{ &owner, chunk_fixture_t.exchange, &fixture, &tx, tx.len, storage.ptr + 8, extent }));
        defer @call(.never_inline, venus_command_free, .{&owner});
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_start, .{ &owner, &VersionCommand, VersionCommand.len }));
        const reads = (extent + 4095) / 4096;
        for (0..reads) |index| {
            try std.testing.expectEqual(if (index + 1 == reads) c.RingOk else c.RingAgain, @call(.never_inline, venus_command_poll, .{&owner}));
            try std.testing.expectEqual(@min((index + 1) * 4096, extent), owner.reply_offset);
        }
        try std.testing.expectEqual(@as(usize, 1), fixture.polls);
        try std.testing.expectEqual(reads, fixture.attempts);
        for (storage[8..][0..extent], 0..) |byte, index|
            try std.testing.expectEqual(chunk_fixture_t.byte_at(index), byte);
        try std.testing.expectEqualSlices(u8, &([_]u8{0xcc} ** 8), storage[0..8]);
        try std.testing.expectEqualSlices(u8, &([_]u8{0xcc} ** 8), storage[extent + 8 ..]);
        var view: ?*const anyopaque = null;
        var length: usize = 0;
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_take, .{ &owner, &view, &length }));
        try std.testing.expectEqual(@intFromPtr(storage.ptr + 8), @intFromPtr(view.?));
        try std.testing.expectEqual(extent, length);
        try std.testing.expectEqual(@as(usize, 0), owner.reply_offset);
    }
}

test "Again at first middle last byte chunks retains cursor and hides partial staging" {
    for ([_]usize{ 0, 4096, 8192 }) |offset| {
        var tx: [128]u8 = undefined;
        var rx: [8200]u8 = undefined;
        var owner = std.mem.zeroes(c.venus_command_t);
        var fixture = chunk_fixture_t{ .extent = rx.len, .pause_offset = offset };
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{ &owner, chunk_fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len }));
        defer @call(.never_inline, venus_command_free, .{&owner});
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_start, .{ &owner, &VersionCommand, VersionCommand.len }));
        for (0..offset / 4096) |_| try std.testing.expectEqual(c.RingAgain, @call(.never_inline, venus_command_poll, .{&owner}));
        try std.testing.expectEqual(c.RingAgain, @call(.never_inline, venus_command_poll, .{&owner}));
        try std.testing.expectEqual(@as(u32, c.CommandReading), owner.state);
        try std.testing.expectEqual(offset, owner.reply_offset);
        try std.testing.expectEqual(@as(u64, 7), owner.cpu_fence);
        const attempts = fixture.attempts;
        try std.testing.expectEqual(c.RingAgain, @call(.never_inline, venus_command_start, .{ &owner, &VersionCommand, VersionCommand.len }));
        var view: ?*const anyopaque = &VersionCommand;
        var length: usize = VersionCommand.len;
        try std.testing.expectEqual(c.RingAgain, @call(.never_inline, venus_command_take, .{ &owner, &view, &length }));
        try std.testing.expect(view == null and length == 0);
        try std.testing.expectEqual(attempts, fixture.attempts);
        const remaining = (rx.len - offset + 4095) / 4096;
        for (0..remaining) |index| try std.testing.expectEqual(if (index + 1 == remaining) c.RingOk else c.RingAgain, @call(.never_inline, venus_command_poll, .{&owner}));
        try std.testing.expectEqual(@as(usize, 1), fixture.polls);
        try std.testing.expectEqual(@as(usize, 4), fixture.attempts);
        for (rx, 0..) |byte, index| try std.testing.expectEqual(chunk_fixture_t.byte_at(index), byte);
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_take, .{ &owner, &view, &length }));
    }
}

test "malformed and terminal first middle last chunks retain sticky ownership" {
    for ([_]usize{ 0, 4096, 8192 }) |offset| {
        for (0..@as(usize, if (offset == 0) 15 else 14)) |fault| {
            var tx: [128]u8 = undefined;
            var rx: [8200]u8 = undefined;
            var owner = std.mem.zeroes(c.venus_command_t);
            const Statuses = [_]c_int{ c.RingInvalid, c.RingLimit, c.RingCorrupt, c.RingClosed, c.RingCancelled, c.RingTimeout };
            const status: c_int = if (fault < 6) Statuses[fault] else c.RingCorrupt;
            var fixture = chunk_fixture_t{ .extent = rx.len, .fault_offset = offset, .fault_status = if (fault < 6) status else c.RingOk, .corrupt = if (fault >= 6 and fault < 14) @intCast(fault - 5) else 0, .wrong_identity = fault == 14 };
            try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_init, .{ &owner, chunk_fixture_t.exchange, &fixture, &tx, tx.len, &rx, rx.len }));
            defer @call(.never_inline, venus_command_free, .{&owner});
            try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_command_start, .{ &owner, &VersionCommand, VersionCommand.len }));
            var observed: c_int = c.RingAgain;
            for (0..3) |_| {
                observed = @call(.never_inline, venus_command_poll, .{&owner});
                if (observed != c.RingAgain) break;
            }
            try std.testing.expectEqual(status, observed);
            try std.testing.expectEqual(@as(u32, c.CommandLost), owner.state);
            try std.testing.expectEqual(if (fault == 14) rx.len else offset, owner.reply_offset);
            const attempts = fixture.attempts;
            try std.testing.expectEqual(status, @call(.never_inline, venus_command_poll, .{&owner}));
            try std.testing.expectEqual(status, @call(.never_inline, venus_command_start, .{ &owner, &VersionCommand, VersionCommand.len }));
            var view: ?*const anyopaque = &VersionCommand;
            var length: usize = VersionCommand.len;
            try std.testing.expectEqual(status, @call(.never_inline, venus_command_take, .{ &owner, &view, &length }));
            try std.testing.expect(view == null and length == 0);
            try std.testing.expectEqual(attempts, fixture.attempts);
        }
    }
}
