//! @file daemon_config.zig
//! @brief Memory-safe INI configuration parser and validator for the Waddle daemon.
//!
//! Provides parsing and validation for ~/.config/waddle/config.ini, VM specs,
//! timeouts, and VirtIO-FS directory export rules using bounds-checked Zig slices.

const std = @import("std");

const c = @cImport({
    @cInclude("waddle/daemon_protocol.h");
    @cInclude("daemon_config.h");
});

/// Resolves leading ~ to $HOME. Returns slice within out buffer.
fn expand_tilde(input: []const u8, out: []u8) []const u8 {
    if (input.len == 0) return "";
    if (input[0] == '~' and (input.len == 1 or input[1] == '/')) {
        const home = std.posix.getenv("HOME") orelse "/root";
        const suffix = if (input.len > 1) input[1..] else "";
        if (home.len + suffix.len >= out.len) {
            return input;
        }
        @memcpy(out[0..home.len], home);
        @memcpy(out[home.len..][0..suffix.len], suffix);
        return out[0 .. home.len + suffix.len];
    }
    const len = @min(input.len, out.len - 1);
    @memcpy(out[0..len], input[0..len]);
    return out[0..len];
}

/// Copies a string slice into a fixed NUL-terminated C buffer.
fn copy_to_c_buf(dst: []u8, src: []const u8) void {
    if (dst.len == 0) return;
    const len = @min(src.len, dst.len - 1);
    @memcpy(dst[0..len], src[0..len]);
    dst[len] = 0;
}

/// Parses an unsigned 32-bit integer from a string slice with bounds checking.
fn parse_u32(s: []const u8) !u32 {
    const trimmed = std.mem.trim(u8, s, " \t\r\n");
    if (trimmed.len == 0) return error.Empty;
    var val: u64 = 0;
    for (trimmed) |ch| {
        if (ch < '0' or ch > '9') return error.InvalidDigit;
        val = (val * 10) + (ch - '0');
        if (val > std.math.maxInt(u32)) return error.Overflow;
    }
    return @intCast(val);
}

/// Parses an export specification: <host_path>:<guest_drive>[:<rw|ro>]
fn parse_export_internal(line: []const u8, mount: *c.waddle_daemon_fs_mount_t) !void {
    const trimmed = std.mem.trim(u8, line, " \t\r\n");
    if (trimmed.len == 0) return error.Empty;

    // First colon separates host_path from the rest
    const first_colon = std.mem.indexOfScalar(u8, trimmed, ':') orelse return error.InvalidFormat;
    const host_part = trimmed[0..first_colon];
    var rest = trimmed[first_colon + 1 ..];

    // Host path must start with '/'
    if (host_part.len == 0 or host_part[0] != '/') return error.InvalidHostPath;
    if (host_part.len >= c.WaddleMaxPathLen) return error.HostPathTooLong;

    if (rest.len < 1 or !std.ascii.isAlphabetic(rest[0])) {
        return error.InvalidGuestDrive;
    }

    var read_only: u32 = 0;
    var drive_candidate = rest;

    // Check if there is a mode colon after the drive specification (which starts at rest[0..])
    // A drive is at least 1 char (e.g. "Z"), optionally with ':' at index 1 and '\' at index 2.
    // Any mode separator colon must appear after the drive colon (i.e. at or after index 2).
    if (std.mem.indexOfScalarPos(u8, rest, 2, ':')) |colon_idx| {
        const mode = std.mem.trim(u8, rest[colon_idx + 1 ..], " \t\r\n");
        if (std.mem.eql(u8, mode, "ro")) {
            read_only = 1;
        } else if (std.mem.eql(u8, mode, "rw")) {
            read_only = 0;
        } else {
            return error.InvalidMode;
        }
        drive_candidate = rest[0..colon_idx];
    }

    const drive_part = std.mem.trim(u8, drive_candidate, " \t\r\n");
    if (drive_part.len < 1 or drive_part.len > 3 or !std.ascii.isAlphabetic(drive_part[0])) {
        return error.InvalidGuestDrive;
    }
    if (drive_part.len >= 2 and drive_part[1] != ':') {
        return error.InvalidGuestDrive;
    }
    if (drive_part.len == 3 and drive_part[2] != '\\' and drive_part[2] != '/') {
        return error.InvalidGuestDrive;
    }

    var drive_buf: [32]u8 = undefined;
    drive_buf[0] = std.ascii.toUpper(drive_part[0]);
    drive_buf[1] = ':';
    drive_buf[2] = '\\';
    const drive_len: usize = 3;

    // Populate mount struct
    @memset(std.mem.asBytes(mount), 0);
    copy_to_c_buf(&mount.host_path, host_part);
    copy_to_c_buf(&mount.guest_drive, drive_buf[0..drive_len]);
    mount.read_only = read_only;
}

/// Initializes a daemon configuration structure with default settings.
export fn daemon_config_init_defaults(config: ?*c.daemon_config_t) void {
    const cfg = config orelse return;
    @memset(std.mem.asBytes(cfg), 0);

    cfg.memory_mb = c.ConfigDefaultMemoryMb;
    cfg.vcpus = c.ConfigDefaultVcpus;
    cfg.vsock_cid = c.WaddleDefaultVsockCid;
    cfg.vsock_port = c.WaddleDefaultVsockPortVal;
    cfg.start_timeout_sec = c.ConfigDefaultStartTimeoutSec;
    cfg.stop_timeout_sec = c.ConfigDefaultStopTimeoutSec;

    copy_to_c_buf(&cfg.default_shell, "powershell.exe");

    var path_buf: [c.WaddleMaxPathLen]u8 = undefined;
    const resolved_disk = expand_tilde("~/.local/state/waddle/vm/windows.qcow2", &path_buf);
    copy_to_c_buf(&cfg.disk_image, resolved_disk);

    // Default primary mount: $HOME -> Z:\ (rw)
    const home = std.posix.getenv("HOME") orelse "/home/dev";
    cfg.mount_count = 1;
    copy_to_c_buf(&cfg.mounts[0].host_path, home);
    copy_to_c_buf(&cfg.mounts[0].guest_drive, "Z:\\");
    cfg.mounts[0].read_only = 0;
}

/// Validates all fields of a daemon_config_t structure against allowed bounds.
export fn daemon_config_validate(config: ?*const c.daemon_config_t) c_int {
    const cfg = config orelse return -1;

    if (cfg.memory_mb < c.ConfigMinMemoryMb or cfg.memory_mb > c.ConfigMaxMemoryMb) {
        return -1;
    }
    if (cfg.vcpus < c.ConfigMinVcpus or cfg.vcpus > c.ConfigMaxVcpus) {
        return -1;
    }
    if (cfg.vsock_cid < 3 or cfg.vsock_cid == std.math.maxInt(u32)) {
        return -1;
    }
    if (cfg.vsock_port < 1) {
        return -1;
    }
    if (cfg.start_timeout_sec < 1 or cfg.start_timeout_sec > c.ConfigMaxStartTimeoutSec) {
        return -1;
    }
    if (cfg.stop_timeout_sec < 1 or cfg.stop_timeout_sec > c.ConfigMaxStopTimeoutSec) {
        return -1;
    }
    if (cfg.disk_image[0] == 0) {
        return -1;
    }
    if (cfg.default_shell[0] == 0) {
        return -1;
    }
    if (cfg.mount_count > c.WaddleMaxMounts) {
        return -1;
    }

    var i: usize = 0;
    while (i < cfg.mount_count) : (i += 1) {
        const m = &cfg.mounts[i];
        if (m.host_path[0] != '/') return -1;
        if (m.guest_drive[0] == 0) return -1;
    }

    if (cfg.av_enabled > 1 or cfg.av_reserved != 0) return -1;
    const av_path_end = std.mem.indexOfScalar(u8, &cfg.av_shm_path, 0) orelse return -1;
    const gpu_end = std.mem.indexOfScalar(u8, &cfg.av_gpu_bdf, 0) orelse return -1;
    if (cfg.av_enabled != 0 and !av_path_valid(cfg.av_shm_path[0..av_path_end])) return -1;
    if (!av_gpu_valid(cfg.av_gpu_bdf[0..gpu_end])) return -1;

    return 0;
}

fn av_path_valid(path: []const u8) bool {
    return path.len > 0 and path[0] == '/' and std.unicode.utf8ValidateSlice(path) and
        std.mem.indexOfAny(u8, path, ",\r\n") == null;
}
fn av_gpu_valid(value: []const u8) bool {
    if (value.len == 0) return true;
    if (value.len != 12 or value[4] != ':' or value[7] != ':' or value[10] != '.' or value[11] < '0' or value[11] > '7') return false;
    for ([_]usize{ 0, 1, 2, 3, 5, 6, 8, 9 }) |index| {
        if (!std.ascii.isHex(value[index])) return false;
    }
    return (std.fmt.parseInt(u8, value[8..10], 16) catch return false) <= 31;
}

/// Parses a single VirtIO-FS export string specification.
export fn daemon_config_parse_export(line: ?[*:0]const u8, mount: ?*c.waddle_daemon_fs_mount_t) c_int {
    const l = line orelse return -1;
    const m = mount orelse return -1;
    parse_export_internal(std.mem.span(l), m) catch return -1;
    return 0;
}

/// Parses an INI configuration string and updates the configuration structure.
export fn daemon_config_parse_string(config: ?*c.daemon_config_t, ini_data: ?[*]const u8, ini_len: usize) c_int {
    const destination = config orelse return -1;
    var candidate = destination.*;
    const cfg = &candidate;
    if (ini_len == 0) return 0;
    if (ini_len > 65536) return -1;
    const data = (ini_data orelse return -1)[0..ini_len];
    if (!std.unicode.utf8ValidateSlice(data) or std.mem.indexOfScalar(u8, data, 0) != null) return -1;

    var mounts_cleared = false;
    var current_section: enum { none, subsystem, filesystem, timeouts, av } = .none;

    var line_iter = std.mem.splitScalar(u8, data, '\n');
    while (line_iter.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (line.len == 0 or line[0] == '#' or line[0] == ';') {
            continue;
        }

        // Section header
        if (line[0] == '[') {
            if (line[line.len - 1] != ']') return -1;
            const sec_name = std.mem.trim(u8, line[1 .. line.len - 1], " \t");
            if (std.mem.eql(u8, sec_name, "subsystem")) {
                current_section = .subsystem;
            } else if (std.mem.eql(u8, sec_name, "filesystem")) {
                current_section = .filesystem;
                if (!mounts_cleared) {
                    cfg.mount_count = 0;
                    mounts_cleared = true;
                }
            } else if (std.mem.eql(u8, sec_name, "av")) {
                current_section = .av;
            } else if (std.mem.eql(u8, sec_name, "timeouts")) {
                current_section = .timeouts;
            } else {
                current_section = .none;
            }
            continue;
        }

        // Key = Value
        const eq_pos = std.mem.indexOfScalar(u8, line, '=') orelse return -1;
        const key = std.mem.trim(u8, line[0..eq_pos], " \t");
        const val = std.mem.trim(u8, line[eq_pos + 1 ..], " \t");

        switch (current_section) {
            .subsystem => {
                if (std.mem.eql(u8, key, "memory_mb")) {
                    cfg.memory_mb = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "vcpus")) {
                    cfg.vcpus = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "disk_image")) {
                    var tilde_buf: [c.WaddleMaxPathLen]u8 = undefined;
                    const resolved = expand_tilde(val, &tilde_buf);
                    if (resolved.len == 0 or resolved.len >= cfg.disk_image.len) return -1;
                    copy_to_c_buf(&cfg.disk_image, resolved);
                } else if (std.mem.eql(u8, key, "vsock_cid")) {
                    cfg.vsock_cid = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "vsock_port")) {
                    cfg.vsock_port = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "default_shell")) {
                    if (val.len == 0 or val.len >= cfg.default_shell.len) return -1;
                    copy_to_c_buf(&cfg.default_shell, val);
                }
            },
            .filesystem => {
                // If user specifies mounts in INI, clear defaults on first mount
                if (!mounts_cleared) {
                    cfg.mount_count = 0;
                    mounts_cleared = true;
                }
                if (cfg.mount_count < c.WaddleMaxMounts) {
                    parse_export_internal(val, &cfg.mounts[cfg.mount_count]) catch return -1;
                    cfg.mount_count += 1;
                } else {
                    return -1; // Exceeded WaddleMaxMounts
                }
            },
            .timeouts => {
                if (std.mem.eql(u8, key, "start_timeout")) {
                    cfg.start_timeout_sec = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "stop_timeout")) {
                    cfg.stop_timeout_sec = parse_u32(val) catch return -1;
                }
            },
            .av => {
                if (std.mem.eql(u8, key, "enabled")) {
                    cfg.av_enabled = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "shm_path")) {
                    if (!av_path_valid(val) or val.len >= cfg.av_shm_path.len) return -1;
                    @memset(&cfg.av_shm_path, 0);
                    copy_to_c_buf(&cfg.av_shm_path, val);
                } else if (std.mem.eql(u8, key, "gpu_bdf")) {
                    if (!av_gpu_valid(val)) return -1;
                    @memset(&cfg.av_gpu_bdf, 0);
                    copy_to_c_buf(&cfg.av_gpu_bdf, val);
                } else return -1;
            },
            .none => {},
        }
    }

    if (daemon_config_validate(cfg) != 0) return -1;
    destination.* = candidate;
    return 0;
}

/// Loads and parses a configuration file from the filesystem.
export fn daemon_config_load_file(config: ?*c.daemon_config_t, path: ?[*:0]const u8) c_int {
    const cfg = config orelse return -1;

    var resolved_path_buf: [c.WaddleMaxPathLen]u8 = undefined;
    const file_path: []const u8 = blk: {
        if (path) |p| {
            const span = std.mem.span(p);
            break :blk expand_tilde(span, &resolved_path_buf);
        } else {
            // Check XDG_CONFIG_HOME or HOME/.config/waddle/config.ini
            if (std.posix.getenv("XDG_CONFIG_HOME")) |xdg| {
                const sub = "/waddle/config.ini";
                if (xdg.len + sub.len < resolved_path_buf.len) {
                    @memcpy(resolved_path_buf[0..xdg.len], xdg);
                    @memcpy(resolved_path_buf[xdg.len..][0..sub.len], sub);
                    break :blk resolved_path_buf[0 .. xdg.len + sub.len];
                }
            }
            const home = std.posix.getenv("HOME") orelse "/root";
            const sub = "/.config/waddle/config.ini";
            if (home.len + sub.len < resolved_path_buf.len) {
                @memcpy(resolved_path_buf[0..home.len], home);
                @memcpy(resolved_path_buf[home.len..][0..sub.len], sub);
                break :blk resolved_path_buf[0 .. home.len + sub.len];
            }
            return -1;
        }
    };

    // Open file
    const file = std.fs.openFileAbsolute(file_path, .{}) catch |err| {
        if (err == error.FileNotFound) {
            return 0; // Missing config file is not an error; retain defaults
        }
        return -1;
    };
    defer file.close();

    if ((file.stat() catch return -1).size > 65536) return -1;

    // Read up to 64 KiB
    var file_buf: [65536]u8 = undefined;
    const bytes_read = file.readAll(&file_buf) catch return -1;

    return daemon_config_parse_string(cfg, &file_buf, bytes_read);
}

test "daemon_config: defaults and validation" {
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);

    try std.testing.expectEqual(@as(u32, 4096), cfg.memory_mb);
    try std.testing.expectEqual(@as(u32, 4), cfg.vcpus);
    try std.testing.expectEqual(@as(u32, 3), cfg.vsock_cid);
    try std.testing.expectEqual(@as(u32, 5242), cfg.vsock_port);
    try std.testing.expectEqual(@as(u32, 60), cfg.start_timeout_sec);
    try std.testing.expectEqual(@as(u32, 15), cfg.stop_timeout_sec);
    try std.testing.expectEqual(@as(u32, 1), cfg.mount_count);
    try std.testing.expectEqualStrings("Z:\\", std.mem.span(@as([*:0]const u8, @ptrCast(&cfg.mounts[0].guest_drive))));
    try std.testing.expectEqual(@as(c_int, 0), daemon_config_validate(&cfg));
}

test "daemon_config: INI string parsing" {
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);

    const sample_ini =
        \\# Sample Waddle subsystem configuration
        \\[subsystem]
        \\memory_mb = 8192
        \\vcpus = 8
        \\disk_image = /var/lib/waddle/win10.qcow2
        \\vsock_cid = 4
        \\vsock_port = 5555
        \\default_shell = cmd.exe
        \\
        \\[filesystem]
        \\export_primary = /home/user:Z:\:rw
        \\export_secondary = /data/shared:X:\:ro
        \\
        \\[timeouts]
        \\start_timeout = 90
        \\stop_timeout = 30
    ;

    try std.testing.expectEqual(@as(c_int, 0), daemon_config_parse_string(&cfg, sample_ini.ptr, sample_ini.len));
    try std.testing.expectEqual(@as(u32, 8192), cfg.memory_mb);
    try std.testing.expectEqual(@as(u32, 8), cfg.vcpus);
    try std.testing.expectEqualStrings("/var/lib/waddle/win10.qcow2", std.mem.span(@as([*:0]const u8, @ptrCast(&cfg.disk_image))));
    try std.testing.expectEqual(@as(u32, 4), cfg.vsock_cid);
    try std.testing.expectEqual(@as(u32, 5555), cfg.vsock_port);
    try std.testing.expectEqualStrings("cmd.exe", std.mem.span(@as([*:0]const u8, @ptrCast(&cfg.default_shell))));
    try std.testing.expectEqual(@as(u32, 2), cfg.mount_count);
    try std.testing.expectEqualStrings("/home/user", std.mem.span(@as([*:0]const u8, @ptrCast(&cfg.mounts[0].host_path))));
    try std.testing.expectEqualStrings("Z:\\", std.mem.span(@as([*:0]const u8, @ptrCast(&cfg.mounts[0].guest_drive))));
    try std.testing.expectEqual(@as(u32, 0), cfg.mounts[0].read_only);
    try std.testing.expectEqualStrings("/data/shared", std.mem.span(@as([*:0]const u8, @ptrCast(&cfg.mounts[1].host_path))));
    try std.testing.expectEqualStrings("X:\\", std.mem.span(@as([*:0]const u8, @ptrCast(&cfg.mounts[1].guest_drive))));
    try std.testing.expectEqual(@as(u32, 1), cfg.mounts[1].read_only);
    try std.testing.expectEqual(@as(u32, 90), cfg.start_timeout_sec);
    try std.testing.expectEqual(@as(u32, 30), cfg.stop_timeout_sec);
}

test "daemon_config: export parsing edge cases" {
    var mount: c.waddle_daemon_fs_mount_t = undefined;
    try std.testing.expectEqual(@as(c_int, 0), daemon_config_parse_export("/home/dev:Z:\\:rw", &mount));
    try std.testing.expectEqualStrings("/home/dev", std.mem.span(@as([*:0]const u8, @ptrCast(&mount.host_path))));
    try std.testing.expectEqualStrings("Z:\\", std.mem.span(@as([*:0]const u8, @ptrCast(&mount.guest_drive))));
    try std.testing.expectEqual(@as(u32, 0), mount.read_only);

    try std.testing.expectEqual(@as(c_int, 0), daemon_config_parse_export("/mnt/backup:Y:\\:ro", &mount));
    try std.testing.expectEqualStrings("/mnt/backup", std.mem.span(@as([*:0]const u8, @ptrCast(&mount.host_path))));
    try std.testing.expectEqualStrings("Y:\\", std.mem.span(@as([*:0]const u8, @ptrCast(&mount.guest_drive))));
    try std.testing.expectEqual(@as(u32, 1), mount.read_only);

    // Invalid host path (relative)
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_export("relative/path:Z:\\", &mount));
    // Invalid guest drive
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_export("/path:123:\\", &mount));
    // Invalid mode
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_export("/path:Z:\\:invalid", &mount));
}

test "daemon_config: invalid configs rejected" {
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);

    // Memory too low (< 512)
    cfg.memory_mb = 256;
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_validate(&cfg));
    cfg.memory_mb = 4096;

    // VCPUs too low
    cfg.vcpus = 0;
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_validate(&cfg));
    cfg.vcpus = 4;

    // CID invalid (< 3)
    cfg.vsock_cid = 2;
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_validate(&cfg));
    cfg.vsock_cid = 3;

    // Malformed INI syntax
    const bad_ini = "this is not ini";
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_string(&cfg, bad_ini.ptr, bad_ini.len));
}

test "daemon_config: failed parse leaves every original byte unchanged" {
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);
    const original = cfg;
    const inputs = [_][]const u8{
        "[subsystem]\nmemory_mb = 8192\nvcpus = 0\n",
        "[subsystem]\nvsock_cid = 4294967295\n",
        "[subsystem]\ndefault_shell = a\x00b\n",
        "[subsystem]\ndefault_shell = \xff\n",
        "[subsystem]\nvsock_port = 4294967296\n",
    };
    for (inputs) |input| {
        try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_string(&cfg, input.ptr, input.len));
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&original), std.mem.asBytes(&cfg));
    }
    const oversized = try std.testing.allocator.alloc(u8, 65537);
    defer std.testing.allocator.free(oversized);
    @memset(oversized, ' ');
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_string(&cfg, oversized.ptr, oversized.len));
    var long_shell: [300]u8 = undefined;
    @memset(&long_shell, 'a');
    const prefix = "[subsystem]\ndefault_shell = ";
    @memcpy(long_shell[0..prefix.len], prefix);
    try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_string(&cfg, &long_shell, long_shell.len));
}

test "daemon_config: full width vsock port and empty export section" {
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);
    const input = "[subsystem]\nvsock_port = 4294967295\n[filesystem]\n";
    try std.testing.expectEqual(@as(c_int, 0), daemon_config_parse_string(&cfg, input.ptr, input.len));
    try std.testing.expectEqual(@as(u32, 4294967295), cfg.vsock_port);
    try std.testing.expectEqual(@as(u32, 0), cfg.mount_count);
}

/// Mutable key schema; immutable storage borrowed by the editor.
const edit_key_t = struct { name: []const u8, section: []const u8, default: []const u8 };
/// The nine supported mutable settings and their exact reset representations.
const EditKeys = [_]edit_key_t{
    .{ .name = "memory_mb", .section = "subsystem", .default = "4096" },
    .{ .name = "vcpus", .section = "subsystem", .default = "4" },
    .{ .name = "default_shell", .section = "subsystem", .default = "powershell.exe" },
    .{ .name = "vsock_port", .section = "subsystem", .default = "5242" },
    .{ .name = "start_timeout", .section = "timeouts", .default = "60" },
    .{ .name = "stop_timeout", .section = "timeouts", .default = "15" },
    .{ .name = "enabled", .section = "av", .default = "0" },
    .{ .name = "shm_path", .section = "av", .default = "/dev/kvmfr0" },
    .{ .name = "gpu_bdf", .section = "av", .default = "" },
};

/// Bounded all-or-none validation; output remains caller-owned and is never allocated.
fn edit_config(data: []const u8, changes: []const []const u8, reset: bool, output: []u8) !usize {
    if (data.len == 0 or data.len > 65536 or changes.len == 0 or changes.len > EditKeys.len) return error.Invalid;
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);
    if (daemon_config_parse_string(&cfg, data.ptr, data.len) != 0) return error.Invalid;
    var values: [EditKeys.len]?[]const u8 = [_]?[]const u8{null} ** EditKeys.len;
    for (changes) |change| {
        const eq = std.mem.indexOfScalar(u8, change, '=');
        if ((reset and eq != null) or (!reset and eq == null)) return error.Invalid;
        const name = if (eq) |at| change[0..at] else change;
        var index: ?usize = null;
        for (EditKeys, 0..) |key, i| {
            if (std.mem.eql(u8, key.name, name)) index = i;
        }
        const i = index orelse return error.Invalid;
        if (values[i] != null) return error.Invalid;
        const value = if (reset) EditKeys[i].default else change[eq.? + 1 ..];
        if ((value.len == 0 and !std.mem.eql(u8, name, "gpu_bdf")) or
            value.len > (if (std.mem.eql(u8, name, "shm_path")) @as(usize, 1023) else 255) or !std.unicode.utf8ValidateSlice(value) or
            std.mem.indexOfAny(u8, value, "\x00\r\n") != null or
            !std.mem.eql(u8, std.mem.trim(u8, value, " \t"), value)) return error.Invalid;
        values[i] = value;
    }
    // Validate the combined request before rendering even a partial replacement.
    var patch: [2048]u8 = undefined;
    var patch_stream = std.io.fixedBufferStream(&patch);
    for (EditKeys, values) |key, value| {
        if (value) |v| try patch_stream.writer().print("[{s}]\n{s}={s}\n", .{ key.section, key.name, v });
    }
    if (daemon_config_parse_string(&cfg, &patch, patch_stream.pos) != 0) return error.Invalid;
    var stream = std.io.fixedBufferStream(output[0..@min(output.len, 65536)]);
    const writer = stream.writer();
    var seen: [EditKeys.len]bool = [_]bool{false} ** EditKeys.len;
    var section: []const u8 = "";
    var offset: usize = 0;
    while (offset < data.len) {
        const end = if (std.mem.indexOfScalarPos(u8, data, offset, '\n')) |at| at + 1 else data.len;
        const raw = data[offset..end];
        const line = std.mem.trim(u8, raw, " \t\r\n");
        var replaced = false;
        if (line.len > 0 and line[0] == '[') {
            section = std.mem.trim(u8, line[1 .. line.len - 1], " \t");
        } else if (line.len > 0 and line[0] != '#' and line[0] != ';') {
            if (std.mem.indexOfScalar(u8, raw, '=')) |eq| {
                const name = std.mem.trim(u8, raw[0..eq], " \t");
                for (EditKeys, values, 0..) |key, value, i| {
                    if (value != null and std.mem.eql(u8, section, key.section) and std.mem.eql(u8, name, key.name)) {
                        var first = eq + 1;
                        while (first < raw.len and (raw[first] == ' ' or raw[first] == '\t')) : (first += 1) {}
                        var last = raw.len;
                        while (last > first and std.mem.indexOfScalar(u8, " \t\r\n", raw[last - 1]) != null) : (last -= 1) {}
                        try writer.writeAll(raw[0..first]);
                        try writer.writeAll(value.?);
                        try writer.writeAll(raw[last..]);
                        seen[i] = true;
                        replaced = true;
                        break;
                    }
                }
            }
        }
        if (!replaced) try writer.writeAll(raw);
        offset = end;
    }
    for (EditKeys, values, seen) |key, value, found| {
        if (value != null and !found) try writer.print("\n[{s}]\n{s}={s}\n", .{ key.section, key.name, value.? });
    }
    daemon_config_init_defaults(&cfg);
    if (daemon_config_parse_string(&cfg, output.ptr, stream.pos) != 0) return error.Invalid;
    return stream.pos;
}

/// C ABI documented in daemon_config.h; borrowed buffers, no heap storage or side effects.
export fn daemon_config_edit(data: ?[*]const u8, length: usize, changes: ?[*]const ?[*:0]const u8, count: usize, reset: c_int, output: ?[*]u8, capacity: usize, output_length: ?*usize) c_int {
    const result = output_length orelse return -1;
    result.* = 0;
    if (count == 0 or count > EditKeys.len) return -1;
    const args = changes orelse return -1;
    var slices: [EditKeys.len][]const u8 = undefined;
    for (args[0..count], 0..) |arg, i| slices[i] = std.mem.span(arg orelse return -1);
    result.* = edit_config((data orelse return -1)[0..length], slices[0..count], reset != 0, (output orelse return -1)[0..capacity]) catch return -1;
    return 0;
}

test "config editor preserves unknown bytes and replaces repeated effective keys" {
    const original = "# comment\r\n[subsystem]\r\n memory_mb = 4096 \r\nvcpus=4\nopaque = untouched\n" ++
        "[filesystem]\nmount=/tmp:Z:\\:ro\n[subsystem]\nmemory_mb=2048\n[other]\nmemory_mb=123\n";
    var output: [4096]u8 = undefined;
    const length = try edit_config(original, &.{ "memory_mb=8192", "stop_timeout=30" }, false, &output);
    try std.testing.expectEqualStrings("# comment\r\n[subsystem]\r\n memory_mb = 8192 \r\nvcpus=4\nopaque = untouched\n" ++
        "[filesystem]\nmount=/tmp:Z:\\:ro\n[subsystem]\nmemory_mb=8192\n[other]\nmemory_mb=123\n" ++
        "\n[timeouts]\nstop_timeout=30\n", output[0..length]);
    var reset_output: [4096]u8 = undefined;
    const reset_length = try edit_config(output[0..length], &.{"memory_mb"}, true, &reset_output);
    try std.testing.expect(std.mem.indexOf(u8, reset_output[0..reset_length], "memory_mb=4096") != null);
}

test "config editor rejects invalid complete requests and bounded output" {
    const source = "[subsystem]\nmemory_mb=4096\n";
    var output: [4096]u8 = undefined;
    for ([_][]const []const u8{
        &.{},                       &.{"unknown=1"},                &.{"vsock_cid=3"},           &.{"vcpus"},              &.{"vcpus=0"},
        &.{ "vcpus=2", "vcpus=3" }, &.{ "vcpus=2", "memory_mb=1" }, &.{"default_shell="},        &.{"default_shell=a\nb"}, &.{"default_shell=\xff"},
        &.{"default_shell=a\x00b"}, &.{"default_shell= trailing"},  &.{"vsock_port=4294967296"}, &.{"start_timeout=601"},  &.{"stop_timeout=301"},
    }) |args| try std.testing.expectError(error.Invalid, edit_config(source, args, false, &output));
    try std.testing.expectError(error.Invalid, edit_config(source, &.{"vcpus=2"}, true, &output));
    try std.testing.expectError(error.Invalid, edit_config("[", &.{"vcpus=2"}, false, &output));
    try std.testing.expectError(error.NoSpaceLeft, edit_config(source, &.{"vcpus=2"}, false, output[0..4]));
    const length = try edit_config(source, &.{ "vsock_port=4294967295", "default_shell=日本語.exe" }, false, &output);
    try std.testing.expect(std.mem.indexOf(u8, output[0..length], "日本語.exe") != null);
}

test "AV device configuration is bounded and transactional" {
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);
    try std.testing.expectEqual(@as(u32, 0), cfg.av_enabled);
    const valid = "[av]\nenabled=1\nshm_path=/dev/shm/waddle-private-av\ngpu_bdf=0000:0e:00.0\n";
    try std.testing.expectEqual(@as(c_int, 0), daemon_config_parse_string(&cfg, valid.ptr, valid.len));
    try std.testing.expectEqual(@as(u32, 1), cfg.av_enabled);
    const before = cfg;
    for ([_][]const u8{
        "[av]\nenabled=2\n",            "[av]\nshm_path=relative\n",    "[av]\nshm_path=/tmp/x,share=off\n",
        "[av]\ngpu_bdf=0000:0e:ff.0\n", "[av]\ngpu_bdf=0000:0e:00.8\n", "[av]\nunknown=1\n",
        "[av]\ngpu_bdf=invalid\n",      "[av]\nshm_path=\n",
    }) |invalid| {
        try std.testing.expectEqual(@as(c_int, -1), daemon_config_parse_string(&cfg, invalid.ptr, invalid.len));
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&before), std.mem.asBytes(&cfg));
    }
}

test "AV config edits enable only a complete bounded environment" {
    const original = "[subsystem]\nmemory_mb=4096\n";
    var output: [4096]u8 = undefined;
    try std.testing.expectError(error.Invalid, edit_config(original, &.{"enabled=1"}, false, &output));
    const length = try edit_config(original, &.{ "enabled=1", "shm_path=/dev/shm/private-av", "gpu_bdf=" }, false, &output);
    var cfg: c.daemon_config_t = undefined;
    daemon_config_init_defaults(&cfg);
    try std.testing.expectEqual(@as(c_int, 0), daemon_config_parse_string(&cfg, output[0..length].ptr, length));
    try std.testing.expectEqual(@as(u32, 1), cfg.av_enabled);
    var reset_output: [4096]u8 = undefined;
    _ = try edit_config(output[0..length], &.{ "enabled", "shm_path", "gpu_bdf" }, true, &reset_output);
    try std.testing.expectError(error.Invalid, edit_config(original, &.{"shm_path=/tmp/x,share=off"}, false, &output));
    try std.testing.expectError(error.Invalid, edit_config(original, &.{"gpu_bdf=0000:00:20.0"}, false, &output));
}
