//! Copied first-color pipeline/pass compatibility; caller validates complete attachment
//! signatures and owns native lifetimes. NoColor explicitly represents a zero-color scope.
const std = @import("std");
/// Immutable core RGBA8_UNORM format37; static lifetime, no owner or synchronization.
pub const Rgba8Unorm: u32 = 37;
/// Immutable core BGRA8_UNORM format44; static lifetime, no owner or synchronization.
pub const Bgra8Unorm: u32 = 44;
/// Explicit no-color compatibility key; never a wire VkFormat or active-scope absence.
/// Immutable scalar owned by no resource, safe for concurrent readers.
pub const NoColor: u32 = 0xffffffff;
/// Owned scalar recording state; no heap/native pointers. Caller serializes mutations.
/// Pipeline token/format survive pass end; zero active_format means outside a pass.
pub const recording_t = struct {
    /// Borrowed guest identity of bound pipeline; caller retains through GPU completion.
    pipeline: u64 = 0,
    /// Copied compatible color attachment format; zero means no bound pipeline.
    pipeline_format: u32 = 0,
    /// Copied active pass format; zero means outside any render pass.
    active_format: u32 = 0,
};
fn valid_format(format: u32) bool {
    return (format > 0 and format <= 184) or format == NoColor;
}
/// [in,out] state nonnull owned metadata; [in] pipeline nonzero guest identity,
/// format copied canonical attachment format. No native pointer retained/allocation.
/// Returns Invalid preserving state for zero/unsupported inputs; success replaces
/// only bound pipeline, even outside or in an incompatible pass. Caller serializes.
pub fn bind_pipeline(state: *recording_t, pipeline: u64, format: u32) !void {
    if (pipeline == 0 or !valid_format(format)) return error.Invalid;
    state.pipeline = pipeline;
    state.pipeline_format = format;
}
/// [in,out] state nonnull owned metadata; [in] format copied canonical pass format.
/// Returns Invalid preserving state on nesting/unsupported formats, otherwise enters
/// a pass without disturbing bound pipeline. No allocation; caller serializes.
pub fn begin_pass(state: *recording_t, format: u32) !void {
    if (state.active_format != 0 or !valid_format(format)) return error.Invalid;
    state.active_format = format;
}
/// [in] state nonnull call-lifetime metadata. Returns bound pipeline token only
/// inside a compatible active pass; Invalid if outside/unbound/incompatible.
/// No mutation/allocation; caller preserves native pipeline and attachment lifetimes.
pub fn draw_pipeline(state: *const recording_t) !u64 {
    if (state.active_format == 0 or state.pipeline == 0 or state.pipeline_format != state.active_format)
        return error.Invalid;
    return state.pipeline;
}
/// [in,out] state nonnull metadata. Returns Invalid preserving state outside a pass;
/// otherwise exits while preserving pipeline. No allocation; caller serializes.
pub fn end_pass(state: *recording_t) !void {
    if (state.active_format == 0) return error.Invalid;
    state.active_format = 0;
}
/// [in] state nonnull call-lifetime metadata. Returns Invalid if a pass remains
/// active, otherwise success; no mutation/allocation, safe with immutable metadata.
pub fn finish(state: *const recording_t) !void {
    if (state.active_format != 0) return error.Invalid;
}
/// [in,out] state nonnull owned metadata; scrub all copied tokens/formats after
/// native reset acknowledgment. No native destruction/allocation; caller serializes.
pub fn reset(state: *recording_t) void {
    state.* = .{};
}

// Test-only fixtures.
fn bind_fixture(state: *recording_t, pipeline: u64, format: u32) !void {
    return @call(.never_inline, bind_pipeline, .{ state, pipeline, format });
}
fn begin_fixture(state: *recording_t, format: u32) !void {
    return @call(.never_inline, begin_pass, .{ state, format });
}
fn draw_fixture(state: *const recording_t) !u64 {
    return @call(.never_inline, draw_pipeline, .{state});
}
fn end_fixture(state: *recording_t) !void {
    return @call(.never_inline, end_pass, .{state});
}
fn finish_fixture(state: *const recording_t) !void {
    return @call(.never_inline, finish, .{state});
}
test "bind before begin and repeated compatible passes preserve compiled definition" {
    var state = recording_t{};
    try bind_fixture(&state, 42, Rgba8Unorm);
    try std.testing.expectError(error.Invalid, draw_fixture(&state));
    for (0..16) |_| {
        try begin_fixture(&state, Rgba8Unorm);
        try std.testing.expectEqual(@as(u64, 42), try draw_fixture(&state));
        try std.testing.expectError(error.Invalid, finish_fixture(&state));
        try end_fixture(&state);
        try finish_fixture(&state);
    }
    @call(.never_inline, reset, .{&state});
    try std.testing.expectEqualDeep(recording_t{}, state);
    try std.testing.expectError(error.Invalid, draw_fixture(&state));
}
test "invalid ordering and scalar errors preserve every field" {
    var state = recording_t{};
    try std.testing.expectError(error.Invalid, end_fixture(&state));
    try finish_fixture(&state);
    for ([_]u32{ 0, 185, 0xfffffffe }) |format| {
        const before = state;
        try std.testing.expectError(error.Invalid, begin_fixture(&state, format));
        try std.testing.expectEqualDeep(before, state);
        try std.testing.expectError(error.Invalid, bind_fixture(&state, 42, format));
        try std.testing.expectEqualDeep(before, state);
    }
    try begin_fixture(&state, Bgra8Unorm);
    const active = state;
    try std.testing.expectError(error.Invalid, draw_fixture(&state));
    try std.testing.expectError(error.Invalid, bind_fixture(&state, 0, Bgra8Unorm));
    try std.testing.expectEqualDeep(active, state);
    try std.testing.expectError(error.Invalid, begin_fixture(&state, Bgra8Unorm));
    try std.testing.expectEqualDeep(active, state);
    try bind_fixture(&state, 43, Rgba8Unorm);
    const incompatible = state;
    try std.testing.expectError(error.Invalid, draw_fixture(&state));
    try std.testing.expectEqualDeep(incompatible, state);
    try bind_fixture(&state, 44, Bgra8Unorm);
    try std.testing.expectEqual(@as(u64, 44), try draw_fixture(&state));
    try end_fixture(&state);
    const ended = state;
    try std.testing.expectError(error.Invalid, end_fixture(&state));
    try std.testing.expectEqualDeep(ended, state);
    try begin_fixture(&state, Rgba8Unorm);
    try std.testing.expectError(error.Invalid, draw_fixture(&state));
    @call(.never_inline, reset, .{&state});
    try std.testing.expectEqualDeep(recording_t{}, state);
    try finish_fixture(&state);
}

test "first color compatibility supports srgb compressed formats and no color scopes" {
    for ([_]u32{ 1, 43, 50, 131, 146, 184, NoColor }) |format| {
        var state = recording_t{};
        try bind_pipeline(&state, 17, format);
        try begin_pass(&state, format);
        try std.testing.expectEqual(@as(u64, 17), try draw_pipeline(&state));
        try end_pass(&state);
        try finish(&state);
    }
}
