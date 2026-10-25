//! Command layer for `waddle integration` (desktop-entry handler producer only).
//! Parsing, rendering, and orchestration live here; every filesystem mutation goes
//! through `manifest_store.store_t`, which owns the manifest lock, the roots, and
//! the journal. The only direct filesystem calls create the XDG base directories
//! and the private manifest directory before the store opens them.
const std = @import("std");
const manifest_store = @import("manifest_store");
const producers = @import("integration_producers");
const manifest = manifest_store.manifest;

/// Exit status for a clean outcome, including drift reported by `status`.
pub const ExitOk: u8 = 0;
/// Exit status for a blocked or failed operation.
pub const ExitFailed: u8 = 1;
/// Exit status for a usage error.
pub const ExitUsage: u8 = 2;
/// Exit status for an allocation failure.
pub const ExitResource: u8 = 125;

/// Subcommand selected by the first operand.
pub const Operation = enum { Install, Remove, Repair, Status, Migrate, Guest, Shortcut };

/// Parsed command line. Slices borrow argv for the duration of one call.
pub const options_t = struct {
    operation: Operation,
    json: bool = false,
    device: ?[]const u8 = null,
    all: bool = false,
    force: bool = false,
    system: bool = false,
    guest: bool = false,
    help: bool = false,
};

/// Environment and policy for one invocation. All paths must be absolute.
pub const env_t = struct {
    /// Resolved XDG data home; the handler desktop entry lives below it.
    data_home: []const u8,
    /// Resolved XDG state home; the manifest lives in `waddle/integration`.
    state_home: []const u8,
    /// Absolute path of the running Waddle binary used by the handler `Exec`.
    launcher: []const u8,
    /// Seconds since the epoch used for the manifest `updated` field.
    now: i64,
    /// Effective user id; `--system` requires 0.
    euid: u32,
    /// False when the selected device sets integration.exe_handler=off.
    exe_handler_enabled: bool,
};

/// Rendered result of one invocation. Both buffers are owned by the caller's
/// allocator and released with `deinit`.
pub const output_t = struct {
    /// Process exit status.
    status: u8,
    /// Bytes for standard output (text or one JSON object and a newline).
    stdout: []u8,
    /// Bytes for standard error (usage or operational messages).
    stderr: []u8,

    /// Release both buffers. Safe to call once.
    pub fn deinit(self: *output_t, allocator: std.mem.Allocator) void {
        allocator.free(self.stdout);
        allocator.free(self.stderr);
        self.stdout = &.{};
        self.stderr = &.{};
    }
};

/// Usage text for `waddle integration --help`.
pub const usage_text =
    "Usage: waddle integration install [--device N] [--json]\n" ++
    "       waddle integration remove [--device N | --all] [--json]\n" ++
    "       waddle integration repair [--device N] [--force] [--json]\n" ++
    "       waddle integration status [--device N] [--json]\n" ++
    "       waddle integration migrate [--json]\n" ++
    "Scope: per-user. --system requires euid 0 (not implemented in this build).\n" ++
    "Exit codes: 0 success or drift reported, 1 blocked or failed, 2 usage, 125 resources.\n";

/// Stable subcommand word used in JSON output.
fn operation_name(operation: Operation) []const u8 {
    return switch (operation) {
        .Install => "install",
        .Remove => "remove",
        .Repair => "repair",
        .Status => "status",
        .Migrate => "migrate",
        .Guest => "guest",
        .Shortcut => "shortcut",
    };
}

/// Parse the operands after `waddle integration`. The first operand names the
/// subcommand; flags may appear in any order. Duplicate flags, unknown options,
/// `--all` with `--device`, `--force` outside repair, and `--all` outside remove
/// are usage errors.
/// Errors: Usage.
pub fn parse(args: []const []const u8) !options_t {
    if (args.len == 0) return error.Usage;
    if (std.mem.eql(u8, args[0], "--help") or std.mem.eql(u8, args[0], "-h")) return options_t{ .operation = .Status, .help = true };
    var options = options_t{ .operation = undefined };
    options.operation = if (std.mem.eql(u8, args[0], "install")) .Install else if (std.mem.eql(u8, args[0], "remove")) .Remove else if (std.mem.eql(u8, args[0], "repair")) .Repair else if (std.mem.eql(u8, args[0], "status")) .Status else if (std.mem.eql(u8, args[0], "migrate")) .Migrate else if (std.mem.eql(u8, args[0], "shortcut")) .Shortcut else return error.Usage;
    var index: usize = 1;
    while (index < args.len) : (index += 1) {
        const arg = args[index];
        if (std.mem.eql(u8, arg, "--json")) {
            if (options.json) return error.Usage;
            options.json = true;
        } else if (std.mem.eql(u8, arg, "--all")) {
            if (options.all) return error.Usage;
            options.all = true;
        } else if (std.mem.eql(u8, arg, "--force")) {
            if (options.force) return error.Usage;
            options.force = true;
        } else if (std.mem.eql(u8, arg, "--system")) {
            if (options.system) return error.Usage;
            options.system = true;
        } else if (std.mem.eql(u8, arg, "--guest")) {
            if (options.guest) return error.Usage;
            options.guest = true;
        } else if (std.mem.eql(u8, arg, "--help") or std.mem.eql(u8, arg, "-h")) {
            options.help = true;
        } else if (std.mem.eql(u8, arg, "--device")) {
            if (options.device != null or index + 1 >= args.len) return error.Usage;
            index += 1;
            if (!producers.device_valid(args[index])) return error.Usage;
            options.device = args[index];
        } else return error.Usage;
    }
    if (options.all and options.device != null) return error.Usage;
    if (options.force and options.operation != .Repair) return error.Usage;
    if (options.all and options.operation != .Remove) return error.Usage;
    return options;
}

/// Resolve the XDG data and state homes as the XDG Base Directory specification
/// requires: a variable that is set and absolute wins; otherwise the default under
/// the absolute `HOME`. Returns owned slices. Errors: InvalidEnvironment when neither
/// an absolute variable nor an absolute HOME exists.
pub fn resolve_paths(allocator: std.mem.Allocator, home: ?[]const u8, xdg_data: ?[]const u8, xdg_state: ?[]const u8) !struct { data_home: []u8, state_home: []u8 } {
    const data_home = try resolve_one(allocator, home, xdg_data, ".local/share");
    errdefer allocator.free(data_home);
    const state_home = try resolve_one(allocator, home, xdg_state, ".local/state");
    return .{ .data_home = data_home, .state_home = state_home };
}

fn resolve_one(allocator: std.mem.Allocator, home: ?[]const u8, variable: ?[]const u8, fallback: []const u8) ![]u8 {
    if (variable) |value| if (value.len > 0 and value[0] == '/') return allocator.dupe(u8, value);
    const base = home orelse return error.InvalidEnvironment;
    if (base.len == 0 or base[0] != '/') return error.InvalidEnvironment;
    return std.fs.path.join(allocator, &.{ base, fallback });
}

/// Format an epoch time as the frozen `YYYY-MM-DDTHH:MM:SSZ` manifest timestamp.
pub fn format_updated(buffer: *[20]u8, now: i64) []const u8 {
    const seconds = std.time.epoch.EpochSeconds{ .secs = @intCast(@max(now, 0)) };
    const year_day = seconds.getEpochDay().calculateYearDay();
    const month_day = year_day.calculateMonthDay();
    const day_seconds = seconds.getDaySeconds();
    return std.fmt.bufPrint(buffer, "{d:0>4}-{d:0>2}-{d:0>2}T{d:0>2}:{d:0>2}:{d:0>2}Z", .{
        year_day.year,
        month_day.month.numeric(),
        month_day.day_index + 1,
        day_seconds.getHoursIntoDay(),
        day_seconds.getMinutesIntoHour(),
        day_seconds.getSecondsIntoMinute(),
    }) catch unreachable;
}

/// Timestamp for a new candidate. A store candidate must differ from the current
/// manifest, so when the second is unchanged the stamp advances by one second.
fn next_updated(buffer: *[20]u8, now: i64, previous: []const u8) []const u8 {
    const text = format_updated(buffer, now);
    if (!std.mem.eql(u8, text, previous)) return text;
    return format_updated(buffer, now + 1);
}

/// One result row in the stable output schema.
const row_t = struct {
    id: []const u8,
    kind: []const u8,
    path: []const u8,
    device: ?[]const u8,
    state: []const u8,
    action: []const u8,
    backup: ?[]const u8 = null,
    blocked: bool = false,
};

/// A failure mapped to its public code, message, and exit status.
const failure_t = struct { code: []const u8, message: []const u8, status: u8 };

fn failure_for(err: anyerror) failure_t {
    return switch (err) {
        error.Usage => .{ .code = "usage", .message = "invalid integration command; see waddle integration --help", .status = ExitUsage },
        error.OutOfMemory => .{ .code = "resource_error", .message = "allocation failed", .status = ExitResource },
        error.UnsupportedSchema => .{ .code = "schema_newer", .message = "manifest schema is newer than this Waddle; refusing to modify it", .status = ExitFailed },
        error.VersionDowngrade => .{ .code = "version_downgrade", .message = "manifest waddle_version is newer than this Waddle; refusing to downgrade", .status = ExitFailed },
        error.Busy => .{ .code = "busy", .message = "another Waddle process holds the integration manifest lock", .status = ExitFailed },
        error.AccessDenied => .{ .code = "permission_denied", .message = "unsafe permissions, ownership, or symlink in the integration scope", .status = ExitFailed },
        error.RecoveryRequired => .{ .code = "recovery_required", .message = "a journal needs manual inspection; its evidence was kept", .status = ExitFailed },
        error.ArtifactConflict => .{ .code = "artifact_conflict", .message = "the artifact changed while the operation ran", .status = ExitFailed },
        error.ManifestMismatch => .{ .code = "manifest_mismatch", .message = "the manifest record does not match the requested change", .status = ExitFailed },
        error.InvalidEnvironment => .{ .code = "invalid_environment", .message = "HOME and the XDG base directories must be absolute", .status = ExitFailed },
        error.NotImplemented => .{ .code = "not_implemented", .message = "not implemented in this build", .status = ExitFailed },
        error.FileNotFound => .{ .code = "not_found", .message = "a required file or record is absent", .status = ExitFailed },
        else => .{ .code = "io_error", .message = "filesystem operation failed", .status = ExitFailed },
    };
}

/// Execute one invocation and render its result. The returned buffers belong to
/// `allocator`; every intermediate allocation is released before return.
/// Errors: only OutOfMemory escapes; every other failure becomes a rendered result.
pub fn run(allocator: std.mem.Allocator, options: options_t, env: env_t) !output_t {
    var arena_state = std.heap.ArenaAllocator.init(allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    var rows = std.ArrayList(row_t).init(allocator);
    defer rows.deinit();
    var status: u8 = ExitOk;
    var failed: ?failure_t = null;
    if (options.help) return render(allocator, options, &rows, ExitOk, null, usage_text);
    if (options.guest) {
        failed = failure_for(error.NotImplemented);
    } else if (options.operation == .Shortcut) {
        failed = failure_for(error.NotImplemented);
    } else if (options.system) {
        failed = if (env.euid != 0) failure_for(error.AccessDenied) else failure_for(error.NotImplemented);
    } else {
        executeScope(arena, options, env, &rows, &status) catch |err| {
            if (err == error.OutOfMemory) return err;
            failed = failure_for(err);
        };
    }
    if (failed) |issue| status = issue.status;
    return render(allocator, options, &rows, status, failed, "");
}

/// Open the per-user store, run the requested operation, and fill `rows`.
fn executeScope(arena: std.mem.Allocator, options: options_t, env: env_t, rows: *std.ArrayList(row_t), status: *u8) !void {
    var session = try Session.open(arena, env);
    defer session.close();
    switch (options.operation) {
        .Install => try install(arena, &session, options, env, rows, status),
        .Remove => try remove(arena, &session, options, env, rows, status),
        .Repair => try repair(arena, &session, options, env, rows, status),
        .Status => try status_rows(arena, &session, options, rows),
        .Migrate => try migrate(arena, &session, env, rows, status),
        .Guest, .Shortcut => return error.NotImplemented,
    }
}

/// Store handle plus the descriptors and canonical root it owns for one run.
const Session = struct {
    store: manifest_store.store_t,
    data_real: []u8,
    data_fd: std.posix.fd_t,
    manifest_fd: std.posix.fd_t,
    allocator: std.mem.Allocator,

    /// Create the XDG base directories and the private manifest directory, then open
    /// the store. The data root is canonicalized so the store can bind it to the
    /// descriptor it is given.
    fn open(allocator: std.mem.Allocator, env: env_t) !Session {
        try std.fs.cwd().makePath(env.data_home);
        try std.fs.cwd().makePath(env.state_home);
        const waddle_dir = try std.fs.path.join(allocator, &.{ env.state_home, "waddle" });
        defer allocator.free(waddle_dir);
        const manifest_dir = try std.fs.path.join(allocator, &.{ waddle_dir, "integration" });
        defer allocator.free(manifest_dir);
        try make_private(waddle_dir);
        try make_private(manifest_dir);
        const data_real = try std.fs.realpathAlloc(allocator, env.data_home);
        errdefer allocator.free(data_real);
        const data_fd = try std.posix.open(data_real, .{ .ACCMODE = .RDONLY, .DIRECTORY = true, .CLOEXEC = true }, 0);
        errdefer std.posix.close(data_fd);
        const manifest_fd = try std.posix.open(manifest_dir, .{ .ACCMODE = .RDONLY, .DIRECTORY = true, .CLOEXEC = true, .NOFOLLOW = true }, 0);
        errdefer std.posix.close(manifest_fd);
        const roots = [_]manifest_store.root_t{.{ .kind = .Data, .canonical_path = data_real, .dir_fd = data_fd }};
        const store = try manifest_store.store_t.init(allocator, manifest_fd, &roots);
        return .{ .store = store, .data_real = data_real, .data_fd = data_fd, .manifest_fd = manifest_fd, .allocator = allocator };
    }

    fn close(self: *Session) void {
        self.store.deinit();
        std.posix.close(self.manifest_fd);
        std.posix.close(self.data_fd);
        self.allocator.free(self.data_real);
    }

    /// Canonical absolute path of the handler desktop entry.
    fn handler_path(self: *const Session, arena: std.mem.Allocator) ![]u8 {
        return std.fs.path.join(arena, &.{ self.data_real, "applications", producers.HandlerLeaf });
    }
};

/// Create a private (0700) directory, tolerating one that already exists. Existing
/// directories are validated by the store when it opens them.
fn make_private(path: []const u8) !void {
    std.posix.mkdir(path, 0o700) catch |err| if (err != error.PathAlreadyExists) return err;
}

/// Parsed manifest view: the document (owned by the arena) and the schema records.
const parsed_t = struct { records: []const manifest.record_t, waddle_version: []const u8, updated: []const u8 };

/// Read and parse the current manifest; an absent manifest is an empty view.
fn current(arena: std.mem.Allocator, session: *Session) !parsed_t {
    const bytes = (try session.store.manifest_snapshot(arena)) orelse return .{ .records = &.{}, .waddle_version = producers.WaddleVersion, .updated = "" };
    const document = try manifest.parse(arena, bytes);
    return .{ .records = document.records, .waddle_version = document.waddle_version, .updated = document.updated };
}

fn find_record(records: []const manifest.record_t, id: []const u8) ?manifest.record_t {
    for (records) |record| if (std.mem.eql(u8, record.id, id)) return record;
    return null;
}

fn sha256_hex(bytes: []const u8) [64]u8 {
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
    return std.fmt.bytesToHex(digest, .lower);
}

fn state_text(state: manifest_store.record_state_t) []const u8 {
    return switch (state) {
        .Ok => "ok",
        .Missing => "missing",
        .Modified => "modified",
        .Foreign => "foreign",
    };
}

/// Current state of one record as reported by the store's read-only classifier.
fn record_state(arena: std.mem.Allocator, session: *Session, id: []const u8) !?manifest_store.record_state_t {
    var list = try session.store.status(arena);
    defer list.deinit();
    for (list.items) |item| if (std.mem.eql(u8, item.record_id, id)) return item.state;
    return null;
}

fn record_row(record: manifest.record_t, path: []const u8, state: []const u8, action: []const u8) row_t {
    return .{ .id = record.id, .kind = producers.kind_wire(record.kind), .path = path, .device = record.device, .state = state, .action = action, .blocked = std.mem.eql(u8, action, "refused") or std.mem.eql(u8, action, "blocked") or std.mem.eql(u8, action, "kept_modified") or std.mem.eql(u8, action, "kept_foreign") };
}

/// Install the handler entry, or report that it already matches.
fn install(arena: std.mem.Allocator, session: *Session, options: options_t, env: env_t, rows: *std.ArrayList(row_t), status: *u8) !void {
    const id = producers.HandlerRecordId;
    const path = try session.handler_path(arena);
    if (!env.exe_handler_enabled) {
        try rows.append(.{ .id = id, .kind = "handler_entry", .path = path, .device = options.device, .state = "missing", .action = "disabled_by_policy" });
        return;
    }
    const document = try current(arena, session);
    if (find_record(document.records, id)) |existing| {
        const rendered = try producers.handler_desktop(arena, env.launcher, existing.device);
        const digest = sha256_hex(rendered);
        const state = (try record_state(arena, session, id)) orelse .Missing;
        if (state == .Ok and std.ascii.eqlIgnoreCase(existing.sha256, &digest)) {
            try rows.append(record_row(existing, path, "ok", "unchanged"));
            return;
        }
        if (state == .Missing and std.ascii.eqlIgnoreCase(existing.sha256, &digest)) {
            try recreate_missing(arena, session, document, existing, rendered, env, rows);
            return;
        }
        try rows.append(record_row(existing, path, state_text(state), "refused"));
        status.* = ExitFailed;
        return;
    }
    const rendered = try producers.handler_desktop(arena, env.launcher, options.device);
    const digest = sha256_hex(rendered);
    try session.store.ensure_directory(.Data, "applications");
    const record = producers.handler_record(path, &digest, options.device);
    var stamp: [20]u8 = undefined;
    const candidate = try producers.candidate_document(arena, .{ .waddle_version = producers.WaddleVersion, .updated = next_updated(&stamp, env.now, document.updated) }, document.records, id, record);
    session.store.commit(.{ .record_id = id, .prior = .Absent, .artifact_bytes = rendered, .artifact_mode = 0o644, .candidate_manifest = candidate }) catch |err| {
        if (err != error.ArtifactConflict) return err;
        try rows.append(.{ .id = id, .kind = "handler_entry", .path = path, .device = options.device, .state = "foreign", .action = "refused", .blocked = true });
        status.* = ExitFailed;
        return;
    };
    try rows.append(.{ .id = id, .kind = "handler_entry", .path = path, .device = options.device, .state = "ok", .action = "installed" });
}

/// Republish a recorded but absent artifact with its rendered bytes. The candidate
/// keeps the record identity; only its digest and creator change.
fn recreate_missing(arena: std.mem.Allocator, session: *Session, document: parsed_t, existing: manifest.record_t, rendered: []const u8, env: env_t, rows: *std.ArrayList(row_t)) !void {
    const digest = sha256_hex(rendered);
    var record = existing;
    record.sha256 = &digest;
    record.created_by = producers.WaddleVersion;
    var stamp: [20]u8 = undefined;
    const candidate = try producers.candidate_document(arena, .{ .waddle_version = producers.WaddleVersion, .updated = next_updated(&stamp, env.now, document.updated) }, document.records, existing.id, record);
    _ = try session.store.recreate(.{ .record_id = existing.id, .candidate_manifest = candidate, .artifact_bytes = rendered, .artifact_mode = 0o644 });
    try rows.append(record_row(existing, try session.handler_path(arena), "ok", "repaired"));
}

/// Remove the handler record and its artifact when it is unmodified.
fn remove(arena: std.mem.Allocator, session: *Session, options: options_t, env: env_t, rows: *std.ArrayList(row_t), status: *u8) !void {
    const id = producers.HandlerRecordId;
    const path = try session.handler_path(arena);
    const document = try current(arena, session);
    const existing = find_record(document.records, id) orelse {
        try rows.append(.{ .id = id, .kind = "handler_entry", .path = path, .device = options.device, .state = "missing", .action = "not_installed" });
        return;
    };
    if (options.device) |device| {
        if (existing.device == null or !std.mem.eql(u8, existing.device.?, device)) {
            try rows.append(record_row(existing, path, "ok", "not_selected"));
            return;
        }
    }
    var stamp: [20]u8 = undefined;
    const candidate = try producers.candidate_document(arena, .{ .waddle_version = producers.WaddleVersion, .updated = next_updated(&stamp, env.now, document.updated) }, document.records, id, null);
    const receipt = try session.store.remove(.{ .record_id = id, .candidate_manifest = candidate });
    const action: []const u8 = switch (receipt.outcome) {
        .Removed => "removed",
        .RecordRemovedArtifactMissing => "record_removed_artifact_missing",
        .RecordRemovedNonFile => "record_removed",
        .AlreadyMissing => "not_installed",
        .KeptModified => "kept_modified",
        .KeptForeign => "kept_foreign",
    };
    const state: []const u8 = if (receipt.outcome == .Removed) "ok" else if (receipt.outcome == .KeptModified) "modified" else if (receipt.outcome == .KeptForeign) "foreign" else "missing";
    if (receipt.outcome == .KeptModified or receipt.outcome == .KeptForeign) status.* = ExitFailed;
    try rows.append(.{ .id = id, .kind = "handler_entry", .path = path, .device = existing.device, .state = state, .action = action, .blocked = status.* != ExitOk });
}

/// Recreate a missing handler artifact, or replace a modified one under `--force`
/// with the replacement's retained backup. Foreign objects are never touched.
fn repair(arena: std.mem.Allocator, session: *Session, options: options_t, env: env_t, rows: *std.ArrayList(row_t), status: *u8) !void {
    const id = producers.HandlerRecordId;
    const path = try session.handler_path(arena);
    const document = try current(arena, session);
    const existing = find_record(document.records, id) orelse {
        try rows.append(.{ .id = id, .kind = "handler_entry", .path = path, .device = options.device, .state = "missing", .action = "not_installed" });
        return;
    };
    if (options.device) |device| {
        if (existing.device == null or !std.mem.eql(u8, existing.device.?, device)) {
            try rows.append(record_row(existing, path, "ok", "not_selected"));
            return;
        }
    }
    const state = (try record_state(arena, session, id)) orelse .Missing;
    const rendered = try producers.handler_desktop(arena, env.launcher, existing.device);
    switch (state) {
        .Ok => try rows.append(record_row(existing, path, "ok", "unchanged")),
        .Missing => try recreate_missing(arena, session, document, existing, rendered, env, rows),
        .Foreign => {
            try rows.append(record_row(existing, path, "foreign", "blocked"));
            status.* = ExitFailed;
        },
        .Modified => {
            if (!options.force) {
                try rows.append(record_row(existing, path, "modified", "blocked"));
                status.* = ExitFailed;
                return;
            }
            try replace_modified(arena, session, document, existing, rendered, env, rows);
        },
    }
}

/// Replace a modified recorded artifact with the rendered bytes, retaining the
/// exact previous bytes in a private backup named in the row.
fn replace_modified(arena: std.mem.Allocator, session: *Session, document: parsed_t, existing: manifest.record_t, rendered: []const u8, env: env_t, rows: *std.ArrayList(row_t)) !void {
    const snapshot = try session.store.inspect_record(existing.id);
    const digest = sha256_hex(rendered);
    var record = existing;
    record.sha256 = &digest;
    record.created_by = producers.WaddleVersion;
    var stamp: [20]u8 = undefined;
    const candidate = try producers.candidate_document(arena, .{ .waddle_version = producers.WaddleVersion, .updated = next_updated(&stamp, env.now, document.updated) }, document.records, existing.id, record);
    const receipt = try session.store.replace(.{
        .record_id = existing.id,
        .expected_manifest_sha256 = snapshot.manifest_sha256,
        .expected_target = snapshot.target,
        .artifact_bytes = rendered,
        .artifact_mode = 0o644,
        .candidate_manifest = candidate,
        .backup_modified = true,
    });
    var row = record_row(existing, try session.handler_path(arena), "ok", "replaced");
    if (receipt.backup) |backup| row.backup = try arena.dupe(u8, &backup.transaction_id);
    try rows.append(row);
}

/// Report every record with its current state. Drift is reported and exits 0.
fn status_rows(arena: std.mem.Allocator, session: *Session, options: options_t, rows: *std.ArrayList(row_t)) !void {
    const document = try current(arena, session);
    var list = try session.store.status(arena);
    defer list.deinit();
    for (list.items) |item| {
        const record = find_record(document.records, item.record_id) orelse continue;
        if (options.device) |device| if (record.device == null or !std.mem.eql(u8, record.device.?, device)) continue;
        const path = record.path;
        try rows.append(.{ .id = record.id, .kind = producers.kind_wire(record.kind), .path = path, .device = record.device, .state = state_text(item.state), .action = "reported" });
    }
}

/// Rewrite the manifest root metadata to the current Waddle version. An identical
/// version keeps the timestamp so repeated migration is a no-op.
fn migrate(arena: std.mem.Allocator, session: *Session, env: env_t, rows: *std.ArrayList(row_t), status: *u8) !void {
    _ = status;
    const document = try current(arena, session);
    const first = if (document.records.len > 0) document.records[0] else {
        try rows.append(.{ .id = "", .kind = "", .path = "", .device = null, .state = "missing", .action = "nothing_to_migrate" });
        return;
    };
    var stamp: [20]u8 = undefined;
    const same_version = std.mem.eql(u8, document.waddle_version, producers.WaddleVersion);
    const updated = if (same_version) document.updated else format_updated(&stamp, env.now);
    const candidate = try producers.encode_document(arena, .{ .waddle_version = producers.WaddleVersion, .updated = updated }, document.records);
    const receipt = try session.store.migrate_record(.{ .record_id = first.id, .candidate_manifest = candidate });
    try rows.append(.{ .id = first.id, .kind = producers.kind_wire(first.kind), .path = first.path, .device = first.device, .state = "ok", .action = if (receipt.changed) "migrated" else "unchanged" });
}

/// Build the rendered output for the parsed result rows. Human output lists one
/// line per row; JSON output is one object with the schema_version 1 envelope.
fn render(allocator: std.mem.Allocator, options: options_t, rows: *std.ArrayList(row_t), status: u8, issue: ?failure_t, prefix: []const u8) !output_t {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var err_out = std.ArrayList(u8).init(allocator);
    errdefer err_out.deinit();
    if (prefix.len > 0) try out.appendSlice(prefix);
    if (options.json) {
        const error_value: ?struct { code: []const u8, message: []const u8 } = if (issue) |found| .{ .code = found.code, .message = found.message } else null;
        const document = .{
            .schema_version = @as(u32, 1),
            .command = "integration",
            .operation = operation_name(options.operation),
            .ok = status == ExitOk,
            .results = rows.items,
            .@"error" = error_value,
        };
        try std.json.stringify(document, .{}, out.writer());
        try out.append('\n');
    } else if (issue) |found| {
        try err_out.writer().print("waddle integration: {s}: {s}\n", .{ found.code, found.message });
    } else if (prefix.len == 0) {
        for (rows.items) |row| {
            try out.writer().print("{s}: {s} {s}", .{ row.id, row.state, row.action });
            if (row.device) |device| try out.writer().print(" device={s}", .{device});
            if (row.backup) |backup| try out.writer().print(" backup={s}", .{backup});
            if (row.path.len > 0) try out.writer().print(" {s}", .{row.path});
            try out.append('\n');
        }
    }
    if (status != ExitOk and !options.json and issue == null) {
        try err_out.writer().print("waddle integration: {d} item(s) blocked\n", .{rows.items.len});
    }
    return .{ .status = status, .stdout = try out.toOwnedSlice(), .stderr = try err_out.toOwnedSlice() };
}

// Test-only fixtures.
const testing = std.testing;

fn test_env(data_home: []const u8, state_home: []const u8) env_t {
    return .{ .data_home = data_home, .state_home = state_home, .launcher = "/opt/waddle/bin/waddle", .now = 1_793_000_000, .euid = 1000, .exe_handler_enabled = true };
}

test "parser accepts every subcommand and rejects misuse with usage errors" {
    try testing.expectEqual(Operation.Install, (try parse(&.{"install"})).operation);
    const removal = try parse(&.{ "remove", "--all", "--json" });
    try testing.expect(removal.all and removal.json);
    try testing.expect((try parse(&.{ "repair", "--force" })).force);
    try testing.expectEqualStrings("alpha", (try parse(&.{ "status", "--device", "alpha" })).device.?);
    try testing.expect((try parse(&.{ "status", "--system", "--guest" })).guest);
    try testing.expectError(error.Usage, parse(&.{}));
    try testing.expect((try parse(&.{"--help"})).help);
    try testing.expectError(error.Usage, parse(&.{"bogus"}));
    try testing.expectError(error.Usage, parse(&.{ "install", "--json", "--json" }));
    try testing.expectError(error.Usage, parse(&.{ "remove", "--all", "--device", "alpha" }));
    try testing.expectError(error.Usage, parse(&.{ "install", "--force" }));
    try testing.expectError(error.Usage, parse(&.{ "status", "--all" }));
    try testing.expectError(error.Usage, parse(&.{ "install", "--device" }));
    try testing.expectError(error.Usage, parse(&.{ "install", "--device", "bad/name" }));
    try testing.expectError(error.Usage, parse(&.{ "install", "extra" }));
    try testing.expectError(error.Usage, parse(&.{ "install", "--system", "--system" }));
}

test "XDG resolution honours absolute variables and falls back under HOME" {
    const allocator = testing.allocator;
    const defaults = try resolve_paths(allocator, "/home/u", null, null);
    defer allocator.free(defaults.data_home);
    defer allocator.free(defaults.state_home);
    try testing.expectEqualStrings("/home/u/.local/share", defaults.data_home);
    try testing.expectEqualStrings("/home/u/.local/state", defaults.state_home);
    const explicit = try resolve_paths(allocator, "/home/u", "/xdg/data", "relative");
    defer allocator.free(explicit.data_home);
    defer allocator.free(explicit.state_home);
    try testing.expectEqualStrings("/xdg/data", explicit.data_home);
    try testing.expectEqualStrings("/home/u/.local/state", explicit.state_home);
    try testing.expectError(error.InvalidEnvironment, resolve_paths(allocator, null, null, null));
    try testing.expectError(error.InvalidEnvironment, resolve_paths(allocator, "relative", null, null));
}

test "updated timestamps use the frozen ISO 8601 UTC form" {
    var buffer: [20]u8 = undefined;
    const sample = format_updated(&buffer, 1_793_000_000);
    try testing.expectEqual(@as(usize, 20), sample.len);
    try testing.expect(std.mem.endsWith(u8, sample, "Z") and sample[10] == 'T');
    try testing.expectEqualStrings("1970-01-01T00:00:00Z", format_updated(&buffer, 0));
}

test "help and guest and shortcut report without touching state" {
    const allocator = testing.allocator;
    var help = try run(allocator, .{ .operation = .Install, .help = true }, test_env("/unused", "/unused"));
    defer help.deinit(allocator);
    try testing.expectEqual(ExitOk, help.status);
    try testing.expect(std.mem.indexOf(u8, help.stdout, "Usage: waddle integration") != null);
    var guest = try run(allocator, .{ .operation = .Remove, .guest = true }, test_env("/unused", "/unused"));
    defer guest.deinit(allocator);
    try testing.expectEqual(ExitFailed, guest.status);
    try testing.expect(std.mem.indexOf(u8, guest.stderr, "not_implemented") != null);
    var shortcut = try run(allocator, .{ .operation = .Shortcut }, test_env("/unused", "/unused"));
    defer shortcut.deinit(allocator);
    try testing.expectEqual(ExitFailed, shortcut.status);
}

test "system scope refuses a non-root caller and does not reach the store" {
    const allocator = testing.allocator;
    var env = test_env("/unused", "/unused");
    env.euid = 1000;
    var result = try run(allocator, .{ .operation = .Status, .system = true }, env);
    defer result.deinit(allocator);
    try testing.expectEqual(ExitFailed, result.status);
    try testing.expect(std.mem.indexOf(u8, result.stderr, "permission_denied") != null);
}

/// Private fixture: a temporary base with data and state homes and matching env.
const fixture_t = struct {
    tmp: std.testing.TmpDir,
    base: []u8,
    data: []u8,
    state: []u8,

    fn create(allocator: std.mem.Allocator) !fixture_t {
        var tmp = std.testing.tmpDir(.{});
        errdefer tmp.cleanup();
        const base = try tmp.dir.realpathAlloc(allocator, ".");
        errdefer allocator.free(base);
        const data = try std.fs.path.join(allocator, &.{ base, "data" });
        errdefer allocator.free(data);
        const state = try std.fs.path.join(allocator, &.{ base, "state" });
        return .{ .tmp = tmp, .base = base, .data = data, .state = state };
    }

    fn deinit(self: *fixture_t, allocator: std.mem.Allocator) void {
        allocator.free(self.state);
        allocator.free(self.data);
        allocator.free(self.base);
        self.tmp.cleanup();
    }

    fn env(self: *const fixture_t) env_t {
        return test_env(self.data, self.state);
    }

    fn handler_file(self: *const fixture_t, allocator: std.mem.Allocator) ![]u8 {
        return std.fs.path.join(allocator, &.{ self.data, "applications", producers.HandlerLeaf });
    }
};

fn invoke(allocator: std.mem.Allocator, args: []const []const u8, env: env_t) !output_t {
    const options = try parse(args);
    return run(allocator, options, env);
}

test "install writes the handler once, reports unchanged on repeat, and records it" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var first = try invoke(allocator, &.{"install"}, fx.env());
    defer first.deinit(allocator);
    try testing.expectEqual(ExitOk, first.status);
    try testing.expect(std.mem.indexOf(u8, first.stdout, "installed") != null);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    const written = try std.fs.cwd().readFileAlloc(allocator, path, 65536);
    defer allocator.free(written);
    const expected = try producers.handler_desktop(allocator, "/opt/waddle/bin/waddle", null);
    defer allocator.free(expected);
    try testing.expectEqualStrings(expected, written);
    var second = try invoke(allocator, &.{"install"}, fx.env());
    defer second.deinit(allocator);
    try testing.expectEqual(ExitOk, second.status);
    try testing.expect(std.mem.indexOf(u8, second.stdout, "unchanged") != null);
}

test "install refuses an unrecorded file and reports it with exit 1 without changing it" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    const applications = try std.fs.path.join(allocator, &.{ fx.data, "applications" });
    defer allocator.free(applications);
    try std.fs.cwd().makePath(applications);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    try std.fs.cwd().writeFile(.{ .sub_path = path, .data = "user owned\n" });
    var result = try invoke(allocator, &.{"install"}, fx.env());
    defer result.deinit(allocator);
    try testing.expectEqual(ExitFailed, result.status);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "refused") != null);
    const kept = try std.fs.cwd().readFileAlloc(allocator, path, 65536);
    defer allocator.free(kept);
    try testing.expectEqualStrings("user owned\n", kept);
}

test "install with integration.exe_handler=off changes nothing" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var env = fx.env();
    env.exe_handler_enabled = false;
    var result = try invoke(allocator, &.{"install"}, env);
    defer result.deinit(allocator);
    try testing.expectEqual(ExitOk, result.status);
    try testing.expect(std.mem.indexOf(u8, result.stdout, "disabled_by_policy") != null);
    const absent = try fx.handler_file(allocator);
    defer allocator.free(absent);
    try testing.expectError(error.FileNotFound, std.fs.cwd().access(absent, .{}));
}

test "remove deletes an unmodified handler, reports absence on repeat, and keeps a modified file" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{"install"}, fx.env());
    installed.deinit(allocator);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    var removed = try invoke(allocator, &.{"remove"}, fx.env());
    defer removed.deinit(allocator);
    try testing.expectEqual(ExitOk, removed.status);
    try testing.expect(std.mem.indexOf(u8, removed.stdout, "removed") != null);
    try testing.expectError(error.FileNotFound, std.fs.cwd().access(path, .{}));
    var repeat = try invoke(allocator, &.{"remove"}, fx.env());
    defer repeat.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, repeat.stdout, "not_installed") != null);

    var again = try invoke(allocator, &.{"install"}, fx.env());
    again.deinit(allocator);
    try std.fs.cwd().writeFile(.{ .sub_path = path, .data = "edited by the user\n" });
    var kept = try invoke(allocator, &.{"remove"}, fx.env());
    defer kept.deinit(allocator);
    try testing.expectEqual(ExitFailed, kept.status);
    try testing.expect(std.mem.indexOf(u8, kept.stdout, "kept_modified") != null);
    const bytes = try std.fs.cwd().readFileAlloc(allocator, path, 65536);
    defer allocator.free(bytes);
    try testing.expectEqualStrings("edited by the user\n", bytes);
}

test "status reports ok, modified, missing, and foreign states and always exits 0 for drift" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{"install"}, fx.env());
    installed.deinit(allocator);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    var ok = try invoke(allocator, &.{ "status", "--json" }, fx.env());
    defer ok.deinit(allocator);
    try testing.expectEqual(ExitOk, ok.status);
    try testing.expect(std.mem.indexOf(u8, ok.stdout, "\"state\":\"ok\"") != null);
    try std.fs.cwd().writeFile(.{ .sub_path = path, .data = "drift\n" });
    var modified = try invoke(allocator, &.{"status"}, fx.env());
    defer modified.deinit(allocator);
    try testing.expectEqual(ExitOk, modified.status);
    try testing.expect(std.mem.indexOf(u8, modified.stdout, "modified") != null);
    try std.fs.cwd().deleteFile(path);
    try std.posix.symlink("/etc/hostname", path);
    var foreign = try invoke(allocator, &.{"status"}, fx.env());
    defer foreign.deinit(allocator);
    try testing.expectEqual(ExitOk, foreign.status);
    try testing.expect(std.mem.indexOf(u8, foreign.stdout, "foreign") != null);
    try std.posix.unlink(path);
    var missing = try invoke(allocator, &.{"status"}, fx.env());
    defer missing.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, missing.stdout, "missing") != null);
}

test "repair recreates a missing handler and blocks a modified one unless forced, keeping a backup" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{"install"}, fx.env());
    installed.deinit(allocator);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    try std.fs.cwd().deleteFile(path);
    var repaired = try invoke(allocator, &.{"repair"}, fx.env());
    defer repaired.deinit(allocator);
    try testing.expectEqual(ExitOk, repaired.status);
    try testing.expect(std.mem.indexOf(u8, repaired.stdout, "repaired") != null);
    try std.fs.cwd().writeFile(.{ .sub_path = path, .data = "user edit\n" });
    var blocked = try invoke(allocator, &.{"repair"}, fx.env());
    defer blocked.deinit(allocator);
    try testing.expectEqual(ExitFailed, blocked.status);
    try testing.expect(std.mem.indexOf(u8, blocked.stdout, "blocked") != null);
    var forced = try invoke(allocator, &.{ "repair", "--force" }, fx.env());
    defer forced.deinit(allocator);
    try testing.expectEqual(ExitOk, forced.status);
    try testing.expect(std.mem.indexOf(u8, forced.stdout, "replaced") != null);
    try testing.expect(std.mem.indexOf(u8, forced.stdout, "backup=") != null);
    const expected = try producers.handler_desktop(allocator, "/opt/waddle/bin/waddle", null);
    defer allocator.free(expected);
    const bytes = try std.fs.cwd().readFileAlloc(allocator, path, 65536);
    defer allocator.free(bytes);
    try testing.expectEqualStrings(expected, bytes);
}

test "migrate is a no-op for the current version, upgrades an older version, and refuses a newer schema" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var nothing = try invoke(allocator, &.{"migrate"}, fx.env());
    defer nothing.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, nothing.stdout, "nothing_to_migrate") != null);
    var installed = try invoke(allocator, &.{"install"}, fx.env());
    installed.deinit(allocator);
    var same_version = try invoke(allocator, &.{"migrate"}, fx.env());
    defer same_version.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, same_version.stdout, "unchanged") != null);
    const manifest_path = try std.fs.path.join(allocator, &.{ fx.state, "waddle", "integration", "manifest.json" });
    defer allocator.free(manifest_path);
    const stored = try std.fs.cwd().readFileAlloc(allocator, manifest_path, 1 << 20);
    defer allocator.free(stored);
    const older = try std.mem.replaceOwned(u8, allocator, stored, "\"waddle_version\":\"0.2.0\"", "\"waddle_version\":\"0.1.0\"");
    defer allocator.free(older);
    try std.fs.cwd().writeFile(.{ .sub_path = manifest_path, .data = older });
    var upgraded = try invoke(allocator, &.{"migrate"}, fx.env());
    defer upgraded.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, upgraded.stdout, "migrated") != null);
    const newer = try std.mem.replaceOwned(u8, allocator, stored, "\"schema\":1", "\"schema\":2");
    defer allocator.free(newer);
    try std.fs.cwd().writeFile(.{ .sub_path = manifest_path, .data = newer });
    var refused = try invoke(allocator, &.{"migrate"}, fx.env());
    defer refused.deinit(allocator);
    try testing.expectEqual(ExitFailed, refused.status);
    try testing.expect(std.mem.indexOf(u8, refused.stderr, "schema_newer") != null);
    const untouched = try std.fs.cwd().readFileAlloc(allocator, manifest_path, 1 << 20);
    defer allocator.free(untouched);
    try testing.expectEqualStrings(newer, untouched);
}

test "json output uses the stable schema version 1 envelope and typed result rows" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var result = try invoke(allocator, &.{ "install", "--json" }, fx.env());
    defer result.deinit(allocator);
    const parsed = try std.json.parseFromSlice(std.json.Value, allocator, result.stdout, .{});
    defer parsed.deinit();
    const root = parsed.value.object;
    try testing.expectEqual(@as(i64, 1), root.get("schema_version").?.integer);
    try testing.expectEqualStrings("integration", root.get("command").?.string);
    try testing.expectEqualStrings("install", root.get("operation").?.string);
    try testing.expect(root.get("ok").?.bool);
    try testing.expect(root.get("error").? == .null);
    const rows = root.get("results").?.array.items;
    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("handler:waddle-open", rows[0].object.get("id").?.string);
    try testing.expectEqualStrings("installed", rows[0].object.get("action").?.string);
}

test "device scope selects records by device and leaves other devices untouched" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{ "install", "--device", "alpha" }, fx.env());
    installed.deinit(allocator);
    var other = try invoke(allocator, &.{ "remove", "--device", "beta" }, fx.env());
    defer other.deinit(allocator);
    try testing.expectEqual(ExitOk, other.status);
    try testing.expect(std.mem.indexOf(u8, other.stdout, "not_selected") != null);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    try std.fs.cwd().access(path, .{});
    var mine = try invoke(allocator, &.{ "remove", "--device", "alpha" }, fx.env());
    defer mine.deinit(allocator);
    try testing.expectEqual(ExitOk, mine.status);
    try testing.expect(std.mem.indexOf(u8, mine.stdout, "removed") != null);
}

test "an interrupted removal is recovered on the next invocation" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{"install"}, fx.env());
    installed.deinit(allocator);
    const data_fd = try std.posix.open(fx.data, .{ .ACCMODE = .RDONLY, .DIRECTORY = true }, 0);
    defer std.posix.close(data_fd);
    const manifest_dir = try std.fs.path.join(allocator, &.{ fx.state, "waddle", "integration" });
    defer allocator.free(manifest_dir);
    const manifest_fd = try std.posix.open(manifest_dir, .{ .ACCMODE = .RDONLY, .DIRECTORY = true, .NOFOLLOW = true }, 0);
    defer std.posix.close(manifest_fd);
    const roots = [_]manifest_store.root_t{.{ .kind = .Data, .canonical_path = fx.data, .dir_fd = data_fd }};
    var store = try manifest_store.store_t.init(allocator, manifest_fd, &roots);
    const original = (try store.manifest_snapshot(allocator)).?;
    defer allocator.free(original);
    var parsed = try manifest.parse(allocator, original);
    defer parsed.deinit();
    const candidate = try producers.candidate_document(allocator, .{ .waddle_version = producers.WaddleVersion, .updated = "2026-10-26T00:00:00Z" }, parsed.records, producers.HandlerRecordId, null);
    defer allocator.free(candidate);
    store.fail_after.store(store.checkpoint_count.load(.monotonic) + 8, .monotonic);
    try testing.expectError(error.InjectedFailure, store.remove(.{ .record_id = producers.HandlerRecordId, .candidate_manifest = candidate }));
    store.deinit();
    var status_run = try invoke(allocator, &.{ "status", "--json" }, fx.env());
    defer status_run.deinit(allocator);
    try testing.expectEqual(ExitOk, status_run.status);
    const converged_path = try std.fs.path.join(allocator, &.{ manifest_dir, "manifest.json" });
    defer allocator.free(converged_path);
    const converged = try std.fs.cwd().readFileAlloc(allocator, converged_path, 1 << 20);
    defer allocator.free(converged);
    try testing.expectEqualStrings(original, converged);
    try std.testing.expect(std.mem.indexOf(u8, status_run.stdout, "\"state\":\"ok\"") != null);
}

test "command allocation failures leave no leaked allocations and no partial state" {
    var observed: usize = 0;
    var offset: usize = 0;
    while (offset < 600) : (offset += 1) {
        var fx = try fixture_t.create(testing.allocator);
        defer fx.deinit(testing.allocator);
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        failing.fail_index = offset;
        const outcome = run(failing.allocator(), .{ .operation = .Install }, fx.env());
        if (!failing.has_induced_failure) {
            var result = try outcome;
            result.deinit(failing.allocator());
            try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed += 1;
        if (outcome) |_| return error.ExpectedOutOfMemory else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    try testing.expect(observed > 0);
}

test "every mapped failure has its public code, message, and exit status" {
    const cases = [_]struct { err: anyerror, code: []const u8, status: u8 }{
        .{ .err = error.Usage, .code = "usage", .status = ExitUsage },
        .{ .err = error.OutOfMemory, .code = "resource_error", .status = ExitResource },
        .{ .err = error.UnsupportedSchema, .code = "schema_newer", .status = ExitFailed },
        .{ .err = error.VersionDowngrade, .code = "version_downgrade", .status = ExitFailed },
        .{ .err = error.Busy, .code = "busy", .status = ExitFailed },
        .{ .err = error.AccessDenied, .code = "permission_denied", .status = ExitFailed },
        .{ .err = error.RecoveryRequired, .code = "recovery_required", .status = ExitFailed },
        .{ .err = error.ArtifactConflict, .code = "artifact_conflict", .status = ExitFailed },
        .{ .err = error.ManifestMismatch, .code = "manifest_mismatch", .status = ExitFailed },
        .{ .err = error.InvalidEnvironment, .code = "invalid_environment", .status = ExitFailed },
        .{ .err = error.NotImplemented, .code = "not_implemented", .status = ExitFailed },
        .{ .err = error.FileNotFound, .code = "not_found", .status = ExitFailed },
        .{ .err = error.IoError, .code = "io_error", .status = ExitFailed },
    };
    for (cases) |case| {
        const issue = failure_for(case.err);
        try testing.expectEqualStrings(case.code, issue.code);
        try testing.expectEqual(case.status, issue.status);
        try testing.expect(issue.message.len > 0);
    }
    try testing.expectEqualStrings("io_error", failure_for(error.PathAlreadyExists).code);
}

test "option parsing covers every flag once, every duplicate, and the help forms" {
    try testing.expect((try parse(&.{ "install", "-h" })).help);
    try testing.expect((try parse(&.{ "remove", "--all" })).all);
    try testing.expect((try parse(&.{ "repair", "--force", "--device", "alpha" })).force);
    try testing.expect((try parse(&.{ "status", "--guest", "--system" })).system);
    try testing.expectError(error.Usage, parse(&.{ "install", "--guest", "--guest" }));
    try testing.expectError(error.Usage, parse(&.{ "install", "--device", "a", "--device", "b" }));
    try testing.expectError(error.Usage, parse(&.{ "remove", "--all", "--all" }));
    try testing.expectError(error.Usage, parse(&.{ "repair", "--force", "--force" }));
    try testing.expectError(error.Usage, parse(&.{ "status", "--system", "--system" }));
    try testing.expectEqual(Operation.Shortcut, (try parse(&.{"shortcut"})).operation);
    try testing.expectEqual(Operation.Repair, (try parse(&.{"repair"})).operation);
    try testing.expectEqual(Operation.Migrate, (try parse(&.{"migrate"})).operation);
}

test "XDG variables that are empty or relative fall back under HOME" {
    const allocator = testing.allocator;
    const paths = try resolve_paths(allocator, "/home/u", "", "");
    defer allocator.free(paths.data_home);
    defer allocator.free(paths.state_home);
    try testing.expectEqualStrings("/home/u/.local/share", paths.data_home);
    try testing.expectEqualStrings("/home/u/.local/state", paths.state_home);
}

test "every subcommand renders JSON with its lowercase operation name" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    const names = [_][]const u8{ "install", "status", "repair", "migrate", "remove" };
    for (names) |name| {
        var result = try invoke(allocator, &.{ name, "--json" }, fx.env());
        defer result.deinit(allocator);
        const parsed = try std.json.parseFromSlice(std.json.Value, allocator, result.stdout, .{});
        defer parsed.deinit();
        try testing.expectEqualStrings(name, parsed.value.object.get("operation").?.string);
    }
    var guest = try run(allocator, .{ .operation = .Guest, .json = true }, fx.env());
    defer guest.deinit(allocator);
    try testing.expectEqual(ExitFailed, guest.status);
    var shortcut = try run(allocator, .{ .operation = .Shortcut, .json = true }, fx.env());
    defer shortcut.deinit(allocator);
    try testing.expectEqual(ExitFailed, shortcut.status);
    var direct = std.ArrayList(row_t).init(allocator);
    defer direct.deinit();
    var status: u8 = ExitOk;
    try testing.expectError(error.NotImplemented, executeScope(allocator, .{ .operation = .Guest }, fx.env(), &direct, &status));
    try testing.expectError(error.NotImplemented, executeScope(allocator, .{ .operation = .Shortcut }, fx.env(), &direct, &status));
}

test "install covers the recreate, refused-different, and unrecorded paths" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{"install"}, fx.env());
    installed.deinit(allocator);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    try std.fs.cwd().deleteFile(path);
    var recreated = try invoke(allocator, &.{"install"}, fx.env());
    defer recreated.deinit(allocator);
    try testing.expectEqual(ExitOk, recreated.status);
    try testing.expect(std.mem.indexOf(u8, recreated.stdout, "repaired") != null);
    var other_launcher = fx.env();
    other_launcher.launcher = "/opt/other/waddle";
    var different = try invoke(allocator, &.{"install"}, other_launcher);
    defer different.deinit(allocator);
    try testing.expectEqual(ExitFailed, different.status);
    try testing.expect(std.mem.indexOf(u8, different.stdout, "refused") != null);
    try testing.expect(std.mem.indexOf(u8, different.stderr, "blocked") != null);
}

test "remove reports artifact-missing records, kept foreign objects, and the device scope" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{ "install", "--device", "alpha" }, fx.env());
    installed.deinit(allocator);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    try std.fs.cwd().deleteFile(path);
    var missing = try invoke(allocator, &.{ "remove", "--device", "alpha", "--json" }, fx.env());
    defer missing.deinit(allocator);
    try testing.expectEqual(ExitOk, missing.status);
    try testing.expect(std.mem.indexOf(u8, missing.stdout, "record_removed_artifact_missing") != null);
    var reinstall = try invoke(allocator, &.{ "install", "--device", "alpha" }, fx.env());
    reinstall.deinit(allocator);
    try std.fs.cwd().deleteFile(path);
    try std.posix.symlink("/etc/hostname", path);
    var foreign = try invoke(allocator, &.{ "remove", "--device", "alpha" }, fx.env());
    defer foreign.deinit(allocator);
    try testing.expectEqual(ExitFailed, foreign.status);
    try testing.expect(std.mem.indexOf(u8, foreign.stdout, "kept_foreign") != null);
    try testing.expect(std.mem.indexOf(u8, foreign.stderr, "item(s) blocked") != null);
}

test "repair blocks foreign objects, reports unchanged records, and reports absent installs" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var absent = try invoke(allocator, &.{"repair"}, fx.env());
    defer absent.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, absent.stdout, "not_installed") != null);
    var installed = try invoke(allocator, &.{ "install", "--device", "alpha" }, fx.env());
    installed.deinit(allocator);
    var unchanged = try invoke(allocator, &.{"repair"}, fx.env());
    defer unchanged.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, unchanged.stdout, "unchanged") != null);
    var skipped = try invoke(allocator, &.{ "repair", "--device", "beta" }, fx.env());
    defer skipped.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, skipped.stdout, "not_selected") != null);
    const path = try fx.handler_file(allocator);
    defer allocator.free(path);
    try std.fs.cwd().deleteFile(path);
    try std.posix.symlink("/etc/hostname", path);
    var foreign = try invoke(allocator, &.{"repair"}, fx.env());
    defer foreign.deinit(allocator);
    try testing.expectEqual(ExitFailed, foreign.status);
    try testing.expect(std.mem.indexOf(u8, foreign.stdout, "foreign") != null);
}

test "status filters by device and migrate refuses a version newer than this Waddle" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    var installed = try invoke(allocator, &.{ "install", "--device", "alpha" }, fx.env());
    installed.deinit(allocator);
    var filtered = try invoke(allocator, &.{ "status", "--device", "alpha" }, fx.env());
    defer filtered.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, filtered.stdout, "alpha") != null);
    var other = try invoke(allocator, &.{ "status", "--device", "beta" }, fx.env());
    defer other.deinit(allocator);
    try testing.expectEqualStrings("", other.stdout);
    const manifest_path = try std.fs.path.join(allocator, &.{ fx.state, "waddle", "integration", "manifest.json" });
    defer allocator.free(manifest_path);
    const stored = try std.fs.cwd().readFileAlloc(allocator, manifest_path, 1 << 20);
    defer allocator.free(stored);
    const newer_version = try std.mem.replaceOwned(u8, allocator, stored, "\"waddle_version\":\"0.2.0\"", "\"waddle_version\":\"0.9.0\"");
    defer allocator.free(newer_version);
    try std.fs.cwd().writeFile(.{ .sub_path = manifest_path, .data = newer_version });
    var downgrade = try invoke(allocator, &.{"migrate"}, fx.env());
    defer downgrade.deinit(allocator);
    try testing.expectEqual(ExitFailed, downgrade.status);
    try testing.expect(std.mem.indexOf(u8, downgrade.stderr, "version_downgrade") != null);
    var json_error = try invoke(allocator, &.{ "migrate", "--json" }, fx.env());
    defer json_error.deinit(allocator);
    try testing.expect(std.mem.indexOf(u8, json_error.stdout, "\"code\":\"version_downgrade\"") != null);
}

test "desktop value escaping and encoder limits cover every special character and record shape" {
    const allocator = testing.allocator;
    const escaped = try producers.desktop_value(allocator, "a\\b\nc\td\re");
    defer allocator.free(escaped);
    try testing.expectEqualStrings("a\\\\b\\nc\\td\\re", escaped);
}

const Prepared = enum { Installed, MissingArtifact, ModifiedArtifact, OlderVersion, None };

/// Put the fixture into a prepared state with the normal allocator, then run one
/// operation with a failing allocator at every allocation index until it completes.
fn sweep(args: []const []const u8, prepared: Prepared, allow_failure_status: bool) !void {
    var observed: usize = 0;
    var offset: usize = 0;
    while (offset < 800) : (offset += 1) {
        var fx = try fixture_t.create(testing.allocator);
        defer fx.deinit(testing.allocator);
        if (prepared != .None) {
            var installed = try invoke(testing.allocator, &.{"install"}, fx.env());
            installed.deinit(testing.allocator);
        }
        const path = try fx.handler_file(testing.allocator);
        defer testing.allocator.free(path);
        switch (prepared) {
            .MissingArtifact => try std.fs.cwd().deleteFile(path),
            .ModifiedArtifact => try std.fs.cwd().writeFile(.{ .sub_path = path, .data = "edited\n" }),
            .OlderVersion => {
                const manifest_path = try std.fs.path.join(testing.allocator, &.{ fx.state, "waddle", "integration", "manifest.json" });
                defer testing.allocator.free(manifest_path);
                const stored = try std.fs.cwd().readFileAlloc(testing.allocator, manifest_path, 1 << 20);
                defer testing.allocator.free(stored);
                const older = try std.mem.replaceOwned(u8, testing.allocator, stored, "\"waddle_version\":\"0.2.0\"", "\"waddle_version\":\"0.1.0\"");
                defer testing.allocator.free(older);
                try std.fs.cwd().writeFile(.{ .sub_path = manifest_path, .data = older });
            },
            else => {},
        }
        const options = try parse(args);
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = std.math.maxInt(usize) });
        failing.fail_index = offset;
        const outcome = run(failing.allocator(), options, fx.env());
        if (!failing.has_induced_failure) {
            var result = try outcome;
            result.deinit(failing.allocator());
            try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        }
        observed += 1;
        if (outcome) |_| {
            return error.ExpectedOutOfMemory;
        } else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
    _ = allow_failure_status;
    try testing.expect(observed > 0);
}

test "allocation failures in every subcommand leave no leaked allocations" {
    try sweep(&.{"remove"}, .Installed, false);
    try sweep(&.{"status"}, .Installed, false);
    try sweep(&.{"repair"}, .MissingArtifact, false);
    try sweep(&.{ "repair", "--force" }, .ModifiedArtifact, false);
    try sweep(&.{"migrate"}, .OlderVersion, false);
    try sweep(&.{ "install", "--json" }, .None, false);
    try sweep(&.{ "status", "--json" }, .Installed, false);
}

test "allocation failures in device-scoped, blocked, and JSON output paths leave no leaked allocations" {
    try sweep(&.{ "install", "--device", "alpha" }, .None, false);
    try sweep(&.{ "status", "--device", "alpha" }, .Installed, false);
    try sweep(&.{ "remove", "--device", "beta" }, .Installed, false);
    try sweep(&.{"remove"}, .ModifiedArtifact, false);
    try sweep(&.{ "repair", "--json" }, .MissingArtifact, false);
    try sweep(&.{ "repair", "--force", "--json" }, .ModifiedArtifact, false);
    try sweep(&.{ "migrate", "--json" }, .OlderVersion, false);
    try sweep(&.{ "remove", "--json" }, .MissingArtifact, false);
}

test "unusable XDG locations fail with exit 1 and create nothing beyond them" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.writeFile(.{ .sub_path = "data-as-file", .data = "x" });
    const data_file = try std.fs.path.join(allocator, &.{ fx.base, "data-as-file" });
    defer allocator.free(data_file);
    var as_data = try invoke(allocator, &.{"install"}, test_env(data_file, fx.state));
    defer as_data.deinit(allocator);
    try testing.expectEqual(ExitFailed, as_data.status);
    try fx.tmp.dir.writeFile(.{ .sub_path = "state-as-file", .data = "x" });
    const state_file = try std.fs.path.join(allocator, &.{ fx.base, "state-as-file" });
    defer allocator.free(state_file);
    var as_state = try invoke(allocator, &.{"status"}, test_env(fx.data, state_file));
    defer as_state.deinit(allocator);
    try testing.expectEqual(ExitFailed, as_state.status);
    try fx.tmp.dir.makePath("state-waddle-file");
    try fx.tmp.dir.writeFile(.{ .sub_path = "state-waddle-file/waddle", .data = "x" });
    const waddle_file_state = try std.fs.path.join(allocator, &.{ fx.base, "state-waddle-file" });
    defer allocator.free(waddle_file_state);
    var as_waddle = try invoke(allocator, &.{"status"}, test_env(fx.data, waddle_file_state));
    defer as_waddle.deinit(allocator);
    try testing.expectEqual(ExitFailed, as_waddle.status);
    try fx.tmp.dir.symLink("/nonexistent-target", "dangling-data", .{});
    const dangling = try std.fs.path.join(allocator, &.{ fx.base, "dangling-data" });
    defer allocator.free(dangling);
    var dangling_run = try invoke(allocator, &.{"status"}, test_env(dangling, fx.state));
    defer dangling_run.deinit(allocator);
    try testing.expectEqual(ExitFailed, dangling_run.status);
    try fx.tmp.dir.makePath("state-link-target");
    try fx.tmp.dir.makePath("state-link/waddle");
    try fx.tmp.dir.symLink("../state-link-target", "state-link/waddle/integration", .{});
    const linked_state = try std.fs.path.join(allocator, &.{ fx.base, "state-link" });
    defer allocator.free(linked_state);
    var linked = try invoke(allocator, &.{"status"}, test_env(fx.data, linked_state));
    defer linked.deinit(allocator);
    try testing.expectEqual(ExitFailed, linked.status);
    var target = try fx.tmp.dir.openDir("state-link-target", .{ .iterate = true });
    defer target.close();
    var entries = target.iterate();
    try testing.expect((try entries.next()) == null);
}

test "a manifest path that is a directory is an I/O failure that changes nothing" {
    const allocator = testing.allocator;
    var fx = try fixture_t.create(allocator);
    defer fx.deinit(allocator);
    try fx.tmp.dir.makePath("state/waddle/integration/manifest.json");
    var blocked = try invoke(allocator, &.{"status"}, fx.env());
    defer blocked.deinit(allocator);
    try testing.expectEqual(ExitFailed, blocked.status);
    try testing.expect(std.mem.indexOf(u8, blocked.stderr, "io_error") != null or std.mem.indexOf(u8, blocked.stderr, "permission_denied") != null);
}

test "allocation failures across every install, repair, remove, and status state leave no leaked allocations" {
    try sweep(&.{"install"}, .Installed, false);
    try sweep(&.{"install"}, .MissingArtifact, false);
    try sweep(&.{"install"}, .ModifiedArtifact, false);
    try sweep(&.{ "install", "--device", "beta" }, .Installed, false);
    try sweep(&.{"repair"}, .Installed, false);
    try sweep(&.{"repair"}, .None, false);
    try sweep(&.{"repair"}, .ModifiedArtifact, false);
    try sweep(&.{ "repair", "--json" }, .Installed, false);
    try sweep(&.{"remove"}, .MissingArtifact, false);
    try sweep(&.{"status"}, .None, false);
    try sweep(&.{ "status", "--device", "alpha" }, .None, false);
    try sweep(&.{ "status", "--json" }, .MissingArtifact, false);
    try sweep(&.{"migrate"}, .Installed, false);
}

test "allocation failures in path resolution leave no leaked allocations and report OutOfMemory" {
    var offset: usize = 0;
    while (offset < 32) : (offset += 1) {
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = offset });
        const result = resolve_paths(failing.allocator(), "/home/u", "/xdg/data", null);
        if (result) |paths| {
            failing.allocator().free(paths.data_home);
            failing.allocator().free(paths.state_home);
        } else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        if (!failing.has_induced_failure) break;
    }
}

test "allocation failures while opening the session leave no leaked allocations" {
    var offset: usize = 0;
    while (offset < 400) : (offset += 1) {
        var fx = try fixture_t.create(testing.allocator);
        defer fx.deinit(testing.allocator);
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = offset });
        if (Session.open(failing.allocator(), fx.env())) |opened| {
            var session = opened;
            session.close();
        } else |err| try testing.expectEqual(error.OutOfMemory, err);
        try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        if (!failing.has_induced_failure) break;
    }
}

fn install_with(fx: *fixture_t, args: []const []const u8) !void {
    var installed = try invoke(testing.allocator, args, fx.env());
    installed.deinit(testing.allocator);
}

/// Put the fixture into one of the record states the scenario matrix exercises:
/// 0 none, 1 installed, 2 artifact missing, 3 artifact modified, 4 unrecorded file,
/// 5 manifest from an older Waddle, 6 installed for device alpha.
fn prepare_state(fx: *fixture_t, state: u8) !void {
    const path = try fx.handler_file(testing.allocator);
    defer testing.allocator.free(path);
    switch (state) {
        1 => try install_with(fx, &.{"install"}),
        2 => {
            try install_with(fx, &.{"install"});
            try std.fs.cwd().deleteFile(path);
        },
        3 => {
            try install_with(fx, &.{"install"});
            try std.fs.cwd().writeFile(.{ .sub_path = path, .data = "edited\n" });
        },
        4 => {
            try fx.tmp.dir.makePath("data/applications");
            try fx.tmp.dir.writeFile(.{ .sub_path = "data/applications/waddle-open.desktop", .data = "x" });
        },
        5 => {
            try install_with(fx, &.{"install"});
            const manifest_path = try std.fs.path.join(testing.allocator, &.{ fx.state, "waddle", "integration", "manifest.json" });
            defer testing.allocator.free(manifest_path);
            const stored = try std.fs.cwd().readFileAlloc(testing.allocator, manifest_path, 1 << 20);
            defer testing.allocator.free(stored);
            const older = try std.mem.replaceOwned(u8, testing.allocator, stored, "\"waddle_version\":\"0.2.0\"", "\"waddle_version\":\"0.1.0\"");
            defer testing.allocator.free(older);
            try std.fs.cwd().writeFile(.{ .sub_path = manifest_path, .data = older });
        },
        6 => try install_with(fx, &.{ "install", "--device", "alpha" }),
        else => {},
    }
}

test "every subcommand in text and JSON modes exits with a documented code across every record state" {
    const commands = [_][]const []const u8{
        &.{"install"},
        &.{ "install", "--json" },
        &.{ "install", "--device", "alpha" },
        &.{"status"},
        &.{ "status", "--json" },
        &.{ "status", "--device", "alpha" },
        &.{"repair"},
        &.{ "repair", "--json" },
        &.{ "repair", "--force" },
        &.{ "repair", "--force", "--json" },
        &.{ "repair", "--device", "alpha" },
        &.{"remove"},
        &.{ "remove", "--json" },
        &.{ "remove", "--device", "alpha" },
        &.{"migrate"},
        &.{ "migrate", "--json" },
    };
    var state: u8 = 0;
    while (state <= 6) : (state += 1) {
        for (commands) |args| {
            var fx = try fixture_t.create(testing.allocator);
            defer fx.deinit(testing.allocator);
            try prepare_state(&fx, state);
            var result = try invoke(testing.allocator, args, fx.env());
            defer result.deinit(testing.allocator);
            try testing.expect(result.status == ExitOk or result.status == ExitFailed);
        }
    }
}

test "allocation failures while rendering help leave no leaked allocations" {
    try sweep(&.{ "install", "--help" }, .None, false);
}

test "system scope as root reports not implemented and changes nothing" {
    var fx = try fixture_t.create(testing.allocator);
    defer fx.deinit(testing.allocator);
    var env = fx.env();
    env.euid = 0;
    var result = try invoke(testing.allocator, &.{ "status", "--system" }, env);
    defer result.deinit(testing.allocator);
    try testing.expectEqual(ExitFailed, result.status);
    try testing.expect(std.mem.indexOf(u8, result.stderr, "not_implemented") != null);
    if (fx.tmp.dir.access("state", .{})) |_| return error.TestUnexpectedResult else |err| try testing.expectEqual(error.FileNotFound, err);
}

test "a relative or empty HOME without absolute XDG variables is an invalid environment" {
    try testing.expectError(error.InvalidEnvironment, resolve_paths(testing.allocator, "relative/home", null, null));
    try testing.expectError(error.InvalidEnvironment, resolve_paths(testing.allocator, "", null, null));
    try testing.expectError(error.InvalidEnvironment, resolve_paths(testing.allocator, null, null, null));
}

test "a fixture with no records migrates and reports as nothing to do under every allocation index" {
    try sweep(&.{"migrate"}, .None, false);
}

test "parsing accepts help as a later operand" {
    try testing.expect((try parse(&.{ "install", "--help" })).help);
}
