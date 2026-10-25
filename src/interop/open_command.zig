//! `waddle open` client: parses the request with open_request.zig, resolves the device,
//! canonicalizes and translates paths, delegates console executables to `waddle exec`,
//! connects to the device's session.sock (spawning waddle-session once when it is absent),
//! and sends one LocalLaunch frame per launched item. All effects go through Hooks so the
//! orchestration is testable; the real hooks at the bottom of this file are the production
//! implementation.
const std = @import("std");
const open_request = @import("open_request.zig");
const activation_store = @import("activation_store.zig");
const c = @cImport({
    @cInclude("daemon_device.h");
    @cInclude("daemon_config.h");
    @cInclude("daemon_fs.h");
    @cInclude("waddle/interop_protocol.h");
    @cInclude("errno.h");
    @cInclude("fcntl.h");
    @cInclude("stdlib.h");
});

/// C function-pointer types of host.c entries passed to waddle_open_command (open_command.h).
const exec_fn_t = ?*const fn (c_int, [*c][*c]u8, c_int) callconv(.C) c_int;
const resolve_fn_t = ?*const fn ([*c]const u8, ?*c.device_info_t, [*c]u8, usize, c_int) callconv(.C) c_int;

/// Deadline for the cold-start connect loop, in monotonic milliseconds (IMPL_DESC 8.44).
pub const DeadlineMs: u64 = 120_000;
/// First and maximum connect backoff in milliseconds (25 ms doubling to 250 ms).
pub const BackoffMinMs: u64 = 25;
pub const BackoffMaxMs: u64 = 250;
/// Wait before the single "Starting Windows" notice, in milliseconds.
pub const NoticeAfterMs: u64 = 1500;
/// Time to wait for an immediate reply when --wait is not given, in milliseconds.
pub const GraceMs: i32 = 2000;
/// Session socket file name inside the device runtime directory.
pub const SessionSocketName = "session.sock";
/// Notice text, sent once after NoticeAfterMs when integration.notify is enabled.
pub const StartingMessage = "Starting Windows\u{2026}";
/// Usage line printed for every usage error.
pub const UsageText = "usage: waddle open [--device NAME] [--wait] [--new-console] (--app APP_KEY | PATH... | -- EXE [ARG...])";

/// Failure of one read callback step; NotFound maps to exit 127, Unreadable to 126.
pub const ReadError = error{ NotFound, Unreadable };
/// Failure to canonicalize a host path.
pub const PathError = error{ NotFound, Failed, OutOfMemory };
/// Failure to translate a canonical host path into a guest path.
pub const TranslateError = error{ Outside, Failed, OutOfMemory };
/// Failure to connect to session.sock.
pub const ConnectError = error{ NotFound, Refused, Failed };
/// Failure of a socket read or write.
pub const IoError = error{ Failed, Timeout };
/// State of the device runtime directory before connecting.
pub const DirState = enum { missing, ok, unsafe };
/// Failure to spawn waddle-session.
pub const SpawnError = error{Failed};

/// Device selected for this invocation. Strings are owned by the hook implementation.
pub const Device = struct {
    /// Registry name, used in messages and for --device when delegating.
    name: []const u8,
    /// Absolute path of session.sock for this device.
    session_path: []const u8,
    /// True when integration.notify is enabled.
    notify: bool,
};

/// Effects of one invocation. Every field is a function taking ctx as its first argument.
pub const Hooks = struct {
    ctx: *anyopaque,
    /// True when stdin and stdout are both terminals.
    tty: *const fn (*anyopaque) bool,
    /// Select the device; returns null on success or the exit code after printing the reason.
    resolve: *const fn (*anyopaque, ?[]const u8, *Device) ?u8,
    /// Read up to buffer.len leading bytes of path; returns the byte count.
    read_prefix: *const fn (*anyopaque, []const u8, *[open_request.PrefixBytes]u8) ReadError!usize,
    /// Canonicalize path with realpath; the result is allocated from allocator.
    realpath: *const fn (*anyopaque, std.mem.Allocator, []const u8) PathError![]u8,
    /// Translate a canonical host path into a guest path with the device's mount rules.
    translate: *const fn (*anyopaque, std.mem.Allocator, []const u8) TranslateError![]u8,
    /// Environment lookup; the result is borrowed until the next hook call.
    getenv: *const fn (*anyopaque, []const u8) ?[]const u8,
    /// Monotonic clock in milliseconds.
    now_ms: *const fn (*anyopaque) u64,
    /// Sleep for the given milliseconds.
    sleep_ms: *const fn (*anyopaque, u64) void,
    /// Classify the runtime directory: must be private to this user when it exists.
    dir_state: *const fn (*anyopaque, []const u8) DirState,
    /// Connect to a session socket; returns the connected descriptor.
    connect: *const fn (*anyopaque, []const u8) ConnectError!c_int,
    /// Close a descriptor.
    close: *const fn (*anyopaque, c_int) void,
    /// Write every byte or fail.
    write_all: *const fn (*anyopaque, c_int, []const u8) IoError!void,
    /// Read some bytes; returns 0 at end of stream. timeout_ms < 0 waits forever.
    poll_read: *const fn (*anyopaque, c_int, []u8, i32) IoError!usize,
    /// Spawn waddle-session for the device, detached.
    spawn_session: *const fn (*anyopaque, []const u8) SpawnError!void,
    /// Best-effort desktop notification; never fails the command.
    notify: *const fn (*anyopaque, []const u8) void,
    /// Delegate a console executable to `waddle exec`; returns its exit code.
    delegate: *const fn (*anyopaque, []const u8, []const u8, []const []const u8) u8,
    /// Write one diagnostic line to stderr.
    message: *const fn (*anyopaque, []const u8) void,
};

/// One item to send to the session.
const Sent = struct {
    kind: open_request.item_kind_t,
    /// Guest target path, or the app key for App items.
    target: []const u8,
    /// Quoted argument tail, or null when the item has no tail.
    arguments: ?[]const u8,
};

/// Session connection and spawn bookkeeping for one invocation.
const Conn = struct {
    fd: c_int = -1,
    spawned: bool = false,
};

/// Outcome of waiting for the reply to one frame.
const Reply = union(enum) {
    /// Request accepted without a result: grace elapsed, or LaunchResult Ok.
    accepted: void,
    /// Final exit code for this item.
    exit: u8,
    /// Connection ended at a frame boundary; the next item must reconnect.
    closed: void,
};

/// Callback adapter state for open_request.parse.
const ParseCtx = struct {
    hooks: *const Hooks,
    missing: ?[]const u8 = null,
    unreadable: ?[]const u8 = null,
};

fn read_adapter(context: ?*anyopaque, path: [*]const u8, path_length: usize, buffer: [*]u8, capacity: usize, length_out: *usize) callconv(.C) c_int {
    _ = capacity;
    const parse_ctx: *ParseCtx = @ptrCast(@alignCast(context.?));
    const name = path[0..path_length];
    const out: *[open_request.PrefixBytes]u8 = @ptrCast(buffer);
    const read = parse_ctx.hooks.read_prefix(parse_ctx.hooks.ctx, name, out) catch |err| switch (err) {
        error.NotFound => {
            parse_ctx.missing = name;
            return open_request.ReadStatusNotFound;
        },
        error.Unreadable => {
            parse_ctx.unreadable = name;
            return 2;
        },
    };
    length_out.* = read;
    return open_request.ReadStatusOk;
}

/// Run the open command. Returns the process exit code; every allocation is released.
/// @param allocator Source of temporary memory; freed before return.
/// @param args Arguments after the `open` word.
/// @param hooks Effects; see Hooks.
/// @return Exit code per open_request_contract.md section 5.
pub fn run(allocator: std.mem.Allocator, args: []const []const u8, hooks: *const Hooks) u8 {
    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();
    const a = arena.allocator();

    var parse_ctx = ParseCtx{ .hooks = hooks };
    var options = open_request.options_t{
        .read_prefix = read_adapter,
        .read_context = &parse_ctx,
        .stdio_is_tty = @intFromBool(hooks.tty(hooks.ctx)),
    };
    var request = std.mem.zeroes(open_request.request_t);
    const status = open_request.parse(args, &options, &request);
    if (status != .None) return report_parse_error(hooks, a, status, &parse_ctx);

    var device: Device = undefined;
    const device_name: ?[]const u8 = if (request.device) |pointer| pointer[0..request.device_length] else null;
    if (hooks.resolve(hooks.ctx, device_name, &device)) |code| return code;

    var sent = std.ArrayList(Sent).init(a);
    var delegated: ?struct { guest: []const u8, tail: []const []const u8 } = null;
    var index: usize = 0;
    while (index < request.item_count) : (index += 1) {
        const item = request.items[index];
        if (item.kind == .App) {
            sent.append(.{ .kind = .App, .target = request.item_text(index), .arguments = null }) catch return 125;
            continue;
        }
        const host = request.item_text(index);
        const canonical = hooks.realpath(hooks.ctx, a, host) catch |err| switch (err) {
            error.NotFound => {
                hooks.message(hooks.ctx, std.fmt.allocPrint(a, "waddle open: '{s}': no such file", .{host}) catch return 125);
                return open_request.ExitNotFound;
            },
            error.Failed, error.OutOfMemory => return 125,
        };
        const guest = hooks.translate(hooks.ctx, a, canonical) catch |err| switch (err) {
            error.Outside => return report_outside(hooks, a, host, &device),
            error.Failed, error.OutOfMemory => return 125,
        };
        const tail = args[item.argument_first..][0..item.argument_count];
        if (item.kind == .DelegateExec) {
            delegated = .{ .guest = guest, .tail = tail };
            continue;
        }
        const arguments: ?[]const u8 = if (item.argument_count == 0) null else quote(a, tail) orelse return 125;
        sent.append(.{ .kind = item.kind, .target = guest, .arguments = arguments }) catch return 125;
    }

    if (delegated) |exec| return hooks.delegate(hooks.ctx, device.name, exec.guest, exec.tail);
    // Every item is either sent or delegated, and a request has at least one item.
    std.debug.assert(sent.items.len > 0);

    const token = pick_token(hooks);
    var conn = Conn{};
    if (connect_session(hooks, a, &device, &conn)) |code| return code;
    var result: u8 = 0;
    for (sent.items, 0..) |item, position| {
        if (conn.fd < 0) {
            if (connect_session(hooks, a, &device, &conn)) |code| {
                if (result == 0) result = code;
                break;
            }
        }
        const frame_token: ?[]const u8 = if (position == 0) token else null;
        const request_id: u32 = @intCast(position + 1);
        const frame = build_frame(a, item, frame_token, request_id) catch return 125;
        hooks.write_all(hooks.ctx, conn.fd, frame) catch {
            hooks.message(hooks.ctx, "waddle open: could not send the launch request");
            if (result == 0) result = 125;
            break;
        };
        const reply = await_reply(hooks, a, &conn, request_id, wait_mode(args)) catch {
            if (result == 0) result = 125;
            break;
        };
        switch (reply) {
            .accepted => {},
            .closed => {
                hooks.close(hooks.ctx, conn.fd);
                conn.fd = -1;
            },
            .exit => |code| if (result == 0) {
                result = code;
            },
        }
    }
    if (conn.fd >= 0) hooks.close(hooks.ctx, conn.fd);
    return result;
}

/// True when --wait appears among the flags before the target.
fn wait_mode(args: []const []const u8) bool {
    for (args) |arg| {
        if (std.mem.eql(u8, arg, "--wait")) return true;
        if (std.mem.eql(u8, arg, "--")) return false;
    }
    return false;
}

fn quote(a: std.mem.Allocator, tail: []const []const u8) ?[]const u8 {
    var buffer = a.alloc(u8, open_request.MaxQuotedBytes) catch return null;
    const length = open_request.quote_into(tail, buffer) catch return null;
    return buffer[0..length];
}

fn pick_token(hooks: *const Hooks) ?[]const u8 {
    return activation_store.pick_token(hooks.getenv(hooks.ctx, "XDG_ACTIVATION_TOKEN"), hooks.getenv(hooks.ctx, "DESKTOP_STARTUP_ID"));
}

fn report_parse_error(hooks: *const Hooks, a: std.mem.Allocator, status: open_request.open_error_t, parse_ctx: *const ParseCtx) u8 {
    const code = open_request.exit_code_for_error(status);
    const text: []const u8 = if (status == .NotFound)
        std.fmt.allocPrint(a, "waddle open: '{s}': no such file", .{parse_ctx.missing orelse ""}) catch return code
    else if (status == .ImageUnreadable)
        std.fmt.allocPrint(a, "waddle open: '{s}': cannot read the executable header", .{parse_ctx.unreadable orelse ""}) catch return code
    else
        std.fmt.allocPrint(a, "waddle open: invalid request (code {d})", .{@intFromEnum(status)}) catch return code;
    hooks.message(hooks.ctx, text);
    if (code == open_request.ExitUsage) hooks.message(hooks.ctx, UsageText);
    return code;
}

fn report_outside(hooks: *const Hooks, a: std.mem.Allocator, host: []const u8, device: *const Device) u8 {
    const text = std.fmt.allocPrint(a, "'{s}' is outside the folders shared with Windows ({s}). Add a share with: waddle device config set filesystem.<name> <dir>:<drive>:\"", .{ host, device.name }) catch return 126;
    hooks.message(hooks.ctx, text);
    if (device.notify) hooks.notify(hooks.ctx, text);
    return open_request.ExitNotExecutable;
}

/// Codec write status. Inputs are bounded by open_request (targets and tails at most 4096
/// bytes, tokens at most 255, one frame of InteropMaxFrameBytes), so a non-zero status is a
/// programming error and is asserted, not reported to the user.
fn codec_ok(code: c_int) void {
    std.debug.assert(code == 0);
}

/// Build one LocalLaunch frame. Fields are written in ascending tag order: key (0x08 or
/// 0x2F), arguments (0x30), launch_kind (0x36), activation_token (0x37).
fn build_frame(a: std.mem.Allocator, item: Sent, token: ?[]const u8, request_id: u32) error{OutOfMemory}![]const u8 {
    const buffer = try a.alloc(u8, c.InteropMaxFrameBytes);
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, buffer.ptr, buffer.len);
    const key_tag: u16 = if (item.kind == .App) c.InteropTagAppKey else c.InteropTagTargetPath;
    codec_ok(c.interop_put_bytes(&writer, key_tag, item.target.ptr, @intCast(item.target.len)));
    if (item.arguments) |arguments| {
        codec_ok(c.interop_put_bytes(&writer, c.InteropTagArguments, arguments.ptr, @intCast(arguments.len)));
    }
    const wire = item.kind.wire_launch_kind() orelse unreachable;
    codec_ok(c.interop_put_uint(&writer, c.InteropTagLaunchKind, wire));
    if (token) |value| {
        codec_ok(c.interop_put_bytes(&writer, c.InteropTagActivationToken, value.ptr, @intCast(value.len)));
    }
    var length: usize = 0;
    codec_ok(c.interop_writer_finish(&writer, c.InteropMsgLocalLaunch, request_id, c.InteropFromLocal, &length));
    return buffer[0..length];
}

fn connect_session(hooks: *const Hooks, a: std.mem.Allocator, device: *const Device, conn: *Conn) ?u8 {
    const dir = std.fs.path.dirname(device.session_path) orelse unreachable;
    const start = hooks.now_ms(hooks.ctx);
    const deadline = start + DeadlineMs;
    var backoff: u64 = BackoffMinMs;
    var notice_sent = false;
    while (true) {
        switch (hooks.dir_state(hooks.ctx, dir)) {
            .unsafe => {
                hooks.message(hooks.ctx, "waddle open: the runtime directory is not private to this user; refusing to connect");
                return 125;
            },
            .missing, .ok => {},
        }
        if (hooks.connect(hooks.ctx, device.session_path)) |fd| {
            conn.fd = fd;
            return null;
        } else |err| switch (err) {
            error.NotFound, error.Refused => {},
            error.Failed => {
                hooks.message(hooks.ctx, "waddle open: could not connect to the session socket");
                return 125;
            },
        }
        if (!conn.spawned) {
            conn.spawned = true;
            hooks.spawn_session(hooks.ctx, device.name) catch {
                hooks.message(hooks.ctx, "waddle open: could not start waddle-session");
                return 125;
            };
        }
        const now = hooks.now_ms(hooks.ctx);
        if (!notice_sent and device.notify and now - start >= NoticeAfterMs) {
            hooks.notify(hooks.ctx, StartingMessage);
            notice_sent = true;
        }
        if (now >= deadline) {
            hooks.message(hooks.ctx, std.fmt.allocPrint(a, "waddle: Windows ({s}) did not start", .{device.name}) catch "waddle: Windows did not start");
            return 125;
        }
        hooks.sleep_ms(hooks.ctx, backoff);
        backoff = @min(backoff * 2, BackoffMaxMs);
    }
}

const ReadExact = enum { ok, eof_boundary, eof_partial, timeout, failed };

fn read_exact(hooks: *const Hooks, fd: c_int, buffer: []u8, timeout_ms: i32) ReadExact {
    var filled: usize = 0;
    while (filled < buffer.len) {
        const read = hooks.poll_read(hooks.ctx, fd, buffer[filled..], timeout_ms) catch |err| switch (err) {
            error.Timeout => return .timeout,
            error.Failed => return .failed,
        };
        if (read == 0) return if (filled == 0) .eof_boundary else .eof_partial;
        filled += read;
    }
    return .ok;
}

/// Wait for the LaunchResult for request_id. Frames for other request ids are skipped.
/// @return Reply outcome, or error.Failed for a protocol error or a connection lost mid-reply.
fn await_reply(hooks: *const Hooks, a: std.mem.Allocator, conn: *Conn, request_id: u32, wait: bool) error{Failed}!Reply {
    const timeout: i32 = if (wait) -1 else GraceMs;
    while (true) {
        var header_bytes: [c.InteropHeaderBytes]u8 = undefined;
        switch (read_exact(hooks, conn.fd, &header_bytes, timeout)) {
            .ok => {},
            .eof_boundary => return if (wait) error.Failed else Reply{ .closed = {} },
            .timeout => return Reply{ .accepted = {} },
            .eof_partial, .failed => return error.Failed,
        }
        var header: c.interop_header_t = undefined;
        if (c.interop_header_decode(&header_bytes, header_bytes.len, &header) != 0) return error.Failed;
        // interop_header_decode has already bounded header.length to InteropMaxPayloadBytes.
        const payload = a.alloc(u8, header.length) catch return error.Failed;
        switch (read_exact(hooks, conn.fd, payload, timeout)) {
            .ok => {},
            .timeout => return Reply{ .accepted = {} },
            .eof_boundary, .eof_partial, .failed => return error.Failed,
        }
        if (header.request_id != request_id) continue;
        if (header.type == c.InteropMsgError) {
            var message: ?[]const u8 = null;
            var value: ?[*]const u8 = null;
            var value_length: u16 = 0;
            if (c.interop_field_get(payload.ptr, payload.len, c.InteropTagMessage, 0, &value, &value_length) == 0) {
                message = value.?[0..value_length];
            }
            hooks.message(hooks.ctx, std.fmt.allocPrint(a, "waddle open: launch refused: {s}", .{message orelse "no message"}) catch "waddle open: launch refused");
            return Reply{ .exit = 1 };
        }
        if (header.type != c.InteropMsgLaunchResult) continue;
        if (c.interop_payload_validate(header.type, c.InteropFromGuest, payload.ptr, payload.len) != 0) return error.Failed;
        var status: u64 = 0;
        if (c.interop_field_uint(payload.ptr, payload.len, c.InteropTagStatus, 4, &status) != 0) return error.Failed;
        if (status == 0) return Reply{ .accepted = {} };
        var value: ?[*]const u8 = null;
        var value_length: u16 = 0;
        const message: []const u8 = if (c.interop_field_get(payload.ptr, payload.len, c.InteropTagMessage, 0, &value, &value_length) == 0) value.?[0..value_length] else "launch failed";
        hooks.message(hooks.ctx, std.fmt.allocPrint(a, "waddle open: {s}", .{message}) catch "waddle open: launch failed");
        return Reply{ .exit = open_request.exit_code_for_launch_status(@intCast(status)) };
    }
}

/// C ABI: run `waddle open` with the production hooks.
/// @param[in] argc Number of arguments after `open`.
/// @param[in] argv Arguments after `open`, readable for argc entries.
/// @param[in] exec_fn Non-NULL delegate (cmd_exec).
/// @param[in] resolve_fn Non-NULL resolver (resolve_target_device).
/// @return Exit code.
pub export fn waddle_open_command(argc: c_int, argv: ?[*][*:0]u8, exec_fn: exec_fn_t, resolve_fn: resolve_fn_t) c_int {
    const delegate_exec = exec_fn orelse return 125;
    const resolver = resolve_fn orelse return 125;
    if (argc < 0 or argc > open_request.MaxArgs) return 2;
    if (argc > 0 and argv == null) return 2;
    var storage: [open_request.MaxArgs][]const u8 = undefined;
    const count: usize = @intCast(argc);
    for (0..count) |index| storage[index] = std.mem.span(argv.?[index]);
    var real = RealHooks.init(delegate_exec, resolver);
    const hooks = real.hooks();
    return run(std.heap.c_allocator, storage[0..count], &hooks);
}

// Production hooks. Each function receives the RealHooks context.
const RealHooks = struct {
    delegate_exec: exec_fn_t,
    resolver: resolve_fn_t,
    config: c.daemon_config_t = undefined,
    config_loaded: bool = false,
    name_buffer: [c.WaddleMaxDeviceNameLen]u8 = undefined,
    session_buffer: [c.WaddleMaxPathLen]u8 = undefined,
    socket_buffer: [c.WaddleMaxPathLen]u8 = undefined,

    fn init(delegate_exec: exec_fn_t, resolver: resolve_fn_t) RealHooks {
        return .{ .delegate_exec = delegate_exec, .resolver = resolver };
    }

    fn hooks(self: *RealHooks) Hooks {
        return .{
            .ctx = self,
            .tty = tty,
            .resolve = resolve,
            .read_prefix = read_prefix,
            .realpath = realpath,
            .translate = translate,
            .getenv = getenv,
            .now_ms = now_ms,
            .sleep_ms = sleep_ms,
            .dir_state = dir_state,
            .connect = connect,
            .close = close,
            .write_all = write_all,
            .poll_read = poll_read,
            .spawn_session = spawn_session,
            .notify = notify,
            .delegate = delegate,
            .message = message,
        };
    }

    fn of(ctx: *anyopaque) *RealHooks {
        return @ptrCast(@alignCast(ctx));
    }

    fn tty(ctx: *anyopaque) bool {
        _ = ctx;
        return std.posix.isatty(0) and std.posix.isatty(1);
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
        const device_name = std.mem.sliceTo(&info.name, 0);
        const socket = std.mem.sliceTo(&self.socket_buffer, 0);
        // The socket path comes from the device registry, which always places it in a directory.
    const directory = std.fs.path.dirname(socket) orelse unreachable;
        const session = std.fmt.bufPrintZ(&self.session_buffer, "{s}/{s}", .{ directory, SessionSocketName }) catch return 125;
        const config_path = std.mem.sliceTo(&info.config_path, 0);
        c.daemon_config_init_defaults(&self.config);
        if (c.daemon_config_load_file(&self.config, if (config_path.len > 0) config_path.ptr else null) != 0) {
            return 125;
        }
        self.config_loaded = true;
        out.* = .{ .name = device_name, .session_path = session, .notify = self.config.integration_notify != 0 };
        return null;
    }

    fn read_prefix(ctx: *anyopaque, path: []const u8, buffer: *[open_request.PrefixBytes]u8) ReadError!usize {
        _ = ctx;
        var file = std.fs.cwd().openFile(path, .{}) catch |err| return switch (err) {
            error.FileNotFound => error.NotFound,
            else => error.Unreadable,
        };
        defer file.close();
        return file.read(buffer) catch error.Unreadable;
    }

    fn realpath(ctx: *anyopaque, a: std.mem.Allocator, path: []const u8) PathError![]u8 {
        _ = ctx;
        var buffer: [std.fs.MAX_PATH_BYTES]u8 = undefined;
        const resolved = std.posix.realpath(path, &buffer) catch |err| return switch (err) {
            error.FileNotFound => error.NotFound,
            else => error.Failed,
        };
        return a.dupe(u8, resolved) catch error.OutOfMemory;
    }

    fn translate(ctx: *anyopaque, a: std.mem.Allocator, path: []const u8) TranslateError![]u8 {
        const self = of(ctx);
        const zpath = a.dupeZ(u8, path) catch return error.OutOfMemory;
        defer a.free(zpath);
        const mapped = c.daemon_fs_translate_path(zpath.ptr, &self.config) orelse {
            // EXDEV: no export contains the path; EINVAL: invalid input. Both mean outside every share.
            const code = std.c._errno().*;
            return if (code == c.EXDEV or code == c.EINVAL) error.Outside else if (code == c.ENOMEM) error.OutOfMemory else error.Failed;
        };
        defer c.free(mapped);
        return a.dupe(u8, std.mem.span(mapped)) catch error.OutOfMemory;
    }

    fn getenv(ctx: *anyopaque, name: []const u8) ?[]const u8 {
        _ = ctx;
        return std.posix.getenv(name);
    }

    fn now_ms(ctx: *anyopaque) u64 {
        _ = ctx;
        const instant = std.time.Instant.now() catch return 0;
        return @as(u64, @intCast(instant.timestamp.tv_sec)) * 1000 + @as(u64, @intCast(instant.timestamp.tv_nsec)) / 1_000_000;
    }

    fn sleep_ms(ctx: *anyopaque, ms: u64) void {
        _ = ctx;
        std.time.sleep(ms * std.time.ns_per_ms);
    }

    fn dir_state(ctx: *anyopaque, dir: []const u8) DirState {
        _ = ctx;
        const st = std.posix.fstatat(std.posix.AT.FDCWD, dir, std.posix.AT.SYMLINK_NOFOLLOW) catch |err| return switch (err) {
            error.FileNotFound => .missing,
            else => .unsafe,
        };
        const is_dir = (st.mode & std.posix.S.IFMT) == std.posix.S.IFDIR;
        if (!is_dir or st.uid != std.os.linux.getuid() or (st.mode & 0o777) != 0o700) return .unsafe;
        return .ok;
    }

    fn connect(ctx: *anyopaque, path: []const u8) ConnectError!c_int {
        _ = ctx;
        const addr = std.net.Address.initUnix(path) catch return error.Failed;
        const fd = std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM | std.posix.SOCK.CLOEXEC, 0) catch return error.Failed;
        std.posix.connect(fd, &addr.any, addr.getOsSockLen()) catch |err| {
            std.posix.close(fd);
            return switch (err) {
                error.FileNotFound => error.NotFound,
                error.ConnectionRefused => error.Refused,
                else => error.Failed,
            };
        };
        return fd;
    }

    fn close(ctx: *anyopaque, fd: c_int) void {
        _ = ctx;
        std.posix.close(fd);
    }

    fn write_all(ctx: *anyopaque, fd: c_int, bytes: []const u8) IoError!void {
        _ = ctx;
        var sent: usize = 0;
        while (sent < bytes.len) {
            sent += std.posix.write(fd, bytes[sent..]) catch return error.Failed;
        }
    }

    fn poll_read(ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) IoError!usize {
        _ = ctx;
        var fds = [_]std.posix.pollfd{.{ .fd = fd, .events = std.posix.POLL.IN, .revents = 0 }};
        const ready = std.posix.poll(&fds, timeout_ms) catch return error.Failed;
        if (ready == 0) return error.Timeout;
        return std.posix.read(fd, buffer) catch error.Failed;
    }

    fn spawn_session(ctx: *anyopaque, device: []const u8) SpawnError!void {
        _ = ctx;
        // Device names are validated to at most 63 bytes before they reach this hook.
        var name_z: [c.WaddleMaxDeviceNameLen]u8 = undefined;
        const name = std.fmt.bufPrintZ(&name_z, "{s}", .{device}) catch unreachable;
        var argv_storage = [_:null]?[*:0]const u8{ "waddle-session", "--device", name.ptr };
        try spawn_detached("waddle-session", &argv_storage);
    }

    fn notify(ctx: *anyopaque, text: []const u8) void {
        _ = ctx;
        var buffer: [1024]u8 = undefined;
        const z = std.fmt.bufPrintZ(&buffer, "{s}", .{text}) catch return;
        var argv_storage = [_:null]?[*:0]const u8{ "notify-send", "--app-name=waddle", z.ptr };
        spawn_detached("notify-send", &argv_storage) catch {};
    }

    fn delegate(ctx: *anyopaque, device: []const u8, guest: []const u8, tail: []const []const u8) u8 {
        const self = of(ctx);
        // Each item is at most 4096 bytes (argv bound), so a 4097-byte buffer always fits.
        var buffers: [open_request.MaxArgs + 8][open_request.MaxArgBytes + 1]u8 = undefined;
        var pointers: [open_request.MaxArgs + 8:null]?[*:0]u8 = undefined;
        var count: usize = 0;
        const fixed = [_][]const u8{ "waddle", "exec", "--device", device, "--translate-path", "--", guest };
        for (fixed) |item| if (!push_arg(&buffers, &pointers, &count, item)) return 125;
        for (tail) |item| if (!push_arg(&buffers, &pointers, &count, item)) return 125;
        pointers[count] = null;
        const argv_pointer: [*c][*c]u8 = @ptrCast(&pointers);
        return @intCast(self.delegate_exec.?(@intCast(count), argv_pointer, 2));
    }

    /// Copy one argument into its own buffer and append its pointer; false when it does not fit.
    fn push_arg(buffers: anytype, pointers: anytype, count: *usize, item: []const u8) bool {
        const z = std.fmt.bufPrintZ(&buffers[count.*], "{s}", .{item}) catch return false;
        pointers[count.*] = z.ptr;
        count.* += 1;
        return true;
    }

    fn message(ctx: *anyopaque, text: []const u8) void {
        _ = ctx;
        const stderr = std.io.getStdErr();
        stderr.writeAll(text) catch {};
        stderr.writeAll("\n") catch {};
    }
};

extern "c" fn posix_spawnp(pid: *std.c.pid_t, file: [*:0]const u8, actions: ?*anyopaque, attr: ?*anyopaque, argv: [*:null]const ?[*:0]const u8, envp: [*:null]const ?[*:0]const u8) c_int;
extern "c" fn posix_spawnattr_init(attr: *anyopaque) c_int;
extern "c" fn posix_spawnattr_setflags(attr: *anyopaque, flags: c_short) c_int;
extern "c" fn posix_spawnattr_destroy(attr: *anyopaque) c_int;
extern "c" fn posix_spawn_file_actions_init(actions: *anyopaque) c_int;
extern "c" fn posix_spawn_file_actions_addopen(actions: *anyopaque, fd: c_int, path: [*:0]const u8, oflag: c_int, mode: std.c.mode_t) c_int;

/// glibc flag: start the child in a new session (POSIX_SPAWN_SETSID).
const SpawnSetsid: c_short = 0x80;

/// Spawn a program fully detached: new session, stdio on /dev/null, no wait.
fn spawn_detached(program: [*:0]const u8, argv: [*:null]const ?[*:0]const u8) SpawnError!void {
    var attr: [1024]u8 align(16) = undefined;
    var actions: [512]u8 align(16) = undefined;
    // The attribute and action objects are initialized in place; these calls cannot fail for
    // valid buffers, so their status is asserted.
    std.debug.assert(posix_spawnattr_init(&attr) == 0);
    defer _ = posix_spawnattr_destroy(&attr);
    std.debug.assert(posix_spawnattr_setflags(&attr, SpawnSetsid) == 0);
    std.debug.assert(posix_spawn_file_actions_init(&actions) == 0);
    for (0..3) |fd| {
        std.debug.assert(posix_spawn_file_actions_addopen(&actions, @intCast(fd), "/dev/null", c.O_RDWR, 0) == 0);
    }
    var pid: std.c.pid_t = 0;
    if (posix_spawnp(&pid, program, &actions, &attr, argv, @ptrCast(std.c.environ)) != 0) return error.Failed;
}

// Test-only fixtures.
const testing = std.testing;

const FakeFile = struct { path: []const u8, status: ReadError!void = {}, bytes: []const u8 = &.{} };

/// Scripted effects for orchestration tests. Records every write, notice and delegation.
const Fake = struct {
    tty_value: bool = false,
    resolve_status: ?u8 = null,
    device: Device = .{ .name = "dev1", .session_path = "/run/u/waddle/dev1/session.sock", .notify = false },
    files: []const FakeFile = &.{},
    share_prefix: []const u8 = "/share/",
    env_xdg: ?[]const u8 = null,
    env_startup: ?[]const u8 = null,
    now: u64 = 1000,
    dir: DirState = .ok,
    connect_script: []const ConnectError!c_int = &.{error.NotFound},
    connect_calls: usize = 0,
    closed: usize = 0,
    /// Bytes delivered by poll_read, then end_mode.
    incoming: []const u8 = &.{},
    cursor: usize = 0,
    end_timeout: bool = false,
    fail_write: bool = false,
    fail_realpath: bool = false,
    written: [8192]u8 = undefined,
    written_len: usize = 0,
    spawns: usize = 0,
    spawn_fails: bool = false,
    notices: usize = 0,
    notice_text: [64]u8 = undefined,
    notice_len: usize = 0,
    delegated: usize = 0,
    delegate_result: u8 = 0,
    delegate_guest: [256]u8 = undefined,
    delegate_guest_len: usize = 0,
    delegate_args: usize = 0,
    messages: [16][256]u8 = undefined,
    message_lens: [16]usize = [_]usize{0} ** 16,
    message_count: usize = 0,
    notify_text: [256]u8 = undefined,
    notify_len: usize = 0,
    notify_count: usize = 0,

    fn message_at(self: *const Fake, index: usize) []const u8 {
        return self.messages[index][0..self.message_lens[index]];
    }

    fn of(ctx: *anyopaque) *Fake {
        return @ptrCast(@alignCast(ctx));
    }

    fn hooks(self: *Fake) Hooks {
        return .{
            .ctx = self,
            .tty = fake_tty,
            .resolve = fake_resolve,
            .read_prefix = fake_read_prefix,
            .realpath = fake_realpath,
            .translate = fake_translate,
            .getenv = fake_getenv,
            .now_ms = fake_now,
            .sleep_ms = fake_sleep,
            .dir_state = fake_dir,
            .connect = fake_connect,
            .close = fake_close,
            .write_all = fake_write,
            .poll_read = fake_poll,
            .spawn_session = fake_spawn,
            .notify = fake_notify,
            .delegate = fake_delegate,
            .message = fake_message,
        };
    }
};

/// Hide a constant from the optimizer so the test copies happen at run time.
fn hide_int(value: c_int) c_int {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

fn hide_bytes(bytes: []const u8) []const u8 {
    var copy = bytes;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

fn fake_tty(ctx: *anyopaque) bool {
    return Fake.of(ctx).tty_value;
}

fn fake_resolve(ctx: *anyopaque, name: ?[]const u8, out: *Device) ?u8 {
    const f = Fake.of(ctx);
    _ = name;
    if (f.resolve_status) |code| return code;
    out.* = f.device;
    return null;
}

fn fake_read_prefix(ctx: *anyopaque, path: []const u8, buffer: *[open_request.PrefixBytes]u8) ReadError!usize {
    for (Fake.of(ctx).files) |file| {
        if (!std.mem.eql(u8, file.path, path)) continue;
        try file.status;
        @memcpy(buffer[0..file.bytes.len], file.bytes);
        return file.bytes.len;
    }
    return error.NotFound;
}

fn fake_realpath(ctx: *anyopaque, a: std.mem.Allocator, path: []const u8) PathError![]u8 {
    if (Fake.of(ctx).fail_realpath) return error.Failed;
    if (std.mem.startsWith(u8, path, "/missing/")) return error.NotFound;
    return a.dupe(u8, path) catch error.OutOfMemory;
}

fn fake_translate(ctx: *anyopaque, a: std.mem.Allocator, path: []const u8) TranslateError![]u8 {
    const f = Fake.of(ctx);
    if (!std.mem.startsWith(u8, path, f.share_prefix)) return error.Outside;
    const rest = path[f.share_prefix.len..];
    const guest = std.fmt.allocPrint(a, "Z:\\{s}", .{rest}) catch return error.OutOfMemory;
    for (guest) |*byte| {
        if (byte.* == '/') byte.* = '\\';
    }
    return guest;
}

fn fake_getenv(ctx: *anyopaque, name: []const u8) ?[]const u8 {
    const f = Fake.of(ctx);
    if (std.mem.eql(u8, name, "XDG_ACTIVATION_TOKEN")) return f.env_xdg;
    if (std.mem.eql(u8, name, "DESKTOP_STARTUP_ID")) return f.env_startup;
    return null;
}

fn fake_now(ctx: *anyopaque) u64 {
    return Fake.of(ctx).now;
}

fn fake_sleep(ctx: *anyopaque, ms: u64) void {
    Fake.of(ctx).now += ms;
}

fn fake_dir(ctx: *anyopaque, dir: []const u8) DirState {
    _ = dir;
    return Fake.of(ctx).dir;
}

fn fake_connect(ctx: *anyopaque, path: []const u8) ConnectError!c_int {
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

fn fake_write(ctx: *anyopaque, fd: c_int, bytes: []const u8) IoError!void {
    _ = fd;
    const f = Fake.of(ctx);
    if (f.fail_write) return error.Failed;
    @memcpy(f.written[f.written_len..][0..bytes.len], bytes);
    f.written_len += bytes.len;
}

fn fake_poll(ctx: *anyopaque, fd: c_int, buffer: []u8, timeout_ms: i32) IoError!usize {
    _ = fd;
    _ = timeout_ms;
    const f = Fake.of(ctx);
    if (f.cursor == f.incoming.len) {
        if (f.end_timeout) return error.Timeout;
        return 0;
    }
    const count = @min(buffer.len, f.incoming.len - f.cursor);
    @memcpy(buffer[0..count], f.incoming[f.cursor..][0..count]);
    f.cursor += count;
    return count;
}

fn fake_spawn(ctx: *anyopaque, device: []const u8) SpawnError!void {
    _ = device;
    const f = Fake.of(ctx);
    f.spawns += 1;
    if (f.spawn_fails) return error.Failed;
}

fn fake_notify(ctx: *anyopaque, text: []const u8) void {
    const f = Fake.of(ctx);
    f.notify_count += 1;
    if (f.notices == 0 and std.mem.eql(u8, text, StartingMessage)) f.notices += 1;
    @memcpy(f.notify_text[0..text.len], text);
    f.notify_len = text.len;
}

fn fake_delegate(ctx: *anyopaque, device: []const u8, guest: []const u8, tail: []const []const u8) u8 {
    _ = device;
    const f = Fake.of(ctx);
    f.delegated += 1;
    @memcpy(f.delegate_guest[0..guest.len], guest);
    f.delegate_guest_len = guest.len;
    f.delegate_args = tail.len;
    return f.delegate_result;
}

fn fake_message(ctx: *anyopaque, text: []const u8) void {
    const f = Fake.of(ctx);
    if (f.message_count == f.messages.len) return;
    @memcpy(f.messages[f.message_count][0..text.len], text);
    f.message_lens[f.message_count] = text.len;
    f.message_count += 1;
}

/// A 256-byte PE32 header prefix with the given subsystem (2 GUI, 3 CUI).
fn pe_bytes(buffer: *[256]u8, subsystem: u16) []const u8 {
    @memset(buffer, 0);
    buffer[0] = 'M';
    buffer[1] = 'Z';
    buffer[0x3c] = 0x80;
    @memcpy(buffer[0x80..0x84], "PE\x00\x00");
    buffer[0x94] = 240;
    buffer[0x98] = 0x0b;
    buffer[0x99] = 0x01;
    buffer[0xdc] = @intCast(subsystem);
    buffer[0xdd] = @intCast(subsystem >> 8);
    return buffer;
}

/// LaunchResult frame with a status and optional message, both written through the codec.
fn launch_result(buffer: []u8, request_id: u32, status: u32, message: ?[]const u8) []const u8 {
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, buffer.ptr, buffer.len);
    _ = c.interop_put_uint(&writer, c.InteropTagProcessId, 4242);
    _ = c.interop_put_uint(&writer, c.InteropTagStatus, status);
    if (message) |text| _ = c.interop_put_bytes(&writer, c.InteropTagMessage, text.ptr, @intCast(text.len));
    var length: usize = 0;
    _ = c.interop_writer_finish(&writer, c.InteropMsgLaunchResult, request_id, c.InteropFromGuest, &length);
    return buffer[0..length];
}

fn error_frame(buffer: []u8, request_id: u32, code: u32, message: ?[]const u8) []const u8 {
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, buffer.ptr, buffer.len);
    // Message (0x39) precedes ErrorCode (0x46): fields are written in ascending tag order.
    if (message) |text| _ = c.interop_put_bytes(&writer, c.InteropTagMessage, text.ptr, @intCast(text.len));
    _ = c.interop_put_uint(&writer, c.InteropTagErrorCode, code);
    var length: usize = 0;
    _ = c.interop_writer_finish(&writer, c.InteropMsgError, request_id, c.InteropFromGuest, &length);
    return buffer[0..length];
}

/// Decode the first frame written by the client: returns type, request id and the payload view.
const Written = struct { kind: u16, request_id: u32, payload: []const u8 };

fn written_frame(f: *const Fake, index: usize) ?Written {
    var offset: usize = 0;
    var frame_index: usize = 0;
    while (offset + c.InteropHeaderBytes <= f.written_len) {
        var header: c.interop_header_t = undefined;
        if (c.interop_header_decode(&f.written[offset], c.InteropHeaderBytes, &header) != 0) return null;
        const total = c.InteropHeaderBytes + header.length;
        if (frame_index == index) return .{ .kind = header.type, .request_id = header.request_id, .payload = f.written[offset + c.InteropHeaderBytes .. offset + total] };
        offset += total;
        frame_index += 1;
    }
    return null;
}

fn payload_text(payload: []const u8, tag: u16) ?[]const u8 {
    var value: ?[*]const u8 = null;
    var length: u16 = 0;
    if (c.interop_field_get(payload.ptr, payload.len, tag, 0, &value, &length) != 0) return null;
    return value.?[0..length];
}

fn payload_uint(payload: []const u8, tag: u16) ?u64 {
    var value: u64 = 0;
    if (c.interop_field_uint(payload.ptr, payload.len, tag, 1, &value) != 0) return null;
    return value;
}

/// Scripted reply stream: LaunchResult frames for each request id, in order.
fn stream(allocator: std.mem.Allocator, parts: []const []const u8) ![]u8 {
    var total: usize = 0;
    for (parts) |part| total += part.len;
    const out = try allocator.alloc(u8, total);
    var offset: usize = 0;
    for (parts) |part| {
        @memcpy(out[offset..][0..part.len], part);
        offset += part.len;
    }
    return out;
}

fn run_fake(fake: *Fake, args: []const []const u8) u8 {
    // Route the arguments through memory the optimizer cannot see into, so the branches on
    // them are run-time decisions in the instrumented binary.
    var storage: [open_request.MaxArgs + 1][]const u8 = undefined;
    @memcpy(storage[0..args.len], args);
    std.mem.doNotOptimizeAway(&storage);
    var hooks = fake.hooks();
    return run(testing.allocator, storage[0..args.len], &hooks);
}

test "warm path: one Path item launches, frame carries kind, target and request id" {
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const incoming = try stream(testing.allocator, &.{ok});
    defer testing.allocator.free(incoming);
    const files = [_]FakeFile{};
    var fake = Fake{ .files = &files, .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{"/share/notes.txt"}));
    try testing.expectEqual(@as(usize, 1), fake.connect_calls);
    try testing.expectEqual(@as(usize, 0), fake.spawns);
    try testing.expectEqual(@as(usize, 1), fake.closed);
    const frame = written_frame(&fake, 0).?;
    try testing.expectEqual(@as(u16, 0x7f00), frame.kind);
    try testing.expectEqual(@as(u32, 1), frame.request_id);
    try testing.expectEqual(@as(u64, 2), payload_uint(frame.payload, c.InteropTagLaunchKind).?);
    try testing.expectEqualSlices(u8, "Z:\\notes.txt", payload_text(frame.payload, c.InteropTagTargetPath).?);
    try testing.expect(payload_text(frame.payload, c.InteropTagActivationToken) == null);
}

test "activation token attaches to the first launched item only" {
    var reply: [512]u8 = undefined;
    const first = launch_result(reply[0..256], 1, 0, null);
    const second = launch_result(reply[256..512], 2, 0, null);
    const incoming = try stream(testing.allocator, &.{ first, second });
    defer testing.allocator.free(incoming);
    var fake = Fake{ .incoming = incoming, .env_xdg = "tok-1", .env_startup = "ignored" };
    fake.connect_script = &.{@as(c_int, 7)};
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{ "/share/a.txt", "/share/b.txt" }));
    const one = written_frame(&fake, 0).?;
    const two = written_frame(&fake, 1).?;
    try testing.expectEqualSlices(u8, "tok-1", payload_text(one.payload, c.InteropTagActivationToken).?);
    try testing.expect(payload_text(two.payload, c.InteropTagActivationToken) == null);
    try testing.expectEqual(@as(u32, 2), two.request_id);
}

test "startup id is used only when the xdg token is absent or invalid" {
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const incoming = try stream(testing.allocator, &.{ok});
    defer testing.allocator.free(incoming);
    var fake = Fake{ .incoming = incoming, .env_xdg = "bad token", .env_startup = "startup-9" };
    fake.connect_script = &.{@as(c_int, 7)};
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqualSlices(u8, "startup-9", payload_text(written_frame(&fake, 0).?.payload, c.InteropTagActivationToken).?);
}

test "cold path spawns waddle-session once, retries with backoff, and sends one notice after 1.5 s" {
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const incoming = try stream(testing.allocator, &.{ok});
    defer testing.allocator.free(incoming);
    var fake = Fake{
        .incoming = incoming,
        .device = .{ .name = "dev1", .session_path = "/run/u/waddle/dev1/session.sock", .notify = true },
        .connect_script = &(.{error.NotFound} ** 10 ++ .{@as(c_int, 9)}),
    };
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 1), fake.spawns);
    try testing.expectEqual(@as(usize, 1), fake.notices);
    try testing.expectEqualSlices(u8, StartingMessage, fake.notify_text[0..fake.notify_len]);
    try testing.expect(fake.now >= 1000 + NoticeAfterMs);
}

test "notice is not sent when integration.notify is off, and not sent when the connection is quick" {
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const incoming = try stream(testing.allocator, &.{ok});
    defer testing.allocator.free(incoming);
    var quiet = Fake{ .incoming = incoming, .connect_script = &.{ error.NotFound, @as(c_int, 9) } };
    try testing.expectEqual(@as(u8, 0), run_fake(&quiet, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 0), quiet.notice_len);
    var quick = Fake{ .incoming = incoming, .device = .{ .name = "dev1", .session_path = "/run/u/waddle/dev1/session.sock", .notify = true }, .connect_script = &.{ error.NotFound, @as(c_int, 9) } };
    try testing.expectEqual(@as(u8, 0), run_fake(&quick, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 0), quick.notify_count);
}

test "deadline: 120 s without a session exits 125 with a message and spawns once" {
    var fake = Fake{ .connect_script = &.{error.Refused} };
    try testing.expectEqual(@as(u8, 125), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 1), fake.spawns);
    try testing.expect(fake.now >= 1000 + DeadlineMs);
    try testing.expectEqualSlices(u8, "waddle: Windows (dev1) did not start", fake.message_at(fake.message_count - 1));
}

test "spawn failure and connect failure exit 125 with a message" {
    var spawn_fail = Fake{ .connect_script = &.{error.NotFound}, .spawn_fails = true };
    try testing.expectEqual(@as(u8, 125), run_fake(&spawn_fail, &.{"/share/a.txt"}));
    var connect_fail = Fake{ .connect_script = &.{error.Failed} };
    try testing.expectEqual(@as(u8, 125), run_fake(&connect_fail, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 0), connect_fail.spawns);
}

test "an unsafe runtime directory is refused before any connect" {
    var fake = Fake{ .dir = .unsafe };
    try testing.expectEqual(@as(u8, 125), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
    try testing.expectEqual(@as(usize, 0), fake.spawns);
}

test "a path outside every share exits 126 with the exact message and notifies when enabled" {
    var fake = Fake{ .device = .{ .name = "dev1", .session_path = "/run/u/waddle/dev1/session.sock", .notify = true } };
    try testing.expectEqual(@as(u8, 126), run_fake(&fake, &.{"/etc/hosts"}));
    const expected = "'/etc/hosts' is outside the folders shared with Windows (dev1). Add a share with: waddle device config set filesystem.<name> <dir>:<drive>:\"";
    try testing.expectEqualSlices(u8, expected, fake.message_at(0));
    try testing.expectEqualSlices(u8, expected, fake.notify_text[0..fake.notify_len]);
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
    var quiet = Fake{};
    try testing.expectEqual(@as(u8, 126), run_fake(&quiet, &.{"/etc/hosts"}));
    try testing.expectEqual(@as(usize, 0), quiet.notify_count);
}

test "missing and unreadable files map to 127 and 126" {
    const files = [_]FakeFile{ .{ .path = "/share/bad.exe", .status = error.Unreadable } };
    var fake = Fake{ .files = &files };
    try testing.expectEqual(@as(u8, 126), run_fake(&fake, &.{"/share/bad.exe"}));
    try testing.expectEqualSlices(u8, "waddle open: '/share/bad.exe': cannot read the executable header", fake.message_at(0));
    try testing.expectEqual(@as(u8, 127), run_fake(&fake, &.{"/share/gone.exe"}));
    try testing.expectEqualSlices(u8, "waddle open: '/share/gone.exe': no such file", fake.message_at(1));
    try testing.expectEqual(@as(u8, 127), run_fake(&fake, &.{"/missing/notes.txt"}));
}

test "usage errors exit 2 with the usage line and reach no effect" {
    var fake = Fake{};
    try testing.expectEqual(@as(u8, 2), run_fake(&fake, &.{}));
    try testing.expectEqual(@as(u8, 2), run_fake(&fake, &.{ "/share/a.txt", "--wait" }));
    try testing.expectEqualSlices(u8, UsageText, fake.message_at(fake.message_count - 1));
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
    try testing.expectEqual(@as(u8, 2), run_fake(&fake, &.{ "--app", "a-x", "/share/a.txt" }));
}

test "device resolution failure returns the resolver's code without connecting" {
    var fake = Fake{ .resolve_status = 2 };
    try testing.expectEqual(@as(u8, 2), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
}

test "with --wait, LaunchResult statuses map to exit codes and carry the guest message" {
    const cases = [_]struct { status: u32, exit: u8 }{ .{ .status = 0, .exit = 0 }, .{ .status = 1, .exit = 1 }, .{ .status = 9, .exit = 1 } };
    for (cases) |case| {
        var reply: [256]u8 = undefined;
        const result = launch_result(&reply, 1, case.status, "guest said no");
        const incoming = try stream(testing.allocator, &.{result});
        defer testing.allocator.free(incoming);
        var fake = Fake{ .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
        try testing.expectEqual(case.exit, run_fake(&fake, &.{ "--wait", "/share/a.txt" }));
        if (case.status != 0) try testing.expectEqualSlices(u8, "waddle open: guest said no", fake.message_at(0));
    }
}

test "Error reply exits 1 with its message; EOF mid-reply and EOF before a result exit 125 under --wait" {
    var reply: [256]u8 = undefined;
    const refused = error_frame(&reply, 1, 3, "not allowed");
    const incoming = try stream(testing.allocator, &.{refused});
    defer testing.allocator.free(incoming);
    var fake = Fake{ .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 1), run_fake(&fake, &.{ "--wait", "/share/a.txt" }));
    try testing.expectEqualSlices(u8, "waddle open: launch refused: not allowed", fake.message_at(0));

    const full = launch_result(&reply, 1, 0, null);
    var torn = Fake{ .incoming = full[0 .. full.len - 3], .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 125), run_fake(&torn, &.{ "--wait", "/share/a.txt" }));
    var empty = Fake{ .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 125), run_fake(&empty, &.{ "--wait", "/share/a.txt" }));
}

test "without --wait: a timeout or end of stream at a frame boundary is a handover (0)" {
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .end_timeout = true };
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{"/share/a.txt"}));
    var closed = Fake{ .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 0), run_fake(&closed, &.{"/share/a.txt"}));
    var reply: [256]u8 = undefined;
    const refused = launch_result(&reply, 1, 5, null);
    const incoming = try stream(testing.allocator, &.{refused});
    defer testing.allocator.free(incoming);
    var failed = Fake{ .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 1), run_fake(&failed, &.{"/share/a.txt"}));
}

test "multiple items share one connection; the first nonzero exit wins" {
    var reply: [768]u8 = undefined;
    const first = launch_result(reply[0..256], 1, 0, null);
    const second = launch_result(reply[256..512], 2, 1, null);
    const third = launch_result(reply[512..768], 3, 0, null);
    const incoming = try stream(testing.allocator, &.{ first, second, third });
    defer testing.allocator.free(incoming);
    var fake = Fake{ .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 1), run_fake(&fake, &.{ "--wait", "/share/a.txt", "/share/b.txt", "/share/c.txt" }));
    try testing.expectEqual(@as(usize, 1), fake.connect_calls);
    try testing.expectEqual(@as(u32, 3), written_frame(&fake, 2).?.request_id);
}

test "a closed connection between items reconnects without another spawn" {
    // The only bytes the server sends are a reply for request 2, which the first item skips.
    // The end of stream then closes the connection at a frame boundary, so the second item
    // reconnects on fd 8 and receives end of stream again.
    var reply: [256]u8 = undefined;
    const late = launch_result(&reply, 2, 0, null);
    const incoming = try stream(testing.allocator, &.{late});
    defer testing.allocator.free(incoming);
    var fake = Fake{ .connect_script = &.{ @as(c_int, 7), @as(c_int, 8) }, .incoming = incoming };
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{ "/share/a.txt", "/share/b.txt" }));
    try testing.expectEqual(@as(usize, 2), fake.connect_calls);
    try testing.expectEqual(@as(usize, 0), fake.spawns);
}

test "console executable with a TTY is delegated to the exec path with the guest path and args" {
    var image: [256]u8 = undefined;
    const cui = pe_bytes(&image, 3);
    var fake = Fake{ .tty_value = true, .delegate_result = 42 };
    const files = [_]FakeFile{.{ .path = "/share/tool.exe", .bytes = cui }};
    fake.files = &files;
    try testing.expectEqual(@as(u8, 42), run_fake(&fake, &.{ "--", "/share/tool.exe", "x" }));
    try testing.expectEqual(@as(usize, 1), fake.delegate_args);
    try testing.expectEqual(@as(usize, 1), fake.delegated);
    try testing.expectEqualSlices(u8, "Z:\\tool.exe", fake.delegate_guest[0..fake.delegate_guest_len]);
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
}

fn tmp_path(allocator: std.mem.Allocator, tmp: *testing.TmpDir, name: []const u8) ![]u8 {
    const base = try tmp.dir.realpathAlloc(allocator, ".");
    defer allocator.free(base);
    return std.fmt.allocPrint(allocator, "{s}/{s}", .{ base, name });
}

fn write_script(dir: std.fs.Dir, name: []const u8, body: []const u8) !void {
    var file = try dir.createFile(name, .{ .mode = 0o755 });
    defer file.close();
    try file.writeAll(body);
}

test "production dir_state accepts only a private directory owned by this user" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makeDir("private");
    try tmp.dir.makeDir("public");
    try tmp.dir.writeFile(.{ .sub_path = "plain", .data = "x" });
    const private = try tmp_path(testing.allocator, &tmp, "private");
    defer testing.allocator.free(private);
    const public = try tmp_path(testing.allocator, &tmp, "public");
    defer testing.allocator.free(public);
    const plain = try tmp_path(testing.allocator, &tmp, "plain");
    defer testing.allocator.free(plain);
    const missing = try tmp_path(testing.allocator, &tmp, "missing");
    defer testing.allocator.free(missing);
    try std.posix.fchmodat(std.posix.AT.FDCWD, private, 0o700, 0);
    try std.posix.fchmodat(std.posix.AT.FDCWD, public, 0o755, 0);
    var real = RealHooks.init(null, null);
    try testing.expectEqual(DirState.ok, RealHooks.dir_state(&real, private));
    try testing.expectEqual(DirState.unsafe, RealHooks.dir_state(&real, public));
    try testing.expectEqual(DirState.unsafe, RealHooks.dir_state(&real, plain));
    try testing.expectEqual(DirState.missing, RealHooks.dir_state(&real, missing));
}

test "production connect distinguishes a live socket, a stale socket file, and a missing one" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const live = try tmp_path(testing.allocator, &tmp, "live.sock");
    defer testing.allocator.free(live);
    const stale = try tmp_path(testing.allocator, &tmp, "stale.sock");
    defer testing.allocator.free(stale);
    const missing = try tmp_path(testing.allocator, &tmp, "none.sock");
    defer testing.allocator.free(missing);
    var real = RealHooks.init(null, null);

    const listener = try std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0);
    const address = try std.net.Address.initUnix(live);
    try std.posix.bind(listener, &address.any, address.getOsSockLen());
    try std.posix.listen(listener, 1);
    const fd = try RealHooks.connect(&real, live);
    RealHooks.close(&real, fd);
    std.posix.close(listener);

    const dead = try std.posix.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0);
    const stale_address = try std.net.Address.initUnix(stale);
    try std.posix.bind(dead, &stale_address.any, stale_address.getOsSockLen());
    std.posix.close(dead);
    try testing.expectError(error.Refused, RealHooks.connect(&real, stale));
    try testing.expectError(error.NotFound, RealHooks.connect(&real, missing));
}

test "production read and write move bytes; poll reports timeout and end of stream" {
    var real = RealHooks.init(null, null);
    var pair: [2]std.posix.fd_t = undefined;
    const rc = std.c.socketpair(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0, &pair);
    try testing.expectEqual(@as(c_int, 0), rc);
    try RealHooks.write_all(&real, pair[0], "hello");
    var buffer: [16]u8 = undefined;
    const count = try RealHooks.poll_read(&real, pair[1], &buffer, 1000);
    try testing.expectEqualSlices(u8, "hello", buffer[0..count]);
    try testing.expectError(error.Timeout, RealHooks.poll_read(&real, pair[1], &buffer, 0));
    std.posix.close(pair[0]);
    try testing.expectEqual(@as(usize, 0), try RealHooks.poll_read(&real, pair[1], &buffer, 1000));
    std.posix.close(pair[1]);
}

test "production read_prefix reads leading bytes; missing and directory paths fail as specified" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "app.exe", .data = "MZ-header" });
    try tmp.dir.makeDir("folder");
    const app = try tmp_path(testing.allocator, &tmp, "app.exe");
    defer testing.allocator.free(app);
    const folder = try tmp_path(testing.allocator, &tmp, "folder");
    defer testing.allocator.free(folder);
    const missing = try tmp_path(testing.allocator, &tmp, "gone.exe");
    defer testing.allocator.free(missing);
    var real = RealHooks.init(null, null);
    var buffer: [open_request.PrefixBytes]u8 = undefined;
    try testing.expectEqual(@as(usize, 9), try RealHooks.read_prefix(&real, app, &buffer));
    try testing.expectEqualSlices(u8, "MZ-header", buffer[0..9]);
    try testing.expectError(error.NotFound, RealHooks.read_prefix(&real, missing, &buffer));
    try testing.expectError(error.Unreadable, RealHooks.read_prefix(&real, folder, &buffer));
}

test "production realpath canonicalizes symlinks and reports missing paths" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "target.txt", .data = "x" });
    try tmp.dir.symLink("target.txt", "link.txt", .{});
    const link = try tmp_path(testing.allocator, &tmp, "link.txt");
    defer testing.allocator.free(link);
    const target = try tmp_path(testing.allocator, &tmp, "target.txt");
    defer testing.allocator.free(target);
    const missing = try tmp_path(testing.allocator, &tmp, "nothing.txt");
    defer testing.allocator.free(missing);
    var real = RealHooks.init(null, null);
    const resolved = try RealHooks.realpath(&real, testing.allocator, link);
    defer testing.allocator.free(resolved);
    try testing.expectEqualSlices(u8, target, resolved);
    try testing.expectError(error.NotFound, RealHooks.realpath(&real, testing.allocator, missing));
}

test "production translate maps configured mounts and reports unmapped paths as outside" {
    var real = RealHooks.init(null, null);
    c.daemon_config_init_defaults(&real.config);
    real.config.mount_count = 1;
    @memcpy(real.config.mounts[0].host_path[0..10], "/home/dev/");
    real.config.mounts[0].host_path[10] = 0;
    @memcpy(real.config.mounts[0].guest_drive[0..3], "Z:\\");
    real.config.mounts[0].guest_drive[3] = 0;
    const guest = try RealHooks.translate(&real, testing.allocator, "/home/dev/notes/a.txt");
    defer testing.allocator.free(guest);
    try testing.expectEqualSlices(u8, "Z:\\notes\\a.txt", guest);
    try testing.expectError(error.Outside, RealHooks.translate(&real, testing.allocator, "/etc/passwd"));
}

test "production spawn runs waddle-session detached from PATH and notify-send is best effort" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const marker = try tmp_path(testing.allocator, &tmp, "spawned.txt");
    defer testing.allocator.free(marker);
    const notified = try tmp_path(testing.allocator, &tmp, "notified.txt");
    defer testing.allocator.free(notified);
    const bin_dir = try tmp_path(testing.allocator, &tmp, "bin");
    defer testing.allocator.free(bin_dir);
    try tmp.dir.makeDir("bin");
    var bin = try tmp.dir.openDir("bin", .{});
    defer bin.close();
    const session_body = try std.fmt.allocPrint(testing.allocator, "#!/bin/sh\necho \"$@\" > '{s}'\n", .{marker});
    defer testing.allocator.free(session_body);
    try write_script(bin, "waddle-session", session_body);
    const notify_body = try std.fmt.allocPrint(testing.allocator, "#!/bin/sh\necho \"$@\" > '{s}'\n", .{notified});
    defer testing.allocator.free(notify_body);
    try write_script(bin, "notify-send", notify_body);

    const old_path = std.posix.getenv("PATH") orelse "/usr/bin:/bin";
    const new_path = try std.fmt.allocPrintZ(testing.allocator, "{s}:{s}", .{ bin_dir, old_path });
    defer testing.allocator.free(new_path);
    try testing.expectEqual(@as(c_int, 0), c.setenv("PATH", new_path.ptr, 1));
    defer _ = c.setenv("PATH", old_path.ptr, 1);

    var real = RealHooks.init(null, null);
    try RealHooks.spawn_session(&real, "dev1");
    RealHooks.notify(&real, "Starting Windows");
    // Both helpers run detached; poll until each has written its marker, bounded to 2 s.
    var waited: usize = 0;
    var session_text: ?[]u8 = null;
    var notify_text: ?[]u8 = null;
    while (waited < 200 and (session_text == null or notify_text == null)) : (waited += 1) {
        if (session_text == null) session_text = std.fs.cwd().readFileAlloc(testing.allocator, marker, 256) catch null;
        if (notify_text == null) notify_text = std.fs.cwd().readFileAlloc(testing.allocator, notified, 256) catch null;
        if (session_text == null or notify_text == null) std.time.sleep(10 * std.time.ns_per_ms);
    }
    defer if (session_text) |text| testing.allocator.free(text);
    defer if (notify_text) |text| testing.allocator.free(text);
    try testing.expectEqualSlices(u8, "--device dev1\n", session_text orelse return error.TestUnexpectedResult);
    try testing.expect(std.mem.indexOf(u8, notify_text orelse return error.TestUnexpectedResult, "Starting Windows") != null);
}

fn recording_exec(argc: c_int, argv: [*c][*c]u8, start: c_int) callconv(.C) c_int {
    var index: usize = 0;
    var total: usize = 0;
    while (index < @as(usize, @intCast(argc))) : (index += 1) total += std.mem.span(argv[index]).len;
    recorded_argc = @intCast(argc);
    recorded_start = start;
    recorded_second = std.mem.span(argv[1]);
    recorded_guest = std.mem.span(argv[6]);
    return 7;
}

var recorded_argc: usize = 0;
var recorded_start: c_int = 0;
var recorded_second: []const u8 = "";
var recorded_guest: []const u8 = "";

test "production delegate builds the exec argv with the device, translate flag and guest path" {
    var real = RealHooks.init(recording_exec, null);
    const code = RealHooks.delegate(&real, "dev1", "Z:\\tool.exe", &.{"x"});
    try testing.expectEqual(@as(u8, 7), code);
    try testing.expectEqual(@as(usize, 8), recorded_argc);
    try testing.expectEqual(@as(c_int, 2), recorded_start);
    try testing.expectEqualSlices(u8, "exec", recorded_second);
    try testing.expectEqualSlices(u8, "Z:\\tool.exe", recorded_guest);
}

fn resolver_fail(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    _ = specified;
    _ = out;
    _ = sock;
    _ = cap;
    _ = single;
    return 2;
}

test "production resolve forwards the resolver's failure and the exported entry returns it" {
    var real = RealHooks.init(null, resolver_fail);
    var device: Device = undefined;
    try testing.expectEqual(@as(?u8, 2), RealHooks.resolve(&real, "dev1", &device));
    try testing.expectEqual(@as(?u8, 2), RealHooks.resolve(&real, null, &device));
    var argv_storage = [_][*:0]u8{ @constCast("--wait"), @constCast("/share/a.txt") };
    try testing.expectEqual(@as(c_int, 2), waddle_open_command(hide_int(2), &argv_storage, recording_exec, resolver_fail));
    try testing.expectEqual(@as(c_int, 125), waddle_open_command(hide_int(0), null, null, resolver_fail));
    try testing.expectEqual(@as(c_int, 125), waddle_open_command(hide_int(0), null, recording_exec, null));
    try testing.expectEqual(@as(c_int, 2), waddle_open_command(hide_int(-1), null, recording_exec, resolver_fail));
    try testing.expectEqual(@as(c_int, 2), waddle_open_command(hide_int(1), null, recording_exec, resolver_fail));
}

test "allocation failure at every point leaves no leak and no crash" {
    var reply: [512]u8 = undefined;
    const first = launch_result(reply[0..256], 1, 0, null);
    const second = launch_result(reply[256..512], 2, 1, "boom");
    var index: usize = 0;
    while (index < 80) : (index += 1) {
        const incoming = try stream(testing.allocator, &.{ first, second });
        defer testing.allocator.free(incoming);
        var fake = Fake{ .incoming = incoming, .env_xdg = "tok-1", .connect_script = &.{@as(c_int, 7)} };
        var hooks = fake.hooks();
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = index });
        _ = run(failing.allocator(), &.{ "--wait", "/share/a.txt", "/share/b.txt" }, &hooks);
    }
}

test "an App item uses the catalog key with no path work and sends a frame with app_key" {
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const incoming = try stream(testing.allocator, &.{ok});
    defer testing.allocator.free(incoming);
    var fake = Fake{ .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{ "--wait", "--app", "a-Microsoft_Notepad" }));
    const frame = written_frame(&fake, 0).?;
    try testing.expectEqualSlices(u8, "a-Microsoft_Notepad", payload_text(frame.payload, c.InteropTagAppKey).?);
    try testing.expectEqual(@as(u64, 1), payload_uint(frame.payload, c.InteropTagLaunchKind).?);
    try testing.expect(payload_text(frame.payload, c.InteropTagTargetPath) == null);
}

test "a GUI executable with a tail sends the quoted arguments with its guest path" {
    var image: [256]u8 = undefined;
    const gui = pe_bytes(&image, 2);
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const incoming = try stream(testing.allocator, &.{ok});
    defer testing.allocator.free(incoming);
    const files = [_]FakeFile{.{ .path = "/share/game.exe", .bytes = gui }};
    var fake = Fake{ .incoming = incoming, .files = &files, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{ "--wait", "--", "/share/game.exe", "-windowed", "a b" }));
    const frame = written_frame(&fake, 0).?;
    try testing.expectEqual(@as(u64, 2), payload_uint(frame.payload, c.InteropTagLaunchKind).?);
    try testing.expectEqualSlices(u8, "Z:\\game.exe", payload_text(frame.payload, c.InteropTagTargetPath).?);
    try testing.expectEqualSlices(u8, "\"-windowed\" \"a b\"", payload_text(frame.payload, c.InteropTagArguments).?);
}

test "a failed write exits 125 with a message; a failed reconnect exits 125 without a spawn" {
    var fake = Fake{ .connect_script = &.{@as(c_int, 7)}, .fail_write = true };
    try testing.expectEqual(@as(u8, 125), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqualSlices(u8, "waddle open: could not send the launch request", fake.message_at(0));

    var reply: [256]u8 = undefined;
    const late = launch_result(&reply, 2, 0, null);
    const incoming = try stream(testing.allocator, &.{late});
    defer testing.allocator.free(incoming);
    var reconnect = Fake{ .connect_script = &.{ @as(c_int, 7), error.Failed }, .incoming = incoming };
    try testing.expectEqual(@as(u8, 125), run_fake(&reconnect, &.{ "/share/a.txt", "/share/b.txt" }));
    try testing.expectEqual(@as(usize, 0), reconnect.spawns);
}

test "a path whose canonical form fails exits 125" {
    var fake = Fake{ .fail_realpath = true };
    try testing.expectEqual(@as(u8, 125), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 0), fake.connect_calls);
}

test "production dir_state reports an unsafe state for a path below a regular file" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "file", .data = "x" });
    const below = try tmp_path(testing.allocator, &tmp, "file/child");
    defer testing.allocator.free(below);
    var real = RealHooks.init(null, null);
    // ENOTDIR is reported as missing: no directory exists there, so the connect step fails instead.
    try testing.expectEqual(DirState.missing, RealHooks.dir_state(&real, below));
}

test "production write and read on a closed descriptor fail; spawn of a missing program fails" {
    var real = RealHooks.init(null, null);
    var pair: [2]std.posix.fd_t = undefined;
    try testing.expectEqual(@as(c_int, 0), std.c.socketpair(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0, &pair));
    std.posix.close(pair[1]);
    try testing.expectError(error.Failed, RealHooks.write_all(&real, pair[0], "x"));
    var buffer: [4]u8 = undefined;
    // A peer that has closed reads as end of stream; a closed descriptor reads as an error.
    try testing.expectEqual(@as(usize, 0), try RealHooks.poll_read(&real, pair[0], &buffer, 0));
    std.posix.close(pair[0]);
    try testing.expectError(error.Failed, RealHooks.poll_read(&real, pair[0], &buffer, 0));
    var missing_bin = testing.tmpDir(.{});
    defer missing_bin.cleanup();
    const empty_path = try tmp_path(testing.allocator, &missing_bin, "");
    defer testing.allocator.free(empty_path);
    const old_path = std.posix.getenv("PATH") orelse "/usr/bin:/bin";
    const isolated = try std.fmt.allocPrintZ(testing.allocator, "{s}", .{empty_path});
    defer testing.allocator.free(isolated);
    try testing.expectEqual(@as(c_int, 0), c.setenv("PATH", isolated.ptr, 1));
    defer _ = c.setenv("PATH", old_path.ptr, 1);
    try testing.expectError(error.Failed, RealHooks.spawn_session(&real, "dev1"));
}

test "production resolve succeeds with a readable config file and derives the session socket beside daemon.sock" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "dev.conf", .data = "" });
    const config = try tmp_path(testing.allocator, &tmp, "dev.conf");
    defer testing.allocator.free(config);
    @memcpy(fixture_config_path[0..config.len], config);
    fixture_config_path[config.len] = 0;
    var real = RealHooks.init(null, resolver_ok);
    var device: Device = undefined;
    try testing.expectEqual(@as(?u8, null), RealHooks.resolve(&real, "dev1", &device));
    try testing.expectEqualSlices(u8, "dev1", device.name);
    try testing.expectEqualSlices(u8, "/run/u/waddle/dev1/session.sock", device.session_path);
    // A config without an integration.notify key keeps the default, which is on.
    try testing.expectEqual(true, device.notify);
}

var fixture_config_path: [4096]u8 = undefined;

fn resolver_ok(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    _ = specified;
    _ = single;
    const info = out.?;
    @memset(&info.name, 0);
    @memcpy(info.name[0..4], "dev1");
    @memset(&info.config_path, 0);
    const config = std.mem.sliceTo(&fixture_config_path, 0);
    @memcpy(info.config_path[0..config.len], config);
    const socket = "/run/u/waddle/dev1/daemon.sock";
    @memcpy(sock[0..socket.len], socket);
    sock[socket.len] = 0;
    _ = cap;
    return 0;
}

test "allocation failure sweep over a request with a missing file, an outside path and a tail" {
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const sets = [_][]const []const u8{
        &.{ "/share/a.txt", "/missing/x.txt" },
        &.{ "/etc/hosts" },
        &.{ "--app", "a-x", "--wait" },
        &.{ "--", "/share/tool.exe", "x" },
    };
    for (sets) |args| {
        var index: usize = 0;
        while (index < 160) : (index += 1) {
            const incoming = try stream(testing.allocator, &.{ok});
            defer testing.allocator.free(incoming);
            var fake = Fake{ .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
            var hooks = fake.hooks();
            var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = index });
            _ = run(failing.allocator(), args, &hooks);
        }
    }
}


test "production environment, clock, sleep and stderr hooks behave as their names say" {
    var real = RealHooks.init(null, null);
    try testing.expect(RealHooks.getenv(&real, "PATH") != null);
    try testing.expect(RealHooks.getenv(&real, "WADDLE_NO_SUCH_VARIABLE_42") == null);
    const before = RealHooks.now_ms(&real);
    RealHooks.sleep_ms(&real, 5);
    try testing.expect(RealHooks.now_ms(&real) >= before + 5);
    _ = RealHooks.tty(&real);
    RealHooks.message(&real, "waddle open: test line");
}

test "a garbage reply header, a LaunchResult without a status, and an Error without a message are refused" {
    var reply: [256]u8 = undefined;
    var garbage = [_]u8{0xab} ** 32;
    var fake = Fake{ .incoming = &garbage, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 125), run_fake(&fake, &.{ "--wait", "/share/a.txt" }));

    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, &reply, reply.len);
    _ = c.interop_put_uint(&writer, c.InteropTagProcessId, 4242);
    var length: usize = 0;
    _ = c.interop_writer_finish(&writer, c.InteropMsgLaunchResult, 1, c.InteropFromGuest, &length);
    var no_status = Fake{ .incoming = reply[0..length], .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 125), run_fake(&no_status, &.{ "--wait", "/share/a.txt" }));

    var bare: [256]u8 = undefined;
    const refused = error_frame(&bare, 1, 3, null);
    var silent = Fake{ .incoming = refused, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 1), run_fake(&silent, &.{ "--wait", "/share/a.txt" }));
    try testing.expectEqualSlices(u8, "waddle open: launch refused: no message", silent.message_at(0));
}

test "a LaunchResult status without a message reports the generic failure text" {
    var reply: [256]u8 = undefined;
    const bare = launch_result(&reply, 1, 1, null);
    var fake = Fake{ .incoming = bare, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 1), run_fake(&fake, &.{ "--wait", "/share/a.txt" }));
    try testing.expectEqualSlices(u8, "waddle open: launch failed", fake.message_at(0));
}

test "resolve refuses an over-long name, a config that does not load, and an over-long session path" {
    var real = RealHooks.init(null, resolver_ok);
    var device: Device = undefined;
    const long_name = [_]u8{'n'} ** c.WaddleMaxDeviceNameLen;
    try testing.expectEqual(@as(?u8, 2), RealHooks.resolve(&real, hide_bytes(&long_name), &device));

    // A missing config file loads the defaults, which is a successful resolution.
    const missing_config = hide_bytes("/nonexistent/dir/dev1.cfg");
    @memcpy(fixture_config_path[0..missing_config.len], missing_config);
    fixture_config_path[missing_config.len] = 0;
    try testing.expectEqual(@as(?u8, null), RealHooks.resolve(&real, "dev1", &device));

    // A config that exists but is malformed does not load: exit 125.
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "bad.cfg", .data = "[section\nkey = \xff\x00 without close\n[[\n" });
    const bad = try tmp_path(testing.allocator, &tmp, "bad.cfg");
    defer testing.allocator.free(bad);
    @memcpy(fixture_config_path[0..bad.len], hide_bytes(bad));
    fixture_config_path[bad.len] = 0;
    try testing.expectEqual(@as(?u8, 125), RealHooks.resolve(&real, "dev1", &device));

    var long_socket = RealHooks.init(null, resolver_long_socket);
    try testing.expectEqual(@as(?u8, 125), RealHooks.resolve(&long_socket, "dev1", &device));
}

fn resolver_long_socket(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    _ = specified;
    _ = single;
    _ = cap;
    // WaddleMaxPathLen is 1024. The socket path (1023 bytes plus NUL) fits its buffer, but
    // the derived "<dir>/session.sock" (1024 bytes plus NUL) does not fit the session buffer.
    const directory = "/" ++ ("d" ** 1010) ++ "/daemon.sock";
    @memset(&out.?.name, 0);
    @memcpy(out.?.name[0..4], "dev1");
    @memset(&out.?.config_path, 0);
    @memcpy(sock[0..directory.len], directory);
    sock[directory.len] = 0;
    return 0;
}

test "the exported entry refuses more than 64 arguments and a null argv with a count" {
    var storage: [65][*:0]u8 = undefined;
    for (&storage) |*slot| slot.* = @constCast("x");
    const count = hide_int(65);
    try testing.expectEqual(@as(c_int, 2), waddle_open_command(count, &storage, recording_exec, resolver_ok_fixture));
}

fn resolver_ok_fixture(specified: [*c]const u8, out: ?*c.device_info_t, sock: [*c]u8, cap: usize, single: c_int) callconv(.C) c_int {
    return resolver_ok(specified, out, sock, cap, single);
}

test "an oversize notice is dropped without failing, and an oversize delegated argument exits 125" {
    var real = RealHooks.init(recording_exec, null);
    const long = [_]u8{'m'} ** 2000;
    RealHooks.notify(&real, &long);
    const huge = [_]u8{'t'} ** 5000;
    try testing.expectEqual(@as(u8, 125), RealHooks.delegate(&real, "dev1", "Z:\\a.exe", &.{&huge}));
}

test "a notice is sent once even when the deadline passes afterwards" {
    var fake = Fake{ .device = .{ .name = "dev1", .session_path = "/run/u/waddle/dev1/session.sock", .notify = true }, .connect_script = &.{error.NotFound} };
    try testing.expectEqual(@as(u8, 125), run_fake(&fake, &.{"/share/a.txt"}));
    try testing.expectEqual(@as(usize, 1), fake.notices);
    try testing.expect(fake.notify_count >= 1);
}

test "frames of other types and other request ids are skipped until the matching LaunchResult" {
    var other_type: [256]u8 = undefined;
    var writer: c.interop_writer_t = undefined;
    c.interop_writer_init(&writer, &other_type, other_type.len);
    _ = c.interop_put_uint(&writer, c.InteropTagProcessId, 7);
    var length: usize = 0;
    _ = c.interop_writer_finish(&writer, 0x0ff0, 1, c.InteropFromGuest, &length);
    var reply: [256]u8 = undefined;
    const ok = launch_result(&reply, 1, 0, null);
    const incoming = try stream(testing.allocator, &.{ other_type[0..length], ok });
    defer testing.allocator.free(incoming);
    var fake = Fake{ .incoming = incoming, .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 0), run_fake(&fake, &.{ "--wait", "/share/a.txt" }));
}

test "a LaunchResult sent in the wrong direction fails payload validation and exits 125" {
    var writer: c.interop_writer_t = undefined;
    var reply: [256]u8 = undefined;
    c.interop_writer_init(&writer, &reply, reply.len);
    _ = c.interop_put_uint(&writer, c.InteropTagProcessId, 4242);
    _ = c.interop_put_uint(&writer, c.InteropTagStatus, 0);
    var length: usize = 0;
    _ = c.interop_writer_finish(&writer, c.InteropMsgLaunchResult, 1, c.InteropFromLocal, &length);
    var fake = Fake{ .incoming = reply[0..length], .connect_script = &.{@as(c_int, 7)} };
    try testing.expectEqual(@as(u8, 125), run_fake(&fake, &.{ "--wait", "/share/a.txt" }));
}

