//! Memory-safe device command parsing and v1 JSON result rendering. C owns registry I/O.
const std = @import("std");
const c = @cImport({
    @cInclude("daemon_device.h");
    @cInclude("daemon_config.h");
    @cInclude("daemon_client.h");
    @cInclude("sys/stat.h");
    @cInclude("unistd.h");
    @cInclude("errno.h");
});

/// Parsed argument view; borrows argv only for one command call.
const command_t = struct {
    operands: [16][]const u8 = undefined,
    count: usize = 0,
    json: bool = false,
    help: bool = false,
    clear: bool = false,
    running: bool = false,
    stopped: bool = false,
    dry_run: bool = false,
};

/// Parse bounded UTF-8 argument slices; duplicate singleton flags fail before I/O.
fn parse(args: []const []const u8) !command_t {
    var command = command_t{};
    var literal = false;
    for (args) |arg| {
        if (arg.len > 4096 or !std.unicode.utf8ValidateSlice(arg)) return error.Usage;
        if (!literal and std.mem.eql(u8, arg, "--")) {
            literal = true;
        } else if (!literal and std.mem.eql(u8, arg, "--json")) {
            if (command.json) return error.Usage;
            command.json = true;
        } else if (!literal and (std.mem.eql(u8, arg, "--help") or std.mem.eql(u8, arg, "-h"))) {
            if (command.help) return error.Usage;
            command.help = true;
        } else if (!literal and std.mem.eql(u8, arg, "--dry-run")) {
            if (command.dry_run) return error.Usage;
            command.dry_run = true;
        } else if (!literal and std.mem.eql(u8, arg, "--clear")) {
            if (command.clear) return error.Usage;
            command.clear = true;
        } else if (!literal and std.mem.eql(u8, arg, "--running")) {
            if (command.running or command.stopped) return error.Usage;
            command.running = true;
        } else if (!literal and std.mem.eql(u8, arg, "--stopped")) {
            if (command.running or command.stopped) return error.Usage;
            command.stopped = true;
        } else {
            if (!literal and std.mem.startsWith(u8, arg, "-")) return error.Usage;
            if (command.count == command.operands.len) return error.Usage;
            command.operands[command.count] = arg;
            command.count += 1;
        }
    }
    return command;
}

/// Validate an ASCII profile identifier independent of locale.
fn name_valid(name: []const u8) bool {
    if (name.len == 0 or name.len >= 64) return false;
    for (name) |ch| if (!std.ascii.isAlphanumeric(ch) and ch != '-' and ch != '_') return false;
    return true;
}

/// Convert an inline C buffer to a borrowed slice bounded by the array capacity.
fn c_text(buffer: anytype) []const u8 {
    const slice = buffer[0..];
    return slice[0 .. std.mem.indexOfScalar(u8, slice, 0) orelse slice.len];
}

/// Allocate a JSON object in the caller's arena; ownership stays with that arena.
fn object(allocator: std.mem.Allocator) std.json.Value {
    return .{ .object = std.json.ObjectMap.init(allocator) };
}

/// Copy text so result fields never refer to expired C stack storage.
fn text_value(allocator: std.mem.Allocator, value: []const u8) !std.json.Value {
    return .{ .string = try allocator.dupe(u8, value) };
}

/// Put a typed JSON field; borrows its key and transfers value storage to the arena.
fn put(value: *std.json.Value, key: []const u8, field: std.json.Value) !void {
    try value.object.put(key, field);
}

/// Build typed effective mutable settings from an already validated configuration.
fn config_value(allocator: std.mem.Allocator, cfg: *const c.daemon_config_t) !std.json.Value {
    var value = object(allocator);
    try put(&value, "memory_mb", .{ .integer = cfg.memory_mb });
    try put(&value, "vcpus", .{ .integer = cfg.vcpus });
    try put(&value, "vsock_port", .{ .integer = cfg.vsock_port });
    try put(&value, "default_shell", try text_value(allocator, c_text(&cfg.default_shell)));
    try put(&value, "start_timeout", .{ .integer = cfg.start_timeout_sec });
    try put(&value, "stop_timeout", .{ .integer = cfg.stop_timeout_sec });
    return value;
}

/// Observe daemon status without spawning it; absent socket is stopped, failed IPC unknown.
fn observed_state(info: *const c.device_info_t) []const u8 {
    var st: c.struct_stat = undefined;
    if (c.lstat(&info.socket_path, &st) != 0) {
        return if (c.__errno_location().* == c.ENOENT) "stopped" else "unknown";
    }
    const fd = c.waddle_client_connect(&info.socket_path);
    if (fd < 0) return "unknown";
    defer _ = c.close(fd);
    var status: c.waddle_daemon_status_resp_t = std.mem.zeroes(c.waddle_daemon_status_resp_t);
    if (c.waddle_client_status(fd, &status) != 0) return "unknown";
    return std.mem.span(c.waddle_subsystem_state_to_string(status.subsystem_state));
}

/// Populate common device result data without treating a connected supervisor as running.
fn device_value(allocator: std.mem.Allocator, info: *const c.device_info_t, selected: []const u8) !std.json.Value {
    var value = object(allocator);
    try put(&value, "state", try text_value(allocator, observed_state(info)));
    try put(&value, "config_valid", .{ .bool = info.config_valid != 0 });
    try put(&value, "is_default", .{ .bool = std.mem.eql(u8, c_text(&info.name), selected) });
    try put(&value, "vsock_cid", .{ .integer = info.vsock_cid });
    try put(&value, "vsock_port", .{ .integer = info.vsock_port });
    try put(&value, "memory_mb", .{ .integer = info.memory_mb });
    try put(&value, "vcpus", .{ .integer = info.vcpus });
    try put(&value, "config_path", try text_value(allocator, c_text(&info.config_path)));
    try put(&value, "state_dir", try text_value(allocator, c_text(&info.state_dir)));
    try put(&value, "disk_image", try text_value(allocator, c_text(&info.disk_image)));
    return value;
}

/// Wrap one named successful result in the common schema.
fn add_result(allocator: std.mem.Allocator, results: *std.json.Value, name: ?[]const u8, data: std.json.Value) !void {
    var value = object(allocator);
    try put(&value, "name", if (name) |n| try text_value(allocator, n) else .null);
    try put(&value, "ok", .{ .bool = true });
    try put(&value, "data", data);
    try put(&value, "error", .null);
    try results.array.append(value);
}

/// Translate registry errno to stable command errors; errno remains process-local.
fn registry_error() anyerror {
    return switch (c.__errno_location().*) {
        c.EINVAL => error.InvalidConfig,
        c.ENOENT => error.NotFound,
        c.EEXIST => error.AlreadyExists,
        c.ENOSPC => error.Capacity,
        c.EACCES, c.EPERM, c.ELOOP => error.PermissionDenied,
        c.EINTR => error.Cancelled,
        c.EBUSY => error.Busy,
        else => error.IoError,
    };
}

/// Execute delivered commands. All operand/flag validation precedes registry operations.
fn execute(allocator: std.mem.Allocator, command: command_t, results: *std.json.Value) !void {
    if (command.count == 0) return error.Usage;
    const op = command.operands[0];
    const is_list = std.mem.eql(u8, op, "list") or std.mem.eql(u8, op, "devices");
    const is_show = std.mem.eql(u8, op, "show");
    const is_default = std.mem.eql(u8, op, "default");
    const is_config = std.mem.eql(u8, op, "config");
    if (!is_list and !is_show and !is_default and !is_config) return error.Usage;
    if (command.clear and !is_default) return error.Usage;
    if ((command.running or command.stopped) and !is_list) return error.Usage;
    if (is_list and command.count != 1) return error.Usage;
    if (is_show and command.count != 2) return error.Usage;
    if (is_default and (command.count > 2 or (command.clear and command.count != 1))) return error.Usage;
    var config_write = false;
    var config_reset = false;
    if (is_config) {
        if (command.count < 3) return error.Usage;
        const action = command.operands[1];
        config_reset = std.mem.eql(u8, action, "reset");
        config_write = config_reset or std.mem.eql(u8, action, "set");
        if (!config_write and !std.mem.eql(u8, action, "get")) return error.Usage;
        if (config_write) {
            if (command.count < 4 or command.count > 9) return error.Usage;
        } else if (command.count > 4) return error.Usage;
    }
    if (command.dry_run and !config_write) return error.Usage;
    const name: ?[]const u8 = if (is_show or (is_default and command.count == 2)) command.operands[1] else if (is_config) command.operands[2] else null;
    if (name) |n| if (!name_valid(n)) return error.InvalidName;
    if (config_write) {
        const terminated = try allocator.dupeZ(u8, name.?);
        var changes: [6][*:0]const u8 = undefined;
        var plan = std.json.Value{ .array = std.ArrayList(std.json.Value).init(allocator) };
        for (command.operands[3..command.count], 0..) |change, i| {
            changes[i] = (try allocator.dupeZ(u8, change)).ptr;
            try plan.array.append(try text_value(allocator, change));
        }
        if (c.daemon_device_config_update(terminated.ptr, &changes, command.count - 3, @intFromBool(config_reset), @intFromBool(command.dry_run)) != 0) return registry_error();
        var data = object(allocator);
        try put(&data, "dry_run", .{ .bool = command.dry_run });
        try put(&data, "action", try text_value(allocator, command.operands[1]));
        try put(&data, "changes", plan);
        try add_result(allocator, results, name, data);
        return;
    }
    if (is_default and (name != null or command.clear)) {
        const terminated = if (name) |n| try allocator.dupeZ(u8, n) else null;
        if (c.daemon_device_default_set(if (terminated) |n| n.ptr else null) != 0) return registry_error();
    }
    var selected: [64]u8 = undefined;
    if (c.daemon_device_default_get(&selected, selected.len) != 0) return registry_error();
    const selected_text = c_text(&selected);
    if (is_default) {
        var data = object(allocator);
        try put(&data, "default_device", if (selected_text.len == 0) .null else try text_value(allocator, selected_text));
        try add_result(allocator, results, null, data);
        return;
    }
    var list: c.device_list_t = undefined;
    if (c.daemon_device_list(&list) < 0) return registry_error();
    if (is_list) {
        for (list.devices[0..list.count]) |*info| {
            const state = observed_state(info);
            if (command.running and !std.mem.eql(u8, state, "running")) continue;
            if (command.stopped and (!std.mem.eql(u8, state, "stopped") or info.config_valid == 0)) continue;
            try add_result(allocator, results, c_text(&info.name), try device_value(allocator, info, selected_text));
        }
        return;
    }
    for (list.devices[0..list.count]) |*info| {
        if (!std.mem.eql(u8, c_text(&info.name), name.?)) continue;
        var data = try device_value(allocator, info, selected_text);
        if (is_config or info.config_valid != 0) {
            var cfg: c.daemon_config_t = undefined;
            c.daemon_config_init_defaults(&cfg);
            if (c.daemon_config_load_file(&cfg, &info.config_path) != 0) return error.InvalidConfig;
            const settings = try config_value(allocator, &cfg);
            if (is_config) {
                data = settings;
                if (command.count == 4) {
                    const key = command.operands[3];
                    const field = settings.object.get(key) orelse return error.Usage;
                    data = object(allocator);
                    try put(&data, try allocator.dupe(u8, key), field);
                }
            } else {
                try put(&data, "config", settings);
                try put(&data, "mount_count", .{ .integer = cfg.mount_count });
            }
        }
        if (is_config and info.config_valid == 0) return error.InvalidConfig;
        try add_result(allocator, results, name, data);
        return;
    }
    return error.NotFound;
}

/// Stable error identifier/message and exit status; no borrowed errno diagnostics in JSON.
const failure_t = struct { code: []const u8, message: []const u8, status: c_int };

/// Resolve typed implementation errors to public result codes.
fn failure(err: anyerror) failure_t {
    return switch (err) {
        error.Usage => .{ .code = "usage", .message = "Invalid command, operands or options; use device --help.", .status = 2 },
        error.InvalidName => .{ .code = "invalid_name", .message = "Names require 1..63 ASCII letters, digits, hyphens or underscores.", .status = 2 },
        error.InvalidConfig => .{ .code = "invalid_config", .message = "Configuration or saved default is invalid.", .status = 2 },
        error.NotFound => .{ .code = "not_found", .message = "Device or required file does not exist.", .status = 2 },
        error.AlreadyExists => .{ .code = "already_exists", .message = "Destination already exists.", .status = 2 },
        error.Capacity => .{ .code = "capacity", .message = "Registry capacity exceeded.", .status = 2 },
        error.PermissionDenied => .{ .code = "permission_denied", .message = "Unsafe permissions or symlink path.", .status = 1 },
        error.Busy => .{ .code = "busy", .message = "Device has a runtime owner, open disk or unresolved runtime sockets.", .status = 1 },
        error.Cancelled => .{ .code = "cancelled", .message = "Operation interrupted.", .status = 130 },
        error.OutOfMemory => .{ .code = "resource_error", .message = "Allocation failed.", .status = 125 },
        else => .{ .code = "io_error", .message = "Registry I/O failed.", .status = 1 },
    };
}

/// Render one newline-terminated schema object, or human-readable result lines.
fn render(allocator: std.mem.Allocator, command: []const u8, json: bool, results: std.json.Value, issue: ?failure_t) !void {
    var root = object(allocator);
    try put(&root, "schema_version", .{ .integer = 1 });
    try put(&root, "command", try text_value(allocator, command));
    try put(&root, "ok", .{ .bool = issue == null });
    try put(&root, "results", results);
    var err_value: std.json.Value = .null;
    if (issue) |f| {
        err_value = object(allocator);
        try put(&err_value, "code", try text_value(allocator, f.code));
        try put(&err_value, "message", try text_value(allocator, f.message));
    }
    try put(&root, "error", err_value);
    const out = std.io.getStdOut().writer();
    if (json) {
        try std.json.stringify(root, .{}, out);
        try out.writeByte('\n');
    } else if (issue) |f| {
        try std.io.getStdErr().writer().print("waddle device: {s}: {s}\n", .{ f.code, f.message });
    } else {
        if (results.array.items.len == 0) try out.writeAll("No devices configured. Use 'waddle init NAME'.\n");
        for (results.array.items) |item| {
            const name = item.object.get("name").?;
            if (name == .string) try out.print("{s}: ", .{name.string});
            try std.json.stringify(item.object.get("data").?, .{}, out);
            try out.writeByte('\n');
        }
    }
}

/// Run borrowed argv with one arena owning every result allocation; no memory retained.
export fn waddle_device_command(argc: c_int, argv: [*]const [*:0]const u8) c_int {
    var arena = std.heap.ArenaAllocator.init(std.heap.c_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    if (argc < 0 or argc > 128) return 2;
    var args: [128][]const u8 = undefined;
    var json = false;
    for (argv[0..@intCast(argc)], 0..) |arg, i| {
        args[i] = std.mem.span(arg);
        if (std.mem.eql(u8, args[i], "--json")) json = true;
    }
    var results = std.json.Value{ .array = std.ArrayList(std.json.Value).init(allocator) };
    const command = parse(args[0..@intCast(argc)]) catch |err| {
        const issue = failure(err);
        render(allocator, "device", json, results, issue) catch return 125;
        return issue.status;
    };
    if (command.help or command.count == 0) {
        std.io.getStdOut().writer().writeAll(
            "Usage: waddle device list [--running|--stopped] [--json]\n" ++
                "       waddle device show NAME [--json]\n" ++
                "       waddle device default [NAME|--clear] [--json]\n" ++
                "       waddle device config get NAME [KEY] [--json]\n" ++
                "       waddle device config set NAME KEY=VALUE... [--dry-run] [--json]\n" ++
                "       waddle device config reset NAME KEY... [--dry-run] [--json]\n" ++
                "Config edits require a stopped device without runtime owners; changes apply together.\n" ++
                "Reset values: memory_mb=4096, vcpus=4, default_shell=powershell.exe,\n" ++
                "vsock_port=5242, start_timeout=60, stop_timeout=15.\n" ++
                "Read commands never start a guest. Default selection does not boot.\n" ++
                "Config keys: memory_mb, vcpus, default_shell, vsock_port, start_timeout, stop_timeout.\n" ++
                "Exit codes: 0 success, 2 usage/validation, 1 I/O, 125 resources, 130 interrupted.\n",
        ) catch return 125;
        return 0;
    }
    const label = std.fmt.allocPrint(allocator, "device {s}", .{command.operands[0]}) catch return 125;
    execute(allocator, command, &results) catch |err| {
        const issue = failure(err);
        render(allocator, label, json, results, issue) catch return 125;
        return issue.status;
    };
    render(allocator, label, json, results, null) catch return 125;
    return 0;
}

test "device parser rejects duplicate flags, invalid UTF-8 and overflow" {
    try std.testing.expectError(error.Usage, parse(&.{ "list", "--json", "--json" }));
    try std.testing.expectError(error.Usage, parse(&.{ "list", "--running", "--stopped" }));
    try std.testing.expectError(error.Usage, parse(&.{ "list", "--bogus" }));
    try std.testing.expectError(error.Usage, parse(&.{ "show", "\xff" }));
    const command = try parse(&.{ "show", "--", "--literal" });
    try std.testing.expectEqualStrings("--literal", command.operands[1]);
    try std.testing.expect(name_valid(command.operands[1]));
}

test "device JSON strings are owned, escaped and allocator-clean" {
    const allocator = std.testing.allocator;
    var data = object(allocator);
    defer data.object.deinit();
    const owned = try text_value(allocator, "quote\"\\日本語\n");
    defer allocator.free(owned.string);
    try put(&data, "path", owned);
    const serialized = try std.json.stringifyAlloc(allocator, data, .{});
    defer allocator.free(serialized);
    const decoded = try std.json.parseFromSlice(std.json.Value, allocator, serialized, .{});
    defer decoded.deinit();
    try std.testing.expectEqualStrings(owned.string, decoded.value.object.get("path").?.string);
}
