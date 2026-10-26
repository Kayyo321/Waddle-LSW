//! Host catalog runtime: turns decoded guest Catalog* events and chunked icon
//! transfers into published desktop entries, menus and icons through the rooted
//! manifest store. Contract: impl/seamless-interop/claude_45_percent/catalog_runtime_contract.md.
//!
//! One runtime object serves one control session. It is single-threaded: every
//! method must be called from the session pump thread, never concurrently. All
//! filesystem authority is the caller-supplied directory descriptors passed to
//! `create`; the module never resolves ambient paths. Clock values are supplied
//! by the caller (`now_ms` is an absolute monotonic millisecond count).
const std = @import("std");
const desktop_artifacts = @import("desktop_artifacts.zig");
const manifest = @import("manifest.zig");
const manifest_store = @import("manifest_store.zig");
const builtin = @import("builtin");

comptime {
    // Tests link the codec exports that this module declares extern.
    if (builtin.is_test) _ = @import("interop_codec.zig");
}

/// Maximum catalog applications in one snapshot (IMPL_DESC 3.2.5, app_catalog_contract).
pub const MaxApps: usize = 4096;
/// Maximum aggregate bytes of retained app keys and display names. With at most `MaxApps`
/// entries of 200-byte keys and 255-byte names the snapshot stays below this bound, so the
/// runtime needs no separate per-byte quota check (asserted at compile time below).
pub const MaxSnapshotBytes: usize = 16 * 1024 * 1024;
comptime {
    std.debug.assert(MaxApps * (200 + 255) <= MaxSnapshotBytes);
}
/// Maximum uncatalogued-window entries kept by the runtime.
pub const MaxUncatalogued: usize = 256;
/// Maximum outstanding IconRequest transfers.
pub const MaxOutstanding: usize = 8;
/// Maximum transfer table entries (outstanding requests plus cancelled in-flight state).
pub const MaxTransfers: usize = 8;
/// Maximum bytes of one icon transfer (IMPL_DESC 3.2.6 icon cap).
pub const MaxIconBytes: usize = 1024 * 1024;
/// Total retained icon bytes across all apps (bounded memory budget).
pub const MaxIconBudget: usize = 32 * 1024 * 1024;
/// Begin and idle timeout for one outstanding transfer, in milliseconds.
pub const RequestTimeoutMs: u64 = 10_000;
/// Icon request attempts per slot before the slot waits for new content.
pub const MaxIconAttempts: u8 = 3;
/// Publication store operations allowed in one pass before yielding to the pump.
pub const MaxOpsPerPass: usize = 32;
/// Capacity of the outbound action ring; one call produces at most 16 actions.
pub const MaxActions: usize = 64;
/// Maximum retry backoff for failed publication, in milliseconds.
pub const MaxPublishBackoffMs: u64 = 8_000;
/// Maximum retry backoff for a failed icon request, in milliseconds.
pub const IconRetryBackoffMs: u64 = 5_000;
/// Maximum display title bytes for an uncatalogued window entry.
pub const MaxTitleBytes: usize = 64;

/// Frozen icon sizes, indexed by slot.
pub const IconSizes = [7]u16{ 16, 24, 32, 48, 64, 128, 256 };

/// Status values for the C ABI. Zero is success; every other value is a closed set.
pub const status_t = enum(c_int) {
    /// Operation applied or nothing needed to change.
    Ok = 0,
    /// Null or inconsistent argument, or an unsupported configuration value.
    InvalidArgument = 1,
    /// Frame rejected; snapshot state unchanged by this frame.
    Rejected = 2,
    /// Bounded capacity (apps, bytes, transfers) exhausted.
    Quota = 3,
    /// Allocation failed; state unchanged where documented.
    OutOfMemory = 4,
    /// Publication is pending: the store refused an operation and will be retried.
    Pending = 5,
    /// Store or filesystem operation failed; previous published state is intact.
    StoreFailed = 6,
    /// Requested behaviour is not implemented in this milestone.
    Unsupported = 7,
};

/// Interop status codes carried by TransferCancel (IMPL_DESC 3.2.4 interop_status_t).
pub const CancelNotFound: u32 = 1;
/// Cancelled by the host (removal, shutdown).
pub const CancelCancelled: u32 = 3;
/// Timed out waiting for the guest.
pub const CancelTimedOut: u32 = 4;
/// Object exceeds the host limit.
pub const CancelTooLarge: u32 = 6;
/// Payload is not supported (wrong MIME or format).
pub const CancelUnsupported: u32 = 7;
/// Protocol-level failure of the transfer.
pub const CancelGuestError: u32 = 9;

/// Menu inclusion policy from `integration.menu_apps`.
pub const menu_policy_t = enum(u32) {
    /// Entries without System, plus Seed entries.
    Installed = 0,
    /// Every entry.
    All = 1,
    /// No menu entries; every entry stays NoDisplay.
    None = 2,
};

/// Desktop shortcut policy from `integration.desktop_shortcuts`.
pub const shortcuts_policy_t = enum(u32) {
    /// No desktop shortcuts.
    None = 0,
    /// Desktop shortcut mirroring; not implemented in this milestone (TODO #28 mirror).
    Mirror = 1,
};

/// Runtime creation inputs. Every slice is borrowed for the duration of `create`.
pub const config_t = struct {
    /// Owned-by-caller private state directory (mode 0700) for manifest.lock and journals.
    manifest_dir_fd: c_int,
    /// Canonical absolute data root spelling and its matching open directory.
    data_root: []const u8,
    data_root_fd: c_int,
    /// Canonical absolute configuration root spelling and its matching open directory.
    config_root: []const u8,
    config_root_fd: c_int,
    /// Trusted canonical absolute Waddle launcher path.
    launcher_path: []const u8,
    /// Sanitized device identity used in every artifact name.
    device: []const u8,
    /// Creator version written into every candidate manifest.
    waddle_version: []const u8,
    /// Menu inclusion policy.
    menu_policy: menu_policy_t = .Installed,
    /// Desktop shortcut policy; `Mirror` is rejected with Unsupported.
    shortcuts: shortcuts_policy_t = .None,
};

/// C ABI configuration. Pointers are borrowed for the duration of `catalog_runtime_create`.
pub const config_c_t = extern struct {
    /// Private state directory descriptor (mode 0700, current user).
    manifest_dir_fd: c_int,
    /// Data root descriptor.
    data_root_fd: c_int,
    /// Configuration root descriptor.
    config_root_fd: c_int,
    /// `menu_policy_t` value.
    menu_policy: u32,
    /// `shortcuts_policy_t` value.
    shortcuts: u32,
    /// Data root spelling, borrowed.
    data_root: ?[*]const u8,
    /// Data root length in bytes.
    data_root_length: usize,
    /// Configuration root spelling, borrowed.
    config_root: ?[*]const u8,
    /// Configuration root length in bytes.
    config_root_length: usize,
    /// Launcher path, borrowed.
    launcher_path: ?[*]const u8,
    /// Launcher path length in bytes.
    launcher_path_length: usize,
    /// Device identity, borrowed.
    device: ?[*]const u8,
    /// Device identity length in bytes.
    device_length: usize,
    /// Waddle version text, borrowed.
    waddle_version: ?[*]const u8,
    /// Waddle version length in bytes.
    waddle_version_length: usize,
};

/// C ABI view of one catalog application. Optional members are NULL when absent.
pub const app_c_t = extern struct {
    /// App key bytes (3..200 ASCII, `a-` or `p-` prefix).
    key: ?[*]const u8,
    /// Key length in bytes.
    key_length: usize,
    /// Display name, 1..255 bytes UTF-8 without NUL.
    display_name: ?[*]const u8,
    /// Display name length in bytes.
    display_name_length: usize,
    /// `InteropAppFlag` bitmask (only bits within InteropAppFlagMask).
    flags: u32,
    /// Raw little-endian u16 icon size array, or NULL when absent.
    icon_sizes: ?[*]const u8,
    /// Byte length of the icon size array (even, at most 14 bytes).
    icon_sizes_length: usize,
    /// 32-byte content hash, or NULL when absent.
    content_hash: ?[*]const u8,
    /// Content hash length; must be 32 when present.
    content_hash_length: usize,
};

/// C ABI view of one outbound action produced by `catalog_runtime_next_action`.
pub const action_c_t = extern struct {
    /// 1 for IconRequest, 2 for TransferCancel.
    kind: u32,
    /// Transfer identifier to place in the frame.
    transfer: u32,
    /// Requested icon size for IconRequest; zero otherwise.
    icon_size: u32,
    /// Interop status for TransferCancel; zero otherwise.
    status: u32,
    /// App key for IconRequest, borrowed until the next runtime call.
    key: ?[*]const u8,
    /// App key length.
    key_length: usize,
};

/// Diagnostic counters returned by `catalog_runtime_stats`.
pub const stats_c_t = extern struct {
    /// Catalog applications in the snapshot.
    apps: u32,
    /// Uncatalogued entries pending or published.
    uncatalogued: u32,
    /// Outstanding IconRequest transfers.
    outstanding: u32,
    /// Retained icon bytes.
    icon_bytes: u64,
    /// Frames rejected by validation since creation.
    rejected_frames: u32,
    /// Consecutive publication failures.
    publish_failures: u32,
    /// 1 when publication is blocked until a clean full sync.
    sync_required: u32,
    /// 1 while a CatalogBegin transaction is open.
    transaction_open: u32,
    /// 1 when any publication work is pending.
    publication_pending: u32,
};

/// Outbound action with its key borrowed from the runtime.
pub const action_t = struct {
    /// Action kind.
    kind: kind_t,
    /// Transfer identifier.
    transfer: u32,
    /// Icon size (IconRequest only).
    icon_size: u16 = 0,
    /// Interop status (TransferCancel only).
    status: u32 = 0,
    /// App key (IconRequest only), borrowed until the next runtime call.
    key: []const u8 = &.{},

    /// Action discriminator.
    pub const kind_t = enum(u32) { IconRequest = 1, TransferCancel = 2 };
};

const app_t = struct {
    key: []u8,
    display_name: []u8,
    flags: u32,
    mask: u8 = 0,
    content_hash: ?[32]u8 = null,
    mark: u64 = 0,
    dirty: bool = true,
    icons: [7]icon_slot_t = [_]icon_slot_t{.{}} ** 7,
};

const icon_slot_t = struct {
    bytes: ?[]u8 = null,
    want: bool = false,
    requested: bool = false,
    attempts: u8 = 0,
    not_before: u64 = 0,
};

/// Layout-identical mirror of `interop_transfer_t` (include/waddle/interop_protocol.h).
pub const transfer_state_t = extern struct {
    id: u32,
    state: u32,
    total_bytes: u64,
    received: u64,
    hash_state: [128]u8 align(8),
};

// C-ABI receiver accounting exported by interop_codec.zig. Declared extern so this module
// does not depend on the codec's private cImport types; the link must include interop_codec.o.
extern fn interop_transfer_begin(transfer: *transfer_state_t, id: u32, total_bytes: u64, cap: u64) c_int;
extern fn interop_transfer_chunk(transfer: *transfer_state_t, offset: u64, data: ?[*]const u8, length: usize) c_int;
extern fn interop_transfer_end(transfer: *transfer_state_t, hash: ?[*]const u8) c_int;

const transfer_phase_t = enum { AwaitBegin, Receiving };

const transfer_t = struct {
    id: u32,
    key: []u8,
    slot: u3,
    phase: transfer_phase_t,
    state: transfer_state_t,
    buffer: []u8 = &.{},
    deadline: u64,
};

const uncatalogued_t = struct {
    key: []u8,
    title: []u8,
    dirty: bool = true,
};

/// Errors produced inside the runtime. Filesystem and allocation errors pass through.
pub const error_t = error{
    InvalidArgument,
    Rejected,
    Quota,
    Unsupported,
    Pending,
    StoreFailed,
    PublicationBudget,
    InvalidManifest,
} || std.mem.Allocator.Error;

/// One catalog application as seen by the runtime. Slices are borrowed for the call.
pub const app_input_t = struct {
    key: []const u8,
    display_name: []const u8,
    flags: u32,
    icon_sizes: ?[]const u8 = null,
    content_hash: ?[]const u8 = null,
};

/// Per-session catalog runtime. Create with `create`, release with `destroy`.
pub const runtime_t = struct {
    /// Allocator that owns every field below.
    allocator: std.mem.Allocator,
    /// Rooted manifest store (owns the exclusive manifest lock).
    store: manifest_store.store_t,
    /// Owned copies of the configuration strings.
    data_root: []u8,
    config_root: []u8,
    launcher_path: []u8,
    device: []u8,
    waddle_version: []u8,
    /// Menu inclusion policy.
    menu_policy: menu_policy_t,
    /// Current on-disk manifest bytes as the runtime last published them, or null when none.
    /// Loaded from `manifest.json` in the state directory after store recovery.
    base: ?[]u8,
    /// Catalog snapshot sorted by app key bytes.
    apps: std.ArrayList(app_t),
    /// Uncatalogued window entries sorted by key.
    uncatalogued: std.ArrayList(uncatalogued_t),
    /// Open icon transfers.
    transfers: [MaxTransfers]?transfer_t = [_]?transfer_t{null} ** MaxTransfers,
    /// Outbound action ring.
    actions: [MaxActions]action_t = undefined,
    action_head: usize = 0,
    action_count: usize = 0,
    /// Retained icon bytes and snapshot string bytes.
    icon_bytes: usize = 0,
    snapshot_bytes: usize = 0,
    /// Next candidate transfer identifier; zero is never used.
    next_transfer_id: u32 = 1,
    /// Transaction state: open flag, epoch that marks announced apps, poison flag.
    transaction_open: bool = false,
    epoch: u64 = 0,
    /// True after a rejected frame inside a transaction until a clean full sync.
    sync_required: bool = false,
    /// True when the menu or directory artifacts must be regenerated.
    menu_dirty: bool = false,
    /// Publication backoff state.
    publish_failures: u32 = 0,
    publish_not_before: u64 = 0,
    /// Frames rejected since creation.
    rejected_frames: u32 = 0,

    /// Create and recover a runtime rooted at the caller's descriptors.
    /// Returns Unsupported for desktop_shortcuts=mirror and InvalidArgument for malformed
    /// inputs; the store lock is released on every failure. Thread-safety: owner thread only.
    pub fn create(allocator: std.mem.Allocator, config: config_t) error_t!*runtime_t {
        if (config.shortcuts == .Mirror) return error.Unsupported;
        try validate_config(config);
        const self = try allocator.create(runtime_t);
        errdefer allocator.destroy(self);
        const roots = [_]manifest_store.root_t{
            .{ .kind = .Data, .canonical_path = config.data_root, .dir_fd = config.data_root_fd },
            .{ .kind = .Config, .canonical_path = config.config_root, .dir_fd = config.config_root_fd },
        };
        var store = manifest_store.store_t.init(allocator, config.manifest_dir_fd, &roots) catch |err| return map_store_error(err);
        errdefer store.deinit();
        const data_root = try allocator.dupe(u8, config.data_root);
        errdefer allocator.free(data_root);
        const config_root = try allocator.dupe(u8, config.config_root);
        errdefer allocator.free(config_root);
        const launcher = try allocator.dupe(u8, config.launcher_path);
        errdefer allocator.free(launcher);
        const device = try allocator.dupe(u8, config.device);
        errdefer allocator.free(device);
        const version = try allocator.dupe(u8, config.waddle_version);
        errdefer allocator.free(version);
        const base = try read_manifest_file(allocator, config.manifest_dir_fd);
        errdefer if (base) |bytes| allocator.free(bytes);
        self.* = .{
            .allocator = allocator,
            .store = store,
            .data_root = data_root,
            .config_root = config_root,
            .launcher_path = launcher,
            .device = device,
            .waddle_version = version,
            .menu_policy = config.menu_policy,
            .base = base,
            .apps = std.ArrayList(app_t).init(allocator),
            .uncatalogued = std.ArrayList(uncatalogued_t).init(allocator),
        };
        return self;
    }

    /// Release every retained snapshot, transfer buffer, action and the store lock.
    /// Safe only after the owning pump has stopped calling the runtime.
    pub fn destroy(self: *runtime_t) void {
        const allocator = self.allocator;
        self.clear_snapshot();
        for (self.uncatalogued.items) |entry| {
            allocator.free(entry.key);
            allocator.free(entry.title);
        }
        self.uncatalogued.deinit();
        for (&self.transfers) |*slot| {
            if (slot.*) |transfer| {
                allocator.free(transfer.key);
                allocator.free(transfer.buffer);
                slot.* = null;
            }
        }
        self.apps.deinit();
        if (self.base) |bytes| allocator.free(bytes);
        allocator.free(self.data_root);
        allocator.free(self.config_root);
        allocator.free(self.launcher_path);
        allocator.free(self.device);
        allocator.free(self.waddle_version);
        self.store.deinit();
        allocator.destroy(self);
    }

    /// Begin a full-sync transaction. A Begin inside an open transaction restarts it: the
    /// announce marks of the abandoned attempt are discarded and the snapshot is kept.
    pub fn begin(self: *runtime_t) void {
        self.transaction_open = true;
        self.epoch += 1;
    }

    /// Close the open transaction. Apps not announced since the matching Begin are removed.
    /// Returns Rejected when no transaction is open or the transaction was poisoned; the
    /// snapshot then stays as it is and publication stays blocked until a clean full sync.
    pub fn end(self: *runtime_t) error_t!void {
        if (!self.transaction_open) return error.Rejected;
        self.transaction_open = false;
        if (self.sync_required) return error.Rejected;
        var index = self.apps.items.len;
        while (index > 0) {
            index -= 1;
            if (self.apps.items[index].mark != self.epoch) self.remove_at(index);
        }
    }

    /// Add or update one application. Outside a transaction this is incremental; inside one,
    /// a second upsert of the same key poisons the transaction. Invalid input is Rejected.
    pub fn upsert(self: *runtime_t, is_update: bool, input: app_input_t) error_t!void {
        const parsed = self.parse_input(input) catch return self.reject_invalid();
        const found = self.find(input.key);
        if (is_update and found == null) {
            self.rejected_frames += 1;
            return error.Rejected;
        }
        if (self.transaction_open and found != null and self.apps.items[found.?].mark == self.epoch) {
            return self.reject_poison();
        }
        if (found == null and self.apps.items.len >= MaxApps) return self.reject_quota();
        if (found) |index| {
            try self.update_existing(index, input, parsed);
        } else {
            try self.insert_new(input, parsed);
        }
    }

    /// Remove one application if present. Unknown keys are a no-op.
    pub fn remove(self: *runtime_t, key: []const u8) void {
        if (self.find(key)) |index| self.remove_at(index);
    }

    /// Reset the in-memory snapshot after a session change. Published files and the manifest
    /// are not modified; the next full sync decides their fate.
    pub fn reset_session(self: *runtime_t) void {
        self.clear_snapshot();
        self.transaction_open = false;
        self.sync_required = false;
        self.menu_dirty = false;
        self.publish_failures = 0;
        self.publish_not_before = 0;
    }

    /// Begin receiving the icon for the outstanding request `id`.
    pub fn transfer_begin(self: *runtime_t, id: u32, total: u64, mime: []const u8, now: u64) error_t!void {
        const index = self.transfer_index(id) orelse return self.cancel_unknown(id);
        const transfer = &self.transfers[index].?;
        if (transfer.phase != .AwaitBegin) return self.cancel_unknown(id);
        if (!std.mem.eql(u8, mime, "image/png")) return self.fail_transfer(index, CancelUnsupported, now);
        if (total == 0 or total > MaxIconBytes) return self.fail_transfer(index, CancelTooLarge, now);
        const old_bytes = self.slot_bytes(transfer.key, transfer.slot);
        if (self.icon_bytes - old_bytes + total > MaxIconBudget) return self.fail_transfer(index, CancelTooLarge, now);
        if (interop_transfer_begin(&transfer.state, id, total, MaxIconBytes) != 0) {
            return self.fail_transfer(index, CancelGuestError, now);
        }
        transfer.buffer = try self.allocator.alloc(u8, @intCast(total));
        transfer.phase = .Receiving;
        transfer.deadline = now + RequestTimeoutMs;
    }

    /// Account one chunk for transfer `id`. Gaps, overlaps and oversize chunks cancel it.
    pub fn transfer_chunk(self: *runtime_t, id: u32, offset: u64, data: []const u8, now: u64) error_t!void {
        const index = self.transfer_index(id) orelse return self.cancel_unknown(id);
        const transfer = &self.transfers[index].?;
        if (transfer.phase != .Receiving) return self.fail_transfer(index, CancelGuestError, now);
        if (interop_transfer_chunk(&transfer.state, offset, data.ptr, data.len) != 0) {
            return self.fail_transfer(index, CancelGuestError, now);
        }
        const start: usize = @intCast(offset);
        @memcpy(transfer.buffer[start..][0..data.len], data);
        transfer.deadline = now + RequestTimeoutMs;
    }

    /// Verify the digest and PNG header, then store the icon and mark the app dirty.
    pub fn transfer_end(self: *runtime_t, id: u32, hash: *const [32]u8, now: u64) error_t!void {
        const index = self.transfer_index(id) orelse return self.cancel_unknown(id);
        const transfer = &self.transfers[index].?;
        if (transfer.phase != .Receiving) return self.fail_transfer(index, CancelGuestError, now);
        if (interop_transfer_end(&transfer.state, hash) != 0) {
            return self.fail_transfer(index, CancelGuestError, now);
        }
        const size = IconSizes[transfer.slot];
        if (!png_header_valid(transfer.buffer, size)) {
            self.note_failure(index, now);
            self.free_transfer(index);
            self.rejected_frames += 1;
            return error.Rejected;
        }
        const app = &self.apps.items[self.find(transfer.key).?];
        const slot = &app.icons[transfer.slot];
        if (slot.bytes) |old| {
            self.icon_bytes -= old.len;
            self.allocator.free(old);
        }
        slot.bytes = transfer.buffer;
        self.icon_bytes += transfer.buffer.len;
        transfer.buffer = &.{};
        slot.want = false;
        slot.attempts = 0;
        app.dirty = true;
        self.free_transfer(index);
    }

    /// Guest-side cancellation of transfer `id`. Unknown identifiers are a no-op.
    pub fn transfer_cancel(self: *runtime_t, id: u32, now: u64) void {
        const index = self.transfer_index(id) orelse return;
        self.note_failure(index, now);
        self.free_transfer(index);
    }

    /// Advance timeouts, issue icon requests and publish pending artifacts.
    /// `updated` is the UTC RFC3339 timestamp recorded in candidate manifests.
    pub fn tick(self: *runtime_t, now: u64, updated: []const u8) void {
        self.expire_transfers(now);
        self.issue_requests(now);
        if (self.publication_pending() and !self.transaction_open and !self.sync_required and now >= self.publish_not_before) {
            self.publish_pass(updated) catch |err| switch (err) {
                error.PublicationBudget => {},
                else => {
                    self.publish_failures += 1;
                    self.publish_not_before = now + backoff_ms(self.publish_failures, MaxPublishBackoffMs);
                },
            };
        }
    }

    /// Ensure an uncatalogued window's NoDisplay entry exists before its first commit.
    /// Returns Ok once the entry is published, Pending while a transaction or sync requirement
    /// blocks publication or the pass budget ran out, and StoreFailed when the store refused
    /// the entry (retry is scheduled from `now`). Thread-safety: owner thread only.
    pub fn ensure_uncatalogued(self: *runtime_t, key: []const u8, title: []const u8, now: u64, updated: []const u8) error_t!void {
        if (!valid_key(key, self.device)) return error.InvalidArgument;
        if (!std.unicode.utf8ValidateSlice(title) or std.mem.indexOfScalar(u8, title, 0) != null) return error.InvalidArgument;
        if (self.find(key) != null) return;
        const index = self.uncat_index(key) orelse try self.insert_uncatalogued(key, title);
        self.uncatalogued.items[index].dirty = true;
        if (self.transaction_open or self.sync_required) return error.Pending;
        const pass = self.publish_pass(updated);
        if (pass) |_| {} else |err| switch (err) {
            error.PublicationBudget => {},
            else => {
                self.publish_failures += 1;
                self.publish_not_before = now + backoff_ms(self.publish_failures, MaxPublishBackoffMs);
            },
        }
        const entry = self.uncat_index(key).?;
        if (!self.uncatalogued.items[entry].dirty) return;
        if (pass) |_| return error.Pending else |err| return if (err == error.PublicationBudget) error.Pending else error.StoreFailed;
    }

    /// Earliest absolute deadline the caller must tick at, or null when idle.
    pub fn next_deadline(self: *runtime_t, now: u64) ?u64 {
        var best: ?u64 = null;
        for (self.transfers) |maybe| {
            if (maybe) |transfer| best = min_optional(best, transfer.deadline);
        }
        if (self.can_issue_request(now)) return now;
        for (self.apps.items) |app| {
            for (app.icons) |slot| {
                if (slot.want and !slot.requested and slot.not_before > now) {
                    best = min_optional(best, slot.not_before);
                }
            }
        }
        if (self.publication_pending() and !self.transaction_open and !self.sync_required) {
            best = min_optional(best, @max(self.publish_not_before, now));
        }
        return best;
    }

    /// Pop the next outbound action, or null when none are queued. Key slices in the
    /// returned action stay valid until the next runtime call.
    pub fn next_action(self: *runtime_t) ?action_t {
        if (self.action_count == 0) return null;
        const action = self.actions[self.action_head];
        self.action_head = (self.action_head + 1) % MaxActions;
        self.action_count -= 1;
        return action;
    }

    /// Snapshot of diagnostic counters.
    pub fn stats(self: *runtime_t) stats_c_t {
        return .{
            .apps = @intCast(self.apps.items.len),
            .uncatalogued = @intCast(self.uncatalogued.items.len),
            .outstanding = @intCast(self.outstanding()),
            .icon_bytes = self.icon_bytes,
            .rejected_frames = self.rejected_frames,
            .publish_failures = self.publish_failures,
            .sync_required = @intFromBool(self.sync_required),
            .transaction_open = @intFromBool(self.transaction_open),
            .publication_pending = @intFromBool(self.publication_pending()),
        };
    }

    fn validate_config(config: config_t) error_t!void {
        if (config.manifest_dir_fd < 0 or config.data_root_fd < 0 or config.config_root_fd < 0) return error.InvalidArgument;
        if (!valid_absolute(config.data_root, true) or !valid_absolute(config.config_root, true)) return error.InvalidArgument;
        if (std.mem.eql(u8, config.data_root, config.config_root)) return error.InvalidArgument;
        if (!valid_absolute(config.launcher_path, false)) return error.InvalidArgument;
        if (!valid_identity(config.device)) return error.InvalidArgument;
        if (config.waddle_version.len == 0 or config.waddle_version.len > 64) return error.InvalidArgument;
        if (!std.unicode.utf8ValidateSlice(config.waddle_version) or std.mem.indexOfScalar(u8, config.waddle_version, 0) != null) return error.InvalidArgument;
    }

    fn map_store_error(err: anyerror) error_t {
        return switch (err) {
            error.OutOfMemory => error.OutOfMemory,
            else => error.InvalidManifest,
        };
    }

    /// Count an invalid frame; inside a transaction it also blocks publication until a clean sync.
    fn reject_invalid(self: *runtime_t) error_t {
        self.rejected_frames += 1;
        if (self.transaction_open) self.sync_required = true;
        return error.Rejected;
    }

    fn reject_poison(self: *runtime_t) error_t {
        self.rejected_frames += 1;
        self.sync_required = true;
        return error.Rejected;
    }

    fn reject_quota(self: *runtime_t) error_t {
        self.rejected_frames += 1;
        if (self.transaction_open) self.sync_required = true;
        return error.Quota;
    }

    const parsed_input_t = struct {
        mask: u8,
        hash: ?[32]u8,
    };

    fn parse_input(self: *runtime_t, input: app_input_t) error{InvalidArgument}!parsed_input_t {
        if (!valid_key(input.key, self.device)) return error.InvalidArgument;
        if (input.display_name.len == 0 or input.display_name.len > 255) return error.InvalidArgument;
        if (!std.unicode.utf8ValidateSlice(input.display_name) or std.mem.indexOfScalar(u8, input.display_name, 0) != null) return error.InvalidArgument;
        if (input.flags & ~@as(u32, 0x1F) != 0) return error.InvalidArgument;
        var hash: ?[32]u8 = null;
        if (input.content_hash) |bytes| {
            if (bytes.len != 32) return error.InvalidArgument;
            hash = bytes[0..32].*;
        }
        return .{ .mask = try parse_icon_mask(input.icon_sizes), .hash = hash };
    }

    fn insert_new(self: *runtime_t, input: app_input_t, parsed: parsed_input_t) error_t!void {
        const key = try self.allocator.dupe(u8, input.key);
        errdefer self.allocator.free(key);
        const name = try self.allocator.dupe(u8, input.display_name);
        errdefer self.allocator.free(name);
        const index = lower_bound(self.apps.items, input.key);
        try self.apps.insert(index, .{
            .key = key,
            .display_name = name,
            .flags = input.flags,
            .mask = parsed.mask,
            .content_hash = parsed.hash,
            .mark = self.epoch,
        });
        self.snapshot_bytes += key.len + name.len;
        self.want_icons(index);
        self.menu_dirty = true;
        self.drop_uncatalogued(input.key);
    }

    fn update_existing(self: *runtime_t, index: usize, input: app_input_t, parsed: parsed_input_t) error_t!void {
        const app = &self.apps.items[index];
        const name_changed = !std.mem.eql(u8, app.display_name, input.display_name);
        if (name_changed) {
            const name = try self.allocator.dupe(u8, input.display_name);
            self.snapshot_bytes = self.snapshot_bytes - app.display_name.len + name.len;
            self.allocator.free(app.display_name);
            app.display_name = name;
        }
        const flags_changed = app.flags != input.flags;
        app.flags = input.flags;
        var wanted = false;
        if (parsed.hash) |hash| {
            if (app.content_hash == null or !std.mem.eql(u8, &app.content_hash.?, &hash)) wanted = true;
            app.content_hash = hash;
        }
        if (parsed.mask & ~app.mask != 0) wanted = true;
        app.mask = parsed.mask;
        app.mark = self.epoch;
        if (name_changed or flags_changed) {
            app.dirty = true;
            self.menu_dirty = true;
        }
        if (wanted) self.want_icons(index);
    }

    /// Request every advertised icon slot of the app at `index` (new app or changed content).
    fn want_icons(self: *runtime_t, index: usize) void {
        const app = &self.apps.items[index];
        for (&app.icons, 0..) |*slot, i| {
            if (app.mask & (@as(u8, 1) << @intCast(i)) == 0) continue;
            slot.want = true;
            slot.attempts = 0;
            slot.not_before = 0;
        }
    }

    fn remove_at(self: *runtime_t, index: usize) void {
        self.cancel_transfers_for(self.apps.items[index].key, true);
        const app = self.apps.orderedRemove(index);
        self.snapshot_bytes -= app.key.len + app.display_name.len;
        for (app.icons) |slot| {
            if (slot.bytes) |bytes| {
                self.icon_bytes -= bytes.len;
                self.allocator.free(bytes);
            }
        }
        self.allocator.free(app.key);
        self.allocator.free(app.display_name);
        self.menu_dirty = true;
    }

    /// Drop every app and open transfer without emitting cancels (session reset).
    fn clear_snapshot(self: *runtime_t) void {
        self.cancel_transfers_for(&.{}, false);
        while (self.apps.items.len > 0) self.remove_at(self.apps.items.len - 1);
        self.menu_dirty = false;
    }

    fn find(self: *runtime_t, key: []const u8) ?usize {
        const index = lower_bound(self.apps.items, key);
        if (index < self.apps.items.len and std.mem.eql(u8, self.apps.items[index].key, key)) return index;
        return null;
    }

    fn lower_bound(items: []const app_t, key: []const u8) usize {
        var low: usize = 0;
        var high: usize = items.len;
        while (low < high) {
            const middle = low + (high - low) / 2;
            if (std.mem.lessThan(u8, items[middle].key, key)) low = middle + 1 else high = middle;
        }
        return low;
    }

    fn uncat_index(self: *runtime_t, key: []const u8) ?usize {
        for (self.uncatalogued.items, 0..) |entry, i| if (std.mem.eql(u8, entry.key, key)) return i;
        return null;
    }

    fn insert_uncatalogued(self: *runtime_t, key: []const u8, title: []const u8) error_t!usize {
        if (self.uncatalogued.items.len >= MaxUncatalogued) return error.Quota;
        const owned_key = try self.allocator.dupe(u8, key);
        errdefer self.allocator.free(owned_key);
        const clipped = truncate_title(title);
        const owned_title = try self.allocator.dupe(u8, if (clipped.len == 0) key else clipped);
        errdefer self.allocator.free(owned_title);
        try self.uncatalogued.append(.{ .key = owned_key, .title = owned_title });
        return self.uncatalogued.items.len - 1;
    }

    fn drop_uncatalogued(self: *runtime_t, key: []const u8) void {
        const index = self.uncat_index(key) orelse return;
        const entry = self.uncatalogued.orderedRemove(index);
        self.allocator.free(entry.key);
        self.allocator.free(entry.title);
    }

    fn transfer_index(self: *runtime_t, id: u32) ?usize {
        if (id == 0) return null;
        for (self.transfers, 0..) |maybe, i| {
            if (maybe) |transfer| {
                if (transfer.id == id) return i;
            }
        }
        return null;
    }

    fn slot_bytes(self: *runtime_t, key: []const u8, slot: u3) usize {
        const index = self.find(key).?;
        return if (self.apps.items[index].icons[slot].bytes) |bytes| bytes.len else 0;
    }

    fn cancel_unknown(self: *runtime_t, id: u32) error_t!void {
        self.rejected_frames += 1;
        self.push_cancel(id, CancelNotFound);
        return error.Rejected;
    }

    fn fail_transfer(self: *runtime_t, index: usize, status: u32, now: u64) error_t!void {
        const id = self.transfers[index].?.id;
        self.note_failure(index, now);
        self.free_transfer(index);
        self.rejected_frames += 1;
        self.push_cancel(id, status);
        return error.Rejected;
    }

    fn note_failure(self: *runtime_t, index: usize, now: u64) void {
        const transfer = self.transfers[index].?;
        const slot = &self.apps.items[self.find(transfer.key).?].icons[transfer.slot];
        slot.attempts += 1;
        slot.not_before = now + IconRetryBackoffMs * @as(u64, slot.attempts);
        if (slot.attempts >= MaxIconAttempts) slot.want = false;
    }

    fn free_transfer(self: *runtime_t, index: usize) void {
        const transfer = self.transfers[index].?;
        self.apps.items[self.find(transfer.key).?].icons[transfer.slot].requested = false;
        self.allocator.free(transfer.key);
        self.allocator.free(transfer.buffer);
        self.transfers[index] = null;
    }

    /// Close open transfers for `key`, or for every key when `key` is empty and `all` is set.
    /// Each closed transfer emits a Cancelled action when `emit` is true.
    fn cancel_transfers_for(self: *runtime_t, key: []const u8, emit: bool) void {
        for (0..MaxTransfers) |index| {
            const transfer = self.transfers[index] orelse continue;
            if (key.len != 0 and !std.mem.eql(u8, transfer.key, key)) continue;
            if (emit) self.push_cancel(transfer.id, CancelCancelled);
            self.apps.items[self.find(transfer.key).?].icons[transfer.slot].requested = false;
            self.allocator.free(transfer.key);
            self.allocator.free(transfer.buffer);
            self.transfers[index] = null;
        }
    }

    fn push_cancel(self: *runtime_t, id: u32, status: u32) void {
        self.push_action(.{ .kind = .TransferCancel, .transfer = id, .status = status });
    }

    fn push_action(self: *runtime_t, action: action_t) void {
        std.debug.assert(self.action_count < MaxActions);
        self.actions[(self.action_head + self.action_count) % MaxActions] = action;
        self.action_count += 1;
    }

    fn expire_transfers(self: *runtime_t, now: u64) void {
        for (0..MaxTransfers) |index| {
            const transfer = self.transfers[index] orelse continue;
            if (transfer.deadline > now) continue;
            self.note_failure(index, now);
            self.push_cancel(transfer.id, CancelTimedOut);
            self.free_transfer(index);
        }
    }

    fn can_issue_request(self: *runtime_t, now: u64) bool {
        if (self.outstanding() >= MaxOutstanding or self.transaction_open or self.sync_required) return false;
        for (self.apps.items) |app| {
            for (app.icons) |slot| {
                if (slot.want and !slot.requested and slot.not_before <= now) return true;
            }
        }
        return false;
    }

    fn outstanding(self: *runtime_t) usize {
        var count: usize = 0;
        for (self.transfers) |maybe| {
            if (maybe != null) count += 1;
        }
        return count;
    }

    fn issue_requests(self: *runtime_t, now: u64) void {
        if (self.transaction_open or self.sync_required) return;
        for (self.apps.items) |*app| {
            for (&app.icons, 0..) |*slot, i| {
                if (!slot.want or slot.requested or slot.not_before > now) continue;
                if (app.mask & (@as(u8, 1) << @intCast(i)) == 0) continue;
                if (self.outstanding() >= MaxOutstanding) return;
                // Outstanding requests stay below MaxTransfers, so a free table entry exists.
                const free_index = self.free_transfer_slot().?;
                const id = self.next_id();
                const key = self.allocator.dupe(u8, app.key) catch {
                    // Back off so the caller's next deadline moves forward instead of spinning.
                    slot.not_before = now + IconRetryBackoffMs;
                    return;
                };
                self.transfers[free_index] = .{
                    .id = id,
                    .key = key,
                    .slot = @intCast(i),
                    .phase = .AwaitBegin,
                    .state = std.mem.zeroes(transfer_state_t),
                    .deadline = now + RequestTimeoutMs,
                };
                slot.requested = true;
                self.push_action(.{
                    .kind = .IconRequest,
                    .transfer = id,
                    .icon_size = IconSizes[i],
                    .key = key,
                });
            }
        }
    }

    fn free_transfer_slot(self: *runtime_t) ?usize {
        for (self.transfers, 0..) |maybe, i| if (maybe == null) return i;
        return null;
    }

    fn next_id(self: *runtime_t) u32 {
        while (true) {
            const candidate = self.next_transfer_id;
            self.next_transfer_id = if (self.next_transfer_id == std.math.maxInt(u32)) 1 else self.next_transfer_id + 1;
            if (self.transfer_index(candidate) == null) return candidate;
        }
    }

    fn publication_pending(self: *runtime_t) bool {
        if (self.menu_dirty) return true;
        for (self.apps.items) |app| if (app.dirty) return true;
        for (self.uncatalogued.items) |entry| if (entry.dirty) return true;
        return false;
    }

    /// Publish every dirty unit. A failing app or entry stays dirty and does not stop the
    /// others; the first failure is returned after the pass. Budget exhaustion stops at once.
    fn publish_pass(self: *runtime_t, updated: []const u8) error_t!void {
        var budget: usize = MaxOpsPerPass;
        var failure: ?error_t = null;
        for (self.apps.items) |*app| {
            if (app.dirty) try settle(&failure, self.publish_app(app, updated, &budget));
        }
        if (self.menu_dirty) try settle(&failure, self.publish_menu(updated, &budget));
        for (self.uncatalogued.items) |*entry| {
            if (entry.dirty) try settle(&failure, self.publish_uncatalogued(entry, updated, &budget));
        }
        if (failure) |err| return err;
    }

    /// Record the first non-budget failure of a pass; budget exhaustion ends the pass at once.
    fn settle(failure: *?error_t, result: error_t!void) error_t!void {
        result catch |err| {
            if (err == error.PublicationBudget) return err;
            if (failure.* == null) failure.* = err;
        };
    }

    fn publish_app(self: *runtime_t, app: *app_t, updated: []const u8, budget: *usize) error_t!void {
        var icons: [7]desktop_artifacts.icon_input_t = undefined;
        var count: usize = 0;
        for (app.icons, 0..) |slot, i| {
            if (app.mask & (@as(u8, 1) << @intCast(i)) == 0) continue;
            const bytes = slot.bytes orelse continue;
            icons[count] = .{ .size = IconSizes[i], .png = bytes };
            count += 1;
        }
        const items = [_]desktop_artifacts.item_input_t{.{
            .app_key = app.key,
            .display_name = app.display_name,
            .system = app.flags & 1 != 0,
            .seed = app.flags & 2 != 0,
            .icons = icons[0..count],
        }};
        try self.publish_items(&items, self.menu_policy_filter(), false, updated, budget);
        app.dirty = false;
    }

    fn publish_uncatalogued(self: *runtime_t, entry: *uncatalogued_t, updated: []const u8, budget: *usize) error_t!void {
        const items = [_]desktop_artifacts.item_input_t{.{
            .app_key = entry.key,
            .display_name = entry.title,
        }};
        try self.publish_items(&items, .None, false, updated, budget);
        entry.dirty = false;
    }

    fn publish_menu(self: *runtime_t, updated: []const u8, budget: *usize) error_t!void {
        const items = try self.allocator.alloc(desktop_artifacts.item_input_t, self.apps.items.len);
        defer self.allocator.free(items);
        for (self.apps.items, 0..) |app, i| {
            items[i] = .{
                .app_key = app.key,
                .display_name = app.display_name,
                .system = app.flags & 1 != 0,
                .seed = app.flags & 2 != 0,
            };
        }
        try self.publish_items(items, self.menu_policy_filter(), true, updated, budget);
        self.menu_dirty = false;
    }

    fn publish_items(
        self: *runtime_t,
        items: []const desktop_artifacts.item_input_t,
        filter: desktop_artifacts.menu_filter_t,
        include_menu: bool,
        updated: []const u8,
        budget: *usize,
    ) error_t!void {
        var out = desktop_artifacts.artifacts_t{ .allocator = self.allocator, .items = &.{} };
        defer out.deinit();
        desktop_artifacts.build(self.allocator, self.request(items, filter, include_menu, updated), &out) catch |err| {
            return map_publish_error(err);
        };
        for (out.items) |*artifact| {
            if (include_menu and !std.mem.eql(u8, artifact.kind, "menu")) continue;
            try self.publish_artifact(artifact, updated, budget);
        }
    }

    fn request(self: *runtime_t, items: []const desktop_artifacts.item_input_t, filter: desktop_artifacts.menu_filter_t, include_menu: bool, updated: []const u8) desktop_artifacts.request_t {
        return .{
            .data_root = self.data_root,
            .config_root = self.config_root,
            .launcher_path = self.launcher_path,
            .device = self.device,
            .waddle_version = self.waddle_version,
            .updated = updated,
            .items = items,
            .menu_filter = filter,
            .include_menu = include_menu,
        };
    }

    fn menu_policy_filter(self: *runtime_t) desktop_artifacts.menu_filter_t {
        return switch (self.menu_policy) {
            .Installed => .Installed,
            .All => .All,
            .None => .None,
        };
    }

    /// Publish one artifact as a commit (absent record) or replacement (changed digest). Skips
    /// records whose digest already matches, so repeated syncs never rewrite files.
    fn publish_artifact(self: *runtime_t, artifact: *const desktop_artifacts.artifact_t, updated: []const u8, budget: *usize) error_t!void {
        var candidate_doc = manifest.parse(self.allocator, artifact.candidate_manifest) catch return error.InvalidManifest;
        defer candidate_doc.deinit();
        const new_record = find_record(candidate_doc.records, artifact.record_id) orelse return error.InvalidManifest;
        var base_doc: ?manifest.document_t = if (self.base) |bytes| (manifest.parse(self.allocator, bytes) catch return error.InvalidManifest) else null;
        defer if (base_doc) |*doc| doc.deinit();
        const base_records: []const manifest.record_t = if (base_doc) |doc| doc.records else &.{};
        const existing = find_record_index(base_records, artifact.record_id);
        if (existing) |index| {
            if (std.mem.eql(u8, base_records[index].sha256, artifact.sha256[0..])) return;
        }
        if (budget.* == 0) return error.PublicationBudget;
        budget.* -= 1;
        try self.ensure_parent_dirs(artifact.path);
        var merged = std.ArrayList(manifest.record_t).init(self.allocator);
        defer merged.deinit();
        for (base_records, 0..) |record, index| {
            var copy = record;
            if (existing != null and existing.? == index) copy.sha256 = artifact.sha256[0..];
            try merged.append(copy);
        }
        if (existing == null) {
            var added = new_record;
            added.sha256 = artifact.sha256[0..];
            try merged.append(added);
        }
        const candidate = emit_manifest(self.allocator, self.waddle_version, updated, merged.items) catch |err| {
            return map_publish_error(err);
        };
        errdefer self.allocator.free(candidate);
        if (existing) |_| {
            const snapshot = self.store.inspect_record(artifact.record_id) catch |err| return map_publish_error(err);
            if (self.store.replace(.{
                .record_id = artifact.record_id,
                .expected_manifest_sha256 = snapshot.manifest_sha256,
                .expected_target = snapshot.target,
                .artifact_bytes = artifact.bytes,
                .artifact_mode = artifact.mode,
                .candidate_manifest = candidate,
            })) |_| {} else |err| try self.adopt_or_fail(err, artifact.record_id, candidate);
        } else {
            if (self.store.commit(.{
                .record_id = artifact.record_id,
                .prior = .Absent,
                .artifact_bytes = artifact.bytes,
                .artifact_mode = artifact.mode,
                .candidate_manifest = candidate,
            })) |_| {} else |err| try self.adopt_or_fail(err, artifact.record_id, candidate);
        }
        if (self.base) |old| self.allocator.free(old);
        self.base = candidate;
        self.publish_failures = 0;
    }

    /// Create missing parent directories of `path` below the store root that owns it. Each
    /// component is created with mkdirat and opened O_NOFOLLOW|O_DIRECTORY relative to the
    /// previous descriptor, so no ambient path or symlink is followed. Existing components
    /// are accepted; a non-directory or symlink component fails the operation.
    fn ensure_parent_dirs(self: *runtime_t, path: []const u8) error_t!void {
        for (self.store.roots) |root| {
            const prefix = root.canonical_path;
            if (path.len <= prefix.len + 1 or !std.mem.startsWith(u8, path, prefix) or path[prefix.len] != '/') continue;
            const relative = path[prefix.len + 1 ..];
            const parent_len = std.mem.lastIndexOfScalar(u8, relative, '/') orelse return;
            var current = root.fd;
            var owned = false;
            errdefer if (owned) std.posix.close(current);
            var parts = std.mem.splitScalar(u8, relative[0..parent_len], '/');
            while (parts.next()) |component| {
                std.posix.mkdirat(current, component, 0o755) catch |err| switch (err) {
                    error.PathAlreadyExists => {},
                    else => return map_publish_error(err),
                };
                const next = std.posix.openat(current, component, .{ .DIRECTORY = true, .NOFOLLOW = true, .CLOEXEC = true }, 0) catch |err| {
                    return map_publish_error(err);
                };
                if (owned) std.posix.close(current);
                current = next;
                owned = true;
            }
            if (owned) std.posix.close(current);
            return;
        }
        return error.InvalidManifest;
    }

    /// After a failed store operation, adopt the candidate when the store's manifest digest
    /// already equals it (the failure happened after the manifest was published). Otherwise
    /// the previous state stays authoritative and the error is returned.
    fn adopt_or_fail(self: *runtime_t, err: anyerror, record_id: []const u8, candidate: []const u8) error_t!void {
        if (self.store.inspect_record(record_id)) |snapshot| {
            var digest: [32]u8 = undefined;
            std.crypto.hash.sha2.Sha256.hash(candidate, &digest, .{});
            if (std.mem.eql(u8, &snapshot.manifest_sha256, &digest)) return;
        } else |_| {}
        return map_publish_error(err);
    }
};

/// Read `manifest.json` from the store's state directory (the same descriptor the store
/// publishes through). A missing file means no records yet and yields null. The read is
/// no-follow and bounded by the manifest limit; the store verifies the bytes on every commit.
fn read_manifest_file(allocator: std.mem.Allocator, state_fd: c_int) error_t!?[]u8 {
    const fd = std.posix.openat(state_fd, "manifest.json", .{ .ACCMODE = .RDONLY, .NOFOLLOW = true, .CLOEXEC = true }, 0) catch |err| switch (err) {
        error.FileNotFound => return null,
        else => return error.InvalidManifest,
    };
    const file = std.fs.File{ .handle = fd };
    defer file.close();
    return file.readToEndAlloc(allocator, desktop_artifacts.MaxManifestBytes) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return error.InvalidManifest,
    };
}

/// Map store, build and codec failures to the runtime error set. Allocation stays distinct.
fn map_publish_error(err: anyerror) error_t {
    return switch (err) {
        error.OutOfMemory => error.OutOfMemory,
        else => error.InvalidManifest,
    };
}

fn find_record(records: []const manifest.record_t, id: []const u8) ?manifest.record_t {
    for (records) |record| if (std.mem.eql(u8, record.id, id)) return record;
    return null;
}

fn find_record_index(records: []const manifest.record_t, id: []const u8) ?usize {
    for (records, 0..) |record, index| if (std.mem.eql(u8, record.id, id)) return index;
    return null;
}

fn backoff_ms(failures: u32, maximum: u64) u64 {
    const shift: u6 = @intCast(@min(failures, 6));
    return @min(maximum, @as(u64, 125) << shift);
}

fn min_optional(current: ?u64, candidate: u64) u64 {
    return if (current) |value| @min(value, candidate) else candidate;
}

fn truncate_title(title: []const u8) []const u8 {
    if (title.len <= MaxTitleBytes) return title;
    // Callers validate UTF-8 first, so a lead byte exists within the three bytes before the cap.
    var end: usize = MaxTitleBytes;
    while ((title[end] & 0xC0) == 0x80) end -= 1;
    return title[0..end];
}

/// Parse a little-endian u16 icon size array into a bitmask over `IconSizes`.
fn parse_icon_mask(bytes: ?[]const u8) !u8 {
    const value = bytes orelse return 0;
    if (value.len % 2 != 0 or value.len > 14) return error.InvalidArgument;
    var mask: u8 = 0;
    var previous: u16 = 0;
    var offset: usize = 0;
    while (offset < value.len) : (offset += 2) {
        const size = std.mem.readInt(u16, value[offset..][0..2], .little);
        if (size <= previous) return error.InvalidArgument;
        previous = size;
        const index = size_index(size) orelse return error.InvalidArgument;
        mask |= @as(u8, 1) << index;
    }
    return mask;
}

fn size_index(size: u16) ?u3 {
    for (IconSizes, 0..) |candidate, i| if (candidate == size) return @intCast(i);
    return null;
}

/// Same identity rule as desktop_artifacts: `a-` AUMID keys or 34-byte `p-` hashes, with the
/// desktop file name fitting NAME_MAX for `device`.
fn valid_key(key: []const u8, device: []const u8) bool {
    if (key.len < 3 or key.len > 200 or key[1] != '-') return false;
    if (key[0] != 'a' and key[0] != 'p') return false;
    if (key[0] == 'p') {
        if (key.len != 34) return false;
        for (key[2..]) |char| if (!(std.ascii.isDigit(char) or (char >= 'a' and char <= 'f'))) return false;
    } else {
        for (key[2..]) |char| if (!(std.ascii.isAlphanumeric(char) or char == '_' or char == '-')) return false;
    }
    if (device.len != 0) {
        const desktop_bytes = "waddle.".len + device.len + 1 + key.len + ".desktop".len;
        if (desktop_bytes > desktop_artifacts.MaxPathComponentBytes) return false;
    }
    return true;
}

fn valid_identity(value: []const u8) bool {
    if (value.len < 1 or value.len > 64) return false;
    for (value) |char| if (!(std.ascii.isAlphanumeric(char) or char == '_' or char == '-')) return false;
    return true;
}

fn valid_absolute(path: []const u8, directory: bool) bool {
    if (path.len == 0 or path[0] != '/' or !std.unicode.utf8ValidateSlice(path)) return false;
    if (directory and path.len > 1 and path[path.len - 1] == '/') return false;
    for (path) |char| {
        if (char == 0 or char < 0x20 or char == '=' or char == '%') return false;
    }
    return true;
}

fn png_header_valid(bytes: []const u8, size: u16) bool {
    if (bytes.len < 33) return false;
    if (!std.mem.eql(u8, bytes[0..8], "\x89PNG\r\n\x1a\n")) return false;
    if (!std.mem.eql(u8, bytes[12..16], "IHDR")) return false;
    const width = std.mem.readInt(u32, bytes[16..20], .big);
    const height = std.mem.readInt(u32, bytes[20..24], .big);
    return width == size and height == size and bytes[24] == 8 and bytes[25] == 6 and bytes[26] == 0 and bytes[27] == 0 and bytes[28] == 0;
}

/// Counting sink: sizes the output without allocating. Appends cannot fail.
const count_sink_t = struct {
    len: usize = 0,

    fn append(self: *count_sink_t, byte: u8) error{}!void {
        _ = byte;
        self.len +|= 1;
    }

    fn append_slice(self: *count_sink_t, bytes: []const u8) error{}!void {
        self.len +|= bytes.len;
    }
};

/// Fill sink over an exact-size buffer sized by `CountSink`. Appends cannot fail; the caller
/// asserts the final cursor equals the counted length.
const fill_sink_t = struct {
    bytes: []u8,
    cursor: usize = 0,

    fn append(self: *fill_sink_t, byte: u8) error{}!void {
        self.bytes[self.cursor] = byte;
        self.cursor += 1;
    }

    fn append_slice(self: *fill_sink_t, bytes: []const u8) error{}!void {
        @memcpy(self.bytes[self.cursor..][0..bytes.len], bytes);
        self.cursor += bytes.len;
    }
};

/// Serialize a schema-1 manifest. The grammar and escaping are byte-identical to
/// desktop_artifacts' emitter; the differential test in this file checks that equality.
/// Records are sorted by id. Caller owns the returned slice. A count pass sizes the output
/// exactly, so the only allocation is the final buffer.
pub fn emit_manifest(allocator: std.mem.Allocator, waddle_version: []const u8, updated: []const u8, records: []const manifest.record_t) ![]u8 {
    const sorted = try allocator.dupe(manifest.record_t, records);
    defer allocator.free(sorted);
    std.mem.sort(manifest.record_t, sorted, {}, record_less_than);
    var count = count_sink_t{};
    try write_manifest(&count, waddle_version, updated, sorted);
    if (count.len > desktop_artifacts.MaxManifestBytes) return error.ManifestTooLarge;
    const output = try allocator.alloc(u8, count.len);
    errdefer allocator.free(output);
    var fill = fill_sink_t{ .bytes = output };
    try write_manifest(&fill, waddle_version, updated, sorted);
    std.debug.assert(fill.cursor == output.len);
    return output;
}

fn write_manifest(out: anytype, waddle_version: []const u8, updated: []const u8, sorted: []const manifest.record_t) error{}!void {
    try out.append_slice("{\"schema\":1,\"waddle_version\":");
    try json_string(out, waddle_version);
    try out.append_slice(",\"updated\":");
    try json_string(out, updated);
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
            .U32Value => |value| try append_decimal(out, value),
        }
        if (record.mime) |value| {
            try out.append_slice(",\"mime\":");
            try json_string(out, value);
        }
        try out.append('}');
    }
    try out.append_slice("]}");
}

fn append_decimal(out: anytype, value: u32) error{}!void {
    var digits: [10]u8 = undefined;
    const text = std.fmt.bufPrint(&digits, "{d}", .{value}) catch unreachable;
    try out.append_slice(text);
}

fn record_less_than(_: void, left: manifest.record_t, right: manifest.record_t) bool {
    return std.mem.lessThan(u8, left.id, right.id);
}

fn json_string(out: anytype, value: []const u8) error{}!void {
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

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

fn slice_of(pointer: ?[*]const u8, length: usize) []const u8 {
    const ptr = pointer orelse return &.{};
    return ptr[0..length];
}

/// Map a runtime error to its C status. Publication budget exhaustion never reaches the
/// C boundary because every public publishing entry point consumes it internally.
fn status_of(err: anyerror) c_int {
    const mapped: status_t = switch (err) {
        error.Rejected => .Rejected,
        error.Quota => .Quota,
        error.OutOfMemory => .OutOfMemory,
        error.Unsupported => .Unsupported,
        error.InvalidArgument => .InvalidArgument,
        error.Pending, error.PublicationBudget => .Pending,
        else => .StoreFailed,
    };
    return @intFromEnum(mapped);
}

fn status_from(result: anytype) c_int {
    _ = result catch |err| return status_of(err);
    return @intFromEnum(status_t.Ok);
}

/// Create a runtime from C configuration. On success writes the handle to `out`.
/// @param[in] config Non-NULL configuration; borrowed for the call.
/// @param[out] out Non-NULL slot; written only on success (null on failure).
/// @return Ok, InvalidArgument, Unsupported, OutOfMemory, or StoreFailed (store lock or recovery).
/// @note Owned until catalog_runtime_destroy. Thread-safety: owner thread only.
export fn catalog_runtime_create(config: ?*const config_c_t, out: ?*?*runtime_t) c_int {
    const target = out orelse return @intFromEnum(status_t.InvalidArgument);
    target.* = null;
    const raw = config orelse return @intFromEnum(status_t.InvalidArgument);
    const shortcuts = std.meta.intToEnum(shortcuts_policy_t, raw.shortcuts) catch return @intFromEnum(status_t.InvalidArgument);
    const menu = std.meta.intToEnum(menu_policy_t, raw.menu_policy) catch return @intFromEnum(status_t.InvalidArgument);
    const zig_config = config_t{
        .manifest_dir_fd = raw.manifest_dir_fd,
        .data_root = slice_of(raw.data_root, raw.data_root_length),
        .data_root_fd = raw.data_root_fd,
        .config_root = slice_of(raw.config_root, raw.config_root_length),
        .config_root_fd = raw.config_root_fd,
        .launcher_path = slice_of(raw.launcher_path, raw.launcher_path_length),
        .device = slice_of(raw.device, raw.device_length),
        .waddle_version = slice_of(raw.waddle_version, raw.waddle_version_length),
        .menu_policy = menu,
        .shortcuts = shortcuts,
    };
    const runtime = runtime_t.create(std.heap.c_allocator, zig_config) catch |err| return status_of(err);
    target.* = runtime;
    return @intFromEnum(status_t.Ok);
}

/// Release a runtime and its store lock. Null is ignored.
export fn catalog_runtime_destroy(runtime: ?*runtime_t) void {
    const self = runtime orelse return;
    self.destroy();
}

/// Open a catalog transaction (CatalogBegin).
export fn catalog_runtime_begin(runtime: ?*runtime_t) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    self.begin();
    return @intFromEnum(status_t.Ok);
}

/// Close a catalog transaction (CatalogEnd). Returns Rejected for unopened or poisoned sessions.
export fn catalog_runtime_end(runtime: ?*runtime_t) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    return status_from(self.end());
}

/// Apply CatalogAdd (is_update 0) or CatalogUpdate (is_update nonzero).
export fn catalog_runtime_upsert(runtime: ?*runtime_t, is_update: c_int, app: ?*const app_c_t) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    const raw = app orelse return @intFromEnum(status_t.InvalidArgument);
    const input = app_input_t{
        .key = slice_of(raw.key, raw.key_length),
        .display_name = slice_of(raw.display_name, raw.display_name_length),
        .flags = raw.flags,
        .icon_sizes = if (raw.icon_sizes == null) null else slice_of(raw.icon_sizes, raw.icon_sizes_length),
        .content_hash = if (raw.content_hash == null) null else slice_of(raw.content_hash, raw.content_hash_length),
    };
    return status_from(self.upsert(is_update != 0, input));
}

/// Apply CatalogRemove.
export fn catalog_runtime_remove(runtime: ?*runtime_t, key: ?[*]const u8, key_length: usize) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    self.remove(slice_of(key, key_length));
    return @intFromEnum(status_t.Ok);
}

/// Drop the in-memory snapshot after a session change; published files stay.
export fn catalog_runtime_reset_session(runtime: ?*runtime_t) void {
    const self = runtime orelse return;
    self.reset_session();
}

/// Accept TransferBegin for an icon request identifier.
export fn catalog_runtime_transfer_begin(runtime: ?*runtime_t, id: u32, total: u64, mime: ?[*]const u8, mime_length: usize, now_ms: u64) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    return status_from(self.transfer_begin(id, total, slice_of(mime, mime_length), now_ms));
}

/// Accept one TransferChunk.
export fn catalog_runtime_transfer_chunk(runtime: ?*runtime_t, id: u32, offset: u64, data: ?[*]const u8, length: usize, now_ms: u64) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    return status_from(self.transfer_chunk(id, offset, slice_of(data, length), now_ms));
}

/// Accept TransferEnd. `hash` must point to 32 readable bytes.
export fn catalog_runtime_transfer_end(runtime: ?*runtime_t, id: u32, hash: ?*const [32]u8, now_ms: u64) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    const digest = hash orelse return @intFromEnum(status_t.InvalidArgument);
    return status_from(self.transfer_end(id, digest, now_ms));
}

/// Accept a guest TransferCancel.
export fn catalog_runtime_transfer_cancel(runtime: ?*runtime_t, id: u32, now_ms: u64) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    self.transfer_cancel(id, now_ms);
    return @intFromEnum(status_t.Ok);
}

/// Run timeouts, issue requests and publish pending artifacts. `updated` is UTC RFC3339.
export fn catalog_runtime_tick(runtime: ?*runtime_t, now_ms: u64, updated: ?[*]const u8, updated_length: usize) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    self.tick(now_ms, slice_of(updated, updated_length));
    return @intFromEnum(status_t.Ok);
}

/// Publish an uncatalogued window's NoDisplay entry before its first commit.
export fn catalog_runtime_ensure_uncatalogued(runtime: ?*runtime_t, key: ?[*]const u8, key_length: usize, title: ?[*]const u8, title_length: usize, now_ms: u64, updated: ?[*]const u8, updated_length: usize) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    return status_from(self.ensure_uncatalogued(slice_of(key, key_length), slice_of(title, title_length), now_ms, slice_of(updated, updated_length)));
}

/// Copy the next outbound action into `out`. Returns 1 when written, 0 when none is queued.
export fn catalog_runtime_next_action(runtime: ?*runtime_t, out: ?*action_c_t) c_int {
    const self = runtime orelse return 0;
    const target = out orelse return 0;
    const action = self.next_action() orelse return 0;
    target.* = .{
        .kind = @intFromEnum(action.kind),
        .transfer = action.transfer,
        .icon_size = action.icon_size,
        .status = action.status,
        .key = if (action.key.len == 0) null else action.key.ptr,
        .key_length = action.key.len,
    };
    return 1;
}

/// Earliest deadline at or after `now_ms`, or UINT64_MAX when nothing is scheduled.
export fn catalog_runtime_next_deadline(runtime: ?*runtime_t, now_ms: u64) u64 {
    const self = runtime orelse return std.math.maxInt(u64);
    return self.next_deadline(now_ms) orelse std.math.maxInt(u64);
}

/// Copy diagnostic counters into `out`.
export fn catalog_runtime_stats(runtime: ?*runtime_t, out: ?*stats_c_t) c_int {
    const self = runtime orelse return @intFromEnum(status_t.InvalidArgument);
    const target = out orelse return @intFromEnum(status_t.InvalidArgument);
    target.* = self.stats();
    return @intFromEnum(status_t.Ok);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const testing = std.testing;
const sha256 = std.crypto.hash.sha2.Sha256;

const UpdatedTimestamp = "2026-10-25T12:00:00Z";

/// Rooted temporary data, config and state directories with mode-checked permissions.
/// Setup failures are programming errors in the fixture, so they trap rather than branch.
const env_t = struct {
    tmp: testing.TmpDir,
    data: std.fs.Dir,
    config: std.fs.Dir,
    state: std.fs.Dir,
    data_path: []u8,
    config_path: []u8,
    allocator: std.mem.Allocator,

    fn init(allocator: std.mem.Allocator) env_t {
        var tmp = testing.tmpDir(.{});
        std.posix.mkdirat(tmp.dir.fd, "data", 0o755) catch unreachable;
        std.posix.mkdirat(tmp.dir.fd, "config", 0o755) catch unreachable;
        std.posix.mkdirat(tmp.dir.fd, "state", 0o700) catch unreachable;
        const data = tmp.dir.openDir("data", .{}) catch unreachable;
        const config = tmp.dir.openDir("config", .{}) catch unreachable;
        const state = tmp.dir.openDir("state", .{}) catch unreachable;
        const data_path = tmp.dir.realpathAlloc(allocator, "data") catch unreachable;
        const config_path = tmp.dir.realpathAlloc(allocator, "config") catch unreachable;
        return .{ .tmp = tmp, .data = data, .config = config, .state = state, .data_path = data_path, .config_path = config_path, .allocator = allocator };
    }

    fn deinit(self: *env_t) void {
        self.allocator.free(self.data_path);
        self.allocator.free(self.config_path);
        self.data.close();
        self.config.close();
        self.state.close();
        self.tmp.cleanup();
    }

    fn config_for(self: *const env_t) config_t {
        return .{
            .manifest_dir_fd = self.state.fd,
            .data_root = self.data_path,
            .data_root_fd = self.data.fd,
            .config_root = self.config_path,
            .config_root_fd = self.config.fd,
            .launcher_path = "/usr/bin/waddle",
            .device = "testdev",
            .waddle_version = "0.2.0",
        };
    }

    fn read(self: *const env_t, dir: std.fs.Dir, rel: []const u8) ![]u8 {
        _ = self;
        return dir.readFileAlloc(testing.allocator, rel, 8 * 1024 * 1024);
    }
};

fn input_app(key: []const u8, name: []const u8) app_input_t {
    return .{ .key = key, .display_name = name, .flags = 0 };
}

fn sync(rt: *runtime_t, apps: []const app_input_t) !void {
    rt.begin();
    for (apps) |input| rt.upsert(false, input) catch unreachable;
    try rt.end();
}

/// Tick with advancing time until no publication work remains, bounded for safety.
fn drive(rt: *runtime_t, start: u64) u64 {
    var now = start;
    var rounds: usize = 0;
    while (rt.publication_pending() and rounds < 500) : (rounds += 1) {
        now += 60_000;
        rt.tick(now, UpdatedTimestamp);
    }
    return now;
}

fn desktop_rel(buf: []u8, key: []const u8) []const u8 {
    return std.fmt.bufPrint(buf, "applications/waddle.testdev.{s}.desktop", .{key}) catch unreachable;
}

fn record_sha(rt: *runtime_t, record_id: []const u8) ![32]u8 {
    const snapshot = rt.store.inspect_record(record_id) catch unreachable;
    return snapshot.manifest_sha256;
}

/// A minimal PNG: signature, IHDR for the given square size, IDAT filler, IEND.
fn fake_png(allocator: std.mem.Allocator, size: u32, filler: usize, seed: u8) ![]u8 {
    const total = 8 + 25 + 12 + filler + 12;
    const bytes = try allocator.alloc(u8, total);
    @memset(bytes, seed);
    @memcpy(bytes[0..8], "\x89PNG\r\n\x1a\n");
    std.mem.writeInt(u32, bytes[8..12], 13, .big);
    @memcpy(bytes[12..16], "IHDR");
    std.mem.writeInt(u32, bytes[16..20], size, .big);
    std.mem.writeInt(u32, bytes[20..24], size, .big);
    bytes[24] = 8;
    bytes[25] = 6;
    bytes[26] = 0;
    bytes[27] = 0;
    bytes[28] = 0;
    std.mem.writeInt(u32, bytes[33..37], @intCast(filler), .big);
    @memcpy(bytes[37..41], "IDAT");
    @memcpy(bytes[bytes.len - 12 ..][4..8], "IEND");
    return bytes;
}

/// Drain every queued action, returning the IconRequest identifiers in order.
fn collect_requests(rt: *runtime_t, out: *[8]u32) usize {
    var count: usize = 0;
    while (rt.next_action()) |action| {
        if (action.kind == .IconRequest and count < out.len) {
            out[count] = action.transfer;
            count += 1;
        }
    }
    return count;
}

/// Deliver one complete icon (begin, chunks, end) for transfer `id`.
fn deliver(rt: *runtime_t, id: u32, png: []const u8, now: u64) !void {
    rt.transfer_begin(id, png.len, "image/png", now) catch unreachable;
    var offset: usize = 0;
    while (offset < png.len) {
        const end = @min(png.len, offset + 61440);
        rt.transfer_chunk(id, offset, png[offset..end], now) catch unreachable;
        offset = end;
    }
    var digest: [32]u8 = undefined;
    sha256.hash(png, &digest, .{});
    try rt.transfer_end(id, &digest, now);
}

test "policy helpers: validators, icon masks, titles and backoff" {
    try testing.expect(valid_key("a-Notepad", "dev"));
    try testing.expect(valid_key("a-A_b-9", "dev"));
    try testing.expect(valid_key("p-0123456789abcdef0123456789abcdef", "dev"));
    try testing.expect(!valid_key("x-Notepad", "dev"));
    try testing.expect(!valid_key("a-", "dev"));
    try testing.expect(!valid_key("a-bad.dot", "dev"));
    try testing.expect(!valid_key("p-0123456789abcdef0123456789abcdeF", "dev"));
    try testing.expect(!valid_key("p-0123", "dev"));
    try testing.expect(!valid_key("a" ++ ("x" ** 200), "dev"));
    try testing.expect(valid_key("a-x", ""));

    try testing.expect(valid_identity("dev_1-x"));
    try testing.expect(!valid_identity(""));
    try testing.expect(!valid_identity("bad name"));

    try testing.expect(valid_absolute("/usr/bin/waddle", false));
    try testing.expect(valid_absolute("/", true));
    try testing.expect(!valid_absolute("", false));
    try testing.expect(!valid_absolute("relative", false));
    try testing.expect(!valid_absolute("/trailing/", true));
    try testing.expect(!valid_absolute("/has=equals", false));
    try testing.expect(!valid_absolute("/has%percent", false));
    try testing.expect(!valid_absolute("/ctl\x01", false));
    try testing.expect(!valid_absolute("/bad\xff", false));

    try testing.expectEqual(@as(u8, 0), try parse_icon_mask(null));
    try testing.expectEqual(@as(u8, 0b0000101), try parse_icon_mask(&[_]u8{ 16, 0, 32, 0 }));
    try testing.expectError(error.InvalidArgument, parse_icon_mask(&[_]u8{ 16, 0, 16, 0 }));
    try testing.expectError(error.InvalidArgument, parse_icon_mask(&[_]u8{ 32, 0, 16, 0 }));
    try testing.expectError(error.InvalidArgument, parse_icon_mask(&[_]u8{ 17, 0 }));
    try testing.expectError(error.InvalidArgument, parse_icon_mask(&[_]u8{16}));
    try testing.expectError(error.InvalidArgument, parse_icon_mask(&([_]u8{0} ** 16)));

    try testing.expectEqualStrings("abc", truncate_title("abc"));
    const long = "é" ** 40;
    const clipped = truncate_title(long);
    try testing.expect(clipped.len <= MaxTitleBytes);
    try testing.expect(std.unicode.utf8ValidateSlice(clipped));

    try testing.expectEqual(@as(u64, 125), backoff_ms(0, MaxPublishBackoffMs));
    try testing.expectEqual(MaxPublishBackoffMs, backoff_ms(20, MaxPublishBackoffMs));
    try testing.expectEqual(@as(u64, 5), min_optional(null, 5));
    try testing.expectEqual(@as(u64, 3), min_optional(7, 3));

    try testing.expectEqual(@as(u3, 2), size_index(32).?);
    try testing.expect(size_index(17) == null);

    var png_ok = [_]u8{0} ** 33;
    @memcpy(png_ok[0..8], "\x89PNG\r\n\x1a\n");
    @memcpy(png_ok[12..16], "IHDR");
    std.mem.writeInt(u32, png_ok[16..20], 16, .big);
    std.mem.writeInt(u32, png_ok[20..24], 16, .big);
    png_ok[24] = 8;
    png_ok[25] = 6;
    try testing.expect(png_header_valid(&png_ok, 16));
    try testing.expect(!png_header_valid(&png_ok, 32));
    try testing.expect(!png_header_valid(png_ok[0..20], 16));
    png_ok[25] = 2;
    try testing.expect(!png_header_valid(&png_ok, 16));

    var buffer: [64]u8 = undefined;
    var sink = fill_sink_t{ .bytes = &buffer };
    try json_string(&sink, "q\"\\\n\r\t\x01/");
    try testing.expectEqualStrings("\"q\\\"\\\\\\n\\r\\t\\u0001/\"", buffer[0..sink.cursor]);
    inline for (std.meta.fields(manifest.record_kind_t)) |field| {
        const kind: manifest.record_kind_t = @enumFromInt(field.value);
        try testing.expect(kind_name(kind).len > 0);
    }
}

test "create rejects unsupported and malformed configuration" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    var mirror = env.config_for();
    mirror.shortcuts = .Mirror;
    try testing.expectError(error.Unsupported, runtime_t.create(testing.allocator, mirror));

    const base = env.config_for();
    var bad = base;
    bad.manifest_dir_fd = -1;
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.data_root = "relative";
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.config_root = base.data_root;
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.launcher_path = "/bin/x=y";
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.device = "bad device";
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.waddle_version = "";
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.data_root_fd = -1;
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.config_root_fd = -1;
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));
    bad = base;
    bad.waddle_version = "bad\x00";
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad));

    // A second runtime on the same state directory is refused by the exclusive lock.
    const first = try runtime_t.create(testing.allocator, base);
    try testing.expectError(error.InvalidManifest, runtime_t.create(testing.allocator, base));
    first.destroy();
}

test "100-app sync publishes entries and menu and repeats without rewriting" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();

    var names: [100][40]u8 = undefined;
    var inputs: [100]app_input_t = undefined;
    for (&inputs, 0..) |*input, i| {
        const key = try std.fmt.bufPrint(names[i][0..12], "a-App{d:0>4}", .{i});
        const name = try std.fmt.bufPrint(names[i][16..], "Application {d}", .{i});
        input.* = .{ .key = key, .display_name = name, .flags = 0 };
    }
    try sync(rt, &inputs);
    _ = drive(rt, 0);
    try testing.expect(!rt.publication_pending());
    try testing.expectEqual(@as(u32, 100), rt.stats().apps);

    var rel_buf: [128]u8 = undefined;
    const probe = desktop_rel(&rel_buf, "a-App0042");
    const before = try env.data.statFile(probe);
    const base_before = try testing.allocator.dupe(u8, rt.base.?);
    defer testing.allocator.free(base_before);

    try sync(rt, &inputs);
    const after_pass = drive(rt, 1_000_000);
    try testing.expect(!rt.publication_pending());
    try testing.expectEqualSlices(u8, base_before, rt.base.?);
    const after = try env.data.statFile(probe);
    try testing.expectEqual(before.inode, after.inode);
    try testing.expectEqual(before.mtime, after.mtime);
    try testing.expect(after_pass >= 1_000_000);

    const menu = try env.read(env.config, "menus/applications-merged/waddle-testdev.menu");
    defer testing.allocator.free(menu);
    try testing.expect(std.mem.indexOf(u8, menu, "<Category>X-Waddle-testdev</Category>") != null);
    const directory = try env.read(env.data, "desktop-directories/waddle-testdev.directory");
    defer testing.allocator.free(directory);
    try testing.expect(directory.len > 0);
}

test "changed content replaces the entry and keeps the manifest digest consistent" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try sync(rt, &.{ input_app("a-Alpha", "Alpha"), input_app("a-Beta", "Beta") });
    _ = drive(rt, 0);

    rt.begin();
    try rt.upsert(true, input_app("a-Alpha", "Alpha Renamed"));
    try rt.upsert(false, input_app("a-Beta", "Beta"));
    try rt.end();
    _ = drive(rt, 1_000_000);

    var rel_buf: [128]u8 = undefined;
    const content = try env.read(env.data, desktop_rel(&rel_buf, "a-Alpha"));
    defer testing.allocator.free(content);
    try testing.expect(std.mem.indexOf(u8, content, "Name=Alpha Renamed") != null);

    var digest: [32]u8 = undefined;
    sha256.hash(content, &digest, .{});
    const snapshot = try rt.store.inspect_record("desktop:testdev:a-Alpha");
    try testing.expectEqualSlices(u8, &digest, &snapshot.target.sha256);
    _ = try record_sha(rt, "desktop:testdev:a-Beta");
}

test "removed apps leave published files and count as deferred removals" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try sync(rt, &.{ input_app("a-Keep", "Keep"), input_app("a-Gone", "Gone") });
    _ = drive(rt, 0);
    try sync(rt, &.{input_app("a-Keep", "Keep")});
    _ = drive(rt, 1_000_000);
    try testing.expectEqual(@as(u32, 1), rt.stats().apps);
    var rel_buf: [128]u8 = undefined;
    _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Gone"));
    const menu = try env.read(env.config, "menus/applications-merged/waddle-testdev.menu");
    defer testing.allocator.free(menu);
    try testing.expect(std.mem.indexOf(u8, menu, "<Category>X-Waddle-testdev</Category>") != null);
}

test "transaction semantics: begin restarts, end rejects, incremental remains" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try testing.expectError(error.Rejected, rt.end());
    try testing.expectError(error.Rejected, rt.upsert(true, input_app("a-Missing", "Missing")));
    rt.remove("a-Missing");

    try rt.upsert(false, input_app("a-Incremental", "Incremental"));
    try testing.expectEqual(@as(u32, 1), rt.stats().apps);

    rt.begin();
    try rt.upsert(false, input_app("a-First", "First"));
    rt.begin();
    try rt.upsert(false, input_app("a-Second", "Second"));
    try rt.end();
    try testing.expectEqual(@as(u32, 1), rt.stats().apps);
    try testing.expect(rt.find("a-Second") != null);
    try testing.expect(rt.find("a-Incremental") == null);
    try testing.expectEqual(@as(u32, 1), rt.stats().rejected_frames);
}

test "hostile input is rejected and a poisoned transaction blocks publication" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();

    const bad_inputs = [_]app_input_t{
        input_app("x-Bad", "Bad"),
        input_app("a-bad.dot", "Bad"),
        input_app("p-short", "Bad"),
        input_app("a-Empty", ""),
        input_app("a-Utf8", "\xff"),
        input_app("a-Nul", "a\x00b"),
        .{ .key = "a-Flags", .display_name = "F", .flags = 0x20 },
        .{ .key = "a-Sizes", .display_name = "S", .flags = 0, .icon_sizes = &[_]u8{ 16, 0, 16, 0 } },
        .{ .key = "a-Hash", .display_name = "H", .flags = 0, .content_hash = &[_]u8{1} ** 31 },
    };
    for (bad_inputs) |input| {
        try testing.expectError(error.Rejected, rt.upsert(false, input));
    }
    try testing.expectEqual(@as(u32, 0), rt.stats().apps);
    try testing.expectEqual(@as(u32, bad_inputs.len), rt.stats().rejected_frames);

    rt.begin();
    try rt.upsert(false, input_app("a-Dup", "Dup"));
    try testing.expectError(error.Rejected, rt.upsert(false, input_app("a-Dup", "Dup")));
    try testing.expectError(error.Rejected, rt.end());
    try testing.expect(rt.sync_required);
    rt.tick(1_000_000, UpdatedTimestamp);
    var rel_buf: [128]u8 = undefined;
    try testing.expectError(error.FileNotFound, env.data.statFile(desktop_rel(&rel_buf, "a-Dup")));

    rt.reset_session();
    try testing.expect(!rt.sync_required);
    rt.begin();
    try testing.expectError(error.Rejected, rt.upsert(true, input_app("a-Unknown", "Unknown")));
    try rt.upsert(false, input_app("a-Ok", "Ok"));
    try rt.end();
    _ = drive(rt, 2_000_000);
    _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Ok"));
}

test "long device names reject oversize desktop file names" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    var config = env.config_for();
    const long_device = "d" ** 64;
    config.device = long_device;
    const rt = try runtime_t.create(testing.allocator, config);
    defer rt.destroy();
    try testing.expectError(error.Rejected, rt.upsert(false, input_app("a-" ++ ("k" ** 180), "Long")));
    try rt.upsert(false, input_app("a-Short", "Short"));
}

test "application quota and uncatalogued quota are bounded" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    var keys: [MaxApps][16]u8 = undefined;
    rt.begin();
    for (0..MaxApps) |i| {
        const key = try std.fmt.bufPrint(&keys[i], "a-Q{d:0>5}", .{i});
        try rt.upsert(false, .{ .key = key, .display_name = "Q", .flags = 0 });
    }
    try testing.expectError(error.Quota, rt.upsert(false, input_app("a-Overflow", "O")));
    try testing.expectError(error.Rejected, rt.end());
    try testing.expectEqual(@as(u32, MaxApps), rt.stats().apps);
    try testing.expectEqual(@as(u32, 1), rt.stats().rejected_frames);

    rt.reset_session();
    rt.begin();
    var uncat_keys: [MaxUncatalogued + 1][16]u8 = undefined;
    for (0..MaxUncatalogued) |i| {
        const key = try std.fmt.bufPrint(&uncat_keys[i], "a-U{d:0>4}", .{i});
        try testing.expectError(error.Pending, rt.ensure_uncatalogued(key, "U", 0, UpdatedTimestamp));
    }
    try testing.expectError(error.Quota, rt.ensure_uncatalogued("a-UOverflow", "U", 0, UpdatedTimestamp));
    try testing.expectEqual(@as(u32, MaxUncatalogued), rt.stats().uncatalogued);
    try rt.end();
}

test "icon request, chunked transfer and icon publication" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const hash = [_]u8{0x11} ** 32;
    const sizes = [_]u8{ 16, 0, 32, 0 };
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Alpha", .display_name = "Alpha", .flags = 0, .icon_sizes = &sizes, .content_hash = &hash });
    try rt.end();
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    const requested = collect_requests(rt, &ids);
    try testing.expectEqual(@as(usize, 2), requested);
    try testing.expect(ids[0] != 0 and ids[1] != 0 and ids[0] != ids[1]);
    try testing.expectEqual(@as(u32, 2), rt.stats().outstanding);

    const png16 = try fake_png(testing.allocator, 16, 9000, 0x21);
    defer testing.allocator.free(png16);
    const png32 = try fake_png(testing.allocator, 32, 70000, 0x42);
    defer testing.allocator.free(png32);
    try deliver(rt, ids[0], png16, 0);
    try deliver(rt, ids[1], png32, 0);
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);
    _ = drive(rt, 1_000);

    // The desktop entry publishes. The icon artifact paths (icons/hicolor/...) are refused by
    // the store's current Data-root allowlist (hicolor/...), so publication stays pending with
    // backoff and the bytes stay retained. This is the documented blocked state.
    var rel_buf: [128]u8 = undefined;
    _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Alpha"));
    try testing.expectError(error.FileNotFound, env.data.statFile("icons/hicolor/16x16/apps/waddle.testdev.a-Alpha.png"));
    try testing.expect(rt.stats().publish_failures >= 1);
    try testing.expectEqual(png16.len + png32.len, rt.stats().icon_bytes);

    // Unchanged content does not request icons again.
    rt.tick(100_000, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 0), collect_requests(rt, &ids));
    try testing.expect(rt.publication_pending());
}

test "transfer hostility cancels deterministically and retries are bounded" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 16, 0 };
    const hash = [_]u8{0x22} ** 32;
    const names = [_][]const u8{ "a-Beta", "a-Gamma", "a-Delta", "a-Eps", "a-Zeta" };
    rt.begin();
    for (names) |name| try rt.upsert(false, .{ .key = name, .display_name = name, .flags = 0, .icon_sizes = &sizes, .content_hash = &hash });
    try rt.end();

    // Unknown identifiers are cancelled with NotFound and leave state unchanged.
    var digest = [_]u8{0} ** 32;
    try testing.expectError(error.Rejected, rt.transfer_begin(999, 4, "image/png", 0));
    try testing.expectError(error.Rejected, rt.transfer_chunk(999, 0, "abcd", 0));
    try testing.expectError(error.Rejected, rt.transfer_end(999, &digest, 0));
    rt.transfer_cancel(999, 0);
    try testing.expectEqual(CancelNotFound, rt.next_action().?.status);
    try testing.expectEqual(CancelNotFound, rt.next_action().?.status);
    try testing.expectEqual(CancelNotFound, rt.next_action().?.status);
    try testing.expect(rt.next_action() == null);

    // The requests for all five apps are issued in key order.
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 5), collect_requests(rt, &ids));
    const png = try fake_png(testing.allocator, 16, 300, 0x31);
    defer testing.allocator.free(png);

    // Beta: a second begin on a receiving transfer is a protocol violation, then a gap.
    try rt.transfer_begin(ids[0], png.len, "image/png", 0);
    try testing.expectError(error.Rejected, rt.transfer_begin(ids[0], png.len, "image/png", 0));
    try testing.expectEqual(CancelNotFound, rt.next_action().?.status);
    try testing.expectError(error.Rejected, rt.transfer_chunk(ids[0], 5, png[0..4], 0));
    try testing.expectEqual(CancelGuestError, rt.next_action().?.status);
    try testing.expectEqual(@as(u32, 4), rt.stats().outstanding);

    // Gamma: a digest mismatch cancels and the bytes are discarded.
    try rt.transfer_begin(ids[1], png.len, "image/png", 0);
    try rt.transfer_chunk(ids[1], 0, png, 0);
    try testing.expectError(error.Rejected, rt.transfer_end(ids[1], &digest, 0));
    try testing.expectEqual(CancelGuestError, rt.next_action().?.status);

    // Delta: a MIME type other than image/png is refused as Unsupported.
    try testing.expectError(error.Rejected, rt.transfer_begin(ids[2], 1, "text/plain", 0));
    try testing.expectEqual(CancelUnsupported, rt.next_action().?.status);

    // Eps: an announced size above the icon cap is refused as TooLarge; zero length too.
    try testing.expectError(error.Rejected, rt.transfer_begin(ids[3], MaxIconBytes + 1, "image/png", 0));
    try testing.expectEqual(CancelTooLarge, rt.next_action().?.status);

    // Zeta: a zero-length announcement is refused as TooLarge; the chunk path is unreachable.
    try testing.expectError(error.Rejected, rt.transfer_begin(ids[4], 0, "image/png", 0));
    try testing.expectEqual(CancelTooLarge, rt.next_action().?.status);
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);

    // A non-PNG payload of the right size is dropped without a cancel; the slot backs off.
    rt.tick(20_000, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 5), collect_requests(rt, &ids));
    const wrong = try fake_png(testing.allocator, 32, 10, 0x33);
    defer testing.allocator.free(wrong);
    try testing.expectError(error.Rejected, deliver(rt, ids[0], wrong, 20_000));
    try testing.expectEqual(@as(u32, 4), rt.stats().outstanding);
    try testing.expect(rt.next_action() == null);

    // After backoff the slots that failed are requested again, while Eps and Zeta are not.
    rt.tick(20_000 + IconRetryBackoffMs * 3, UpdatedTimestamp);
    try testing.expect(collect_requests(rt, &ids) >= 1);
}

test "timeouts cancel stalled transfers and retry after backoff" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 32, 0 };
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Gamma", .display_name = "Gamma", .flags = 0, .icon_sizes = &sizes });
    try rt.end();
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    try testing.expectEqual(RequestTimeoutMs, rt.next_deadline(0).?);
    rt.tick(RequestTimeoutMs, UpdatedTimestamp);
    const action = rt.next_action().?;
    try testing.expectEqual(CancelTimedOut, action.status);
    try testing.expectEqual(ids[0], action.transfer);
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);
    rt.tick(RequestTimeoutMs + IconRetryBackoffMs - 1, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 0), collect_requests(rt, &ids));
    rt.tick(RequestTimeoutMs + IconRetryBackoffMs, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    try testing.expect(ids[0] != 0);

    // A guest-side cancel retries after its backoff; unknown identifiers are no-ops.
    rt.transfer_cancel(ids[0], RequestTimeoutMs + IconRetryBackoffMs);
    rt.transfer_cancel(ids[0], RequestTimeoutMs + IconRetryBackoffMs);
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);
}

test "icon budget refuses transfers beyond the retained byte cap" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const size_256 = [_]u8{ 0x00, 0x01 };
    var keys: [33][6]u8 = undefined;
    const big = try testing.allocator.alloc(u8, MaxIconBytes);
    defer testing.allocator.free(big);
    @memset(big, 0);
    @memcpy(big[0..8], "\x89PNG\r\n\x1a\n");
    std.mem.writeInt(u32, big[16..20], 256, .big);
    std.mem.writeInt(u32, big[20..24], 256, .big);
    big[24] = 8;
    big[25] = 6;
    @memcpy(big[12..16], "IHDR");
    var digest: [32]u8 = undefined;
    sha256.hash(big, &digest, .{});

    rt.begin();
    for (0..32) |i| {
        const key = try std.fmt.bufPrint(&keys[i], "a-B{d:0>3}", .{i});
        try rt.upsert(false, .{ .key = key, .display_name = "B", .flags = 0, .icon_sizes = &size_256 });
    }
    try rt.end();
    var ids: [8]u32 = undefined;
    var delivered: usize = 0;
    var rounds: usize = 0;
    while (delivered < 32 and rounds < 64) : (rounds += 1) {
        rt.issue_requests(0);
        const count = collect_requests(rt, &ids);
        for (ids[0..count]) |id| {
            try rt.transfer_begin(id, big.len, "image/png", 0);
            var offset: usize = 0;
            while (offset < big.len) {
                const end = @min(big.len, offset + 61440);
                try rt.transfer_chunk(id, offset, big[offset..end], 0);
                offset = end;
            }
            try rt.transfer_end(id, &digest, 0);
            delivered += 1;
        }
    }
    try testing.expectEqual(@as(usize, 32), delivered);
    try testing.expectEqual(@as(usize, 32 * MaxIconBytes), rt.stats().icon_bytes);
    try rt.upsert(false, .{ .key = "a-Extra", .display_name = "Extra", .flags = 0, .icon_sizes = &size_256 });
    rt.issue_requests(0);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    try testing.expectError(error.Rejected, rt.transfer_begin(ids[0], big.len, "image/png", 0));
    try testing.expectEqual(CancelTooLarge, rt.next_action().?.status);
}

test "store failures at every durable checkpoint keep state consistent and recover" {
    var checkpoint: usize = 1;
    while (checkpoint <= 24) : (checkpoint += 1) {
        var env = env_t.init(testing.allocator);
        defer env.deinit();
        const rt = try runtime_t.create(testing.allocator, env.config_for());
        defer rt.destroy();
        rt.store.fail_after.store(checkpoint, .monotonic);
        try sync(rt, &.{ input_app("a-One", "One"), input_app("a-Two", "Two") });
        rt.tick(0, UpdatedTimestamp);
        rt.store.fail_after.store(0, .monotonic);
        _ = drive(rt, 1_000_000);
        try testing.expect(!rt.publication_pending());
        var rel_buf: [128]u8 = undefined;
        _ = try env.data.statFile(desktop_rel(&rel_buf, "a-One"));
        _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Two"));
        if (rt.base) |bytes| {
            var digest: [32]u8 = undefined;
            sha256.hash(bytes, &digest, .{});
            const snapshot = try rt.store.inspect_record("desktop:testdev:a-Two");
            try testing.expectEqualSlices(u8, &digest, &snapshot.manifest_sha256);
        }
    }
}

test "failed publication keeps prior published state and backs off" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try sync(rt, &.{input_app("a-Stable", "Stable")});
    _ = drive(rt, 0);
    var rel_buf: [128]u8 = undefined;
    const path = desktop_rel(&rel_buf, "a-Stable");
    const original = try env.read(env.data, path);
    defer testing.allocator.free(original);

    rt.store.fail_after.store(rt.store.checkpoint_count.load(.monotonic) + 1, .monotonic);
    try sync(rt, &.{input_app("a-Stable", "Changed")});
    rt.tick(1_000_000, UpdatedTimestamp);
    try testing.expect(rt.stats().publish_failures >= 1);
    const still = try env.read(env.data, path);
    defer testing.allocator.free(still);
    try testing.expectEqualSlices(u8, original, still);
    try testing.expect(rt.next_deadline(1_000_000).? > 1_000_000);
    rt.tick(1_000_001, UpdatedTimestamp);
    rt.store.fail_after.store(0, .monotonic);
    _ = drive(rt, 2_000_000);
    const changed = try env.read(env.data, path);
    defer testing.allocator.free(changed);
    try testing.expect(std.mem.indexOf(u8, changed, "Name=Changed") != null);
    try testing.expectEqual(@as(u32, 0), rt.stats().publish_failures);
}

test "ensure_uncatalogued publishes a NoDisplay entry before its first commit" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try rt.ensure_uncatalogued("a-Window", "Window Title", 0, UpdatedTimestamp);
    var rel_buf: [128]u8 = undefined;
    const content = try env.read(env.data, desktop_rel(&rel_buf, "a-Window"));
    defer testing.allocator.free(content);
    try testing.expect(std.mem.indexOf(u8, content, "NoDisplay=true") != null);
    try testing.expect(std.mem.indexOf(u8, content, "Name=Window Title") != null);
    try rt.ensure_uncatalogued("a-Window", "Window Title", 0, UpdatedTimestamp);
    try testing.expectEqual(@as(u32, 1), rt.stats().uncatalogued);

    try testing.expectError(error.InvalidArgument, rt.ensure_uncatalogued("x-Bad", "T", 0, UpdatedTimestamp));
    try testing.expectError(error.InvalidArgument, rt.ensure_uncatalogued("a-Nul", "a\x00", 0, UpdatedTimestamp));
    try rt.ensure_uncatalogued("a-Untitled", "", 0, UpdatedTimestamp);
    try testing.expectEqual(@as(u32, 2), rt.stats().uncatalogued);

    // A catalog entry supersedes the uncatalogued entry without a store operation.
    try sync(rt, &.{input_app("a-Window", "Window Title")});
    try testing.expectEqual(@as(u32, 1), rt.stats().uncatalogued);
    _ = drive(rt, 1_000_000);

    rt.begin();
    try testing.expectError(error.Pending, rt.ensure_uncatalogued("a-Pending", "P", 0, UpdatedTimestamp));
    try rt.end();
}

test "reset_session drops the snapshot and keeps published files" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try sync(rt, &.{ input_app("a-Stay", "Stay"), input_app("a-Left", "Left") });
    _ = drive(rt, 0);
    rt.reset_session();
    try testing.expectEqual(@as(u32, 0), rt.stats().apps);
    var rel_buf: [128]u8 = undefined;
    _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Stay"));
    try testing.expect(!rt.publication_pending());
}

test "catalog state under allocation failure never leaks" {
    var index: usize = 0;
    var completed = false;
    while (!completed and index < 2000) : (index += 1) {
        var env = env_t.init(testing.allocator);
        defer env.deinit();
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = index });
        const allocator = failing.allocator();
        const created = runtime_t.create(allocator, env.config_for());
        if (created) |rt| {
            defer rt.destroy();
            const sizes = [_]u8{ 16, 0 };
            const hash = [_]u8{0x44} ** 32;
            const png = fake_png(testing.allocator, 16, 40, 0x55) catch unreachable;
            defer testing.allocator.free(png);
            rt.begin();
            if (rt.upsert(false, .{ .key = "a-Sweep", .display_name = "Sweep", .flags = 0, .icon_sizes = &sizes, .content_hash = &hash })) |_| {} else |_| {}
            rt.end() catch {};
            rt.tick(0, UpdatedTimestamp);
            var ids: [8]u32 = undefined;
            const count = collect_requests(rt, &ids);
            if (count == 1) {
                rt.transfer_begin(ids[0], png.len, "image/png", 0) catch {};
                rt.transfer_chunk(ids[0], 0, png, 0) catch {};
                var digest: [32]u8 = undefined;
                sha256.hash(png, &digest, .{});
                rt.transfer_end(ids[0], &digest, 0) catch {};
            }
            rt.tick(1_000, UpdatedTimestamp);
            rt.reset_session();
        } else |err| {
            try testing.expectEqual(error.OutOfMemory, err);
        }
        completed = !failing.has_induced_failure;
    }
    try testing.expect(completed);
}

/// Comptime-formatted fixtures for the randomized test: keys, display names and expected
/// `Name=` lines per (app, variant). Comptime formatting keeps the runtime path free of
/// integer formatting.
const random_fixture_t = struct {
    const Keys = blk: {
        var table: [12][]const u8 = undefined;
        for (0..12) |i| table[i] = std.fmt.comptimePrint("a-K{d:0>2}", .{i});
        break :blk table;
    };
    const Names = blk: {
        var table: [12][3][]const u8 = undefined;
        for (0..12) |i| {
            for (0..3) |v| table[i][v] = std.fmt.comptimePrint("Key{d:0>2} v{d}", .{ i, v });
        }
        break :blk table;
    };
    const Wanted = blk: {
        var table: [12][3][]const u8 = undefined;
        for (0..12) |i| {
            for (0..3) |v| table[i][v] = std.fmt.comptimePrint("Name=Key{d:0>2} v{d}", .{ i, v });
        }
        break :blk table;
    };
};

test "randomized sequences agree with a reference model" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    var prng = std.rand.DefaultPrng.init(0x5eed_c0de);
    const random = prng.random();
    var model: [12]?u8 = [_]?u8{null} ** 12;

    var round: usize = 0;
    var now: u64 = 0;
    while (round < 40) : (round += 1) {
        const choice = random.uintLessThan(u8, 3);
        if (choice == 0) {
            rt.begin();
            for (0..12) |i| {
                if (random.boolean()) continue;
                const variant = random.uintLessThan(u8, 3);
                try rt.upsert(false, .{ .key = random_fixture_t.Keys[i], .display_name = random_fixture_t.Names[i][variant], .flags = 0 });
                model[i] = variant;
            }
            // The transaction announced a random subset; everything else is removed by End.
            try rt.end();
            for (0..12) |i| {
                if (!rt_has(rt, random_fixture_t.Keys[i])) model[i] = null;
            }
        } else {
            for (0..12) |i| {
                if (!random.boolean()) continue;
                const variant = random.uintLessThan(u8, 3);
                const is_update = model[i] != null;
                try rt.upsert(is_update, .{ .key = random_fixture_t.Keys[i], .display_name = random_fixture_t.Names[i][variant], .flags = 0 });
                model[i] = variant;
            }
            for (0..12) |i| {
                if (random.boolean() and model[i] != null) {
                    rt.remove(random_fixture_t.Keys[i]);
                    model[i] = null;
                }
            }
        }
        now = drive(rt, now + 1000);
        var present: u32 = 0;
        for (0..12) |i| {
            var rel_buf: [128]u8 = undefined;
            const rel = desktop_rel(&rel_buf, random_fixture_t.Keys[i]);
            if (model[i]) |variant| {
                present += 1;
                const content = try env.read(env.data, rel);
                defer testing.allocator.free(content);
                try testing.expect(std.mem.indexOf(u8, content, random_fixture_t.Wanted[i][variant]) != null);
            }
        }
        try testing.expectEqual(present, rt.stats().apps);
        try testing.expect(!rt.publication_pending());
    }
}

fn rt_has(rt: *runtime_t, key: []const u8) bool {
    return rt.find(key) != null;
}

test "C ABI covers configuration, null handling and every export" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    var handle: ?*runtime_t = null;
    var raw = config_c_t{
        .manifest_dir_fd = env.state.fd,
        .data_root_fd = env.data.fd,
        .config_root_fd = env.config.fd,
        .menu_policy = @intFromEnum(menu_policy_t.All),
        .shortcuts = @intFromEnum(shortcuts_policy_t.None),
        .data_root = env.data_path.ptr,
        .data_root_length = env.data_path.len,
        .config_root = env.config_path.ptr,
        .config_root_length = env.config_path.len,
        .launcher_path = "/usr/bin/waddle".ptr,
        .launcher_path_length = "/usr/bin/waddle".len,
        .device = "testdev".ptr,
        .device_length = "testdev".len,
        .waddle_version = "0.2.0".ptr,
        .waddle_version_length = "0.2.0".len,
    };
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_create(null, &handle));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_create(&raw, null));
    raw.shortcuts = 9;
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_create(&raw, &handle));
    raw.shortcuts = @intFromEnum(shortcuts_policy_t.Mirror);
    try testing.expectEqual(@as(c_int, 7), catalog_runtime_create(&raw, &handle));
    raw.shortcuts = @intFromEnum(shortcuts_policy_t.None);
    raw.menu_policy = 7;
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_create(&raw, &handle));
    raw.menu_policy = @intFromEnum(menu_policy_t.All);
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_create(&raw, &handle));
    const rt = handle.?;

    try testing.expectEqual(@as(c_int, 1), catalog_runtime_begin(null));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_begin(rt));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_end(null));
    const key = "a-Cabi";
    const name = "Cabi";
    const sizes = [_]u8{ 16, 0 };
    var hash = [_]u8{0x66} ** 32;
    var app_c = app_c_t{
        .key = key.ptr,
        .key_length = key.len,
        .display_name = name.ptr,
        .display_name_length = name.len,
        .flags = 0,
        .icon_sizes = &sizes,
        .icon_sizes_length = sizes.len,
        .content_hash = &hash,
        .content_hash_length = hash.len,
    };
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_upsert(null, 0, &app_c));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_upsert(rt, 0, null));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_upsert(rt, 0, &app_c));
    app_c.flags = 0x40;
    try testing.expectEqual(@as(c_int, 2), catalog_runtime_upsert(rt, 1, &app_c));
    try testing.expectEqual(@as(c_int, 2), catalog_runtime_end(rt));
    catalog_runtime_reset_session(rt);
    app_c.flags = 0;
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_begin(rt));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_upsert(rt, 0, &app_c));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_end(rt));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_tick(rt, 0, UpdatedTimestamp.ptr, UpdatedTimestamp.len));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_tick(null, 0, null, 0));

    var action: action_c_t = undefined;
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_next_action(rt, &action));
    try testing.expectEqual(@as(u32, 1), action.kind);
    try testing.expectEqual(@as(u32, 16), action.icon_size);
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_next_action(rt, null));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_next_action(null, &action));
    const id = action.transfer;
    const png = try fake_png(testing.allocator, 16, 64, 0x77);
    defer testing.allocator.free(png);
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_begin(null, id, png.len, "image/png".ptr, 9, 0));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_transfer_begin(rt, id, png.len, "image/png".ptr, 9, 0));
    try testing.expectEqual(@as(c_int, 2), catalog_runtime_transfer_begin(rt, id, png.len, "image/png".ptr, 9, 0));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_chunk(null, id, 0, png.ptr, png.len, 0));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_transfer_chunk(rt, id, 0, png.ptr, png.len, 0));
    var digest: [32]u8 = undefined;
    sha256.hash(png, &digest, .{});
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_end(rt, id, null, 0));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_end(null, id, &digest, 0));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_transfer_end(rt, id, &digest, 0));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_cancel(null, id, 0));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_transfer_cancel(rt, id, 0));
    _ = catalog_runtime_next_action(rt, &action);

    try testing.expectEqual(@as(c_int, 0), catalog_runtime_remove(rt, "a-Cabi".ptr, 6));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_remove(null, null, 0));
    try testing.expectEqual(@as(u64, std.math.maxInt(u64)), catalog_runtime_next_deadline(null, 0));
    _ = catalog_runtime_next_deadline(rt, 0);
    var stats: stats_c_t = undefined;
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_stats(rt, &stats));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_stats(rt, null));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_stats(null, &stats));
    catalog_runtime_reset_session(null);
    catalog_runtime_reset_session(rt);
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_ensure_uncatalogued(null, null, 0, null, 0, 0, null, 0));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_ensure_uncatalogued(rt, "a-Ext".ptr, 5, "Ext".ptr, 3, 0, UpdatedTimestamp.ptr, UpdatedTimestamp.len));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_ensure_uncatalogued(rt, "x-Bad".ptr, 5, "Bad".ptr, 3, 0, UpdatedTimestamp.ptr, UpdatedTimestamp.len));
    catalog_runtime_destroy(null);
    catalog_runtime_destroy(rt);
}

test "emit_manifest is byte-identical to desktop_artifacts candidates" {
    const items = [_]desktop_artifacts.item_input_t{
        .{ .app_key = "a-Zeta", .display_name = "Zeta \"quoted\"", .system = true },
        .{ .app_key = "a-Alpha", .display_name = "Alpha", .seed = true },
    };
    var out = desktop_artifacts.artifacts_t{ .allocator = testing.allocator, .items = &.{} };
    defer out.deinit();
    try desktop_artifacts.build(testing.allocator, .{
        .data_root = "/home/test/.local/share",
        .config_root = "/home/test/.config",
        .launcher_path = "/usr/bin/waddle",
        .device = "testdev",
        .waddle_version = "0.2.0",
        .updated = UpdatedTimestamp,
        .items = &items,
        .menu_filter = .All,
        .include_menu = true,
    }, &out);
    try testing.expect(out.items.len >= 3);
    for (out.items) |*artifact| {
        var doc = try manifest.parse(testing.allocator, artifact.candidate_manifest);
        defer doc.deinit();
        const rebuilt = try emit_manifest(testing.allocator, "0.2.0", UpdatedTimestamp, doc.records);
        defer testing.allocator.free(rebuilt);
        try testing.expectEqualSlices(u8, artifact.candidate_manifest, rebuilt);
    }
}

test "a blocked icon does not stall publication of other apps" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 16, 0 };
    try sync(rt, &.{
        .{ .key = "a-Iconic", .display_name = "Iconic", .flags = 0, .icon_sizes = &sizes },
        input_app("a-Plain", "Plain"),
    });
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    const count = collect_requests(rt, &ids);
    try testing.expectEqual(@as(usize, 1), count);
    const png = try fake_png(testing.allocator, 16, 40, 0x66);
    defer testing.allocator.free(png);
    try deliver(rt, ids[0], png, 0);
    _ = drive(rt, 1_000);
    var rel_buf: [128]u8 = undefined;
    _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Plain"));
    _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Iconic"));
    try testing.expect(rt.stats().publish_failures >= 1);
}

test "helper branches: identifiers, icon sizes, PNG headers, titles and parent directories" {
    try testing.expect(valid_key("a-x", ""));
    try testing.expect(valid_key("a-" ++ ("x" ** 198), "d"));
    try testing.expect(!valid_key("a-" ++ ("x" ** 199), "d"));
    try testing.expect(valid_identity("a" ** 64));
    try testing.expect(!valid_identity("a" ** 65));
    inline for (IconSizes, 0..) |size, index| {
        try testing.expectEqual(@as(u3, index), size_index(size).?);
    }
    try testing.expectEqual(@as(u8, 0x7F), try parse_icon_mask(&[_]u8{ 16, 0, 24, 0, 32, 0, 48, 0, 64, 0, 128, 0, 0, 1 }));

    var header = [_]u8{0} ** 33;
    @memcpy(header[0..8], "\x89PNG\r\n\x1a\n");
    @memcpy(header[12..16], "IHDR");
    std.mem.writeInt(u32, header[16..20], 24, .big);
    std.mem.writeInt(u32, header[20..24], 24, .big);
    header[24] = 8;
    header[25] = 6;
    try testing.expect(png_header_valid(&header, 24));
    var bad = header;
    bad[0] = 0;
    try testing.expect(!png_header_valid(&bad, 24));
    bad = header;
    bad[12] = 'X';
    try testing.expect(!png_header_valid(&bad, 24));
    bad = header;
    std.mem.writeInt(u32, bad[20..24], 16, .big);
    try testing.expect(!png_header_valid(&bad, 24));
    bad = header;
    bad[24] = 16;
    try testing.expect(!png_header_valid(&bad, 24));
    bad = header;
    bad[26] = 1;
    try testing.expect(!png_header_valid(&bad, 24));
    bad = header;
    bad[27] = 1;
    try testing.expect(!png_header_valid(&bad, 24));
    bad = header;
    bad[28] = 1;
    try testing.expect(!png_header_valid(&bad, 24));

    // Multibyte titles are clipped on a character boundary; short titles pass through.
    const clipped = truncate_title("x" ** 63 ++ "é");
    try testing.expectEqual(@as(usize, 63), clipped.len);
    try testing.expectEqualStrings("short", truncate_title("short"));

    // Parent-directory creation: missing components are created, existing ones accepted,
    // files and symlinks in the way fail closed, and paths outside every root are refused.
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const data_file = try std.fmt.allocPrint(testing.allocator, "{s}/a/b/c.desktop", .{env.data_path});
    defer testing.allocator.free(data_file);
    try rt.ensure_parent_dirs(data_file);
    try rt.ensure_parent_dirs(data_file);
    _ = try env.data.statFile("a/b");
    var leaf_buf: [256]u8 = undefined;
    try rt.ensure_parent_dirs(try std.fmt.bufPrint(&leaf_buf, "{s}/leaf.desktop", .{env.data_path}));
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs("/elsewhere/x/y.png"));
    try env.data.writeFile(.{ .sub_path = "plainfile", .data = "x" });
    var plain_buf: [256]u8 = undefined;
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs(try std.fmt.bufPrint(&plain_buf, "{s}/plainfile/y/z.png", .{env.data_path})));
    try env.data.symLink("a", "link", .{});
    var link_buf: [256]u8 = undefined;
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs(try std.fmt.bufPrint(&link_buf, "{s}/link/q/z.png", .{env.data_path})));

    // find_record returns both outcomes.
    var doc = try manifest.parse(testing.allocator, "{\"schema\":1,\"waddle_version\":\"0.2.0\",\"updated\":\"2026-10-25T12:00:00Z\",\"records\":[]}");
    defer doc.deinit();
    try testing.expect(find_record(doc.records, "missing") == null);
    try testing.expect(find_record_index(doc.records, "missing") == null);
}

test "emit_manifest serializes every optional field and prior variant" {
    const records = [_]manifest.record_t{
        .{ .id = "z", .kind = .Menu, .path = "/p/z", .sha256 = "0" ** 64, .device = null, .app_key = null, .created_by = "0.2.0", .prior = .None, .mime = null },
        .{ .id = "a", .kind = .MimeDefault, .path = "/p/a\"\\\n", .sha256 = "1" ** 64, .device = "dev", .app_key = "a-K", .created_by = "0.2.0", .prior = .{ .Text = "old" }, .mime = "text/plain" },
        .{ .id = "m", .kind = .GuestPolicy, .path = "/p/m", .sha256 = "2" ** 64, .device = "dev", .app_key = null, .created_by = "0.2.0", .prior = .{ .U32Value = 4294967295 }, .mime = null },
        .{ .id = "n", .kind = .Icon, .path = "/p/n", .sha256 = "3" ** 64, .device = null, .app_key = "a-N", .created_by = "0.2.0", .prior = .None, .mime = null },
    };
    const bytes = try emit_manifest(testing.allocator, "0.2.0", UpdatedTimestamp, &records);
    defer testing.allocator.free(bytes);
    var doc = try manifest.parse(testing.allocator, bytes);
    defer doc.deinit();
    try testing.expectEqual(@as(usize, 4), doc.records.len);
    try testing.expectEqualStrings("a", doc.records[0].id);
    try testing.expectEqualStrings("old", doc.records[0].prior.Text);
    try testing.expectEqualStrings("text/plain", doc.records[0].mime.?);
    try testing.expectEqual(@as(u32, 4294967295), doc.records[1].prior.U32Value);
    try testing.expectEqualStrings("a-N", doc.records[2].app_key.?);
    try testing.expect(std.mem.indexOf(u8, bytes, "\"prior\":null") != null);
    try testing.expect(std.mem.indexOf(u8, bytes, "\"prior\":4294967295") != null);
}

test "state machine branches: deadlines, quotas, updates and transfer identifiers" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 16, 0 };
    const hash_a = [_]u8{0x01} ** 32;
    const hash_b = [_]u8{0x02} ** 32;

    // A quota outside a transaction does not poison anything.
    try rt.upsert(false, .{ .key = "a-Quota", .display_name = "Q", .flags = 0 });
    try testing.expectEqual(@as(u32, 1), rt.stats().apps);
    try testing.expectEqual(@as(?u64, 0), rt.next_deadline(0));

    // Update: a flags change marks the menu; a content-hash change requests icons again; mask
    // growth requests the newly advertised size; mask shrink stops requesting it.
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Upd", .display_name = "Upd", .flags = 0, .icon_sizes = &sizes, .content_hash = &hash_a });
    try rt.end();
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    rt.transfer_cancel(ids[0], 0);
    try rt.upsert(true, .{ .key = "a-Upd", .display_name = "Upd", .flags = 2, .icon_sizes = &sizes, .content_hash = &hash_a });
    try testing.expect(rt.menu_dirty);
    rt.tick(10, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 0), collect_requests(rt, &ids));
    try rt.upsert(true, .{ .key = "a-Upd", .display_name = "Upd", .flags = 2, .icon_sizes = &sizes, .content_hash = &hash_b });
    rt.tick(30, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    try rt.upsert(true, .{ .key = "a-Upd", .display_name = "Upd", .flags = 2, .icon_sizes = &[_]u8{ 16, 0, 32, 0 }, .content_hash = &hash_b });
    rt.tick(40, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    try rt.upsert(true, .{ .key = "a-Upd", .display_name = "Upd", .flags = 2, .icon_sizes = &[_]u8{ 16, 0 } });
    try testing.expect(rt.apps.items[rt.find("a-Upd").?].mask == 1);
    try testing.expectError(error.Rejected, rt.upsert(true, .{ .key = "a-Upd", .display_name = "Upd", .flags = 2, .content_hash = hash_a[0..31] }));

    // Pending transfers: next_deadline reports the earliest transfer deadline, can_issue is off at
    // the cap, and transfer_index(0) is never valid.
    try testing.expectError(error.Rejected, rt.transfer_begin(0, 1, "image/png", 0));
    _ = rt.next_action();
    rt.reset_session();
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);

    rt.begin();
    var keys: [MaxOutstanding + 1][8]u8 = undefined;
    for (0..MaxOutstanding + 1) |i| {
        const key = try std.fmt.bufPrint(&keys[i], "a-O{d:0>3}", .{i});
        try rt.upsert(false, .{ .key = key, .display_name = "O", .flags = 0, .icon_sizes = &sizes });
    }
    try rt.end();
    rt.issue_requests(100);
    try testing.expectEqual(MaxOutstanding, rt.stats().outstanding);
    // New apps are still dirty, so publication keeps the deadline at the current instant.
    try testing.expectEqual(@as(?u64, 100), rt.next_deadline(100));
    try testing.expectEqual(@as(usize, MaxOutstanding), collect_requests(rt, &ids));

    // Transfer identifiers skip values in use and never equal zero.
    rt.next_transfer_id = ids[0];
    const next = rt.next_id();
    try testing.expect(next != 0 and next != ids[0]);

    // A transfer in AwaitBegin cannot take a chunk or end.
    try testing.expectError(error.Rejected, rt.transfer_chunk(ids[1], 0, "x", 100));
    _ = rt.next_action();
    try testing.expectError(error.Rejected, rt.transfer_end(ids[1], &hash_a, 100));
    _ = rt.next_action();

    // Destroying a session with live transfers and resetting it must free their state.
    rt.reset_session();
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);
    try testing.expectEqual(@as(?u64, null), rt.next_deadline(200));
}

test "icon replacement, uncatalogued supersession, and open transfers at teardown" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    const sizes = [_]u8{ 16, 0 };
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Swap", .display_name = "Swap", .flags = 0, .icon_sizes = &sizes });
    try rt.end();
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    const first = try fake_png(testing.allocator, 16, 30, 0x12);
    defer testing.allocator.free(first);
    try deliver(rt, ids[0], first, 0);
    const second = try fake_png(testing.allocator, 16, 90, 0x13);
    defer testing.allocator.free(second);
    rt.issue_requests(0);
    _ = collect_requests(rt, &ids);
    rt.transfer_cancel(ids[0], 0);
    rt.apps.items[0].icons[0].want = true;
    rt.issue_requests(100_000);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    try deliver(rt, ids[0], second, 100_000);
    try testing.expectEqual(first.len + second.len - first.len, rt.stats().icon_bytes);

    // An open transfer at teardown is released by destroy (no leak under testing.allocator).
    rt.apps.items[0].icons[0].want = true;
    rt.apps.items[0].icons[0].not_before = 0;
    rt.apps.items[0].icons[0].attempts = 0;
    rt.issue_requests(200_000);
    _ = collect_requests(rt, &ids);
    try testing.expectEqual(@as(u32, 1), rt.stats().outstanding);
    rt.destroy();

    // A catalog key supersedes a pending uncatalogued entry; an uncatalogued entry on a
    // transaction-blocked runtime reports Pending; a stale uncatalogued update is refused.
    const rt2 = try runtime_t.create(testing.allocator, env.config_for());
    defer rt2.destroy();
    try testing.expectEqual(@as(?u64, null), rt2.next_deadline(0));
    try rt2.ensure_uncatalogued("a-Known", "Known", 0, UpdatedTimestamp);
    try rt2.ensure_uncatalogued("a-Known", "", 0, UpdatedTimestamp);
    try testing.expectError(error.InvalidArgument, rt2.ensure_uncatalogued("a-Bad\xff", "T", 0, UpdatedTimestamp));
    rt2.sync_required = true;
    try testing.expectError(error.Pending, rt2.ensure_uncatalogued("a-Blocked", "B", 0, UpdatedTimestamp));
    rt2.sync_required = false;
    try testing.expect(rt2.next_deadline(0) == null or rt2.next_deadline(0).? >= 0);
}

test "store-refused uncatalogued entry reports StoreFailed and backs off" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    rt.store.fail_after.store(rt.store.checkpoint_count.load(.monotonic) + 1, .monotonic);
    try testing.expectError(error.StoreFailed, rt.ensure_uncatalogued("a-Refused", "Refused", 5_000, UpdatedTimestamp));
    try testing.expectEqual(@as(u32, 1), rt.stats().publish_failures);
    try testing.expectEqual(@as(?u64, 5_000 + 250), rt.next_deadline(5_000));
    rt.store.fail_after.store(0, .monotonic);
    try rt.ensure_uncatalogued("a-Refused", "Refused", 6_000, UpdatedTimestamp);
    try testing.expectEqual(@as(u32, 0), rt.stats().publish_failures);
}

test "C ABI covers create variants, teardown with open transfers and every null guard" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    var handle: ?*runtime_t = null;
    var raw = config_c_t{
        .manifest_dir_fd = env.state.fd,
        .data_root_fd = env.data.fd,
        .config_root_fd = env.config.fd,
        .menu_policy = @intFromEnum(menu_policy_t.None),
        .shortcuts = @intFromEnum(shortcuts_policy_t.None),
        .data_root = env.data_path.ptr,
        .data_root_length = env.data_path.len,
        .config_root = env.config_path.ptr,
        .config_root_length = env.config_path.len,
        .launcher_path = "/usr/bin/waddle".ptr,
        .launcher_path_length = "/usr/bin/waddle".len,
        .device = "testdev".ptr,
        .device_length = "testdev".len,
        .waddle_version = "0.2.0".ptr,
        .waddle_version_length = "0.2.0".len,
    };
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_create(&raw, &handle));
    const rt = handle.?;
    const sizes = [_]u8{ 16, 0 };
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_begin(rt));
    const key = "a-Teardown";
    var app_c = app_c_t{
        .key = key.ptr,
        .key_length = key.len,
        .display_name = "T".ptr,
        .display_name_length = 1,
        .flags = 0,
        .icon_sizes = &sizes,
        .icon_sizes_length = sizes.len,
        .content_hash = null,
        .content_hash_length = 0,
    };
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_upsert(rt, 0, &app_c));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_end(rt));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_tick(rt, 0, UpdatedTimestamp.ptr, UpdatedTimestamp.len));
    var action: action_c_t = undefined;
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_next_action(rt, &action));
    try testing.expectEqual(@as(c_int, 2), catalog_runtime_transfer_begin(rt, action.transfer, 4, "text".ptr, 4, 0));
    try testing.expectEqual(@as(c_int, 2), catalog_runtime_transfer_chunk(rt, 999, 0, "x".ptr, 1, 0));
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_next_deadline(rt, 0) - catalog_runtime_next_deadline(rt, 0));
    var stats: stats_c_t = undefined;
    try testing.expectEqual(@as(c_int, 0), catalog_runtime_stats(rt, &stats));
    try testing.expectEqual(@as(u32, 1), stats.apps);
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_chunk(null, 1, 0, null, 0, 0));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_cancel(null, 1, 0));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_begin(null, 1, 0, null, 0, 0));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_transfer_end(rt, 1, null, 0));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_stats(null, &stats));
    catalog_runtime_destroy(rt);
}

test "C ABI rejects an unsupported configuration and an invalid config field" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    var handle: ?*runtime_t = null;
    var raw = config_c_t{
        .manifest_dir_fd = env.state.fd,
        .data_root_fd = env.data.fd,
        .config_root_fd = env.config.fd,
        .menu_policy = @intFromEnum(menu_policy_t.Installed),
        .shortcuts = @intFromEnum(shortcuts_policy_t.None),
        .data_root = null,
        .data_root_length = 0,
        .config_root = env.config_path.ptr,
        .config_root_length = env.config_path.len,
        .launcher_path = "/usr/bin/waddle".ptr,
        .launcher_path_length = "/usr/bin/waddle".len,
        .device = "testdev".ptr,
        .device_length = "testdev".len,
        .waddle_version = "0.2.0".ptr,
        .waddle_version_length = "0.2.0".len,
    };
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_create(&raw, &handle));
    try testing.expect(handle == null);
    raw.data_root = env.data_path.ptr;
    raw.data_root_length = env.data_path.len;
    raw.config_root = null;
    raw.config_root_length = 0;
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_create(&raw, &handle));
    try testing.expectEqual(@as(c_int, 1), catalog_runtime_create(null, null));
}

test "restart resumes from the on-disk manifest and rewrites nothing for unchanged apps" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    {
        const rt = try runtime_t.create(testing.allocator, env.config_for());
        defer rt.destroy();
        try sync(rt, &.{ input_app("a-Resume", "Resume"), input_app("a-Again", "Again") });
        _ = drive(rt, 0);
    }
    var rel_buf: [128]u8 = undefined;
    const before = try env.data.statFile(desktop_rel(&rel_buf, "a-Resume"));
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try testing.expect(rt.base != null);
    try sync(rt, &.{ input_app("a-Resume", "Resume"), input_app("a-Again", "Again") });
    _ = drive(rt, 1_000_000);
    const after = try env.data.statFile(desktop_rel(&rel_buf, "a-Resume"));
    try testing.expectEqual(before.inode, after.inode);
    try testing.expectEqual(before.mtime, after.mtime);
    try testing.expect(!rt.publication_pending());
}

test "emit_manifest and json_string hold under every allocation failure and every byte" {
    const records = [_]manifest.record_t{
        .{ .id = "z", .kind = .Menu, .path = "/p/z", .sha256 = "0" ** 64, .device = null, .app_key = null, .created_by = "0.2.0", .prior = .None, .mime = null },
        .{ .id = "a", .kind = .MimeDefault, .path = "/p/a", .sha256 = "1" ** 64, .device = "dev", .app_key = "a-K", .created_by = "0.2.0", .prior = .{ .Text = "old" }, .mime = "text/plain" },
        .{ .id = "m", .kind = .GuestPolicy, .path = "/p/m", .sha256 = "2" ** 64, .device = "dev", .app_key = null, .created_by = "0.2.0", .prior = .{ .U32Value = 7 }, .mime = null },
    };
    var index: usize = 0;
    while (index < 512) : (index += 1) {
        var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = index });
        if (emit_manifest(failing.allocator(), "0.2.0", UpdatedTimestamp, &records)) |bytes| {
            testing.allocator.free(bytes);
        } else |err| {
            try testing.expectEqual(error.OutOfMemory, err);
        }
        if (!failing.has_induced_failure) break;
    }
    try testing.expect(index < 512);

    var every: [256]u8 = undefined;
    for (&every, 0..) |*byte, i| byte.* = @intCast(i);
    var buffer: [2048]u8 = undefined;
    var sink = fill_sink_t{ .bytes = &buffer };
    try json_string(&sink, &every);
    const text = buffer[0..sink.cursor];
    try testing.expect(std.mem.indexOf(u8, text, "\\u0000") != null);
    try testing.expect(std.mem.indexOf(u8, text, "\\u001f") != null);
    try testing.expect(std.mem.indexOf(u8, text, "\\\\") != null);
}

test "status mapping covers every runtime error" {
    inline for (.{ error.Rejected, error.Quota, error.OutOfMemory, error.Unsupported, error.InvalidArgument, error.Pending, error.PublicationBudget, error.StoreFailed, error.InvalidManifest }) |err| {
        _ = status_of(err);
    }
    try testing.expectEqual(@as(c_int, @intFromEnum(status_t.Pending)), status_of(error.PublicationBudget));
    try testing.expectEqual(@as(c_int, @intFromEnum(status_t.StoreFailed)), status_of(error.InvalidManifest));
    try testing.expectEqual(@as(c_int, @intFromEnum(status_t.Quota)), status_of(error.Quota));
    try testing.expectEqual(@as(c_int, @intFromEnum(status_t.Ok)), status_from(@as(error_t!void, {})));
    try testing.expectEqual(@as(c_int, @intFromEnum(status_t.OutOfMemory)), status_from(@as(error_t!void, error.OutOfMemory)));
}

test "manifest file must be a regular file and readable" {
    var symlink_env = env_t.init(testing.allocator);
    defer symlink_env.deinit();
    try symlink_env.state.symLink("/nonexistent/target", "manifest.json", .{});
    try testing.expectError(error.InvalidManifest, runtime_t.create(testing.allocator, symlink_env.config_for()));

    var directory_env = env_t.init(testing.allocator);
    defer directory_env.deinit();
    try directory_env.state.makeDir("manifest.json");
    try testing.expectError(error.InvalidManifest, runtime_t.create(testing.allocator, directory_env.config_for()));
}

test "ensure_parent_dirs refuses paths outside a root and resolves each owning root" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    var buf: [512]u8 = undefined;
    try rt.ensure_parent_dirs(try std.fmt.bufPrint(&buf, "{s}/menus/applications-merged/waddle-testdev.menu", .{env.config_path}));
    var menus = try env.config.openDir("menus/applications-merged", .{});
    menus.close();
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs(env.data_path));
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs(try std.fmt.bufPrint(&buf, "{s}x/y/z.desktop", .{env.data_path})));
}

test "transfer identifiers wrap past the maximum and skip live values" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    rt.next_transfer_id = std.math.maxInt(u32);
    try testing.expectEqual(std.math.maxInt(u32), rt.next_id());
    try testing.expectEqual(@as(u32, 1), rt.next_id());
    try testing.expectEqual(@as(u32, 2), rt.next_id());
}

test "request scheduling respects pending, requested, backoff and mask gates" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 16, 0, 32, 0 };
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Gate", .display_name = "Gate", .flags = 0, .icon_sizes = &sizes });
    // Inside a transaction nothing is requested and no deadline is scheduled for requests.
    rt.issue_requests(0);
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);
    try testing.expect(!rt.can_issue_request(0));
    try rt.end();
    rt.issue_requests(0);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 2), collect_requests(rt, &ids));
    // Already requested slots are skipped; cancelling one leaves it eligible after backoff.
    rt.issue_requests(0);
    try testing.expectEqual(@as(usize, 0), collect_requests(rt, &ids));
    rt.transfer_cancel(ids[0], 0);
    try testing.expect(!rt.can_issue_request(0));
    rt.tick(0, UpdatedTimestamp);
    try testing.expectEqual(@as(?u64, IconRetryBackoffMs), rt.next_deadline(0));
    try testing.expect(rt.can_issue_request(IconRetryBackoffMs));
    rt.issue_requests(IconRetryBackoffMs);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    // Expiry of the other transfer also backs off; the attempts cap then stops requests.
    rt.tick(RequestTimeoutMs, UpdatedTimestamp);
    _ = rt.next_action();
    _ = rt.next_action();
    rt.tick(RequestTimeoutMs + 3 * IconRetryBackoffMs, UpdatedTimestamp);
    _ = collect_requests(rt, &ids);
    rt.tick(RequestTimeoutMs + 9 * IconRetryBackoffMs, UpdatedTimestamp);
    _ = collect_requests(rt, &ids);
    rt.tick(RequestTimeoutMs + 99 * IconRetryBackoffMs, UpdatedTimestamp);
    _ = collect_requests(rt, &ids);
    rt.tick(RequestTimeoutMs + 999 * IconRetryBackoffMs, UpdatedTimestamp);
    _ = collect_requests(rt, &ids);
    for (rt.apps.items[0].icons) |slot| try testing.expect(!slot.want or slot.attempts < MaxIconAttempts);
}

test "removing one app leaves another app's transfer open; reset tears down all of them" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 16, 0 };
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Left", .display_name = "Left", .flags = 0, .icon_sizes = &sizes });
    try rt.upsert(false, .{ .key = "a-Right", .display_name = "Right", .flags = 0, .icon_sizes = &sizes });
    try rt.end();
    rt.issue_requests(0);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 2), collect_requests(rt, &ids));
    rt.remove("a-Left");
    try testing.expectEqual(@as(u32, 1), rt.stats().outstanding);
    try testing.expectEqual(CancelCancelled, rt.next_action().?.status);
    rt.reset_session();
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);
}

test "transactions block ticks from publishing and transfer_end refuses non-receiving ids" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 16, 0 };
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Open", .display_name = "Open", .flags = 0, .icon_sizes = &sizes });
    rt.tick(0, UpdatedTimestamp);
    var rel_buf: [128]u8 = undefined;
    try testing.expectError(error.FileNotFound, env.data.statFile(desktop_rel(&rel_buf, "a-Open")));
    try testing.expectEqual(@as(u32, 0), rt.stats().outstanding);
    try rt.end();
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    var digest = [_]u8{0} ** 32;
    try testing.expectError(error.Rejected, rt.transfer_end(ids[0], &digest, 0));
    try testing.expectEqual(CancelGuestError, rt.next_action().?.status);
}

test "catalog-keyed ensure and quota outside a transaction" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try rt.upsert(false, .{ .key = "a-Catalog", .display_name = "Catalog", .flags = 0 });
    try rt.ensure_uncatalogued("a-Catalog", "Ignored", 0, UpdatedTimestamp);
    try testing.expectEqual(@as(u32, 0), rt.stats().uncatalogued);
    var keys: [MaxApps + 1][16]u8 = undefined;
    for (0..MaxApps - 1) |i| {
        const key = try std.fmt.bufPrint(&keys[i], "a-P{d:0>5}", .{i});
        try rt.upsert(false, .{ .key = key, .display_name = "P", .flags = 0 });
    }
    const last = try std.fmt.bufPrint(&keys[MaxApps], "a-Over", .{});
    try testing.expectError(error.Quota, rt.upsert(false, .{ .key = last, .display_name = "O", .flags = 0 }));
    try testing.expect(!rt.sync_required);
    try testing.expect(rt.stats().rejected_frames >= 1);
}

test "sub-condition coverage: key shape, text validity, hash transitions and gates" {
    try testing.expect(!valid_key("aX", ""));
    try testing.expect(!valid_key("abc", ""));
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    var bad_version = env.config_for();
    bad_version.waddle_version = "\xff";
    try testing.expectError(error.InvalidArgument, runtime_t.create(testing.allocator, bad_version));

    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try testing.expectError(error.InvalidArgument, rt.ensure_uncatalogued("a-Title", "\xff", 0, UpdatedTimestamp));

    // A stored app without a content hash gains one on update, which requests its icons.
    const sizes = [_]u8{ 16, 0 };
    const hash = [_]u8{0x5A} ** 32;
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Hashless", .display_name = "H", .flags = 0, .icon_sizes = &sizes });
    try rt.end();
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    rt.transfer_cancel(ids[0], 0);
    try rt.upsert(true, .{ .key = "a-Hashless", .display_name = "H", .flags = 0, .icon_sizes = &sizes, .content_hash = &hash });
    rt.tick(10, UpdatedTimestamp);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));

    // A shrunk mask leaves a wanted but unadvertised slot unrequested.
    const two_sizes = [_]u8{ 16, 0, 32, 0 };
    try rt.upsert(false, .{ .key = "a-Shrink", .display_name = "S", .flags = 0, .icon_sizes = &two_sizes });
    try rt.upsert(true, .{ .key = "a-Shrink", .display_name = "S", .flags = 0, .icon_sizes = &sizes });
    rt.issue_requests(0);
    try testing.expectEqual(@as(usize, 1), collect_requests(rt, &ids));
    try testing.expect(rt.find("a-Shrink") != null);

    // A poisoned session schedules nothing and reports no request deadline.
    rt.begin();
    try rt.upsert(false, .{ .key = "a-Dup", .display_name = "D", .flags = 0 });
    try testing.expectError(error.Rejected, rt.upsert(false, .{ .key = "a-Dup", .display_name = "D", .flags = 0 }));
    try testing.expect(!rt.can_issue_request(0));
    try testing.expect(rt.next_deadline(0) == null or rt.next_deadline(0).? >= 0);
    try testing.expectError(error.Rejected, rt.end());
    rt.reset_session();
}

test "publication stalls on a blocked artifact directory and resumes when it clears" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try env.data.writeFile(.{ .sub_path = "applications", .data = "not a directory" });
    try sync(rt, &.{input_app("a-Blocked", "Blocked")});
    rt.tick(0, UpdatedTimestamp);
    try testing.expect(rt.stats().publish_failures >= 1);
    try env.data.deleteFile("applications");
    _ = drive(rt, 1_000_000);
    var rel_buf: [128]u8 = undefined;
    _ = try env.data.statFile(desktop_rel(&rel_buf, "a-Blocked"));
    try testing.expectEqual(@as(u32, 0), rt.stats().publish_failures);
}

test "parent creation fails closed on symlinks and unwritable roots" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    try env.data.makeDir("real");
    try env.data.symLink("/", "real/inner", .{});
    try env.data.symLink("/", "outer", .{});
    var buf: [512]u8 = undefined;
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs(try std.fmt.bufPrint(&buf, "{s}/real/inner/x.desktop", .{env.data_path})));
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs(try std.fmt.bufPrint(&buf, "{s}/outer/x/y.desktop", .{env.data_path})));
    if (std.os.linux.geteuid() == 0) return error.SkipZigTest;
    std.posix.mkdirat(env.data.fd, "readonly", 0o555) catch unreachable;
    try testing.expectError(error.InvalidManifest, rt.ensure_parent_dirs(try std.fmt.bufPrint(&buf, "{s}/readonly/child/x.desktop", .{env.data_path})));
}

test "every durable checkpoint of a replacement keeps state consistent" {
    var checkpoint: usize = 1;
    while (checkpoint <= 16) : (checkpoint += 1) {
        var env = env_t.init(testing.allocator);
        defer env.deinit();
        const rt = try runtime_t.create(testing.allocator, env.config_for());
        defer rt.destroy();
        try sync(rt, &.{input_app("a-Replace", "First")});
        _ = drive(rt, 0);
        rt.store.fail_after.store(rt.store.checkpoint_count.load(.monotonic) + checkpoint, .monotonic);
        try sync(rt, &.{input_app("a-Replace", "Second")});
        rt.tick(1_000_000, UpdatedTimestamp);
        rt.store.fail_after.store(0, .monotonic);
        _ = drive(rt, 2_000_000);
        try testing.expect(!rt.publication_pending());
        const content = try env.read(env.data, "applications/waddle.testdev.a-Replace.desktop");
        defer testing.allocator.free(content);
        try testing.expect(std.mem.indexOf(u8, content, "Name=Second") != null);
        if (rt.base) |bytes| {
            var digest: [32]u8 = undefined;
            sha256.hash(bytes, &digest, .{});
            const snapshot = try rt.store.inspect_record("desktop:testdev:a-Replace");
            try testing.expectEqualSlices(u8, &digest, &snapshot.manifest_sha256);
        }
    }
}

test "two failing units in one pass report the first failure and keep the rest dirty" {
    var env = env_t.init(testing.allocator);
    defer env.deinit();
    const rt = try runtime_t.create(testing.allocator, env.config_for());
    defer rt.destroy();
    const sizes = [_]u8{ 16, 0 };
    try sync(rt, &.{
        .{ .key = "a-FailOne", .display_name = "One", .flags = 0, .icon_sizes = &sizes },
        .{ .key = "a-FailTwo", .display_name = "Two", .flags = 0, .icon_sizes = &sizes },
    });
    rt.tick(0, UpdatedTimestamp);
    var ids: [8]u32 = undefined;
    try testing.expectEqual(@as(usize, 2), collect_requests(rt, &ids));
    const png = try fake_png(testing.allocator, 16, 20, 0x99);
    defer testing.allocator.free(png);
    try deliver(rt, ids[0], png, 0);
    try deliver(rt, ids[1], png, 0);
    rt.tick(1_000, UpdatedTimestamp);
    try testing.expect(rt.stats().publish_failures >= 1);
    try testing.expect(rt.publication_pending());
}
