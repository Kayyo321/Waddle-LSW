//! Guest Win32 compatibility presentation. Images remain real host Vulkan objects;
//! synchronous readback and the native sink complete before an acquired image is released.
const std = @import("std");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum simultaneously live native surfaces; owned by one serialized ICD state.
pub const MaxSurfaces = 16;
/// Maximum simultaneously live swapchains, including retired old swapchains.
pub const MaxSwapchains = 16;
/// Per-swapchain owned host-image ceiling.
pub const MaxImages = 3;
/// Synchronous borrowed backend. All callbacks execute with the caller's ICD lock held.
/// create transfers image/memory ownership only on success. destroy retires both owners on acknowledged host teardown. After transport loss,
/// destroy transfers both owners to the ICD pending-abandon ledger until receiver retirement. acquire signals real host sync.
/// readback consumes real wait semaphores and waits for GPU completion before writing pixels.
/// No callback may retain pointers, reenter the ICD, or free caller pixel storage.
pub const backend_t = struct {
    context: ?*anyopaque,
    create: *const fn (?*anyopaque, u32, u32, u32, u32, *u64, *u64) callconv(.C) c_int,
    destroy: *const fn (?*anyopaque, u64, u64) callconv(.C) void,
    acquire: *const fn (?*anyopaque, u64, u64) callconv(.C) c_int,
    readback: *const fn (?*anyopaque, u64, u32, u32, u32, [*]u8, usize, u32, [*c]const c.VkSemaphore) callconv(.C) c_int,
};
const surface_t = struct { id: u64 = 0, instance: u64 = 0, hwnd: usize = 0 };
const image_t = struct { image: u64 = 0, memory: u64 = 0, acquired: bool = false };
const swapchain_t = struct {
    id: u64 = 0,
    device: u64 = 0,
    surface: u64 = 0,
    width: u32 = 0,
    height: u32 = 0,
    format: u32 = 0,
    present_mode: u32 = c.VK_PRESENT_MODE_FIFO_KHR,
    count: u32 = 0,
    next: u32 = 0,
    retired: bool = false,
    images: [MaxImages]image_t = [_]image_t{.{}} ** MaxImages,
};
/// Caller-owned fixed registry, initialized by .{} and exclusively synchronized by ICD lock.
/// Host image ownership belongs to live swapchain slots until destroy_swapchain/destroy_device.
/// Monotonic handles never alias destroyed objects. No heap allocation survives a call.
pub const state_t = struct {
    next_id: u64 = 1,
    surfaces: [MaxSurfaces]surface_t = [_]surface_t{.{}} ** MaxSurfaces,
    swapchains: [MaxSwapchains]swapchain_t = [_]swapchain_t{.{}} ** MaxSwapchains,
};
extern fn venus_win32_present_extent(usize, *u32, *u32) c_int;
extern fn venus_win32_present_pixels(usize, u32, u32, [*]const u8, usize, u32) c_int;
fn surface(state: *state_t, id: u64) ?*surface_t {
    if (id == 0) return null;
    for (&state.surfaces) |*slot| if (slot.id == id) return slot;
    return null;
}
fn swapchain(state: *state_t, device: u64, id: u64) ?*swapchain_t {
    if (device == 0 or id == 0) return null;
    for (&state.swapchains) |*slot| if (slot.id == id and slot.device == device) return slot;
    return null;
}
fn reserve_id(state: *state_t) ?u64 {
    if (state.next_id == std.math.maxInt(u64)) return null;
    const id = state.next_id;
    state.next_id += 1;
    return id;
}
/// [in,out] state exclusive; instance nonzero owner, hwnd borrowed live HWND.
/// Returns VkResult and writes output only on success; output nonnull exclusive.
/// Native window lifetime remains caller-owned through surface destruction.
pub fn create_surface(state: *state_t, instance: u64, hwnd: usize, output: *u64) c_int {
    var width: u32 = 0;
    var height: u32 = 0;
    if (instance == 0 or hwnd == 0 or venus_win32_present_extent(hwnd, &width, &height) == 0) return c.VK_ERROR_SURFACE_LOST_KHR;
    for (&state.surfaces) |*slot| if (slot.id == 0) {
        const id = reserve_id(state) orelse return c.VK_ERROR_TOO_MANY_OBJECTS;
        slot.* = .{ .id = id, .instance = instance, .hwnd = hwnd };
        output.* = id;
        return c.VK_SUCCESS;
    };
    return c.VK_ERROR_OUT_OF_HOST_MEMORY;
}
/// Retire a matching surface only when no swapchain retains it. No allocation.
/// Returns false for stale/wrong-owner/in-use objects, preserving all owners.
pub fn destroy_surface(state: *state_t, instance: u64, id: u64) bool {
    const slot = surface(state, id) orelse return false;
    if (slot.instance != instance) return false;
    for (state.swapchains) |chain| if (chain.id != 0 and chain.surface == id) return false;
    slot.* = .{};
    return true;
}
/// Publish real current HWND extent and compatibility-path capabilities.
/// Borrowed exclusive output; stale/lost surfaces leave it unchanged. Caller holds lock.
pub fn capabilities(state: *state_t, id: u64, output: *c.VkSurfaceCapabilitiesKHR) c_int {
    const slot = surface(state, id) orelse return c.VK_ERROR_SURFACE_LOST_KHR;
    var width: u32 = 0;
    var height: u32 = 0;
    if (venus_win32_present_extent(slot.hwnd, &width, &height) == 0) return c.VK_ERROR_SURFACE_LOST_KHR;
    output.* = .{ .minImageCount = 2, .maxImageCount = MaxImages, .currentExtent = .{ .width = width, .height = height }, .minImageExtent = .{ .width = 1, .height = 1 }, .maxImageExtent = .{ .width = 16384, .height = 16384 }, .maxImageArrayLayers = 1, .supportedTransforms = c.VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .currentTransform = c.VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .supportedCompositeAlpha = c.VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .supportedUsageFlags = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT | c.VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    return c.VK_SUCCESS;
}
/// Validate a live surface owned by instance. Borrowed state; no mutation or allocation.
pub fn supports_surface(state: *state_t, instance: u64, id: u64) bool {
    const slot = surface(state, id) orelse return false;
    if (slot.instance != instance) return false;
    var width: u32 = 0;
    var height: u32 = 0;
    return venus_win32_present_extent(slot.hwnd, &width, &height) != 0;
}
/// Enumerate supported four-channel byte formats, count/fill with borrowed output.
/// Caller synchronizes state and guarantees output capacity from input count.
/// No ownership transfer; lost surface leaves outputs unchanged.
pub fn formats(state: *state_t, id: u64, count: *u32, output: ?[*]c.VkSurfaceFormatKHR) c_int {
    const slot = surface(state, id) orelse return c.VK_ERROR_SURFACE_LOST_KHR;
    if (!supports_surface(state, slot.instance, id)) return c.VK_ERROR_SURFACE_LOST_KHR;
    const values = [_]c.VkFormat{ c.VK_FORMAT_B8G8R8A8_UNORM, c.VK_FORMAT_B8G8R8A8_SRGB, c.VK_FORMAT_R8G8B8A8_UNORM, c.VK_FORMAT_R8G8B8A8_SRGB };
    if (output) |target| {
        const amount = @min(count.*, values.len);
        for (values[0..amount], 0..) |value, index| target[index] = .{ .format = value, .colorSpace = c.VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        count.* = @intCast(amount);
        return if (amount < values.len) c.VK_INCOMPLETE else c.VK_SUCCESS;
    }
    count.* = values.len;
    return c.VK_SUCCESS;
}
/// Enumerate synchronous immediate and compositor-paced FIFO presentation modes.
/// count/fill uses borrowed exclusive output and no ownership transfer; caller holds lock.
pub fn modes(state: *state_t, id: u64, count: *u32, output: ?[*]c.VkPresentModeKHR) c_int {
    const slot = surface(state, id) orelse return c.VK_ERROR_SURFACE_LOST_KHR;
    if (!supports_surface(state, slot.instance, id)) return c.VK_ERROR_SURFACE_LOST_KHR;
    const values = [_]c.VkPresentModeKHR{ c.VK_PRESENT_MODE_FIFO_KHR, c.VK_PRESENT_MODE_IMMEDIATE_KHR };
    if (output) |target| {
        const amount = @min(count.*, values.len);
        @memcpy(target[0..amount], values[0..amount]);
        count.* = @intCast(amount);
        return if (amount < values.len) c.VK_INCOMPLETE else c.VK_SUCCESS;
    }
    count.* = values.len;
    return c.VK_SUCCESS;
}
/// Identify an owned swapchain image so ICD can translate PRESENT_SRC_KHR to GENERAL.
/// The device and image IDs are borrowed nonzero identities; no mutation or allocation.
pub fn is_present_image(state: *const state_t, device: u64, image: u64) bool {
    if (device == 0 or image == 0) return false;
    for (state.swapchains) |chain| {
        if (chain.id == 0 or chain.device != device) continue;
        for (chain.images[0..chain.count]) |owned| if (owned.image == image) return true;
    }
    return false;
}
/// Create 2..3 real backend images transactionally. info borrowed canonical SDK record;
/// backend/context remain borrowed during call. output changes only on success.
/// On failure every successfully created image is deterministically destroyed.
/// Successful oldSwapchain retirement happens after all new resources exist.
pub fn create_swapchain(state: *state_t, backend: *const backend_t, device: u64, info: *const c.VkSwapchainCreateInfoKHR, output: *u64) c_int {
    if (device == 0 or info.sType != c.VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR or info.pNext != null or info.flags != 0 or info.surface == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const surface_id = @intFromPtr(info.surface.?);
    var caps: c.VkSurfaceCapabilitiesKHR = undefined;
    const result = capabilities(state, surface_id, &caps);
    if (result != c.VK_SUCCESS) return result;
    if (info.minImageCount < 2 or info.minImageCount > MaxImages or info.imageArrayLayers != 1 or
        (info.imageFormat != c.VK_FORMAT_B8G8R8A8_UNORM and info.imageFormat != c.VK_FORMAT_B8G8R8A8_SRGB and info.imageFormat != c.VK_FORMAT_R8G8B8A8_UNORM and info.imageFormat != c.VK_FORMAT_R8G8B8A8_SRGB) or
        info.imageColorSpace != c.VK_COLOR_SPACE_SRGB_NONLINEAR_KHR or info.imageExtent.width == 0 or info.imageExtent.height == 0 or
        info.imageExtent.width > 16384 or info.imageExtent.height > 16384 or info.imageUsage == 0 or info.imageUsage & ~caps.supportedUsageFlags != 0 or
        info.imageSharingMode != c.VK_SHARING_MODE_EXCLUSIVE or info.preTransform != c.VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR or
        info.compositeAlpha != c.VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR or
        (info.presentMode != c.VK_PRESENT_MODE_FIFO_KHR and info.presentMode != c.VK_PRESENT_MODE_IMMEDIATE_KHR)) return c.VK_ERROR_INITIALIZATION_FAILED;
    const old = if (info.oldSwapchain) |handle| swapchain(state, device, @intFromPtr(handle)) orelse return c.VK_ERROR_INITIALIZATION_FAILED else null;
    if (old) |previous| if (previous.surface != surface_id) return c.VK_ERROR_INITIALIZATION_FAILED;
    for (&state.swapchains) |*slot| if (slot.id == 0) {
        const id = reserve_id(state) orelse return c.VK_ERROR_TOO_MANY_OBJECTS;
        var pending = swapchain_t{ .id = id, .device = device, .surface = surface_id, .width = info.imageExtent.width, .height = info.imageExtent.height, .format = info.imageFormat, .present_mode = info.presentMode, .count = info.minImageCount };
        var created: usize = 0;
        while (created < pending.count) : (created += 1) {
            const image = &pending.images[created];
            const status = backend.create(backend.context, pending.width, pending.height, pending.format, info.imageUsage | c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &image.image, &image.memory);
            if (status != c.VK_SUCCESS) {
                for (pending.images[0..created]) |owned| backend.destroy(backend.context, owned.image, owned.memory);
                return status;
            }
        }
        slot.* = pending;
        if (old) |previous| previous.retired = true;
        output.* = id;
        return c.VK_SUCCESS;
    };
    return c.VK_ERROR_OUT_OF_HOST_MEMORY;
}
/// Release swapchain references exactly once; backend either destroys acknowledged host
/// resources or retains their ownership in its pending-abandon ledger. Stale IDs ignored.
pub fn destroy_swapchain(state: *state_t, backend: *const backend_t, device: u64, id: u64) void {
    const slot = swapchain(state, device, id) orelse return;
    for (slot.images[0..slot.count]) |owned| backend.destroy(backend.context, owned.image, owned.memory);
    slot.* = .{};
}
/// Destroy all swapchains owned by device before backend device teardown. Caller holds lock.
pub fn destroy_device(state: *state_t, backend: *const backend_t, device: u64) void {
    for (&state.swapchains) |*slot| if (slot.id != 0 and slot.device == device) destroy_swapchain(state, backend, device, slot.id);
}
/// Enumerate real backend image IDs; nullable output follows Vulkan count/fill semantics.
/// count nonnull exclusive; output capacity equals input count. No ownership transfer.
pub fn get_images(state: *state_t, device: u64, id: u64, count: *u32, output: ?[*]u64) c_int {
    const slot = swapchain(state, device, id) orelse return c.VK_ERROR_OUT_OF_DATE_KHR;
    if (output) |values| {
        const amount = @min(count.*, slot.count);
        for (slot.images[0..amount], 0..) |image, index| values[index] = image.image;
        count.* = amount;
        return if (amount < slot.count) c.VK_INCOMPLETE else c.VK_SUCCESS;
    }
    count.* = slot.count;
    return c.VK_SUCCESS;
}
/// Acquire an available image, signaling real host semaphore/fence via backend.
/// output changes only after successful signaling; failed signaling preserves availability.
/// No available image returns NOT_READY for timeout0 and TIMEOUT otherwise.
pub fn acquire_next(state: *state_t, backend: *const backend_t, device: u64, id: u64, timeout: u64, semaphore: u64, fence: u64, output: *u32) c_int {
    const slot = swapchain(state, device, id) orelse return c.VK_ERROR_OUT_OF_DATE_KHR;
    if (slot.retired) return c.VK_ERROR_OUT_OF_DATE_KHR;
    if (semaphore == 0 and fence == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    var offset: u32 = 0;
    while (offset < slot.count) : (offset += 1) {
        const index = (slot.next + offset) % slot.count;
        if (!slot.images[index].acquired) {
            const status = backend.acquire(backend.context, semaphore, fence);
            if (status != c.VK_SUCCESS) return status;
            slot.images[index].acquired = true;
            slot.next = (index + 1) % slot.count;
            output.* = index;
            return c.VK_SUCCESS;
        }
    }
    return if (timeout == 0) c.VK_NOT_READY else c.VK_TIMEOUT;
}
/// Synchronous real GPU readback then native HWND presentation. waits is borrowed,
/// consumed once by backend; caller handles multi-swapchain wait distribution.
/// Pixel storage owned temporarily by supplied allocator, always freed on every exit.
/// Acquired image returns to availability only after successful sink completion.
pub fn present(state: *state_t, backend: *const backend_t, allocator: std.mem.Allocator, device: u64, id: u64, index: u32, waits: []const c.VkSemaphore) c_int {
    const slot = swapchain(state, device, id) orelse return c.VK_ERROR_OUT_OF_DATE_KHR;
    if (index >= slot.count or !slot.images[index].acquired or waits.len > 64) return c.VK_ERROR_INITIALIZATION_FAILED;
    const target = surface(state, slot.surface) orelse return c.VK_ERROR_SURFACE_LOST_KHR;
    var width: u32 = 0;
    var height: u32 = 0;
    if (venus_win32_present_extent(target.hwnd, &width, &height) == 0) return c.VK_ERROR_SURFACE_LOST_KHR;
    if (width != slot.width or height != slot.height) return c.VK_ERROR_OUT_OF_DATE_KHR;
    const size = @as(usize, slot.width) * slot.height * 4;
    const pixels = allocator.alloc(u8, size) catch return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    defer allocator.free(pixels);
    const result = backend.readback(backend.context, slot.images[index].image, slot.width, slot.height, slot.format, pixels.ptr, pixels.len, @intCast(waits.len), if (waits.len == 0) null else waits.ptr);
    if (result != c.VK_SUCCESS) return result;
    if (slot.format == c.VK_FORMAT_R8G8B8A8_UNORM or slot.format == c.VK_FORMAT_R8G8B8A8_SRGB) {
        var pixel: usize = 0;
        while (pixel < pixels.len) : (pixel += 4) std.mem.swap(u8, &pixels[pixel], &pixels[pixel + 2]);
    }
    if (venus_win32_present_pixels(target.hwnd, slot.width, slot.height, pixels.ptr, pixels.len, slot.present_mode) == 0) return c.VK_ERROR_SURFACE_LOST_KHR;
    slot.images[index].acquired = false;
    return c.VK_SUCCESS;
}

// Independent native sink/backend fixtures, emitted only by Zig tests.
const fixture_t = struct { created: u32 = 0, destroyed: u32 = 0, fail_at: u32 = 0, signal_result: c_int = 0, read_result: c_int = 0, consumed_waits: u32 = 0 };
fn test_create(context: ?*anyopaque, _: u32, _: u32, _: u32, _: u32, image: *u64, memory: *u64) callconv(.C) c_int {
    const fixture: *fixture_t = @ptrCast(@alignCast(context.?));
    fixture.created += 1;
    if (fixture.fail_at == fixture.created) return c.VK_ERROR_OUT_OF_DEVICE_MEMORY;
    image.* = fixture.created + 100;
    memory.* = fixture.created + 200;
    return c.VK_SUCCESS;
}
fn test_destroy(context: ?*anyopaque, _: u64, _: u64) callconv(.C) void {
    const fixture: *fixture_t = @ptrCast(@alignCast(context.?));
    fixture.destroyed += 1;
}
fn test_acquire(context: ?*anyopaque, _: u64, _: u64) callconv(.C) c_int {
    const fixture: *fixture_t = @ptrCast(@alignCast(context.?));
    return fixture.signal_result;
}
fn test_read(context: ?*anyopaque, _: u64, width: u32, height: u32, _: u32, pixels: [*]u8, size: usize, count: u32, _: [*c]const c.VkSemaphore) callconv(.C) c_int {
    const fixture: *fixture_t = @ptrCast(@alignCast(context.?));
    fixture.consumed_waits += count;
    if (fixture.read_result != 0) return fixture.read_result;
    std.debug.assert(size == @as(usize, width) * height * 4);
    for (pixels[0..size], 0..) |*value, index| value.* = @intCast(index % 4);
    return c.VK_SUCCESS;
}
fn test_backend(fixture: *fixture_t) backend_t {
    return .{ .context = fixture, .create = test_create, .destroy = test_destroy, .acquire = test_acquire, .readback = test_read };
}
fn test_info(id: u64) c.VkSwapchainCreateInfoKHR {
    return .{ .sType = c.VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = @ptrFromInt(id), .minImageCount = 2, .imageFormat = c.VK_FORMAT_R8G8B8A8_UNORM, .imageColorSpace = c.VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = .{ .width = 64, .height = 64 }, .imageArrayLayers = 1, .imageUsage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode = c.VK_SHARING_MODE_EXCLUSIVE, .preTransform = c.VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = c.VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = c.VK_PRESENT_MODE_FIFO_KHR };
}
fn test_extent(hwnd: usize, width: *u32, height: *u32) callconv(.C) c_int {
    if (hwnd != 1) return 0;
    width.* = 64;
    height.* = 64;
    return 1;
}
fn test_pixels(hwnd: usize, width: u32, height: u32, pixels: [*]const u8, size: usize, _: u32) callconv(.C) c_int {
    std.debug.assert(hwnd == 1 and width == 64 and height == 64 and size == 16384);
    std.debug.assert(pixels[0] == 2 and pixels[1] == 1 and pixels[2] == 0 and pixels[3] == 3);
    return 1;
}
comptime {
    if (@import("builtin").is_test) {
        @export(test_extent, .{ .name = "venus_win32_present_extent" });
        @export(test_pixels, .{ .name = "venus_win32_present_pixels" });
    }
}
test "swapchain actual image owners acquire present retire and teardown" {
    var state: state_t = .{};
    var fixture: fixture_t = .{};
    const backend = test_backend(&fixture);
    var surface_id: u64 = 0;
    var chain_id: u64 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, create_surface(&state, 7, 1, &surface_id));
    var caps: c.VkSurfaceCapabilitiesKHR = undefined;
    try std.testing.expectEqual(c.VK_SUCCESS, capabilities(&state, surface_id, &caps));
    try std.testing.expectEqual(@as(u32, 64), caps.currentExtent.width);
    try std.testing.expect(supports_surface(&state, 7, surface_id));
    var format_count: u32 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, formats(&state, surface_id, &format_count, null));
    try std.testing.expectEqual(@as(u32, 4), format_count);
    var format_values: [4]c.VkSurfaceFormatKHR = undefined;
    try std.testing.expectEqual(c.VK_SUCCESS, formats(&state, surface_id, &format_count, &format_values));
    var mode_count: u32 = 2;
    var mode_values: [2]c.VkPresentModeKHR = undefined;
    try std.testing.expectEqual(c.VK_SUCCESS, modes(&state, surface_id, &mode_count, &mode_values));
    var info = test_info(surface_id);
    try std.testing.expectEqual(c.VK_SUCCESS, create_swapchain(&state, &backend, 9, &info, &chain_id));
    try std.testing.expect(!destroy_surface(&state, 7, surface_id));
    var count: u32 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, get_images(&state, 9, chain_id, &count, null));
    try std.testing.expectEqual(@as(u32, 2), count);
    var images: [3]u64 = undefined;
    count = 1;
    try std.testing.expectEqual(c.VK_INCOMPLETE, get_images(&state, 9, chain_id, &count, &images));
    try std.testing.expectEqual(@as(u64, 101), images[0]);
    try std.testing.expect(is_present_image(&state, 9, images[0]));
    var index: u32 = 99;
    try std.testing.expectEqual(c.VK_SUCCESS, acquire_next(&state, &backend, 9, chain_id, 0, 12, 0, &index));
    try std.testing.expectEqual(@as(u32, 0), index);
    const waits = [_]c.VkSemaphore{@ptrFromInt(12)};
    try std.testing.expectEqual(c.VK_SUCCESS, present(&state, &backend, std.testing.allocator, 9, chain_id, index, &waits));
    try std.testing.expectEqual(@as(u32, 1), fixture.consumed_waits);
    info.oldSwapchain = @ptrFromInt(chain_id);
    var new_id: u64 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, create_swapchain(&state, &backend, 9, &info, &new_id));
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_DATE_KHR, acquire_next(&state, &backend, 9, chain_id, 0, 12, 0, &index));
    destroy_swapchain(&state, &backend, 9, chain_id);
    destroy_device(&state, &backend, 9);
    try std.testing.expectEqual(fixture.created, fixture.destroyed);
    try std.testing.expect(destroy_surface(&state, 7, surface_id));
    try std.testing.expect(!destroy_surface(&state, 7, surface_id));
}
test "transactional create failure and signal readback failure preserve ownership" {
    var state: state_t = .{};
    var fixture = fixture_t{ .fail_at = 2 };
    const backend = test_backend(&fixture);
    var surface_id: u64 = 0;
    var chain_id: u64 = 88;
    try std.testing.expectEqual(c.VK_SUCCESS, create_surface(&state, 7, 1, &surface_id));
    var info = test_info(surface_id);
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_DEVICE_MEMORY, create_swapchain(&state, &backend, 9, &info, &chain_id));
    try std.testing.expectEqual(@as(u64, 88), chain_id);
    try std.testing.expectEqual(@as(u32, 1), fixture.destroyed);
    fixture.fail_at = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, create_swapchain(&state, &backend, 9, &info, &chain_id));
    var index: u32 = 77;
    fixture.signal_result = c.VK_ERROR_DEVICE_LOST;
    try std.testing.expectEqual(c.VK_ERROR_DEVICE_LOST, acquire_next(&state, &backend, 9, chain_id, 0, 12, 0, &index));
    try std.testing.expectEqual(@as(u32, 77), index);
    fixture.signal_result = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, acquire_next(&state, &backend, 9, chain_id, 0, 12, 0, &index));
    fixture.read_result = c.VK_ERROR_DEVICE_LOST;
    try std.testing.expectEqual(c.VK_ERROR_DEVICE_LOST, present(&state, &backend, std.testing.allocator, 9, chain_id, index, &.{}));
    try std.testing.expect(swapchain(&state, 9, chain_id).?.images[index].acquired);
    var second: u32 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, acquire_next(&state, &backend, 9, chain_id, 0, 12, 0, &second));
    try std.testing.expectEqual(c.VK_NOT_READY, acquire_next(&state, &backend, 9, chain_id, 0, 12, 0, &second));
    try std.testing.expectEqual(c.VK_TIMEOUT, acquire_next(&state, &backend, 9, chain_id, 1, 12, 0, &second));
    destroy_device(&state, &backend, 9);
    try std.testing.expectEqual(fixture.created - 1, fixture.destroyed);
    try std.testing.expect(destroy_surface(&state, 7, surface_id));
}

test "surface namespace exhaustion stale owners incomplete enumeration and invalid swapchain parameters" {
    var state: state_t = .{};
    var fixture: fixture_t = .{};
    const backend = test_backend(&fixture);
    var id: u64 = 99;
    for ([_][2]u64{ .{ 0, 1 }, .{ 7, 0 }, .{ 7, 2 } }) |invalid| try std.testing.expectEqual(c.VK_ERROR_SURFACE_LOST_KHR, create_surface(&state, invalid[0], @intCast(invalid[1]), &id));
    state.next_id = std.math.maxInt(u64);
    try std.testing.expectEqual(c.VK_ERROR_TOO_MANY_OBJECTS, create_surface(&state, 7, 1, &id));
    state.next_id = 1;
    for (0..MaxSurfaces) |_| try std.testing.expectEqual(c.VK_SUCCESS, create_surface(&state, 7, 1, &id));
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_HOST_MEMORY, create_surface(&state, 7, 1, &id));
    try std.testing.expect(!supports_surface(&state, 8, id));
    try std.testing.expect(!supports_surface(&state, 7, 0));
    try std.testing.expect(!destroy_surface(&state, 8, id));
    var caps: c.VkSurfaceCapabilitiesKHR = undefined;
    try std.testing.expectEqual(c.VK_ERROR_SURFACE_LOST_KHR, capabilities(&state, 0, &caps));
    var count: u32 = 1;
    var format_values: [4]c.VkSurfaceFormatKHR = undefined;
    var mode_values: [2]c.VkPresentModeKHR = undefined;
    try std.testing.expectEqual(c.VK_ERROR_SURFACE_LOST_KHR, formats(&state, 0, &count, null));
    try std.testing.expectEqual(c.VK_INCOMPLETE, formats(&state, id, &count, &format_values));
    try std.testing.expectEqual(c.VK_ERROR_SURFACE_LOST_KHR, modes(&state, 0, &count, null));
    try std.testing.expectEqual(c.VK_INCOMPLETE, modes(&state, id, &count, &mode_values));
    try std.testing.expectEqual(c.VK_SUCCESS, modes(&state, id, &count, null));
    const initial = test_info(id);
    inline for (.{ "sType", "flags", "minImageCount", "imageArrayLayers", "imageFormat", "imageColorSpace", "imageUsage", "imageSharingMode", "preTransform", "compositeAlpha", "presentMode" }) |field| {
        var info = initial;
        @field(info, field) = if (comptime std.mem.eql(u8, field, "sType")) 0 else if (comptime std.mem.eql(u8, field, "minImageCount")) 1 else if (comptime std.mem.eql(u8, field, "imageArrayLayers")) 2 else if (comptime std.mem.eql(u8, field, "imageFormat")) 1 else if (comptime std.mem.eql(u8, field, "imageUsage")) 0 else if (comptime std.mem.eql(u8, field, "preTransform") or std.mem.eql(u8, field, "compositeAlpha")) 0 else 1;
        try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 9, &info, &id));
    }
    var info = initial;
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 0, &info, &id));
    info.pNext = @ptrFromInt(1);
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 9, &info, &id));
    info = initial;
    info.surface = null;
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 9, &info, &id));
    info.surface = @ptrFromInt(999);
    try std.testing.expectEqual(c.VK_ERROR_SURFACE_LOST_KHR, create_swapchain(&state, &backend, 9, &info, &id));
    for ([_]u32{ 0, 16385 }) |extent| {
        inline for (.{ "width", "height" }) |axis| {
            info = initial;
            @field(info.imageExtent, axis) = extent;
            try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 9, &info, &id));
        }
    }
    info = initial;
    info.minImageCount = 4;
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 9, &info, &id));
    info = initial;
    info.imageUsage = 0x80000000;
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 9, &info, &id));
    info = initial;
    info.oldSwapchain = @ptrFromInt(999);
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, create_swapchain(&state, &backend, 9, &info, &id));
    info = initial;
    var chain: u64 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, create_swapchain(&state, &backend, 9, &info, &chain));
    var images: [3]u64 = undefined;
    count = 3;
    try std.testing.expectEqual(c.VK_SUCCESS, get_images(&state, 9, chain, &count, &images));
    try std.testing.expect(!is_present_image(&state, 0, images[0]));
    try std.testing.expect(!is_present_image(&state, 9, 0));
    try std.testing.expect(!is_present_image(&state, 8, images[0]));
    try std.testing.expect(!is_present_image(&state, 9, 999));
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_DATE_KHR, get_images(&state, 0, chain, &count, null));
    var index: u32 = 0;
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_DATE_KHR, acquire_next(&state, &backend, 9, 0, 0, 1, 0, &index));
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, acquire_next(&state, &backend, 9, chain, 0, 0, 0, &index));
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_DATE_KHR, present(&state, &backend, std.testing.allocator, 9, 0, 0, &.{}));
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, present(&state, &backend, std.testing.allocator, 9, chain, 3, &.{}));
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, present(&state, &backend, std.testing.allocator, 9, chain, 0, &.{}));
    destroy_swapchain(&state, &backend, 0, chain);
    destroy_device(&state, &backend, 8);
    destroy_device(&state, &backend, 9);
    try std.testing.expectEqual(fixture.created, fixture.destroyed);
}

test "presentation resize allocation failure and swapchain namespace quotas preserve owners" {
    var state: state_t = .{};
    var fixture: fixture_t = .{};
    const backend = test_backend(&fixture);
    var surface_id: u64 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, create_surface(&state, 7, 1, &surface_id));
    const info = test_info(surface_id);
    var chain_id: u64 = 0;
    state.next_id = std.math.maxInt(u64);
    try std.testing.expectEqual(c.VK_ERROR_TOO_MANY_OBJECTS, create_swapchain(&state, &backend, 9, &info, &chain_id));
    state.next_id = 2;
    try std.testing.expectEqual(c.VK_SUCCESS, create_swapchain(&state, &backend, 9, &info, &chain_id));
    var index: u32 = 0;
    try std.testing.expectEqual(c.VK_SUCCESS, acquire_next(&state, &backend, 9, chain_id, 0, 0, 12, &index));
    const slot = swapchain(&state, 9, chain_id).?;
    slot.width = 63;
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_DATE_KHR, present(&state, &backend, std.testing.allocator, 9, chain_id, index, &.{}));
    slot.width = 64;
    slot.height = 63;
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_DATE_KHR, present(&state, &backend, std.testing.allocator, 9, chain_id, index, &.{}));
    slot.height = 64;
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_HOST_MEMORY, present(&state, &backend, std.testing.failing_allocator, 9, chain_id, index, &.{}));
    const waits = [_]c.VkSemaphore{@ptrFromInt(1)} ** 65;
    try std.testing.expectEqual(c.VK_ERROR_INITIALIZATION_FAILED, present(&state, &backend, std.testing.allocator, 9, chain_id, index, &waits));
    for (1..MaxSwapchains) |_| try std.testing.expectEqual(c.VK_SUCCESS, create_swapchain(&state, &backend, 9, &info, &chain_id));
    try std.testing.expectEqual(c.VK_ERROR_OUT_OF_HOST_MEMORY, create_swapchain(&state, &backend, 9, &info, &chain_id));
    destroy_device(&state, &backend, 9);
    try std.testing.expectEqual(fixture.created, fixture.destroyed);
    state.surfaces[0].hwnd = 2;
    var count: u32 = 0;
    try std.testing.expectEqual(c.VK_ERROR_SURFACE_LOST_KHR, formats(&state, surface_id, &count, null));
    try std.testing.expectEqual(c.VK_ERROR_SURFACE_LOST_KHR, modes(&state, surface_id, &count, null));
}
