//! Bounded authenticated TCP hello/config guards; no fabricated receiver replies.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_tcp.h");
});
const Magic = "WDTCP001";
const ConfigScratchBytes = 65536;
fn strict_integer(value: std.json.Value) ?u32 {
    return switch (value) {
        .integer => |integer| if (integer > 0 and integer <= std.math.maxInt(u32)) @intCast(integer) else null,
        else => null,
    };
}
const config_json_t = struct {
    version: std.json.Value,
    port: std.json.Value,
    exchange_timeout_ms: std.json.Value,
    host: []const u8,
    token: []const u8,
    icd_path: []const u8,
};
const Profile = profile();
fn profile() [160]u8 {
    var bytes = [_]u8{0} ** 160;
    bytes[0] = 1;
    bytes[4] = 0x33;
    bytes[5] = 0x41;
    bytes[6] = 0x40;
    bytes[8] = 1;
    bytes[12] = 3;
    bytes[16] = 1;
    bytes[20] = 1;
    bytes[68] = 3;
    bytes[152] = 1;
    return bytes;
}
fn ascii(value: []const u8) bool {
    for (value) |byte| if (byte == 0 or byte > 127) return false;
    return true;
}
fn absolute_path(value: []const u8) bool {
    if (value.len < 3) return false;
    if (std.ascii.isAlphabetic(value[0]) and value[1] == ':' and value[2] == '\\') return true;
    if (!std.mem.startsWith(u8, value, "\\\\") or value[2] == '\\') return false;
    const slash = std.mem.indexOfScalarPos(u8, value, 2, '\\') orelse return false;
    return slash + 1 < value.len and value[slash + 1] != '\\';
}
fn hex(byte: u8) ?u8 {
    return switch (byte) {
        '0'...'9' => byte - '0',
        'a'...'f' => byte - 'a' + 10,
        else => null,
    };
}
/// Exact bounded filename ABI; header specifies byte extent, errors and borrowed lifetime.
export fn venus_tcp_windows_path_validate(input: ?[*]const u8, length: usize) c_int {
    const source = input orelse return c.RingInvalid;
    if (length == 0) return c.RingInvalid;
    if (length > c.VenusTcpMaxWindowsPathBytes) return c.RingLimit;
    const bytes = source[0 .. length - 1];
    if (source[length - 1] != 0 or bytes.len == 0 or !ascii(bytes) or !absolute_path(bytes)) return c.RingCorrupt;
    return c.RingOk;
}
/// Strict bounded config ABI; header specifies copied outputs, errors and ownership.
export fn venus_tcp_config_decode(config: ?*c.venus_tcp_config_t, input: ?[*]const u8, length: usize) c_int {
    const output = config orelse return c.RingInvalid;
    output.* = std.mem.zeroes(c.venus_tcp_config_t);
    const source = input orelse return c.RingInvalid;
    if (length == 0) return c.RingCorrupt;
    if (length > c.VenusTcpMaxConfigBytes) return c.RingLimit;
    const bytes = source[0..length];
    if (!ascii(bytes)) return c.RingCorrupt;
    var scratch: [ConfigScratchBytes]u8 = undefined;
    defer for (&scratch) |*byte| {
        @as(*volatile u8, @ptrCast(byte)).* = 0;
    };
    var fixed = std.heap.FixedBufferAllocator.init(&scratch);
    const parsed = std.json.parseFromSlice(config_json_t, fixed.allocator(), bytes, .{
        .duplicate_field_behavior = .@"error",
        .ignore_unknown_fields = false,
        .allocate = .alloc_always,
    }) catch |err| return if (err == error.OutOfMemory) c.RingLimit else c.RingCorrupt;
    defer parsed.deinit();
    const value = parsed.value;
    const version = strict_integer(value.version) orelse return c.RingCorrupt;
    const port = strict_integer(value.port) orelse return c.RingCorrupt;
    const timeout = strict_integer(value.exchange_timeout_ms) orelse return c.RingCorrupt;
    if (version != 1 or port > 65535 or timeout > 60000 or
        !(std.mem.eql(u8, value.host, "10.0.2.2") or std.mem.eql(u8, value.host, "127.0.0.1")) or
        value.token.len != 64 or value.icd_path.len >= output.icd_path.len or
        !ascii(value.icd_path) or !absolute_path(value.icd_path)) return c.RingCorrupt;
    var staged = std.mem.zeroes(c.venus_tcp_config_t);
    for (0..32) |index| {
        const high = hex(value.token[index * 2]) orelse return c.RingCorrupt;
        const low = hex(value.token[index * 2 + 1]) orelse return c.RingCorrupt;
        staged.token[index] = high * 16 + low;
    }
    staged.version = version;
    staged.port = port;
    staged.exchange_timeout_ms = timeout;
    @memcpy(staged.host[0..value.host.len], value.host);
    @memcpy(staged.icd_path[0..value.icd_path.len], value.icd_path);
    output.* = staged;
    return c.RingOk;
}
fn prefix(bytes: []u8) void {
    @memset(bytes, 0);
    @memcpy(bytes[0..8], Magic);
    std.mem.writeInt(u32, bytes[8..12], 1, .little);
    std.mem.writeInt(u32, bytes[12..16], @intCast(bytes.len), .little);
}
fn valid_prefix(bytes: []const u8) bool {
    return std.mem.eql(u8, bytes[0..8], Magic) and std.mem.readInt(u32, bytes[8..12], .little) == 1 and
        std.mem.readInt(u32, bytes[12..16], .little) == bytes.len;
}
fn zero(bytes: []const u8) bool {
    for (bytes) |byte| if (byte != 0) return false;
    return true;
}
/// Encode exactly128 borrowed hello bytes; header defines unchanged failure output.
export fn venus_tcp_client_hello_encode(hello: ?*const c.venus_tcp_client_hello_t, output: ?[*]u8, length: usize) c_int {
    const value = hello orelse return c.RingInvalid;
    const bytes = output orelse return c.RingInvalid;
    if (length != 128) return c.RingInvalid;
    prefix(bytes[0..128]);
    @memcpy(bytes[16..48], &value.token);
    @memcpy(bytes[48..64], &value.nonce);
    return c.RingOk;
}
/// Decode exactly128 immutable bytes; copied output zeroed on failure, no retention.
export fn venus_tcp_client_hello_decode(hello: ?*c.venus_tcp_client_hello_t, input: ?[*]const u8, length: usize) c_int {
    const value = hello orelse return c.RingInvalid;
    value.* = std.mem.zeroes(c.venus_tcp_client_hello_t);
    const bytes = input orelse return c.RingInvalid;
    if (length != 128 or !valid_prefix(bytes[0..128]) or !zero(bytes[64..128])) return c.RingCorrupt;
    @memcpy(&value.token, bytes[16..48]);
    @memcpy(&value.nonce, bytes[48..64]);
    return c.RingOk;
}
/// Encode exactly224 actual-capset hello bytes; semantics remain existing capset owner.
export fn venus_tcp_server_hello_encode(hello: ?*const c.venus_tcp_server_hello_t, output: ?[*]u8, length: usize) c_int {
    const value = hello orelse return c.RingInvalid;
    const bytes = output orelse return c.RingInvalid;
    if (length != 224 or value.session == 0) return c.RingInvalid;
    prefix(bytes[0..224]);
    std.mem.writeInt(u64, bytes[16..24], value.session, .little);
    @memcpy(bytes[24..40], &value.nonce);
    @memcpy(bytes[64..224], &value.capabilities);
    return c.RingOk;
}
/// Decode bounded actual-capset hello; caller separately matches nonce/capset.
export fn venus_tcp_server_hello_decode(hello: ?*c.venus_tcp_server_hello_t, input: ?[*]const u8, length: usize) c_int {
    const value = hello orelse return c.RingInvalid;
    value.* = std.mem.zeroes(c.venus_tcp_server_hello_t);
    const bytes = input orelse return c.RingInvalid;
    if (length != 224 or !valid_prefix(bytes[0..224]) or !zero(bytes[40..64])) return c.RingCorrupt;
    const session = std.mem.readInt(u64, bytes[16..24], .little);
    if (session == 0) return c.RingCorrupt;
    value.session = session;
    @memcpy(&value.nonce, bytes[24..40]);
    @memcpy(&value.capabilities, bytes[64..224]);
    return c.RingOk;
}
/// Encode exact32-byte session Ack; borrowed output unchanged on invalid input.
export fn venus_tcp_ack_encode(session: u64, output: ?[*]u8, length: usize) c_int {
    const bytes = output orelse return c.RingInvalid;
    if (length != 32 or session == 0) return c.RingInvalid;
    prefix(bytes[0..32]);
    std.mem.writeInt(u64, bytes[16..24], session, .little);
    return c.RingOk;
}
/// Decode exact32-byte session Ack, zeroing caller identity on every failure.
export fn venus_tcp_ack_decode(session: ?*u64, input: ?[*]const u8, length: usize) c_int {
    const value = session orelse return c.RingInvalid;
    value.* = 0;
    const bytes = input orelse return c.RingInvalid;
    if (length != 32 or !valid_prefix(bytes[0..32]) or !zero(bytes[24..32])) return c.RingCorrupt;
    const identity = std.mem.readInt(u64, bytes[16..24], .little);
    if (identity == 0) return c.RingCorrupt;
    value.* = identity;
    return c.RingOk;
}
/// Timing-safe equality; nullable32-byte borrowed inputs, no logged/retained secret.
export fn venus_tcp_token_equal(first: ?[*]const u8, second: ?[*]const u8) c_int {
    const a = first orelse return 0;
    const b = second orelse return 0;
    return @intFromBool(std.crypto.utils.timingSafeEql([32]u8, a[0..32].*, b[0..32].*));
}
/// Copy exact compiled160-byte negotiation declaration, allocation-free/thread-safe.
export fn venus_tcp_profile_encode(output: ?[*]u8, length: usize) c_int {
    const bytes = output orelse return c.RingInvalid;
    if (length != 160) return c.RingInvalid;
    @memcpy(bytes[0..160], &Profile);
    return c.RingOk;
}
/// Compare immutable exact profile; this is not a Vulkan feature advertisement.
export fn venus_tcp_profile_validate(input: ?[*]const u8, length: usize) c_int {
    const bytes = input orelse return c.RingInvalid;
    if (length != 160 or !std.mem.eql(u8, bytes[0..160], &Profile)) return c.RingCorrupt;
    return c.RingOk;
}
/// Stricter TCP profile ceiling after authoritative envelope validation.
export fn venus_tcp_request_limit(request: ?*const c.venus_request_t) c_int {
    const value = request orelse return c.RingInvalid;
    if (value.direction != 0 or value.kind < 2 or value.kind > 10) return c.RingInvalid;
    if (value.payload_bytes > 65620) return c.RingLimit;
    if ((value.kind == 3 or value.kind == 6 or value.kind == 7) and value.argument_one > 4096) return c.RingLimit;
    if (value.kind == 7 and value.payload_bytes > 4096) return c.RingLimit;
    return c.RingOk;
}
/// Response pairing/ceiling after authoritative envelope validation, no mutation.
export fn venus_tcp_response_limit(request: ?*const c.venus_request_t, response: ?*const c.venus_request_t) c_int {
    const wanted = request orelse return c.RingInvalid;
    const value = response orelse return c.RingInvalid;
    if (value.direction != 1 or value.kind != wanted.kind or value.sequence != wanted.sequence or value.status > 7) return c.RingCorrupt;
    if (value.payload_bytes > 4096) return c.RingLimit;
    if (value.status != 0 and value.payload_bytes != 0) return c.RingCorrupt;
    if (value.status == 0 and (wanted.kind == 3 or wanted.kind == 6) and value.payload_bytes != wanted.argument_one) return c.RingCorrupt;
    return c.RingOk;
}

// Test-only fixtures.
extern fn venus_tcp_wire_oracle(kind: c_uint, output: [*]u8, capacity: usize) c_int;
const TestSession: u64 = 0x1122334455667788;
const TestConfig = "{\"version\":1,\"port\":55987,\"exchange_timeout_ms\":60000,\"host\":\"10.0.2.2\",\"token\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"icd_path\":\"C:\\\\waddle\\\\driver.dll\"}";
fn client_fixture() c.venus_tcp_client_hello_t {
    var result: c.venus_tcp_client_hello_t = undefined;
    for (&result.token, 0..) |*byte, index| byte.* = @truncate(index * 7 + 3);
    for (&result.nonce, 0..) |*byte, index| byte.* = @truncate(index * 11 + 5);
    return result;
}
fn server_fixture() c.venus_tcp_server_hello_t {
    var result: c.venus_tcp_server_hello_t = undefined;
    result.session = TestSession;
    result.nonce = client_fixture().nonce;
    for (&result.capabilities, 0..) |*byte, index| byte.* = @truncate(index * 13 + 9);
    return result;
}
fn is_zero(value: anytype) !void {
    try std.testing.expect(zero(std.mem.asBytes(value)));
}
fn config_error(bytes: []const u8) !void {
    var output = std.mem.zeroes(c.venus_tcp_config_t);
    output.version = 99;
    try std.testing.expect(@call(.never_inline, venus_tcp_config_decode, .{ &output, bytes.ptr, bytes.len }) != c.RingOk);
    try is_zero(&output);
}
fn config_replace(old: []const u8, replacement: []const u8, good: bool) !void {
    const index = std.mem.indexOf(u8, TestConfig, old).?;
    const bytes = try std.mem.concat(std.testing.allocator, u8, &.{ TestConfig[0..index], replacement, TestConfig[index + old.len ..] });
    defer std.testing.allocator.free(bytes);
    if (!good) return config_error(bytes);
    var output: c.venus_tcp_config_t = undefined;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_config_decode, .{ &output, bytes.ptr, bytes.len }));
}
test "independent literal packets roundtrip every copied byte" {
    var client: [128]u8 = undefined;
    var server: [224]u8 = undefined;
    var ack: [32]u8 = undefined;
    var declared: [160]u8 = undefined;
    var oracle: [224]u8 = undefined;
    const input_client = client_fixture();
    const input_server = server_fixture();
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_client_hello_encode, .{ &input_client, &client, client.len }));
    try std.testing.expectEqual(@as(c_int, 1), venus_tcp_wire_oracle(1, &oracle, client.len));
    try std.testing.expectEqualSlices(u8, oracle[0..client.len], &client);
    var output_client: c.venus_tcp_client_hello_t = undefined;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_client_hello_decode, .{ &output_client, &client, client.len }));
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&input_client), std.mem.asBytes(&output_client));
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_server_hello_encode, .{ &input_server, &server, server.len }));
    try std.testing.expectEqual(@as(c_int, 1), venus_tcp_wire_oracle(2, &oracle, server.len));
    try std.testing.expectEqualSlices(u8, &oracle, &server);
    var output_server: c.venus_tcp_server_hello_t = undefined;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_server_hello_decode, .{ &output_server, &server, server.len }));
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&input_server), std.mem.asBytes(&output_server));
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_ack_encode, .{ TestSession, &ack, ack.len }));
    try std.testing.expectEqual(@as(c_int, 1), venus_tcp_wire_oracle(3, &oracle, ack.len));
    try std.testing.expectEqualSlices(u8, oracle[0..ack.len], &ack);
    var identity: u64 = 0;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_ack_decode, .{ &identity, &ack, ack.len }));
    try std.testing.expectEqual(TestSession, identity);
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_profile_encode, .{ &declared, declared.len }));
    try std.testing.expectEqual(@as(c_int, 1), venus_tcp_wire_oracle(4, &oracle, declared.len));
    try std.testing.expectEqualSlices(u8, oracle[0..declared.len], &declared);
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_profile_validate, .{ &declared, declared.len }));
}
test "every hello truncation and malformed prefix is rejected before read" {
    var client: [128]u8 = undefined;
    var server: [224]u8 = undefined;
    var ack: [32]u8 = undefined;
    _ = venus_tcp_wire_oracle(1, &client, client.len);
    _ = venus_tcp_wire_oracle(2, &server, server.len);
    _ = venus_tcp_wire_oracle(3, &ack, ack.len);
    var output_client: c.venus_tcp_client_hello_t = undefined;
    var output_server: c.venus_tcp_server_hello_t = undefined;
    var identity: u64 = undefined;
    for (0..128) |extent| {
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_client_hello_decode, .{ &output_client, &client, extent }));
        try is_zero(&output_client);
    }
    for (0..224) |extent| {
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_server_hello_decode, .{ &output_server, &server, extent }));
        try is_zero(&output_server);
    }
    for (0..32) |extent| {
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_ack_decode, .{ &identity, &ack, extent }));
        try std.testing.expectEqual(@as(u64, 0), identity);
    }
    for (0..16) |index| {
        client[index] ^= 0x80;
        server[index] ^= 0x80;
        ack[index] ^= 0x80;
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_client_hello_decode, .{ &output_client, &client, client.len }));
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_server_hello_decode, .{ &output_server, &server, server.len }));
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_ack_decode, .{ &identity, &ack, ack.len }));
        client[index] ^= 0x80;
        server[index] ^= 0x80;
        ack[index] ^= 0x80;
    }
}
test "reserved bytes and zero session never pass authentication framing" {
    var client: [128]u8 = undefined;
    var server: [224]u8 = undefined;
    var ack: [32]u8 = undefined;
    _ = venus_tcp_wire_oracle(1, &client, client.len);
    _ = venus_tcp_wire_oracle(2, &server, server.len);
    _ = venus_tcp_wire_oracle(3, &ack, ack.len);
    var output_client: c.venus_tcp_client_hello_t = undefined;
    var output_server: c.venus_tcp_server_hello_t = undefined;
    var identity: u64 = undefined;
    for (64..128) |index| {
        client[index] = 1;
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_client_hello_decode, .{ &output_client, &client, client.len }));
        client[index] = 0;
    }
    for (40..64) |index| {
        server[index] = 1;
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_server_hello_decode, .{ &output_server, &server, server.len }));
        server[index] = 0;
    }
    for (24..32) |index| {
        ack[index] = 1;
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_ack_decode, .{ &identity, &ack, ack.len }));
        ack[index] = 0;
    }
    @memset(server[16..24], 0);
    @memset(ack[16..24], 0);
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_server_hello_decode, .{ &output_server, &server, server.len }));
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_ack_decode, .{ &identity, &ack, ack.len }));
}
test "invalid encoder arguments preserve caller storage and null decoders clear outputs" {
    var bytes = [_]u8{0xa5} ** 224;
    const client = client_fixture();
    var server = server_fixture();
    var output_client: c.venus_tcp_client_hello_t = undefined;
    var output_server: c.venus_tcp_server_hello_t = undefined;
    var identity: u64 = 99;
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_client_hello_encode, .{ null, &bytes, 128 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_client_hello_encode, .{ &client, null, 128 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_client_hello_encode, .{ &client, &bytes, 127 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_server_hello_encode, .{ null, &bytes, 224 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_server_hello_encode, .{ &server, null, 224 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_server_hello_encode, .{ &server, &bytes, 223 }));
    server.session = 0;
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_server_hello_encode, .{ &server, &bytes, 224 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_ack_encode, .{ 1, null, 32 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_ack_encode, .{ 1, &bytes, 31 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_ack_encode, .{ 0, &bytes, 32 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_profile_encode, .{ null, 160 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_profile_encode, .{ &bytes, 159 }));
    for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0xa5), byte);
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_client_hello_decode, .{ null, &bytes, 128 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_client_hello_decode, .{ &output_client, null, 128 }));
    try is_zero(&output_client);
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_server_hello_decode, .{ null, &bytes, 224 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_server_hello_decode, .{ &output_server, null, 224 }));
    try is_zero(&output_server);
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_ack_decode, .{ null, &bytes, 32 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_ack_decode, .{ &identity, null, 32 }));
    try std.testing.expectEqual(@as(u64, 0), identity);
}
test "all token mismatch positions and declaration bytes rejected" {
    const first = client_fixture().token;
    var second = first;
    try std.testing.expectEqual(@as(c_int, 1), @call(.never_inline, venus_tcp_token_equal, .{ &first, &second }));
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_tcp_token_equal, .{ null, &second }));
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_tcp_token_equal, .{ &first, null }));
    for (0..32) |index| {
        second[index] ^= 1;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_tcp_token_equal, .{ &first, &second }));
        second[index] ^= 1;
    }
    var bytes = Profile;
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_profile_validate, .{ null, 160 }));
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_profile_validate, .{ &bytes, 159 }));
    for (0..160) |index| {
        bytes[index] ^= 1;
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_profile_validate, .{ &bytes, 160 }));
        bytes[index] ^= 1;
    }
}
test "strict configuration copies fields without retaining parser scratch" {
    var output: c.venus_tcp_config_t = undefined;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_config_decode, .{ &output, TestConfig, TestConfig.len }));
    try std.testing.expectEqual(@as(u32, 1), output.version);
    try std.testing.expectEqual(@as(u32, 55987), output.port);
    try std.testing.expectEqual(@as(u32, 60000), output.exchange_timeout_ms);
    try std.testing.expectEqualStrings("10.0.2.2", std.mem.sliceTo(&output.host, 0));
    try std.testing.expectEqualStrings("C:\\waddle\\driver.dll", std.mem.sliceTo(&output.icd_path, 0));
    for (output.token, 0..) |byte, index| try std.testing.expectEqual(@as(u8, if (index % 8 == 0) 1 else if (index % 8 == 1) 0x23 else if (index % 8 == 2) 0x45 else if (index % 8 == 3) 0x67 else if (index % 8 == 4) 0x89 else if (index % 8 == 5) 0xab else if (index % 8 == 6) 0xcd else 0xef), byte);
    try config_replace("10.0.2.2", "127.0.0.1", true);
    try config_replace("55987", "1", true);
    try config_replace("55987", "65535", true);
    try config_replace("60000", "1", true);
    try config_replace("C:\\\\waddle\\\\driver.dll", "\\\\\\\\server\\\\share\\\\driver.dll", true);
}
test "schema duplicates extras missing wrong types and complete truncations reject" {
    for (0..TestConfig.len) |length| try config_error(TestConfig[0..length]);
    try config_error("{}");
    try config_replace("\"version\":1", "\"version\":1,\"version\":1", false);
    try config_replace("\"version\":1", "\"unknown\":1", false);
    try config_replace("\"version\":1", "\"version\":\"1\"", false);
    try config_replace("\"version\":1", "\"version\":2", false);
    try config_replace("55987", "-1", false);
    try config_replace("55987", "0", false);
    try config_replace("55987", "65536", false);
    try config_replace("60000", "0", false);
    try config_replace("60000", "60001", false);
    try config_replace("10.0.2.2", "example.com", false);
    try config_replace("0123456789abcdef", "0123456789abcdeF", false);
    try config_replace("0123456789abcdef", "0123456789abcdez", false);
    try config_replace("0123456789abcdef", "0123456789abcde", false);
}
test "configuration bytes paths escaped null and fixed extent errors" {
    try config_replace("C:\\\\waddle\\\\driver.dll", "driver.dll", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "C:/driver.dll", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "\\\\\\\\server", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "\\\\\\\\server\\\\", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "\\\\\\\\\\\\server\\\\share", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "\\\\\\\\server\\\\\\\\share", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "C:\\\\bad\\u0000.dll", false);
    try config_replace("C:\\\\waddle\\\\driver.dll", "C:\\\\bad\\u0080.dll", false);
    var altered: [TestConfig.len]u8 = undefined;
    @memcpy(&altered, TestConfig);
    altered[0] = 0;
    try config_error(&altered);
    altered[0] = 128;
    try config_error(&altered);
    const long_path = try std.testing.allocator.alloc(u8, 1030);
    defer std.testing.allocator.free(long_path);
    @memset(long_path, 'a');
    long_path[0] = 'C';
    long_path[1] = ':';
    long_path[2] = '\\';
    long_path[3] = '\\';
    try config_replace("C:\\\\waddle\\\\driver.dll", long_path, false);
    var oversized = [_]u8{' '} ** 4097;
    var output: c.venus_tcp_config_t = undefined;
    try std.testing.expectEqual(@as(c_int, c.RingLimit), @call(.never_inline, venus_tcp_config_decode, .{ &output, &oversized, oversized.len }));
    try is_zero(&output);
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_config_decode, .{ null, TestConfig, TestConfig.len }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_config_decode, .{ &output, null, 1 }));
    try is_zero(&output);
}
test "bounded admitted requests exclude handshake and presentation operations" {
    var request = std.mem.zeroes(c.venus_request_t);
    request.sequence = 1;
    for (2..11) |kind| {
        request.kind = @intCast(kind);
        try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_request_limit, .{&request}));
    }
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_request_limit, .{null}));
    for ([_]u32{ 0, 1, 11, 12, 13, 0xffffffff }) |kind| {
        request.kind = kind;
        try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_request_limit, .{&request}));
    }
    request.kind = 2;
    request.direction = 1;
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_request_limit, .{&request}));
    request.direction = 0;
    request.payload_bytes = 65620;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_request_limit, .{&request}));
    request.payload_bytes += 1;
    try std.testing.expectEqual(@as(c_int, c.RingLimit), @call(.never_inline, venus_tcp_request_limit, .{&request}));
    request.payload_bytes = 0;
    for ([_]u32{ 3, 6, 7 }) |kind| {
        request.kind = kind;
        request.argument_one = 4096;
        try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_request_limit, .{&request}));
        request.argument_one += 1;
        try std.testing.expectEqual(@as(c_int, c.RingLimit), @call(.never_inline, venus_tcp_request_limit, .{&request}));
    }
    request.argument_one = 0;
    request.payload_bytes = 4097;
    try std.testing.expectEqual(@as(c_int, c.RingLimit), @call(.never_inline, venus_tcp_request_limit, .{&request}));
}
test "response pairing error payload and exact read ceilings" {
    var request = std.mem.zeroes(c.venus_request_t);
    request.kind = 2;
    request.sequence = 42;
    var response = request;
    response.direction = 1;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_response_limit, .{ null, &response }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_response_limit, .{ &request, null }));
    response.direction = 0;
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    response.direction = 1;
    response.kind = 3;
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    response.kind = 2;
    response.sequence = 43;
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    response.sequence = 42;
    response.status = 8;
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    response.status = 1;
    response.payload_bytes = 1;
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    response.payload_bytes = 0;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    response.status = 0;
    response.payload_bytes = 4097;
    try std.testing.expectEqual(@as(c_int, c.RingLimit), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    response.payload_bytes = 4096;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
    for ([_]u32{ 3, 6 }) |kind| {
        request.kind = kind;
        response.kind = kind;
        request.argument_one = 4096;
        try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
        response.payload_bytes = 4095;
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_response_limit, .{ &request, &response }));
        response.payload_bytes = 4096;
    }
}

test "exact filename array is bounded immutable and shares strict config grammar" {
    const Valid = [_][:0]const u8{ "C:\\config.json", "c:\\x", "\\\\server\\share\\config.json" };
    for (Valid) |path| try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_windows_path_validate, .{ path.ptr, path.len + 1 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_windows_path_validate, .{ null, 10 }));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), @call(.never_inline, venus_tcp_windows_path_validate, .{ Valid[0].ptr, 0 }));
    try std.testing.expectEqual(@as(c_int, c.RingLimit), @call(.never_inline, venus_tcp_windows_path_validate, .{ Valid[0].ptr, 1025 }));
    const Invalid = [_][:0]const u8{ "", "relative.json", "C:relative", "C:/file", "\\\\server", "\\\\server\\", "\\\\server\\\\share" };
    for (Invalid) |path| try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_windows_path_validate, .{ path.ptr, path.len + 1 }));
    var bytes = [_]u8{0xa5} ** 1026;
    bytes[1] = 'C';
    bytes[2] = ':';
    bytes[3] = '\\';
    for (4..1024) |index| bytes[index] = 'x';
    bytes[1024] = 0;
    try std.testing.expectEqual(@as(c_int, c.RingOk), @call(.never_inline, venus_tcp_windows_path_validate, .{ bytes[1..].ptr, 1024 }));
    try std.testing.expectEqual(@as(u8, 0xa5), bytes[0]);
    try std.testing.expectEqual(@as(u8, 0xa5), bytes[1025]);
    const saved = bytes;
    for (1..1024) |length| try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_windows_path_validate, .{ bytes[1..].ptr, length }));
    try std.testing.expectEqualSlices(u8, &saved, &bytes);
    bytes[4] = 0;
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_windows_path_validate, .{ bytes[1..].ptr, 1024 }));
    bytes[4] = 255;
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), @call(.never_inline, venus_tcp_windows_path_validate, .{ bytes[1..].ptr, 1024 }));
}

test "all config integer fields reject float exponent overflow and nonnumeric nodes" {
    const Invalid = [_][]const u8{ "0", "-0", "-1", "4294967295", "4294967296", "9223372036854775808", "18446744073709551616", "1.0", "1e0", "100e-2", "1E+0", "1e999", "true", "false", "null", "[]", "{}", "\"1\"", "[1]", "{\"value\":1}" };
    const Targets = [_][]const u8{ "\"version\":1", "\"port\":55987", "\"exchange_timeout_ms\":60000" };
    for (Targets) |target| {
        const colon = std.mem.indexOfScalar(u8, target, ':').?;
        for (Invalid) |number| {
            var replacement: [128]u8 = undefined;
            const replaced = try std.fmt.bufPrint(&replacement, "{s}{s}", .{ target[0 .. colon + 1], number });
            try config_replace(target, replaced, false);
        }
    }
    try config_replace("55987", "1", true);
    try config_replace("55987", "65535", true);
    try config_replace("60000", "1", true);
}
