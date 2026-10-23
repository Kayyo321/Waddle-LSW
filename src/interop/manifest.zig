//! Bounded, ownership-safe decoder for the frozen integration manifest schema 1.
//! Filesystem authorization, path scope, journaling and serialization belong to
//! later integration layers and are intentionally not implemented here.
const std = @import("std");

/// Maximum number of bytes accepted for one serialized manifest.
pub const MaxManifestBytes = 4 * 1024 * 1024;
/// Maximum number of records accepted by one manifest.
pub const MaxManifestRecords = 4096;
/// Maximum UTF-8 byte length of any JSON string, including object keys.
pub const MaxManifestStringBytes = 4096;
/// Maximum nesting depth accepted by the JSON scanner.
pub const MaxManifestNesting = 8;

/// Frozen schema-1 record kind names from IMPL_DESC.md §3.12.2.
pub const record_kind_t = enum {
    /// Per-device `.desktop` application entry.
    DesktopEntry,
    /// Managed icon file below the hicolor icon tree.
    Icon,
    /// Per-device application menu or desktop-directory file.
    Menu,
    /// Shortcut mirrored to the user's desktop.
    DesktopShortcut,
    /// MIME default entry; the record also requires a MIME type.
    MimeDefault,
    /// User-session autostart entry.
    Autostart,
    /// Shared `waddle-open.desktop` handler entry.
    HandlerEntry,
    /// System binfmt_misc registration file.
    Binfmt,
    /// System polkit action file.
    Polkit,
    /// Guest Windows policy value associated with a device.
    GuestPolicy,
    /// Versioned guest session component installation.
    GuestComponent,
    /// Guest logon Run entry.
    GuestAutostart,
    /// Guest LocalMachine root certificate.
    GuestCertificate,
    /// Guest printer registration.
    GuestPrinter,
};

/// Previous state preserved for restoration by a future journaled owner.
/// `text` and `u32_value` borrow/contain only validated scalar data.
pub const prior_value_t = union(enum) {
    /// No previous value existed or the record kind has no restorable value.
    None,
    /// Previous textual value, borrowed from the owning document.
    Text: []const u8,
    /// Previous DWORD value for a guest policy record.
    U32Value: u32,
};

/// A validated manifest record. All string slices borrow from the owning
/// `document_t` and become invalid when that document is deinitialized.
pub const record_t = struct {
    /// Unique schema-level record identifier.
    id: []const u8,
    /// Frozen host or guest artifact category.
    kind: record_kind_t,
    /// Recorded artifact identity; this codec does not authorize or open it.
    path: []const u8,
    /// SHA-256 digest as 64 hexadecimal UTF-8 bytes.
    sha256: []const u8,
    /// Optional owning device name; required for guest policy records.
    device: ?[]const u8,
    /// Optional application key for app-associated records.
    app_key: ?[]const u8,
    /// Waddle version that created the recorded artifact.
    created_by: []const u8,
    /// Validated restoration value with semantics determined by `kind`.
    prior: prior_value_t,
    /// Required for `mime_default`, forbidden for every other kind.
    mime: ?[]const u8,
};

/// Owned parsed schema-1 document. The arena owns the decoded JSON tree,
/// record array and all strings. Call `deinit` exactly once after the last
/// borrowed field is used. This value is single-owner and is not thread-safe
/// to mutate or deinitialize while another thread reads it.
pub const document_t = struct {
    /// Arena backing all parsed strings and the record slice; owned by this document.
    arena: std.heap.ArenaAllocator,
    /// Waddle version from the schema-1 root, borrowed until `deinit`.
    waddle_version: []const u8,
    /// Update timestamp string from the schema-1 root, borrowed until `deinit`.
    updated: []const u8,
    /// Parsed records, borrowed until `deinit`.
    records: []const record_t,
    /// Internal lifecycle guard used to make repeated `deinit` calls harmless.
    is_live: bool,

    /// Release every allocation owned by this document and invalidate its
    /// borrowed slices. A second call after successful deinitialization is safe.
    /// `self` is an in/out, non-null pointer to the sole owning document. This
    /// method returns no value or error and must not race with borrowed readers.
    pub fn deinit(self: *document_t) void {
        if (!self.is_live) return;
        self.arena.deinit();
        self.is_live = false;
        self.waddle_version = "";
        self.updated = "";
        self.records = &.{};
    }
};

const json_value_t = std.json.Value;

fn object_field(object: std.json.ObjectMap, key: []const u8) ?json_value_t {
    return object.get(key);
}

fn required_field(object: std.json.ObjectMap, key: []const u8) error{InvalidManifest}!json_value_t {
    return object_field(object, key) orelse error.InvalidManifest;
}

fn string_value(value: json_value_t) error{InvalidManifest}![]const u8 {
    return switch (value) {
        .string => |text| text,
        else => error.InvalidManifest,
    };
}

fn valid_text(text: []const u8, allow_empty: bool) bool {
    return text.len <= MaxManifestStringBytes and
        (allow_empty or text.len != 0) and
        std.unicode.utf8ValidateSlice(text) and
        std.mem.indexOfScalar(u8, text, 0) == null;
}

fn checked_text(value: json_value_t, allow_empty: bool) error{InvalidManifest}![]const u8 {
    const text = try string_value(value);
    if (!valid_text(text, allow_empty)) return error.InvalidManifest;
    return text;
}

fn valid_hash(text: []const u8) bool {
    if (text.len != 64) return false;
    for (text) |byte| {
        if (!std.ascii.isHex(byte)) return false;
    }
    return true;
}

fn valid_mime_type(text: []const u8) bool {
    if (text.len < 3 or text.len > 255) return false;
    const slash = std.mem.indexOfScalar(u8, text, '/') orelse return false;
    if (slash == 0 or slash + 1 == text.len or std.mem.indexOfScalarPos(u8, text, slash + 1, '/') != null) return false;
    for (text) |byte| {
        if (!(std.ascii.isAlphanumeric(byte) or byte == '/' or byte == '!' or byte == '#' or
            byte == '$' or byte == '&' or byte == '^' or byte == '_' or byte == '.' or
            byte == '+' or byte == '-')) return false;
    }
    return true;
}

fn allowed_root_field(key: []const u8) bool {
    return std.mem.eql(u8, key, "schema") or std.mem.eql(u8, key, "waddle_version") or
        std.mem.eql(u8, key, "updated") or std.mem.eql(u8, key, "records");
}

fn allowed_record_field(key: []const u8) bool {
    return std.mem.eql(u8, key, "id") or std.mem.eql(u8, key, "kind") or
        std.mem.eql(u8, key, "path") or std.mem.eql(u8, key, "sha256") or
        std.mem.eql(u8, key, "device") or std.mem.eql(u8, key, "app_key") or
        std.mem.eql(u8, key, "created_by") or std.mem.eql(u8, key, "prior") or
        std.mem.eql(u8, key, "mime");
}

fn scan_bounds(allocator: std.mem.Allocator, bytes: []const u8) !void {
    var scanner = std.json.Scanner.initCompleteInput(allocator, bytes);
    defer scanner.deinit();
    var depth: usize = 0;
    while (true) {
        const token = scanner.nextAllocMax(allocator, .alloc_always, MaxManifestStringBytes) catch |err| {
            if (err == error.OutOfMemory) return error.OutOfMemory;
            return error.InvalidManifest;
        };
        switch (token) {
            .object_begin, .array_begin => {
                depth += 1;
                if (depth > MaxManifestNesting) return error.InvalidManifest;
            },
            .object_end, .array_end => {
                if (depth == 0) return error.InvalidManifest;
                depth -= 1;
            },
            .end_of_document => break,
            else => {},
        }
    }
    if (depth != 0) return error.InvalidManifest;
}

fn check_string_bounds(value: json_value_t) error{InvalidManifest}!void {
    switch (value) {
        .string => |text| if (!valid_text(text, true)) return error.InvalidManifest,
        .array => |array| for (array.items) |item| try check_string_bounds(item),
        .object => |object| {
            var iterator = object.iterator();
            while (iterator.next()) |entry| {
                if (!valid_text(entry.key_ptr.*, true)) return error.InvalidManifest;
                try check_string_bounds(entry.value_ptr.*);
            }
        },
        else => {},
    }
}

fn parse_kind(text: []const u8) error{InvalidManifest}!record_kind_t {
    const KindEntries = .{
        .{ "desktop_entry", record_kind_t.DesktopEntry },
        .{ "icon", record_kind_t.Icon },
        .{ "menu", record_kind_t.Menu },
        .{ "desktop_shortcut", record_kind_t.DesktopShortcut },
        .{ "mime_default", record_kind_t.MimeDefault },
        .{ "autostart", record_kind_t.Autostart },
        .{ "handler_entry", record_kind_t.HandlerEntry },
        .{ "binfmt", record_kind_t.Binfmt },
        .{ "polkit", record_kind_t.Polkit },
        .{ "guest_policy", record_kind_t.GuestPolicy },
        .{ "guest_component", record_kind_t.GuestComponent },
        .{ "guest_autostart", record_kind_t.GuestAutostart },
        .{ "guest_certificate", record_kind_t.GuestCertificate },
        .{ "guest_printer", record_kind_t.GuestPrinter },
    };
    inline for (KindEntries) |entry| {
        if (std.mem.eql(u8, text, entry[0])) return entry[1];
    }
    return error.InvalidManifest;
}

fn parse_prior(kind: record_kind_t, value: json_value_t) error{InvalidManifest}!prior_value_t {
    if (value == .null) {
        if (kind == .GuestPolicy or kind == .GuestAutostart) return error.InvalidManifest;
        return .None;
    }
    switch (value) {
        .string => |text| {
            if (!valid_text(text, true)) return error.InvalidManifest;
            switch (kind) {
                .MimeDefault => if (text.len == 0) return error.InvalidManifest,
                .Binfmt => if (!valid_hash(text)) return error.InvalidManifest,
                .GuestPolicy => if (!std.mem.eql(u8, text, "absent")) return error.InvalidManifest,
                .GuestAutostart => {},
                else => return error.InvalidManifest,
            }
            return .{ .Text = text };
        },
        .integer => |integer| {
            if (kind != .GuestPolicy or integer < 0 or integer > std.math.maxInt(u32)) return error.InvalidManifest;
            return .{ .U32Value = @intCast(integer) };
        },
        else => return error.InvalidManifest,
    }
}

fn parse_record(value: json_value_t) !record_t {
    if (value != .object) return error.InvalidManifest;
    const object = value.object;
    var iterator = object.iterator();
    while (iterator.next()) |entry| {
        if (!allowed_record_field(entry.key_ptr.*)) return error.InvalidManifest;
    }

    const id = try checked_text(try required_field(object, "id"), false);
    const kind = try parse_kind(try checked_text(try required_field(object, "kind"), false));
    const path = try checked_text(try required_field(object, "path"), false);
    const sha256 = try checked_text(try required_field(object, "sha256"), false);
    const created_by = try checked_text(try required_field(object, "created_by"), false);
    if (!valid_hash(sha256)) return error.InvalidManifest;
    const prior = try parse_prior(kind, try required_field(object, "prior"));

    const device = if (object_field(object, "device")) |field|
        try checked_text(field, false)
    else
        null;
    const app_key = if (object_field(object, "app_key")) |field|
        try checked_text(field, false)
    else
        null;
    const mime = if (object_field(object, "mime")) |field|
        try checked_text(field, false)
    else
        null;

    if (kind == .GuestPolicy and device == null) return error.InvalidManifest;
    if (kind == .MimeDefault) {
        const mime_text = mime orelse return error.InvalidManifest;
        if (!valid_mime_type(mime_text)) return error.InvalidManifest;
    } else if (mime != null) return error.InvalidManifest;

    return .{
        .id = id,
        .kind = kind,
        .path = path,
        .sha256 = sha256,
        .device = device,
        .app_key = app_key,
        .created_by = created_by,
        .prior = prior,
        .mime = mime,
    };
}

/// Parse a complete schema-1 JSON manifest using the caller allocator as the
/// backing allocator for one owned arena. Input bytes are borrowed only for the
/// duration of this call; every returned string borrows from the returned
/// document. The caller must call `document_t.deinit` exactly once. On any
/// error, all partial allocations are released. Errors distinguish malformed
/// input (`InvalidManifest`), unsupported schema (`UnsupportedSchema`) and
/// allocator failure (`OutOfMemory`). This function performs syntax/schema
/// validation only; it does not authorize paths, inspect files or mutate state.
/// It is safe for concurrent calls with distinct allocators/documents.
///
/// Parameters: `allocator` is an in, non-null allocator value used for every
/// owned allocation; `bytes` is an in, non-null borrowed slice valid only for
/// this call. Returns one owned document on success. No returned slice aliases
/// the input. Thread safety: independent calls are safe; the returned document
/// has single-owner lifetime and requires caller synchronization if shared.
pub fn parse(allocator: std.mem.Allocator, bytes: []const u8) error{ InvalidManifest, UnsupportedSchema, OutOfMemory }!document_t {
    if (bytes.len == 0 or bytes.len > MaxManifestBytes or
        !std.unicode.utf8ValidateSlice(bytes) or std.mem.indexOfScalar(u8, bytes, 0) != null)
        return error.InvalidManifest;

    var arena = std.heap.ArenaAllocator.init(allocator);
    errdefer arena.deinit();
    const arena_allocator = arena.allocator();
    scan_bounds(arena_allocator, bytes) catch |err| {
        if (err == error.OutOfMemory) return error.OutOfMemory;
        return error.InvalidManifest;
    };
    const parsed = std.json.parseFromSliceLeaky(json_value_t, arena_allocator, bytes, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = false,
        .duplicate_field_behavior = .@"error",
    }) catch |err| {
        if (err == error.OutOfMemory) return error.OutOfMemory;
        return error.InvalidManifest;
    };
    try check_string_bounds(parsed);
    if (parsed != .object) return error.InvalidManifest;
    const root = parsed.object;
    var root_iterator = root.iterator();
    while (root_iterator.next()) |entry| {
        if (!allowed_root_field(entry.key_ptr.*)) return error.InvalidManifest;
    }

    const schema = try required_field(root, "schema");
    if (schema != .integer) return error.InvalidManifest;
    if (schema.integer != 1) return error.UnsupportedSchema;
    const waddle_version = try checked_text(try required_field(root, "waddle_version"), false);
    const updated = try checked_text(try required_field(root, "updated"), false);
    const records_value = try required_field(root, "records");
    if (records_value != .array or records_value.array.items.len > MaxManifestRecords)
        return error.InvalidManifest;

    const records = try arena_allocator.alloc(record_t, records_value.array.items.len);
    var seen = std.StringHashMap(void).init(arena_allocator);
    for (records_value.array.items, 0..) |item, index| {
        records[index] = try parse_record(item);
        if (seen.contains(records[index].id)) return error.InvalidManifest;
        try seen.put(records[index].id, {});
    }

    return .{
        .arena = arena,
        .waddle_version = waddle_version,
        .updated = updated,
        .records = records,
        .is_live = true,
    };
}

// Test-only fixtures.
fn kind_to_wire(kind: record_kind_t) []const u8 {
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

test "schema one parses all host and guest record kinds with owned borrowed views" {
    const bytes =
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[" ++
        "{\"id\":\"desktop\",\"kind\":\"desktop_entry\",\"path\":\"/applications/a.desktop\",\"sha256\":\"" ++ "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ++ "\",\"created_by\":\"0.2.0\",\"prior\":null}," ++
        "{\"id\":\"mime\",\"kind\":\"mime_default\",\"path\":\"mimeapps.list\",\"sha256\":\"" ++ "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ++ "\",\"device\":\"default\",\"created_by\":\"0.2.0\",\"prior\":\"old.desktop\",\"mime\":\"application/x-test\"}," ++
        "{\"id\":\"binfmt\",\"kind\":\"binfmt\",\"path\":\"/etc/binfmt.d/waddle.conf\",\"sha256\":\"" ++ "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc" ++ "\",\"created_by\":\"0.2.0\",\"prior\":\"" ++ "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" ++ "\"}," ++
        "{\"id\":\"policy\",\"kind\":\"guest_policy\",\"path\":\"HKLM\\\\Policy\",\"sha256\":\"" ++ "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee" ++ "\",\"device\":\"default\",\"created_by\":\"0.2.0\",\"prior\":3}," ++
        "{\"id\":\"guest-autostart\",\"kind\":\"guest_autostart\",\"path\":\"HKLM\\\\Run\\\\WaddleSession\",\"sha256\":\"" ++ "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" ++ "\",\"created_by\":\"0.2.0\",\"prior\":\"absent\"}" ++
        "]}";
    var document = try parse(std.testing.allocator, bytes);
    defer document.deinit();
    try std.testing.expectEqual(@as(usize, 5), document.records.len);
    try std.testing.expectEqualStrings("0.2.0", document.waddle_version);
    try std.testing.expectEqualStrings("application/x-test", document.records[1].mime.?);
    try std.testing.expectEqual(@as(u32, 3), document.records[3].prior.U32Value);
}

test "all frozen schema one kinds parse with their kind-specific fields" {
    const kinds = [_][]const u8{
        "desktop_entry", "icon",   "menu",         "desktop_shortcut", "mime_default",    "autostart",         "handler_entry",
        "binfmt",        "polkit", "guest_policy", "guest_component",  "guest_autostart", "guest_certificate", "guest_printer",
    };
    var bytes = std.ArrayList(u8).init(std.testing.allocator);
    defer bytes.deinit();
    try bytes.appendSlice("{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[");
    for (kinds, 0..) |kind, index| {
        if (index != 0) try bytes.append(',');
        try bytes.appendSlice("{\"id\":\"");
        try bytes.appendSlice(kind);
        try bytes.appendSlice("\",\"kind\":\"");
        try bytes.appendSlice(kind);
        try bytes.appendSlice("\",\"path\":\"record\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":");
        if (std.mem.eql(u8, kind, "guest_policy")) {
            try bytes.appendSlice("0");
        } else if (std.mem.eql(u8, kind, "guest_autostart")) {
            try bytes.appendSlice("\"absent\"");
        } else {
            try bytes.appendSlice("null");
        }
        if (std.mem.eql(u8, kind, "guest_policy")) try bytes.appendSlice(",\"device\":\"default\"");
        if (std.mem.eql(u8, kind, "mime_default")) try bytes.appendSlice(",\"device\":\"default\",\"mime\":\"application/x-test\"");
        try bytes.append('}');
    }
    try bytes.appendSlice("]}");
    var document = try parse(std.testing.allocator, bytes.items);
    defer document.deinit();
    try std.testing.expectEqual(kinds.len, document.records.len);
    for (kinds, 0..) |kind, index| try std.testing.expectEqualStrings(kind, kind_to_wire(document.records[index].kind));
    try std.testing.expectError(error.InvalidManifest, parse_kind("unknown"));
}

test "rejects malformed schema duplicate keys and duplicate record ids" {
    const invalid = [_][]const u8{
        "{}",
        "{\"schema\":1,\"schema\":1}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":false,\"records\":[]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[{\"id\":\"x\",\"id\":\"x\"}]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[],\"unknown\":1}",
    };
    for (invalid) |bytes| try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, bytes));
    const unsupported_schema = "{\"schema\":2,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[]}";
    try std.testing.expectError(error.UnsupportedSchema, parse(std.testing.allocator, unsupported_schema));
    const duplicate_id =
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[" ++
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"" ++ "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ++ "\",\"created_by\":\"0.2.0\",\"prior\":null}," ++
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"y\",\"sha256\":\"" ++ "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ++ "\",\"created_by\":\"0.2.0\",\"prior\":null}] }";
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, duplicate_id));
}

test "rejects root type omissions and malformed JSON paths" {
    const invalid_roots = [_][]const u8{
        "null",
        "[]",
        "{\"schema\":\"1\",\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[]}",
        "{\"schema\":true,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[]}",
        "{\"schema\":1}",
        "{\"schema\":1,\"updated\":\"now\",\"records\":[]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"records\":[]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\"}",
        "{\"schema\":1,\"waddle_version\":null,\"updated\":\"now\",\"records\":[]}",
        "{\"schema\":1,\"waddle_version\":\"\",\"updated\":\"now\",\"records\":[]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":null,\"records\":[]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"\",\"records\":[]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":null}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":{}}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":\"[]\"}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[],",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[],\"x\":[{},[]]}",
        "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[],\"\\u0000\":\"x\"}",
    };
    for (invalid_roots) |bytes|
        try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, bytes));
}

test "rejects each required record field when missing or mistyped" {
    const invalid_records = [_][]const u8{
        "{\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":1,\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":1,\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":1,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":1,\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":1,\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\"}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":true}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":[]}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":{}}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":0}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"x\",\"unexpected\":0}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":1,\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"app_key\":false,\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"mime\":false,\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":0}",
        "{\"id\":\"x\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"d\",\"created_by\":\"0.2.0\",\"prior\":4294967296}",
        "{\"id\":\"x\",\"kind\":\"guest_autostart\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":1}",
        "{\"id\":\"x\",\"kind\":\"autostart\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"old\"}",
    };
    const prefix = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[";
    for (invalid_records) |record_json| {
        const bytes = try std.fmt.allocPrint(std.testing.allocator, "{s}{s}]}}", .{ prefix, record_json });
        defer std.testing.allocator.free(bytes);
        try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, bytes));
    }
}

test "validates prior variants and optional record values" {
    const prefix = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[";
    const valid_records = [_][]const u8{
        "{\"id\":\"p0\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"d\",\"app_key\":\"app\",\"created_by\":\"0.2.0\",\"prior\":0}",
        "{\"id\":\"pmax\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"d\",\"created_by\":\"0.2.0\",\"prior\":4294967295}",
        "{\"id\":\"pabsent\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"d\",\"created_by\":\"0.2.0\",\"prior\":\"absent\"}",
        "{\"id\":\"autostart\",\"kind\":\"guest_autostart\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"\"}",
        "{\"id\":\"mime-min\",\"kind\":\"mime_default\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"old\",\"mime\":\"a/b\"}",
        "{\"id\":\"binfmt\",\"kind\":\"binfmt\",\"path\":\"x\",\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"created_by\":\"0.2.0\",\"prior\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
        "{\"id\":\"optional\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"d\",\"app_key\":\"app\",\"created_by\":\"0.2.0\",\"prior\":null}",
    };
    var bytes = std.ArrayList(u8).init(std.testing.allocator);
    defer bytes.deinit();
    try bytes.appendSlice(prefix);
    for (valid_records, 0..) |record_json, index| {
        if (index != 0) try bytes.append(',');
        try bytes.appendSlice(record_json);
    }
    try bytes.appendSlice("]}");
    var document = try parse(std.testing.allocator, bytes.items);
    defer document.deinit();
    try std.testing.expectEqual(valid_records.len, document.records.len);
    try std.testing.expectEqual(@as(u32, 0), document.records[0].prior.U32Value);
    try std.testing.expectEqual(@as(u32, std.math.maxInt(u32)), document.records[1].prior.U32Value);
    try std.testing.expectEqualStrings("", document.records[3].prior.Text);
    try std.testing.expectEqualStrings("app", document.records[6].app_key.?);

    const invalid_mime = [_][]const u8{ "", "ab", "/x", "x/", "x/y/z", "x y/z", "x/y?" };
    for (invalid_mime) |mime_text| {
        const invalid = try std.fmt.allocPrint(
            std.testing.allocator,
            "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[{{\"id\":\"x\",\"kind\":\"mime_default\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"old\",\"mime\":\"{s}\"}}]}}",
            .{mime_text},
        );
        defer std.testing.allocator.free(invalid);
        try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, invalid));
    }
    const long_mime = try std.testing.allocator.alloc(u8, 256);
    defer std.testing.allocator.free(long_mime);
    @memset(long_mime, 'a');
    long_mime[127] = '/';
    const invalid_long_mime = try std.fmt.allocPrint(
        std.testing.allocator,
        "{{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[{{\"id\":\"x\",\"kind\":\"mime_default\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"old\",\"mime\":\"{s}\"}}]}}",
        .{long_mime},
    );
    defer std.testing.allocator.free(invalid_long_mime);
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, invalid_long_mime));
}

test "rejects record kind field prior hash UTF8 and string bound violations" {
    const base = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[";
    const suffix = "]}";
    const bad_records = [_][]const u8{
        "{\"id\":\"x\",\"kind\":\"new_kind\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"mime_default\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"bad\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"oops\"}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null,\"mime\":\"application/x-test\"}",
        "{\"id\":\"x\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":-1}",
        "{\"id\":\"x\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"default\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"guest_autostart\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}",
        "{\"id\":\"x\",\"kind\":\"guest_policy\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"default\",\"created_by\":\"0.2.0\",\"prior\":\"other\"}",
        "{\"id\":\"x\",\"kind\":\"binfmt\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":\"not-a-hash\"}",
        "{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg\",\"created_by\":\"0.2.0\",\"prior\":null}",
    };
    for (bad_records) |record_json| {
        const bytes = try std.fmt.allocPrint(std.testing.allocator, "{s}{s}{s}", .{ base, record_json, suffix });
        defer std.testing.allocator.free(bytes);
        try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, bytes));
    }
    const bad_utf8 = "{\"schema\":1,\"waddle_version\":\"\xff\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[]}";
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, bad_utf8));
    const escaped_nul = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[{\"id\":\"x\\u0000y\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}]}";
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, escaped_nul));
}

test "enforces input record string and nesting bounds" {
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, ""));
    const too_long = try std.testing.allocator.alloc(u8, MaxManifestBytes + 1);
    defer std.testing.allocator.free(too_long);
    @memset(too_long, ' ');
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, too_long));
    const too_deep = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[[[[[[[[[[0]]]]]]]]]]]}";
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, too_deep));
    const too_long_text = try std.testing.allocator.alloc(u8, MaxManifestStringBytes + 1);
    defer std.testing.allocator.free(too_long_text);
    @memset(too_long_text, 'x');
    const long_json = try std.fmt.allocPrint(std.testing.allocator, "{{\"schema\":1,\"waddle_version\":\"{s}\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[]}}", .{too_long_text});
    defer std.testing.allocator.free(long_json);
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, long_json));
    var too_many = std.ArrayList(u8).init(std.testing.allocator);
    defer too_many.deinit();
    try too_many.appendSlice("{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[");
    for (0..MaxManifestRecords + 1) |index| {
        if (index != 0) try too_many.append(',');
        try too_many.appendSlice("null");
    }
    try too_many.appendSlice("]}");
    try std.testing.expectError(error.InvalidManifest, parse(std.testing.allocator, too_many.items));
}

fn allocation_probe(allocator: std.mem.Allocator, bytes: []const u8, expected_records: usize) !void {
    var document = try parse(allocator, bytes);
    defer document.deinit();
    try std.testing.expectEqual(expected_records, document.records.len);
}

fn invalid_allocation_probe(allocator: std.mem.Allocator, bytes: []const u8) !void {
    var document = parse(allocator, bytes) catch |err| {
        if (err == error.OutOfMemory) return error.OutOfMemory;
        if (err == error.InvalidManifest) return;
        return err;
    };
    document.deinit();
    return error.ExpectedInvalidManifest;
}

test "valid parse releases every arena allocation" {
    const bytes = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}]}";
    try std.testing.checkAllAllocationFailures(std.testing.allocator, allocation_probe, .{ bytes, 1 });
}

test "document owns strings independently of input and deinit is idempotent" {
    const source = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[{\"id\":\"owned-id\",\"kind\":\"icon\",\"path\":\"owned-path\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}]}";
    const input = try std.testing.allocator.dupe(u8, source);
    var document = try parse(std.testing.allocator, input);
    std.testing.allocator.free(input);
    const churn = try std.testing.allocator.alloc(u8, 1024);
    defer std.testing.allocator.free(churn);
    @memset(churn, 0xA5);
    try std.testing.expectEqualStrings("owned-id", document.records[0].id);
    try std.testing.expectEqualStrings("owned-path", document.records[0].path);
    document.deinit();
    document.deinit();
    try std.testing.expectEqual(@as(usize, 0), document.records.len);
}

test "invalid parse frees partial allocations under allocation failure" {
    const bytes = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-22T01:00:00Z\",\"records\":[{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"x\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null},{\"id\":\"x\",\"kind\":\"icon\",\"path\":\"y\",\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"created_by\":\"0.2.0\",\"prior\":null}]}";
    try std.testing.checkAllAllocationFailures(std.testing.allocator, invalid_allocation_probe, .{bytes});
}

test "allocation failures cover record and duplicate-index growth" {
    var valid = std.ArrayList(u8).init(std.testing.allocator);
    defer valid.deinit();
    var invalid = std.ArrayList(u8).init(std.testing.allocator);
    defer invalid.deinit();
    const prefix = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"now\",\"records\":[";
    try valid.appendSlice(prefix);
    try invalid.appendSlice(prefix);
    for (0..16) |index| {
        if (index != 0) {
            try valid.append(',');
            try invalid.append(',');
        }
        const record = try std.fmt.allocPrint(
            std.testing.allocator,
            "{{\"id\":\"id-{d}\",\"kind\":\"icon\",\"path\":\"path-{d}\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}}",
            .{ index, index },
        );
        defer std.testing.allocator.free(record);
        try valid.appendSlice(record);
        try invalid.appendSlice(record);
    }
    try invalid.append(',');
    try invalid.appendSlice("{\"id\":\"id-0\",\"kind\":\"icon\",\"path\":\"duplicate\",\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"created_by\":\"0.2.0\",\"prior\":null}");
    try valid.appendSlice("]}");
    try invalid.appendSlice("]}");
    try std.testing.checkAllAllocationFailures(std.testing.allocator, allocation_probe, .{ valid.items, 16 });
    try std.testing.checkAllAllocationFailures(std.testing.allocator, invalid_allocation_probe, .{invalid.items});
}
