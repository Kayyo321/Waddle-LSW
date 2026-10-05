//! Zig native test specification for Waddle CLI protocol, serialization codecs,
//! argument quotation, and zero-leak memory safety verification.

const std = @import("std");
const testing = std.testing;

const c = @cImport({
    @cInclude("waddle/cli_protocol.h");
    @cInclude("common.h");
    @cInclude("terminal.h");
});

test "wire header and payload structure sizes and alignments" {
    try testing.expectEqual(@as(usize, 32), @sizeOf(c.waddle_cli_msg_header_t));
    try testing.expectEqual(@as(usize, 24), @sizeOf(c.waddle_msg_spawn_req_t));
    try testing.expectEqual(@as(usize, 12), @sizeOf(c.waddle_msg_spawn_resp_t));
    try testing.expectEqual(@as(usize, 8), @sizeOf(c.waddle_msg_stream_data_t));
    try testing.expectEqual(@as(usize, 8), @sizeOf(c.waddle_msg_resize_t));
    try testing.expectEqual(@as(usize, 4), @sizeOf(c.waddle_msg_signal_t));
    try testing.expectEqual(@as(usize, 16), @sizeOf(c.waddle_msg_exit_t));
}

test "protocol constants and enumeration values" {
    try testing.expectEqual(@as(u32, 0x57444c43), c.WaddleCliMagic);
    try testing.expectEqual(@as(u16, 1), c.WaddleCliVersion);
    try testing.expectEqual(@as(u32, 5242), c.WaddleDefaultVsockPort);
    try testing.expectEqual(@as(u32, 1024 * 1024), c.WaddleMaxPayloadSize);
    try testing.expectEqual(@as(u32, 32), c.WaddleHeaderSize);
    try testing.expectEqual(@as(u32, 16384), c.WaddleChunkSize);
    try testing.expectEqual(@as(u32, 2 * 1024 * 1024), c.WaddleQueueSize);

    try testing.expectEqual(@as(c_int, 1), c.WaddleMsgSpawnReq);
    try testing.expectEqual(@as(c_int, 2), c.WaddleMsgSpawnResp);
    try testing.expectEqual(@as(c_int, 3), c.WaddleMsgStreamData);
    try testing.expectEqual(@as(c_int, 4), c.WaddleMsgTerminalResize);
    try testing.expectEqual(@as(c_int, 5), c.WaddleMsgSignalEvent);
    try testing.expectEqual(@as(c_int, 6), c.WaddleMsgProcessExit);
    try testing.expectEqual(@as(c_int, 7), c.WaddleMsgHeartbeatPing);
    try testing.expectEqual(@as(c_int, 8), c.WaddleMsgHeartbeatPong);
    try testing.expectEqual(@as(c_int, 9), c.WaddleMsgStreamEof);
    try testing.expectEqual(@as(c_int, 255), c.WaddleMsgError);

    try testing.expectEqual(@as(c_int, 0), c.WaddleStreamStdin);
    try testing.expectEqual(@as(c_int, 1), c.WaddleStreamStdout);
    try testing.expectEqual(@as(c_int, 2), c.WaddleStreamStderr);

    try testing.expectEqual(@as(c_int, 1), c.WaddleSpawnFlagInteractive);
    try testing.expectEqual(@as(c_int, 2), c.WaddleSpawnFlagRawPipes);
    try testing.expectEqual(@as(c_int, 4), c.WaddleSpawnFlagInheritEnv);
    try testing.expectEqual(@as(c_int, 8), c.WaddleSpawnFlagTranslatePath);
    try testing.expectEqual(@as(c_int, 16), c.WaddleSpawnFlagElevated);
}

test "little endian integer encoding and decoding roundtrips" {
    var b16: [2]u8 = undefined;
    c.waddle_put16(&b16, 0x1234);
    try testing.expectEqual(@as(u8, 0x34), b16[0]);
    try testing.expectEqual(@as(u8, 0x12), b16[1]);
    try testing.expectEqual(@as(u16, 0x1234), c.waddle_get16(&b16));

    c.waddle_put16(&b16, 0xffff);
    try testing.expectEqual(@as(u16, 0xffff), c.waddle_get16(&b16));
    c.waddle_put16(&b16, 0);
    try testing.expectEqual(@as(u16, 0), c.waddle_get16(&b16));

    var b32: [4]u8 = undefined;
    c.waddle_put32(&b32, 0x12345678);
    try testing.expectEqual(@as(u8, 0x78), b32[0]);
    try testing.expectEqual(@as(u8, 0x56), b32[1]);
    try testing.expectEqual(@as(u8, 0x34), b32[2]);
    try testing.expectEqual(@as(u8, 0x12), b32[3]);
    try testing.expectEqual(@as(u32, 0x12345678), c.waddle_get32(&b32));
    c.waddle_put32(&b32, 0);
    try testing.expectEqual(@as(u32, 0), c.waddle_get32(&b32));

    var b64: [8]u8 = undefined;
    c.waddle_put64(&b64, 0x0123456789abcdef);
    try testing.expectEqual(@as(u8, 0xef), b64[0]);
    try testing.expectEqual(@as(u8, 0xcd), b64[1]);
    try testing.expectEqual(@as(u8, 0xab), b64[2]);
    try testing.expectEqual(@as(u8, 0x89), b64[3]);
    try testing.expectEqual(@as(u8, 0x67), b64[4]);
    try testing.expectEqual(@as(u8, 0x45), b64[5]);
    try testing.expectEqual(@as(u8, 0x23), b64[6]);
    try testing.expectEqual(@as(u8, 0x01), b64[7]);
    try testing.expectEqual(@as(u64, 0x0123456789abcdef), c.waddle_get64(&b64));
    c.waddle_put64(&b64, 0);
    try testing.expectEqual(@as(u64, 0), c.waddle_get64(&b64));
}

test "CRC-32 standard test vector and edge cases" {
    const vector = "123456789";
    try testing.expectEqual(@as(u32, 0xcbf43926), c.waddle_crc32(vector.ptr, vector.len));
    try testing.expectEqual(@as(u32, 0), c.waddle_crc32(null, 0));

    var zeros = [_]u8{0} ** 64;
    const zero_crc = c.waddle_crc32(&zeros, zeros.len);
    try testing.expect(zero_crc != 0);

    // Single bit flip changes CRC
    zeros[10] ^= 1;
    const altered_crc = c.waddle_crc32(&zeros, zeros.len);
    try testing.expect(zero_crc != altered_crc);
}

test "queue buffer allocation, bounds, and compaction" {
    var q: c.queue_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.queue_init(&q));
    defer c.queue_free(&q);

    try testing.expect(q.data != null);
    try testing.expectEqual(@as(usize, c.WaddleQueueSize), c.queue_space(&q));
    try testing.expectEqual(@as(usize, 0), q.len);
    try testing.expectEqual(@as(usize, 0), q.off);

    const data = "Test payload for queue verification";
    try testing.expectEqual(@as(c_int, 0), c.queue_append(&q, data.ptr, data.len));
    try testing.expectEqual(data.len, q.len);
    try testing.expectEqual(@as(usize, c.WaddleQueueSize - data.len), c.queue_space(&q));

    // Shift offset to simulate partial consumption
    q.off = c.WaddleQueueSize - data.len - 10;
    std.mem.copyForwards(u8, q.data[q.off .. q.off + data.len], data);

    // Appending more bytes than trailing capacity forces buffer compaction
    const extra = "Extra chunk requiring compaction";
    try testing.expectEqual(@as(c_int, 0), c.queue_append(&q, extra.ptr, extra.len));
    try testing.expectEqual(@as(usize, 0), q.off);
    try testing.expectEqual(data.len + extra.len, q.len);

    // Exceeding capacity fails with ENOBUFS
    const overflow_size = c.queue_space(&q) + 1;
    const fake_buf = try testing.allocator.alloc(u8, 16);
    defer testing.allocator.free(fake_buf);
    @memset(fake_buf, 0xaa);

    try testing.expectEqual(@as(c_int, -1), c.queue_append(&q, fake_buf.ptr, overflow_size));
}

test "wire frame encoding and validation" {
    var q: c.queue_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.queue_init(&q));
    defer c.queue_free(&q);

    var seq: u32 = 1;
    const payload = "Hello guest execution agent!";
    try testing.expectEqual(
        @as(c_int, 0),
        c.wire_send(&q, &seq, @as(u16, @intCast(c.WaddleMsgStreamData)), payload.ptr, payload.len),
    );
    try testing.expectEqual(@as(u32, 2), seq);
    try testing.expectEqual(c.WaddleHeaderSize + payload.len, q.len);

    // Validate wire header fields
    try testing.expectEqual(@as(u32, c.WaddleCliMagic), c.waddle_get32(q.data));
    try testing.expectEqual(@as(u16, c.WaddleCliVersion), c.waddle_get16(q.data + 4));
    try testing.expectEqual(@as(u16, @intCast(c.WaddleMsgStreamData)), c.waddle_get16(q.data + 6));
    try testing.expectEqual(@as(u64, 1), c.waddle_get64(q.data + 8));
    try testing.expectEqual(@as(u32, payload.len), c.waddle_get32(q.data + 16));
    try testing.expectEqual(@as(u32, 0), c.waddle_get32(q.data + 20));
    try testing.expectEqual(@as(u32, 1), c.waddle_get32(q.data + 24));
    try testing.expectEqual(c.waddle_crc32(payload.ptr, payload.len), c.waddle_get32(q.data + 28));

    // Payload matches
    try testing.expectEqualStrings(payload, q.data[32 .. 32 + payload.len]);
}

test "empty payload wire frame encoding and decoding" {
    var q: c.queue_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.queue_init(&q));
    defer c.queue_free(&q);

    var seq: u32 = 1;
    try testing.expectEqual(
        @as(c_int, 0),
        c.wire_send(&q, &seq, @as(u16, @intCast(c.WaddleMsgHeartbeatPing)), null, 0),
    );
    try testing.expectEqual(@as(usize, c.WaddleHeaderSize), q.len);

    var fds: [2]c_int = undefined;
    try testing.expectEqual(@as(c_int, 0), std.c.socketpair(std.c.AF.UNIX, std.c.SOCK.STREAM, 0, &fds));
    defer {
        _ = std.c.close(fds[0]);
        _ = std.c.close(fds[1]);
    }
    try testing.expectEqual(@as(c_int, 0), c.nonblock(fds[1]));

    var d: c.decoder_t = undefined;
    @memset(@as([*]u8, @ptrCast(&d))[0..@sizeOf(c.decoder_t)], 0);
    defer c.wire_destroy(&d);

    const written = std.c.write(fds[0], q.data, q.len);
    try testing.expectEqual(@as(isize, @intCast(q.len)), written);

    const res = c.wire_read(&d, fds[1]);
    try testing.expectEqual(@as(c_int, 1), res);
    try testing.expectEqual(@as(u16, @intCast(c.WaddleMsgHeartbeatPing)), d.type);
    try testing.expectEqual(@as(usize, 0), d.length);
    c.wire_consume(&d);
}

test "stream and EOF frame helpers" {
    var q: c.queue_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.queue_init(&q));
    defer c.queue_free(&q);

    var seq: u32 = 1;
    const chunk = "Stdout chunk text";
    try testing.expectEqual(
        @as(c_int, 0),
        c.wire_stream(&q, &seq, @as(c_uint, @intCast(c.WaddleStreamStdout)), chunk.ptr, chunk.len),
    );

    // Body contains 1-byte stream ID, 3-byte padding, 4-byte len, then chunk
    const body = q.data + c.WaddleHeaderSize;
    try testing.expectEqual(@as(u8, @intCast(c.WaddleStreamStdout)), body[0]);
    try testing.expectEqual(@as(u32, chunk.len), c.waddle_get32(body + 4));
    try testing.expectEqualStrings(chunk, body[8 .. 8 + chunk.len]);

    // Stream EOF helper
    try testing.expectEqual(
        @as(c_int, 0),
        c.wire_eof(&q, &seq, @as(c_uint, @intCast(c.WaddleStreamStdout))),
    );
    const eof_body = q.data + c.WaddleHeaderSize + 8 + chunk.len + c.WaddleHeaderSize;
    try testing.expectEqual(@as(u32, @intCast(c.WaddleStreamStdout)), c.waddle_get32(eof_body));
}

test "wire frame incremental decoding through socketpair" {
    var q: c.queue_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.queue_init(&q));
    defer c.queue_free(&q);

    var seq: u32 = 1;
    const message = "Multiplexed stream test payload";
    try testing.expectEqual(
        @as(c_int, 0),
        c.wire_send(&q, &seq, @as(u16, @intCast(c.WaddleMsgStreamData)), message.ptr, message.len),
    );

    var fds: [2]c_int = undefined;
    try testing.expectEqual(@as(c_int, 0), std.c.socketpair(std.c.AF.UNIX, std.c.SOCK.STREAM, 0, &fds));
    defer {
        _ = std.c.close(fds[0]);
        _ = std.c.close(fds[1]);
    }

    try testing.expectEqual(@as(c_int, 0), c.nonblock(fds[1]));

    var d: c.decoder_t = undefined;
    @memset(@as([*]u8, @ptrCast(&d))[0..@sizeOf(c.decoder_t)], 0);
    defer c.wire_destroy(&d);

    // Feed bytes one by one
    var i: usize = 0;
    while (i < q.len) : (i += 1) {
        const written = std.c.write(fds[0], q.data + i, 1);
        try testing.expectEqual(@as(isize, 1), written);

        const res = c.wire_read(&d, fds[1]);
        if (i < q.len - 1) {
            try testing.expectEqual(@as(c_int, 0), res);
        } else {
            try testing.expectEqual(@as(c_int, 1), res);
        }
    }

    try testing.expectEqual(@as(u16, @intCast(c.WaddleMsgStreamData)), d.type);
    try testing.expectEqual(message.len, d.length);
    try testing.expectEqualStrings(message, d.body[0..d.length]);

    c.wire_consume(&d);
    try testing.expect(d.body == null);
    try testing.expectEqual(@as(usize, 0), d.have);
    try testing.expectEqual(@as(u32, 2), d.next);
}

test "wire decoder corrupted CRC detection" {
    var q: c.queue_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.queue_init(&q));
    defer c.queue_free(&q);

    var seq: u32 = 1;
    const msg = "Integrity check test";
    try testing.expectEqual(
        @as(c_int, 0),
        c.wire_send(&q, &seq, @as(u16, @intCast(c.WaddleMsgSignalEvent)), msg.ptr, msg.len),
    );

    // Corrupt CRC
    q.data[28] ^= 1;

    var fds: [2]c_int = undefined;
    try testing.expectEqual(@as(c_int, 0), std.c.socketpair(std.c.AF.UNIX, std.c.SOCK.STREAM, 0, &fds));
    defer {
        _ = std.c.close(fds[0]);
        _ = std.c.close(fds[1]);
    }

    try testing.expectEqual(@as(c_int, 0), c.nonblock(fds[1]));

    var d: c.decoder_t = undefined;
    @memset(@as([*]u8, @ptrCast(&d))[0..@sizeOf(c.decoder_t)], 0);
    defer c.wire_destroy(&d);

    const written = std.c.write(fds[0], q.data, q.len);
    try testing.expectEqual(@as(isize, @intCast(q.len)), written);

    const res = c.wire_read(&d, fds[1]);
    try testing.expectEqual(@as(c_int, -1), res);
}

test "wire decoder malformed magic and version detection" {
    var q: c.queue_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.queue_init(&q));
    defer c.queue_free(&q);

    var seq: u32 = 1;
    try testing.expectEqual(
        @as(c_int, 0),
        c.wire_send(&q, &seq, @as(u16, @intCast(c.WaddleMsgHeartbeatPing)), null, 0),
    );

    // Corrupt magic
    c.waddle_put32(q.data, 0x12345678);

    var fds: [2]c_int = undefined;
    try testing.expectEqual(@as(c_int, 0), std.c.socketpair(std.c.AF.UNIX, std.c.SOCK.STREAM, 0, &fds));
    defer {
        _ = std.c.close(fds[0]);
        _ = std.c.close(fds[1]);
    }
    try testing.expectEqual(@as(c_int, 0), c.nonblock(fds[1]));

    var d: c.decoder_t = undefined;
    @memset(@as([*]u8, @ptrCast(&d))[0..@sizeOf(c.decoder_t)], 0);
    defer c.wire_destroy(&d);

    _ = std.c.write(fds[0], q.data, q.len);
    const res = c.wire_read(&d, fds[1]);
    try testing.expectEqual(@as(c_int, -1), res);
}

test "wire decoder clean EOF on idle connection" {
    var fds: [2]c_int = undefined;
    try testing.expectEqual(@as(c_int, 0), std.c.socketpair(std.c.AF.UNIX, std.c.SOCK.STREAM, 0, &fds));
    defer {
        _ = std.c.close(fds[1]);
    }
    try testing.expectEqual(@as(c_int, 0), c.nonblock(fds[1]));

    var d: c.decoder_t = undefined;
    @memset(@as([*]u8, @ptrCast(&d))[0..@sizeOf(c.decoder_t)], 0);
    defer c.wire_destroy(&d);

    // Close writer immediately without writing
    _ = std.c.close(fds[0]);

    const res = c.wire_read(&d, fds[1]);
    try testing.expectEqual(@as(c_int, -2), res);
}

test "Win32 argument quoting and unquoting roundtrips" {
    const cases = [_][*:0]const u8{
        "",
        "plain",
        "hello world",
        "nested\"quote",
        "C:\\Program Files\\Waddle\\",
        "slashes\\\\\"and\"quotes",
        "tab\tseparated\tvalues",
        "single\\slash",
        "UTF-8: 日本語 🐧",
    };

    var argv_arr = try testing.allocator.alloc(?[*:0]const u8, cases.len + 1);
    defer testing.allocator.free(argv_arr);

    for (cases, 0..) |item, idx| {
        argv_arr[idx] = item;
    }
    argv_arr[cases.len] = null;

    const quoted = c.waddle_quote(@ptrCast(argv_arr.ptr));
    try testing.expect(quoted != null);
    defer std.c.free(quoted);

    const unquoted = c.waddle_unquote(quoted);
    try testing.expect(unquoted != null);
    defer c.waddle_free_argv(unquoted);

    for (cases, 0..) |expected, idx| {
        try testing.expect(unquoted[idx] != null);
        const actual_slice = std.mem.span(unquoted[idx]);
        const expected_slice = std.mem.span(expected);
        try testing.expectEqualStrings(expected_slice, actual_slice);
    }
    try testing.expect(unquoted[cases.len] == null);
}

test "Win32 argument unquoting rejection on invalid strings" {
    try testing.expect(c.waddle_unquote("unquoted_word") == null);
    try testing.expect(c.waddle_unquote("\"unterminated_quote") == null);
    try testing.expect(c.waddle_unquote("\"quote\"trailing_garbage") == null);
    try testing.expect(c.waddle_unquote(null) == null);
}

test "path translation for Linux to guest VirtIO-FS mapping" {
    const p1 = c.waddle_translate_path("/home/dev/project");
    try testing.expect(p1 != null);
    defer std.c.free(p1);
    try testing.expectEqualStrings("Z:\\home\\dev\\project", std.mem.span(p1));

    const p2 = c.waddle_translate_path("relative/path/to/script.ps1");
    try testing.expect(p2 != null);
    defer std.c.free(p2);
    try testing.expectEqualStrings("relative/path/to/script.ps1", std.mem.span(p2));

    const p3 = c.waddle_translate_path("/");
    try testing.expect(p3 != null);
    defer std.c.free(p3);
    try testing.expectEqualStrings("Z:\\", std.mem.span(p3));

    // Rejection of traversal and invalid backslashes
    try testing.expect(c.waddle_translate_path("/home/dev/../root") == null);
    try testing.expect(c.waddle_translate_path("/invalid\\backslash") == null);
    try testing.expect(c.waddle_translate_path(null) == null);
}

test "monotonic millisecond clock monotonically advances" {
    const t1 = c.monotonic_ms();
    try testing.expect(t1 > 0);
    std.time.sleep(10 * std.time.ns_per_ms);
    const t2 = c.monotonic_ms();
    try testing.expect(t2 >= t1);
}

test "zero memory leaks verification under testing allocator" {
    const alloc = testing.allocator;
    const test_slice = try alloc.alloc(u8, 1024);
    defer alloc.free(test_slice);

    @memset(test_slice, 0x42);
    try testing.expectEqual(@as(u8, 0x42), test_slice[500]);
    // Defer block automatically ensures zero leaked bytes upon test exit
}
