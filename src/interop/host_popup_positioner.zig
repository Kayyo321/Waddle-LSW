//! Pure popup positioner geometry for host xdg_popup requests (TODO #11c).
//!
//! `request` converts a guest popup rectangle in physical pixels into the logical positioner
//! values for a parent with integer host scale `s`. `restore` converts a compositor-adjusted logical
//! rectangle back into guest pixels with overflow checks. Both are pure functions over value types:
//! no allocation, no operating-system call, no retained state.
//!
//! The contract is `impl/seamless-interop/claude_45_percent/host_popup_positioner_contract.md`.
const std = @import("std");

/// Maximum accepted integer host scale. Fractional scales are rounded up by `integer_scale`.
pub const MaxScale: u32 = 8;
/// Maximum absolute logical offset or dimension accepted by `request`.
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

/// Result code of the C ABI exports.
pub const status_t = enum(c_int) {
    /// The output was written.
    Ok = 0,
    /// A NULL pointer was passed.
    InvalidArgument = 1,
    /// The scale is outside 1..8, or a fractional scale rounds outside that range.
    InvalidScale = 2,
    /// A logical offset or dimension exceeds MaxLogicalExtent in absolute value.
    OutOfRange = 3,
    /// A scaled coordinate or dimension does not fit its output type.
    Overflow = 4,
};

/// Errors of the pure functions. Each one is mapped to a `status_t` by the C ABI.
pub const positioner_error_t = error{
    InvalidScale,
    OutOfRange,
    Overflow,
};

fn validate_scale(scale: u32) positioner_error_t!void {
    if (scale < 1 or scale > MaxScale) return error.InvalidScale;
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

/// Integer host scale for a fractional scale `numerator / denominator`, rounded up.
///
/// @param numerator Fraction numerator, e.g. 150 for 1.25 when `denominator` is 120.
/// @param denominator Fraction denominator, nonzero.
/// @return `ceil(numerator / denominator)`, in 1..8.
/// @error InvalidScale The denominator is zero, or the rounded result is 0 or above 8.
pub noinline fn integer_scale(numerator: u32, denominator: u32) positioner_error_t!u32 {
    const scale = std.math.divCeil(u64, @as(u64, numerator), @as(u64, denominator)) catch return error.InvalidScale;
    if (scale < 1 or scale > MaxScale) return error.InvalidScale;
    return @intCast(scale);
}

/// Logical popup request for a guest pixel rectangle at integer host scale `scale`.
///
/// The origin rounds toward negative infinity. Each dimension rounds up and is at least 1.
/// Any absolute offset or dimension above MaxLogicalExtent is refused.
///
/// @param rect Guest physical rectangle, relative to the parent's top-left.
/// @param scale Integer host scale, 1..8.
/// @return Logical anchor origin in `x`/`y` and positioner size in `width`/`height`.
/// @error InvalidScale The scale is outside 1..8.
/// @error OutOfRange An offset or a dimension exceeds 32768 in absolute value.
pub noinline fn request(rect: rect_t, scale: u32) positioner_error_t!rect_t {
    try validate_scale(scale);
    const x = floor_div(@as(i64, rect.x), @as(i64, scale));
    const y = floor_div(@as(i64, rect.y), @as(i64, scale));
    const width = @max(1, ceil_div(rect.width, scale));
    const height = @max(1, ceil_div(rect.height, scale));
    if (outside_extent(x) or outside_extent(y) or outside_extent(width) or outside_extent(height)) return error.OutOfRange;
    return .{
        .x = @intCast(x),
        .y = @intCast(y),
        .width = @intCast(width),
        .height = @intCast(height),
    };
}

/// Guest pixel rectangle for a compositor-adjusted logical rectangle at integer host scale `scale`.
///
/// Each field is multiplied by `scale`. The results are range-checked for their own types, so no
/// wrapped value is ever returned.
///
/// @param logical Compositor-adjusted logical rectangle.
/// @param scale Integer host scale, 1..8.
/// @return Guest physical rectangle.
/// @error InvalidScale The scale is outside 1..8.
/// @error Overflow A scaled `x` or `y` does not fit `i32`, or a scaled size does not fit `u32`.
pub noinline fn restore(logical: rect_t, scale: u32) positioner_error_t!rect_t {
    try validate_scale(scale);
    const x = std.math.cast(i32, @as(i64, logical.x) * @as(i64, scale)) orelse return error.Overflow;
    const y = std.math.cast(i32, @as(i64, logical.y) * @as(i64, scale)) orelse return error.Overflow;
    const width = std.math.cast(u32, @as(u64, logical.width) * @as(u64, scale)) orelse return error.Overflow;
    const height = std.math.cast(u32, @as(u64, logical.height) * @as(u64, scale)) orelse return error.Overflow;
    return .{ .x = x, .y = y, .width = width, .height = height };
}

// Test-only fixtures.
const testing = std.testing;

fn make_rect(x: i32, y: i32, width: u32, height: u32) rect_t {
    return .{ .x = x, .y = y, .width = width, .height = height };
}

/// Independent floor division: the test computes the expected value without `@divFloor`.
fn expected_floor(value: i64, scale: i64) i64 {
    if (value >= 0) return @divTrunc(value, scale);
    return -@divTrunc(-value + scale - 1, scale);
}

test "integer scale rounds fractions up and rejects zero, empty and out-of-range results" {
    try testing.expectEqual(@as(u32, 1), try integer_scale(1, 1));
    try testing.expectEqual(@as(u32, 1), try integer_scale(120, 120));
    try testing.expectEqual(@as(u32, 1), try integer_scale(60, 120));
    try testing.expectEqual(@as(u32, 2), try integer_scale(150, 120));
    try testing.expectEqual(@as(u32, 2), try integer_scale(240, 120));
    try testing.expectEqual(@as(u32, 3), try integer_scale(241, 120));
    try testing.expectEqual(@as(u32, 8), try integer_scale(960, 120));
    try testing.expectError(error.InvalidScale, integer_scale(961, 120));
    try testing.expectError(error.InvalidScale, integer_scale(0, 120));
    try testing.expectError(error.InvalidScale, integer_scale(1, 0));
    try testing.expectError(error.InvalidScale, integer_scale(0, 0));
    try testing.expectError(error.InvalidScale, integer_scale(std.math.maxInt(u32), 1));
}

test "every scale from 1 to 8 converts guest origins with floor rounding including negative values" {
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        var value: i32 = -17;
        while (value <= 17) : (value += 1) {
            const logical = try request(make_rect(value, -value, 1, 1), scale);
            try testing.expectEqual(@as(i32, @intCast(expected_floor(value, scale))), logical.x);
            try testing.expectEqual(@as(i32, @intCast(expected_floor(-value, scale))), logical.y);
        }
    }
}

test "dimensions round up and never fall below one logical unit" {
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        var width: u32 = 0;
        while (width <= 20) : (width += 1) {
            const logical = try request(make_rect(0, 0, width, width), scale);
            const expected = @max(1, (@as(u64, width) + scale - 1) / scale);
            try testing.expectEqual(@as(u32, @intCast(expected)), logical.width);
            try testing.expectEqual(@as(u32, @intCast(expected)), logical.height);
        }
    }
    try testing.expectEqual(@as(u32, 1), (try request(make_rect(0, 0, 0, 0), 8)).width);
    try testing.expectEqual(@as(u32, 3), (try request(make_rect(0, 0, 5, 5), 2)).width);
    try testing.expectEqual(@as(u32, 2), (try request(make_rect(0, 0, 4, 4), 2)).height);
}

test "negative origins at each scale round toward negative infinity" {
    const cases = [_]struct { scale: u32, x: i32, expected: i32 }{
        .{ .scale = 1, .x = -1, .expected = -1 },
        .{ .scale = 2, .x = -1, .expected = -1 },
        .{ .scale = 2, .x = -2, .expected = -1 },
        .{ .scale = 2, .x = -3, .expected = -2 },
        .{ .scale = 3, .x = -3, .expected = -1 },
        .{ .scale = 3, .x = -4, .expected = -2 },
        .{ .scale = 8, .x = -8, .expected = -1 },
        .{ .scale = 8, .x = -9, .expected = -2 },
        .{ .scale = 8, .x = -7, .expected = -1 },
    };
    for (cases) |case| {
        const logical = try request(make_rect(case.x, case.x, 1, 1), case.scale);
        try testing.expectEqual(case.expected, logical.x);
        try testing.expectEqual(case.expected, logical.y);
    }
}

test "offset and dimension boundaries at 32768 are accepted and 32769 is refused" {
    try testing.expectEqual(@as(i32, 32768), (try request(make_rect(32768, 0, 1, 1), 1)).x);
    try testing.expectEqual(@as(i32, -32768), (try request(make_rect(-32768, 0, 1, 1), 1)).x);
    try testing.expectError(error.OutOfRange, request(make_rect(32769, 0, 1, 1), 1));
    try testing.expectError(error.OutOfRange, request(make_rect(-32769, 0, 1, 1), 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 32769, 1, 1), 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, -32769, 1, 1), 1));
    try testing.expectEqual(@as(u32, 32768), (try request(make_rect(0, 0, 32768, 32768), 1)).width);
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 32769, 1), 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 1, 32769), 1));
    // At scale 2 the boundary moves to 65536 physical pixels; ceil(65537 / 2) is 32769.
    try testing.expectEqual(@as(u32, 32768), (try request(make_rect(0, 0, 65536, 1), 2)).width);
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 65537, 1), 2));
    // floor(-65536 / 2) is -32768 (accepted); floor(-65537 / 2) is -32769 (refused).
    try testing.expectEqual(@as(i32, -32768), (try request(make_rect(-65536, 0, 1, 1), 2)).x);
    try testing.expectError(error.OutOfRange, request(make_rect(-65537, 0, 1, 1), 2));
}

test "extreme guest values are refused as out of range without overflow" {
    try testing.expectError(error.OutOfRange, request(make_rect(std.math.maxInt(i32), 0, 1, 1), 1));
    try testing.expectError(error.OutOfRange, request(make_rect(std.math.minInt(i32), 0, 1, 1), 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, std.math.maxInt(u32), 1), 1));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, std.math.maxInt(u32), 1), 8));
    try testing.expectError(error.OutOfRange, request(make_rect(0, 0, 1, std.math.maxInt(u32)), 2));
    try testing.expectError(error.OutOfRange, request(make_rect(std.math.minInt(i32), 0, 1, 1), 8));
}

test "invalid scales are refused by both directions" {
    try testing.expectError(error.InvalidScale, request(make_rect(0, 0, 1, 1), 0));
    try testing.expectError(error.InvalidScale, request(make_rect(0, 0, 1, 1), 9));
    try testing.expectError(error.InvalidScale, restore(make_rect(0, 0, 1, 1), 0));
    try testing.expectError(error.InvalidScale, restore(make_rect(0, 0, 1, 1), 9));
}

test "restore multiplies every field at scales 1 to 8 and keeps zero at zero" {
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        const guest = try restore(make_rect(-5, 7, 3, 9), scale);
        try testing.expectEqual(@as(i32, -5 * @as(i32, @intCast(scale))), guest.x);
        try testing.expectEqual(@as(i32, 7 * @as(i32, @intCast(scale))), guest.y);
        try testing.expectEqual(@as(u32, 3 * scale), guest.width);
        try testing.expectEqual(@as(u32, 9 * scale), guest.height);
    }
    const zero = try restore(make_rect(0, 0, 0, 0), 8);
    try testing.expectEqual(make_rect(0, 0, 0, 0), zero);
}

test "restore refuses x and y products outside i32 and sizes outside u32" {
    try testing.expectEqual(@as(i32, 2147483640), (try restore(make_rect(268435455, 0, 0, 0), 8)).x);
    try testing.expectError(error.Overflow, restore(make_rect(268435456, 0, 0, 0), 8));
    try testing.expectError(error.Overflow, restore(make_rect(std.math.maxInt(i32), 0, 0, 0), 2));
    try testing.expectError(error.Overflow, restore(make_rect(std.math.minInt(i32), 0, 0, 0), 2));
    try testing.expectError(error.Overflow, restore(make_rect(0, std.math.maxInt(i32), 0, 0), 2));
    try testing.expectError(error.Overflow, restore(make_rect(0, std.math.minInt(i32), 0, 0), 2));
    try testing.expectEqual(@as(u32, 4294967294), (try restore(make_rect(0, 0, 2147483647, 0), 2)).width);
    try testing.expectError(error.Overflow, restore(make_rect(0, 0, 2147483648, 0), 2));
    try testing.expectError(error.Overflow, restore(make_rect(0, 0, 0, std.math.maxInt(u32)), 2));
    try testing.expectError(error.Overflow, restore(make_rect(0, 0, 0, 536870912), 8));
}

test "restore of a request at the same scale returns the floor-rounded origin" {
    var scale: u32 = 1;
    while (scale <= MaxScale) : (scale += 1) {
        var value: i32 = -40;
        while (value <= 40) : (value += 1) {
            const logical = try request(make_rect(value, value, 1, 1), scale);
            const guest = try restore(logical, scale);
            try testing.expectEqual(@as(i32, @intCast(scale)) * @as(i32, @intCast(expected_floor(value, scale))), guest.x);
            try testing.expectEqual(@as(i32, @intCast(scale)) * @as(i32, @intCast(expected_floor(value, scale))), guest.y);
            try testing.expectEqual(@as(u32, scale), guest.width);
        }
    }
}
