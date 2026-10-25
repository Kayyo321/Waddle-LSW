//! Bounded producer for frozen host desktop-entry, menu, directory, and icon artifacts.
//! The module creates bytes and cumulative schema-1 manifests only; rooted filesystem
//! authority remains with `manifest_store.store_t.commit()`.
const std = @import("std");
const manifest = @import("manifest.zig");
const manifest_store = @import("manifest_store.zig");

/// Maximum candidate catalog items and resulting manifest records.
pub const MaxItems: usize = 4096;
/// Maximum aggregate borrowed item and metadata input bytes.
pub const MaxInputBytes: usize = 16 * 1024 * 1024;
/// Maximum aggregate returned bytes, including repeated candidate manifests.
pub const MaxOutputBytes: usize = 32 * 1024 * 1024;
/// Maximum one generated artifact, including supplied PNG bytes.
pub const MaxArtifactBytes: usize = 4 * 1024 * 1024;
/// Maximum one candidate manifest.
pub const MaxManifestBytes: usize = 4 * 1024 * 1024;
/// Maximum path or display-name field size in bytes.
pub const MaxTextBytes: usize = 4096;
/// Maximum item icon count; bounded by the seven frozen icon sizes.
pub const MaxIconsPerItem: usize = 7;

/// Maximum encoded basename accepted for a generated file under POSIX NAME_MAX.
pub const MaxPathComponentBytes: usize = 255;

/// Frozen catalog-menu inclusion policy.
pub const menu_filter_t = enum {
    /// Include nonsystem apps and all explicitly seeded apps.
    Installed,
    /// Include every catalog item.
    All,
    /// Hide every catalog item from menus while retaining NoDisplay entries.
    None,
};

/// One borrowed icon PNG supplied by the upstream icon producer.
pub const icon_input_t = struct {
    /// Frozen icon size in pixels; must be one of 16, 24, 32, 48, 64, 128, 256.
    size: u16,
    /// Borrowed PNG bytes, copied unchanged on successful generation.
    png: []const u8,
};

/// One borrowed catalog item. `app_key` is the already validated frozen key.
pub const item_input_t = struct {
    /// Borrowed sanitized catalog app key, 3..200 ASCII bytes.
    app_key: []const u8,
    /// Borrowed catalog display name; controls are replaced with app_key.
    display_name: []const u8,
    /// Whether the catalog marked this item as System.
    system: bool = false,
    /// Whether the catalog marked this item as Seed.
    seed: bool = false,
    /// Whether this item is an uncatalogued window launch. This producer does
    /// not accept these because its frozen Exec form requires a catalog key.
    uncatalogued: bool = false,
    /// Borrowed optional PNG icon observations. Sizes must be unique.
    icons: []const icon_input_t = &.{},
};

/// One artifact and its cumulative manifest snapshot for exactly one store commit.
pub const artifact_t = struct {
    /// Stable schema-1 manifest record ID, owned by `artifacts_t`.
    record_id: []u8,
    /// Absolute canonical manifest path below the configured Data/Config root.
    path: []u8,
    /// Exact artifact bytes; owned by `artifacts_t`.
    bytes: []u8,
    /// SHA-256 of `bytes`, represented as lowercase hexadecimal.
    sha256: [64]u8,
    /// Frozen manifest kind spelling.
    kind: []u8,
    /// Permission mode passed to `manifest_store.store_t.commit()`.
    mode: u9,
    /// Complete schema-1 candidate manifest adding only this artifact to the
    /// prior snapshot. Owned by `artifacts_t`; use with this artifact's commit.
    candidate_manifest: []u8,
};

/// Owned complete output. Call `deinit()` to release every nested allocation.
pub const artifacts_t = struct {
    /// Allocator that owns `items` and all their slices.
    allocator: std.mem.Allocator,
    /// Artifacts in the exact publication order required by the store.
    items: []artifact_t,
    /// Release all owned output and reset to an empty state; safe repeatedly.
    pub fn deinit(self: *artifacts_t) void {
        for (self.items) |item| {
            self.allocator.free(item.record_id);
            self.allocator.free(item.path);
            self.allocator.free(item.bytes);
            self.allocator.free(item.kind);
            self.allocator.free(item.candidate_manifest);
        }
        self.allocator.free(self.items);
        self.items = &.{};
    }
};

/// Borrowed complete generator request. Existing manifest may be null only
/// when no manifest file exists yet. Roots and executable are trusted install
/// layer inputs; this function validates spelling but does not inspect them.
pub const request_t = struct {
    /// Canonical absolute XDG data root spelling, borrowed for the call.
    data_root: []const u8,
    /// Canonical absolute XDG config root spelling, borrowed for the call.
    config_root: []const u8,
    /// Trusted canonical absolute host path to the Waddle launcher.
    launcher_path: []const u8,
    /// Sanitized frozen device identity.
    device: []const u8,
    /// Current schema creator version, borrowed UTF-8 text.
    waddle_version: []const u8,
    /// Deterministic schema timestamp supplied by the caller (UTC RFC3339 subset; seconds 00..59).
    updated: []const u8,
    /// Existing validated schema-1 manifest bytes, or null for a new store.
    existing_manifest: ?[]const u8 = null,
    /// Catalog items in any order; output order is deterministic.
    items: []const item_input_t,
    /// Menu filter applied to every catalog item.
    menu_filter: menu_filter_t = .Installed,
    /// Whether to include the menu and desktop-directory artifacts.
    include_menu: bool = true,
    /// True (default): each candidate is the cumulative snapshot of every earlier artifact, so the
    /// output is quadratic in the artifact count. False: each candidate holds only its own record,
    /// so the output is linear. Non-cumulative candidates are not store-ready on their own; the
    /// caller composes the commit candidate from the base manifest and this record.
    cumulative_candidates: bool = true,
};

const record_view_t = manifest.record_t;
const generated_t = struct { record_id: []const u8, kind: []const u8, path: []const u8, device: ?[]const u8, app_key: ?[]const u8, bytes: []const u8, mode: u9, digest: [64]u8 = undefined };

/// Build deterministic desktop artifacts and one cumulative manifest per
/// artifact. `output` is an in/out non-null pointer initialized with the same
/// allocator and an empty `items` slice; callers must deinitialize a prior
/// result before reuse. On failure `output` remains unchanged. All request
/// slices are borrowed only during the call. The returned object owns all bytes
/// and must be deinitialized by its sole owner. No filesystem I/O occurs.
/// Returns void on success. Errors include InvalidInput, InvalidUtf8,
/// InvalidControl, InvalidExecPath, DuplicateIdentity, DuplicateArtifactPath,
/// InvalidIcon, UnsupportedUncatalogued, TooManyItems, TooMuchInput,
/// OutputTooLarge, ManifestTooLarge, OutputNotEmpty, schema-validation errors
/// from the existing manifest parser, and OutOfMemory. Every error leaves the
/// non-null output object unchanged and frees partial owned buffers.
/// Distinct calls are thread-safe; mutation/deinit of one output requires
/// caller synchronization.
pub fn build(allocator: std.mem.Allocator, request: request_t, output: *artifacts_t) !void {
    if (output.items.len != 0) return error.OutputNotEmpty;
    if (request.items.len > MaxItems) return error.TooManyItems;
    try validate_request(request);
    var existing_doc: ?manifest.document_t = null;
    defer if (existing_doc) |*doc| doc.deinit();
    if (request.existing_manifest) |bytes| {
        existing_doc = try manifest.parse(allocator, bytes);
    }
    const prior_records: []const record_view_t = if (existing_doc) |doc| doc.records else &.{};
    var generated = std.ArrayList(generated_t).init(allocator);
    defer generated.deinit();
    defer for (generated.items) |item| {
        allocator.free(item.record_id);
        allocator.free(item.path);
        allocator.free(item.bytes);
    };
    var output_total: usize = 0;
    var input_total: usize = 0;
    input_total = try add_bounded(input_total, request.data_root.len, MaxInputBytes, error.TooMuchInput);
    input_total = try add_bounded(input_total, request.config_root.len, MaxInputBytes, error.TooMuchInput);
    input_total = try add_bounded(input_total, request.launcher_path.len, MaxInputBytes, error.TooMuchInput);
    input_total = try add_bounded(input_total, request.device.len, MaxInputBytes, error.TooMuchInput);
    input_total = try add_bounded(input_total, request.waddle_version.len, MaxInputBytes, error.TooMuchInput);
    input_total = try add_bounded(input_total, request.updated.len, MaxInputBytes, error.TooMuchInput);
    if (request.existing_manifest) |bytes| input_total = try add_bounded(input_total, bytes.len, MaxInputBytes, error.TooMuchInput);
    for (request.items) |item| {
        input_total = try add_bounded(input_total, item.app_key.len, MaxInputBytes, error.TooMuchInput);
        input_total = try add_bounded(input_total, item.display_name.len, MaxInputBytes, error.TooMuchInput);
        if (item.icons.len > MaxIconsPerItem) return error.InvalidIcon;
        for (item.icons) |icon| input_total = try add_bounded(input_total, icon.png.len, MaxInputBytes, error.TooMuchInput);
    }
    var seen_keys = std.StringHashMap(void).init(allocator);
    defer seen_keys.deinit();
    for (request.items) |item| {
        try validate_item(item, request.device);
        if (seen_keys.contains(item.app_key)) return error.DuplicateIdentity;
        try seen_keys.put(item.app_key, {});
    }
    const sorted_items = try allocator.dupe(item_input_t, request.items);
    defer allocator.free(sorted_items);
    std.mem.sort(item_input_t, sorted_items, {}, item_less_than);

    for (sorted_items) |item| {
        const display = if (has_forbidden_control(item.display_name)) item.app_key else item.display_name;
        const desktop_bytes = try render_desktop(allocator, request, item, display);
        var desktop_bytes_transferred = false;
        errdefer if (!desktop_bytes_transferred) allocator.free(desktop_bytes);
        output_total = try add_bounded(output_total, desktop_bytes.len, MaxOutputBytes, error.OutputTooLarge);
        output_total = try add_bounded(output_total, 64, MaxOutputBytes, error.OutputTooLarge);
        const desktop_id = try std.fmt.allocPrint(allocator, "waddle.{s}.{s}", .{ request.device, item.app_key });
        defer allocator.free(desktop_id);
        const desktop_path = try join_path(allocator, request.data_root, "applications", "", desktop_id, ".desktop");
        var desktop_path_transferred = false;
        errdefer if (!desktop_path_transferred) allocator.free(desktop_path);
        const rec_id = try std.fmt.allocPrint(allocator, "desktop:{s}:{s}", .{ request.device, item.app_key });
        var rec_id_transferred = false;
        errdefer if (!rec_id_transferred) allocator.free(rec_id);
        try generated.append(.{ .record_id = rec_id, .kind = "desktop_entry", .path = desktop_path, .device = request.device, .app_key = item.app_key, .bytes = desktop_bytes, .mode = 0o644 });
        desktop_bytes_transferred = true;
        desktop_path_transferred = true;
        rec_id_transferred = true;
        for (item.icons) |icon| {
            if (!png_signature_valid(icon.png)) return error.InvalidIcon;
            const icon_id = try std.fmt.allocPrint(allocator, "icon:{s}:{s}:{d}", .{ request.device, item.app_key, icon.size });
            var icon_id_transferred = false;
            errdefer if (!icon_id_transferred) allocator.free(icon_id);
            const icon_path = try icon_relative_path(allocator, request.data_root, request.device, item.app_key, icon.size);
            var icon_path_transferred = false;
            errdefer if (!icon_path_transferred) allocator.free(icon_path);
            const icon_bytes = try allocator.dupe(u8, icon.png);
            var icon_bytes_transferred = false;
            errdefer if (!icon_bytes_transferred) allocator.free(icon_bytes);
            output_total = try add_bounded(output_total, icon_bytes.len, MaxOutputBytes, error.OutputTooLarge);
            output_total = try add_bounded(output_total, 64, MaxOutputBytes, error.OutputTooLarge);
            try generated.append(.{ .record_id = icon_id, .kind = "icon", .path = icon_path, .device = request.device, .app_key = item.app_key, .bytes = icon_bytes, .mode = 0o644 });
            icon_id_transferred = true;
            icon_path_transferred = true;
            icon_bytes_transferred = true;
        }
    }
    if (request.include_menu) {
        const menu_bytes = try render_menu(allocator, request, sorted_items);
        var menu_bytes_transferred = false;
        errdefer if (!menu_bytes_transferred) allocator.free(menu_bytes);
        output_total = try add_bounded(output_total, menu_bytes.len, MaxOutputBytes, error.OutputTooLarge);
        output_total = try add_bounded(output_total, 64, MaxOutputBytes, error.OutputTooLarge);
        const menu_path = try join_path(allocator, request.config_root, "menus/applications-merged", "waddle-", request.device, ".menu");
        var menu_path_transferred = false;
        errdefer if (!menu_path_transferred) allocator.free(menu_path);
        const menu_id = try std.fmt.allocPrint(allocator, "menu:{s}:applications-merged", .{request.device});
        var menu_id_transferred = false;
        errdefer if (!menu_id_transferred) allocator.free(menu_id);
        try generated.append(.{ .record_id = menu_id, .kind = "menu", .path = menu_path, .device = request.device, .app_key = null, .bytes = menu_bytes, .mode = 0o644 });
        menu_bytes_transferred = true;
        menu_path_transferred = true;
        menu_id_transferred = true;
        const directory_bytes = try render_directory(allocator, request.device);
        var directory_bytes_transferred = false;
        errdefer if (!directory_bytes_transferred) allocator.free(directory_bytes);
        output_total = try add_bounded(output_total, directory_bytes.len, MaxOutputBytes, error.OutputTooLarge);
        output_total = try add_bounded(output_total, 64, MaxOutputBytes, error.OutputTooLarge);
        const directory_path = try join_path(allocator, request.data_root, "desktop-directories", "waddle-", request.device, ".directory");
        var directory_path_transferred = false;
        errdefer if (!directory_path_transferred) allocator.free(directory_path);
        const directory_id = try std.fmt.allocPrint(allocator, "menu:{s}:directory", .{request.device});
        var directory_id_transferred = false;
        errdefer if (!directory_id_transferred) allocator.free(directory_id);
        try generated.append(.{ .record_id = directory_id, .kind = "menu", .path = directory_path, .device = request.device, .app_key = null, .bytes = directory_bytes, .mode = 0o644 });
        directory_bytes_transferred = true;
        directory_path_transferred = true;
        directory_id_transferred = true;
    }
    if (generated.items.len + prior_records.len > manifest.MaxManifestRecords) return error.TooManyItems;
    try reject_manifest_collisions(prior_records, generated.items);
    std.mem.sort(generated_t, generated.items, {}, generated_less_than);
    // publication order is record-ID sort; every cumulative candidate adds one record.
    var owned_items = try allocator.alloc(artifact_t, generated.items.len);
    var initialized: usize = 0;
    errdefer {
        for (owned_items[0..initialized]) |entry| free_artifact(allocator, entry);
        allocator.free(owned_items);
    }
    var cumulative = std.ArrayList(record_view_t).init(allocator);
    defer cumulative.deinit();
    try cumulative.appendSlice(prior_records);
    for (generated.items, 0..) |*item, index| {
        item.digest = sha256_hex(item.bytes);
        const new_record = record_view_t{ .id = item.record_id, .kind = parse_kind(item.kind), .path = item.path, .sha256 = &item.digest, .device = item.device, .app_key = item.app_key, .created_by = request.waddle_version, .prior = .None, .mime = null };
        var single = [_]record_view_t{new_record};
        const candidate_records: []const record_view_t = if (request.cumulative_candidates) blk: {
            try cumulative.append(new_record);
            break :blk cumulative.items;
        } else &single;
        const candidate = try serialize_manifest(allocator, request, candidate_records);
        var candidate_transferred = false;
        errdefer if (!candidate_transferred) allocator.free(candidate);
        output_total = try add_bounded(output_total, candidate.len, MaxOutputBytes, error.OutputTooLarge);
        output_total = try add_bounded(output_total, item.record_id.len, MaxOutputBytes, error.OutputTooLarge);
        output_total = try add_bounded(output_total, item.path.len, MaxOutputBytes, error.OutputTooLarge);
        output_total = try add_bounded(output_total, item.kind.len, MaxOutputBytes, error.OutputTooLarge);
        const id_copy = try allocator.dupe(u8, item.record_id);
        errdefer allocator.free(id_copy);
        const path_copy = try allocator.dupe(u8, item.path);
        errdefer allocator.free(path_copy);
        const bytes_copy = try allocator.dupe(u8, item.bytes);
        errdefer allocator.free(bytes_copy);
        const kind_copy = try allocator.dupe(u8, item.kind);
        errdefer allocator.free(kind_copy);
        owned_items[index] = .{ .record_id = id_copy, .path = path_copy, .bytes = bytes_copy, .sha256 = item.digest, .kind = kind_copy, .mode = item.mode, .candidate_manifest = candidate };
        candidate_transferred = true;
        initialized += 1;
    }
    const result = artifacts_t{ .allocator = allocator, .items = owned_items };
    output.* = result;
}

fn parse_kind(text: []const u8) manifest.record_kind_t {
    if (std.mem.eql(u8, text, "desktop_entry")) return .DesktopEntry;
    if (std.mem.eql(u8, text, "icon")) return .Icon;
    return .Menu;
}

fn validate_request(request: request_t) !void {
    try validate_utf8(request.device, false, 64);
    if (!valid_identity_component(request.device, 1, 64)) return error.InvalidInput;
    try validate_utf8(request.waddle_version, false, 64);
    try validate_utf8(request.updated, false, 64);
    if (!valid_timestamp(request.updated)) return error.InvalidInput;
    try validate_absolute_path(request.data_root, true);
    try validate_absolute_path(request.config_root, true);
    try validate_absolute_path(request.launcher_path, false);
    if (std.mem.eql(u8, request.data_root, request.config_root)) return error.InvalidInput;
}

fn validate_item(item: item_input_t, device: []const u8) !void {
    if (item.uncatalogued) return error.UnsupportedUncatalogued;
    try validate_utf8(item.app_key, false, 200);
    if (!valid_app_key(item.app_key)) return error.InvalidInput;
    const desktop_name_bytes = "waddle.".len + device.len + 1 + item.app_key.len + ".desktop".len;
    const icon_name_bytes = "waddle.".len + device.len + 1 + item.app_key.len + ".png".len;
    if (desktop_name_bytes > MaxPathComponentBytes or icon_name_bytes > MaxPathComponentBytes) return error.InvalidInput;
    try validate_utf8(item.display_name, false, 255);
    if (item.icons.len > MaxIconsPerItem) return error.InvalidIcon;
    var sizes: [MaxIconsPerItem]u16 = undefined;
    for (item.icons, 0..) |icon, index| {
        if (!valid_icon_size(icon.size) or icon.png.len == 0 or icon.png.len > MaxArtifactBytes) return error.InvalidIcon;
        for (sizes[0..index]) |prior| if (prior == icon.size) return error.InvalidIcon;
        sizes[index] = icon.size;
    }
}

fn validate_utf8(bytes: []const u8, allow_empty: bool, max: usize) !void {
    if (bytes.len > max or (!allow_empty and bytes.len == 0) or !std.unicode.utf8ValidateSlice(bytes) or std.mem.indexOfScalar(u8, bytes, 0) != null) return error.InvalidUtf8;
}

fn validate_absolute_path(path: []const u8, directory: bool) !void {
    try validate_utf8(path, false, MaxTextBytes);
    if (path.len == 1 or path[0] != '/' or path[path.len - 1] == '/' or std.mem.indexOf(u8, path, "//") != null or has_forbidden_control(path) or (!directory and (std.mem.indexOfScalar(u8, path, '=') != null or std.mem.indexOfScalar(u8, path, '%') != null))) return if (directory) error.InvalidInput else error.InvalidExecPath;
    var parts = std.mem.tokenizeScalar(u8, path[1..], '/');
    while (parts.next()) |part| if (std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return if (directory) error.InvalidInput else error.InvalidExecPath;
}

fn valid_timestamp(value: []const u8) bool {
    if (value.len != 20 or value[4] != '-' or value[7] != '-' or value[10] != 'T' or value[13] != ':' or value[16] != ':' or value[19] != 'Z') return false;
    for (value, 0..) |byte, index| {
        if (index == 4 or index == 7 or index == 10 or index == 13 or index == 16 or index == 19) continue;
        if (!std.ascii.isDigit(byte)) return false;
    }
    const year: u16 = @as(u16, parse_decimal(value[0..2])) * 100 + parse_decimal(value[2..4]);
    const month = parse_decimal(value[5..7]);
    const day = parse_decimal(value[8..10]);
    const hour = parse_decimal(value[11..13]);
    const minute = parse_decimal(value[14..16]);
    const second = parse_decimal(value[17..19]);
    if (month < 1 or month > 12 or day < 1 or hour > 23 or minute > 59 or second > 59) return false;
    const month_days = [_]u8{ 31, if (is_leap_year(year)) 29 else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return day <= month_days[month - 1];
}

fn is_leap_year(year: u16) bool {
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0);
}

fn parse_decimal(value: []const u8) u8 {
    return (value[0] - '0') * 10 + (value[1] - '0');
}

fn valid_identity_component(value: []const u8, min: usize, max: usize) bool {
    if (value.len < min or value.len > max) return false;
    for (value) |char| if (!(std.ascii.isAlphanumeric(char) or char == '_' or char == '-')) return false;
    return true;
}

fn valid_app_key(value: []const u8) bool {
    if (value.len < 3 or value.len > 200 or value[1] != '-') return false;
    if (value[0] != 'a' and value[0] != 'p') return false;
    if (value[0] == 'p') {
        if (value.len != 34) return false;
        for (value[2..]) |char| if (!(std.ascii.isDigit(char) or (char >= 'a' and char <= 'f'))) return false;
        return true;
    }
    for (value[2..]) |char| if (!(std.ascii.isAlphanumeric(char) or char == '_' or char == '-')) return false;
    return true;
}

fn item_less_than(_: void, left: item_input_t, right: item_input_t) bool {
    return std.mem.lessThan(u8, left.app_key, right.app_key);
}
fn generated_less_than(_: void, left: generated_t, right: generated_t) bool {
    return std.mem.lessThan(u8, left.record_id, right.record_id);
}
fn valid_icon_size(size: u16) bool {
    return size == 16 or size == 24 or size == 32 or size == 48 or size == 64 or size == 128 or size == 256;
}
fn png_signature_valid(bytes: []const u8) bool {
    return bytes.len >= 8 and std.mem.eql(u8, bytes[0..8], "\x89PNG\r\n\x1a\n");
}
fn has_forbidden_control(bytes: []const u8) bool {
    var index: usize = 0;
    while (index < bytes.len) {
        const first = bytes[index];
        const width: usize = if (first < 0x80) 1 else if (first < 0xe0) 2 else if (first < 0xf0) 3 else 4;
        const codepoint: u32 = switch (width) {
            1 => first,
            2 => (@as(u32, first & 0x1f) << 6) | (bytes[index + 1] & 0x3f),
            3 => (@as(u32, first & 0x0f) << 12) | (@as(u32, bytes[index + 1] & 0x3f) << 6) | (bytes[index + 2] & 0x3f),
            else => (@as(u32, first & 0x07) << 18) | (@as(u32, bytes[index + 1] & 0x3f) << 12) | (@as(u32, bytes[index + 2] & 0x3f) << 6) | (bytes[index + 3] & 0x3f),
        };
        if (codepoint < 0x20 or (codepoint >= 0x7f and codepoint <= 0x9f) or codepoint == 0x2028 or codepoint == 0x2029) return true;
        index += width;
    }
    return false;
}

fn add_bounded(current: usize, amount: usize, limit: usize, comptime err: anytype) !usize {
    const sum = std.math.add(usize, current, amount) catch return err;
    if (sum > limit) return err;
    return sum;
}

fn join_path(allocator: std.mem.Allocator, root: []const u8, middle: []const u8, prefix: []const u8, suffix_input: []const u8, suffix: []const u8) ![]u8 {
    const path = try std.fmt.allocPrint(allocator, "{s}/{s}/{s}{s}{s}", .{ root, middle, prefix, suffix_input, suffix });
    if (path.len > manifest.MaxManifestStringBytes) {
        allocator.free(path);
        return error.InvalidInput;
    }
    return path;
}

fn icon_relative_path(allocator: std.mem.Allocator, root: []const u8, device: []const u8, key: []const u8, size: u16) ![]u8 {
    const path = try std.fmt.allocPrint(allocator, "{s}/icons/hicolor/{d}x{d}/apps/waddle.{s}.{s}.png", .{ root, size, size, device, key });
    if (path.len > manifest.MaxManifestStringBytes) {
        allocator.free(path);
        return error.InvalidInput;
    }
    return path;
}

fn exec_quote(allocator: std.mem.Allocator, arg: []const u8) ![]u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    try out.append('"');
    for (arg) |byte| {
        if (byte == '%') try out.append('%');
        if (byte == '"' or byte == '`' or byte == '$' or byte == '\\') try out.append('\\');
        try out.append(byte);
    }
    try out.append('"');
    const quoted = try out.toOwnedSlice();
    defer allocator.free(quoted);
    return desktop_value(allocator, quoted);
}

fn desktop_value(allocator: std.mem.Allocator, value: []const u8) ![]u8 {
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

fn render_desktop(allocator: std.mem.Allocator, request: request_t, item: item_input_t, display: []const u8) ![]u8 {
    const escaped_name = try desktop_value(allocator, display);
    defer allocator.free(escaped_name);
    const comment = try std.fmt.allocPrint(allocator, "Windows application ({s})", .{request.device});
    defer allocator.free(comment);
    const escaped_comment = try desktop_value(allocator, comment);
    defer allocator.free(escaped_comment);
    const exec_path = try exec_quote(allocator, request.launcher_path);
    defer allocator.free(exec_path);
    const try_exec = try desktop_value(allocator, request.launcher_path);
    defer allocator.free(try_exec);
    const app_id = try std.fmt.allocPrint(allocator, "waddle.{s}.{s}", .{ request.device, item.app_key });
    defer allocator.free(app_id);
    const no_display = !menu_includes(request.menu_filter, item);
    return std.fmt.allocPrint(allocator, "[Desktop Entry]\nType=Application\nVersion=1.5\nName={s}\nComment={s}\nExec={s} \"open\" \"--device\" \"{s}\" \"--app\" \"{s}\" %F\nTryExec={s}\nIcon={s}\nStartupWMClass={s}\nStartupNotify=true\nCategories=X-Waddle;X-Waddle-{s};\nTerminal=false\nNoDisplay={s}\nX-Waddle-Device={s}\nX-Waddle-AppKey={s}\nX-Waddle-Schema=1\n", .{ escaped_name, escaped_comment, exec_path, request.device, item.app_key, try_exec, app_id, app_id, request.device, if (no_display) "true" else "false", request.device, item.app_key });
}

fn menu_includes(filter: menu_filter_t, item: item_input_t) bool {
    return switch (filter) {
        .All => true,
        .None => false,
        .Installed => !item.system or item.seed,
    };
}

fn xml_escape(allocator: std.mem.Allocator, value: []const u8, attribute: bool) ![]u8 {
    if (!valid_xml_text(value) or has_forbidden_control(value)) return error.InvalidControl;
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    for (value) |byte| switch (byte) {
        '&' => try out.appendSlice("&amp;"),
        '<' => try out.appendSlice("&lt;"),
        '>' => try out.appendSlice("&gt;"),
        '"' => if (attribute) try out.appendSlice("&quot;") else try out.append(byte),
        '\'' => if (attribute) try out.appendSlice("&apos;") else try out.append(byte),
        else => try out.append(byte),
    };
    return out.toOwnedSlice();
}

fn valid_xml_text(value: []const u8) bool {
    if (!std.unicode.utf8ValidateSlice(value)) return false;
    var index: usize = 0;
    while (index < value.len) {
        const first = value[index];
        const width: usize = if (first < 0x80) 1 else if (first < 0xe0) 2 else if (first < 0xf0) 3 else 4;
        const codepoint: u32 = switch (width) {
            1 => first,
            2 => (@as(u32, first & 0x1f) << 6) | (value[index + 1] & 0x3f),
            3 => (@as(u32, first & 0x0f) << 12) | (@as(u32, value[index + 1] & 0x3f) << 6) | (value[index + 2] & 0x3f),
            else => (@as(u32, first & 0x07) << 18) | (@as(u32, value[index + 1] & 0x3f) << 12) | (@as(u32, value[index + 2] & 0x3f) << 6) | (value[index + 3] & 0x3f),
        };
        if (!(codepoint == 0x9 or codepoint == 0xa or codepoint == 0xd or
            (codepoint >= 0x20 and codepoint <= 0xd7ff) or
            (codepoint >= 0xe000 and codepoint <= 0xfffd) or
            (codepoint >= 0x10000 and codepoint <= 0x10ffff))) return false;
        index += width;
    }
    return true;
}

fn render_menu(allocator: std.mem.Allocator, request: request_t, items: []const item_input_t) ![]u8 {
    const name = try xml_escape(allocator, request.device, false);
    defer allocator.free(name);
    // One owned allocation contains the complete bounded template; the escaped
    // name is borrowed only while formatting and no intermediate output buffer
    // can outlive this call.
    const output = try std.fmt.allocPrint(allocator, "<!DOCTYPE Menu PUBLIC \"-//freedesktop//DTD Menu 1.0//EN\" \"http://www.freedesktop.org/standards/menu-spec/1.0/menu.dtd\">\n<Menu><Name>Applications</Name><Menu><Name>Windows ({s})</Name><Directory>waddle-{s}.directory</Directory><Include><Category>X-Waddle-{s}</Category></Include></Menu></Menu>\n", .{ name, name, name });
    _ = items;
    return output;
}

fn render_directory(allocator: std.mem.Allocator, device: []const u8) ![]u8 {
    const name = try xml_escape(allocator, device, false);
    defer allocator.free(name);
    return std.fmt.allocPrint(allocator, "[Desktop Entry]\nType=Directory\nName=Windows ({s})\nIcon=waddle.{s}\nX-Waddle-Schema=1\n", .{ name, device });
}

fn reject_manifest_collisions(prior: []const record_view_t, added: []const generated_t) !void {
    for (added, 0..) |candidate, index| {
        for (added[0..index]) |previous| if (std.mem.eql(u8, previous.record_id, candidate.record_id) or std.mem.eql(u8, previous.path, candidate.path)) return error.DuplicateArtifactPath;
        for (prior) |existing| if (std.mem.eql(u8, existing.id, candidate.record_id) or std.mem.eql(u8, existing.path, candidate.path)) return error.DuplicateArtifactPath;
    }
}

fn sha256_hex(bytes: []const u8) [64]u8 {
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
    const Alphabet = "0123456789abcdef";
    var text: [64]u8 = undefined;
    for (digest, 0..) |byte, index| {
        text[index * 2] = Alphabet[byte >> 4];
        text[index * 2 + 1] = Alphabet[byte & 15];
    }
    return text;
}

fn sha256_raw(bytes: []const u8) [32]u8 {
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
    return digest;
}

/// Count pass for the manifest emitter. The same append grammar is used by the
/// bounded fill pass, so encoded-size calculation cannot drift from emission.
/// Arithmetic overflow saturates to `maxInt(usize)` as a rejection sentinel;
/// serialization rejects that count before allocating the output slice.
const manifest_count_sink_t = struct {
    len: usize = 0,
    overflowed: bool = false,

    fn append(self: *@This(), value: u8) error{}!void {
        try self.append_slice(&.{value});
    }

    fn append_slice(self: *@This(), value: []const u8) error{}!void {
        self.len = std.math.add(usize, self.len, value.len) catch {
            self.len = std.math.maxInt(usize);
            self.overflowed = true;
            return;
        };
    }
};

/// Exact-size manifest destination. Every write is bounds-checked by Zig's
/// slice indexing; the preceding shared-emitter count pass proves the slices fit.
const manifest_fill_sink_t = struct {
    bytes: []u8,
    cursor: usize = 0,

    fn append(self: *@This(), value: u8) error{}!void {
        self.bytes[self.cursor] = value;
        self.cursor += 1;
    }

    fn append_slice(self: *@This(), value: []const u8) error{}!void {
        @memcpy(self.bytes[self.cursor..][0..value.len], value);
        self.cursor += value.len;
    }
};

/// Adapter used only by direct JSON golden tests; production uses bounded sinks.
const test_array_sink_t = struct {
    list: *std.ArrayList(u8),

    fn append(self: *@This(), value: u8) !void {
        try self.list.append(value);
    }

    fn append_slice(self: *@This(), value: []const u8) !void {
        try self.list.appendSlice(value);
    }
};

fn json_string(out: anytype, value: []const u8) !void {
    try out.append('"');
    for (value) |byte| switch (byte) {
        '"' => try out.append_slice("\\\""),
        '\\' => try out.append_slice("\\\\"),
        '\n' => try out.append_slice("\\n"),
        '\r' => try out.append_slice("\\r"),
        '\t' => try out.append_slice("\\t"),
        else => if (byte < 0x20) {
            const hex = "0123456789abcdef";
            const escaped = [6]u8{ '\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0x0f] };
            try out.append_slice(&escaped);
        } else try out.append(byte),
    };
    try out.append('"');
}

/// Request and record string slices are borrowed and must remain immutable
/// through both passes; the returned exact-size slice is owned by the caller.
fn serialize_manifest(allocator: std.mem.Allocator, request: request_t, records: []const record_view_t) ![]u8 {
    const sorted = try allocator.dupe(record_view_t, records);
    defer allocator.free(sorted);
    std.mem.sort(record_view_t, sorted, {}, record_less_than);
    var count = manifest_count_sink_t{};
    try emit_manifest(&count, request, sorted);
    if (count.overflowed or count.len > MaxManifestBytes) return error.ManifestTooLarge;
    const output = try allocator.alloc(u8, count.len);
    errdefer allocator.free(output);
    var fill = manifest_fill_sink_t{ .bytes = output };
    try emit_manifest(&fill, request, sorted);
    std.debug.assert(fill.cursor == output.len);
    return output;
}

fn emit_manifest(out: anytype, request: request_t, sorted: []const record_view_t) !void {
    try out.append_slice("{\"schema\":1,\"waddle_version\":");
    try json_string(out, request.waddle_version);
    try out.append_slice(",\"updated\":");
    try json_string(out, request.updated);
    try out.append_slice(",\"records\":[");
    for (sorted, 0..) |record, index| {
        if (index != 0) try out.append(',');
        try out.append_slice("{\"id\":");
        try json_string(out, record.id);
        try out.append_slice(",\"kind\":");
        try json_string(out, kind_name(record.kind));
        try out.append_slice(",\"path\":");
        try json_string(out, record.path);
        try out.append_slice(",\"sha256\":");
        try json_string(out, record.sha256);
        if (record.device) |value| {
            try out.append_slice(",\"device\":");
            try json_string(out, value);
        }
        if (record.app_key) |value| {
            try out.append_slice(",\"app_key\":");
            try json_string(out, value);
        }
        try out.append_slice(",\"created_by\":");
        try json_string(out, record.created_by);
        try out.append_slice(",\"prior\":");
        switch (record.prior) {
            .None => try out.append_slice("null"),
            .Text => |text| try json_string(out, text),
            .U32Value => |value| try append_decimal_u32(out, value),
        }
        if (record.mime) |value| {
            try out.append_slice(",\"mime\":");
            try json_string(out, value);
        }
        try out.append('}');
    }
    try out.append_slice("]}");
}

fn append_decimal_u32(out: anytype, value: u32) !void {
    var digits: [10]u8 = undefined;
    var position = digits.len;
    var remaining = value;
    while (remaining != 0) : (remaining /= 10) {
        position -= 1;
        digits[position] = '0' + @as(u8, @intCast(remaining % 10));
    }
    if (position == digits.len) {
        position -= 1;
        digits[position] = '0';
    }
    try out.append_slice(digits[position..]);
}

fn record_less_than(_: void, left: record_view_t, right: record_view_t) bool {
    return std.mem.lessThan(u8, left.id, right.id);
}
fn kind_name(kind: manifest.record_kind_t) []const u8 {
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

fn free_artifact(allocator: std.mem.Allocator, item: artifact_t) void {
    allocator.free(item.record_id);
    allocator.free(item.path);
    allocator.free(item.bytes);
    allocator.free(item.kind);
    allocator.free(item.candidate_manifest);
}

// Test-only fixtures.
fn basic_request(items: []const item_input_t) request_t {
    return .{ .data_root = "/home/test/.local/share", .config_root = "/home/test/.config", .launcher_path = "/usr/bin/waddle", .device = "default", .waddle_version = "0.2.0", .updated = "2026-10-25T12:00:00Z", .items = items };
}

test "non-cumulative candidates hold exactly their own record and stay linear in size" {
    const items = [_]item_input_t{
        .{ .app_key = "a-Editor", .display_name = "Editor" },
        .{ .app_key = "a-Viewer", .display_name = "Viewer" },
        .{ .app_key = "a-Player", .display_name = "Player" },
    };
    var request = basic_request(&items);
    request.cumulative_candidates = false;
    request.include_menu = false;
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &output);
    defer output.deinit();
    var total: usize = 0;
    for (output.items) |artifact| {
        var parsed = try manifest.parse(std.testing.allocator, artifact.candidate_manifest);
        defer parsed.deinit();
        try std.testing.expectEqual(@as(usize, 1), parsed.records.len);
        try std.testing.expectEqualStrings(artifact.record_id, parsed.records[0].id);
        total += artifact.candidate_manifest.len;
    }
    // Cumulative output for the same request grows quadratically; this one is the sum of single records.
    var cumulative = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    request.cumulative_candidates = true;
    try build(std.testing.allocator, request, &cumulative);
    defer cumulative.deinit();
    var cumulative_total: usize = 0;
    for (cumulative.items) |artifact| cumulative_total += artifact.candidate_manifest.len;
    try std.testing.expect(total < cumulative_total);
}

test "linear candidates publish a menu for a catalog the cumulative cap refuses" {
    var items: [600]item_input_t = undefined;
    var keys: [600][16]u8 = undefined;
    for (&items, 0..) |*item, i| {
        const key = try std.fmt.bufPrint(&keys[i], "a-App{d:0>4}", .{i});
        item.* = .{ .app_key = key, .display_name = "Application" };
    }
    var request = basic_request(&items);
    request.cumulative_candidates = true;
    var refused = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try std.testing.expectError(error.OutputTooLarge, build(std.testing.allocator, request, &refused));
    try std.testing.expectEqual(@as(usize, 0), refused.items.len);
    request.cumulative_candidates = false;
    var linear = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &linear);
    defer linear.deinit();
    try std.testing.expect(linear.items.len >= items.len);
}

test "desktop output and cumulative manifests are deterministic and valid" {
    const items = [_]item_input_t{
        .{ .app_key = "a-Editor", .display_name = "Editor", .system = true },
        .{ .app_key = "p-0123456789abcdef0123456789abcdef", .display_name = "Notepad" },
    };
    const request = basic_request(&items);
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &output);
    defer output.deinit();
    try std.testing.expectEqual(@as(usize, 4), output.items.len);
    try std.testing.expect(std.mem.indexOf(u8, output.items[0].bytes, "Name=Editor\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, output.items[0].bytes, "NoDisplay=true\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, output.items[1].bytes, "NoDisplay=false\n") != null);
    var previous_count: usize = 0;
    for (output.items) |artifact| {
        var parsed = try manifest.parse(std.testing.allocator, artifact.candidate_manifest);
        defer parsed.deinit();
        try std.testing.expectEqual(previous_count + 1, parsed.records.len);
        previous_count = parsed.records.len;
        try std.testing.expectEqualStrings(artifact.record_id, parsed.records[parsed.records.len - 1].id);
    }
    try std.testing.expect(std.mem.indexOf(u8, output.items[2].bytes, "X-Waddle-default") != null);
    // Bind the menu reference to the actual generated directory basename so a
    // spelling mismatch cannot be hidden by two independent expected strings.
    const directory_reference = try std.fmt.allocPrint(std.testing.allocator, "<Directory>{s}</Directory>", .{std.fs.path.basename(output.items[3].path)});
    defer std.testing.allocator.free(directory_reference);
    try std.testing.expect(std.mem.indexOf(u8, output.items[2].bytes, directory_reference) != null);
    try std.testing.expect(std.mem.indexOf(u8, output.items[3].bytes, "Name=Windows (default)") != null);
}

test "Exec quoting separates literal percent and hostile path metacharacters" {
    const item = [_]item_input_t{.{ .app_key = "a-App", .display_name = "日本語 & App" }};
    var request = basic_request(&item);
    request.launcher_path = "/opt/Waddle $bin/\"run\\.exe";
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &output);
    defer output.deinit();
    const bytes = output.items[0].bytes;
    try std.testing.expect(std.mem.indexOf(u8, bytes, "Exec=\x22/opt/Waddle \x5c\x5c$bin/\x5c\x5c\x22run\x5c\x5c\x5c\x5c.exe\x22") != null);
    try std.testing.expect(std.mem.indexOf(u8, bytes, "Name=日本語 & App") != null);
    try std.testing.expect(std.mem.indexOf(u8, bytes, "TryExec=/opt/Waddle $bin/\"run\\\\.exe") != null);
    const quoted_percent = try exec_quote(std.testing.allocator, "/opt/Waddle %F/$bin");
    defer std.testing.allocator.free(quoted_percent);
    try std.testing.expectEqualStrings("\"/opt/Waddle %%F/\\\\$bin\"", quoted_percent);
}

test "GDesktopAppInfo and GLib decode the hostile Exec into exact argv" {
    const item = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App" }};
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const temporary_path = try temporary.dir.realpathAlloc(std.testing.allocator, ".");
    defer std.testing.allocator.free(temporary_path);
    const executable_path = try std.fs.path.join(std.testing.allocator, &.{ temporary_path, "Waddle $bin \"run\\.exe" });
    defer std.testing.allocator.free(executable_path);
    var executable = try temporary.dir.createFile("Waddle $bin \"run\\.exe", .{});
    try executable.chmod(0o700);
    executable.close();
    var request = basic_request(&item);
    request.launcher_path = executable_path;
    var generated = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &generated);
    defer generated.deinit();

    var desktop_file = try temporary.dir.createFile("consumer.desktop", .{});
    try desktop_file.writeAll(generated.items[0].bytes);
    desktop_file.close();
    const desktop_path = try temporary.dir.realpathAlloc(std.testing.allocator, "consumer.desktop");
    defer std.testing.allocator.free(desktop_path);
    const consumer_path = try std.fs.path.join(std.testing.allocator, &.{ temporary_path, "desktop_artifacts_consumer" });
    defer std.testing.allocator.free(consumer_path);

    const pkg_config = try std.process.Child.run(.{ .allocator = std.testing.allocator, .argv = &.{ "pkg-config", "--cflags", "--libs", "gio-unix-2.0" } });
    defer std.testing.allocator.free(pkg_config.stdout);
    defer std.testing.allocator.free(pkg_config.stderr);
    try std.testing.expectEqual(std.process.Child.Term{ .Exited = 0 }, pkg_config.term);
    var compile_args = std.ArrayList([]const u8).init(std.testing.allocator);
    defer compile_args.deinit();
    try compile_args.appendSlice(&.{ "cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-g", "-O1", "-fno-omit-frame-pointer", "-fsanitize=address,leak,undefined", "-no-pie", "tests/interop/desktop_artifacts_consumer.c", "-o", consumer_path });
    var flag_iter = std.mem.tokenizeAny(u8, pkg_config.stdout, " \t\r\n");
    while (flag_iter.next()) |flag| try compile_args.append(flag);
    const compile = try std.process.Child.run(.{ .allocator = std.testing.allocator, .argv = compile_args.items });
    defer std.testing.allocator.free(compile.stdout);
    defer std.testing.allocator.free(compile.stderr);
    if (compile.term != .Exited or compile.term.Exited != 0) std.debug.print("GLib consumer compile failed: {s}\n", .{compile.stderr});
    try std.testing.expectEqual(std.process.Child.Term{ .Exited = 0 }, compile.term);
    var sanitizer_env = try std.process.getEnvMap(std.testing.allocator);
    defer sanitizer_env.deinit();
    try sanitizer_env.put("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1:abort_on_error=1");
    try sanitizer_env.put("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1");
    const consume = try std.process.Child.run(.{ .allocator = std.testing.allocator, .env_map = &sanitizer_env, .argv = &.{ consumer_path, desktop_path, request.launcher_path } });
    defer std.testing.allocator.free(consume.stdout);
    defer std.testing.allocator.free(consume.stderr);
    if (consume.term != .Exited or consume.term.Exited != 0) std.debug.print("GLib consumer failed ({any}): {s}\n", .{ consume.term, consume.stderr });
    try std.testing.expectEqual(std.process.Child.Term{ .Exited = 0 }, consume.term);
}

test "icons use frozen relative target and exact byte hash" {
    const png = "\x89PNG\r\n\x1a\nrest";
    const icons = [_]icon_input_t{.{ .size = 64, .png = png }};
    const items = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App", .icons = &icons }};
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, basic_request(&items), &output);
    defer output.deinit();
    var found = false;
    for (output.items) |artifact| if (std.mem.eql(u8, artifact.kind, "icon")) {
        found = true;
        try std.testing.expectEqualStrings("/home/test/.local/share/icons/hicolor/64x64/apps/waddle.default.a-App.png", artifact.path);
        try std.testing.expectEqualStrings(png, artifact.bytes);
        try std.testing.expectEqualStrings(&sha256_hex(png), &artifact.sha256);
    };
    try std.testing.expect(found);
}

test "malformed identity paths icons and duplicate candidates fail closed" {
    const base = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App" }};
    var request = basic_request(&base);
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    request.launcher_path = "relative/waddle";
    try std.testing.expectError(error.InvalidExecPath, build(std.testing.allocator, request, &output));
    request = basic_request(&base);
    const duplicate = [_]item_input_t{ base[0], base[0] };
    request.items = &duplicate;
    try std.testing.expectError(error.DuplicateIdentity, build(std.testing.allocator, request, &output));
    request = basic_request(&base);
    const bad_key = [_]item_input_t{.{ .app_key = "x-unsafe!", .display_name = "App" }};
    request.items = &bad_key;
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&base);
    const wrong_png = [_]icon_input_t{.{ .size = 32, .png = "not png" }};
    const bad_icon = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App", .icons = &wrong_png }};
    request.items = &bad_icon;
    try std.testing.expectError(error.InvalidIcon, build(std.testing.allocator, request, &output));
    request = basic_request(&base);
    const uncatalogued = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App", .uncatalogued = true }};
    request.items = &uncatalogued;
    try std.testing.expectError(error.UnsupportedUncatalogued, build(std.testing.allocator, request, &output));
    var long_key: [190]u8 = undefined;
    @memcpy(long_key[0..2], "a-");
    @memset(long_key[2..], 'x');
    const oversized_component = [_]item_input_t{.{ .app_key = &long_key, .display_name = "App" }};
    request = basic_request(&oversized_component);
    request.device = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    try std.testing.expectEqual(@as(usize, 0), output.items.len);
}

test "control display name falls back to key and unicode c1 continuation is retained" {
    const items = [_]item_input_t{
        .{ .app_key = "a-Control", .display_name = "bad\nname" },
        .{ .app_key = "a-Unicode", .display_name = "日本語" },
        .{ .app_key = "a-Zsupplementary", .display_name = "window 🪟" },
    };
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, basic_request(&items), &output);
    defer output.deinit();
    try std.testing.expect(std.mem.indexOf(u8, output.items[0].bytes, "Name=a-Control\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, output.items[1].bytes, "Name=日本語\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, output.items[2].bytes, "Name=window 🪟\n") != null);
}

test "all menu filters and optional menu artifacts are explicit" {
    const items = [_]item_input_t{
        .{ .app_key = "a-App", .display_name = "App", .system = true },
        .{ .app_key = "a-Seed", .display_name = "Seed", .system = true, .seed = true },
    };
    var request = basic_request(&items);
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &output);
    defer output.deinit();
    try std.testing.expect(std.mem.indexOf(u8, output.items[0].bytes, "NoDisplay=true") != null);
    try std.testing.expect(std.mem.indexOf(u8, output.items[1].bytes, "NoDisplay=false") != null);
    output.deinit();
    request.menu_filter = .All;
    try build(std.testing.allocator, request, &output);
    try std.testing.expect(std.mem.indexOf(u8, output.items[0].bytes, "NoDisplay=false") != null);
    output.deinit();
    request.menu_filter = .None;
    request.include_menu = false;
    try build(std.testing.allocator, request, &output);
    try std.testing.expectEqual(@as(usize, 2), output.items.len);
    for (output.items) |artifact| try std.testing.expect(std.mem.indexOf(u8, artifact.bytes, "NoDisplay=true") != null);
}

test "value and XML escaping cover reserved characters and reject controls" {
    const value = try desktop_value(std.testing.allocator, "slash\\ newline\n tab\t return\r");
    defer std.testing.allocator.free(value);
    try std.testing.expectEqualStrings("slash\\\\ newline\\n tab\\t return\\r", value);
    const xml_text = try xml_escape(std.testing.allocator, "A&B<C>", false);
    defer std.testing.allocator.free(xml_text);
    try std.testing.expectEqualStrings("A&amp;B&lt;C&gt;", xml_text);
    const xml_attribute = try xml_escape(std.testing.allocator, "\"x'&", true);
    defer std.testing.allocator.free(xml_attribute);
    try std.testing.expectEqualStrings("&quot;x&apos;&amp;", xml_attribute);
    const xml_text_quotes = try xml_escape(std.testing.allocator, "\"'", false);
    defer std.testing.allocator.free(xml_text_quotes);
    try std.testing.expectEqualStrings("\"'", xml_text_quotes);
    try std.testing.expectError(error.InvalidControl, xml_escape(std.testing.allocator, "x\u{0001}", false));
    try std.testing.expectError(error.InvalidControl, xml_escape(std.testing.allocator, "x\u{FFFE}", false));
    try std.testing.expectError(error.InvalidControl, xml_escape(std.testing.allocator, "x\u{FFFF}", true));
    try std.testing.expect(has_forbidden_control("\u{0085}"));
    try std.testing.expect(has_forbidden_control("\u{2028}"));
    try std.testing.expect(!has_forbidden_control("日本語"));
}

test "input guards reject timestamp, roots, key forms, NUL, invalid UTF-8 and control path" {
    const items = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App" }};
    var request = basic_request(&items);
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    request.updated = "not-a-timestamp";
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.data_root = "/home//test/data";
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.config_root = request.data_root;
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.launcher_path = "/usr/bin/waddle\tunsafe";
    try std.testing.expectError(error.InvalidExecPath, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.launcher_path = "/usr/bin/waddle=unsafe";
    try std.testing.expectError(error.InvalidExecPath, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.launcher_path = "/usr/bin/waddle%F";
    try std.testing.expectError(error.InvalidExecPath, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    const nul_key = [_]item_input_t{.{ .app_key = "a-App\x00", .display_name = "App" }};
    request.items = &nul_key;
    try std.testing.expectError(error.InvalidUtf8, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    const invalid_utf8_name = [_]item_input_t{.{ .app_key = "a-App", .display_name = "\xff" }};
    request.items = &invalid_utf8_name;
    try std.testing.expectError(error.InvalidUtf8, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    const invalid_path_key = [_]item_input_t{.{ .app_key = "p-ABCDEF0123456789ABCDEF0123456789", .display_name = "App" }};
    request.items = &invalid_path_key;
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    const nonhex_path_key = [_]item_input_t{.{ .app_key = "p-0123456789abcdef0123456789abcdeg", .display_name = "App" }};
    request.items = &nonhex_path_key;
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.device = "bad.device";
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.updated = "2026-13-01T25:61:61Z";
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.data_root = "/";
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.launcher_path = "/usr/bin/../bin/waddle";
    try std.testing.expectError(error.InvalidExecPath, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.config_root = "/home/test/.config/";
    try std.testing.expectError(error.InvalidInput, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.launcher_path = "/usr/bin/./waddle";
    try std.testing.expectError(error.InvalidExecPath, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.launcher_path = "/usr/bin//waddle";
    try std.testing.expectError(error.InvalidExecPath, build(std.testing.allocator, request, &output));
}

test "timestamp calendar days and Gregorian leap years are validated" {
    try std.testing.expect(valid_timestamp("2024-02-29T12:00:00Z"));
    try std.testing.expect(valid_timestamp("2000-02-29T12:00:00Z"));
    try std.testing.expect(valid_timestamp("2026-01-31T12:00:00Z"));
    try std.testing.expect(!valid_timestamp("2026-02-29T12:00:00Z"));
    try std.testing.expect(!valid_timestamp("1900-02-29T12:00:00Z"));
    try std.testing.expect(!valid_timestamp("2026-02-31T12:00:00Z"));
    try std.testing.expect(!valid_timestamp("2026-04-31T12:00:00Z"));
    try std.testing.expect(!valid_timestamp("2026-06-00T12:00:00Z"));
    try std.testing.expect(!valid_timestamp("2026-13-01T12:00:00Z"));
    try std.testing.expect(!valid_timestamp("2026-10-25T12:00:60Z"));
    try std.testing.expect(valid_timestamp("2026-10-25T23:59:59Z"));
}

test "timestamp separators and digit fields are checked independently" {
    const valid = "2026-10-25T12:00:00Z";
    try std.testing.expect(valid_timestamp(valid));
    try std.testing.expect(!valid_timestamp("2026-10-25T12:00:00"));
    for ([_]usize{ 4, 7, 10, 13, 16, 19 }) |index| {
        var invalid = valid.*;
        invalid[index] = 'x';
        try std.testing.expect(!valid_timestamp(&invalid));
    }
    var invalid_digit = valid.*;
    invalid_digit[0] = 'x';
    try std.testing.expect(!valid_timestamp(&invalid_digit));
}

test "identity and application key bounds reject each malformed form" {
    try std.testing.expect(!valid_identity_component("", 1, 64));
    try std.testing.expect(!valid_identity_component("too-long", 1, 4));
    try std.testing.expect(!valid_identity_component("bad.device", 1, 64));
    try std.testing.expect(valid_identity_component("device_1", 1, 64));

    try std.testing.expect(!valid_app_key("a-"));
    try std.testing.expect(!valid_app_key("a" ** 201));
    try std.testing.expect(!valid_app_key("aApp"));
    try std.testing.expect(!valid_app_key("x-App"));
    try std.testing.expect(!valid_app_key("p-short"));
    try std.testing.expect(!valid_app_key("a-Bad!"));
    try std.testing.expect(valid_app_key("a-Good_Key-1"));
    try std.testing.expect(valid_app_key("p-0123456789abcdef0123456789abcdef"));
}

test "manifest path helpers reject oversized generated paths and bounds overflow" {
    const long_root = try std.testing.allocator.alloc(u8, MaxTextBytes);
    defer std.testing.allocator.free(long_root);
    long_root[0] = '/';
    @memset(long_root[1..], 'r');
    try std.testing.expectError(error.InvalidInput, join_path(std.testing.allocator, long_root, "applications", "", "entry", ".desktop"));
    try std.testing.expectError(error.InvalidInput, icon_relative_path(std.testing.allocator, long_root, "device", "a-App", 64));
    try std.testing.expectError(error.TooMuchInput, add_bounded(std.math.maxInt(usize), 1, std.math.maxInt(usize), error.TooMuchInput));
    try std.testing.expectError(error.TooMuchInput, add_bounded(10, 1, 10, error.TooMuchInput));
}

test "icon bounds size duplicates signature and input bounds are checked" {
    const items = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App" }};
    var request = basic_request(&items);
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    const unsupported = [_]icon_input_t{.{ .size = 20, .png = "\x89PNG\r\n\x1a\n" }};
    const unsupported_item = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App", .icons = &unsupported }};
    request.items = &unsupported_item;
    try std.testing.expectError(error.InvalidIcon, build(std.testing.allocator, request, &output));
    const duplicate_icons = [_]icon_input_t{ .{ .size = 16, .png = "\x89PNG\r\n\x1a\n" }, .{ .size = 16, .png = "\x89PNG\r\n\x1a\n" } };
    const duplicate_item = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App", .icons = &duplicate_icons }};
    request.items = &duplicate_item;
    try std.testing.expectError(error.InvalidIcon, build(std.testing.allocator, request, &output));
    const too_many_icons = [_]icon_input_t{.{ .size = 16, .png = "\x89PNG\r\n\x1a\n" }} ** 8;
    const too_many_item = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App", .icons = &too_many_icons }};
    request.items = &too_many_item;
    try std.testing.expectError(error.InvalidIcon, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    request.launcher_path = "x" ** (MaxTextBytes + 1);
    try std.testing.expectError(error.InvalidUtf8, build(std.testing.allocator, request, &output));
    request = basic_request(&items);
    const too_many_items = [_]item_input_t{items[0]} ** (MaxItems + 1);
    request.items = &too_many_items;
    try std.testing.expectError(error.TooManyItems, build(std.testing.allocator, request, &output));
}

test "output remains unchanged on manifest collision and invalid existing manifest" {
    const items = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App" }};
    const existing = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T11:00:00Z\",\"records\":[{\"id\":\"desktop:default:a-App\",\"kind\":\"desktop_entry\",\"path\":\"/home/test/.local/share/applications/waddle.default.a-App.desktop\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_by\":\"0.2.0\",\"prior\":null}]}";
    var request = basic_request(&items);
    request.existing_manifest = existing;
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try std.testing.expectError(error.DuplicateArtifactPath, build(std.testing.allocator, request, &output));
    try std.testing.expectEqual(@as(usize, 0), output.items.len);
    request.existing_manifest = "not json";
    try std.testing.expectError(error.InvalidManifest, build(std.testing.allocator, request, &output));
}

test "nonempty output requires explicit deinitialization before reuse" {
    const items = [_]item_input_t{.{ .app_key = "a-App", .display_name = "App" }};
    const request = basic_request(&items);
    var output = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &output);
    const original_ptr = output.items.ptr;
    const original_count = output.items.len;
    try std.testing.expectError(error.OutputNotEmpty, build(std.testing.allocator, request, &output));
    try std.testing.expectEqual(original_ptr, output.items.ptr);
    try std.testing.expectEqual(original_count, output.items.len);
    output.deinit();
}

test "manifest serialization escapes JSON strings and preserves prior variants" {
    var encoded = std.ArrayList(u8).init(std.testing.allocator);
    defer encoded.deinit();
    var sink = test_array_sink_t{ .list = &encoded };
    try json_string(&sink, "quote\" slash\\ newline\n return\r tab\t low\x01");
    try std.testing.expectEqualStrings("\"quote\\\" slash\\\\ newline\\n return\\r tab\\t low\\u0001\"", encoded.items);

    const rows = "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T11:00:00Z\",\"records\":[" ++
        "{\"id\":\"mime\",\"kind\":\"mime_default\",\"path\":\"mimeapps.list\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"device\":\"default\",\"created_by\":\"0.2.0\",\"prior\":\"old.desktop\",\"mime\":\"application/x-test\"}," ++
        "{\"id\":\"policy\",\"kind\":\"guest_policy\",\"path\":\"HKLM\\\\Policy\",\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"device\":\"default\",\"created_by\":\"0.2.0\",\"prior\":42}]}";
    var parsed = try manifest.parse(std.testing.allocator, rows);
    defer parsed.deinit();
    const request = basic_request(&.{});
    const encoded_manifest = try serialize_manifest(std.testing.allocator, request, parsed.records);
    defer std.testing.allocator.free(encoded_manifest);
    var reparsed = try manifest.parse(std.testing.allocator, encoded_manifest);
    defer reparsed.deinit();
    try std.testing.expectEqual(@as(usize, 2), reparsed.records.len);
    try std.testing.expectEqualStrings("old.desktop", reparsed.records[0].prior.Text);
    try std.testing.expectEqual(@as(u32, 42), reparsed.records[1].prior.U32Value);
    try std.testing.expectEqualStrings("application/x-test", reparsed.records[0].mime.?);
    try std.testing.expect(std.mem.indexOf(u8, encoded_manifest, "HKLM\\\\Policy") != null);
}

test "manifest serializer covers absent and present optional fields and prior variants" {
    const hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const records = [_]record_view_t{
        .{ .id = "desktop", .kind = .DesktopEntry, .path = "/data/app.desktop", .sha256 = hash, .device = null, .app_key = null, .created_by = "1.0", .prior = .None, .mime = null },
        .{ .id = "mime", .kind = .MimeDefault, .path = "/config/mimeapps.list", .sha256 = hash, .device = "device-1", .app_key = "a-App", .created_by = "1.0", .prior = .{ .Text = "old\"\\path\n" }, .mime = "application/x-test" },
        .{ .id = "policy", .kind = .GuestPolicy, .path = "HKLM\\Policy", .sha256 = hash, .device = "device-1", .app_key = null, .created_by = "1.0", .prior = .{ .U32Value = 42 }, .mime = null },
    };
    const encoded = try serialize_manifest(std.testing.allocator, basic_request(&.{}), &records);
    defer std.testing.allocator.free(encoded);
    try std.testing.expect(std.mem.indexOf(u8, encoded, "\"id\":\"desktop\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, encoded, "\"prior\":null") != null);
    try std.testing.expect(std.mem.indexOf(u8, encoded, "\"prior\":\"old\\\"\\\\path\\n\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, encoded, "\"prior\":42") != null);
    try std.testing.expect(std.mem.indexOf(u8, encoded, "\"mime\":\"application/x-test\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, encoded, "\"app_key\":\"a-App\"") != null);
}

test "escaping helpers cover every desktop and JSON byte escape" {
    const value = try desktop_value(std.testing.allocator, "\\\n\t\rplain");
    defer std.testing.allocator.free(value);
    try std.testing.expectEqualStrings("\\\\\\n\\t\\rplain", value);
    const json_input = [_]u8{ '"', '\\', '\n', '\r', '\t', 1, 'x' };
    var encoded = std.ArrayList(u8).init(std.testing.allocator);
    defer encoded.deinit();
    var sink = test_array_sink_t{ .list = &encoded };
    try json_string(&sink, &json_input);
    try std.testing.expectEqualStrings("\"\\\"\\\\\\n\\r\\t\\u0001x\"", encoded.items);
    const supplementary = try xml_escape(std.testing.allocator, "window 🪟", false);
    defer std.testing.allocator.free(supplementary);
    try std.testing.expectEqualStrings("window 🪟", supplementary);
    try std.testing.expectError(error.InvalidControl, xml_escape(std.testing.allocator, "\xff", false));
}

test "two-pass serializer handles every JSON control and integer boundary" {
    var controls: [32]u8 = undefined;
    for (&controls, 0..) |*byte, index| byte.* = @intCast(index);
    var encoded_controls = std.ArrayList(u8).init(std.testing.allocator);
    defer encoded_controls.deinit();
    var control_sink = test_array_sink_t{ .list = &encoded_controls };
    try json_string(&control_sink, &controls);
    var expected: [32 * 6 + 2]u8 = undefined;
    var expected_len: usize = 1;
    expected[0] = '"';
    for (controls) |byte| {
        switch (byte) {
            '"' => {
                @memcpy(expected[expected_len..][0..2], "\\\"");
                expected_len += 2;
            },
            '\\' => {
                @memcpy(expected[expected_len..][0..2], "\\\\");
                expected_len += 2;
            },
            '\n' => {
                @memcpy(expected[expected_len..][0..2], "\\n");
                expected_len += 2;
            },
            '\r' => {
                @memcpy(expected[expected_len..][0..2], "\\r");
                expected_len += 2;
            },
            '\t' => {
                @memcpy(expected[expected_len..][0..2], "\\t");
                expected_len += 2;
            },
            else => {
                const hex = "0123456789abcdef";
                const escaped_byte = [6]u8{ '\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0x0f] };
                @memcpy(expected[expected_len..][0..6], &escaped_byte);
                expected_len += 6;
            },
        }
    }
    expected[expected_len] = '"';
    expected_len += 1;
    try std.testing.expectEqualSlices(u8, expected[0..expected_len], encoded_controls.items);

    const hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const numeric_records = [_]record_view_t{
        .{ .id = "zero", .kind = .GuestPolicy, .path = "HKLM\\Zero", .sha256 = hash, .device = null, .app_key = null, .created_by = "1.0", .prior = .{ .U32Value = 0 }, .mime = null },
        .{ .id = "max", .kind = .GuestPolicy, .path = "HKLM\\Max", .sha256 = hash, .device = null, .app_key = null, .created_by = "1.0", .prior = .{ .U32Value = std.math.maxInt(u32) }, .mime = null },
    };
    const numeric_json = try serialize_manifest(std.testing.allocator, basic_request(&.{}), &numeric_records);
    defer std.testing.allocator.free(numeric_json);
    try std.testing.expect(std.mem.indexOf(u8, numeric_json, "\"prior\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, numeric_json, "\"prior\":4294967295") != null);
}

test "manifest count sink tracks exact cap and saturates arithmetic overflow" {
    const exact = try std.testing.allocator.alloc(u8, MaxManifestBytes);
    defer std.testing.allocator.free(exact);
    var sink = manifest_count_sink_t{};
    try sink.append_slice(exact);
    try std.testing.expectEqual(MaxManifestBytes, sink.len);
    try sink.append('x');
    try std.testing.expectEqual(MaxManifestBytes + 1, sink.len);
    try std.testing.expect(sink.len > MaxManifestBytes);
    var overflow = manifest_count_sink_t{ .len = std.math.maxInt(usize) };
    try overflow.append('x');
    try std.testing.expect(overflow.overflowed);
}

test "manifest serializer rejects oversized fields at every encoded section" {
    const oversized = try std.testing.allocator.alloc(u8, MaxManifestBytes + 1);
    defer std.testing.allocator.free(oversized);
    @memset(oversized, 'x');
    const hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    var request = basic_request(&.{});
    request.waddle_version = oversized;
    try std.testing.expectError(error.ManifestTooLarge, serialize_manifest(std.testing.allocator, request, &.{}));
    request = basic_request(&.{});
    request.updated = oversized;
    try std.testing.expectError(error.ManifestTooLarge, serialize_manifest(std.testing.allocator, request, &.{}));

    var record = record_view_t{
        .id = "id",
        .kind = .DesktopEntry,
        .path = "/data/app.desktop",
        .sha256 = hash,
        .device = null,
        .app_key = null,
        .created_by = "1.0",
        .prior = .None,
        .mime = null,
    };
    inline for (.{ "id", "path", "sha256", "device", "app_key", "created_by", "prior", "mime" }) |field| {
        record = .{
            .id = "id",
            .kind = .DesktopEntry,
            .path = "/data/app.desktop",
            .sha256 = hash,
            .device = null,
            .app_key = null,
            .created_by = "1.0",
            .prior = .None,
            .mime = null,
        };
        if (comptime std.mem.eql(u8, field, "id")) record.id = oversized;
        if (comptime std.mem.eql(u8, field, "path")) record.path = oversized;
        if (comptime std.mem.eql(u8, field, "sha256")) record.sha256 = oversized;
        if (comptime std.mem.eql(u8, field, "device")) record.device = oversized;
        if (comptime std.mem.eql(u8, field, "app_key")) record.app_key = oversized;
        if (comptime std.mem.eql(u8, field, "created_by")) record.created_by = oversized;
        if (comptime std.mem.eql(u8, field, "prior")) record.prior = .{ .Text = oversized };
        if (comptime std.mem.eql(u8, field, "mime")) record.mime = oversized;
        try std.testing.expectError(error.ManifestTooLarge, serialize_manifest(std.testing.allocator, basic_request(&.{}), &.{record}));
    }
}

test "allocator failure at every reached allocation index is leak free" {
    const icons = [_]icon_input_t{.{ .size = 32, .png = "\x89PNG\r\n\x1a\nbytes" }};
    const items = [_]item_input_t{.{ .app_key = "a-Fail", .display_name = "Failure test", .icons = &icons }};
    const request = basic_request(&items);
    var reached_success = false;
    var fail_index: usize = 0;
    while (fail_index < 512) : (fail_index += 1) {
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = fail_index });
        var output = artifacts_t{ .allocator = failing.allocator(), .items = &.{} };
        build(failing.allocator(), request, &output) catch |err| {
            try std.testing.expectEqual(error.OutOfMemory, err);
            try std.testing.expectEqual(@as(usize, 0), output.items.len);
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            continue;
        };
        output.deinit();
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        reached_success = true;
        break;
    }
    try std.testing.expect(reached_success);
}

test "small renderers unwind every allocator failure point" {
    try allocation_sweep(fault_exec_quote);
    try allocation_sweep(fault_desktop_value);
    try allocation_sweep(fault_xml_text);
    try allocation_sweep(fault_render_menu);
    try allocation_sweep(fault_render_menu_special);
    try allocation_sweep(fault_render_directory);
    try allocation_sweep(fault_serialize_manifest);
}

test "varying renderer and manifest sizes fail cleanly at growth boundaries" {
    var device: [256]u8 = undefined;
    @memset(&device, 'd');
    for (1..65) |length| {
        try allocation_sweep_menu(device[0..length]);
    }

    var escaped: [4096]u8 = undefined;
    @memset(&escaped, 1);
    for (0..escaped.len + 1) |length| {
        try allocation_sweep_manifest(escaped[0..length]);
    }
}

fn allocation_sweep_menu(device: []const u8) !void {
    var request = basic_request(&.{});
    request.device = device;
    var succeeded = false;
    var fail_index: usize = 0;
    while (fail_index < 128) : (fail_index += 1) {
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = fail_index });
        const result = render_menu(failing.allocator(), request, &.{}) catch |err| {
            try std.testing.expectEqual(error.OutOfMemory, err);
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            continue;
        };
        failing.allocator().free(result);
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        succeeded = true;
        break;
    }
    try std.testing.expect(succeeded);
}

fn allocation_sweep_manifest(prior: []const u8) !void {
    const record = record_view_t{
        .id = "desktop",
        .kind = .DesktopEntry,
        .path = "/data/app.desktop",
        .sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .device = "device",
        .app_key = "a-App",
        .created_by = "1.0",
        .prior = .{ .Text = prior },
        .mime = "application/x-test",
    };
    var succeeded = false;
    var fail_index: usize = 0;
    while (fail_index < 128) : (fail_index += 1) {
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = fail_index });
        const result = serialize_manifest(failing.allocator(), basic_request(&.{}), &.{record}) catch |err| {
            try std.testing.expectEqual(error.OutOfMemory, err);
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            continue;
        };
        failing.allocator().free(result);
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        succeeded = true;
        break;
    }
    try std.testing.expect(succeeded);
}

fn allocation_sweep(comptime transform: anytype) !void {
    var succeeded = false;
    var fail_index: usize = 0;
    while (fail_index < 128) : (fail_index += 1) {
        var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = fail_index });
        const result = transform(failing.allocator()) catch |err| {
            try std.testing.expectEqual(error.OutOfMemory, err);
            try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            continue;
        };
        failing.allocator().free(result);
        try std.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        succeeded = true;
        break;
    }
    try std.testing.expect(succeeded);
}

fn fault_exec_quote(allocator: std.mem.Allocator) ![]u8 {
    return exec_quote(allocator, "a long path %F / with $ and \\\" quotes");
}
fn fault_desktop_value(allocator: std.mem.Allocator) ![]u8 {
    return desktop_value(allocator, "a long value \\\\ with\t escapes\n and more bytes");
}
fn fault_xml_text(allocator: std.mem.Allocator) ![]u8 {
    return xml_escape(allocator, "<menu & item> \\\\ text", false);
}
fn fault_render_menu(allocator: std.mem.Allocator) ![]u8 {
    return render_menu(allocator, basic_request(&.{}), &.{});
}
fn fault_render_menu_special(allocator: std.mem.Allocator) ![]u8 {
    var request = basic_request(&.{});
    request.device = "menu-device";
    return render_menu(allocator, request, &.{});
}
fn fault_render_directory(allocator: std.mem.Allocator) ![]u8 {
    return render_directory(allocator, "device");
}
fn fault_serialize_manifest(allocator: std.mem.Allocator) ![]u8 {
    const hash = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const records = [_]record_view_t{
        .{ .id = "a", .kind = .DesktopEntry, .path = "/data/a.desktop", .sha256 = hash, .device = null, .app_key = null, .created_by = "1.0", .prior = .None, .mime = null },
        .{ .id = "b", .kind = .MimeDefault, .path = "/config/mimeapps.list", .sha256 = hash, .device = "device", .app_key = "a-App", .created_by = "1.0", .prior = .{ .Text = "previous\nvalue" }, .mime = "application/x-test" },
        .{ .id = "c", .kind = .GuestPolicy, .path = "HKLM\\Policy", .sha256 = hash, .device = "device", .app_key = null, .created_by = "1.0", .prior = .{ .U32Value = 7 }, .mime = null },
    };
    return serialize_manifest(allocator, basic_request(&.{}), &records);
}

test "artifact candidates publish and recover through rooted manifest store" {
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    try temporary.dir.makeDir("data");
    try temporary.dir.makeDir("config");
    try temporary.dir.makeDir("state");
    var data_dir = try temporary.dir.openDir("data", .{});
    defer data_dir.close();
    var config_dir = try temporary.dir.openDir("config", .{});
    defer config_dir.close();
    var state_dir = try temporary.dir.openDir("state", .{ .iterate = true });
    defer state_dir.close();
    try state_dir.chmod(0o700);
    try data_dir.makePath("applications");
    try data_dir.makePath("desktop-directories");
    try config_dir.makePath("menus/applications-merged");
    const data_path = try temporary.dir.realpathAlloc(std.testing.allocator, "data");
    defer std.testing.allocator.free(data_path);
    const config_path = try temporary.dir.realpathAlloc(std.testing.allocator, "config");
    defer std.testing.allocator.free(config_path);
    const app = [_]item_input_t{.{ .app_key = "a-Editor", .display_name = "Editor" }};
    const request = request_t{ .data_root = data_path, .config_root = config_path, .launcher_path = "/usr/bin/waddle", .device = "test-device", .waddle_version = "0.2.0", .updated = "2026-10-25T12:00:00Z", .items = &app };
    var generated = artifacts_t{ .allocator = std.testing.allocator, .items = &.{} };
    try build(std.testing.allocator, request, &generated);
    defer generated.deinit();
    var store = try manifest_store.store_t.init(std.testing.allocator, state_dir.fd, &.{
        .{ .kind = .Data, .canonical_path = data_path, .dir_fd = data_dir.fd },
        .{ .kind = .Config, .canonical_path = config_path, .dir_fd = config_dir.fd },
    });
    defer store.deinit();
    for (generated.items, 0..) |artifact, index| {
        const commit_request = manifest_store.request_t{ .record_id = artifact.record_id, .prior = .Absent, .artifact_bytes = artifact.bytes, .artifact_mode = artifact.mode, .candidate_manifest = artifact.candidate_manifest };
        // Pin the actual durable phase and published bytes; a pre-staging
        // failure alone does not qualify journal rollback or roll-forward.
        const prepared_offset: usize = if (index == 0) 9 else 10;
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + prepared_offset, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.commit(commit_request));
        store.fail_after.store(0, .monotonic);
        try expect_journal_phase(state_dir, "prepared");
        const installed_manifest = try state_dir.readFileAlloc(std.testing.allocator, "manifest.json", MaxManifestBytes);
        defer std.testing.allocator.free(installed_manifest);
        try std.testing.expectEqualStrings(artifact.candidate_manifest, installed_manifest);
        var installed = try std.fs.openFileAbsolute(artifact.path, .{});
        const installed_bytes = try installed.readToEndAlloc(std.testing.allocator, MaxArtifactBytes);
        installed.close();
        defer std.testing.allocator.free(installed_bytes);
        try std.testing.expectEqualSlices(u8, artifact.bytes, installed_bytes);
        try store.recover();
        try store.recover();
        try std.testing.expectError(error.FileNotFound, store.inspect_record(artifact.record_id));
        for (generated.items[0..index]) |prior| {
            const observed = try store.inspect_record(prior.record_id);
            try std.testing.expectEqual(sha256_raw(prior.bytes), observed.target.sha256);
        }
        const committed_offset: usize = if (index == 0) 11 else 12;
        store.fail_after.store(store.checkpoint_count.load(.monotonic) + committed_offset, .monotonic);
        try std.testing.expectError(error.InjectedFailure, store.commit(commit_request));
        store.fail_after.store(0, .monotonic);
        try expect_journal_phase(state_dir, "committed");
        try store.recover();
        try store.recover();
        const committed = try store.inspect_record(artifact.record_id);
        try std.testing.expectEqual(sha256_raw(artifact.bytes), committed.target.sha256);
    }
    try store.recover();
    for (generated.items) |artifact| {
        const observed = try store.inspect_record(artifact.record_id);
        try std.testing.expectEqual(sha256_raw(artifact.bytes), observed.target.sha256);
        if (std.mem.eql(u8, artifact.kind, "desktop_entry")) {
            const argv = [_][]const u8{ "desktop-file-validate", artifact.path };
            const validation = try std.process.Child.run(.{ .allocator = std.testing.allocator, .argv = &argv });
            defer std.testing.allocator.free(validation.stdout);
            defer std.testing.allocator.free(validation.stderr);
            try std.testing.expectEqual(std.process.Child.Term{ .Exited = 0 }, validation.term);
        }
    }
}

fn expect_journal_phase(state_dir: std.fs.Dir, phase: []const u8) !void {
    var transactions = try state_dir.openDir("transactions", .{ .iterate = true });
    defer transactions.close();
    var iterator = transactions.iterate();
    const entry = (try iterator.next()) orelse return error.TestExpectedJournal;
    try std.testing.expectEqual(@as(usize, 32), entry.name.len);
    var transaction = try transactions.openDir(entry.name, .{});
    defer transaction.close();
    const bytes = try transaction.readFileAlloc(std.testing.allocator, "journal.json", 16384);
    defer std.testing.allocator.free(bytes);
    const journal = try std.json.parseFromSlice(std.json.Value, std.testing.allocator, bytes, .{});
    defer journal.deinit();
    try std.testing.expectEqualStrings(phase, journal.value.object.get("phase").?.string);
    try std.testing.expect((try iterator.next()) == null);
}
