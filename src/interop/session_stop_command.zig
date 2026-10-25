//! `waddle session stop [--device|-d NAME]` (IMPL_DESC 3.12.5). The device is resolved with the
//! rules `waddle open` uses. The client connects to that device's session.sock, sends one LocalStop
//! (0x7F01, no fields, request id StopRequestId), and waits up to ReplyTimeoutMs for the
//! LocalStopResult that echoes the id. When no session accepts a connection, session.lock decides:
//! an unlocked lock means already stopped, and a held lock means the stop cannot be confirmed.
//! The command never unlinks a socket or lock file and never signals a process. Every effect goes
//! through Hooks, so the decisions are tested with fakes; RealHooks at the bottom is production.
//! Exit codes: 0 stopped or already stopped, 1 unconfirmed or refused, 2 usage, otherwise the
//! device resolver's own code.
const std = @import("std");
const local = @import("local_session_client.zig");
const c = @cImport({
    @cInclude("daemon_device.h");
    @cInclude("waddle/interop_protocol.h");
});

/// Time from the end of the LocalStop send to the end of the wait for its result, in milliseconds.
pub const ReplyTimeoutMs: u64 = 5000;
/// Request id of the LocalStop frame. The protocol requires it to be nonzero.
pub const StopRequestId: u32 = 1;
/// Largest reply payload this client reads. Session replies are a few dozen bytes.
pub const PayloadBytes = 4096;
/// Usage line printed for every usage error.
pub const UsageText = "usage: waddle session stop [--device|-d NAME]";
/// Exit code when the session is stopped, or when no session is running.
pub const ExitStopped: u8 = 0;
/// Exit code when the stop is unconfirmed or the session refused it.
pub const ExitUnconfirmed: u8 = 1;
/// Exit code for a usage error.
pub const ExitUsage: u8 = 2;
/// Exit code used when the resolver reports a path that is too long to derive session.sock from.
const ExitInternal: u8 = 125;
/// Argument count the exported entry accepts; `session stop -d NAME` needs three.
const MaxArgs = 16;

/// C function-pointer type of the device resolver passed from host.c (same as `waddle open`).
const resolve_fn_t = ?*const fn ([*c]const u8, ?*c.device_info_t, [*c]u8, usize, c_int) callconv(.C) c_int;

/// Device selected for this invocation. Strings are borrowed from the hook implementation.
pub const Device = struct {
    /// Registry name, used in messages.
    name: []const u8,
    /// Absolute path of session.sock for this device.
    session_path: []const u8,
};

/// Effects of one invocation. Every function receives ctx unchanged.
pub const Hooks = struct {
    /// Opaque state of the implementation.
    ctx: *anyopaque,
    /// Resolve the device (null name selects the default). Returns null with out filled, or the
    /// resolver's nonzero exit code after it has printed its own diagnostic.
    resolve: *const fn (ctx: *anyopaque, name: ?[]const u8, out: *Device) ?u8,
    /// Monotonic clock in milliseconds.
    now_ms: *const fn (ctx: *anyopaque) u64,
    /// Classify the runtime directory of the device.
    dir_state: *const fn (ctx: *anyopaque, dir: []const u8) local.DirState,
    /// Probe session.lock in the runtime directory without holding it.
    lock_state: *const fn (ctx: *anyopaque, dir: []const u8) local.LockState,
    /// Connect to session.sock; the returned descriptor is closed through close.
    connect: *const fn (ctx: *anyopaque, path: []const u8) local.ConnectError!c_int,
    /// Close a descriptor.
    close: *const fn (ctx: *anyopaque, fd: c_int) void,
    /// Write every byte or fail.
    write_all: *const fn (ctx: *anyopaque, fd: c_int, bytes: []const u8) local.IoError!void,
    /// Read some bytes; timeout_ms is never negative in this command.
    poll_read: *const fn (ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) local.IoError!usize,
    /// Write one diagnostic line to stderr.
    message: *const fn (ctx: *anyopaque, text: []const u8) void,
};

/// Run `session` with its arguments after the word `session`.
/// @param[in] args Arguments; the first must be `stop`.
/// @param[in] hooks Effects, borrowed for the call.
/// @return Exit code (see the module comment).
pub fn run(args: []const []const u8, hooks: *const Hooks) u8 {
    if (args.len == 0 or !std.mem.eql(u8, args[0], "stop")) return usage(hooks);
    var name: ?[]const u8 = null;
    var index: usize = 1;
    while (index < args.len) : (index += 1) {
        const arg = args[index];
        if (std.mem.eql(u8, arg, "--device") or std.mem.eql(u8, arg, "-d")) {
            index += 1;
            if (index >= args.len or name != null or args[index].len == 0) return usage(hooks);
            name = args[index];
        } else {
            return usage(hooks);
        }
    }
    var device: Device = undefined;
    if (hooks.resolve(hooks.ctx, name, &device)) |code| return code;
    return stop_device(hooks, &device);
}

fn usage(hooks: *const Hooks) u8 {
    hooks.message(hooks.ctx, UsageText);
    return ExitUsage;
}

/// Print "waddle session stop: REASON; the stop is unconfirmed" and return the unconfirmed exit code.
fn unconfirmed(hooks: *const Hooks, reason: []const u8) u8 {
    var text = local.Message{};
    text.add("waddle session stop: ");
    text.add(reason);
    text.add("; the stop is unconfirmed");
    hooks.message(hooks.ctx, text.slice());
    return ExitUnconfirmed;
}

fn stop_device(hooks: *const Hooks, device: *const Device) u8 {
    const dir = std.fs.path.dirname(device.session_path) orelse unreachable;
    if (hooks.dir_state(hooks.ctx, dir) == .unsafe) {
        return unconfirmed(hooks, "the runtime directory is not private to this user");
    }
    const fd = hooks.connect(hooks.ctx, device.session_path) catch |err| switch (err) {
        error.NotFound, error.Refused => return no_listener(hooks, device, dir),
        error.Failed => return unconfirmed(hooks, "could not connect to the session socket"),
        error.PeerMismatch => return unconfirmed(hooks, "the session socket belongs to another user"),
    };
    defer hooks.close(hooks.ctx, fd);
    return request_stop(hooks, fd);
}

/// No live session accepted a connection: session.lock is the authority on whether one still runs.
fn no_listener(hooks: *const Hooks, device: *const Device, dir: []const u8) u8 {
    switch (hooks.lock_state(hooks.ctx, dir)) {
        .unlocked => {
            var text = local.Message{};
            text.add("waddle session stop: no session is running for ");
            text.add(device.name);
            text.add(" (already stopped)");
            hooks.message(hooks.ctx, text.slice());
            return ExitStopped;
        },
        .held => return unconfirmed(hooks, "the session lock is held but no session accepts connections"),
        .unsafe => return unconfirmed(hooks, "the session lock cannot be inspected safely"),
    }
}

/// Send LocalStop on a connected socket and wait for its result within ReplyTimeoutMs.
fn request_stop(hooks: *const Hooks, fd: c_int) u8 {
    // A header-only frame always fits its buffer, so the codec cannot fail here.
    var frame: [c.InteropHeaderBytes]u8 = undefined;
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, &frame, frame.len);
    var frame_length: usize = 0;
    std.debug.assert(c.interop_writer_finish(&writer, c.InteropMsgLocalStop, StopRequestId, c.InteropFromLocal, &frame_length) == 0);
    hooks.write_all(hooks.ctx, fd, frame[0..frame_length]) catch return unconfirmed(hooks, "could not send the stop request");

    const deadline = hooks.now_ms(hooks.ctx) + ReplyTimeoutMs;
    var header_bytes: [c.InteropHeaderBytes]u8 = undefined;
    if (read_failure(hooks, read_bounded(hooks, fd, &header_bytes, deadline))) |code| return code;
    var header: c.interop_header_t = undefined;
    if (c.interop_header_decode(&header_bytes, header_bytes.len, &header) != 0) {
        return unconfirmed(hooks, "the session sent an invalid reply");
    }
    if (header.length > PayloadBytes) return unconfirmed(hooks, "the session sent a reply too large to read");
    var payload_buffer: [PayloadBytes]u8 = undefined;
    const payload = payload_buffer[0..header.length];
    if (read_failure(hooks, read_bounded(hooks, fd, payload, deadline))) |code| return code;

    // An Error with request id 0 is the session refusing the connection because it is busy.
    if (header.type == c.InteropMsgError and header.request_id == 0) return unconfirmed(hooks, "the session is busy");
    if (header.request_id != StopRequestId) return unconfirmed(hooks, "the session replied to a different request");
    if (header.type == c.InteropMsgError) {
        var text = local.Message{};
        text.add("waddle session stop: the session refused the stop: ");
        text.add(optional_message(payload) orelse "no message");
        hooks.message(hooks.ctx, text.slice());
        return ExitUnconfirmed;
    }
    if (header.type != c.InteropMsgLocalStopResult) return unconfirmed(hooks, "the session sent an unexpected reply");
    if (c.interop_payload_validate(header.type, c.InteropFromLocal, payload.ptr, payload.len) != 0) {
        return unconfirmed(hooks, "the session sent an invalid stop result");
    }
    // interop_payload_validate has already required the four-byte status field.
    var status: u64 = 0;
    std.debug.assert(c.interop_field_uint(payload.ptr, payload.len, c.InteropTagStatus, 4, &status) == 0);
    if (status == c.InteropOk) {
        hooks.message(hooks.ctx, "waddle session stop: session stopped");
        return ExitStopped;
    }
    var text = local.Message{};
    text.add("waddle session stop: the session did not stop: ");
    if (optional_message(payload)) |detail| {
        text.add(detail);
    } else {
        text.add("status ");
        text.add_number(status);
    }
    hooks.message(hooks.ctx, text.slice());
    return ExitUnconfirmed;
}

/// Map a read outcome to the unconfirmed exit, or null when the buffer was filled.
fn read_failure(hooks: *const Hooks, outcome: local.ReadExact) ?u8 {
    return switch (outcome) {
        .ok => null,
        .timeout => unconfirmed(hooks, "no result within 5 seconds"),
        .eof_boundary, .eof_partial => unconfirmed(hooks, "the session closed without a result"),
        .failed => unconfirmed(hooks, "could not read the session reply"),
    };
}

/// Fill buffer completely, checking the deadline before every read so that a slow trickle cannot
/// extend the wait beyond ReplyTimeoutMs.
fn read_bounded(hooks: *const Hooks, fd: c_int, buffer: []u8, deadline: u64) local.ReadExact {
    var filled: usize = 0;
    while (filled < buffer.len) {
        const now = hooks.now_ms(hooks.ctx);
        if (now >= deadline) return .timeout;
        const remaining: i32 = @intCast(@min(deadline - now, std.math.maxInt(i32)));
        const read = hooks.poll_read(hooks.ctx, fd, buffer[filled..], remaining) catch |err| switch (err) {
            error.Timeout => return .timeout,
            error.Failed => return .failed,
        };
        if (read == 0) return if (filled == 0) .eof_boundary else .eof_partial;
        filled += read;
    }
    return .ok;
}

/// The message field of a payload, when present and well formed.
fn optional_message(payload: []const u8) ?[]const u8 {
    var value: ?[*]const u8 = null;
    var length: u16 = 0;
    if (c.interop_field_get(payload.ptr, payload.len, c.InteropTagMessage, 0, &value, &length) != 0) return null;
    return value.?[0..length];
}

/// C ABI: run `waddle session` with the production hooks.
/// @param[in] argc Number of arguments after `session`.
/// @param[in] argv Arguments after `session`, readable for argc entries.
/// @param[in] resolve_fn Non-NULL device resolver (resolve_target_device in host.c).
/// @return Exit code.
pub export fn waddle_session_command(argc: c_int, argv: ?[*][*:0]u8, resolve_fn: resolve_fn_t) c_int {
    return command_entry(argc, argv, resolve_fn);
}

/// Body of waddle_session_command; the export only forwards to it.
fn command_entry(argc: c_int, argv: ?[*][*:0]u8, resolve_fn: resolve_fn_t) c_int {
    const resolver = resolve_fn orelse return ExitInternal;
    if (argc < 0 or argc > MaxArgs) return ExitUsage;
    if (argc > 0 and argv == null) return ExitUsage;
    var storage: [MaxArgs][]const u8 = undefined;
    const count: usize = @intCast(argc);
    for (0..count) |index| storage[index] = std.mem.span(argv.?[index]);
    var real = RealHooks{ .resolver = resolver };
    const hooks = real.hooks();
    return run(storage[0..count], &hooks);
}

// Production hooks. The resolver fills the socket path of the device registry; session.sock is
// placed beside it, as `waddle open` does.
const RealHooks = struct {
    resolver: resolve_fn_t,
    name_buffer: [c.WaddleMaxDeviceNameLen]u8 = undefined,
    device_buffer: [c.WaddleMaxDeviceNameLen]u8 = undefined,
    socket_buffer: [c.WaddleMaxPathLen]u8 = undefined,
    session_buffer: [c.WaddleMaxPathLen]u8 = undefined,

    fn hooks(self: *RealHooks) Hooks {
        return .{
            .ctx = self,
            .resolve = resolve,
            .now_ms = now_ms,
            .dir_state = dir_state,
            .lock_state = lock_state,
            .connect = connect,
            .close = close,
            .write_all = write_all,
            .poll_read = poll_read,
            .message = message,
        };
    }

    fn of(ctx: *anyopaque) *RealHooks {
        return @ptrCast(@alignCast(ctx));
    }

    fn resolve(ctx: *anyopaque, name: ?[]const u8, out: *Device) ?u8 {
        const self = of(ctx);
        var specified: ?[*:0]const u8 = null;
        if (name) |value| {
            if (value.len >= self.name_buffer.len) return 2;
            @memcpy(self.name_buffer[0..value.len], value);
            self.name_buffer[value.len] = 0;
            specified = @ptrCast(&self.name_buffer);
        }
        var info: c.device_info_t = undefined;
        const status = self.resolver.?(specified, &info, &self.socket_buffer, self.socket_buffer.len, 1);
        if (status != 0) return @intCast(status);
        const resolved_name = std.mem.sliceTo(&info.name, 0);
        @memcpy(self.device_buffer[0..resolved_name.len], resolved_name);
        const socket = std.mem.sliceTo(&self.socket_buffer, 0);
        const directory = std.fs.path.dirname(socket) orelse return ExitInternal;
        const session_name = local.SessionSocketName;
        if (directory.len + 1 + session_name.len >= self.session_buffer.len) return ExitInternal;
        @memcpy(self.session_buffer[0..directory.len], directory);
        self.session_buffer[directory.len] = '/';
        @memcpy(self.session_buffer[directory.len + 1 ..][0..session_name.len], session_name);
        const session_length = directory.len + 1 + session_name.len;
        out.* = .{
            .name = self.device_buffer[0..resolved_name.len],
            .session_path = self.session_buffer[0..session_length],
        };
        return null;
    }

    fn now_ms(ctx: *anyopaque) u64 {
        _ = ctx;
        const instant = std.time.Instant.now() catch return 0;
        return @as(u64, @intCast(instant.timestamp.tv_sec)) * 1000 + @as(u64, @intCast(instant.timestamp.tv_nsec)) / 1_000_000;
    }

    fn dir_state(ctx: *anyopaque, dir: []const u8) local.DirState {
        _ = ctx;
        return local.dir_state(dir);
    }

    fn lock_state(ctx: *anyopaque, dir: []const u8) local.LockState {
        _ = ctx;
        return local.lock_state(dir);
    }

    fn connect(ctx: *anyopaque, path: []const u8) local.ConnectError!c_int {
        _ = ctx;
        return local.connect(path);
    }

    fn close(ctx: *anyopaque, fd: c_int) void {
        _ = ctx;
        local.close(fd);
    }

    fn write_all(ctx: *anyopaque, fd: c_int, bytes: []const u8) local.IoError!void {
        _ = ctx;
        return local.write_all(fd, bytes);
    }

    fn poll_read(ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) local.IoError!usize {
        _ = ctx;
        return local.poll_read(fd, buffer, timeout_ms);
    }

    fn message(ctx: *anyopaque, text: []const u8) void {
        _ = ctx;
        const stderr = std.io.getStdErr();
        stderr.writeAll(text) catch {};
        stderr.writeAll("\n") catch {};
    }
};

test {
    // The shared client primitives have their own tests; run them with this suite.
    _ = local;
}

// ---- Unit tests with fake effects -------------------------------------------------------------

const testing = std.testing;

/// Scripted effects. Records the frame sent, every message, and the device name the resolver saw.
const Fake = struct {
    device: Device = .{ .name = "alpha", .session_path = "/run/u/waddle/alpha/session.sock" },
    resolve_code: ?u8 = null,
    resolved: ?[]const u8 = null,
    resolve_calls: usize = 0,
    dir: local.DirState = .ok,
    lock: local.LockState = .unlocked,
    connect_script: []const local.ConnectError!c_int = &.{error.NotFound},
    connect_calls: usize = 0,
    closed: usize = 0,
    fail_write: bool = false,
    sent: [256]u8 = undefined,
    sent_length: usize = 0,
    /// Bytes delivered by poll_read, in order; delivered in chunks of at most chunk bytes.
    incoming: []const u8 = &.{},
    cursor: usize = 0,
    chunk: usize = 0,
    /// Milliseconds the clock advances on every read, to model a slow session.
    advance_per_read: u64 = 0,
    /// When true every read fails, as a broken descriptor would.
    read_fails: bool = false,
    /// After incoming ends: false means end of stream, true means silence until the timeout.
    silent: bool = false,
    now: u64 = 10_000,
    messages: [4][256]u8 = undefined,
    message_lengths: [4]usize = [_]usize{0} ** 4,
    message_count: usize = 0,

    fn of(ctx: *anyopaque) *Fake {
        return @ptrCast(@alignCast(ctx));
    }

    fn hooks(self: *Fake) Hooks {
        return .{
            .ctx = self,
            .resolve = fake_resolve,
            .now_ms = fake_now,
            .dir_state = fake_dir,
            .lock_state = fake_lock,
            .connect = fake_connect,
            .close = fake_close,
            .write_all = fake_write,
            .poll_read = fake_poll,
            .message = fake_message,
        };
    }

    fn message_at(self: *const Fake, index: usize) []const u8 {
        return self.messages[index][0..self.message_lengths[index]];
    }

    /// The last message printed; every scenario prints exactly one.
    fn last_message(self: *const Fake) []const u8 {
        return self.message_at(self.message_count - 1);
    }
};

fn fake_resolve(ctx: *anyopaque, name: ?[]const u8, out: *Device) ?u8 {
    const f = Fake.of(ctx);
    f.resolve_calls += 1;
    f.resolved = name;
    if (f.resolve_code) |code| return code;
    out.* = f.device;
    return null;
}

fn fake_now(ctx: *anyopaque) u64 {
    return Fake.of(ctx).now;
}

fn fake_dir(ctx: *anyopaque, dir: []const u8) local.DirState {
    _ = dir;
    return Fake.of(ctx).dir;
}

fn fake_lock(ctx: *anyopaque, dir: []const u8) local.LockState {
    _ = dir;
    return Fake.of(ctx).lock;
}

fn fake_connect(ctx: *anyopaque, path: []const u8) local.ConnectError!c_int {
    _ = path;
    const f = Fake.of(ctx);
    const index = @min(f.connect_calls, f.connect_script.len - 1);
    f.connect_calls += 1;
    return f.connect_script[index];
}

fn fake_close(ctx: *anyopaque, fd: c_int) void {
    _ = fd;
    Fake.of(ctx).closed += 1;
}

fn fake_write(ctx: *anyopaque, fd: c_int, bytes: []const u8) local.IoError!void {
    _ = fd;
    const f = Fake.of(ctx);
    if (f.fail_write) return error.Failed;
    @memcpy(f.sent[f.sent_length..][0..bytes.len], bytes);
    f.sent_length += bytes.len;
}

fn fake_poll(ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) local.IoError!usize {
    _ = fd;
    const f = Fake.of(ctx);
    if (f.read_fails) return error.Failed;
    f.now += f.advance_per_read;
    if (f.cursor < f.incoming.len) {
        var count = @min(buffer.len, f.incoming.len - f.cursor);
        if (f.chunk > 0) count = @min(count, f.chunk);
        @memcpy(buffer[0..count], f.incoming[f.cursor..][0..count]);
        f.cursor += count;
        return count;
    }
    if (f.silent) {
        // Silence advances the clock to the timeout, as a real wait would.
        f.now += @as(u64, @intCast(timeout_ms));
        return error.Timeout;
    }
    return 0;
}

fn fake_message(ctx: *anyopaque, text: []const u8) void {
    const f = Fake.of(ctx);
    std.debug.assert(f.message_count < f.messages.len);
    @memcpy(f.messages[f.message_count][0..text.len], text);
    f.message_lengths[f.message_count] = text.len;
    f.message_count += 1;
}

/// Build one reply frame. status null omits the status field; message null omits the message.
fn build_reply(buffer: []u8, type_code: u16, request_id: u32, status: ?u64, message: ?[]const u8) []const u8 {
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, buffer.ptr, buffer.len);
    // Fields must be written in ascending tag order: Status precedes Message.
    if (status) |value| check(c.interop_put_uint(&writer, c.InteropTagStatus, value) == 0);
    if (message) |text| check(c.interop_put_bytes(&writer, c.InteropTagMessage, text.ptr, @intCast(text.len)) == 0);
    var length: usize = 0;
    check(c.interop_writer_finish(&writer, type_code, request_id, c.InteropFromLocal, &length) == 0);
    return buffer[0..length];
}

/// Build one Error frame from the host direction, as the session handler sends it.
fn build_error(buffer: []u8, request_id: u32, message: ?[]const u8) []const u8 {
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, buffer.ptr, buffer.len);
    if (message) |text| check(c.interop_put_bytes(&writer, c.InteropTagMessage, text.ptr, @intCast(text.len)) == 0);
    check(c.interop_put_uint(&writer, c.InteropTagErrorCode, 3) == 0);
    var length: usize = 0;
    check(c.interop_writer_finish(&writer, c.InteropMsgError, request_id, c.InteropFromHost, &length) == 0);
    return buffer[0..length];
}

fn check(condition: bool) void {
    if (!condition) @panic("test fixture failed");
}

fn run_fake(fake: *Fake, args: []const []const u8) u8 {
    var hooks = fake.hooks();
    return run(args, &hooks);
}

test "usage errors exit 2 before the device is resolved" {
    const cases = [_][]const []const u8{
        &.{},
        &.{"start"},
        &.{ "stop", "--bogus" },
        &.{ "stop", "-d" },
        &.{ "stop", "-d", "" },
        &.{ "stop", "-d", "a", "-d", "b" },
        &.{ "stop", "extra" },
    };
    for (cases) |args| {
        var fake = Fake{};
        try testing.expectEqual(ExitUsage, run_fake(&fake, args));
        try testing.expectEqual(@as(usize, 0), fake.resolve_calls);
        try testing.expectEqualSlices(u8, UsageText, fake.last_message());
    }
}

test "a resolver failure is returned unchanged and nothing else happens" {
    var fake = Fake{ .resolve_code = 3 };
    try testing.expectEqual(@as(u8, 3), run_fake(&fake, &.{ "stop", "-d", "nosuch" }));
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
    try testing.expectEqual(@as(usize, 0), fake.message_count);
}

test "the device name is passed to the resolver in both spellings; the default is null" {
    var named = Fake{ .connect_script = &.{error.NotFound} };
    try testing.expectEqual(ExitStopped, run_fake(&named, &.{ "stop", "-d", "beta" }));
    try testing.expectEqualSlices(u8, "beta", named.resolved.?);
    var long = Fake{ .connect_script = &.{error.NotFound} };
    try testing.expectEqual(ExitStopped, run_fake(&long, &.{ "stop", "--device", "gamma" }));
    try testing.expectEqualSlices(u8, "gamma", long.resolved.?);
    var default = Fake{ .connect_script = &.{error.NotFound} };
    try testing.expectEqual(ExitStopped, run_fake(&default, &.{"stop"}));
    try testing.expect(default.resolved == null);
}

test "no socket and an unlocked session lock is already stopped (exit 0)" {
    var fake = Fake{ .connect_script = &.{error.NotFound}, .lock = .unlocked };
    try testing.expectEqual(ExitStopped, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: no session is running for alpha (already stopped)", fake.last_message());
    try testing.expectEqual(@as(usize, 0), fake.sent_length);
}

test "no socket and a held session lock is unconfirmed (exit 1); nothing is unlinked or signalled" {
    var fake = Fake{ .connect_script = &.{error.NotFound}, .lock = .held };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session lock is held but no session accepts connections; the stop is unconfirmed", fake.last_message());
    try testing.expectEqual(@as(usize, 0), fake.sent_length);
}

test "a refused socket counts as no listener: unlocked is already stopped, held is unconfirmed" {
    var free = Fake{ .connect_script = &.{error.Refused}, .lock = .unlocked };
    try testing.expectEqual(ExitStopped, run_fake(&free, &.{"stop"}));
    var busy = Fake{ .connect_script = &.{error.Refused}, .lock = .held };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&busy, &.{"stop"}));
}

test "a lock file that cannot be inspected safely is unconfirmed" {
    var fake = Fake{ .connect_script = &.{error.NotFound}, .lock = .unsafe };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session lock cannot be inspected safely; the stop is unconfirmed", fake.last_message());
}

test "an unsafe runtime directory is refused before connecting" {
    var fake = Fake{ .dir = .unsafe };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
    try testing.expectEqualSlices(u8, "waddle session stop: the runtime directory is not private to this user; the stop is unconfirmed", fake.last_message());
}

test "a connect failure other than missing or refused is unconfirmed" {
    var fake = Fake{ .connect_script = &.{error.Failed} };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqual(@as(usize, 0), fake.sent_length);
}

test "a successful stop sends LocalStop with request id 1 and exits 0" {
    var reply: [64]u8 = undefined;
    const incoming = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId, c.InteropOk, null);
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = incoming };
    try testing.expectEqual(ExitStopped, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: session stopped", fake.last_message());
    try testing.expectEqual(@as(usize, 1), fake.closed);
    // The request is exactly one header: LocalStop, no fields, id 1.
    var header: c.interop_header_t = undefined;
    try testing.expectEqual(@as(c_int, 0), c.interop_header_decode(&fake.sent, fake.sent_length, &header));
    try testing.expectEqual(@as(u16, c.InteropMsgLocalStop), header.type);
    try testing.expectEqual(StopRequestId, header.request_id);
    try testing.expectEqual(@as(u32, 0), header.length);
    try testing.expectEqual(@as(usize, c.InteropHeaderBytes), fake.sent_length);
}

test "a non-Ok status exits 1 with the session's message, or its status when there is no message" {
    var reply: [128]u8 = undefined;
    const with_message = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId, 3, "busy");
    var busy = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = with_message };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&busy, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session did not stop: busy", busy.last_message());
    var without = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId, 5, null) };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&without, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session did not stop: status 5", without.last_message());
}

test "a LocalStopResult that echoes another request id is unconfirmed" {
    var reply: [64]u8 = undefined;
    const incoming = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId + 1, c.InteropOk, null);
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = incoming };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session replied to a different request; the stop is unconfirmed", fake.last_message());
}

test "an Error frame for this request is a refusal, with its message when present" {
    var reply: [128]u8 = undefined;
    const with_message = build_error(&reply, StopRequestId, "nope");
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = with_message };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session refused the stop: nope", fake.last_message());
    var bare = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = build_error(&reply, StopRequestId, null) };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&bare, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session refused the stop: no message", bare.last_message());
}

test "a busy refusal from the session (Error with request id 0) is unconfirmed" {
    var reply: [64]u8 = undefined;
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = build_error(&reply, 0, "busy") };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session is busy; the stop is unconfirmed", fake.last_message());
}

test "an unexpected frame type for this request is unconfirmed" {
    var reply: [64]u8 = undefined;
    const incoming = build_reply(&reply, c.InteropMsgLocalStop, StopRequestId, null, null);
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = incoming };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session sent an unexpected reply; the stop is unconfirmed", fake.last_message());
}

test "end of stream before a reply, or inside the header, is unconfirmed" {
    var empty = Fake{ .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&empty, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session closed without a result; the stop is unconfirmed", empty.last_message());
    var reply: [64]u8 = undefined;
    const full = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId, c.InteropOk, null);
    var inside = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = full[0..5] };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&inside, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session closed without a result; the stop is unconfirmed", inside.last_message());
}

test "silence until the deadline is unconfirmed after 5 seconds" {
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .silent = true };
    const start = fake.now;
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expect(fake.now - start >= ReplyTimeoutMs);
    try testing.expectEqualSlices(u8, "waddle session stop: no result within 5 seconds; the stop is unconfirmed", fake.last_message());
}

test "a reply that trickles in one byte at a time still completes" {
    var reply: [64]u8 = undefined;
    const incoming = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId, c.InteropOk, null);
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = incoming, .chunk = 1 };
    try testing.expectEqual(ExitStopped, run_fake(&fake, &.{"stop"}));
}

test "a reply that runs past the deadline mid-read is unconfirmed at once" {
    var reply: [64]u8 = undefined;
    const incoming = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId, c.InteropOk, null);
    // Each read takes six seconds, so the header completes after the 5 s deadline has passed.
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = incoming, .chunk = 1, .advance_per_read = 6000 };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: no result within 5 seconds; the stop is unconfirmed", fake.last_message());
}

test "a payload cut off after its header is unconfirmed" {
    var reply: [32]u8 = undefined;
    _ = build_reply(&reply, c.InteropMsgLocalStop, StopRequestId, null, null);
    std.mem.writeInt(u16, reply[6..8], c.InteropMsgLocalStopResult, .little);
    std.mem.writeInt(u32, reply[8..12], 4, .little);
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = reply[0..16] };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session closed without a result; the stop is unconfirmed", fake.last_message());
}

test "a failed read is unconfirmed" {
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .read_fails = true };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: could not read the session reply; the stop is unconfirmed", fake.last_message());
}

test "an invalid header is unconfirmed" {
    const zeros = [_]u8{0} ** c.InteropHeaderBytes;
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = &zeros };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session sent an invalid reply; the stop is unconfirmed", fake.last_message());
}

test "a payload larger than the client reads is refused without reading it" {
    var reply: [64]u8 = undefined;
    const frame = build_reply(&reply, c.InteropMsgLocalStopResult, StopRequestId, c.InteropOk, null);
    // The length field sits at bytes 8..12 of the header. Claim 5000 bytes of payload.
    std.mem.writeInt(u32, reply[8..12], 5000, .little);
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = frame };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session sent a reply too large to read; the stop is unconfirmed", fake.last_message());
}

test "a stop result without a status field is unconfirmed" {
    // The codec refuses to encode a LocalStopResult without its status, so the reply is patched
    // from a header-only frame: type LocalStopResult, id 1, and a four-byte payload whose field
    // claims eight bytes that are not there.
    var reply: [32]u8 = undefined;
    _ = build_reply(&reply, c.InteropMsgLocalStop, StopRequestId, null, null);
    std.mem.writeInt(u16, reply[6..8], c.InteropMsgLocalStopResult, .little);
    std.mem.writeInt(u32, reply[8..12], 4, .little);
    reply[16..20].* = .{ 1, 0, 8, 0 };
    const incoming = reply[0..20];
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .incoming = incoming };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: the session sent an invalid stop result; the stop is unconfirmed", fake.last_message());
}

test "a failed send is unconfirmed and no reply is read" {
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .fail_write = true };
    try testing.expectEqual(ExitUnconfirmed, run_fake(&fake, &.{"stop"}));
    try testing.expectEqualSlices(u8, "waddle session stop: could not send the stop request; the stop is unconfirmed", fake.last_message());
    try testing.expectEqual(@as(usize, 1), fake.closed);
}

/// Resolver fixture: device "alpha" whose registry socket is daemon.sock in the device directory.
fn resolver_alpha(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    _ = specified;
    _ = single;
    _ = cap;
    const info = out.?;
    @memset(&info.name, 0);
    @memcpy(info.name[0..5], "alpha");
    const socket = "/run/u/waddle/alpha/daemon.sock";
    @memcpy(sock[0..socket.len], socket);
    sock[socket.len] = 0;
    return 0;
}

fn resolver_no_directory(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    _ = specified;
    _ = single;
    _ = cap;
    const info = out.?;
    @memset(&info.name, 0);
    @memcpy(info.name[0..5], "alpha");
    @memcpy(sock[0..10], "daemon.sok");
    sock[10] = 0;
    return 0;
}

/// Resolver fixture whose socket directory is so long that session.sock does not fit beside it.
fn resolver_long_directory(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    _ = specified;
    _ = cap;
    _ = single;
    const info = out.?;
    @memset(&info.name, 0);
    @memcpy(info.name[0..5], "alpha");
    // Directory of 1011 bytes: the socket path (1023 bytes) fits its buffer, and session.sock beside
    // it would need 1024 bytes, which does not fit.
    var length: usize = 0;
    sock[length] = '/';
    length += 1;
    @memset(sock[length..][0..1010], 'd');
    length += 1010;
    @memcpy(sock[length..][0..12], "/daemon.sock");
    length += 12;
    sock[length] = 0;
    return 0;
}

fn resolver_refuses(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    _ = specified;
    _ = out;
    _ = sock;
    _ = cap;
    _ = single;
    return 4;
}

test "production resolve places session.sock beside the registry socket and forwards resolver failures" {
    var real = RealHooks{ .resolver = resolver_alpha };
    var hooks = real.hooks();
    var device: Device = undefined;
    try testing.expectEqual(@as(?u8, null), hooks.resolve(hooks.ctx, "alpha", &device));
    try testing.expectEqualSlices(u8, "alpha", device.name);
    try testing.expectEqualSlices(u8, "/run/u/waddle/alpha/session.sock", device.session_path);

    var none = RealHooks{ .resolver = resolver_no_directory };
    var refused_hooks = none.hooks();
    try testing.expectEqual(@as(?u8, ExitInternal), refused_hooks.resolve(refused_hooks.ctx, null, &device));

    var failing = RealHooks{ .resolver = resolver_refuses };
    var failing_hooks = failing.hooks();
    try testing.expectEqual(@as(?u8, 4), failing_hooks.resolve(failing_hooks.ctx, null, &device));

    var long_name = RealHooks{ .resolver = resolver_alpha };
    var long_hooks = long_name.hooks();
    var name_bytes = [_]u8{'n'} ** c.WaddleMaxDeviceNameLen;
    std.mem.doNotOptimizeAway(&name_bytes);
    try testing.expectEqual(@as(?u8, 2), long_hooks.resolve(long_hooks.ctx, name_bytes[0..], &device));

    var deep = RealHooks{ .resolver = resolver_long_directory };
    var deep_hooks = deep.hooks();
    try testing.expectEqual(@as(?u8, ExitInternal), deep_hooks.resolve(deep_hooks.ctx, null, &device));
}

test "production hooks act on real files, sockets and the clock" {
    const base = try local.scratch_root(testing.allocator);
    defer testing.allocator.free(base);
    defer std.fs.deleteTreeAbsolute(base) catch {};
    const device_root = try std.fmt.allocPrint(testing.allocator, "{s}/device", .{base});
    defer testing.allocator.free(device_root);
    try std.fs.makeDirAbsolute(device_root);
    try std.posix.fchmodat(std.posix.AT.FDCWD, device_root, 0o700, 0);
    const device_dir = try std.fmt.allocPrint(testing.allocator, "{s}/device", .{base});
    defer testing.allocator.free(device_dir);
    const socket_path = try std.fmt.allocPrint(testing.allocator, "{s}/session.sock", .{device_dir});
    defer testing.allocator.free(socket_path);

    var real = RealHooks{ .resolver = resolver_alpha };
    var hooks = real.hooks();
    try testing.expectEqual(local.DirState.ok, hooks.dir_state(hooks.ctx, device_dir));
    try testing.expectEqual(local.LockState.unlocked, hooks.lock_state(hooks.ctx, device_dir));
    try testing.expectError(error.NotFound, hooks.connect(hooks.ctx, socket_path));

    const address = try std.net.Address.initUnix(socket_path);
    const listener = try std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0);
    defer std.posix.close(listener);
    try std.posix.bind(listener, &address.any, address.getOsSockLen());
    try std.posix.listen(listener, 1);
    const fd = try hooks.connect(hooks.ctx, socket_path);
    defer hooks.close(hooks.ctx, fd);
    try hooks.write_all(hooks.ctx, fd, "stop");
    var buffer: [8]u8 = undefined;
    try testing.expectError(error.Timeout, hooks.poll_read(hooks.ctx, fd, &buffer, 0));
    const first = hooks.now_ms(hooks.ctx);
    try testing.expect(hooks.now_ms(hooks.ctx) >= first);
    hooks.message(hooks.ctx, "");
}

test "the exported entry runs the stop path with production hooks; a missing device reports already stopped" {
    // resolver_alpha names /run/u/waddle/alpha/session.sock, which does not exist on any test host.
    var argv = [_][*:0]u8{ @constCast("stop"), @constCast("-d"), @constCast("alpha") };
    std.mem.doNotOptimizeAway(&argv);
    try testing.expectEqual(@as(c_int, ExitStopped), waddle_session_command(hide_int(3), &argv, hide_resolver(resolver_alpha)));
}

test "the exported entry rejects missing resolvers and bad counts" {
    try testing.expectEqual(@as(c_int, ExitInternal), waddle_session_command(hide_int(0), null, hide_resolver(null)));
    try testing.expectEqual(@as(c_int, ExitUsage), waddle_session_command(hide_int(-1), null, hide_resolver(resolver_alpha)));
    try testing.expectEqual(@as(c_int, ExitUsage), waddle_session_command(hide_int(MaxArgs + 1), null, hide_resolver(resolver_alpha)));
    try testing.expectEqual(@as(c_int, ExitUsage), waddle_session_command(hide_int(1), null, hide_resolver(resolver_alpha)));
}

/// Pass a constant through run-time memory so the optimizer cannot fold the checks it guards.
fn hide_int(value: c_int) c_int {
    var hidden = value;
    std.mem.doNotOptimizeAway(&hidden);
    return hidden;
}

/// Pass a function pointer, possibly null, through run-time memory.
fn hide_resolver(value: resolve_fn_t) resolve_fn_t {
    var hidden = value;
    std.mem.doNotOptimizeAway(&hidden);
    return hidden;
}
