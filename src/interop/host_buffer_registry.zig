const std_t = @import("std");
const layout_t = @import("interop_layout.zig");

/// Frozen layout-v3 descriptor capacity used to bound outstanding host frames.
pub const MaxRecords: usize = layout_t.SlotCount;
/// Frozen number of slots owned by one window incarnation.
pub const SlotsPerWindow: u32 = 3;

/// Exact copied identity of one published frame and its eventual release.
/// @ownership Value-only; the registry copies it and retains no mapping pointer or frame bytes.
/// @thread_safety Read/write only through the serialized owner of registry_t.
pub const frame_key_t = struct {
    /// Nonzero immutable session epoch captured by both endpoints.
    session_epoch: u64,
    /// Nonzero exported window identity.
    window: u64,
    /// Nonzero incarnation generation for the window identity.
    generation: u64,
    /// Frozen per-window slot index, in the range zero through two.
    slot: u32,
    /// Nonzero frame sequence unique within this window incarnation and slot history.
    frame_sequence: u64,
};

/// Monotonic opaque registry-local ticket assigned only to a successful import admission.
/// @ownership Copyable scalar; callers must return it only with the original registry epoch.
/// @thread_safety Methods using tickets require serialized event-loop access.
pub const ticket_t = u64;

/// Admission and completion phase for one registry instance.
/// @ownership Value-only state snapshot; it owns no external resource.
/// @thread_safety Read through the serialized registry owner.
pub const state_t = enum {
    /// New frame imports may be admitted.
    Open,
    /// New frame imports are stopped; admitted records may still complete.
    Draining,
    /// New imports are permanently disabled while retained records may complete.
    Quarantined,
};

/// Local reason a release completion became eligible; not a wire field.
/// @ownership Value-only metadata; neither case is a session-drain proof.
/// @thread_safety Copied with completion_t.
pub const completion_kind_t = enum {
    /// Import was conclusively quiesced before any surface attach was issued.
    NeverAttached,
    /// The exact attached wl_buffer produced its actual release callback.
    CompositorRelease,
};

/// Copied exact frame completion returned to the host adapter.
/// @ownership Returned by value; does not borrow registry storage or prove native teardown.
/// @thread_safety Caller obtains it through the serialized registry owner.
pub const completion_t = struct {
    /// Exact identity to use for host slot transition and FrameRelease encoding.
    key: frame_key_t,
    /// Local provenance of the completion; not serialized to the guest.
    kind: completion_kind_t,
    /// True only after the caller confirmed the exact Consuming-to-Free transition.
    slot_free: bool,
};

/// Bounded observational counts for one registry instance.
/// @ownership Returned by value; no pointers or proof are exposed.
/// @thread_safety Snapshot through the serialized registry owner.
pub const counts_t = struct {
    /// Number of records awaiting async import resolution.
    import_pending: usize,
    /// Number of records with an attached compositor buffer awaiting release.
    attached: usize,
    /// Number of completed records awaiting slot-free and notification commit.
    released: usize,
    /// Total occupied record count; always at most MaxRecords.
    total: usize,
    /// Current admission state.
    state: state_t,
};

/// Errors returned by allocation-free host frame ownership bookkeeping.
/// @ownership Compile-time error set; it retains no runtime state.
pub const registry_error_t = error{
    /// Initialization or frame identity used the zero session epoch.
    InvalidSessionEpoch,
    /// Caller epoch does not match the immutable epoch of this registry.
    WrongSessionEpoch,
    /// Frame identity contains a zero window identifier.
    InvalidWindow,
    /// Frame identity contains a zero window generation.
    InvalidGeneration,
    /// Frame slot is outside the frozen three-slot range.
    InvalidSlot,
    /// Frame sequence is zero.
    InvalidFrameSequence,
    /// Ticket zero is the invalid sentinel and is never issued.
    InvalidTicket,
    /// Nonzero ticket is not occupied by this registry.
    UnknownTicket,
    /// Another retained record owns this window and slot, regardless of generation.
    DuplicateSlot,
    /// All fixed descriptor records are retained.
    CapacityFull,
    /// Monotonic ticket space is exhausted and must never wrap or reuse.
    TicketExhausted,
    /// Admission is closed in Draining or Quarantined state.
    AdmissionClosed,
    /// Operation is invalid for the ticket's current phase.
    WrongState,
    /// Never-attached completion lacks caller confirmation that callbacks are quiesced.
    CallbacksNotQuiesced,
    /// Exact slot-free transition was already committed for this record.
    SlotAlreadyFree,
};

const record_phase_t = enum { ImportPending, Attached, Released };

const record_t = struct {
    occupied: bool = false,
    ticket: ticket_t = 0,
    key: frame_key_t = .{
        .session_epoch = 0,
        .window = 0,
        .generation = 0,
        .slot = 0,
        .frame_sequence = 0,
    },
    phase: record_phase_t = .ImportPending,
    completion_kind: completion_kind_t = .NeverAttached,
    slot_free: bool = false,
};

/// Allocation-free ledger for host ownership of frames awaiting Wayland use/release.
/// @ownership Caller owns this fixed value. It retains scalar identities only: no Wayland
///          proxy, callback, mapping, native handle, wire frame, or release proof.
/// @thread_safety All mutations and snapshots require one externally serialized event loop.
pub const registry_t = struct {
    /// Immutable nonzero epoch; only initialization assigns it.
    session_epoch: u64,
    /// Admission gate changed only by begin_drain or quarantine.
    admission_state: state_t = .Open,
    /// Fixed scalar ledger; callers must use methods rather than mutate records.
    records: [MaxRecords]record_t = [_]record_t{.{}} ** MaxRecords,
    /// Next never-issued nonzero ticket; modified only on successful admission.
    next_ticket: ticket_t = 1,
    /// Permanent admission exhaustion latch after issuing the maximum ticket.
    ticket_exhausted: bool = false,

    /// Initialize an empty registry for one caller-validated nonzero session epoch.
    /// @param[in] session_epoch Immutable epoch for every future operation.
    /// @return Empty Open registry, or InvalidSessionEpoch when epoch is zero.
    /// @ownership No allocation occurs; returned value is owned by the caller.
    /// @thread_safety Subsequent methods are serialized by the caller.
    pub fn init(session_epoch: u64) registry_error_t!registry_t {
        if (session_epoch == 0) return error.InvalidSessionEpoch;
        return .{ .session_epoch = session_epoch };
    }

    /// Admit one exact published frame before starting any asynchronous import.
    /// @param[in,out] self Open registry owned by the serialized event loop.
    /// @param[in] session_epoch Captured epoch for this work item.
    /// @param[in] key Exact nonzero session/window/generation/sequence and slot 0..2 identity.
    /// @return A never-reused registry-local ticket assigned only on success.
    /// @errors WrongSessionEpoch, invalid key fields, DuplicateSlot, CapacityFull,
    /// TicketExhausted, or AdmissionClosed. Every error preserves all registry bytes.
    /// @ownership Registry copies key and retains no external resource. Caller must have
    /// acquired the validated Ready-to-Consuming slot before calling and remains responsible
    /// for its failure path if admission fails.
    /// @thread_safety Caller serializes admission with callbacks and drain.
    pub fn begin_import(self: *registry_t, session_epoch: u64, key: frame_key_t) registry_error_t!ticket_t {
        try self.check_epoch(session_epoch);
        if (key.session_epoch == 0) return error.InvalidSessionEpoch;
        if (key.session_epoch != self.session_epoch) return error.WrongSessionEpoch;
        if (key.window == 0) return error.InvalidWindow;
        if (key.generation == 0) return error.InvalidGeneration;
        if (key.slot >= SlotsPerWindow) return error.InvalidSlot;
        if (key.frame_sequence == 0) return error.InvalidFrameSequence;
        if (self.admission_state != .Open) return error.AdmissionClosed;
        if (self.find_slot(key.window, key.slot) != null) return error.DuplicateSlot;
        const free_index = self.find_free_record() orelse return error.CapacityFull;
        if (self.ticket_exhausted) return error.TicketExhausted;

        const ticket = self.next_ticket;
        self.records[free_index] = .{
            .occupied = true,
            .ticket = ticket,
            .key = key,
            .phase = .ImportPending,
            .completion_kind = .NeverAttached,
            .slot_free = false,
        };
        if (ticket == std_t.math.maxInt(ticket_t)) {
            self.ticket_exhausted = true;
        } else {
            self.next_ticket = ticket + 1;
        }
        return ticket;
    }

    /// Confirm async import resolution and the serialized adapter's attach/commit operation.
    /// @param[in,out] self Registry containing ticket in ImportPending.
    /// @param[in] session_epoch Captured callback epoch.
    /// @param[in] ticket Exact ticket captured by the immutable callback context.
    /// @return No value after ImportPending transitions to Attached.
    /// @errors WrongSessionEpoch, InvalidTicket, UnknownTicket, or WrongState unchanged.
    /// @ownership No proxy or callback pointer is retained. Caller guarantees the import callback
    /// is quiesced, listener identity is stable, and attach/commit is performed without reentrant
    /// dispatch between this transition and the native attach.
    /// @thread_safety Caller serializes callback and registry transitions.
    pub fn mark_attached(self: *registry_t, session_epoch: u64, ticket: ticket_t) registry_error_t!void {
        const index = try self.require_ticket(session_epoch, ticket);
        if (self.records[index].phase != .ImportPending) return error.WrongState;
        self.records[index].phase = .Attached;
        self.records[index].completion_kind = .CompositorRelease;
    }

    /// Complete an import that never attached after async callbacks are impossible.
    /// @param[in,out] self Registry containing ticket in ImportPending.
    /// @param[in] session_epoch Captured import-callback epoch.
    /// @param[in] ticket Exact frame ticket.
    /// @param[in] callbacks_quiesced Caller assertion that no callback can arrive and no attach was issued.
    /// @return No value after the record becomes Released/NeverAttached.
    /// @errors WrongSessionEpoch, invalid/unknown ticket, CallbacksNotQuiesced, or WrongState;
    /// every error preserves the complete registry state.
    /// @ownership Does not destroy proxies or prove external teardown; caller owns native cleanup.
    /// @thread_safety Caller serializes callback cancellation and this transition.
    pub fn complete_never_attached(
        self: *registry_t,
        session_epoch: u64,
        ticket: ticket_t,
        callbacks_quiesced: bool,
    ) registry_error_t!void {
        const index = try self.require_ticket(session_epoch, ticket);
        if (self.records[index].phase != .ImportPending) return error.WrongState;
        if (!callbacks_quiesced) return error.CallbacksNotQuiesced;
        self.records[index].phase = .Released;
        self.records[index].completion_kind = .NeverAttached;
        self.records[index].slot_free = false;
    }

    /// Record the actual wl_buffer.release callback for one attached frame.
    /// @param[in,out] self Registry containing the exact ticket in Attached.
    /// @param[in] session_epoch Captured Wayland callback epoch.
    /// @param[in] ticket Stable callback ticket for the exact immutable frame identity.
    /// @return No value after Attached transitions to Released/CompositorRelease.
    /// @errors WrongSessionEpoch, invalid/unknown ticket, or WrongState unchanged.
    /// @ownership Caller must invoke only from the matching real Wayland release callback;
    /// this value-only method cannot authenticate native evidence and retains no proxy.
    /// @thread_safety Callback is serialized with all registry transitions.
    pub fn buffer_released(self: *registry_t, session_epoch: u64, ticket: ticket_t) registry_error_t!void {
        const index = try self.require_ticket(session_epoch, ticket);
        if (self.records[index].phase != .Attached) return error.WrongState;
        self.records[index].phase = .Released;
        self.records[index].completion_kind = .CompositorRelease;
        self.records[index].slot_free = false;
    }

    /// Copy completion identity and provenance after either valid completion path.
    /// @param[in] self Registry containing a Released ticket.
    /// @param[in] session_epoch Captured session epoch.
    /// @param[in] ticket Exact release ticket.
    /// @return Copied completion; caller owns the returned value.
    /// @errors WrongSessionEpoch, InvalidTicket, UnknownTicket, or WrongState without mutation.
    /// @ownership Returned key is copied and never aliases registry storage.
    /// @thread_safety Caller serializes this read with registry mutations.
    pub fn get_completion(self: *const registry_t, session_epoch: u64, ticket: ticket_t) registry_error_t!completion_t {
        const index = try self.require_ticket(session_epoch, ticket);
        const record = self.records[index];
        if (record.phase != .Released) return error.WrongState;
        return .{
            .key = record.key,
            .kind = record.completion_kind,
            .slot_free = record.slot_free,
        };
    }

    /// Record caller-confirmed successful Consuming-to-Free for the exact released frame.
    /// @param[in,out] self Registry containing Released and not-yet-free ticket.
    /// @param[in] session_epoch Captured session epoch.
    /// @param[in] ticket Exact released frame ticket.
    /// @return No value after the caller-confirmed slot transition is recorded.
    /// @errors WrongSessionEpoch, invalid/unknown ticket, WrongState, or SlotAlreadyFree unchanged.
    /// @ownership Caller must perform and validate the exact descriptor CAS before calling.
    /// @thread_safety Caller serializes the shared-state transition and registry update.
    pub fn mark_slot_free(self: *registry_t, session_epoch: u64, ticket: ticket_t) registry_error_t!void {
        const index = try self.require_ticket(session_epoch, ticket);
        const record = &self.records[index];
        if (record.phase != .Released) return error.WrongState;
        if (record.slot_free) return error.SlotAlreadyFree;
        record.slot_free = true;
    }

    /// Retire a released record after the bounded send queue owns its exact FrameRelease bytes.
    /// @param[in,out] self Registry containing a Released, slot-free ticket.
    /// @param[in] session_epoch Captured session epoch.
    /// @param[in] ticket Exact frame ticket whose notification was committed.
    /// @return No value after the record is cleared; ticket cursor is never reset.
    /// @errors WrongSessionEpoch, invalid/unknown ticket, or WrongState unchanged.
    /// @ownership Caller asserts the queue owns the exact encoded identity and no native callback
    /// can reference this ticket. Queue/socket ACK is not compositor lifetime proof.
    /// @thread_safety Caller serializes enqueue commit and retirement.
    pub fn notify_committed(self: *registry_t, session_epoch: u64, ticket: ticket_t) registry_error_t!void {
        const index = try self.require_ticket(session_epoch, ticket);
        const record = self.records[index];
        if (record.phase != .Released or !record.slot_free) return error.WrongState;
        self.records[index] = .{};
    }

    /// Stop new admission while preserving every import and release record.
    /// @param[in,out] self Open or Draining registry.
    /// @param[in] session_epoch Captured session epoch.
    /// @return No value; Open becomes Draining and repeated Draining is idempotent.
    /// @errors WrongSessionEpoch or WrongState (including Quarantined), unchanged on error.
    /// @ownership Does not complete or clear any frame and proves no drain condition.
    /// @thread_safety Caller serializes drain with admissions and callbacks.
    pub fn begin_drain(self: *registry_t, session_epoch: u64) registry_error_t!void {
        try self.check_epoch(session_epoch);
        switch (self.admission_state) {
            .Open => self.admission_state = .Draining,
            .Draining => {},
            .Quarantined => return error.WrongState,
        }
    }

    /// Permanently close admission while retaining records for exact callbacks and completion.
    /// @param[in,out] self Registry in any admission state.
    /// @param[in] session_epoch Captured session epoch.
    /// @return No value; state becomes Quarantined and repeated calls are idempotent.
    /// @errors WrongSessionEpoch without mutation.
    /// @ownership Never frees records, mappings, proxies, or proves safe unmap.
    /// @thread_safety Caller serializes quarantine with every other operation.
    pub fn quarantine(self: *registry_t, session_epoch: u64) registry_error_t!void {
        try self.check_epoch(session_epoch);
        self.admission_state = .Quarantined;
    }

    /// Return bounded phase counts and admission state without asserting safe teardown.
    /// @param[in] self Registry whose scalar records are observed.
    /// @return Copied counts; zero records are necessary but not sufficient for unmapping.
    /// @ownership Returned value owns no pointers/resources and is not a drain proof.
    /// @thread_safety Caller serializes this snapshot with mutation.
    pub fn counts(self: *const registry_t) counts_t {
        var result = counts_t{
            .import_pending = 0,
            .attached = 0,
            .released = 0,
            .total = 0,
            .state = self.admission_state,
        };
        for (self.records) |record| {
            if (!record.occupied) continue;
            result.total += 1;
            switch (record.phase) {
                .ImportPending => result.import_pending += 1,
                .Attached => result.attached += 1,
                .Released => result.released += 1,
            }
        }
        return result;
    }

    fn check_epoch(self: *const registry_t, session_epoch: u64) registry_error_t!void {
        if (session_epoch == 0) return error.InvalidSessionEpoch;
        if (session_epoch != self.session_epoch) return error.WrongSessionEpoch;
    }

    fn require_ticket(self: *const registry_t, session_epoch: u64, ticket: ticket_t) registry_error_t!usize {
        try self.check_epoch(session_epoch);
        if (ticket == 0) return error.InvalidTicket;
        for (self.records, 0..) |record, index| {
            if (record.occupied and record.ticket == ticket) return index;
        }
        return error.UnknownTicket;
    }

    fn find_slot(self: *const registry_t, window: u64, slot: u32) ?usize {
        for (self.records, 0..) |record, index| {
            if (record.occupied and record.key.window == window and record.key.slot == slot) return index;
        }
        return null;
    }

    fn find_free_record(self: *const registry_t) ?usize {
        for (self.records, 0..) |record, index| {
            if (!record.occupied) return index;
        }
        return null;
    }
};

/// Test-only state seeding, compiled only into Zig test builds.
/// @ownership Mutates only a caller-owned test registry; unavailable in production builds.
/// @thread_safety Tests must serialize calls as for registry_t methods.
pub const test_support_t = if (@import("builtin").is_test) struct {
    /// Seed the next ticket cursor to exercise the terminal non-wrapping ticket boundary.
    /// @param[in,out] registry Private test instance with no ticket reuse assumptions.
    /// @param[in] next_ticket Nonzero ticket to issue on the next successful admission.
    /// @return No value; this fixture is absent from non-test builds.
    /// @ownership Test fixture only; not a production operation.
    /// @thread_safety Test caller serializes the registry.
    pub fn set_next_ticket(registry: *registry_t, next_ticket: ticket_t) void {
        registry.next_ticket = next_ticket;
        registry.ticket_exhausted = false;
    }
} else struct {};

// Test-only fixtures.
test "initialization rejects zero and stores one immutable epoch" {
    try std_t.testing.expectError(error.InvalidSessionEpoch, registry_t.init(0));
    const registry = try registry_t.init(9);
    try std_t.testing.expectEqual(@as(u64, 9), registry.session_epoch);
    try std_t.testing.expectEqual(state_t.Open, registry.counts().state);
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
}

test "admission validates every key component and errors preserve all state" {
    var registry = try registry_t.init(9);
    const base = frame_key_t{ .session_epoch = 9, .window = 1, .generation = 1, .slot = 0, .frame_sequence = 1 };
    const before = registry;
    try std_t.testing.expectError(error.InvalidSessionEpoch, registry.begin_import(0, base));
    try std_t.testing.expectEqualDeep(before, registry);
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.begin_import(8, base));
    try std_t.testing.expectEqualDeep(before, registry);
    var key = base;
    key.session_epoch = 0;
    try std_t.testing.expectError(error.InvalidSessionEpoch, registry.begin_import(9, key));
    key = base;
    key.session_epoch = 8;
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.begin_import(9, key));
    key = base;
    key.window = 0;
    try std_t.testing.expectError(error.InvalidWindow, registry.begin_import(9, key));
    key = base;
    key.generation = 0;
    try std_t.testing.expectError(error.InvalidGeneration, registry.begin_import(9, key));
    key = base;
    key.slot = SlotsPerWindow;
    try std_t.testing.expectError(error.InvalidSlot, registry.begin_import(9, key));
    key = base;
    key.frame_sequence = 0;
    try std_t.testing.expectError(error.InvalidFrameSequence, registry.begin_import(9, key));
    try std_t.testing.expectEqualDeep(before, registry);
}

test "exact frame lifecycle retains identity through notify and rejects stale callbacks" {
    var registry = try registry_t.init(7);
    const key = frame_key_t{ .session_epoch = 7, .window = 22, .generation = 4, .slot = 2, .frame_sequence = 91 };
    const ticket = try registry.begin_import(7, key);
    try std_t.testing.expectEqual(@as(ticket_t, 1), ticket);
    try std_t.testing.expectError(error.DuplicateSlot, registry.begin_import(7, .{ .session_epoch = 7, .window = 22, .generation = 5, .slot = 2, .frame_sequence = 92 }));
    const pending = registry;
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.mark_attached(8, ticket));
    try std_t.testing.expectEqualDeep(pending, registry);
    try std_t.testing.expectError(error.InvalidTicket, registry.mark_attached(7, 0));
    try std_t.testing.expectError(error.UnknownTicket, registry.mark_attached(7, 99));
    try std_t.testing.expectEqualDeep(pending, registry);
    try std_t.testing.expectError(error.WrongState, registry.buffer_released(7, ticket));
    try std_t.testing.expectEqualDeep(pending, registry);
    try registry.mark_attached(7, ticket);
    const attached = registry;
    try std_t.testing.expectError(error.WrongState, registry.mark_attached(7, ticket));
    try std_t.testing.expectEqualDeep(attached, registry);
    try registry.buffer_released(7, ticket);
    try std_t.testing.expectError(error.WrongState, registry.buffer_released(7, ticket));
    const completion = try registry.get_completion(7, ticket);
    try std_t.testing.expectEqualDeep(key, completion.key);
    try std_t.testing.expectEqual(completion_kind_t.CompositorRelease, completion.kind);
    try std_t.testing.expect(!completion.slot_free);
    const released = registry;
    try std_t.testing.expectError(error.WrongState, registry.notify_committed(7, ticket));
    try std_t.testing.expectEqualDeep(released, registry);
    try registry.mark_slot_free(7, ticket);
    try std_t.testing.expectError(error.SlotAlreadyFree, registry.mark_slot_free(7, ticket));
    try std_t.testing.expectEqual(completion_kind_t.CompositorRelease, (try registry.get_completion(7, ticket)).kind);
    try registry.notify_committed(7, ticket);
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
    try std_t.testing.expectError(error.UnknownTicket, registry.buffer_released(7, ticket));
}

test "never attached requires callback quiescence and exact slot-free before notify" {
    var registry = try registry_t.init(12);
    const key = frame_key_t{ .session_epoch = 12, .window = 2, .generation = 8, .slot = 1, .frame_sequence = 33 };
    const ticket = try registry.begin_import(12, key);
    const before = registry;
    try std_t.testing.expectError(error.CallbacksNotQuiesced, registry.complete_never_attached(12, ticket, false));
    try std_t.testing.expectEqualDeep(before, registry);
    try std_t.testing.expectError(error.WrongState, registry.mark_slot_free(12, ticket));
    try std_t.testing.expectEqualDeep(before, registry);
    try registry.complete_never_attached(12, ticket, true);
    const completion = try registry.get_completion(12, ticket);
    try std_t.testing.expectEqual(completion_kind_t.NeverAttached, completion.kind);
    try std_t.testing.expectEqualDeep(key, completion.key);
    try std_t.testing.expectError(error.WrongState, registry.mark_attached(12, ticket));
    try registry.mark_slot_free(12, ticket);
    try registry.notify_committed(12, ticket);
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
}

test "full capacity preserves state and reuses slots only after committed notification" {
    var registry = try registry_t.init(3);
    var tickets: [MaxRecords]ticket_t = undefined;
    for (0..MaxRecords) |index| {
        const key = frame_key_t{ .session_epoch = 3, .window = @as(u64, @intCast(index / 3 + 1)), .generation = 1, .slot = @as(u32, @intCast(index % 3)), .frame_sequence = 1 };
        tickets[index] = try registry.begin_import(3, key);
    }
    try std_t.testing.expectEqual(MaxRecords, registry.counts().total);
    const full = registry;
    try std_t.testing.expectError(error.CapacityFull, registry.begin_import(3, .{ .session_epoch = 3, .window = 100, .generation = 1, .slot = 0, .frame_sequence = 1 }));
    try std_t.testing.expectEqualDeep(full, registry);
    try registry.mark_attached(3, tickets[0]);
    try registry.buffer_released(3, tickets[0]);
    try registry.mark_slot_free(3, tickets[0]);
    try registry.notify_committed(3, tickets[0]);
    const replacement = try registry.begin_import(3, .{ .session_epoch = 3, .window = 100, .generation = 1, .slot = 0, .frame_sequence = 1 });
    try std_t.testing.expect(replacement > tickets[MaxRecords - 1]);
    try std_t.testing.expectError(error.UnknownTicket, registry.mark_attached(3, tickets[0]));
    try std_t.testing.expectEqual(MaxRecords, registry.counts().total);
}

test "drain and quarantine stop admission but retain completions" {
    var registry = try registry_t.init(5);
    const ticket = try registry.begin_import(5, .{ .session_epoch = 5, .window = 1, .generation = 1, .slot = 0, .frame_sequence = 1 });
    try registry.begin_drain(5);
    try registry.begin_drain(5);
    try std_t.testing.expectEqual(state_t.Draining, registry.counts().state);
    const draining = registry;
    try std_t.testing.expectError(error.AdmissionClosed, registry.begin_import(5, .{ .session_epoch = 5, .window = 1, .generation = 1, .slot = 1, .frame_sequence = 1 }));
    try std_t.testing.expectEqualDeep(draining, registry);
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.begin_drain(6));
    try std_t.testing.expectEqualDeep(draining, registry);
    try registry.mark_attached(5, ticket);
    try registry.quarantine(5);
    try registry.quarantine(5);
    try std_t.testing.expectEqual(state_t.Quarantined, registry.counts().state);
    try std_t.testing.expectEqual(@as(usize, 1), registry.counts().attached);
    try std_t.testing.expectError(error.AdmissionClosed, registry.begin_import(5, .{ .session_epoch = 5, .window = 2, .generation = 1, .slot = 0, .frame_sequence = 1 }));
    try registry.buffer_released(5, ticket);
    try registry.mark_slot_free(5, ticket);
    try registry.notify_committed(5, ticket);
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
    try std_t.testing.expectEqual(state_t.Quarantined, registry.counts().state);
}

test "quarantine from open and ticket exhaustion never wraps or reuses" {
    var registry = try registry_t.init(6);
    test_support_t.set_next_ticket(&registry, std_t.math.maxInt(ticket_t));
    const max_ticket = try registry.begin_import(6, .{ .session_epoch = 6, .window = 1, .generation = 1, .slot = 0, .frame_sequence = 1 });
    try std_t.testing.expectEqual(std_t.math.maxInt(ticket_t), max_ticket);
    const exhausted = registry;
    try std_t.testing.expectError(error.TicketExhausted, registry.begin_import(6, .{ .session_epoch = 6, .window = 1, .generation = 1, .slot = 1, .frame_sequence = 1 }));
    try std_t.testing.expectEqualDeep(exhausted, registry);
    try registry.quarantine(6);
    try std_t.testing.expectEqual(state_t.Quarantined, registry.counts().state);
    const quarantined = registry;
    try std_t.testing.expectError(error.WrongState, registry.begin_drain(6));
    try std_t.testing.expectEqualDeep(quarantined, registry);
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.quarantine(7));
    try std_t.testing.expectEqualDeep(quarantined, registry);
}

test "completion lookup rejects wrong epoch unknown ticket and pending phase unchanged" {
    var registry = try registry_t.init(10);
    const ticket = try registry.begin_import(10, .{ .session_epoch = 10, .window = 4, .generation = 2, .slot = 0, .frame_sequence = 1 });
    const before = registry;
    try std_t.testing.expectError(error.InvalidSessionEpoch, registry.get_completion(0, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.get_completion(11, ticket));
    try std_t.testing.expectError(error.InvalidTicket, registry.get_completion(10, 0));
    try std_t.testing.expectError(error.UnknownTicket, registry.get_completion(10, 2));
    try std_t.testing.expectError(error.WrongState, registry.get_completion(10, ticket));
    try std_t.testing.expectEqualDeep(before, registry);
}
