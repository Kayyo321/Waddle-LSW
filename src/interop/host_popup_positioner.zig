//! Pure popup positioner geometry for host xdg_popup requests (TODO #11c).
//!
//! `request` converts a guest popup rectangle in physical pixels into the logical positioner values
//! for a direct parent of logical size `(pw, ph)` and integer host scale `s`. The anchor point is
//! clamped into the parent's window geometry and the remainder becomes the positioner offset.
//! `reposition` maps the compositor-adjusted position back into guest pixels relative to the requested
//! origin, keeping the guest's own size. Both are pure functions over value types: no allocation, no
//! operating-system call, no retained state.
//!
//! The contract is `impl/seamless-interop/claude_45_percent/host_popup_positioner_contract.md`.
const std = @import("std");

/// Maximum accepted integer host scale. Fractional scales are rounded up by `integer_scale`.
pub const MaxScale: u32 = 8;
/// Maximum absolute logical origin, dimension or parent size accepted by `request`.
pub const MaxLogicalExtent: i64 = 32768;
/// xdg_positioner anchor value `top_left`.
pub const AnchorTopLeft: u32 = 5;
/// xdg_positioner gravity value `bottom_right`.
pub const GravityBottomRight: u32 = 8;
/// xdg_positioner constraint_adjustment bits `slide_x | slide_y | flip_x | flip_y`.
pub const ConstraintSlideFlip: u32 = 15;

/// Rectangle in one of the coordinate spaces named by the function that uses it. See the contract
/// for the meaning of each use.
pub const rect_t = extern struct {
    /// Signed horizontal origin.
    x: i32,
    /// Signed vertical origin.
    y: i32,
    /// Unsigned width.
    width: u32,
    /// Unsigned height.
    height: u32,
};

/// Complete positioner request for one popup, ready for the xdg_positioner setters.
pub const popup_request_t = extern struct {
    /// `set_anchor_rect` x: the clamped anchor point, inside the parent window geometry.
    anchor_x: i32,
    /// `set_anchor_rect` y: the clamped anchor point, inside the parent window geometry.
    anchor_y: i32,
    /// `set_anchor_rect` width; always 1.
    anchor_width: u32,
    /// `set_anchor_rect` height; always 1.
    anchor_height: u32,
    /// `set_offset` x: the part of the requested origin that clamping removed.
    offset_x: i32,
    /// `set_offset` y: the part of the requested origin that clamping removed.
    offset_y: i32,
    /// `set_size` width, logical units, at least 1.
    width: u32,
    /// `set_size` height, logical units, at least 1.
    height: u32,
    /// `set_anchor` value; always AnchorTopLeft.
    anchor: u32,
    /// `set_gravity` value; always GravityBottomRight.
    gravity: u32,
    /// `set_constraint_adjustment` mask; always ConstraintSlideFlip.
    constraint_adjustment: u32,
};

/// Result code of the C ABI exports.
pub const status_t = enum(c_int) {
    /// The output was written.
    Ok = 0,
    /// A NULL pointer was passed.
    InvalidArgument = 1,
    /// The scale is outside 1..8, or a fractional scale rounds outside that range.
    InvalidScale = 2,
    /// A logical origin, dimension or parent size exceeds MaxLogicalExtent in absolute value.
    OutOfRange = 3,
    /// A scaled coordinate or dimension does not fit its output type.
    Overflow = 4,
    /// A parent width or height is zero.
    InvalidParent = 5,
};

/// Errors of the pure functions. Each one is mapped to a `status_t` by the C ABI.
pub const positioner_error_t = error{
    InvalidScale,
    OutOfRange,
    Overflow,
    InvalidParent,
};

fn validate_scale(scale: u32) positioner_error_t!void {
    if (scale < 1 or scale > MaxScale) return error.InvalidScale;
}

fn validate_parent(width: u32, height: u32) positioner_error_t!void {
    if (width == 0 or height == 0) return error.InvalidParent;
    if (@as(i64, width) > MaxLogicalExtent or @as(i64, height) > MaxLogicalExtent) return error.OutOfRange;
}

fn outside_extent(value: i64) bool {
    return value < -MaxLogicalExtent or value > MaxLogicalExtent;
}

/// Floor of `value / divisor` for a positive divisor, rounding toward negative infinity.
fn floor_div(value: i64, divisor: i64) i64 {
    const quotient = @divTrunc(value, divisor);
    return if (value < 0 and quotient * divisor != value) quotient - 1 else quotient;
}

/// Ceiling of `value / divisor` for a positive divisor and a non-negative value, in 64-bit space.
fn ceil_div(value: u32, divisor: u32) i64 {
    return @divTrunc(@as(i64, value) + @as(i64, divisor) - 1, @as(i64, divisor));
}

/// Clamp `value` into the closed interval `[low, high]`.
fn clamp_to(value: i64, low: i64, high: i64) i64 {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/// Integer host scale for a fractional scale `numerator / denominator`, rounded up.
///
/// @param numerator Fraction numerator, e.g. 150 for 1.25 when `denominator` is 120.
/// @param denominator Fraction denominator; zero is refused.
/// @return `ceil(numerator / denominator)`, in 1..8.
/// @error InvalidScale The denominator is zero, or the rounded result is 0 or above 8.
pub noinline fn integer_scale(numerator: u32, denominator: u32) positioner_error_t!u32 {
    const scale = std.math.divCeil(u64, @as(u64, numerator), @as(u64, denominator)) catch return error.InvalidScale;
    if (scale < 1 or scale > MaxScale) return error.InvalidScale;
    return @intCast(scale);
}

/// Logical popup request for a guest rectangle relative to a direct parent of logical size `(parent_width, parent_height)`.
///
/// The requested origin is `lx = floor(x / s)`, `ly = floor(y / s)` (toward negative infinity). Each
/// dimension is `max(1, ceil(w / s))`. The anchor point is clamped into the parent's window geometry,
/// `ax = clamp(lx, 0, parent_width - 1)` and likewise for y, and the remainder is the offset
/// `(lx - ax, ly - ay)`. The offset is therefore never larger than the requested origin, so its
/// magnitude is bounded by MaxLogicalExtent whenever the origin is.
///
/// @param guest Guest physical rectangle relative to the direct parent's top-left.
/// @param scale Integer host scale, 1..8.
/// @param parent_width Logical width of the direct parent, 1..32768.
/// @param parent_height Logical height of the direct parent, 1..32768.
/// @return The positioner values for the xdg_positioner setters.
/// @error InvalidScale The scale is outside 1..8.
/// @error InvalidParent A parent dimension is zero.
/// @error OutOfRange A parent dimension, a requested origin or a dimension exceeds 32768 in absolute value.
pub noinline fn request(guest: rect_t, scale: u32, parent_width: u32, parent_height: u32) positioner_error_t!popup_request_t {
    try validate_scale(scale);
    try validate_parent(parent_width, parent_height);
    const s: i64 = scale;
    const lx = floor_div(@as(i64, guest.x), s);
    const ly = floor_div(@as(i64, guest.y), s);
    const width = @max(1, ceil_div(guest.width, scale));
    const height = @max(1, ceil_div(guest.height, scale));
    if (outside_extent(lx) or outside_extent(ly) or outside_extent(width) or outside_extent(height)) return error.OutOfRange;

    const ax = clamp_to(lx, 0, @as(i64, parent_width) - 1);
    const ay = clamp_to(ly, 0, @as(i64, parent_height) - 1);
    return .{
        .anchor_x = @intCast(ax),
        .anchor_y = @intCast(ay),
        .anchor_width = 1,
        .anchor_height = 1,
        .offset_x = @intCast(lx - ax),
        .offset_y = @intCast(ly - ay),
        .width = @intCast(width),
        .height = @intCast(height),
        .anchor = AnchorTopLeft,
        .gravity = GravityBottomRight,
        .constraint_adjustment = ConstraintSlideFlip,
    };
}

/// Guest rectangle for a popup whose compositor-adjusted top-left is `(adjusted_x, adjusted_y)`.
///
/// The requested logical origin is `anchor + offset`. The compositor displacement from that origin is
/// scaled back to guest pixels: `x' = x + (adjusted_x - origin_x) * scale`, and likewise for y. The
/// guest's own width and height are returned unchanged, because the constraint mask has no resize
/// bits. An unmoved popup therefore reports exactly its requested rectangle.
///
/// @param guest The rectangle that was requested, in guest physical pixels.
/// @param scale Integer host scale, 1..8.
/// @param requested The request produced by `request` for `guest` and the same scale.
/// @param adjusted_x Compositor top-left x in the direct parent's logical coordinates.
/// @param adjusted_y Compositor top-left y in the direct parent's logical coordinates.
/// @return Guest physical rectangle for PopupRepositioned.
/// @error InvalidScale The scale is outside 1..8.
/// @error Overflow `x'` or `y'` does not fit `i32`.
pub noinline fn reposition(guest: rect_t, scale: u32, requested: popup_request_t, adjusted_x: i32, adjusted_y: i32) positioner_error_t!rect_t {
    try validate_scale(scale);
    const s: i64 = scale;
    const origin_x = @as(i64, requested.anchor_x) + requested.offset_x;
    const origin_y = @as(i64, requested.anchor_y) + requested.offset_y;
    const x = std.math.cast(i32, @as(i64, guest.x) + (@as(i64, adjusted_x) - origin_x) * s) orelse return error.Overflow;
    const y = std.math.cast(i32, @as(i64, guest.y) + (@as(i64, adjusted_y) - origin_y) * s) orelse return error.Overflow;
    return .{ .x = x, .y = y, .width = guest.width, .height = guest.height };
}

// ---- C ABI ------------------------------------------------------------------------------------

/// Map a pure-function error to its C ABI status.
fn status_of(err: positioner_error_t) status_t {
    return switch (err) {
        error.InvalidScale => .InvalidScale,
        error.OutOfRange => .OutOfRange,
        error.Overflow => .Overflow,
        error.InvalidParent => .InvalidParent,
    };
}

/// C ABI: integer host scale for a fractional scale. See `integer_scale`.
///
/// @param numerator In. Fraction numerator.
/// @param denominator In. Fraction denominator; zero gives HostPopupInvalidScale.
/// @param out_scale Out. Receives the integer scale on success only. NULL gives HostPopupInvalidArgument.
/// @return Status. Thread safety: pure; any thread.
export fn host_popup_positioner_integer_scale(numerator: u32, denominator: u32, out_scale: ?*u32) callconv(.C) status_t {
    const destination = out_scale orelse return .InvalidArgument;
    const scale = integer_scale(numerator, denominator) catch |err| return status_of(err);
    destination.* = scale;
    return .Ok;
}

/// C ABI: logical popup request for a guest rectangle relative to its direct parent. See `request`.
///
/// @param guest In. Guest physical rectangle. NULL gives HostPopupInvalidArgument.
/// @param scale In. Integer host scale, 1..8.
/// @param parent_width In. Logical width of the direct parent, 1..32768.
/// @param parent_height In. Logical height of the direct parent, 1..32768.
/// @param out Out. Receives the request on success only. NULL gives HostPopupInvalidArgument.
/// @return Status. Thread safety: pure; any thread.
export fn host_popup_positioner_request(guest: ?*const rect_t, scale: u32, parent_width: u32, parent_height: u32, out: ?*popup_request_t) callconv(.C) status_t {
    const input = (guest orelse return .InvalidArgument).*;
    const destination = out orelse return .InvalidArgument;
    const result = request(input, scale, parent_width, parent_height) catch |err| return status_of(err);
    destination.* = result;
    return .Ok;
}

/// C ABI: guest rectangle for a compositor-adjusted popup position. See `reposition`.
///
/// @param guest In. The requested guest rectangle. NULL gives HostPopupInvalidArgument.
/// @param scale In. Integer host scale, 1..8.
/// @param requested In. The request produced for `guest`. NULL gives HostPopupInvalidArgument.
/// @param adjusted_x In. Compositor top-left x in the direct parent's logical coordinates.
/// @param adjusted_y In. Compositor top-left y in the direct parent's logical coordinates.
/// @param out Out. Receives the guest rectangle on success only. NULL gives HostPopupInvalidArgument.
/// @return Status. Thread safety: pure; any thread.
export fn host_popup_positioner_reposition(guest: ?*const rect_t, scale: u32, requested: ?*const popup_request_t, adjusted_x: i32, adjusted_y: i32, out: ?*rect_t) callconv(.C) status_t {
    const input = (guest orelse return .InvalidArgument).*;
    const request_value = (requested orelse return .InvalidArgument).*;
    const destination = out orelse return .InvalidArgument;
    const result = reposition(input, scale, request_value, adjusted_x, adjusted_y) catch |err| return status_of(err);
    destination.* = result;
    return .Ok;
}

comptime {
    // Layout frozen by src/interop/host_popup_positioner.h (checked there by _Static_assert).
    if (@sizeOf(rect_t) != 16) @compileError("host popup rect layout changed");
    if (@sizeOf(popup_request_t) != 44) @compileError("host popup request layout changed");
}

// Test-only fixtures.
const testing = std.testing;

fn make_rect(x: i32, y: i32, width: u32, height: u32) rect_t {
    return .{ .x = x, .y = y, .width = width, .height = height };
}

/// Independent floor division: computed from the remainder instead of `@divFloor`.
fn reference_floor(value: i64, scale: i64) i64 {
    const remainder = @mod(value, scale);
    return @divExact(value - remainder, scale);
}

/// Independent reference for `request`. It shares no helper with the module.
fn reference_request(guest: rect_t, scale: i64, parent_width: i64, parent_height: i64) ?popup_request_t {
    const lx = reference_floor(guest.x, scale);
    const ly = reference_floor(guest.y, scale);
    const raw_width = -@divFloor(-@as(i64, guest.width), scale);
    const raw_height = -@divFloor(-@as(i64, guest.height), scale);
    const width = if (raw_width < 1) 1 else raw_width;
    const height = if (raw_height < 1) 1 else raw_height;
    const extent: i64 = 32768;
    if (lx > extent or lx < -extent or ly > extent or ly < -extent) return null;
    if (width > extent or height > extent) return null;
    var ax = lx;
    if (ax < 0) ax = 0;
    if (ax > parent_width - 1) ax = parent_width - 1;
    var ay = ly;
    if (ay < 0) ay = 0;
    if (ay > parent_height - 1) ay = parent_height - 1;
    return .{
        .anchor_x = @intCast(ax),
        .anchor_y = @intCast(ay),
        .anchor_width = 1,
        .anchor_height = 1,
        .offset_x = @intCast(lx - ax),
        .offset_y = @intCast(ly - ay),
        .width = @intCast(width),
        .height = @intCast(height),
        .anchor = 5,
        .gravity = 8,
        .constraint_adjustment = 15,
    };
}

/// The public functions are called through `never_inline` shims so that the optimized bodies keep
/// their own branches. A call with constant arguments would otherwise fold refusal paths away.
const abi_shim_t = struct {
    fn integer_scale(numerator: u32, denominator: u32, out_scale: ?*u32) status_t {
        return @call(.never_inline, host_popup_positioner_integer_scale, .{ numerator, denominator, out_scale });
    }

    fn request(guest: ?*const rect_t, scale: u32, parent_width: u32, parent_height: u32, out: ?*popup_request_t) status_t {
        return @call(.never_inline, host_popup_positioner_request, .{ guest, scale, parent_width, parent_height, out });
    }

    fn reposition(guest: ?*const rect_t, scale: u32, requested: ?*const popup_request_t, adjusted_x: i32, adjusted_y: i32, out: ?*rect_t) status_t {
        return @call(.never_inline, host_popup_positioner_reposition, .{ guest, scale, requested, adjusted_x, adjusted_y, out });
    }
};

test "integer scale rounds fractions up and rejects zero, empty and out-of-range results" {
    try testing.expectEqual(@as(u32, 1), try integer_scale(1, 1));
    try testing.expectEqual(@as(u32, 1), try integer_scale(120, 120));
    try testing.expectEqual(@as(u32, 1), try integer_scale(60, 120));
    try testing.expectEqual(@as(u32, 2), try integer_scale(150, 120));
    try testing.expectEqual(@as(u32, 3), try integer_scale(241, 120));
    try testing.expectEqual(@as(u32, 8), try integer_scale(960, 120));
    try testing.expectError(error.InvalidScale, integer_scale(961, 120));
    try testing.expectError(error.InvalidScale, integer_scale(0, 120));
    try testing.expectError(error.InvalidScale, integer_scale(1, 0));
    try testing.expectError(error.InvalidScale, integer_scale(0, 0));
    try testing.expectError(error.InvalidScale, integer_scale(std.math.maxInt(u32), 1));
}

test "an unmoved request with the origin inside the parent has zero offset" {
    const logical = try request(make_rect(30, 12, 40, 20), 2, 1920, 1080);
    try testing.expectEqual(@as(i32, 15), logical.anchor_x);
    try testing.expectEqual(@as(i32, 6), logical.anchor_y);
    try testing.expectEqual(@as(i32, 0), logical.offset_x);
    try testing.expectEqual(@as(i32, 0), logical.offset_y);
    try testing.expectEqual(@as(u32, 20), logical.width);
    try testing.expectEqual(@as(u32, 10), logical.height);
    try testing.expectEqual(@as(u32, 1), logical.anchor_width);
    try testing.expectEqual(@as(u32, 1), logical.anchor_height);
    try testing.expectEqual(AnchorTopLeft, logical.anchor);
    try testing.expectEqual(GravityBottomRight, logical.gravity);
    try testing.expectEqual(ConstraintSlideFlip, logical.constraint_adjustment);
}

test "a negative origin is clamped to the parent edge and the remainder becomes the offset" {
    const logical = try request(make_rect(-7, -3, 4, 4), 2, 100, 100);
    // floor(-7 / 2) = -4 and floor(-3 / 2) = -2 are clamped to 0; the offsets carry them.
    try testing.expectEqual(@as(i32, 0), logical.anchor_x);
    try testing.expectEqual(@as(i32, 0), logical.anchor_y);
    try testing.expectEqual(@as(i32, -4), logical.offset_x);
    try testing.expectEqual(@as(i32, -2), logical.offset_y);
}

test "an origin past the parent far edge is clamped to the last logical unit" {
    const logical = try request(make_rect(1000, 500, 4, 4), 1, 100, 60);
    try testing.expectEqual(@as(i32, 99), logical.anchor_x);
    try testing.expectEqual(@as(i32, 59), logical.anchor_y);
    try testing.expectEqual(@as(i32, 901), logical.offset_x);
    try testing.expectEqual(@as(i32, 441), logical.offset_y);
}

test "a one by one parent clamps every origin to zero" {
    const logical = try request(make_rect(5, -9, 1, 1), 1, 1, 1);
    try testing.expectEqual(@as(i32, 0), logical.anchor_x);
    try testing.expectEqual(@as(i32, 0), logical.anchor_y);
    try testing.expectEqual(@as(i32, 5), logical.offset_x);
    try testing.expectEqual(@as(i32, -9), logical.offset_y);
}

test "nested popups clamp against the direct parent and not the toplevel" {
    // A tooltip inside a 300 by 200 menu at scale 2: the guest rect is relative to the menu.
    const inside_menu = try request(make_rect(40, 450, 40, 40), 2, 300, 200);
    try testing.expectEqual(@as(i32, 20), inside_menu.anchor_x);
    try testing.expectEqual(@as(i32, 199), inside_menu.anchor_y);
    try testing.expectEqual(@as(i32, 0), inside_menu.offset_x);
    try testing.expectEqual(@as(i32, 26), inside_menu.offset_y);
    // The same rect against a 1920 by 1080 toplevel is not clamped at all.
    const against_toplevel = try request(make_rect(40, 450, 40, 40), 2, 1920, 1080);
    try testing.expectEqual(@as(i32, 225), against_toplevel.anchor_y);
    try testing.expectEqual(@as(i32, 0), against_toplevel.offset_y);
}

test "dimensions round up and never fall below one logical unit" {
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        var width: u32 = 0;
        while (width <= 20) : (width += 1) {
            const logical = try request(make_rect(0, 0, width, width), scale, 32768, 32768);
            const expected = @max(1, (@as(u64, width) + scale - 1) / scale);
            try testing.expectEqual(@as(u32, @intCast(expected)), logical.width);
            try testing.expectEqual(@as(u32, @intCast(expected)), logical.height);
        }
    }
    try testing.expectEqual(@as(u32, 1), (try request(make_rect(0, 0, 0, 0), 8, 10, 10)).width);
    try testing.expectEqual(@as(u32, 3), (try request(make_rect(0, 0, 5, 5), 2, 10, 10)).width);
}

test "a zero parent dimension is refused and a parent above 32768 is out of range" {
    try testing.expectError(error.InvalidParent, request(make_rect(0, 0, 1, 1), 1, 0, 10));
    try testing.expectError(error.InvalidParent, request(make_rect(0, 0, 1, 1), 1, 10, 0));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 1, 1), 1, 32769, 10));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 1, 1), 1, 10, 32769));
    try testing.expectEqual(@as(i32, 32767), (try request(make_rect(32768, 0, 1, 1), 1, 32768, 1)).anchor_x);
}

test "origin and dimension boundaries at 32768 are accepted and 32769 is refused" {
    try testing.expectEqual(@as(i32, 32768), (try request(make_rect(32768, 0, 1, 1), 1, 1, 1)).offset_x);
    try testing.expectError(error.OutOfRange, request(make_rect(32769, 0, 1, 1), 1, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(-32769, 0, 1, 1), 1, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 32769, 1, 1), 1, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, -32769, 1, 1), 1, 1, 1));
    try testing.expectEqual(@as(u32, 32768), (try request(make_rect(0, 0, 32768, 32768), 1, 1, 1)).width);
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 32769, 1), 1, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 1, 32769), 1, 1, 1));
    // At scale 2 the boundary moves: ceil(65537 / 2) is 32769.
    try testing.expectEqual(@as(u32, 32768), (try request(make_rect(0, 0, 65536, 1), 2, 1, 1)).width);
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 65537, 1), 2, 1, 1));
    // floor(-65536 / 2) is -32768 (accepted); floor(-65537 / 2) is -32769 (refused).
    try testing.expectEqual(@as(i32, -32768), (try request(make_rect(-65536, 0, 1, 1), 2, 1, 1)).offset_x);
    try testing.expectError(error.OutOfRange, request(make_rect(-65537, 0, 1, 1), 2, 1, 1));
}

test "extreme guest values are refused as out of range without overflow" {
    try testing.expectError(error.OutOfRange, request(make_rect(std.math.maxInt(i32), 0, 1, 1), 1, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(std.math.minInt(i32), 0, 1, 1), 1, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, std.math.maxInt(u32), 1), 1, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, std.math.maxInt(u32), 1), 8, 1, 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 1, std.math.maxInt(u32)), 2, 1, 1));
}

test "invalid scales are refused by both directions" {
    try testing.expectError(error.InvalidScale, request(make_rect(0, 0, 1, 1), 0, 1, 1));
    try testing.expectError(error.InvalidScale, request(make_rect(0, 0, 1, 1), 9, 1, 1));
    const requested = try request(make_rect(0, 0, 1, 1), 1, 1, 1);
    try testing.expectError(error.InvalidScale, reposition(make_rect(0, 0, 1, 1), 0, requested, 0, 0));
    try testing.expectError(error.InvalidScale, reposition(make_rect(0, 0, 1, 1), 9, requested, 0, 0));
}

test "request matches the independent reference for exhaustive origins and seeded sizes" {
    // Every x in -300000..300000 at every scale, with a parent size drawn from a seeded stream.
    var prng = std.rand.DefaultPrng.init(0x5EED);
    const random = prng.random();
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        var x: i32 = -300000;
        while (x <= 300000) : (x += 1) {
            const parent_w: u32 = 1 + random.uintLessThan(u32, 32768);
            const parent_h: u32 = 1 + random.uintLessThan(u32, 32768);
            const guest = make_rect(x, -x, 1 + random.uintLessThan(u32, 40), 1 + random.uintLessThan(u32, 40));
            const expected = reference_request(guest, scale, parent_w, parent_h);
            if (expected) |want| {
                const got = try request(guest, scale, parent_w, parent_h);
                try testing.expectEqual(want, got);
            } else {
                try testing.expectError(error.OutOfRange, request(guest, scale, parent_w, parent_h));
            }
        }
    }
}

test "request matches the independent reference for seeded two dimensional samples and extremes" {
    var prng = std.rand.DefaultPrng.init(0xC0FFEE);
    const random = prng.random();
    const extremes = [_]i32{ std.math.minInt(i32), std.math.maxInt(i32), -32768, 32768, -32769, 32769, 0, -1, 1 };
    var sample: usize = 0;
    while (sample < 200000) : (sample += 1) {
        const x = if (sample < extremes.len * extremes.len) extremes[sample % extremes.len] else @as(i32, @intCast(random.intRangeAtMost(i64, -70000, 70000)));
        const y = if (sample < extremes.len * extremes.len) extremes[sample / extremes.len] else @as(i32, @intCast(random.intRangeAtMost(i64, -70000, 70000)));
        const w = random.uintLessThan(u32, 70000);
        const h = random.uintLessThan(u32, 70000);
        const scale = 1 + random.uintLessThan(u32, MaxScale);
        const pw = 1 + random.uintLessThan(u32, 32768);
        const ph = 1 + random.uintLessThan(u32, 32768);
        const guest = make_rect(x, y, w, h);
        if (reference_request(guest, scale, pw, ph)) |want| {
            try testing.expectEqual(want, try request(guest, scale, pw, ph));
        } else {
            try testing.expectError(error.OutOfRange, request(guest, scale, pw, ph));
        }
    }
}

/// Independent reference for `reposition`, computed in 128-bit arithmetic. Returns null on overflow.
fn reference_reposition(guest: rect_t, scale: i64, origin_x: i64, origin_y: i64, adjusted_x: i64, adjusted_y: i64) ?rect_t {
    const wide_x: i128 = @as(i128, guest.x) + (@as(i128, adjusted_x) - origin_x) * scale;
    const wide_y: i128 = @as(i128, guest.y) + (@as(i128, adjusted_y) - origin_y) * scale;
    if (wide_x < std.math.minInt(i32) or wide_x > std.math.maxInt(i32)) return null;
    if (wide_y < std.math.minInt(i32) or wide_y > std.math.maxInt(i32)) return null;
    return .{ .x = @intCast(wide_x), .y = @intCast(wide_y), .width = guest.width, .height = guest.height };
}

test "an unmoved popup reports exactly its requested rectangle at every scale and origin" {
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        var x: i32 = -300000;
        while (x <= 300000) : (x += 1) {
            const guest = make_rect(x, @divTrunc(x, 10), 3, 7);
            // Origins beyond 32768 logical units are refused by request; they have no unmoved case.
            const requested = request(guest, scale, 32768, 32768) catch continue;
            const origin_x = @as(i64, requested.anchor_x) + requested.offset_x;
            const origin_y = @as(i64, requested.anchor_y) + requested.offset_y;
            const unmoved = try reposition(guest, scale, requested, @intCast(origin_x), @intCast(origin_y));
            try testing.expectEqual(guest, unmoved);
        }
    }
}

test "a moved popup is displaced by the compositor delta scaled to guest pixels" {
    // Parent 100 by 60 at scale 3. The guest asks for (-1000 .. ) past the far edge; the compositor
    // slides the popup 7 logical units left of its requested origin.
    const guest = make_rect(1000, 500, 40, 30);
    const requested = try request(guest, 3, 100, 60);
    const origin_x = @as(i32, requested.anchor_x) + requested.offset_x;
    const origin_y = @as(i32, requested.anchor_y) + requested.offset_y;
    const moved = try reposition(guest, 3, requested, origin_x - 7, origin_y + 2);
    try testing.expectEqual(@as(i32, 1000 - 21), moved.x);
    try testing.expectEqual(@as(i32, 500 + 6), moved.y);
    try testing.expectEqual(@as(u32, 40), moved.width);
    try testing.expectEqual(@as(u32, 30), moved.height);
}

test "a clamped request keeps the displacement relative to the requested origin, not the anchor" {
    // Requested origin (1000, 500) is clamped to anchor (99, 59) with offset (901, 441) at scale 1.
    // The compositor places the popup at parent position (50, 10): the displacement is from the origin.
    const guest = make_rect(1000, 500, 4, 4);
    const requested = try request(guest, 1, 100, 60);
    const moved = try reposition(guest, 1, requested, 50, 10);
    try testing.expectEqual(@as(i32, 50), moved.x);
    try testing.expectEqual(@as(i32, 10), moved.y);
}

test "a nested popup is repositioned in its direct parent's coordinates" {
    // Menu 300 by 200 at scale 2; the tooltip is requested at guest (40, 450) and clamped to y = 199.
    const guest = make_rect(40, 450, 40, 40);
    const requested = try request(guest, 2, 300, 200);
    // The compositor moves it to menu-logical (-3, 225): x shifts by -23 * 2, y stays unmoved.
    const moved = try reposition(guest, 2, requested, -3, 225);
    try testing.expectEqual(@as(i32, 40 - 46), moved.x);
    try testing.expectEqual(@as(i32, 450), moved.y);
    try testing.expectEqual(@as(u32, 40), moved.width);
}

test "reposition overflows are refused and never wrap" {
    const guest = make_rect(std.math.maxInt(i32) - 1, 0, 1, 1);
    const requested = try request(make_rect(0, 0, 1, 1), 1, 100, 100);
    try testing.expectError(error.Overflow, reposition(guest, 1, requested, 10, 0));
    try testing.expectError(error.Overflow, reposition(make_rect(0, std.math.minInt(i32), 1, 1), 1, requested, 0, -100));
    const big = make_rect(0, 0, 1, 1);
    try testing.expectError(error.Overflow, reposition(big, 8, requested, std.math.maxInt(i32), 0));
    // A displacement that fits exactly is accepted.
    try testing.expectEqual(@as(i32, 2147483640), (try reposition(big, 8, requested, 268435455, 0)).x);
}

test "reposition matches the independent 128-bit reference for exhaustive origins and seeded displacements" {
    var prng = std.rand.DefaultPrng.init(0xBEEF);
    const random = prng.random();
    const deltas = [_]i64{ 0, 1, -1, 3, -3, 40, -40, 1000, -1000, 40000, -40000, std.math.maxInt(i32), std.math.minInt(i32) };
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        var x: i32 = -300000;
        while (x <= 300000) : (x += 1) {
            const guest = make_rect(x, 300000 - x, 1 + random.uintLessThan(u32, 500), 1 + random.uintLessThan(u32, 500));
            const requested = request(guest, scale, 1 + random.uintLessThan(u32, 32768), 1 + random.uintLessThan(u32, 32768)) catch continue;
            const origin_x = @as(i64, requested.anchor_x) + requested.offset_x;
            const origin_y = @as(i64, requested.anchor_y) + requested.offset_y;
            const delta = deltas[random.uintLessThan(usize, deltas.len)];
            const adjusted_x = origin_x + delta;
            const adjusted_y = origin_y - delta;
            if (adjusted_x < std.math.minInt(i32) or adjusted_x > std.math.maxInt(i32)) continue;
            if (adjusted_y < std.math.minInt(i32) or adjusted_y > std.math.maxInt(i32)) continue;
            const ax: i32 = @intCast(adjusted_x);
            const ay: i32 = @intCast(adjusted_y);
            if (reference_reposition(guest, scale, origin_x, origin_y, adjusted_x, adjusted_y)) |want| {
                try testing.expectEqual(want, try reposition(guest, scale, requested, ax, ay));
            } else {
                try testing.expectError(error.Overflow, reposition(guest, scale, requested, ax, ay));
            }
        }
    }
}

test "C ABI maps every error to its status and writes output only on success" {
    var scale: u32 = 99;
    try testing.expectEqual(status_t.InvalidArgument, abi_shim_t.integer_scale(1, 1, null));
    try testing.expectEqual(status_t.InvalidScale, abi_shim_t.integer_scale(1, 0, &scale));
    try testing.expectEqual(status_t.Ok, abi_shim_t.integer_scale(150, 120, &scale));
    try testing.expectEqual(@as(u32, 2), scale);

    var guest = make_rect(0, 0, 1, 1);
    var result: popup_request_t = undefined;
    result.width = 7;
    try testing.expectEqual(status_t.InvalidArgument, abi_shim_t.request(null, 1, 1, 1, &result));
    try testing.expectEqual(status_t.InvalidArgument, abi_shim_t.request(&guest, 1, 1, 1, null));
    try testing.expectEqual(status_t.InvalidScale, abi_shim_t.request(&guest, 9, 1, 1, &result));
    try testing.expectEqual(status_t.InvalidParent, abi_shim_t.request(&guest, 1, 0, 1, &result));
    try testing.expectEqual(@as(u32, 7), result.width);
    guest = make_rect(40000, 0, 1, 1);
    try testing.expectEqual(status_t.OutOfRange, abi_shim_t.request(&guest, 1, 1, 1, &result));
    try testing.expectEqual(@as(u32, 7), result.width);
    guest = make_rect(-8, 16, 9, 17);
    try testing.expectEqual(status_t.Ok, abi_shim_t.request(&guest, 8, 100, 100, &result));
    try testing.expectEqual(@as(i32, 0), result.anchor_x);
    try testing.expectEqual(@as(i32, -1), result.offset_x);
    try testing.expectEqual(@as(i32, 2), result.anchor_y);

    var guest_out = make_rect(7, 7, 7, 7);
    const requested = result;
    try testing.expectEqual(status_t.InvalidArgument, abi_shim_t.reposition(null, 1, &requested, 0, 0, &guest_out));
    try testing.expectEqual(status_t.InvalidArgument, abi_shim_t.reposition(&guest, 1, null, 0, 0, &guest_out));
    try testing.expectEqual(status_t.InvalidArgument, abi_shim_t.reposition(&guest, 1, &requested, 0, 0, null));
    try testing.expectEqual(status_t.InvalidScale, abi_shim_t.reposition(&guest, 0, &requested, 0, 0, &guest_out));
    try testing.expectEqual(make_rect(7, 7, 7, 7), guest_out);
    const big_request = popup_request_t{ .anchor_x = 0, .anchor_y = 0, .anchor_width = 1, .anchor_height = 1, .offset_x = 0, .offset_y = 0, .width = 1, .height = 1, .anchor = AnchorTopLeft, .gravity = GravityBottomRight, .constraint_adjustment = ConstraintSlideFlip };
    const far = make_rect(std.math.maxInt(i32), 0, 1, 1);
    try testing.expectEqual(status_t.Overflow, abi_shim_t.reposition(&far, 2, &big_request, 1, 0, &guest_out));
    try testing.expectEqual(make_rect(7, 7, 7, 7), guest_out);
    // Scale 8 request for (-8, 16); the compositor places it at (0, 0): x' = -8 + (0 + 1) * 8 = 0, y' = 16 + (0 - 2) * 8 = 0.
    try testing.expectEqual(status_t.Ok, abi_shim_t.reposition(&guest, 8, &requested, 0, 0, &guest_out));
    try testing.expectEqual(make_rect(0, 0, 9, 17), guest_out);
}
