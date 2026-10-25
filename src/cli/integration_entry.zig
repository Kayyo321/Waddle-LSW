//! C-facing entry for `waddle integration`. Resolves HOME, the XDG base directories,
//! the running binary path, and the selected device's integration.exe_handler policy,
//! then runs the pure command module. Exercised end to end by the Python integration
//! test; the module-level gate covers integration_commands.zig.
const std = @import("std");
const integration = @import("integration_commands");
const c = @cImport({
    @cInclude("daemon_device.h");
    @cInclude("daemon_config.h");
});

/// Read the exe-handler policy of the device that `device` names, or of the saved
/// default when it is null. A device or default that is not registered yields the
/// specification default (waddle). An explicitly named device must exist.
fn exe_handler_enabled(device: ?[]const u8) !bool {
    var list: c.device_list_t = undefined;
    if (c.daemon_device_list(&list) < 0) return error.Registry;
    var selected: [64]u8 = undefined;
    const wanted: []const u8 = device orelse blk: {
        if (c.daemon_device_default_get(&selected, selected.len) != 0) return error.Registry;
        const name = std.mem.sliceTo(&selected, 0);
        if (name.len == 0) return true;
        break :blk name;
    };
    for (list.devices[0..@intCast(list.count)]) |*info| {
        if (!std.mem.eql(u8, std.mem.sliceTo(&info.name, 0), wanted)) continue;
        var cfg: c.daemon_config_t = undefined;
        c.daemon_config_init_defaults(&cfg);
        if (c.daemon_config_load_file(&cfg, &info.config_path) != 0) return error.InvalidConfig;
        return cfg.integration_exe_handler == c.IntegrationExeHandlerWaddle;
    }
    if (device != null) return error.NotFound;
    return true;
}

/// Entry point called by host.c for `waddle integration ...`.
export fn waddle_integration_command(argc: c_int, argv: [*]const [*:0]const u8) c_int {
    if (argc < 0 or argc > 64) return 2;
    const allocator = std.heap.c_allocator;
    var args: [64][]const u8 = undefined;
    for (argv[0..@intCast(argc)], 0..) |arg, index| args[index] = std.mem.span(arg);
    const options = integration.parse(args[0..@intCast(argc)]) catch {
        std.io.getStdErr().writer().writeAll("waddle integration: usage: see waddle integration --help\n") catch return 125;
        return 2;
    };
    const enabled = exe_handler_enabled(options.device) catch |err| {
        const message = if (err == error.NotFound) "waddle integration: not_found: the named device is not registered\n" else "waddle integration: registry_error: device policy could not be read\n";
        std.io.getStdErr().writer().writeAll(message) catch return 125;
        return 1;
    };
    const paths = integration.resolve_paths(allocator, std.posix.getenv("HOME"), std.posix.getenv("XDG_DATA_HOME"), std.posix.getenv("XDG_STATE_HOME")) catch {
        std.io.getStdErr().writer().writeAll("waddle integration: invalid_environment: HOME and XDG directories must be absolute\n") catch return 125;
        return 1;
    };
    defer allocator.free(paths.data_home);
    defer allocator.free(paths.state_home);
    const config_home = integration.resolve_config(allocator, std.posix.getenv("HOME"), std.posix.getenv("XDG_CONFIG_HOME")) catch {
        std.io.getStdErr().writer().writeAll("waddle integration: invalid_environment: HOME and XDG directories must be absolute\n") catch return 125;
        return 1;
    };
    defer allocator.free(config_home);
    const launcher = std.fs.selfExePathAlloc(allocator) catch return 125;
    defer allocator.free(launcher);
    const env = integration.env_t{
        .data_home = paths.data_home,
        .state_home = paths.state_home,
        .config_home = config_home,
        .launcher = launcher,
        .now = std.time.timestamp(),
        .euid = std.os.linux.geteuid(),
        .exe_handler_enabled = enabled,
    };
    var output = integration.run(allocator, options, env) catch return 125;
    defer output.deinit(allocator);
    std.io.getStdOut().writeAll(output.stdout) catch return 125;
    std.io.getStdErr().writeAll(output.stderr) catch return 125;
    return output.status;
}
