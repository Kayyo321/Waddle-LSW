const std_t = @import("std");
const registry_t = @import("registry");

const SessionEpoch: u64 = 0x1122;
const MaxRecords: usize = 192;

fn key_for(window: u64, generation: u64, slot: u32, sequence: u64) registry_t.frame_key_t {
    return .{
        .session_epoch = SessionEpoch,
        .window = window,
        .generation = generation,
        .slot = slot,
        .frame_sequence = sequence,
    };
}

fn expect_registry_equal(expected: registry_t.registry_t, actual: registry_t.registry_t) !void {
    try std_t.testing.expectEqualDeep(expected, actual);
}

fn finish_pending_never_attached(registry: *registry_t.registry_t, ticket: registry_t.ticket_t) !void {
    try registry.complete_never_attached(SessionEpoch, ticket, true);
    try registry.mark_slot_free(SessionEpoch, ticket);
    try registry.notify_committed(SessionEpoch, ticket);
}

test "admission validates epoch key fields and preserves state on errors" {
    try std_t.testing.expectError(error.InvalidSessionEpoch, registry_t.registry_t.init(0));
    var registry = try registry_t.registry_t.init(SessionEpoch);
    const empty = registry;

    try std_t.testing.expectError(error.WrongSessionEpoch, registry.begin_import(SessionEpoch + 1, key_for(1, 1, 0, 1)));
    try expect_registry_equal(empty, registry);

    var bad_key = key_for(1, 1, 0, 1);
    bad_key.session_epoch = SessionEpoch + 1;
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.begin_import(SessionEpoch, bad_key));
    bad_key = key_for(0, 1, 0, 1);
    try std_t.testing.expectError(error.InvalidWindow, registry.begin_import(SessionEpoch, bad_key));
    bad_key = key_for(1, 0, 0, 1);
    try std_t.testing.expectError(error.InvalidGeneration, registry.begin_import(SessionEpoch, bad_key));
    bad_key = key_for(1, 1, 3, 1);
    try std_t.testing.expectError(error.InvalidSlot, registry.begin_import(SessionEpoch, bad_key));
    bad_key = key_for(1, 1, 0, 0);
    try std_t.testing.expectError(error.InvalidFrameSequence, registry.begin_import(SessionEpoch, bad_key));
    try expect_registry_equal(empty, registry);

    const first_ticket = try registry.begin_import(SessionEpoch, key_for(1, 1, 0, 1));
    try std_t.testing.expectEqual(@as(u64, 1), first_ticket);
    const occupied = registry;
    try std_t.testing.expectError(error.DuplicateSlot, registry.begin_import(SessionEpoch, key_for(1, 2, 0, 2)));
    try expect_registry_equal(occupied, registry);

    const second_ticket = try registry.begin_import(SessionEpoch, key_for(1, 1, 1, 1));
    try std_t.testing.expect(second_ticket > first_ticket);
    try finish_pending_never_attached(&registry, first_ticket);
    try finish_pending_never_attached(&registry, second_ticket);
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
}

test "ticket transitions enforce proof ordering and preserve counts on failure" {
    var registry = try registry_t.registry_t.init(SessionEpoch);
    const ticket = try registry.begin_import(SessionEpoch, key_for(7, 3, 2, 91));
    const pending = registry;

    try std_t.testing.expectError(error.InvalidSessionEpoch, registry.mark_attached(0, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.mark_attached(SessionEpoch + 1, ticket));
    try std_t.testing.expectError(error.UnknownTicket, registry.mark_attached(SessionEpoch, ticket + 1));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.complete_never_attached(SessionEpoch + 1, ticket, true));
    try std_t.testing.expectError(error.WrongState, registry.buffer_released(SessionEpoch, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.buffer_released(SessionEpoch + 1, ticket));
    try std_t.testing.expectError(error.WrongState, registry.mark_slot_free(SessionEpoch, ticket));
    try std_t.testing.expectError(error.WrongState, registry.notify_committed(SessionEpoch, ticket));
    try std_t.testing.expectError(error.WrongState, registry.get_completion(SessionEpoch, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.get_completion(SessionEpoch + 1, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.mark_slot_free(SessionEpoch + 1, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.notify_committed(SessionEpoch + 1, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.begin_drain(SessionEpoch + 1));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.quarantine(SessionEpoch + 1));
    try std_t.testing.expectError(error.CallbacksNotQuiesced, registry.complete_never_attached(SessionEpoch, ticket, false));
    try expect_registry_equal(pending, registry);

    try registry.mark_attached(SessionEpoch, ticket);
    const attached = registry;
    try std_t.testing.expectError(error.WrongState, registry.mark_attached(SessionEpoch, ticket));
    try std_t.testing.expectError(error.WrongState, registry.complete_never_attached(SessionEpoch, ticket, true));
    try expect_registry_equal(attached, registry);

    try registry.buffer_released(SessionEpoch, ticket);
    const released = registry;
    try std_t.testing.expectError(error.WrongState, registry.buffer_released(SessionEpoch, ticket));
    try std_t.testing.expectError(error.WrongState, registry.notify_committed(SessionEpoch, ticket));
    try std_t.testing.expectError(error.WrongSessionEpoch, registry.mark_slot_free(SessionEpoch + 1, ticket));
    try expect_registry_equal(released, registry);

    const completion = try registry.get_completion(SessionEpoch, ticket);
    try std_t.testing.expectEqual(key_for(7, 3, 2, 91), completion.key);
    try std_t.testing.expectEqual(registry_t.completion_kind_t.CompositorRelease, completion.kind);
    try std_t.testing.expect(!completion.slot_free);
    try registry.mark_slot_free(SessionEpoch, ticket);
    try std_t.testing.expectError(error.SlotAlreadyFree, registry.mark_slot_free(SessionEpoch, ticket));
    const slot_free_completion = try registry.get_completion(SessionEpoch, ticket);
    try std_t.testing.expect(slot_free_completion.slot_free);
    try registry.notify_committed(SessionEpoch, ticket);
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
    try std_t.testing.expectError(error.UnknownTicket, registry.get_completion(SessionEpoch, ticket));
}

test "never-attached completion requires quiesced callback and exact release sequence" {
    var registry = try registry_t.registry_t.init(SessionEpoch);
    const ticket = try registry.begin_import(SessionEpoch, key_for(9, 2, 0, 4));
    const before = registry;
    try std_t.testing.expectError(error.CallbacksNotQuiesced, registry.complete_never_attached(SessionEpoch, ticket, false));
    try expect_registry_equal(before, registry);

    try registry.complete_never_attached(SessionEpoch, ticket, true);
    const completion = try registry.get_completion(SessionEpoch, ticket);
    try std_t.testing.expectEqual(registry_t.completion_kind_t.NeverAttached, completion.kind);
    try std_t.testing.expectEqual(key_for(9, 2, 0, 4), completion.key);
    try std_t.testing.expect(!completion.slot_free);

    try std_t.testing.expectError(error.WrongState, registry.buffer_released(SessionEpoch, ticket));
    try registry.mark_slot_free(SessionEpoch, ticket);
    try registry.notify_committed(SessionEpoch, ticket);
    try std_t.testing.expectError(error.UnknownTicket, registry.notify_committed(SessionEpoch, ticket));
}

test "stale ticket cannot complete a reused slot" {
    var registry = try registry_t.registry_t.init(SessionEpoch);
    const old_ticket = try registry.begin_import(SessionEpoch, key_for(11, 4, 1, 20));
    try finish_pending_never_attached(&registry, old_ticket);

    const new_ticket = try registry.begin_import(SessionEpoch, key_for(11, 5, 1, 21));
    try std_t.testing.expect(new_ticket > old_ticket);
    const before = registry;
    try std_t.testing.expectError(error.UnknownTicket, registry.mark_attached(SessionEpoch, old_ticket));
    try std_t.testing.expectError(error.UnknownTicket, registry.buffer_released(SessionEpoch, old_ticket));
    try std_t.testing.expectError(error.UnknownTicket, registry.mark_slot_free(SessionEpoch, old_ticket));
    try std_t.testing.expectError(error.UnknownTicket, registry.notify_committed(SessionEpoch, old_ticket));
    try expect_registry_equal(before, registry);
    try finish_pending_never_attached(&registry, new_ticket);
}

test "drain and quarantine retain pending and attached records while allowing exact completion" {
    var registry = try registry_t.registry_t.init(SessionEpoch);
    const pending_ticket = try registry.begin_import(SessionEpoch, key_for(21, 1, 0, 1));
    const attached_ticket = try registry.begin_import(SessionEpoch, key_for(21, 1, 1, 1));
    try registry.mark_attached(SessionEpoch, attached_ticket);

    try registry.begin_drain(SessionEpoch);
    try registry.begin_drain(SessionEpoch);
    const draining_snapshot = registry;
    const draining = registry.counts();
    try std_t.testing.expectEqual(registry_t.state_t.Draining, draining.state);
    try std_t.testing.expectEqual(@as(usize, 2), draining.total);
    try std_t.testing.expectError(error.AdmissionClosed, registry.begin_import(SessionEpoch, key_for(22, 1, 0, 1)));
    try expect_registry_equal(draining_snapshot, registry);

    try registry.quarantine(SessionEpoch);
    try registry.quarantine(SessionEpoch);
    const quarantine_snapshot = registry;
    const quarantined = registry.counts();
    try std_t.testing.expectEqual(registry_t.state_t.Quarantined, quarantined.state);
    try std_t.testing.expectEqual(@as(usize, 2), quarantined.total);
    try std_t.testing.expectError(error.AdmissionClosed, registry.begin_import(SessionEpoch, key_for(22, 1, 0, 1)));
    try expect_registry_equal(quarantine_snapshot, registry);

    try finish_pending_never_attached(&registry, pending_ticket);
    try registry.buffer_released(SessionEpoch, attached_ticket);
    try registry.mark_slot_free(SessionEpoch, attached_ticket);
    try registry.notify_committed(SessionEpoch, attached_ticket);
    const empty_but_quarantined = registry.counts();
    try std_t.testing.expectEqual(@as(usize, 0), empty_but_quarantined.total);
    try std_t.testing.expectEqual(registry_t.state_t.Quarantined, empty_but_quarantined.state);
    try std_t.testing.expectError(error.AdmissionClosed, registry.begin_import(SessionEpoch, key_for(22, 1, 0, 1)));
}

test "all 192 records are bounded and capacity failure is transactional" {
    var registry = try registry_t.registry_t.init(SessionEpoch);
    var tickets: [MaxRecords]registry_t.ticket_t = undefined;
    for (0..MaxRecords) |index| {
        const window = @as(u64, @intCast(index / 3)) + 1;
        const slot: u32 = @intCast(index % 3);
        tickets[index] = try registry.begin_import(SessionEpoch, key_for(window, 1, slot, @as(u64, @intCast(index)) + 1));
        try std_t.testing.expectEqual(@as(u64, @intCast(index)) + 1, tickets[index]);
    }
    const full = registry;
    const full_counts = full.counts();
    try std_t.testing.expectEqual(MaxRecords, full_counts.total);
    try std_t.testing.expectEqual(MaxRecords, full_counts.import_pending);
    try std_t.testing.expectError(error.CapacityFull, registry.begin_import(SessionEpoch, key_for(1000, 1, 0, 1)));
    try expect_registry_equal(full, registry);

    try finish_pending_never_attached(&registry, tickets[0]);
    const replacement = try registry.begin_import(SessionEpoch, key_for(1, 2, 0, 2));
    try std_t.testing.expectEqual(@as(u64, MaxRecords + 1), replacement);
    try std_t.testing.expectEqual(MaxRecords, registry.counts().total);
}

test "ticket UINT64_MAX is usable once and exhaustion never wraps" {
    var registry = try registry_t.registry_t.init(SessionEpoch);
    registry_t.test_support_t.set_next_ticket(&registry, std_t.math.maxInt(u64));
    const max_ticket = try registry.begin_import(SessionEpoch, key_for(31, 1, 0, 1));
    try std_t.testing.expectEqual(std_t.math.maxInt(u64), max_ticket);
    const full = registry;
    try std_t.testing.expectError(error.TicketExhausted, registry.begin_import(SessionEpoch, key_for(32, 1, 0, 1)));
    try expect_registry_equal(full, registry);
    try finish_pending_never_attached(&registry, max_ticket);
    try std_t.testing.expectError(error.TicketExhausted, registry.begin_import(SessionEpoch, key_for(32, 1, 0, 1)));
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
}

test "192-frame varied completion and slot-reuse churn stays bounded" {
    var registry = try registry_t.registry_t.init(SessionEpoch);
    var tickets: [MaxRecords]registry_t.ticket_t = undefined;
    var generations: [MaxRecords]u64 = undefined;
    var sequences: [MaxRecords]u64 = undefined;

    for (0..MaxRecords) |index| {
        const window = @as(u64, @intCast(index / 3)) + 1;
        const slot: u32 = @intCast(index % 3);
        generations[index] = 1;
        sequences[index] = 1;
        tickets[index] = try registry.begin_import(SessionEpoch, key_for(window, 1, slot, 1));
    }

    for (0..MaxRecords) |step| {
        const index = (step * 73) % MaxRecords;
        const ticket = tickets[index];
        if (step % 2 == 0) {
            try registry.mark_attached(SessionEpoch, ticket);
            try registry.buffer_released(SessionEpoch, ticket);
        } else {
            try registry.complete_never_attached(SessionEpoch, ticket, true);
        }
        try registry.mark_slot_free(SessionEpoch, ticket);
        try registry.notify_committed(SessionEpoch, ticket);

        generations[index] += 1;
        sequences[index] += 1;
        const new_ticket = try registry.begin_import(
            SessionEpoch,
            key_for(@as(u64, @intCast(index / 3)) + 1, generations[index], @intCast(index % 3), sequences[index]),
        );
        try std_t.testing.expect(new_ticket > ticket);
        tickets[index] = new_ticket;
        try std_t.testing.expectEqual(MaxRecords, registry.counts().total);
    }

    for (0..MaxRecords) |step| {
        const index = (step * 53) % MaxRecords;
        try finish_pending_never_attached(&registry, tickets[index]);
        try std_t.testing.expectEqual(MaxRecords - step - 1, registry.counts().total);
    }
    try std_t.testing.expectEqual(@as(usize, 0), registry.counts().total);
}
