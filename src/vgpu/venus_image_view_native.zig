//! Owned core image-view compatibility metadata. Format classes follow pinned
//! submodules/venus_protocol/xmls/vk.xml; block-texel and multiplanar extensions excluded.
const std = @import("std");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum owned format-list prefix. No borrowed format pointer survives snapshot.
pub const MaxViewFormats: usize = 32;
/// Pointer-free actual image creation metadata. Caller owns this until image retirement.
/// No allocation/shared state; immutable snapshots may be used concurrently.
pub const image_t = struct {
    flags: u32 = 0,
    image_type: u32 = 0,
    format: u32 = 0,
    extent: [3]u32 = .{ 0, 0, 0 },
    levels: u32 = 0,
    layers: u32 = 0,
    samples: u32 = 0,
    usage: u32 = 0,
    view_format_count: usize = 0,
    view_formats: [MaxViewFormats]u32 = [_]u32{0} ** MaxViewFormats,
};
fn format_class(format: u32) !u8 {
    return switch (format) {
        1, 9...15 => 1, // 8-bit
        2...8, 16...22, 70...76 => 2, // 16-bit
        23...36 => 3, // 24-bit
        37...69, 77...83, 98...100, 122...123 => 4, // 32-bit
        84...90 => 5, // 48-bit
        91...97, 101...103, 110...112 => 6, // 64-bit
        104...106 => 7, // 96-bit
        107...109, 113...115 => 8, // 128-bit
        116...118 => 9, // 192-bit
        119...121 => 10, // 256-bit
        124 => 11, // D16
        125 => 12, // D24
        126 => 13, // D32
        127 => 14, // S8
        128 => 15, // D16S8
        129 => 16, // D24S8
        130 => 17, // D32S8
        131...132 => 18, // BC1_RGB
        133...134 => 19, // BC1_RGBA
        135...136 => 20, // BC2
        137...138 => 21, // BC3
        139...140 => 22, // BC4
        141...142 => 23, // BC5
        143...144 => 24, // BC6H
        145...146 => 25, // BC7
        147...148 => 26, // ETC2_RGB
        149...150 => 27, // ETC2_RGBA
        151...152 => 28, // ETC2_EAC_RGBA
        153...154 => 29, // EAC_R
        155...156 => 30, // EAC_RG
        157...158 => 31, // ASTC_4x4
        159...160 => 32, // ASTC_5x4
        161...162 => 33, // ASTC_5x5
        163...164 => 34, // ASTC_6x5
        165...166 => 35, // ASTC_6x6
        167...168 => 36, // ASTC_8x5
        169...170 => 37, // ASTC_8x6
        171...172 => 38, // ASTC_8x8
        173...174 => 39, // ASTC_10x5
        175...176 => 40, // ASTC_10x6
        177...178 => 41, // ASTC_10x8
        179...180 => 42, // ASTC_10x10
        181...182 => 43, // ASTC_12x10
        183...184 => 44, // ASTC_12x12
        else => error.Invalid,
    };
}
/// Core format compatibility according to the pinned XML class, not just byte size.
/// [in] source/target core1..184; returns bool or Invalid for unsupported formats.
/// No pointers, ownership changes, allocations or shared mutable state.
pub fn compatible(source: u32, target: u32) !bool {
    return try format_class(source) == try format_class(target);
}
fn aspects(format: u32) u32 {
    return switch (format) {
        124...126 => 2,
        127 => 4,
        128...130 => 6,
        else => 1,
    };
}
/// Copy actual image geometry/flags and optional format list from borrowed native input.
/// [in] info accessible for call; only a single FormatList chain node supported, count<=32.
/// Core1..184 classes, dimensional/cube shapes and mip counts checked; host support/usages/
/// device limits remain caller-owned. Cube images require square2D and >=6 layers, not6N.
/// Returns owned snapshot or Invalid/Limit; no allocations, partial results or input retention.
pub fn snapshot(info: *const c.VkImageCreateInfo) !image_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO or info.flags & ~@as(u32, c.VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | c.VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0 or info.imageType > 2 or info.extent.width == 0 or info.extent.height == 0 or info.extent.depth == 0 or info.mipLevels == 0 or info.arrayLayers == 0 or info.samples == 0 or info.samples > 64 or info.samples & (info.samples - 1) != 0) return error.Invalid;
    _ = try format_class(info.format);
    const dimension = @max(info.extent.width, @max(info.extent.height, info.extent.depth));
    if (info.mipLevels > 32 - @clz(dimension)) return error.Invalid;
    if ((info.imageType == 0 and (info.extent.height != 1 or info.extent.depth != 1)) or (info.imageType == 1 and info.extent.depth != 1) or (info.imageType == 2 and info.arrayLayers != 1)) return error.Invalid;
    if (info.samples != 1 and (info.imageType != 1 or info.mipLevels != 1)) return error.Invalid;
    if (info.flags & c.VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT != 0 and (info.imageType != 1 or info.extent.width != info.extent.height or info.arrayLayers < 6 or info.samples != 1)) return error.Invalid;
    var result = image_t{ .flags = info.flags, .image_type = info.imageType, .format = info.format, .extent = .{ info.extent.width, info.extent.height, info.extent.depth }, .levels = info.mipLevels, .layers = info.arrayLayers, .samples = info.samples, .usage = info.usage };
    const address = @as(*align(1) const usize, @ptrCast(&info.pNext)).*;
    if (address != 0) {
        if (address % @alignOf(c.VkImageFormatListCreateInfo) != 0) return error.Invalid;
        const list: *const c.VkImageFormatListCreateInfo = @ptrFromInt(address);
        if (list.sType != c.VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO or list.pNext != null) return error.Invalid;
        if (list.viewFormatCount > MaxViewFormats) return error.Limit;
        if (info.flags & c.VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT == 0 and list.viewFormatCount > 1) return error.Invalid;
        const formats_address = @as(*align(1) const usize, @ptrCast(&list.pViewFormats)).*;
        if (list.viewFormatCount != 0 and (formats_address == 0 or formats_address % @alignOf(c.VkFormat) != 0)) return error.Invalid;
        result.view_format_count = list.viewFormatCount;
        if (result.view_format_count != 0) {
            const formats: [*]const c.VkFormat = @ptrFromInt(formats_address);
            for (formats[0..result.view_format_count], 0..) |format, index| {
                if (!try compatible(info.format, format)) return error.Invalid;
                result.view_formats[index] = format;
            }
        }
    }
    return result;
}
/// Snapshot a single optional ImageViewUsage chain into an owned scalar.
/// [in] info borrowed accessible SDK record; recognized aligned node requires nonzero usage
/// and no successor. Returns null when absent, usage value when present, or Invalid.
/// No allocations, caller pointer retention, output mutation, or shared state.
pub fn view_usage(info: *const c.VkImageViewCreateInfo) !?u32 {
    const address = @as(*align(1) const usize, @ptrCast(&info.pNext)).*;
    if (address == 0) return null;
    if (address % @alignOf(c.VkImageViewUsageCreateInfo) != 0) return error.Invalid;
    const node: *const c.VkImageViewUsageCreateInfo = @ptrFromInt(address);
    if (node.sType != c.VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO or node.pNext != null or node.usage == 0) return error.Invalid;
    return node.usage;
}
/// Validate an image view and resolve REMAINING mip/layer counts into actual positive spans.
/// [in] image immutable owned snapshot, info borrowed SDK record; cube_array_enabled reflects
/// this device's admitted feature, not raw host support. Optional view usage must be a nonzero
/// subset of owned image usage. Image token/binding and actual
/// host format/view support remain caller-owned. CUBE=6 layers; CUBE_ARRAY=6N with feature.
/// Mutable views use the same XML class and must appear in any nonempty owned format list.
/// [out] returned range owns scalars only. Invalid leaves callers untouched; no allocation.
pub fn validate(image: *const image_t, info: *const c.VkImageViewCreateInfo, cube_array_enabled: bool) !c.VkImageSubresourceRange {
    if (info.sType != c.VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO or info.flags != 0 or info.viewType > 6 or image.levels == 0 or image.layers == 0 or image.view_format_count > MaxViewFormats) return error.Invalid;
    if (try view_usage(info)) |usage| if (usage & ~image.usage != 0) return error.Invalid;
    if (info.format != image.format and (image.flags & c.VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT == 0 or !try compatible(image.format, info.format))) return error.Invalid;
    _ = try format_class(info.format);
    if (image.view_format_count != 0 and std.mem.indexOfScalar(u32, image.view_formats[0..image.view_format_count], info.format) == null) return error.Invalid;
    var range = info.subresourceRange;
    if (range.aspectMask == 0 or range.aspectMask & ~aspects(info.format) != 0 or range.baseMipLevel >= image.levels or range.baseArrayLayer >= image.layers or range.levelCount == 0 or range.layerCount == 0) return error.Invalid;
    if (range.levelCount == c.VK_REMAINING_MIP_LEVELS) range.levelCount = image.levels - range.baseMipLevel;
    if (range.layerCount == c.VK_REMAINING_ARRAY_LAYERS) range.layerCount = image.layers - range.baseArrayLayer;
    if (range.levelCount > image.levels - range.baseMipLevel or range.layerCount > image.layers - range.baseArrayLayer) return error.Invalid;
    switch (image.image_type) {
        0 => if (info.viewType != 0 and info.viewType != 4) return error.Invalid,
        1 => if (info.viewType != 1 and info.viewType != 5 and info.viewType != 3 and info.viewType != 6) return error.Invalid,
        2 => if (info.viewType != 2 or range.baseArrayLayer != 0) return error.Invalid,
        else => return error.Invalid,
    }
    if ((info.viewType == 0 or info.viewType == 1 or info.viewType == 2) and range.layerCount != 1) return error.Invalid;
    if (image.samples != 1 and info.viewType != 1 and info.viewType != 5) return error.Invalid;
    if (info.viewType == 3 or info.viewType == 6) {
        if (image.flags & c.VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT == 0 or image.extent[0] != image.extent[1] or image.samples != 1) return error.Invalid;
        if (info.viewType == 3 and range.layerCount != 6) return error.Invalid;
        if (info.viewType == 6 and (!cube_array_enabled or range.layerCount % 6 != 0)) return error.Invalid;
    }
    return range;
}
fn image_info() c.VkImageCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = c.VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, .imageType = 1, .format = 37, .extent = .{ .width = 64, .height = 64, .depth = 1 }, .mipLevels = 7, .arrayLayers = 13, .samples = 1 };
}
fn view_info() c.VkImageViewCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = 3, .format = 37, .subresourceRange = .{ .aspectMask = 1, .levelCount = c.VK_REMAINING_MIP_LEVELS, .baseArrayLayer = 1, .layerCount = 6 } };
}
test "cube compatible odd image layers cube six and arrays remaining ranges" {
    var creation = image_info();
    var image = try snapshot(&creation);
    var view = view_info();
    const cube = try validate(&image, &view, false);
    try std.testing.expectEqual(@as(u32, 7), cube.levelCount);
    try std.testing.expectEqual(@as(u32, 6), cube.layerCount);
    view.viewType = 6;
    view.subresourceRange.layerCount = c.VK_REMAINING_ARRAY_LAYERS;
    try std.testing.expectError(error.Invalid, validate(&image, &view, false));
    const arrays = try validate(&image, &view, true);
    try std.testing.expectEqual(@as(u32, 12), arrays.layerCount);
    view.subresourceRange.baseArrayLayer = 0;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    creation.arrayLayers = 7;
    image = try snapshot(&creation);
    view = view_info();
    _ = try validate(&image, &view, false);
    creation.arrayLayers = 5;
    try std.testing.expectError(error.Invalid, snapshot(&creation));
    creation.arrayLayers = 6;
    creation.extent.height = 32;
    try std.testing.expectError(error.Invalid, snapshot(&creation));
}
test "mutable owned list and compatibility classes ignore caller lifetime" {
    var formats = [_]c.VkFormat{ 37, 43, 98 };
    var list = c.VkImageFormatListCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = formats.len, .pViewFormats = &formats };
    var creation = image_info();
    creation.flags |= c.VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    creation.pNext = &list;
    const image = try snapshot(&creation);
    @memset(&formats, 0);
    var view = view_info();
    view.format = 43;
    _ = try validate(&image, &view, false);
    view.format = 98;
    _ = try validate(&image, &view, false);
    view.format = 44;
    try std.testing.expectError(error.Invalid, validate(&image, &view, false));
    creation.pNext = null;
    const unrestricted = try snapshot(&creation);
    _ = try validate(&unrestricted, &view, false);
    view.format = 97;
    try std.testing.expectError(error.Invalid, validate(&unrestricted, &view, false));
    try std.testing.expect(!(try compatible(131, 133)));
    try std.testing.expect(try compatible(131, 132));
    try std.testing.expect(!(try compatible(124, 126)));
    try std.testing.expect(!(try compatible(157, 159)));
}
test "native header quotas malformed pointers aspect and dimensional ranges reject" {
    var creation = image_info();
    const image = try snapshot(&creation);
    var view = view_info();
    view.subresourceRange.baseMipLevel = 7;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    view = view_info();
    view.subresourceRange.aspectMask = 2;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    view = view_info();
    view.subresourceRange.layerCount = 5;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    view = view_info();
    view.viewType = 2;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    var formats = [_]c.VkFormat{37} ** (MaxViewFormats + 1);
    var list = c.VkImageFormatListCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = formats.len, .pViewFormats = &formats };
    creation.flags |= c.VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    creation.pNext = &list;
    try std.testing.expectError(error.Limit, snapshot(&creation));
    list.viewFormatCount = 1;
    list.pViewFormats = null;
    try std.testing.expectError(error.Invalid, snapshot(&creation));
    @as(*align(1) usize, @ptrCast(&list.pViewFormats)).* = 1;
    try std.testing.expectError(error.Invalid, snapshot(&creation));
    list.viewFormatCount = 0;
    list.pViewFormats = null;
    _ = try snapshot(&creation);
    list.pNext = &list;
    try std.testing.expectError(error.Invalid, snapshot(&creation));
    creation.pNext = @ptrFromInt(3);
    try std.testing.expectError(error.Invalid, snapshot(&creation));
    creation.pNext = null;
    creation.mipLevels = 8;
    try std.testing.expectError(error.Invalid, snapshot(&creation));
}

test "image view usage chain owns scalar and requires actual image usage subset" {
    var creation = image_info();
    creation.usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | c.VK_IMAGE_USAGE_SAMPLED_BIT;
    const image = try snapshot(&creation);
    var view = view_info();
    var node: c.VkImageViewUsageCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
    view.pNext = &node;
    const usage = (try view_usage(&view)).?;
    _ = try validate(&image, &view, false);
    node.usage = c.VK_IMAGE_USAGE_STORAGE_BIT;
    try std.testing.expectEqual(@as(u32, c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), usage);
    try std.testing.expectError(error.Invalid, validate(&image, &view, false));
    node.usage = 0;
    try std.testing.expectError(error.Invalid, view_usage(&view));
    node.usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    node.pNext = &node;
    try std.testing.expectError(error.Invalid, view_usage(&view));
    node.pNext = null;
    node.sType = c.VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    try std.testing.expectError(error.Invalid, view_usage(&view));
    view.pNext = @ptrFromInt(3);
    try std.testing.expectError(error.Invalid, view_usage(&view));
    view.pNext = null;
    try std.testing.expectEqual(@as(?u32, null), try view_usage(&view));
}

test "complete core formats and image creation dimensional failure boundaries" {
    for (1..185) |format| try std.testing.expect(try compatible(@intCast(format), @intCast(format)));
    try std.testing.expectError(error.Invalid, compatible(0, 37));
    try std.testing.expectError(error.Invalid, compatible(37, 185));
    const initial = image_info();
    inline for (.{ "sType", "flags", "imageType", "mipLevels", "arrayLayers", "samples", "format" }) |field| {
        var info = initial;
        @field(info, field) = if (comptime std.mem.eql(u8, field, "sType")) 0 else if (comptime std.mem.eql(u8, field, "flags")) 1 else if (comptime std.mem.eql(u8, field, "imageType")) 3 else 0;
        try std.testing.expectError(error.Invalid, snapshot(&info));
    }
    inline for (.{ "width", "height", "depth" }) |field| {
        var info = initial;
        @field(info.extent, field) = 0;
        try std.testing.expectError(error.Invalid, snapshot(&info));
    }
    for ([_]u32{ 3, 128 }) |samples| {
        var info = initial;
        info.samples = samples;
        try std.testing.expectError(error.Invalid, snapshot(&info));
    }
    var info = initial;
    info.flags = 0;
    info.imageType = 0;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info.extent.height = 1;
    info.extent.depth = 2;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info.extent.depth = 1;
    _ = try snapshot(&info);
    info.imageType = 1;
    info.extent.depth = 2;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info.imageType = 2;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info.arrayLayers = 1;
    _ = try snapshot(&info);
    info.samples = 2;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info.imageType = 1;
    info.extent.depth = 1;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info.mipLevels = 1;
    _ = try snapshot(&info);
}

test "view headers remaining ranges dimensions and multisample ownership boundaries" {
    const creation = image_info();
    const initial_image = try snapshot(&creation);
    const initial_view = view_info();
    inline for (.{ "sType", "flags", "viewType" }) |field| {
        var view = initial_view;
        @field(view, field) = if (comptime std.mem.eql(u8, field, "sType")) 0 else if (comptime std.mem.eql(u8, field, "viewType")) 7 else 1;
        try std.testing.expectError(error.Invalid, validate(&initial_image, &view, true));
    }
    inline for (.{ "levels", "layers", "view_format_count" }) |field| {
        var image = initial_image;
        @field(image, field) = if (comptime std.mem.eql(u8, field, "view_format_count")) 33 else 0;
        try std.testing.expectError(error.Invalid, validate(&image, &initial_view, true));
    }
    inline for (.{ "aspectMask", "baseMipLevel", "baseArrayLayer", "levelCount", "layerCount" }) |field| {
        var view = initial_view;
        @field(view.subresourceRange, field) = if (comptime std.mem.startsWith(u8, field, "base")) 99 else 0;
        try std.testing.expectError(error.Invalid, validate(&initial_image, &view, true));
    }
    var view = initial_view;
    view.subresourceRange.levelCount = 8;
    try std.testing.expectError(error.Invalid, validate(&initial_image, &view, true));
    view = initial_view;
    view.subresourceRange.layerCount = 13;
    try std.testing.expectError(error.Invalid, validate(&initial_image, &view, true));
    var image = initial_image;
    image.flags = 0;
    try std.testing.expectError(error.Invalid, validate(&image, &initial_view, true));
    image = initial_image;
    image.extent[1] = 32;
    try std.testing.expectError(error.Invalid, validate(&image, &initial_view, true));
    image = initial_image;
    image.samples = 2;
    try std.testing.expectError(error.Invalid, validate(&image, &initial_view, true));
    view = initial_view;
    view.viewType = 5;
    _ = try validate(&image, &view, true);
    view.viewType = 1;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    view.subresourceRange.layerCount = 1;
    _ = try validate(&image, &view, true);
    image.image_type = 0;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    image.samples = 1;
    view.viewType = 0;
    _ = try validate(&image, &view, true);
    view.viewType = 4;
    _ = try validate(&image, &view, true);
    image.image_type = 2;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    view.viewType = 2;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    view.subresourceRange.baseArrayLayer = 0;
    _ = try validate(&image, &view, true);
    image.image_type = 3;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    image = initial_image;
    image.flags |= c.VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    view = initial_view;
    view.format = 0;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    view.format = 124;
    try std.testing.expectError(error.Invalid, validate(&image, &view, true));
    image.format = 124;
    view.subresourceRange.aspectMask = 2;
    _ = try validate(&image, &view, true);
    image.format = 127;
    view.format = 127;
    view.subresourceRange.aspectMask = 4;
    _ = try validate(&image, &view, true);
}
