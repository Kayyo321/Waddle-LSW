//! Pure producers for the per-user executable handler and the schema-1 manifest
//! encoder. Filesystem authority stays with `manifest_store.store_t`; this module
//! renders artifact bytes and candidate manifests and never opens a path.
const std = @import("std");
const manifest = @import("manifest_store").manifest;

/// Waddle version recorded as `created_by` and as the schema root `waddle_version`.
pub const WaddleVersion: []const u8 = "0.2.0";

/// Manifest record identifier of the per-user executable handler entry.
pub const HandlerRecordId: []const u8 = "handler:waddle-open";

/// Leaf name of the handler desktop entry below the Data root `applications/`.
pub const HandlerLeaf: []const u8 = "waddle-open.desktop";

/// Frozen MIME types claimed by the executable handler (IMPL_DESC 3.14).
pub const HandlerMimeTypes = [_][]const u8{
    "application/x-ms-dos-executable",
    "application/x-msdownload",
    "application/vnd.microsoft.portable-executable",
    "application/x-msi",
    "application/x-ms-shortcut",
    "application/x-msdos-program",
    "application/x-ms-installer",
};

/// Maximum launcher path bytes accepted for the handler `Exec` and `TryExec` lines.
pub const MaxLauncherBytes: usize = 4096;

/// Errors specific to producer input validation.
pub const ProducerError = error{ InvalidLauncher, InvalidDevice, InvalidDocument };

/// Root-level schema fields of a manifest document.
pub const root_fields_t = struct {
    /// Waddle version that wrote the document; `WaddleVersion` for new writes.
    waddle_version: []const u8,
    /// Update timestamp in the frozen `YYYY-MM-DDTHH:MM:SSZ` form.
    updated: []const u8,
};

/// Validate a launcher path for embedding in a desktop entry. It must be absolute,
/// within `MaxLauncherBytes`, and free of control characters that a desktop entry
/// cannot represent.
pub fn launcher_valid(path: []const u8) bool {
    if (path.len < 2 or path.len > MaxLauncherBytes or path[0] != '/') return false;
    if (!std.unicode.utf8ValidateSlice(path)) return false;
    for (path) |byte| if (byte < 0x20 or byte == 0x7f) return false;
    return true;
}

/// Validate a device name with the frozen identifier alphabet (ASCII letters,
/// digits, hyphen, underscore; 1..63 bytes).
pub fn device_valid(name: []const u8) bool {
    if (name.len == 0 or name.len >= 64) return false;
    for (name) |byte| if (!std.ascii.isAlphanumeric(byte) and byte != '-' and byte != '_') return false;
    return true;
}

/// Quote one argument for an `Exec=` key using the same rules as
/// `desktop_artifacts.zig`: double quotes, `%` doubled, `"`, backtick, `$`, and
/// backslash escaped, then desktop-value escaping of backslashes. Returns owned bytes.
pub fn exec_quote(allocator: std.mem.Allocator, arg: []const u8) ![]u8 {
    var quoted = std.ArrayList(u8).init(allocator);
    defer quoted.deinit();
    try quoted.append('"');
    for (arg) |byte| {
        if (byte == '%') try quoted.append('%');
        if (byte == '"' or byte == '`' or byte == '$' or byte == '\\') try quoted.append('\\');
        try quoted.append(byte);
    }
    try quoted.append('"');
    return desktop_value(allocator, quoted.items);
}

/// Escape one desktop-entry value: backslash, LF, TAB, and CR become escape pairs.
pub fn desktop_value(allocator: std.mem.Allocator, value: []const u8) ![]u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    for (value) |byte| switch (byte) {
        '\\' => try out.appendSlice("\\\\"),
        '\n' => try out.appendSlice("\\n"),
        '\t' => try out.appendSlice("\\t"),
        '\r' => try out.appendSlice("\\r"),
        else => try out.append(byte),
    };
    return out.toOwnedSlice();
}

/// Render the frozen per-user executable handler desktop entry (IMPL_DESC 3.14).
/// `launcher` is the absolute path of the running Waddle binary; `device`, when
/// present, is the device the handler opens against. Returns owned UTF-8 bytes.
/// Errors: InvalidLauncher, InvalidDevice, or OutOfMemory.
pub fn handler_desktop(allocator: std.mem.Allocator, launcher: []const u8, device: ?[]const u8) ![]u8 {
    if (!launcher_valid(launcher)) return error.InvalidLauncher;
    if (device) |name| if (!device_valid(name)) return error.InvalidDevice;
    const exec_path = try exec_quote(allocator, launcher);
    defer allocator.free(exec_path);
    const try_exec = try desktop_value(allocator, launcher);
    defer allocator.free(try_exec);
    const exec_device = if (device) |name| try std.fmt.allocPrint(allocator, " --device {s}", .{name}) else try allocator.dupe(u8, "");
    defer allocator.free(exec_device);
    var mime = std.ArrayList(u8).init(allocator);
    defer mime.deinit();
    for (HandlerMimeTypes) |value| {
        try mime.appendSlice(value);
        try mime.append(';');
    }
    return std.fmt.allocPrint(allocator, "[Desktop Entry]\nType=Application\nName=Run with Windows (Waddle)\nExec={s} open{s} %F\nTryExec={s}\nIcon=waddle\nNoDisplay=true\nTerminal=false\nMimeType={s}\nX-Waddle-Schema=1\n", .{ exec_path, exec_device, try_exec, mime.items });
}

/// Build the handler record for the rendered artifact bytes. `path` is the absolute
/// canonical path under the Data root; `digest` is the lowercase hex SHA-256 of the bytes.
pub fn handler_record(path: []const u8, digest: []const u8, device: ?[]const u8) manifest.record_t {
    return .{
        .id = HandlerRecordId,
        .kind = .HandlerEntry,
        .path = path,
        .sha256 = digest,
        .device = device,
        .app_key = null,
        .created_by = WaddleVersion,
        .prior = .None,
        .mime = null,
    };
}

/// Wire spelling of a record kind; the inverse of the manifest codec's kind parser.
pub fn kind_wire(kind: manifest.record_kind_t) []const u8 {
    return switch (kind) {
        .DesktopEntry => "desktop_entry",
        .Icon => "icon",
        .Menu => "menu",
        .DesktopShortcut => "desktop_shortcut",
        .MimeDefault => "mime_default",
        .Autostart => "autostart",
        .HandlerEntry => "handler_entry",
        .Binfmt => "binfmt",
        .Polkit => "polkit",
        .GuestPolicy => "guest_policy",
        .GuestComponent => "guest_component",
        .GuestAutostart => "guest_autostart",
        .GuestCertificate => "guest_certificate",
        .GuestPrinter => "guest_printer",
    };
}

/// Write one JSON string with the standard escapes.
fn write_json_string(writer: anytype, text: []const u8) !void {
    try std.json.stringify(text, .{}, writer);
}

/// Serialize a complete schema-1 document in canonical member order. Optional
/// members are emitted only when present, `prior` is null, a string, or an integer.
/// Returns owned bytes that `manifest.parse` accepts when the records are valid.
pub fn encode_document(allocator: std.mem.Allocator, root: root_fields_t, records: []const manifest.record_t) ![]u8 {
    if (records.len > manifest.MaxManifestRecords) return error.InvalidDocument;
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    const writer = out.writer();
    try writer.writeAll("{\"schema\":1,\"waddle_version\":");
    try write_json_string(writer, root.waddle_version);
    try writer.writeAll(",\"updated\":");
    try write_json_string(writer, root.updated);
    try writer.writeAll(",\"records\":[");
    for (records, 0..) |record, index| {
        if (index != 0) try writer.writeByte(',');
        try writer.writeAll("{\"id\":");
        try write_json_string(writer, record.id);
        try writer.writeAll(",\"kind\":");
        try write_json_string(writer, kind_wire(record.kind));
        try writer.writeAll(",\"path\":");
        try write_json_string(writer, record.path);
        try writer.writeAll(",\"sha256\":");
        try write_json_string(writer, record.sha256);
        if (record.device) |device| {
            try writer.writeAll(",\"device\":");
            try write_json_string(writer, device);
        }
        if (record.app_key) |app_key| {
            try writer.writeAll(",\"app_key\":");
            try write_json_string(writer, app_key);
        }
        try writer.writeAll(",\"created_by\":");
        try write_json_string(writer, record.created_by);
        try writer.writeAll(",\"prior\":");
        switch (record.prior) {
            .None => try writer.writeAll("null"),
            .Text => |text| try write_json_string(writer, text),
            .U32Value => |value| try writer.print("{d}", .{value}),
        }
        if (record.mime) |mime| {
            try writer.writeAll(",\"mime\":");
            try write_json_string(writer, mime);
        }
        try writer.writeByte('}');
    }
    try writer.writeAll("]}");
    return out.toOwnedSlice();
}

/// Build the candidate document that differs from `records` only by one record:
/// `replacement` null deletes the record `target_id`; non-null replaces it or appends
/// it when absent. Root fields come from `root`. Returns owned bytes.
pub fn candidate_document(allocator: std.mem.Allocator, root: root_fields_t, records: []const manifest.record_t, target_id: []const u8, replacement: ?manifest.record_t) ![]u8 {
    var merged = std.ArrayList(manifest.record_t).init(allocator);
    defer merged.deinit();
    var placed = false;
    for (records) |record| {
        if (std.mem.eql(u8, record.id, target_id)) {
            placed = true;
            if (replacement) |value| try merged.append(value);
        } else try merged.append(record);
    }
    if (!placed) if (replacement) |value| try merged.append(value);
    return encode_document(allocator, root, merged.items);
}

// Test-only fixtures.
fn sample_records() [2]manifest.record_t {
    return .{
        .{ .id = "desktop:alpha", .kind = .DesktopEntry, .path = "/home/u/.local/share/applications/alpha.desktop", .sha256 = "aa" ** 32, .device = "alpha", .app_key = "alpha-app", .created_by = "0.2.0", .prior = .None, .mime = null },
        .{ .id = "mime:exe", .kind = .MimeDefault, .path = "/home/u/.config/mimeapps.list", .sha256 = "bb" ** 32, .device = null, .app_key = null, .created_by = "0.2.0", .prior = .{ .Text = "old.desktop" }, .mime = "application/x-msi" },
    };
}

test "handler desktop entry renders the frozen keys with quoted Exec and TryExec" {
    const allocator = std.testing.allocator;
    const text = try handler_desktop(allocator, "/opt/waddle/bin/waddle", null);
    defer allocator.free(text);
    try std.testing.expect(std.mem.indexOf(u8, text, "Exec=\"/opt/waddle/bin/waddle\" open %F\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "TryExec=/opt/waddle/bin/waddle\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "NoDisplay=true\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "X-Waddle-Schema=1\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "MimeType=application/x-ms-dos-executable;") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "application/x-ms-installer;\n") != null);
    const with_device = try handler_desktop(allocator, "/opt/waddle/bin/waddle", "alpha");
    defer allocator.free(with_device);
    try std.testing.expect(std.mem.indexOf(u8, with_device, "Exec=\"/opt/waddle/bin/waddle\" open --device alpha %F\n") != null);
}

test "handler Exec quoting escapes percent, quotes, dollar, backtick, and backslash exactly" {
    const allocator = std.testing.allocator;
    const quoted = try exec_quote(allocator, "/a%b\"c$d`e\\f");
    defer allocator.free(quoted);
    const expected =
        \\"/a%%b\\"c\\$d\\`e\\\\f"
    ;
    try std.testing.expectEqualStrings(expected, quoted);
    const text = try handler_desktop(allocator, "/a path/with space", null);
    defer allocator.free(text);
    try std.testing.expect(std.mem.indexOf(u8, text, "Exec=\"/a path/with space\" open %F\n") != null);
    try std.testing.expectError(error.InvalidLauncher, handler_desktop(allocator, "relative", null));
    try std.testing.expectError(error.InvalidLauncher, handler_desktop(allocator, "/bad\nline", null));
    try std.testing.expectError(error.InvalidDevice, handler_desktop(allocator, "/ok", "bad/name"));
    try std.testing.expect(!launcher_valid(""));
    try std.testing.expect(!launcher_valid("/"));
    try std.testing.expect(!launcher_valid("/\xff"));
    try std.testing.expect(!device_valid(""));
    try std.testing.expect(device_valid("alpha-1_B"));
}

test "encoder output parses back through the schema-1 codec with identical fields" {
    const allocator = std.testing.allocator;
    const records = sample_records();
    const bytes = try encode_document(allocator, .{ .waddle_version = WaddleVersion, .updated = "2026-10-25T00:00:00Z" }, &records);
    defer allocator.free(bytes);
    var parsed = try manifest.parse(allocator, bytes);
    defer parsed.deinit();
    try std.testing.expectEqualStrings(WaddleVersion, parsed.waddle_version);
    try std.testing.expectEqual(@as(usize, 2), parsed.records.len);
    try std.testing.expectEqualStrings("desktop:alpha", parsed.records[0].id);
    try std.testing.expectEqualStrings("alpha", parsed.records[0].device.?);
    try std.testing.expectEqualStrings("alpha-app", parsed.records[0].app_key.?);
    try std.testing.expect(parsed.records[0].mime == null);
    try std.testing.expectEqualStrings("application/x-msi", parsed.records[1].mime.?);
    try std.testing.expectEqualStrings("old.desktop", parsed.records[1].prior.Text);
}

test "encoder round-trips every kind and prior shape through the codec" {
    const allocator = std.testing.allocator;
    const kinds = [_]manifest.record_kind_t{ .DesktopEntry, .Icon, .Menu, .DesktopShortcut, .Autostart, .HandlerEntry, .Binfmt, .Polkit };
    var records: [kinds.len]manifest.record_t = undefined;
    const ids = [_][]const u8{ "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7" };
    for (kinds, 0..) |kind, index| {
        records[index] = .{ .id = ids[index], .kind = kind, .path = "/p", .sha256 = "cc" ** 32, .device = null, .app_key = null, .created_by = "0.2.0", .prior = .None, .mime = null };
    }
    const guest = manifest.record_t{ .id = "policy", .kind = .GuestPolicy, .path = "HKLM-Policy", .sha256 = "dd" ** 32, .device = "default", .app_key = null, .created_by = "0.2.0", .prior = .{ .U32Value = 9 }, .mime = null };
    var all: [kinds.len + 1]manifest.record_t = undefined;
    @memcpy(all[0..kinds.len], &records);
    all[kinds.len] = guest;
    const bytes = try encode_document(allocator, .{ .waddle_version = WaddleVersion, .updated = "2026-10-25T00:00:00Z" }, &all);
    defer allocator.free(bytes);
    var parsed = try manifest.parse(allocator, bytes);
    defer parsed.deinit();
    try std.testing.expectEqual(all.len, parsed.records.len);
    for (kinds, 0..) |kind, index| try std.testing.expectEqual(kind, parsed.records[index].kind);
    try std.testing.expectEqual(@as(u32, 9), parsed.records[kinds.len].prior.U32Value);
    const hostile = manifest.record_t{ .id = "quote\"back\\slash\n", .kind = .DesktopEntry, .path = "/x\"y", .sha256 = "ee" ** 32, .device = null, .app_key = null, .created_by = "0.2.0", .prior = .None, .mime = null };
    const escaped = try encode_document(allocator, .{ .waddle_version = WaddleVersion, .updated = "2026-10-25T00:00:00Z" }, &.{hostile});
    defer allocator.free(escaped);
    var reparsed = try manifest.parse(allocator, escaped);
    defer reparsed.deinit();
    try std.testing.expectEqualStrings("quote\"back\\slash\n", reparsed.records[0].id);
}

test "candidate documents change exactly one record and append when absent" {
    const allocator = std.testing.allocator;
    const records = sample_records();
    const root = root_fields_t{ .waddle_version = WaddleVersion, .updated = "2026-10-25T00:00:00Z" };
    const without = try candidate_document(allocator, root, &records, "desktop:alpha", null);
    defer allocator.free(without);
    var removed = try manifest.parse(allocator, without);
    defer removed.deinit();
    try std.testing.expectEqual(@as(usize, 1), removed.records.len);
    try std.testing.expectEqualStrings("mime:exe", removed.records[0].id);

    const added = handler_record("/d/applications/waddle-open.desktop", "ff" ** 32, "alpha");
    const appended = try candidate_document(allocator, root, &records, manifest_handler_id(), added);
    defer allocator.free(appended);
    var grown = try manifest.parse(allocator, appended);
    defer grown.deinit();
    try std.testing.expectEqual(@as(usize, 3), grown.records.len);
    try std.testing.expectEqualStrings(manifest_handler_id(), grown.records[2].id);

    const replaced_value = handler_record("/d/applications/waddle-open.desktop", "01" ** 32, null);
    const replaced = try candidate_document(allocator, root, &records, "desktop:alpha", replaced_value);
    defer allocator.free(replaced);
    var same_count = try manifest.parse(allocator, replaced);
    defer same_count.deinit();
    try std.testing.expectEqual(@as(usize, 2), same_count.records.len);
    try std.testing.expectEqualStrings(manifest_handler_id(), same_count.records[0].id);
}

fn manifest_handler_id() []const u8 {
    return HandlerRecordId;
}

test "allocation failures in producers leave no leaked allocations" {
    const records = sample_records();
    const root = root_fields_t{ .waddle_version = WaddleVersion, .updated = "2026-10-25T00:00:00Z" };
    var index: usize = 0;
    while (index < 128) : (index += 1) {
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = index });
        const allocator = failing.allocator();
        if (handler_desktop(allocator, "/opt/waddle", "alpha")) |text| {
            allocator.free(text);
        } else |err| try std.testing.expectEqual(error.OutOfMemory, err);
        if (encode_document(allocator, root, &records)) |bytes| {
            allocator.free(bytes);
        } else |err| try std.testing.expectEqual(error.OutOfMemory, err);
        if (candidate_document(allocator, root, &records, "desktop:alpha", null)) |bytes| {
            allocator.free(bytes);
        } else |err| try std.testing.expectEqual(error.OutOfMemory, err);
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
}
