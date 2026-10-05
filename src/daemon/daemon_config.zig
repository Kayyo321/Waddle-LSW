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
    if (cfg.vsock_cid < 3) {
        return -1;
    }
    if (cfg.vsock_port < 1 or cfg.vsock_port > 65535) {
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

    return 0;
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
    const cfg = config orelse return -1;
    if (ini_len == 0) return 0;
    const data = (ini_data orelse return -1)[0..ini_len];

    var mounts_cleared = false;
    var current_section: enum { none, subsystem, filesystem, timeouts } = .none;

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
                    copy_to_c_buf(&cfg.disk_image, resolved);
                } else if (std.mem.eql(u8, key, "vsock_cid")) {
                    cfg.vsock_cid = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "vsock_port")) {
                    cfg.vsock_port = parse_u32(val) catch return -1;
                } else if (std.mem.eql(u8, key, "default_shell")) {
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
            .none => {},
        }
    }

    return daemon_config_validate(cfg);
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
