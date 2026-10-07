//! Allocation-free exclusive private command/reply owner; see venus_command.h.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_command.h");
});
const PrefixBytes: usize = 36;
const MaxBytes: usize = 16777216;
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
    input: ?[]const u8,
    output: ?[]u8,
    fence: ?*u64,
) c_int {
    var offered = std.mem.zeroes(c.venus_request_t);
    offered.kind = kind;
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
    const status = request(owner, c.RequestSubmit, tx[0 .. PrefixBytes + length], null, &fence);
    if (status != c.RingOk) {
        if (status == c.RingAgain or status == c.RingInvalid or status == c.RingLimit)
            return status;
        return fail(owner, status);
    }
    if (fence == 0) return fail(owner, c.RingCorrupt);
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
    if (owner.state != c.CommandSubmitted) return c.RingInvalid;
    var status = request(owner, c.RequestPoll, null, null, null);
    if (status == c.RingAgain) return status;
    if (status != c.RingOk) return fail(owner, status);
    status = request(owner, c.RequestReply, null, owner.rx[0..owner.rx_bytes], null);
    if (status == c.RingAgain) return status;
    if (status != c.RingOk) return fail(owner, status);
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
    if (owner.state == c.CommandSubmitted) return c.RingAgain;
    if (owner.state != c.CommandReady) return c.RingInvalid;
    bytes.?.* = owner.rx;
    length.?.* = owner.rx_bytes;
    owner.state = c.CommandIdle;
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
    const statuses = [_]c_int{
        c.RingAgain,  c.RingInvalid,   c.RingLimit,   c.RingCorrupt,
        c.RingClosed, c.RingCancelled, c.RingTimeout,
    };
    for (statuses) |status| {
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
    const statuses = [_]c_int{
        c.RingAgain,  c.RingInvalid,   c.RingLimit,   c.RingCorrupt,
        c.RingClosed, c.RingCancelled, c.RingTimeout,
    };
    for (statuses) |status| {
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
            try std.testing.expectEqual(@as(u32, c.CommandSubmitted), owner.state);
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
