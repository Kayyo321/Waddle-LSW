//! Bounded AV CLI grammar; C owns configuration and process lifecycles.
const std = @import("std");
const c = @cImport({
    @cInclude("av_commands.h");
});
/// Pure canonical PCI address validation; no retained input or allocation.
fn valid_gpu(value: []const u8) bool {
    if (value.len != 12 or value[4] != ':' or value[7] != ':' or value[10] != '.' or value[11] < '0' or value[11] > '7') return false;
    for (value, 0..) |byte, index| {
        if (index == 4 or index == 7 or index == 10 or index == 11) continue;
        if (!std.ascii.isHex(byte) or (byte >= 'A' and byte <= 'F')) return false;
    }
    return (std.fmt.parseInt(u8, value[8..10], 16) catch return false) < 32;
}
/// Validate command and copy only after all values pass; empty/default device supported.
fn parse(args: []const []const u8) !c.av_command_t {
    if (args.len == 0 or args.len > 8) return error.Invalid;
    var command = std.mem.zeroes(c.av_command_t);
    command.kind = if (std.mem.eql(u8, args[0], "setup")) c.AvSetup else if (std.mem.eql(u8, args[0], "probe")) c.AvProbe else if (std.mem.eql(u8, args[0], "run")) c.AvRun else return error.Invalid;
    var index: usize = 1;
    while (index < args.len) : (index += 1) {
        const arg = args[index];
        if (std.mem.eql(u8, arg, "--uefi")) {
            if (command.kind != c.AvSetup or command.uefi != 0) return error.Invalid;
            command.uefi = 1;
        } else if (std.mem.eql(u8, arg, "--kvmfr")) {
            if (command.kind != c.AvSetup or command.kvmfr != 0) return error.Invalid;
            command.kvmfr = 1;
        } else if (std.mem.eql(u8, arg, "--device") or std.mem.eql(u8, arg, "--gpu")) {
            index += 1;
            if (index >= args.len) return error.Invalid;
            const value = args[index];
            if (std.mem.eql(u8, arg, "--device")) {
                if (command.device[0] != 0 or value.len == 0 or value.len > 63) return error.Invalid;
                for (value) |byte| if (!std.ascii.isAlphanumeric(byte) and byte != '-' and byte != '_') return error.Invalid;
                @memcpy(command.device[0..value.len], value);
            } else {
                if (command.kind != c.AvSetup or command.gpu_bdf[0] != 0 or !valid_gpu(value)) return error.Invalid;
                @memcpy(command.gpu_bdf[0..value.len], value);
            }
        } else {
            if (command.kind != c.AvRun or command.process_id != 0 or arg.len == 0 or arg.len > 10) return error.Invalid;
            for (arg) |byte| if (!std.ascii.isDigit(byte)) return error.Invalid;
            command.process_id = std.fmt.parseInt(u32, arg, 10) catch return error.Invalid;
            if (command.process_id == 0) return error.Invalid;
        }
    }
    if (command.kind == c.AvRun and command.process_id == 0) return error.Invalid;
    return command;
}
/// C ABI contract in av_commands.h; argv borrowed during call, result private.
export fn waddle_av_parse(count: usize, arguments: ?[*]const ?[*:0]const u8, command: ?*c.av_command_t) c_int {
    const result = command orelse return -1;
    if (count == 0 or count > 8) return -1;
    const argv = arguments orelse return -1;
    var args: [8][]const u8 = undefined;
    for (argv[0..count], 0..) |arg, index| {
        const value = std.mem.span(arg orelse return -1);
        if (value.len > 63 or !std.unicode.utf8ValidateSlice(value)) return -1;
        args[index] = value;
        if (std.mem.eql(u8, value, "--help") or std.mem.eql(u8, value, "-h")) return 1;
    }
    result.* = parse(args[0..count]) catch return -1;
    return 0;
}
test "AV grammar rejects ambiguous and unsafe provisioning inputs" {
    const command = try parse(&.{ "setup", "--device", "gaming", "--gpu", "0000:0e:00.0", "--kvmfr" });
    try std.testing.expectEqual(@as(u32, c.AvSetup), command.kind);
    try std.testing.expectEqual(@as(u32, 1), command.kvmfr);
    try std.testing.expectEqualStrings("gaming", std.mem.sliceTo(&command.device, 0));
    try std.testing.expectEqual(@as(u32, 4294967295), (try parse(&.{ "run", "4294967295" })).process_id);
    _ = try parse(&.{"probe"});
    for ([_][]const []const u8{ &.{}, &.{"unknown"}, &.{"run"}, &.{ "run", "0" }, &.{ "run", "+1" }, &.{ "run", "4294967296" }, &.{ "run", "1", "2" }, &.{ "setup", "--device", "../x" }, &.{ "setup", "--gpu", "0000:00:20.0" }, &.{ "setup", "--gpu", "0000:0e:00.8" }, &.{ "setup", "--gpu", "0000:0E:00.0" }, &.{ "setup", "--device" }, &.{ "probe", "--kvmfr" }, &.{ "setup", "--kvmfr", "--kvmfr" }, &.{ "probe", "--device", "a", "--device", "b" } }) |args| try std.testing.expectError(error.Invalid, parse(args));
}
