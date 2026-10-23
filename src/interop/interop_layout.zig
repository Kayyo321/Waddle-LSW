const std = @import("std");

/// Layout v3 mapping size in bytes.
pub const MappingBytes: u64 = 0x8000_0000;
/// Layout v3 shared header size in bytes.
pub const HeaderBytes: usize = 4096;
/// Layout v3 descriptor size in bytes.
pub const DescriptorBytes: usize = 64;
/// Number of shared frame descriptors.
pub const SlotCount: u32 = 192;
/// Number of shared audio rings.
pub const AudioRingCount: u32 = 8;
/// Layout v3 magic, little-endian bytes SMW3.
pub const ShmMagic: u32 = 0x3357_4D53;
/// Layout version required by seamless interop.
pub const LayoutVersion: u32 = 3;
/// Guest-owned slot heap start in bytes from mapping base.
pub const HeapOffset: u64 = 0x0020_0000;
/// Guest-owned slot heap size in bytes.
pub const HeapBytes: u64 = 0x7FC0_0000;
/// First byte after the slot heap.
pub const HeapEndOffset: u64 = HeapOffset + HeapBytes;
/// Allocation page size in bytes.
pub const PageBytes: u32 = 2_097_152;
/// Required mapping and descriptor alignment in bytes.
pub const MappingAlignment: usize = 64;
/// Guest heartbeat fields start at this byte offset.
pub const GuestHeartbeatOffset: usize = 56;
/// Host heartbeat field byte offset.
pub const HostHeartbeatOffset: usize = 64;
/// First byte of header reserved storage.
pub const HeaderReservedOffset: usize = 72;
/// Audio ring region start in bytes from mapping base.
pub const AudioRingBaseOffset: u64 = 0x0001_0000;
/// Audio ring allocation stride in bytes.
pub const AudioRingStrideBytes: u64 = 0x0002_0000;
/// Audio ring header size in bytes.
pub const AudioRingHeaderBytes: u64 = 64;
/// Offset of audio PCM relative to its ring header.
pub const AudioPcmOffset: u64 = 128;
/// Number of stereo frames in each layout-v3 audio ring.
pub const AudioCapacityFrames: u32 = 16_384;
/// Bytes in one stereo S16LE frame.
pub const AudioFrameBytes: u32 = 4;
/// Premultiplied BGRA pixel format value used by the shared descriptor.
pub const PixelFormatBgra: u32 = 0x3432_5241;
/// Maximum captured frame width in physical pixels.
pub const MaxCaptureWidth: u32 = 7680;
/// Maximum captured frame height in physical pixels.
pub const MaxCaptureHeight: u32 = 4320;
/// Required row-stride alignment in bytes.
pub const RowAlignmentBytes: u32 = 64;
/// Descriptor state meaning the guest published a complete frame.
pub const SlotReady: u32 = 2;

/// Validated immutable header values copied out of a caller-owned byte snapshot.
pub const shm_header_snapshot_t = struct {
    /// Nonzero agent session epoch copied from byte offset 8.
    session_epoch: u64,
    /// Total mapping length copied from byte offset 16.
    total_bytes: u64,
    /// Descriptor capacity copied from byte offset 24.
    slot_count: u32,
    /// Audio ring count copied from byte offset 28.
    audio_ring_count: u32,
    /// Heap start copied from byte offset 32.
    heap_offset: u64,
    /// Heap length copied from byte offset 40.
    heap_bytes: u64,
    /// Heap page size copied from byte offset 48.
    page_bytes: u32,
    /// Guest heartbeat snapshot copied from byte offset 56.
    guest_heartbeat: u64,
    /// Host heartbeat snapshot copied from byte offset 64.
    host_heartbeat: u64,
};

/// Validated ready-frame descriptor values; all offsets remain mapping-relative.
pub const slot_snapshot_t = struct {
    /// Descriptor index in the fixed shared descriptor table.
    descriptor_index: u32,
    /// Owning guest HWND encoded as a nonzero u64.
    window: u64,
    /// Owning window generation, nonzero for every admitted incarnation.
    generation: u64,
    /// Nonzero monotonically assigned frame sequence.
    frame_sequence: u64,
    /// Mapping-relative start of the pixel allocation.
    data_offset: u64,
    /// Pixel allocation capacity in whole 2 MiB pages.
    data_bytes: u32,
    /// Captured frame width in physical pixels.
    width: u32,
    /// Captured frame height in physical pixels.
    height: u32,
    /// Row size in bytes, including any alignment padding.
    stride: u32,
};

/// Checked result of calculating one clamped BGRA frame allocation.
pub const frame_layout_t = struct {
    /// Width after independently clamping to MaxCaptureWidth.
    width: u32,
    /// Height after independently clamping to MaxCaptureHeight.
    height: u32,
    /// 64-byte-aligned row size in bytes.
    stride: u32,
    /// Exact pixel payload bytes before page rounding.
    pixel_bytes: u64,
    /// Allocation capacity rounded up to whole PageBytes pages.
    data_bytes: u32,
};

/// Bounded offsets and byte lengths for one fixed-format audio ring.
pub const audio_ring_bounds_t = struct {
    /// Ring index in the range 0..AudioRingCount.
    ring_index: u32,
    /// Mapping-relative ring header offset.
    header_offset: u64,
    /// Mapping-relative PCM start offset.
    pcm_offset: u64,
    /// PCM payload capacity in bytes.
    pcm_bytes: u32,
    /// PCM frame capacity.
    capacity_frames: u32,
};

/// Errors returned when a byte snapshot does not match the frozen layout v3 ABI.
pub const validation_error_t = error{
    InvalidHeaderLength,
    InvalidMappingLength,
    InvalidMappingAlignment,
    InvalidMagic,
    InvalidLayoutVersion,
    InvalidSessionEpoch,
    InvalidTotalBytes,
    InvalidSlotCount,
    InvalidAudioRingCount,
    InvalidHeapRange,
    InvalidPageSize,
    NonzeroReservedBytes,
    InvalidDescriptorLength,
    InvalidDescriptorIndex,
    InvalidSlotState,
    InvalidPixelFormat,
    InvalidWindowIdentity,
    InvalidFrameSequence,
    InvalidGeometry,
    InvalidStride,
    InvalidSlotCapacity,
    SlotOutsideHeap,
    InvalidAudioRingIndex,
};

fn read_u32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

fn read_u64(bytes: []const u8, offset: usize) u64 {
    return std.mem.readInt(u64, bytes[offset..][0..8], .little);
}

fn bytes_are_zero(bytes: []const u8) bool {
    for (bytes) |byte| {
        if (byte != 0) return false;
    }
    return true;
}

/// Validate an owned 4096-byte header snapshot against the actual mapping facts.
///
/// @param[in] header_bytes Non-NULL private snapshot of exactly HeaderBytes;
///                         must not alias live atomic mapping storage.
/// @param[in] mapping_address Nonzero actual borrowed mapping base address,
///                            used only for the required 64-byte alignment check.
/// @param[in] mapping_length Actual mapped byte length, which must equal 2 GiB.
/// @param[in] expected_session_epoch Nonzero epoch received in the v2 Hello.
/// @param[out] snapshot Non-NULL caller-owned output; unchanged on every error.
/// @return void on a valid layout-v3 header, otherwise validation_error_t.
/// @note Allocation-free and thread-safe for an immutable private byte copy.
///       The caller owns the input and output memory. This function performs
///       no atomic loads and must not read a live mapped heartbeat field.
pub fn validate_shm_header(
    header_bytes: []const u8,
    mapping_address: usize,
    mapping_length: u64,
    expected_session_epoch: u64,
    snapshot: *shm_header_snapshot_t,
) validation_error_t!void {
    if (header_bytes.len != HeaderBytes) return error.InvalidHeaderLength;
    if (mapping_length != MappingBytes) return error.InvalidMappingLength;
    if (mapping_address == 0 or mapping_address % MappingAlignment != 0)
        return error.InvalidMappingAlignment;
    if (expected_session_epoch == 0) return error.InvalidSessionEpoch;
    if (read_u32(header_bytes, 0) != ShmMagic) return error.InvalidMagic;
    if (read_u32(header_bytes, 4) != LayoutVersion) return error.InvalidLayoutVersion;

    const session_epoch = read_u64(header_bytes, 8);
    if (session_epoch == 0 or session_epoch != expected_session_epoch)
        return error.InvalidSessionEpoch;
    const total_bytes = read_u64(header_bytes, 16);
    if (total_bytes != mapping_length) return error.InvalidTotalBytes;

    const slot_count = read_u32(header_bytes, 24);
    if (slot_count != SlotCount) return error.InvalidSlotCount;
    const audio_ring_count = read_u32(header_bytes, 28);
    if (audio_ring_count != AudioRingCount) return error.InvalidAudioRingCount;

    const heap_offset = read_u64(header_bytes, 32);
    const heap_bytes = read_u64(header_bytes, 40);
    // These exact fixed ranges, combined with the exact 2 GiB mapping length
    // checked above, prove the heap end without separate overflow-prone math.
    if (heap_offset != HeapOffset or heap_bytes != HeapBytes)
        return error.InvalidHeapRange;
    if (read_u32(header_bytes, 48) != PageBytes or read_u32(header_bytes, 52) != 0)
        return error.InvalidPageSize;
    if (!bytes_are_zero(header_bytes[HeaderReservedOffset..]))
        return error.NonzeroReservedBytes;

    const result = shm_header_snapshot_t{
        .session_epoch = session_epoch,
        .total_bytes = total_bytes,
        .slot_count = slot_count,
        .audio_ring_count = audio_ring_count,
        .heap_offset = heap_offset,
        .heap_bytes = heap_bytes,
        .page_bytes = PageBytes,
        .guest_heartbeat = read_u64(header_bytes, GuestHeartbeatOffset),
        .host_heartbeat = read_u64(header_bytes, HostHeartbeatOffset),
    };
    snapshot.* = result;
}

/// Validate one detached descriptor snapshot and bind it to expected identity.
///
/// @param[in] descriptor_bytes Non-NULL private snapshot of exactly 64 bytes;
///                             must not alias a live atomic state field.
/// @param[in] header Non-NULL previously validated immutable header snapshot.
/// @param[in] descriptor_index Table index 0..191 represented by these bytes.
/// @param[in] expected_window Nonzero expected HWND identity for this slot.
/// @param[in] expected_generation Nonzero expected window generation.
/// @param[out] snapshot Non-NULL caller-owned output; unchanged on every error.
/// @return void on a valid Ready frame, otherwise validation_error_t.
/// @note Allocation-free; the caller owns snapshots. This function does not
///       perform atomic ownership transitions or dereference mapped memory.
pub fn validate_slot_descriptor(
    descriptor_bytes: []const u8,
    header: *const shm_header_snapshot_t,
    descriptor_index: u32,
    expected_window: u64,
    expected_generation: u64,
    snapshot: *slot_snapshot_t,
) validation_error_t!void {
    if (descriptor_bytes.len != DescriptorBytes) return error.InvalidDescriptorLength;
    if (descriptor_index >= SlotCount) return error.InvalidDescriptorIndex;
    if (header.total_bytes != MappingBytes or header.heap_offset != HeapOffset or
        header.heap_bytes != HeapBytes or header.page_bytes != PageBytes)
        return error.InvalidHeapRange;
    if (read_u32(descriptor_bytes, 0) != SlotReady) return error.InvalidSlotState;
    if (read_u32(descriptor_bytes, 4) != PixelFormatBgra) return error.InvalidPixelFormat;

    const window = read_u64(descriptor_bytes, 8);
    const generation = read_u64(descriptor_bytes, 16);
    if (window == 0 or generation == 0 or window != expected_window or
        generation != expected_generation)
        return error.InvalidWindowIdentity;
    const frame_sequence = read_u64(descriptor_bytes, 24);
    if (frame_sequence == 0) return error.InvalidFrameSequence;

    const data_offset = read_u64(descriptor_bytes, 32);
    const data_bytes = read_u32(descriptor_bytes, 40);
    const width = read_u32(descriptor_bytes, 44);
    const height = read_u32(descriptor_bytes, 48);
    const stride = read_u32(descriptor_bytes, 52);
    if (read_u64(descriptor_bytes, 56) != 0) return error.NonzeroReservedBytes;
    if (width == 0 or height == 0 or width > MaxCaptureWidth or height > MaxCaptureHeight)
        return error.InvalidGeometry;

    const row_bytes = std.math.mul(u64, width, 4) catch return error.InvalidStride;
    if (stride < row_bytes or stride % RowAlignmentBytes != 0)
        return error.InvalidStride;
    const pixel_bytes = std.math.mul(u64, stride, height) catch
        return error.InvalidSlotCapacity;
    const rounded_bytes = std.math.add(u64, pixel_bytes, PageBytes - 1) catch
        return error.InvalidSlotCapacity;
    const required_bytes = (rounded_bytes / PageBytes) * PageBytes;
    if (data_bytes == 0 or data_bytes % PageBytes != 0 or
        @as(u64, data_bytes) < required_bytes)
        return error.InvalidSlotCapacity;
    if (data_offset < header.heap_offset or data_offset % PageBytes != 0 or
        data_offset > HeapEndOffset or @as(u64, data_bytes) > HeapEndOffset - data_offset)
        return error.SlotOutsideHeap;

    snapshot.* = .{
        .descriptor_index = descriptor_index,
        .window = window,
        .generation = generation,
        .frame_sequence = frame_sequence,
        .data_offset = data_offset,
        .data_bytes = data_bytes,
        .width = width,
        .height = height,
        .stride = stride,
    };
}

/// Calculate a clamped BGRA frame layout with checked stride and page math.
///
/// @param[in] requested_width Requested nonzero physical-pixel width.
/// @param[in] requested_height Requested nonzero physical-pixel height.
/// @return A frame layout or InvalidGeometry/InvalidSlotCapacity.
/// @note Width and height are independently clamped to their frozen maxima;
///       this helper allocates no memory and is thread-safe.
pub fn frame_layout_for_capture(requested_width: u32, requested_height: u32) validation_error_t!frame_layout_t {
    if (requested_width == 0 or requested_height == 0) return error.InvalidGeometry;
    const width = @min(requested_width, MaxCaptureWidth);
    const height = @min(requested_height, MaxCaptureHeight);
    const unaligned_stride = std.math.mul(u64, width, 4) catch
        return error.InvalidSlotCapacity;
    const aligned_stride = std.math.add(u64, unaligned_stride, RowAlignmentBytes - 1) catch
        return error.InvalidSlotCapacity;
    const stride = (aligned_stride / RowAlignmentBytes) * RowAlignmentBytes;
    const pixel_bytes = std.math.mul(u64, stride, height) catch
        return error.InvalidSlotCapacity;
    const rounded_bytes = std.math.add(u64, pixel_bytes, PageBytes - 1) catch
        return error.InvalidSlotCapacity;
    const data_bytes = (rounded_bytes / PageBytes) * PageBytes;
    if (stride > std.math.maxInt(u32) or data_bytes > std.math.maxInt(u32))
        return error.InvalidSlotCapacity;
    return .{
        .width = width,
        .height = height,
        .stride = @intCast(stride),
        .pixel_bytes = pixel_bytes,
        .data_bytes = @intCast(data_bytes),
    };
}

/// Return fixed PCM bounds for one layout-v3 audio ring.
///
/// @param[in] mapping_length Actual mapped byte length; must equal 2 GiB.
/// @param[in] ring_index Ring number 0..7.
/// @param[out] bounds Non-NULL caller-owned output; unchanged on every error.
/// @return void on success or InvalidMappingLength/InvalidAudioRingIndex.
/// @note Allocation-free and thread-safe; offsets are borrowed mapping-relative
///       values and this function does not read or initialize audio atomics.
pub fn audio_ring_bounds(
    mapping_length: u64,
    ring_index: u32,
    bounds: *audio_ring_bounds_t,
) validation_error_t!void {
    if (mapping_length != MappingBytes) return error.InvalidMappingLength;
    if (ring_index >= AudioRingCount) return error.InvalidAudioRingIndex;
    const pcm_bytes = AudioCapacityFrames * AudioFrameBytes;
    const ring_offset = AudioRingBaseOffset + @as(u64, ring_index) * AudioRingStrideBytes;
    const pcm_offset = ring_offset + AudioPcmOffset;
    const pcm_end = pcm_offset + pcm_bytes;
    const ring_end = ring_offset + AudioRingStrideBytes;
    if (pcm_end > ring_end or pcm_end > mapping_length)
        return error.InvalidMappingLength;
    bounds.* = .{
        .ring_index = ring_index,
        .header_offset = ring_offset,
        .pcm_offset = pcm_offset,
        .pcm_bytes = pcm_bytes,
        .capacity_frames = AudioCapacityFrames,
    };
}

// Test-only fixtures.
fn call_validate_shm_header(
    header_bytes: []const u8,
    mapping_address: usize,
    mapping_length: u64,
    expected_session_epoch: u64,
    snapshot: *shm_header_snapshot_t,
) validation_error_t!void {
    return @call(.never_inline, validate_shm_header, .{
        header_bytes,
        mapping_address,
        mapping_length,
        expected_session_epoch,
        snapshot,
    });
}

fn call_validate_slot_descriptor(
    descriptor_bytes: []const u8,
    header: *const shm_header_snapshot_t,
    descriptor_index: u32,
    expected_window: u64,
    expected_generation: u64,
    snapshot: *slot_snapshot_t,
) validation_error_t!void {
    return @call(.never_inline, validate_slot_descriptor, .{
        descriptor_bytes,
        header,
        descriptor_index,
        expected_window,
        expected_generation,
        snapshot,
    });
}

fn call_frame_layout_for_capture(
    requested_width: u32,
    requested_height: u32,
) validation_error_t!frame_layout_t {
    return @call(.never_inline, frame_layout_for_capture, .{
        requested_width,
        requested_height,
    });
}

fn write_u32(bytes: []u8, offset: usize, value: u32) void {
    std.mem.writeInt(u32, bytes[offset..][0..4], value, .little);
}

fn write_u64(bytes: []u8, offset: usize, value: u64) void {
    std.mem.writeInt(u64, bytes[offset..][0..8], value, .little);
}

fn make_valid_header(allocator: std.mem.Allocator) ![]u8 {
    const bytes = try allocator.alloc(u8, HeaderBytes);
    @memset(bytes, 0);
    write_u32(bytes, 0, ShmMagic);
    write_u32(bytes, 4, LayoutVersion);
    write_u64(bytes, 8, 0xAABB_CCDD_EEFF_0011);
    write_u64(bytes, 16, MappingBytes);
    write_u32(bytes, 24, SlotCount);
    write_u32(bytes, 28, AudioRingCount);
    write_u64(bytes, 32, HeapOffset);
    write_u64(bytes, 40, HeapBytes);
    write_u32(bytes, 48, PageBytes);
    write_u64(bytes, GuestHeartbeatOffset, 17);
    write_u64(bytes, HostHeartbeatOffset, 23);
    return bytes;
}

fn validate_fixture_header(bytes: []const u8, out: *shm_header_snapshot_t) validation_error_t!void {
    return call_validate_shm_header(bytes, 0x1000, MappingBytes, 0xAABB_CCDD_EEFF_0011, out);
}

test "valid layout header is snapshotted with ABI fields" {
    const allocator = std.testing.allocator;
    const bytes = try make_valid_header(allocator);
    defer allocator.free(bytes);
    var snapshot = shm_header_snapshot_t{
        .session_epoch = 0,
        .total_bytes = 0,
        .slot_count = 0,
        .audio_ring_count = 0,
        .heap_offset = 0,
        .heap_bytes = 0,
        .page_bytes = 0,
        .guest_heartbeat = 0,
        .host_heartbeat = 0,
    };
    try validate_fixture_header(bytes, &snapshot);
    try std.testing.expectEqual(@as(u64, 0xAABB_CCDD_EEFF_0011), snapshot.session_epoch);
    try std.testing.expectEqual(MappingBytes, snapshot.total_bytes);
    try std.testing.expectEqual(SlotCount, snapshot.slot_count);
    try std.testing.expectEqual(AudioRingCount, snapshot.audio_ring_count);
    try std.testing.expectEqual(@as(u64, 17), snapshot.guest_heartbeat);
    try std.testing.expectEqual(@as(u64, 23), snapshot.host_heartbeat);
}

test "header validation rejects length mapping alignment and epoch boundaries" {
    const allocator = std.testing.allocator;
    const bytes = try make_valid_header(allocator);
    defer allocator.free(bytes);
    var out = shm_header_snapshot_t{
        .session_epoch = 1,
        .total_bytes = 2,
        .slot_count = 3,
        .audio_ring_count = 4,
        .heap_offset = 5,
        .heap_bytes = 6,
        .page_bytes = 7,
        .guest_heartbeat = 8,
        .host_heartbeat = 9,
    };
    const before = out;
    try std.testing.expectError(error.InvalidHeaderLength, call_validate_shm_header(bytes[0 .. HeaderBytes - 1], 0x1000, MappingBytes, out.session_epoch, &out));
    try std.testing.expectError(error.InvalidMappingLength, call_validate_shm_header(bytes, 0x1000, MappingBytes - 1, out.session_epoch, &out));
    try std.testing.expectError(error.InvalidMappingAlignment, call_validate_shm_header(bytes, 0, MappingBytes, out.session_epoch, &out));
    try std.testing.expectError(error.InvalidMappingAlignment, call_validate_shm_header(bytes, 0x1001, MappingBytes, out.session_epoch, &out));
    try std.testing.expectError(error.InvalidSessionEpoch, call_validate_shm_header(bytes, 0x1000, MappingBytes, 0, &out));
    try std.testing.expectError(error.InvalidSessionEpoch, call_validate_shm_header(bytes, 0x1000, MappingBytes, 7, &out));
    const zero_epoch = try allocator.dupe(u8, bytes);
    defer allocator.free(zero_epoch);
    write_u64(zero_epoch, 8, 0);
    try std.testing.expectError(error.InvalidSessionEpoch, validate_fixture_header(zero_epoch, &out));
    try std.testing.expectEqualDeep(before, out);
}

test "header validation rejects changed fixed fields and nonzero reserved bytes" {
    const allocator = std.testing.allocator;
    const original = try make_valid_header(allocator);
    defer allocator.free(original);
    const mutated = try allocator.dupe(u8, original);
    defer allocator.free(mutated);
    var out: shm_header_snapshot_t = undefined;

    const cases = [_]struct { offset: usize, value: u8, expected: anyerror }{
        .{ .offset = 0, .value = 0, .expected = error.InvalidMagic },
        .{ .offset = 4, .value = 2, .expected = error.InvalidLayoutVersion },
        .{ .offset = 16, .value = 1, .expected = error.InvalidTotalBytes },
        .{ .offset = 24, .value = 0, .expected = error.InvalidSlotCount },
        .{ .offset = 28, .value = 0, .expected = error.InvalidAudioRingCount },
        .{ .offset = 32, .value = 1, .expected = error.InvalidHeapRange },
        .{ .offset = 40, .value = 1, .expected = error.InvalidHeapRange },
        .{ .offset = 48, .value = 1, .expected = error.InvalidPageSize },
        .{ .offset = 52, .value = 1, .expected = error.InvalidPageSize },
        .{ .offset = 80, .value = 1, .expected = error.NonzeroReservedBytes },
    };
    for (cases) |case| {
        @memcpy(mutated, original);
        mutated[case.offset] = case.value;
        try std.testing.expectError(case.expected, validate_fixture_header(mutated, &out));
    }
}

fn make_valid_descriptor(allocator: std.mem.Allocator) ![]u8 {
    const bytes = try allocator.alloc(u8, DescriptorBytes);
    @memset(bytes, 0);
    write_u32(bytes, 0, SlotReady);
    write_u32(bytes, 4, PixelFormatBgra);
    write_u64(bytes, 8, 0x1234);
    write_u64(bytes, 16, 9);
    write_u64(bytes, 24, 1);
    write_u64(bytes, 32, HeapOffset);
    write_u32(bytes, 40, PageBytes);
    write_u32(bytes, 44, 640);
    write_u32(bytes, 48, 480);
    write_u32(bytes, 52, 2560);
    return bytes;
}

fn validate_fixture_descriptor(bytes: []const u8, out: *slot_snapshot_t) !void {
    var header: shm_header_snapshot_t = undefined;
    const raw_header = try make_valid_header(std.testing.allocator);
    defer std.testing.allocator.free(raw_header);
    try validate_fixture_header(raw_header, &header);
    return call_validate_slot_descriptor(bytes, &header, 0, 0x1234, 9, out);
}

fn validate_fixture_descriptor_with_header(
    bytes: []const u8,
    header: *const shm_header_snapshot_t,
    out: *slot_snapshot_t,
) validation_error_t!void {
    return call_validate_slot_descriptor(bytes, header, 0, 0x1234, 9, out);
}

test "ready descriptor validates identity geometry and complete heap bounds" {
    const allocator = std.testing.allocator;
    const bytes = try make_valid_descriptor(allocator);
    defer allocator.free(bytes);
    var out: slot_snapshot_t = undefined;
    try validate_fixture_descriptor(bytes, &out);
    try std.testing.expectEqual(@as(u32, 0), out.descriptor_index);
    try std.testing.expectEqual(@as(u64, 0x1234), out.window);
    try std.testing.expectEqual(@as(u64, 9), out.generation);
    try std.testing.expectEqual(@as(u64, HeapOffset), out.data_offset);
    try std.testing.expectEqual(@as(u32, PageBytes), out.data_bytes);
}

test "descriptor validation rejects malformed identity state and bounds without changing output" {
    const allocator = std.testing.allocator;
    const original = try make_valid_descriptor(allocator);
    defer allocator.free(original);
    const mutated = try allocator.dupe(u8, original);
    defer allocator.free(mutated);
    var out = slot_snapshot_t{
        .descriptor_index = 7,
        .window = 8,
        .generation = 9,
        .frame_sequence = 10,
        .data_offset = 11,
        .data_bytes = 12,
        .width = 13,
        .height = 14,
        .stride = 15,
    };
    const before = out;
    try std.testing.expectError(
        error.InvalidDescriptorLength,
        validate_fixture_descriptor(original[0 .. DescriptorBytes - 1], &out),
    );
    try std.testing.expectEqualDeep(before, out);
    const cases = [_]struct { offset: usize, value: u8, expected: anyerror }{
        .{ .offset = 0, .value = 0, .expected = error.InvalidSlotState },
        .{ .offset = 4, .value = 0, .expected = error.InvalidPixelFormat },
        .{ .offset = 8, .value = 0, .expected = error.InvalidWindowIdentity },
        .{ .offset = 16, .value = 0, .expected = error.InvalidWindowIdentity },
        .{ .offset = 24, .value = 0, .expected = error.InvalidFrameSequence },
        .{ .offset = 40, .value = 1, .expected = error.InvalidSlotCapacity },
        .{ .offset = 52, .value = 1, .expected = error.InvalidStride },
        .{ .offset = 56, .value = 1, .expected = error.NonzeroReservedBytes },
    };
    for (cases) |case| {
        @memcpy(mutated, original);
        mutated[case.offset] = case.value;
        try std.testing.expectError(case.expected, validate_fixture_descriptor(mutated, &out));
        try std.testing.expectEqualDeep(before, out);
    }
    @memcpy(mutated, original);
    write_u32(mutated, 44, 0);
    try std.testing.expectError(error.InvalidGeometry, validate_fixture_descriptor(mutated, &out));
    try std.testing.expectEqualDeep(before, out);
    @memcpy(mutated, original);
    write_u32(mutated, 48, 0);
    try std.testing.expectError(error.InvalidGeometry, validate_fixture_descriptor(mutated, &out));
    try std.testing.expectEqualDeep(before, out);
    @memcpy(mutated, original);
    write_u32(mutated, 52, 2561);
    try std.testing.expectError(error.InvalidStride, validate_fixture_descriptor(mutated, &out));
    try std.testing.expectEqualDeep(before, out);
    @memcpy(mutated, original);
    write_u64(mutated, 32, HeapEndOffset);
    try std.testing.expectError(error.SlotOutsideHeap, validate_fixture_descriptor(mutated, &out));
    @memcpy(mutated, original);
    write_u64(mutated, 32, std.math.maxInt(u64));
    write_u32(mutated, 40, 0xFFE0_0000);
    try std.testing.expectError(error.SlotOutsideHeap, validate_fixture_descriptor(mutated, &out));
    try std.testing.expectEqualDeep(before, out);
    @memcpy(mutated, original);
    write_u32(mutated, 40, PageBytes);
    write_u32(mutated, 44, MaxCaptureWidth);
    write_u32(mutated, 48, MaxCaptureHeight);
    write_u32(mutated, 52, 30_720);
    try std.testing.expectError(error.InvalidSlotCapacity, validate_fixture_descriptor(mutated, &out));
    try std.testing.expectEqualDeep(before, out);
    try std.testing.expectError(error.InvalidDescriptorIndex, call_validate_slot_descriptor(original, &.{
        .session_epoch = 1,
        .total_bytes = MappingBytes,
        .slot_count = SlotCount,
        .audio_ring_count = AudioRingCount,
        .heap_offset = HeapOffset,
        .heap_bytes = HeapBytes,
        .page_bytes = PageBytes,
        .guest_heartbeat = 0,
        .host_heartbeat = 0,
    }, SlotCount, 0x1234, 9, &out));

    var valid_header: shm_header_snapshot_t = undefined;
    const raw_header = try make_valid_header(allocator);
    defer allocator.free(raw_header);
    try validate_fixture_header(raw_header, &valid_header);
    const malformed_headers = [_]shm_header_snapshot_t{
        .{ .session_epoch = valid_header.session_epoch, .total_bytes = 1, .slot_count = SlotCount, .audio_ring_count = AudioRingCount, .heap_offset = HeapOffset, .heap_bytes = HeapBytes, .page_bytes = PageBytes, .guest_heartbeat = 0, .host_heartbeat = 0 },
        .{ .session_epoch = valid_header.session_epoch, .total_bytes = MappingBytes, .slot_count = SlotCount, .audio_ring_count = AudioRingCount, .heap_offset = 1, .heap_bytes = HeapBytes, .page_bytes = PageBytes, .guest_heartbeat = 0, .host_heartbeat = 0 },
        .{ .session_epoch = valid_header.session_epoch, .total_bytes = MappingBytes, .slot_count = SlotCount, .audio_ring_count = AudioRingCount, .heap_offset = HeapOffset, .heap_bytes = 1, .page_bytes = PageBytes, .guest_heartbeat = 0, .host_heartbeat = 0 },
        .{ .session_epoch = valid_header.session_epoch, .total_bytes = MappingBytes, .slot_count = SlotCount, .audio_ring_count = AudioRingCount, .heap_offset = HeapOffset, .heap_bytes = HeapBytes, .page_bytes = 1, .guest_heartbeat = 0, .host_heartbeat = 0 },
    };
    for (malformed_headers) |malformed_header_value| {
        var malformed_header = malformed_header_value;
        try std.testing.expectError(
            error.InvalidHeapRange,
            validate_fixture_descriptor_with_header(original, &malformed_header, &out),
        );
        try std.testing.expectEqualDeep(before, out);
    }
}

test "frame layout clamps maxima and rounds stride and heap pages" {
    const maximum = try call_frame_layout_for_capture(MaxCaptureWidth + 100, MaxCaptureHeight + 1);
    try std.testing.expectEqual(MaxCaptureWidth, maximum.width);
    try std.testing.expectEqual(MaxCaptureHeight, maximum.height);
    try std.testing.expectEqual(@as(u32, 30_720), maximum.stride);
    try std.testing.expectEqual(@as(u64, 132_710_400), maximum.pixel_bytes);
    try std.testing.expectEqual(@as(u32, 134_217_728), maximum.data_bytes);

    const odd = try call_frame_layout_for_capture(1, 1);
    try std.testing.expectEqual(@as(u32, 64), odd.stride);
    try std.testing.expectEqual(@as(u32, PageBytes), odd.data_bytes);
    try std.testing.expectError(error.InvalidGeometry, call_frame_layout_for_capture(0, 1));
    try std.testing.expectError(error.InvalidGeometry, call_frame_layout_for_capture(1, 0));
}

test "audio ring bounds cover eight disjoint strides and preserve output on error" {
    var prior = audio_ring_bounds_t{
        .ring_index = 55,
        .header_offset = 66,
        .pcm_offset = 77,
        .pcm_bytes = 88,
        .capacity_frames = 99,
    };
    const before = prior;
    try std.testing.expectError(error.InvalidAudioRingIndex, audio_ring_bounds(MappingBytes, AudioRingCount, &prior));
    try std.testing.expectError(error.InvalidMappingLength, audio_ring_bounds(MappingBytes - 1, 0, &prior));
    try std.testing.expectEqualDeep(before, prior);

    var previous_end: u64 = 0;
    for (0..AudioRingCount) |index| {
        try audio_ring_bounds(MappingBytes, @intCast(index), &prior);
        try std.testing.expectEqual(@as(u32, @intCast(index)), prior.ring_index);
        try std.testing.expectEqual(@as(u32, 65_536), prior.pcm_bytes);
        try std.testing.expectEqual(AudioCapacityFrames, prior.capacity_frames);
        try std.testing.expect(prior.pcm_offset >= previous_end);
        previous_end = prior.pcm_offset + prior.pcm_bytes;
        const stride_end = prior.header_offset + AudioRingStrideBytes;
        try std.testing.expect(previous_end <= stride_end);
    }
}

test "header parser survives deterministic one-byte mutation corpus" {
    const allocator = std.testing.allocator;
    const original = try make_valid_header(allocator);
    defer allocator.free(original);
    const mutated = try allocator.dupe(u8, original);
    defer allocator.free(mutated);
    var out: shm_header_snapshot_t = undefined;
    var random = std.Random.DefaultPrng.init(0x5EED_2026);
    const rng = random.random();
    for (0..20_000) |_| {
        @memcpy(mutated, original);
        const index = rng.uintLessThan(usize, HeaderBytes);
        mutated[index] ^= @as(u8, @intCast(rng.uintLessThan(u16, 255) + 1));
        const result = validate_fixture_header(mutated, &out);
        if (result) |_| {
            try std.testing.expect(index >= GuestHeartbeatOffset and index < HeaderReservedOffset);
        } else |_| {
            try std.testing.expect(index < GuestHeartbeatOffset or index >= HeaderReservedOffset);
        }
    }
}
