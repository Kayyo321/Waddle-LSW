const std = @import("std");
const layout = @import("interop_layout.zig");
const heap = @import("interop_heap.zig");

/// Frozen number of slot descriptors owned by one window incarnation.
pub const SlotsPerWindow: usize = 3;
/// Frozen maximum number of concurrently admitted windows.
pub const MaxWindows: usize = @intCast(layout.SlotCount / SlotsPerWindow);

/// Errors returned by fixed layout-v3 slot lifetime bookkeeping.
pub const slot_runtime_error_t = error{
    InvalidSessionEpoch, // Session epoch is zero.
    WrongSessionEpoch, // Caller work belongs to a different runtime epoch.
    InvalidWindow, // Window identity is zero.
    InvalidGeneration, // Window generation is zero.
    StaleGeneration, // Generation is not greater than prior admissions.
    DuplicateWindow, // Window identity is already admitted.
    NoWindowCapacity, // All 64 descriptor triples are occupied.
    UnknownWindow, // Window/generation pair is not admitted.
    InvalidSlot, // Slot is outside the fixed three-slot window range.
    InvalidGeometry, // Requested frame geometry is invalid.
    HeapFull, // No contiguous heap run satisfies the request.
    TicketExhausted, // The heap allocator cannot issue another run identity.
    Busy, // A previous growth triple is still pending.
    SlotTooSmall, // Current slot capacity cannot satisfy desired geometry.
    WriterAlreadyActive, // One provisional writer already exists for this window.
    NoWriter, // No matching provisional claim exists.
    WriterStateMismatch, // Caller transition evidence conflicts with the claim state.
    InvalidSequence, // Sequence is zero, reused, or not strictly increasing.
    SequenceExhausted, // Sequence reached its maximum and cannot wrap.
    WrongRelease, // Release identity or sequence does not match a published pin.
    SlotPinned, // Slot/window is still pinned or has a provisional writer.
    InvalidLifecycle, // Operation is invalid for the current runtime/window lifecycle.
    MissingProof, // Caller did not provide required producer/host teardown evidence.
    HeapStateCorrupt, // A retained heap token no longer matches allocator ownership.
    FailedClosed, // Runtime encountered an inconsistency and permits drain only.
};

/// One exact heap reservation exposed for a descriptor operation.
///
/// @note The value is a snapshot. The runtime retains ownership of the run;
///       callers must not release it or retain mapping pointers through it.
pub const slot_view_t = struct {
    /// Frozen descriptor index in the shared 192-entry table.
    descriptor_index: u32,
    /// Window identity bound to this descriptor.
    window: u64,
    /// Monotonically increasing agent-session generation.
    generation: u64,
    /// Latest desired, clamped frame geometry for a future publication.
    frame_layout: layout.frame_layout_t,
    /// Current allocation identity and mapping-relative byte range.
    run: heap.heap_run_t,
};

const slot_record_t = struct {
    active: ?heap.heap_run_t = null,
    replacement: ?heap.heap_run_t = null,
    retired: ?heap.heap_run_t = null,
    pinned_sequence: ?u64 = null,
    provisional_sequence: ?u64 = null,
    writer_entered: bool = false,
};

const window_record_t = struct {
    active: bool = false,
    retiring: bool = false,
    window: u64 = 0,
    generation: u64 = 0,
    desired_layout: layout.frame_layout_t = .{
        .width = 0,
        .height = 0,
        .stride = 0,
        .pixel_bytes = 0,
        .data_bytes = 0,
    },
    last_sequence: u64 = 0,
    writer_slot: ?u8 = null,
    slots: [SlotsPerWindow]slot_record_t = [_]slot_record_t{.{}} ** SlotsPerWindow,
};

const lifecycle_t = enum(u8) { Active, Draining, Drained };

/// Fixed, allocation-free bookkeeping for one layout-v3 session.
///
/// @note The caller owns this value. It owns heap reservations and slot pins,
///       but no mapping, OS handle, frame bytes, or host resource. All methods
///       require one externally serialized guest event-loop thread. Shared
///       state atomics are performed by the separate native adapter; this type
///       only records caller-confirmed transition outcomes.
pub const slot_runtime_t = struct {
    /// Immutable validated nonzero epoch for this runtime instance.
    session_epoch: u64,
    /// Highest generation successfully admitted in this session; never decreases.
    last_generation: u64 = 0,
    /// Lifecycle gate; only this module advances it.
    lifecycle: lifecycle_t = .Active,
    /// Sticky fail-closed bit after post-publication or heap bookkeeping inconsistency.
    failed_closed: bool = false,
    /// Fixed allocator whose tokens are owned exclusively by this runtime.
    allocator: heap.heap_allocator_t,
    /// Fixed 64-window table; descriptor indices are each table index times three.
    windows: [MaxWindows]window_record_t = [_]window_record_t{.{}} ** MaxWindows,

    /// Initialize a fresh caller-owned runtime for one validated session.
    ///
    /// @param[in] session_epoch Nonzero validated and fresh layout epoch.
    /// @return Empty runtime, or InvalidSessionEpoch for zero.
    /// @note No allocation or mapping access. Caller guarantees uniqueness and
    ///       must not create a replacement runtime for the same mapping until
    ///       the prior runtime accepted explicit host-drained proof.
    pub fn init(session_epoch: u64) slot_runtime_error_t!slot_runtime_t {
        if (session_epoch == 0) return error.InvalidSessionEpoch;
        return .{ .session_epoch = session_epoch, .allocator = heap.heap_allocator_t.init(session_epoch) catch return error.InvalidSessionEpoch };
    }

    /// Admit a window and reserve its complete three-slot allocation atomically.
    ///
    /// @param[in,out] self Active externally serialized session runtime.
    /// @param[in] window Nonzero HWND-like identity unique among admitted windows.
    /// @param[in] generation Nonzero generation greater than every prior admission.
    /// @param[in] requested_width Requested capture width; layout clamps to frozen maximum.
    /// @param[in] requested_height Requested capture height; layout clamps to frozen maximum.
    /// @param[out] output Caller-owned result; unchanged on failure.
    /// @return Three fixed descriptor indices and reservations, or a documented error.
    /// @note Runtime retains all tokens. No mapping access or internal synchronization.
    pub fn admit_window(
        self: *slot_runtime_t,
        window: u64,
        generation: u64,
        requested_width: u32,
        requested_height: u32,
        output: *[SlotsPerWindow]slot_view_t,
    ) slot_runtime_error_t!void {
        try self.ensure_active();
        if (window == 0) return error.InvalidWindow;
        if (generation == 0) return error.InvalidGeneration;
        if (generation <= self.last_generation) return error.StaleGeneration;
        if (self.find_window(window, generation) != null) return error.DuplicateWindow;
        if (self.find_any_window(window) != null) return error.DuplicateWindow;
        const layout_value = layout.frame_layout_for_capture(requested_width, requested_height) catch
            return error.InvalidGeometry;
        const free_index = self.find_free_window() orelse return error.NoWindowCapacity;

        var staged_allocator = self.allocator;
        var staged_runs: [SlotsPerWindow]heap.heap_run_t = undefined;
        for (&staged_runs) |*run| {
            staged_allocator.allocate(layout_value.data_bytes, run) catch |err| return map_heap_error(err);
        }

        var result: [SlotsPerWindow]slot_view_t = undefined;
        for (0..SlotsPerWindow) |slot_index| {
            result[slot_index] = .{
                .descriptor_index = @intCast(free_index * SlotsPerWindow + slot_index),
                .window = window,
                .generation = generation,
                .frame_layout = layout_value,
                .run = staged_runs[slot_index],
            };
        }

        var record = window_record_t{
            .active = true,
            .window = window,
            .generation = generation,
            .desired_layout = layout_value,
        };
        for (0..SlotsPerWindow) |slot_index| record.slots[slot_index].active = staged_runs[slot_index];
        self.allocator = staged_allocator;
        self.windows[free_index] = record;
        self.last_generation = generation;
        output.* = result;
    }

    /// Update future frame geometry or stage a complete replacement triple.
    ///
    /// @param[in,out] self Active externally serialized runtime.
    /// @param[in] window Exact admitted window identity.
    /// @param[in] generation Exact admitted incarnation.
    /// @param[in] requested_width Requested width, clamped by the frozen layout helper.
    /// @param[in] requested_height Requested height, clamped by the frozen layout helper.
    /// @return void, Busy for an existing growth, HeapFull, or identity/geometry error.
    /// @note Same-capacity and shrink changes affect future publications only;
    ///       each existing pin retains the exact run and published sequence.
    pub fn request_geometry(
        self: *slot_runtime_t,
        window: u64,
        generation: u64,
        requested_width: u32,
        requested_height: u32,
    ) slot_runtime_error_t!void {
        try self.ensure_active();
        const index = try self.require_window(window, generation);
        const desired = layout.frame_layout_for_capture(requested_width, requested_height) catch
            return error.InvalidGeometry;
        const record = &self.windows[index];
        if (record.retiring) return error.InvalidLifecycle;
        var minimum_capacity: u64 = std.math.maxInt(u64);
        for (record.slots) |slot_record| {
            const capacity = slot_record.active orelse return error.InvalidLifecycle;
            minimum_capacity = @min(minimum_capacity, capacity.capacity_bytes);
        }
        if (@as(u64, desired.data_bytes) <= minimum_capacity) {
            record.desired_layout = desired;
            return;
        }
        if (has_growth(record)) return error.Busy;

        var staged_allocator = self.allocator;
        var replacements: [SlotsPerWindow]heap.heap_run_t = undefined;
        for (&replacements) |*run| {
            staged_allocator.allocate(desired.data_bytes, run) catch |err| return map_heap_error(err);
        }
        self.allocator = staged_allocator;
        record.desired_layout = desired;
        for (0..SlotsPerWindow) |slot_index| record.slots[slot_index].replacement = replacements[slot_index];
    }

    /// Prepare one provisional writer claim before attempting Free-to-Writing.
    ///
    /// @param[in,out] self Active runtime; caller owns no returned reservation.
    /// @param[in] session_epoch Exact epoch captured when asynchronous writer work was scheduled.
    /// @param[in] window Exact window identity.
    /// @param[in] generation Exact window generation.
    /// @param[in] slot Slot number 0..2.
    /// @param[in] frame_sequence Nonzero sequence greater than every prior claim in this incarnation.
    /// @param[out] output Caller-owned view, unchanged on failure.
    /// @return Current descriptor view or a documented state/identity error;
    ///         WrongSessionEpoch leaves runtime and output unchanged.
    /// @note Claim is recorded before caller's atomic CAS. At most one writer per window.
    pub fn prepare_publish(
        self: *slot_runtime_t,
        session_epoch: u64,
        window: u64,
        generation: u64,
        slot: u8,
        frame_sequence: u64,
        output: *slot_view_t,
    ) slot_runtime_error_t!void {
        if (session_epoch != self.session_epoch) return error.WrongSessionEpoch;
        try self.ensure_active();
        const index = try self.require_window(window, generation);
        const record = &self.windows[index];
        if (record.retiring) return error.InvalidLifecycle;
        if (slot >= SlotsPerWindow) return error.InvalidSlot;
        if (frame_sequence == 0) return error.InvalidSequence;
        if (record.last_sequence == std.math.maxInt(u64)) return error.SequenceExhausted;
        if (frame_sequence <= record.last_sequence) return error.InvalidSequence;
        if (record.writer_slot != null) return error.WriterAlreadyActive;

        const slot_index: usize = slot;
        const slot_record_before_switch = &record.slots[slot_index];
        if (slot_record_before_switch.pinned_sequence != null or slot_record_before_switch.provisional_sequence != null)
            return error.SlotPinned;
        self.switch_slot_if_ready(index, slot_index);
        try self.complete_growth(index);
        const slot_record = &record.slots[slot_index];
        const run = slot_record.active orelse return error.InvalidLifecycle;
        if (run.capacity_bytes < record.desired_layout.data_bytes) return error.SlotTooSmall;

        const view = slot_view_t{
            .descriptor_index = @intCast(index * SlotsPerWindow + slot_index),
            .window = window,
            .generation = generation,
            .frame_layout = record.desired_layout,
            .run = run,
        };
        record.writer_slot = slot;
        slot_record.provisional_sequence = frame_sequence;
        slot_record.writer_entered = false;
        record.last_sequence = frame_sequence;
        output.* = view;
    }

    /// Record that caller's Free-to-Writing atomic transition succeeded.
    ///
    /// @param[in,out] self Runtime with one matching provisional claim.
    /// @param[in] session_epoch Exact epoch captured when asynchronous writer work was scheduled.
    /// @param[in] window Window identity; @param[in] generation Incarnation.
    /// @param[in] slot Claimed slot; @param[in] frame_sequence Claimed sequence.
    /// @return void, WrongSessionEpoch without mutation, or a writer-state error.
    ///         No shared atomic is performed here.
    /// @note Call only after native adapter confirms Free-to-Writing succeeded.
    pub fn mark_writer_entered(
        self: *slot_runtime_t,
        session_epoch: u64,
        window: u64,
        generation: u64,
        slot: u8,
        frame_sequence: u64,
    ) slot_runtime_error_t!void {
        if (session_epoch != self.session_epoch) return error.WrongSessionEpoch;
        try self.ensure_active();
        const index = try self.require_window(window, generation);
        const record = &self.windows[index];
        const slot_record = try self.require_writer(record, slot, frame_sequence);
        if (slot_record.writer_entered) return error.WriterStateMismatch;
        slot_record.writer_entered = true;
    }

    /// Commit the published pin after caller confirms Writing-to-Ready.
    ///
    /// @param[in,out] self Runtime with a matching entered provisional writer.
    /// @param[in] session_epoch Exact epoch captured when asynchronous writer work was scheduled.
    /// @param[in] window Window identity; @param[in] generation Incarnation.
    /// @param[in] slot Claimed slot; @param[in] frame_sequence Published sequence.
    /// @return void on commit; WrongSessionEpoch leaves state unchanged, while a
    ///         current-epoch publication mismatch fails closed and retains runs.
    /// @note Call only after the native adapter confirms Writing-to-Ready.
    pub fn commit_publish(
        self: *slot_runtime_t,
        session_epoch: u64,
        window: u64,
        generation: u64,
        slot: u8,
        frame_sequence: u64,
    ) slot_runtime_error_t!void {
        if (session_epoch != self.session_epoch) return error.WrongSessionEpoch;
        if (self.failed_closed) return error.FailedClosed;
        const index = self.find_window(window, generation) orelse {
            self.failed_closed = true;
            return error.FailedClosed;
        };
        const record = &self.windows[index];
        const slot_record = self.require_writer(record, slot, frame_sequence) catch {
            self.failed_closed = true;
            return error.FailedClosed;
        };
        if (!slot_record.writer_entered or slot_record.pinned_sequence != null) {
            self.failed_closed = true;
            return error.FailedClosed;
        }
        slot_record.provisional_sequence = null;
        slot_record.writer_entered = false;
        slot_record.pinned_sequence = frame_sequence;
        record.writer_slot = null;
    }

    /// Cancel a provisional claim only when Free-to-Writing never succeeded.
    ///
    /// @param[in,out] self Runtime with matching, not-entered provisional writer.
    /// @param[in] session_epoch Exact epoch captured when asynchronous writer work was scheduled.
    /// @param[in] window Window identity; @param[in] generation Incarnation.
    /// @param[in] slot Claimed slot; @param[in] frame_sequence Claimed sequence.
    /// @return void, WrongSessionEpoch without mutation, or a writer-state error.
    /// @note Sequence remains consumed; this function never frees heap runs.
    pub fn cancel_unentered(
        self: *slot_runtime_t,
        session_epoch: u64,
        window: u64,
        generation: u64,
        slot: u8,
        frame_sequence: u64,
    ) slot_runtime_error_t!void {
        if (session_epoch != self.session_epoch) return error.WrongSessionEpoch;
        try self.ensure_active();
        const index = try self.require_window(window, generation);
        const record = &self.windows[index];
        const slot_record = try self.require_writer(record, slot, frame_sequence);
        if (slot_record.writer_entered) return error.WriterStateMismatch;
        clear_writer(record, slot_record);
        try self.finalize_retired(index);
    }

    /// Cancel an entered writer only after confirmed Writing-to-Free.
    ///
    /// @param[in,out] self Runtime with matching entered provisional writer.
    /// @param[in] session_epoch Exact epoch captured when asynchronous writer work was scheduled.
    /// @param[in] window Window identity; @param[in] generation Incarnation.
    /// @param[in] slot Claimed slot; @param[in] frame_sequence Claimed sequence.
    /// @return void, WrongSessionEpoch without mutation, or a writer-state error.
    /// @note Caller must first confirm native Writing-to-Free; no atomic occurs here.
    pub fn cancel_entered(
        self: *slot_runtime_t,
        session_epoch: u64,
        window: u64,
        generation: u64,
        slot: u8,
        frame_sequence: u64,
    ) slot_runtime_error_t!void {
        if (session_epoch != self.session_epoch) return error.WrongSessionEpoch;
        try self.ensure_active();
        const index = try self.require_window(window, generation);
        const record = &self.windows[index];
        const slot_record = try self.require_writer(record, slot, frame_sequence);
        if (!slot_record.writer_entered) return error.WriterStateMismatch;
        clear_writer(record, slot_record);
        try self.finalize_retired(index);
    }

    /// Clear exactly one published pin after a matching FrameRelease arrives.
    ///
    /// @param[in,out] self Active/draining runtime, externally serialized.
    /// @param[in] session_epoch Exact session epoch echoed by the caller's validated control session.
    /// @param[in] window Exact window identity.
    /// @param[in] generation Exact incarnation.
    /// @param[in] slot Descriptor slot 0..2.
    /// @param[in] frame_sequence Exact published sequence.
    /// @return void, or WrongRelease without mutation for any stale/mismatched token.
    /// @note Shared Free state alone never clears this runtime pin.
    pub fn release_frame(
        self: *slot_runtime_t,
        session_epoch: u64,
        window: u64,
        generation: u64,
        slot: u8,
        frame_sequence: u64,
    ) slot_runtime_error_t!void {
        if (self.failed_closed) return error.FailedClosed;
        if (session_epoch != self.session_epoch or slot >= SlotsPerWindow or frame_sequence == 0)
            return error.WrongRelease;
        const index = self.find_window(window, generation) orelse return error.WrongRelease;
        const record = &self.windows[index];
        const slot_record = &record.slots[slot];
        if (slot_record.pinned_sequence == null or slot_record.pinned_sequence.? != frame_sequence)
            return error.WrongRelease;
        slot_record.pinned_sequence = null;
        self.switch_slot_if_ready(index, slot);
        try self.complete_growth(index);
        try self.finalize_retired(index);
    }

    /// Mark a window retiring and quarantine its descriptor triple.
    ///
    /// @param[in,out] self Active runtime.
    /// @param[in] window Exact window identity; @param[in] generation Incarnation.
    /// @return void; resources release automatically only after all writers/pins end.
    /// @note No new publish claim is admitted after this call.
    pub fn retire_window(self: *slot_runtime_t, window: u64, generation: u64) slot_runtime_error_t!void {
        try self.ensure_active();
        const index = try self.require_window(window, generation);
        self.windows[index].retiring = true;
        try self.finalize_retired(index);
    }

    /// Begin session drain after caller has stopped and joined all producers.
    ///
    /// @param[in,out] self Active session runtime.
    /// @param[in] producers_stopped Explicit caller proof all writers are stopped and joined.
    /// @return void or MissingProof/InvalidLifecycle.
    /// @note Blocks admission and new publish claims; pins and runs remain retained.
    pub fn begin_drain(self: *slot_runtime_t, producers_stopped: bool) slot_runtime_error_t!void {
        if (self.lifecycle != .Active) return error.InvalidLifecycle;
        if (!producers_stopped) return error.MissingProof;
        if (!self.failed_closed) {
            for (&self.windows) |*record| {
                if (!record.active) continue;
                if (record.writer_slot != null) return error.SlotPinned;
            }
        }
        self.lifecycle = .Draining;
    }

    /// Release all reservations after explicit host compositor-drain proof.
    ///
    /// @param[in,out] self Draining runtime.
    /// @param[in] host_buffers_destroyed Explicit caller proof all buffers for this epoch are destroyed.
    /// @return void, MissingProof, InvalidLifecycle, or HeapStateCorrupt.
    /// @note This is the only operation that can discard pins without individual releases.
    pub fn confirm_host_drained(self: *slot_runtime_t, host_buffers_destroyed: bool) slot_runtime_error_t!void {
        if (self.lifecycle != .Draining) return error.InvalidLifecycle;
        if (!host_buffers_destroyed) return error.MissingProof;
        var staged_allocator = self.allocator;
        for (&self.windows) |*record| {
            if (!record.active) continue;
            release_record_runs(&staged_allocator, record) catch {
                self.failed_closed = true;
                return error.HeapStateCorrupt;
            };
        }
        self.allocator = staged_allocator;
        self.windows = [_]window_record_t{.{}} ** MaxWindows;
        self.lifecycle = .Drained;
    }

    fn ensure_active(self: *slot_runtime_t) slot_runtime_error_t!void {
        if (self.failed_closed) return error.FailedClosed;
        if (self.lifecycle != .Active) return error.InvalidLifecycle;
    }

    fn find_window(self: *const slot_runtime_t, window: u64, generation: u64) ?usize {
        for (self.windows, 0..) |record, index| {
            if (record.active and record.window == window and record.generation == generation) return index;
        }
        return null;
    }

    fn find_any_window(self: *const slot_runtime_t, window: u64) ?usize {
        for (self.windows, 0..) |record, index| {
            if (record.active and record.window == window) return index;
        }
        return null;
    }

    fn find_free_window(self: *const slot_runtime_t) ?usize {
        for (self.windows, 0..) |record, index| if (!record.active) return index;
        return null;
    }

    fn require_window(self: *const slot_runtime_t, window: u64, generation: u64) slot_runtime_error_t!usize {
        if (window == 0) return error.InvalidWindow;
        if (generation == 0) return error.InvalidGeneration;
        return self.find_window(window, generation) orelse error.UnknownWindow;
    }

    fn require_writer(
        self: *slot_runtime_t,
        record: *window_record_t,
        slot: u8,
        frame_sequence: u64,
    ) slot_runtime_error_t!*slot_record_t {
        _ = self;
        if (slot >= SlotsPerWindow) return error.InvalidSlot;
        if (record.writer_slot == null or record.writer_slot.? != slot) return error.NoWriter;
        const slot_record = &record.slots[slot];
        if (slot_record.provisional_sequence == null or slot_record.provisional_sequence.? != frame_sequence)
            return error.NoWriter;
        return slot_record;
    }

    fn clear_writer(record: *window_record_t, slot_record: *slot_record_t) void {
        slot_record.provisional_sequence = null;
        slot_record.writer_entered = false;
        record.writer_slot = null;
    }

    fn switch_slot_if_ready(self: *slot_runtime_t, window_index: usize, slot_index: usize) void {
        const record = &self.windows[window_index];
        if (record.retiring) return;
        const slot_record = &record.slots[slot_index];
        if (slot_record.pinned_sequence != null or slot_record.provisional_sequence != null) return;
        if (slot_record.retired != null) return;
        if (slot_record.replacement) |replacement| {
            const previous = slot_record.active.?;
            slot_record.active = replacement;
            slot_record.replacement = null;
            slot_record.retired = previous;
        }
    }

    fn complete_growth(self: *slot_runtime_t, window_index: usize) slot_runtime_error_t!void {
        const record = &self.windows[window_index];
        var complete = true;
        for (record.slots) |slot_record| {
            if (slot_record.replacement != null or slot_record.retired == null) {
                complete = false;
                break;
            }
        }
        if (!complete) return;
        var staged_allocator = self.allocator;
        for (&record.slots) |*slot_record| {
            if (slot_record.retired) |old_run| {
                staged_allocator.release(old_run) catch {
                    self.failed_closed = true;
                    return error.HeapStateCorrupt;
                };
            }
        }
        self.allocator = staged_allocator;
        for (&record.slots) |*slot_record| slot_record.retired = null;
    }

    fn finalize_retired(self: *slot_runtime_t, window_index: usize) slot_runtime_error_t!void {
        const record = &self.windows[window_index];
        if (!record.active or !record.retiring or record.writer_slot != null) return;
        for (record.slots) |slot_record| {
            if (slot_record.pinned_sequence != null or slot_record.provisional_sequence != null) return;
        }
        var staged_allocator = self.allocator;
        release_record_runs(&staged_allocator, record) catch {
            self.failed_closed = true;
            return error.HeapStateCorrupt;
        };
        self.allocator = staged_allocator;
        self.windows[window_index] = .{};
    }
};

fn has_growth(record: *const window_record_t) bool {
    for (record.slots) |slot_record| if (slot_record.replacement != null or slot_record.retired != null) return true;
    return false;
}

fn release_record_runs(allocator: *heap.heap_allocator_t, record: *const window_record_t) slot_runtime_error_t!void {
    for (record.slots) |slot_record| {
        if (slot_record.active) |run| allocator.release(run) catch return error.HeapStateCorrupt;
        if (slot_record.replacement) |run| allocator.release(run) catch return error.HeapStateCorrupt;
        if (slot_record.retired) |run| allocator.release(run) catch return error.HeapStateCorrupt;
    }
}

fn map_heap_error(err: heap.heap_error_t) slot_runtime_error_t {
    return switch (err) {
        error.HeapFull => error.HeapFull,
        error.TicketExhausted => error.TicketExhausted,
        else => error.HeapStateCorrupt,
    };
}

// Test-only helpers keep production methods exercised with runtime inputs.
fn call_admit(runtime: *slot_runtime_t, window: u64, generation: u64, width: u32, height: u32, output: *[SlotsPerWindow]slot_view_t) slot_runtime_error_t!void {
    return @call(.never_inline, slot_runtime_t.admit_window, .{ runtime, window, generation, width, height, output });
}

fn call_request_geometry(runtime: *slot_runtime_t, window: u64, generation: u64, width: u32, height: u32) slot_runtime_error_t!void {
    return @call(.never_inline, slot_runtime_t.request_geometry, .{ runtime, window, generation, width, height });
}

fn call_prepare(runtime: *slot_runtime_t, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64, output: *slot_view_t) slot_runtime_error_t!void {
    return @call(.never_inline, slot_runtime_t.prepare_publish, .{ runtime, epoch, window, generation, slot, sequence, output });
}

fn call_release(runtime: *slot_runtime_t, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64) slot_runtime_error_t!void {
    return @call(.never_inline, slot_runtime_t.release_frame, .{ runtime, epoch, window, generation, slot, sequence });
}

fn empty_view() slot_view_t {
    return .{
        .descriptor_index = 99,
        .window = 99,
        .generation = 99,
        .frame_layout = .{ .width = 1, .height = 1, .stride = 4, .pixel_bytes = 4, .data_bytes = layout.PageBytes },
        .run = .{ .session_epoch = 99, .ticket = 99, .start_page = 99, .page_count = 99, .data_offset = 99, .capacity_bytes = 99 },
    };
}

test "admission, descriptor indices, identity and unchanged errors" {
    try std.testing.expectError(error.InvalidSessionEpoch, slot_runtime_t.init(0));
    var runtime = try slot_runtime_t.init(41);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    const before = views;
    try std.testing.expectError(error.InvalidWindow, call_admit(&runtime, 0, 1, 100, 100, &views));
    try std.testing.expectError(error.InvalidGeneration, call_admit(&runtime, 4, 0, 100, 100, &views));
    try std.testing.expectEqualDeep(before, views);
    try std.testing.expectError(error.InvalidGeometry, call_admit(&runtime, 5, 1, 0, 10, &views));
    try std.testing.expectEqualDeep(before, views);
    try call_admit(&runtime, 5, 1, 100, 100, &views);
    try std.testing.expectEqual(@as(u32, 0), views[0].descriptor_index);
    try std.testing.expectEqual(@as(u32, 1), views[1].descriptor_index);
    try std.testing.expectEqual(@as(u32, 2), views[2].descriptor_index);
    try std.testing.expectEqual(@as(u64, 41), views[2].run.session_epoch);
    const heap_before = runtime.allocator;
    try std.testing.expectError(error.DuplicateWindow, call_admit(&runtime, 5, 2, 100, 100, &views));
    try std.testing.expectError(error.StaleGeneration, call_admit(&runtime, 6, 1, 100, 100, &views));
    try std.testing.expectError(error.UnknownWindow, call_request_geometry(&runtime, 5, 99, 100, 100));
    try std.testing.expectEqualDeep(heap_before, runtime.allocator);
}

test "provisional writer has distinct cancellation and exact release pin" {
    var runtime = try slot_runtime_t.init(77);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 9, 3, 128, 64, &views);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 9, 3, 0, 1, &view);
    try std.testing.expectError(error.WriterAlreadyActive, call_prepare(&runtime, runtime.session_epoch, 9, 3, 1, 2, &view));
    try std.testing.expectError(error.WriterStateMismatch, runtime.cancel_entered(runtime.session_epoch, 9, 3, 0, 1));
    try runtime.cancel_unentered(runtime.session_epoch, 9, 3, 0, 1);
    try std.testing.expectError(error.InvalidSequence, call_prepare(&runtime, runtime.session_epoch, 9, 3, 0, 1, &view));
    try call_prepare(&runtime, runtime.session_epoch, 9, 3, 0, 2, &view);
    try runtime.mark_writer_entered(runtime.session_epoch, 9, 3, 0, 2);
    try std.testing.expectError(error.WriterStateMismatch, runtime.mark_writer_entered(runtime.session_epoch, 9, 3, 0, 2));
    try std.testing.expectError(error.WriterStateMismatch, runtime.cancel_unentered(runtime.session_epoch, 9, 3, 0, 2));
    try runtime.cancel_entered(runtime.session_epoch, 9, 3, 0, 2);
    try call_prepare(&runtime, runtime.session_epoch, 9, 3, 0, 3, &view);
    try runtime.mark_writer_entered(runtime.session_epoch, 9, 3, 0, 3);
    try runtime.commit_publish(runtime.session_epoch, 9, 3, 0, 3);
    const pinned_heap = runtime.allocator;
    try std.testing.expectError(error.WrongRelease, call_release(&runtime, 78, 9, 3, 0, 3));
    try std.testing.expectError(error.WrongRelease, call_release(&runtime, 77, 9, 3, 0, 2));
    try std.testing.expectEqualDeep(pinned_heap, runtime.allocator);
    try call_release(&runtime, 77, 9, 3, 0, 3);
    try std.testing.expectError(error.WrongRelease, call_release(&runtime, 77, 9, 3, 0, 3));
}

test "growth waits on every pin and switches only free descriptors" {
    var runtime = try slot_runtime_t.init(101);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 12, 1, 100, 100, &views);
    const old_runs = [_]heap.heap_run_t{ views[0].run, views[1].run, views[2].run };
    var first = empty_view();
    for (0..SlotsPerWindow) |slot| {
        try call_prepare(&runtime, runtime.session_epoch, 12, 1, @intCast(slot), @intCast(slot + 1), &first);
        try runtime.mark_writer_entered(runtime.session_epoch, 12, 1, @intCast(slot), @intCast(slot + 1));
        try runtime.commit_publish(runtime.session_epoch, 12, 1, @intCast(slot), @intCast(slot + 1));
    }
    try call_request_geometry(&runtime, 12, 1, 2000, 1200);
    try std.testing.expectError(error.Busy, call_request_geometry(&runtime, 12, 1, 3000, 2000));
    try std.testing.expectError(error.SlotPinned, call_prepare(&runtime, runtime.session_epoch, 12, 1, 0, 4, &first));
    try call_release(&runtime, 101, 12, 1, 1, 2);
    try std.testing.expect(runtime.windows[0].slots[1].retired != null);
    try std.testing.expect(runtime.windows[0].slots[0].retired == null);
    try std.testing.expect(runtime.allocator.page_bitmap[old_runs[0].start_page]);
    try call_prepare(&runtime, runtime.session_epoch, 12, 1, 1, 4, &first);
    try runtime.mark_writer_entered(runtime.session_epoch, 12, 1, 1, 4);
    try runtime.commit_publish(runtime.session_epoch, 12, 1, 1, 4);
    try call_release(&runtime, 101, 12, 1, 2, 3);
    try call_release(&runtime, 101, 12, 1, 0, 1);
    for (old_runs) |run| {
        for (run.start_page..run.start_page + run.page_count) |page|
            try std.testing.expect(!runtime.allocator.page_bitmap[page]);
    }
    try call_release(&runtime, 101, 12, 1, 1, 4);
    try call_prepare(&runtime, runtime.session_epoch, 12, 1, 0, 5, &first);
    try std.testing.expectEqual(runtime.windows[0].desired_layout.width, first.frame_layout.width);
    try runtime.cancel_unentered(runtime.session_epoch, 12, 1, 0, 5);
}

test "retirement quarantines claims and releases on exact final pin" {
    var runtime = try slot_runtime_t.init(103);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 22, 5, 64, 64, &views);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 22, 5, 0, 1, &view);
    try runtime.mark_writer_entered(runtime.session_epoch, 22, 5, 0, 1);
    try runtime.commit_publish(runtime.session_epoch, 22, 5, 0, 1);
    try runtime.retire_window(22, 5);
    try std.testing.expectError(error.InvalidLifecycle, call_prepare(&runtime, runtime.session_epoch, 22, 5, 1, 2, &view));
    try std.testing.expectError(error.InvalidLifecycle, call_request_geometry(&runtime, 22, 5, 100, 100));
    try call_release(&runtime, 103, 22, 5, 0, 1);
    try std.testing.expect(!runtime.windows[0].active);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
}

test "drain requires producer and host proofs and releases all pinned runs" {
    var runtime = try slot_runtime_t.init(107);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 31, 7, 64, 64, &views);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 31, 7, 0, 1, &view);
    try runtime.mark_writer_entered(runtime.session_epoch, 31, 7, 0, 1);
    try runtime.commit_publish(runtime.session_epoch, 31, 7, 0, 1);
    try std.testing.expectError(error.MissingProof, runtime.begin_drain(false));
    try runtime.begin_drain(true);
    try std.testing.expectError(error.InvalidLifecycle, call_prepare(&runtime, runtime.session_epoch, 31, 7, 1, 2, &view));
    try std.testing.expectError(error.MissingProof, runtime.confirm_host_drained(false));
    try runtime.confirm_host_drained(true);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
    try std.testing.expectError(error.InvalidLifecycle, runtime.begin_drain(true));
}

test "all descriptor indices are assigned and generation never reuses" {
    var runtime = try slot_runtime_t.init(109);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    for (0..MaxWindows) |index| {
        try call_admit(&runtime, @intCast(index + 1), @intCast(index + 1), 1, 1, &views);
        try std.testing.expectEqual(@as(u32, @intCast(index * SlotsPerWindow)), views[0].descriptor_index);
        try std.testing.expectEqual(@as(u32, @intCast(index * SlotsPerWindow + 2)), views[2].descriptor_index);
    }
    try std.testing.expectError(error.NoWindowCapacity, call_admit(&runtime, 99, 65, 1, 1, &views));
    try runtime.retire_window(1, 1);
    try std.testing.expectError(error.StaleGeneration, call_admit(&runtime, 99, 64, 1, 1, &views));
}

test "transactional allocation errors preserve output and allocator" {
    var runtime = try slot_runtime_t.init(113);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    const output_before = views;
    runtime.allocator.tickets_exhausted = true;
    const allocator_before = runtime.allocator;
    try std.testing.expectError(error.TicketExhausted, call_admit(&runtime, 44, 1, 128, 128, &views));
    try std.testing.expectEqualDeep(allocator_before, runtime.allocator);
    try std.testing.expectEqualDeep(output_before, views);
    runtime.allocator.tickets_exhausted = false;
    try call_admit(&runtime, 44, 1, 128, 128, &views);

    const record_before = runtime.windows[0];
    const allocator_after_admit = runtime.allocator;
    runtime.allocator.tickets_exhausted = true;
    const exhausted_allocator = runtime.allocator;
    try std.testing.expectError(error.TicketExhausted, call_request_geometry(&runtime, 44, 1, 2000, 1200));
    try std.testing.expectEqualDeep(record_before, runtime.windows[0]);
    try std.testing.expectEqualDeep(exhausted_allocator, runtime.allocator);
    runtime.allocator = allocator_after_admit;
}

test "shrink updates future geometry without replacing pinned runs" {
    var runtime = try slot_runtime_t.init(127);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 51, 8, 1000, 600, &views);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 51, 8, 0, 1, &view);
    try runtime.mark_writer_entered(runtime.session_epoch, 51, 8, 0, 1);
    try runtime.commit_publish(runtime.session_epoch, 51, 8, 0, 1);
    const pinned_run = runtime.windows[0].slots[0].active.?;
    try call_request_geometry(&runtime, 51, 8, 100, 100);
    try std.testing.expectEqualDeep(pinned_run, runtime.windows[0].slots[0].active.?);
    try std.testing.expectEqual(@as(u32, 100), runtime.windows[0].desired_layout.width);
    try call_release(&runtime, 127, 51, 8, 0, 1);
    try call_prepare(&runtime, runtime.session_epoch, 51, 8, 0, 2, &view);
    try std.testing.expectEqual(@as(u32, 100), view.frame_layout.width);
    try runtime.cancel_unentered(runtime.session_epoch, 51, 8, 0, 2);
}

test "invalid writer evidence fails closed and explicit proof still drains" {
    var runtime = try slot_runtime_t.init(131);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 61, 9, 64, 64, &views);
    var output = empty_view();
    const before = output;
    try std.testing.expectError(error.InvalidSlot, call_prepare(&runtime, runtime.session_epoch, 61, 9, 3, 1, &output));
    try std.testing.expectEqualDeep(before, output);
    try std.testing.expectError(error.InvalidSequence, call_prepare(&runtime, runtime.session_epoch, 61, 9, 0, 0, &output));
    try std.testing.expectError(error.NoWriter, runtime.mark_writer_entered(runtime.session_epoch, 61, 9, 0, 1));
    const pages_before_failure = count_pages(&runtime.allocator);
    try std.testing.expectError(error.FailedClosed, runtime.commit_publish(runtime.session_epoch, 61, 9, 0, 1));
    try std.testing.expect(runtime.failed_closed);
    try std.testing.expectEqual(pages_before_failure, count_pages(&runtime.allocator));
    try std.testing.expectError(error.FailedClosed, call_prepare(&runtime, runtime.session_epoch, 61, 9, 0, 1, &output));
    try runtime.begin_drain(true);
    try std.testing.expectError(error.MissingProof, runtime.confirm_host_drained(false));
    try runtime.confirm_host_drained(true);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
}

test "sequence is strictly increasing and exhausted sequence cannot wrap" {
    var runtime = try slot_runtime_t.init(137);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 71, 10, 64, 64, &views);
    var output = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 71, 10, 0, 4, &output);
    try runtime.cancel_unentered(runtime.session_epoch, 71, 10, 0, 4);
    try std.testing.expectError(error.InvalidSequence, call_prepare(&runtime, runtime.session_epoch, 71, 10, 1, 4, &output));
    runtime.windows[0].last_sequence = std.math.maxInt(u64);
    try std.testing.expectError(error.SequenceExhausted, call_prepare(&runtime, runtime.session_epoch, 71, 10, 1, std.math.maxInt(u64), &output));
}

test "long-running slot cycle preserves exact ownership across reuse" {
    var runtime = try slot_runtime_t.init(149);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 81, 11, 300, 200, &views);
    var view = empty_view();
    for (1..513) |sequence| {
        const slot: u8 = @intCast((sequence - 1) % SlotsPerWindow);
        try call_prepare(&runtime, runtime.session_epoch, 81, 11, slot, sequence, &view);
        try std.testing.expectEqual(@as(u64, 81), view.window);
        try std.testing.expectEqual(@as(u32, slot), view.descriptor_index % SlotsPerWindow);
        try runtime.mark_writer_entered(runtime.session_epoch, 81, 11, slot, sequence);
        try runtime.commit_publish(runtime.session_epoch, 81, 11, slot, sequence);
        try call_release(&runtime, 149, 81, 11, slot, sequence);
    }
    try std.testing.expectEqual(@as(u64, 512), runtime.windows[0].last_sequence);
    try runtime.retire_window(81, 11);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
}

test "capture dimensions use frozen independent clamps" {
    var runtime = try slot_runtime_t.init(151);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 82, 12, layout.MaxCaptureWidth + 500, layout.MaxCaptureHeight + 500, &views);
    try std.testing.expectEqual(layout.MaxCaptureWidth, views[0].frame_layout.width);
    try std.testing.expectEqual(layout.MaxCaptureHeight, views[0].frame_layout.height);
    try std.testing.expectEqual(@as(u64, 64 * 2 * 1024 * 1024), views[0].run.capacity_bytes);
    try runtime.retire_window(82, 12);
}

test "retirement cancels a provisional claim before releasing descriptors" {
    var runtime = try slot_runtime_t.init(157);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 91, 13, 64, 64, &views);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 91, 13, 2, 1, &view);
    try runtime.retire_window(91, 13);
    try std.testing.expect(runtime.windows[0].active);
    try std.testing.expectError(error.InvalidLifecycle, call_prepare(&runtime, runtime.session_epoch, 91, 13, 1, 2, &view));
    try runtime.cancel_unentered(runtime.session_epoch, 91, 13, 2, 1);
    try std.testing.expect(!runtime.windows[0].active);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
}

test "drain releases mixed active replacement and retired runs" {
    var runtime = try slot_runtime_t.init(163);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 92, 14, 1000, 700, &views);
    try call_request_geometry(&runtime, 92, 14, 3000, 2000);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 92, 14, 0, 1, &view);
    try runtime.cancel_unentered(runtime.session_epoch, 92, 14, 0, 1);
    try std.testing.expect(runtime.windows[0].slots[0].retired != null);
    try std.testing.expect(runtime.windows[0].slots[1].replacement != null);
    try runtime.begin_drain(true);
    try runtime.confirm_host_drained(true);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
}

test "slot capacity corruption is rejected without claiming a writer" {
    var runtime = try slot_runtime_t.init(181);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 96, 18, 64, 64, &views);
    var output = empty_view();
    const saved_run = runtime.windows[0].slots[2].active.?;
    runtime.windows[0].slots[2].active = null;
    try std.testing.expectError(error.InvalidLifecycle, call_request_geometry(&runtime, 96, 18, 500, 500));
    runtime.windows[0].slots[2].active = saved_run;

    const saved_layout = runtime.windows[0].desired_layout;
    runtime.windows[0].desired_layout.data_bytes = @intCast(saved_run.capacity_bytes + layout.PageBytes);
    try std.testing.expectError(error.SlotTooSmall, call_prepare(&runtime, runtime.session_epoch, 96, 18, 2, 1, &output));
    try std.testing.expectError(error.InvalidSlot, call_prepare(&runtime, runtime.session_epoch, 96, 18, 3, 1, &output));
    try std.testing.expectEqual(@as(u64, 0), runtime.windows[0].last_sequence);
    runtime.windows[0].desired_layout = saved_layout;
    try runtime.retire_window(96, 18);
}

test "growth release corruption fails closed and retains reservations" {
    var runtime = try slot_runtime_t.init(191);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 97, 19, 64, 64, &views);
    var output = empty_view();
    for (0..SlotsPerWindow) |slot| {
        const sequence: u64 = @intCast(slot + 1);
        try call_prepare(&runtime, runtime.session_epoch, 97, 19, @intCast(slot), sequence, &output);
        try runtime.mark_writer_entered(runtime.session_epoch, 97, 19, @intCast(slot), sequence);
        try runtime.commit_publish(runtime.session_epoch, 97, 19, @intCast(slot), sequence);
    }
    try call_request_geometry(&runtime, 97, 19, 2000, 1400);
    try call_release(&runtime, 191, 97, 19, 0, 1);
    try call_release(&runtime, 191, 97, 19, 1, 2);
    const old_run = runtime.windows[0].slots[0].retired.?;
    const valid_heap = runtime.allocator;
    try runtime.allocator.release(old_run);
    const corrupted_heap = runtime.allocator;
    try std.testing.expectError(error.HeapStateCorrupt, call_release(&runtime, 191, 97, 19, 2, 3));
    try std.testing.expect(runtime.failed_closed);
    try std.testing.expectEqualDeep(corrupted_heap, runtime.allocator);
    runtime.allocator = valid_heap;
    try runtime.begin_drain(true);
    try runtime.confirm_host_drained(true);
}

test "retirement release corruption remains recoverable only with drain proof" {
    var runtime = try slot_runtime_t.init(193);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 98, 20, 64, 64, &views);
    var output = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 98, 20, 0, 1, &output);
    try runtime.mark_writer_entered(runtime.session_epoch, 98, 20, 0, 1);
    try runtime.commit_publish(runtime.session_epoch, 98, 20, 0, 1);
    try runtime.retire_window(98, 20);
    const valid_heap = runtime.allocator;
    try runtime.allocator.release(views[0].run);
    const corrupted_heap = runtime.allocator;
    try std.testing.expectError(error.HeapStateCorrupt, call_release(&runtime, 193, 98, 20, 0, 1));
    try std.testing.expectEqualDeep(corrupted_heap, runtime.allocator);
    runtime.allocator = valid_heap;
    try runtime.begin_drain(true);
    try runtime.confirm_host_drained(true);
}

test "host drain allocation failure does not partially release runtime runs" {
    var runtime = try slot_runtime_t.init(197);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 99, 21, 64, 64, &views);
    const valid_heap = runtime.allocator;
    try runtime.allocator.release(views[1].run);
    const corrupted_heap = runtime.allocator;
    try runtime.begin_drain(true);
    try std.testing.expectError(error.HeapStateCorrupt, runtime.confirm_host_drained(true));
    try std.testing.expect(runtime.failed_closed);
    try std.testing.expectEqualDeep(corrupted_heap, runtime.allocator);
    runtime.allocator = valid_heap;
    try runtime.confirm_host_drained(true);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
}

test "caller-owned runtime allocation has a complete leak-free lifecycle" {
    const runtime = try std.testing.allocator.create(slot_runtime_t);
    defer std.testing.allocator.destroy(runtime);
    runtime.* = try slot_runtime_t.init(199);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(runtime, 100, 22, 64, 64, &views);
    try runtime.begin_drain(true);
    try runtime.confirm_host_drained(true);
}

test "writer identity guards and active-writer drain guard" {
    var runtime = try slot_runtime_t.init(167);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 93, 15, 64, 64, &views);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 93, 15, 1, 1, &view);
    try std.testing.expectError(error.NoWriter, runtime.mark_writer_entered(runtime.session_epoch, 93, 15, 0, 1));
    try std.testing.expectError(error.NoWriter, runtime.mark_writer_entered(runtime.session_epoch, 93, 15, 1, 2));
    try std.testing.expectError(error.SlotPinned, runtime.begin_drain(true));
    try runtime.cancel_unentered(runtime.session_epoch, 93, 15, 1, 1);
    try runtime.begin_drain(true);
    try runtime.confirm_host_drained(true);
}

test "pending growth accepts a smaller desired geometry without dropping replacement" {
    var runtime = try slot_runtime_t.init(173);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 94, 16, 100, 100, &views);
    try call_request_geometry(&runtime, 94, 16, 2000, 1200);
    const replacement = runtime.windows[0].slots[0].replacement.?;
    try call_request_geometry(&runtime, 94, 16, 50, 50);
    try std.testing.expectEqual(@as(u32, 50), runtime.windows[0].desired_layout.width);
    try std.testing.expectEqualDeep(replacement, runtime.windows[0].slots[0].replacement.?);
    try runtime.begin_drain(true);
    try runtime.confirm_host_drained(true);
}

test "corrupt post-publication bookkeeping fails closed and preserves all runs" {
    var runtime = try slot_runtime_t.init(179);
    var views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&runtime, 95, 17, 64, 64, &views);
    var view = empty_view();
    try call_prepare(&runtime, runtime.session_epoch, 95, 17, 0, 1, &view);
    try runtime.mark_writer_entered(runtime.session_epoch, 95, 17, 0, 1);
    try runtime.commit_publish(runtime.session_epoch, 95, 17, 0, 1);
    try call_prepare(&runtime, runtime.session_epoch, 95, 17, 1, 2, &view);
    try runtime.mark_writer_entered(runtime.session_epoch, 95, 17, 1, 2);
    runtime.windows[0].slots[1].pinned_sequence = 99;
    const pages_before_failure = count_pages(&runtime.allocator);
    try std.testing.expectError(error.FailedClosed, runtime.commit_publish(runtime.session_epoch, 95, 17, 1, 2));
    try std.testing.expectEqual(pages_before_failure, count_pages(&runtime.allocator));
    try std.testing.expectError(error.FailedClosed, call_release(&runtime, 179, 95, 17, 0, 1));
    try runtime.begin_drain(true);
    try runtime.confirm_host_drained(true);
    try std.testing.expectEqual(@as(usize, 0), count_pages(&runtime.allocator));
}

test "stale asynchronous writer work cannot mutate a replacement session" {
    var old_runtime = try slot_runtime_t.init(211);
    var current_runtime = try slot_runtime_t.init(223);
    var old_views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    var current_views = [_]slot_view_t{empty_view()} ** SlotsPerWindow;
    try call_admit(&old_runtime, 501, 1, 64, 64, &old_views);
    try call_admit(&current_runtime, 501, 1, 64, 64, &current_views);

    var old_view = empty_view();
    var current_view = empty_view();
    try call_prepare(&old_runtime, old_runtime.session_epoch, 501, 1, 0, 1, &old_view);
    try std.testing.expectError(
        error.WrongSessionEpoch,
        call_prepare(&current_runtime, old_runtime.session_epoch, 501, 1, 0, 1, &current_view),
    );
    try std.testing.expectEqualDeep(empty_view(), current_view);
    try call_prepare(&current_runtime, current_runtime.session_epoch, 501, 1, 0, 1, &current_view);
    try current_runtime.mark_writer_entered(current_runtime.session_epoch, 501, 1, 0, 1);

    const current_record_before = current_runtime.windows[0];
    const current_allocator_before = current_runtime.allocator;
    const current_output_before = current_view;
    try std.testing.expectError(
        error.WrongSessionEpoch,
        current_runtime.mark_writer_entered(old_runtime.session_epoch, 501, 1, 0, 1),
    );
    try std.testing.expectError(
        error.WrongSessionEpoch,
        current_runtime.commit_publish(old_runtime.session_epoch, 501, 1, 0, 1),
    );
    try std.testing.expectError(
        error.WrongSessionEpoch,
        current_runtime.cancel_unentered(old_runtime.session_epoch, 501, 1, 0, 1),
    );
    try std.testing.expectError(
        error.WrongSessionEpoch,
        current_runtime.cancel_entered(old_runtime.session_epoch, 501, 1, 0, 1),
    );
    try std.testing.expectEqualDeep(current_record_before, current_runtime.windows[0]);
    try std.testing.expectEqualDeep(current_allocator_before, current_runtime.allocator);
    try std.testing.expectEqualDeep(current_output_before, current_view);
    try std.testing.expect(!current_runtime.failed_closed);

    try current_runtime.commit_publish(current_runtime.session_epoch, 501, 1, 0, 1);
    try call_release(&current_runtime, current_runtime.session_epoch, 501, 1, 0, 1);
    try current_runtime.retire_window(501, 1);
    try old_runtime.cancel_unentered(old_runtime.session_epoch, 501, 1, 0, 1);
    try old_runtime.retire_window(501, 1);
}

fn count_pages(allocator: *const heap.heap_allocator_t) usize {
    var count: usize = 0;
    for (allocator.page_bitmap) |used| if (used) {
        count += 1;
    };
    return count;
}
