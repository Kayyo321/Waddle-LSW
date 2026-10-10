//! Allocation-free typed Properties2 profile. Recognition grants no public API or extension support.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Immutable recognized node ceiling; no ownership or mutable storage.
pub const MaxNodes: usize = 5;
/// Immutable maximal initialized reply extent including core data.
pub const MaxReplyBytes: usize = 2044;
/// Exact recognized canonical SDK tags, aliases share numeric identity.
pub const KnownTags = [_]u32{ c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR };
/// Owned typed SDK payload; active member follows node_t.type_tag, headers carry no borrowed pointers.
pub const data_t = extern union {
    /// Owned VkPhysicalDeviceVulkan11Properties named payload and zeroed native padding.
    core11: c.VkPhysicalDeviceVulkan11Properties,
    /// Owned VkPhysicalDeviceVulkan12Properties named payload and zeroed native padding.
    core12: c.VkPhysicalDeviceVulkan12Properties,
    /// Owned VkPhysicalDeviceVulkan13Properties named payload and zeroed native padding.
    core13: c.VkPhysicalDeviceVulkan13Properties,
    /// Owned VkPhysicalDeviceRobustness2PropertiesEXT named payload and zeroed native padding.
    robustness: c.VkPhysicalDeviceRobustness2PropertiesEXT,
    /// Owned VkPhysicalDeviceMaintenance5Properties named payload and zeroed native padding.
    maintenance: c.VkPhysicalDeviceMaintenance5Properties,
};
/// Owned recognized node; no foreign storage or heap ownership.
pub const node_t = struct {
    /// Canonical SDK tag; zero for inactive storage.
    type_tag: u32,
    /// Owned SDK union selected by type_tag.
    data: data_t,
};
/// Owned complete reply; all inactive storage is zero, immutable sharing is safe.
pub const result_t = struct {
    /// Initialized forward-order node extent0..5.
    count: u8,
    /// Owned forward-order nodes, no retained application pointers.
    nodes: [MaxNodes]node_t,
    /// Owned core SDK named values, no native padding read from wire.
    properties: c.VkPhysicalDeviceProperties,
};
const BooleanMembers = .{
    .{ c.VkPhysicalDeviceLimits, "timestampComputeAndGraphics" },
    .{ c.VkPhysicalDeviceLimits, "strictLines" },
    .{ c.VkPhysicalDeviceLimits, "standardSampleLocations" },
    .{ c.VkPhysicalDeviceSparseProperties, "residencyStandard2DBlockShape" },
    .{ c.VkPhysicalDeviceSparseProperties, "residencyStandard2DMultisampleBlockShape" },
    .{ c.VkPhysicalDeviceSparseProperties, "residencyStandard3DBlockShape" },
    .{ c.VkPhysicalDeviceSparseProperties, "residencyAlignedMipSize" },
    .{ c.VkPhysicalDeviceSparseProperties, "residencyNonResidentStrict" },
    .{ c.VkPhysicalDeviceVulkan11Properties, "deviceLUIDValid" },
    .{ c.VkPhysicalDeviceVulkan11Properties, "subgroupQuadOperationsInAllStages" },
    .{ c.VkPhysicalDeviceVulkan11Properties, "protectedNoFault" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderSignedZeroInfNanPreserveFloat16" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderSignedZeroInfNanPreserveFloat32" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderSignedZeroInfNanPreserveFloat64" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderDenormPreserveFloat16" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderDenormPreserveFloat32" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderDenormPreserveFloat64" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderDenormFlushToZeroFloat16" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderDenormFlushToZeroFloat32" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderDenormFlushToZeroFloat64" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTEFloat16" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTEFloat32" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTEFloat64" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTZFloat16" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTZFloat32" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTZFloat64" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderUniformBufferArrayNonUniformIndexingNative" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderSampledImageArrayNonUniformIndexingNative" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderStorageBufferArrayNonUniformIndexingNative" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderStorageImageArrayNonUniformIndexingNative" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "shaderInputAttachmentArrayNonUniformIndexingNative" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "robustBufferAccessUpdateAfterBind" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "quadDivergentImplicitLod" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "independentResolveNone" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "independentResolve" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "filterMinmaxSingleComponentFormats" },
    .{ c.VkPhysicalDeviceVulkan12Properties, "filterMinmaxImageComponentMapping" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct8BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct8BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct8BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct4x8BitPackedUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct4x8BitPackedSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct4x8BitPackedMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct16BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct16BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct16BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct32BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct32BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct32BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct64BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct64BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct64BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating8BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating8BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating16BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating16BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating32BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating32BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating64BitUnsignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating64BitSignedAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "storageTexelBufferOffsetSingleTexelAlignment" },
    .{ c.VkPhysicalDeviceVulkan13Properties, "uniformTexelBufferOffsetSingleTexelAlignment" },
    .{ c.VkPhysicalDeviceMaintenance5Properties, "earlyFragmentMultisampleCoverageAfterSampleCounting" },
    .{ c.VkPhysicalDeviceMaintenance5Properties, "earlyFragmentSampleMaskTestBeforeSampleCounting" },
    .{ c.VkPhysicalDeviceMaintenance5Properties, "depthStencilSwizzleOneSupport" },
    .{ c.VkPhysicalDeviceMaintenance5Properties, "polygonModePointSize" },
    .{ c.VkPhysicalDeviceMaintenance5Properties, "nonStrictSinglePixelWideLinesUseParallelogram" },
    .{ c.VkPhysicalDeviceMaintenance5Properties, "nonStrictWideLinesUseParallelogram" },
};
comptime {
    if (BooleanMembers.len != 75) @compileError("Exact Boolean inventory");
    if (@sizeOf(c.VkPhysicalDeviceProperties) != 824 or @alignOf(c.VkPhysicalDeviceProperties) != 8 or @typeInfo(c.VkPhysicalDeviceProperties).Struct.fields.len != 9) @compileError("Pinned VkPhysicalDeviceProperties layout");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "apiVersion") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).apiVersion)) != 4) @compileError("Pinned VkPhysicalDeviceProperties.apiVersion");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).apiVersion) != u32) @compileError("Pinned VkPhysicalDeviceProperties.apiVersion type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "driverVersion") != 4 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).driverVersion)) != 4) @compileError("Pinned VkPhysicalDeviceProperties.driverVersion");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).driverVersion) != u32) @compileError("Pinned VkPhysicalDeviceProperties.driverVersion type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "vendorID") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).vendorID)) != 4) @compileError("Pinned VkPhysicalDeviceProperties.vendorID");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).vendorID) != u32) @compileError("Pinned VkPhysicalDeviceProperties.vendorID type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "deviceID") != 12 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).deviceID)) != 4) @compileError("Pinned VkPhysicalDeviceProperties.deviceID");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).deviceID) != u32) @compileError("Pinned VkPhysicalDeviceProperties.deviceID type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "deviceType") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).deviceType)) != 4) @compileError("Pinned VkPhysicalDeviceProperties.deviceType");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).deviceType) != c.VkPhysicalDeviceType) @compileError("Pinned VkPhysicalDeviceProperties.deviceType type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "deviceName") != 20 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).deviceName)) != 256) @compileError("Pinned VkPhysicalDeviceProperties.deviceName");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).deviceName) != [256]u8) @compileError("Pinned VkPhysicalDeviceProperties.deviceName type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "pipelineCacheUUID") != 276 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).pipelineCacheUUID)) != 16) @compileError("Pinned VkPhysicalDeviceProperties.pipelineCacheUUID");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).pipelineCacheUUID) != [16]u8) @compileError("Pinned VkPhysicalDeviceProperties.pipelineCacheUUID type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "limits") != 296 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).limits)) != 504) @compileError("Pinned VkPhysicalDeviceProperties.limits");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).limits) != c.VkPhysicalDeviceLimits) @compileError("Pinned VkPhysicalDeviceProperties.limits type");
    if (@offsetOf(c.VkPhysicalDeviceProperties, "sparseProperties") != 800 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).sparseProperties)) != 20) @compileError("Pinned VkPhysicalDeviceProperties.sparseProperties");
    if (@TypeOf(@as(c.VkPhysicalDeviceProperties, undefined).sparseProperties) != c.VkPhysicalDeviceSparseProperties) @compileError("Pinned VkPhysicalDeviceProperties.sparseProperties type");
    if (@sizeOf(c.VkPhysicalDeviceLimits) != 504 or @alignOf(c.VkPhysicalDeviceLimits) != 8 or @typeInfo(c.VkPhysicalDeviceLimits).Struct.fields.len != 106) @compileError("Pinned VkPhysicalDeviceLimits layout");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxImageDimension1D") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimension1D)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimension1D");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimension1D) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimension1D type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxImageDimension2D") != 4 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimension2D)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimension2D");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimension2D) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimension2D type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxImageDimension3D") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimension3D)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimension3D");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimension3D) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimension3D type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxImageDimensionCube") != 12 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimensionCube)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimensionCube");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageDimensionCube) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxImageDimensionCube type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxImageArrayLayers") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageArrayLayers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxImageArrayLayers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxImageArrayLayers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxImageArrayLayers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTexelBufferElements") != 20 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTexelBufferElements)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTexelBufferElements");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTexelBufferElements) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTexelBufferElements type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxUniformBufferRange") != 24 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxUniformBufferRange)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxUniformBufferRange");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxUniformBufferRange) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxUniformBufferRange type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxStorageBufferRange") != 28 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxStorageBufferRange)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxStorageBufferRange");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxStorageBufferRange) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxStorageBufferRange type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPushConstantsSize") != 32 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPushConstantsSize)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPushConstantsSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPushConstantsSize) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPushConstantsSize type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxMemoryAllocationCount") != 36 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxMemoryAllocationCount)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxMemoryAllocationCount");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxMemoryAllocationCount) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxMemoryAllocationCount type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxSamplerAllocationCount") != 40 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSamplerAllocationCount)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxSamplerAllocationCount");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSamplerAllocationCount) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxSamplerAllocationCount type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "bufferImageGranularity") != 48 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).bufferImageGranularity)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.bufferImageGranularity");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).bufferImageGranularity) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.bufferImageGranularity type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "sparseAddressSpaceSize") != 56 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sparseAddressSpaceSize)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.sparseAddressSpaceSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sparseAddressSpaceSize) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.sparseAddressSpaceSize type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxBoundDescriptorSets") != 64 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxBoundDescriptorSets)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxBoundDescriptorSets");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxBoundDescriptorSets) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxBoundDescriptorSets type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPerStageDescriptorSamplers") != 68 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorSamplers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorSamplers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorSamplers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorSamplers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPerStageDescriptorUniformBuffers") != 72 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorUniformBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorUniformBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorUniformBuffers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorUniformBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPerStageDescriptorStorageBuffers") != 76 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorStorageBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorStorageBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorStorageBuffers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorStorageBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPerStageDescriptorSampledImages") != 80 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorSampledImages)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorSampledImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorSampledImages) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorSampledImages type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPerStageDescriptorStorageImages") != 84 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorStorageImages)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorStorageImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorStorageImages) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorStorageImages type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPerStageDescriptorInputAttachments") != 88 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorInputAttachments)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorInputAttachments");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageDescriptorInputAttachments) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageDescriptorInputAttachments type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxPerStageResources") != 92 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageResources)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageResources");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxPerStageResources) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxPerStageResources type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetSamplers") != 96 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetSamplers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetSamplers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetSamplers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetSamplers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetUniformBuffers") != 100 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetUniformBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetUniformBuffers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetUniformBuffersDynamic") != 104 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetUniformBuffersDynamic)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffersDynamic");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetUniformBuffersDynamic) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffersDynamic type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetStorageBuffers") != 108 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetStorageBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetStorageBuffers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetStorageBuffersDynamic") != 112 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetStorageBuffersDynamic)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffersDynamic");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetStorageBuffersDynamic) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffersDynamic type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetSampledImages") != 116 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetSampledImages)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetSampledImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetSampledImages) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetSampledImages type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetStorageImages") != 120 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetStorageImages)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetStorageImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetStorageImages) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetStorageImages type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDescriptorSetInputAttachments") != 124 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetInputAttachments)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetInputAttachments");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDescriptorSetInputAttachments) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDescriptorSetInputAttachments type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxVertexInputAttributes") != 128 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputAttributes)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputAttributes");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputAttributes) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputAttributes type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxVertexInputBindings") != 132 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputBindings)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputBindings");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputBindings) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputBindings type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxVertexInputAttributeOffset") != 136 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputAttributeOffset)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputAttributeOffset");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputAttributeOffset) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputAttributeOffset type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxVertexInputBindingStride") != 140 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputBindingStride)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputBindingStride");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexInputBindingStride) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexInputBindingStride type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxVertexOutputComponents") != 144 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexOutputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexOutputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxVertexOutputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxVertexOutputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationGenerationLevel") != 148 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationGenerationLevel)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationGenerationLevel");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationGenerationLevel) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationGenerationLevel type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationPatchSize") != 152 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationPatchSize)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationPatchSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationPatchSize) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationPatchSize type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationControlPerVertexInputComponents") != 156 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlPerVertexInputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlPerVertexInputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlPerVertexInputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlPerVertexInputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationControlPerVertexOutputComponents") != 160 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlPerVertexOutputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlPerVertexOutputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlPerVertexOutputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlPerVertexOutputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationControlPerPatchOutputComponents") != 164 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlPerPatchOutputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlPerPatchOutputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlPerPatchOutputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlPerPatchOutputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationControlTotalOutputComponents") != 168 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlTotalOutputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlTotalOutputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationControlTotalOutputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationControlTotalOutputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationEvaluationInputComponents") != 172 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationEvaluationInputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationEvaluationInputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationEvaluationInputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationEvaluationInputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTessellationEvaluationOutputComponents") != 176 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationEvaluationOutputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationEvaluationOutputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTessellationEvaluationOutputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTessellationEvaluationOutputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxGeometryShaderInvocations") != 180 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryShaderInvocations)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryShaderInvocations");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryShaderInvocations) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryShaderInvocations type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxGeometryInputComponents") != 184 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryInputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryInputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryInputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryInputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxGeometryOutputComponents") != 188 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryOutputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryOutputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryOutputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryOutputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxGeometryOutputVertices") != 192 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryOutputVertices)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryOutputVertices");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryOutputVertices) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryOutputVertices type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxGeometryTotalOutputComponents") != 196 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryTotalOutputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryTotalOutputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxGeometryTotalOutputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxGeometryTotalOutputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxFragmentInputComponents") != 200 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentInputComponents)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentInputComponents");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentInputComponents) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentInputComponents type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxFragmentOutputAttachments") != 204 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentOutputAttachments)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentOutputAttachments");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentOutputAttachments) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentOutputAttachments type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxFragmentDualSrcAttachments") != 208 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentDualSrcAttachments)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentDualSrcAttachments");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentDualSrcAttachments) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentDualSrcAttachments type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxFragmentCombinedOutputResources") != 212 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentCombinedOutputResources)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentCombinedOutputResources");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFragmentCombinedOutputResources) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxFragmentCombinedOutputResources type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxComputeSharedMemorySize") != 216 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeSharedMemorySize)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeSharedMemorySize");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeSharedMemorySize) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeSharedMemorySize type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxComputeWorkGroupCount") != 220 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeWorkGroupCount)) != 12) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeWorkGroupCount");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeWorkGroupCount) != [3]u32) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeWorkGroupCount type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxComputeWorkGroupInvocations") != 232 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeWorkGroupInvocations)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeWorkGroupInvocations");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeWorkGroupInvocations) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeWorkGroupInvocations type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxComputeWorkGroupSize") != 236 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeWorkGroupSize)) != 12) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeWorkGroupSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxComputeWorkGroupSize) != [3]u32) @compileError("Pinned VkPhysicalDeviceLimits.maxComputeWorkGroupSize type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "subPixelPrecisionBits") != 248 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).subPixelPrecisionBits)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.subPixelPrecisionBits");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).subPixelPrecisionBits) != u32) @compileError("Pinned VkPhysicalDeviceLimits.subPixelPrecisionBits type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "subTexelPrecisionBits") != 252 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).subTexelPrecisionBits)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.subTexelPrecisionBits");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).subTexelPrecisionBits) != u32) @compileError("Pinned VkPhysicalDeviceLimits.subTexelPrecisionBits type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "mipmapPrecisionBits") != 256 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).mipmapPrecisionBits)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.mipmapPrecisionBits");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).mipmapPrecisionBits) != u32) @compileError("Pinned VkPhysicalDeviceLimits.mipmapPrecisionBits type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDrawIndexedIndexValue") != 260 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDrawIndexedIndexValue)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDrawIndexedIndexValue");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDrawIndexedIndexValue) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDrawIndexedIndexValue type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxDrawIndirectCount") != 264 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDrawIndirectCount)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxDrawIndirectCount");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxDrawIndirectCount) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxDrawIndirectCount type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxSamplerLodBias") != 268 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSamplerLodBias)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxSamplerLodBias");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSamplerLodBias) != f32) @compileError("Pinned VkPhysicalDeviceLimits.maxSamplerLodBias type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxSamplerAnisotropy") != 272 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSamplerAnisotropy)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxSamplerAnisotropy");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSamplerAnisotropy) != f32) @compileError("Pinned VkPhysicalDeviceLimits.maxSamplerAnisotropy type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxViewports") != 276 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxViewports)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxViewports");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxViewports) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxViewports type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxViewportDimensions") != 280 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxViewportDimensions)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.maxViewportDimensions");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxViewportDimensions) != [2]u32) @compileError("Pinned VkPhysicalDeviceLimits.maxViewportDimensions type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "viewportBoundsRange") != 288 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).viewportBoundsRange)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.viewportBoundsRange");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).viewportBoundsRange) != [2]f32) @compileError("Pinned VkPhysicalDeviceLimits.viewportBoundsRange type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "viewportSubPixelBits") != 296 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).viewportSubPixelBits)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.viewportSubPixelBits");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).viewportSubPixelBits) != u32) @compileError("Pinned VkPhysicalDeviceLimits.viewportSubPixelBits type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "minMemoryMapAlignment") != 304 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minMemoryMapAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.minMemoryMapAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minMemoryMapAlignment) != usize) @compileError("Pinned VkPhysicalDeviceLimits.minMemoryMapAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "minTexelBufferOffsetAlignment") != 312 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minTexelBufferOffsetAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.minTexelBufferOffsetAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minTexelBufferOffsetAlignment) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.minTexelBufferOffsetAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "minUniformBufferOffsetAlignment") != 320 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minUniformBufferOffsetAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minUniformBufferOffsetAlignment) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "minStorageBufferOffsetAlignment") != 328 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minStorageBufferOffsetAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minStorageBufferOffsetAlignment) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "minTexelOffset") != 336 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minTexelOffset)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.minTexelOffset");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minTexelOffset) != i32) @compileError("Pinned VkPhysicalDeviceLimits.minTexelOffset type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTexelOffset") != 340 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTexelOffset)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTexelOffset");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTexelOffset) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTexelOffset type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "minTexelGatherOffset") != 344 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minTexelGatherOffset)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.minTexelGatherOffset");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minTexelGatherOffset) != i32) @compileError("Pinned VkPhysicalDeviceLimits.minTexelGatherOffset type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxTexelGatherOffset") != 348 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTexelGatherOffset)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxTexelGatherOffset");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxTexelGatherOffset) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxTexelGatherOffset type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "minInterpolationOffset") != 352 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minInterpolationOffset)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.minInterpolationOffset");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).minInterpolationOffset) != f32) @compileError("Pinned VkPhysicalDeviceLimits.minInterpolationOffset type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxInterpolationOffset") != 356 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxInterpolationOffset)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxInterpolationOffset");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxInterpolationOffset) != f32) @compileError("Pinned VkPhysicalDeviceLimits.maxInterpolationOffset type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "subPixelInterpolationOffsetBits") != 360 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).subPixelInterpolationOffsetBits)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.subPixelInterpolationOffsetBits");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).subPixelInterpolationOffsetBits) != u32) @compileError("Pinned VkPhysicalDeviceLimits.subPixelInterpolationOffsetBits type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxFramebufferWidth") != 364 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFramebufferWidth)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxFramebufferWidth");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFramebufferWidth) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxFramebufferWidth type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxFramebufferHeight") != 368 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFramebufferHeight)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxFramebufferHeight");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFramebufferHeight) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxFramebufferHeight type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxFramebufferLayers") != 372 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFramebufferLayers)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxFramebufferLayers");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxFramebufferLayers) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxFramebufferLayers type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "framebufferColorSampleCounts") != 376 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferColorSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.framebufferColorSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferColorSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.framebufferColorSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "framebufferDepthSampleCounts") != 380 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferDepthSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.framebufferDepthSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferDepthSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.framebufferDepthSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "framebufferStencilSampleCounts") != 384 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferStencilSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.framebufferStencilSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferStencilSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.framebufferStencilSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "framebufferNoAttachmentsSampleCounts") != 388 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferNoAttachmentsSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.framebufferNoAttachmentsSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).framebufferNoAttachmentsSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.framebufferNoAttachmentsSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxColorAttachments") != 392 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxColorAttachments)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxColorAttachments");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxColorAttachments) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxColorAttachments type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "sampledImageColorSampleCounts") != 396 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageColorSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageColorSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageColorSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageColorSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "sampledImageIntegerSampleCounts") != 400 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageIntegerSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageIntegerSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageIntegerSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageIntegerSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "sampledImageDepthSampleCounts") != 404 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageDepthSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageDepthSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageDepthSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageDepthSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "sampledImageStencilSampleCounts") != 408 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageStencilSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageStencilSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).sampledImageStencilSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.sampledImageStencilSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "storageImageSampleCounts") != 412 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).storageImageSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.storageImageSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).storageImageSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceLimits.storageImageSampleCounts type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxSampleMaskWords") != 416 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSampleMaskWords)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxSampleMaskWords");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxSampleMaskWords) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxSampleMaskWords type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "timestampComputeAndGraphics") != 420 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).timestampComputeAndGraphics)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.timestampComputeAndGraphics");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).timestampComputeAndGraphics) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceLimits.timestampComputeAndGraphics type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "timestampPeriod") != 424 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).timestampPeriod)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.timestampPeriod");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).timestampPeriod) != f32) @compileError("Pinned VkPhysicalDeviceLimits.timestampPeriod type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxClipDistances") != 428 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxClipDistances)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxClipDistances");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxClipDistances) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxClipDistances type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxCullDistances") != 432 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxCullDistances)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxCullDistances");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxCullDistances) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxCullDistances type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "maxCombinedClipAndCullDistances") != 436 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxCombinedClipAndCullDistances)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.maxCombinedClipAndCullDistances");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).maxCombinedClipAndCullDistances) != u32) @compileError("Pinned VkPhysicalDeviceLimits.maxCombinedClipAndCullDistances type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "discreteQueuePriorities") != 440 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).discreteQueuePriorities)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.discreteQueuePriorities");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).discreteQueuePriorities) != u32) @compileError("Pinned VkPhysicalDeviceLimits.discreteQueuePriorities type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "pointSizeRange") != 444 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).pointSizeRange)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.pointSizeRange");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).pointSizeRange) != [2]f32) @compileError("Pinned VkPhysicalDeviceLimits.pointSizeRange type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "lineWidthRange") != 452 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).lineWidthRange)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.lineWidthRange");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).lineWidthRange) != [2]f32) @compileError("Pinned VkPhysicalDeviceLimits.lineWidthRange type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "pointSizeGranularity") != 460 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).pointSizeGranularity)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.pointSizeGranularity");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).pointSizeGranularity) != f32) @compileError("Pinned VkPhysicalDeviceLimits.pointSizeGranularity type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "lineWidthGranularity") != 464 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).lineWidthGranularity)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.lineWidthGranularity");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).lineWidthGranularity) != f32) @compileError("Pinned VkPhysicalDeviceLimits.lineWidthGranularity type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "strictLines") != 468 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).strictLines)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.strictLines");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).strictLines) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceLimits.strictLines type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "standardSampleLocations") != 472 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).standardSampleLocations)) != 4) @compileError("Pinned VkPhysicalDeviceLimits.standardSampleLocations");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).standardSampleLocations) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceLimits.standardSampleLocations type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "optimalBufferCopyOffsetAlignment") != 480 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).optimalBufferCopyOffsetAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.optimalBufferCopyOffsetAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).optimalBufferCopyOffsetAlignment) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.optimalBufferCopyOffsetAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "optimalBufferCopyRowPitchAlignment") != 488 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).optimalBufferCopyRowPitchAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.optimalBufferCopyRowPitchAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).optimalBufferCopyRowPitchAlignment) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.optimalBufferCopyRowPitchAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceLimits, "nonCoherentAtomSize") != 496 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).nonCoherentAtomSize)) != 8) @compileError("Pinned VkPhysicalDeviceLimits.nonCoherentAtomSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceLimits, undefined).nonCoherentAtomSize) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceLimits.nonCoherentAtomSize type");
    if (@sizeOf(c.VkPhysicalDeviceSparseProperties) != 20 or @alignOf(c.VkPhysicalDeviceSparseProperties) != 4 or @typeInfo(c.VkPhysicalDeviceSparseProperties).Struct.fields.len != 5) @compileError("Pinned VkPhysicalDeviceSparseProperties layout");
    if (@offsetOf(c.VkPhysicalDeviceSparseProperties, "residencyStandard2DBlockShape") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyStandard2DBlockShape)) != 4) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyStandard2DBlockShape");
    if (@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyStandard2DBlockShape) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyStandard2DBlockShape type");
    if (@offsetOf(c.VkPhysicalDeviceSparseProperties, "residencyStandard2DMultisampleBlockShape") != 4 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyStandard2DMultisampleBlockShape)) != 4) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyStandard2DMultisampleBlockShape");
    if (@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyStandard2DMultisampleBlockShape) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyStandard2DMultisampleBlockShape type");
    if (@offsetOf(c.VkPhysicalDeviceSparseProperties, "residencyStandard3DBlockShape") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyStandard3DBlockShape)) != 4) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyStandard3DBlockShape");
    if (@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyStandard3DBlockShape) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyStandard3DBlockShape type");
    if (@offsetOf(c.VkPhysicalDeviceSparseProperties, "residencyAlignedMipSize") != 12 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyAlignedMipSize)) != 4) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyAlignedMipSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyAlignedMipSize) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyAlignedMipSize type");
    if (@offsetOf(c.VkPhysicalDeviceSparseProperties, "residencyNonResidentStrict") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyNonResidentStrict)) != 4) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyNonResidentStrict");
    if (@TypeOf(@as(c.VkPhysicalDeviceSparseProperties, undefined).residencyNonResidentStrict) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceSparseProperties.residencyNonResidentStrict type");
    if (@sizeOf(c.VkConformanceVersion) != 4 or @alignOf(c.VkConformanceVersion) != 1 or @typeInfo(c.VkConformanceVersion).Struct.fields.len != 4) @compileError("Pinned VkConformanceVersion layout");
    if (@offsetOf(c.VkConformanceVersion, "major") != 0 or @sizeOf(@TypeOf(@as(c.VkConformanceVersion, undefined).major)) != 1) @compileError("Pinned VkConformanceVersion.major");
    if (@TypeOf(@as(c.VkConformanceVersion, undefined).major) != u8) @compileError("Pinned VkConformanceVersion.major type");
    if (@offsetOf(c.VkConformanceVersion, "minor") != 1 or @sizeOf(@TypeOf(@as(c.VkConformanceVersion, undefined).minor)) != 1) @compileError("Pinned VkConformanceVersion.minor");
    if (@TypeOf(@as(c.VkConformanceVersion, undefined).minor) != u8) @compileError("Pinned VkConformanceVersion.minor type");
    if (@offsetOf(c.VkConformanceVersion, "subminor") != 2 or @sizeOf(@TypeOf(@as(c.VkConformanceVersion, undefined).subminor)) != 1) @compileError("Pinned VkConformanceVersion.subminor");
    if (@TypeOf(@as(c.VkConformanceVersion, undefined).subminor) != u8) @compileError("Pinned VkConformanceVersion.subminor type");
    if (@offsetOf(c.VkConformanceVersion, "patch") != 3 or @sizeOf(@TypeOf(@as(c.VkConformanceVersion, undefined).patch)) != 1) @compileError("Pinned VkConformanceVersion.patch");
    if (@TypeOf(@as(c.VkConformanceVersion, undefined).patch) != u8) @compileError("Pinned VkConformanceVersion.patch type");
    if (@sizeOf(c.VkPhysicalDeviceVulkan11Properties) != 112 or @alignOf(c.VkPhysicalDeviceVulkan11Properties) != 8 or @typeInfo(c.VkPhysicalDeviceVulkan11Properties).Struct.fields.len != 17) @compileError("Pinned VkPhysicalDeviceVulkan11Properties layout");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "sType") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).sType)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.sType");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).sType) != c.VkStructureType) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.sType type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "pNext") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).pNext)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.pNext");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).pNext) != ?*anyopaque) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.pNext type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "deviceUUID") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceUUID)) != 16) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceUUID");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceUUID) != [16]u8) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceUUID type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "driverUUID") != 32 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).driverUUID)) != 16) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.driverUUID");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).driverUUID) != [16]u8) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.driverUUID type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "deviceLUID") != 48 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceLUID)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceLUID");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceLUID) != [8]u8) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceLUID type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "deviceNodeMask") != 56 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceNodeMask)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceNodeMask");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceNodeMask) != u32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceNodeMask type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "deviceLUIDValid") != 60 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceLUIDValid)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceLUIDValid");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).deviceLUIDValid) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.deviceLUIDValid type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "subgroupSize") != 64 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupSize)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupSize) != u32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupSize type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "subgroupSupportedStages") != 68 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupSupportedStages)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupSupportedStages");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupSupportedStages) != c.VkShaderStageFlags) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupSupportedStages type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "subgroupSupportedOperations") != 72 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupSupportedOperations)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupSupportedOperations");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupSupportedOperations) != c.VkSubgroupFeatureFlags) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupSupportedOperations type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "subgroupQuadOperationsInAllStages") != 76 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupQuadOperationsInAllStages)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupQuadOperationsInAllStages");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).subgroupQuadOperationsInAllStages) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.subgroupQuadOperationsInAllStages type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "pointClippingBehavior") != 80 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).pointClippingBehavior)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.pointClippingBehavior");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).pointClippingBehavior) != c.VkPointClippingBehavior) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.pointClippingBehavior type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "maxMultiviewViewCount") != 84 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxMultiviewViewCount)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxMultiviewViewCount");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxMultiviewViewCount) != u32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxMultiviewViewCount type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "maxMultiviewInstanceIndex") != 88 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxMultiviewInstanceIndex)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxMultiviewInstanceIndex");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxMultiviewInstanceIndex) != u32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxMultiviewInstanceIndex type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "protectedNoFault") != 92 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).protectedNoFault)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.protectedNoFault");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).protectedNoFault) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.protectedNoFault type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "maxPerSetDescriptors") != 96 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxPerSetDescriptors)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxPerSetDescriptors");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxPerSetDescriptors) != u32) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxPerSetDescriptors type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan11Properties, "maxMemoryAllocationSize") != 104 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxMemoryAllocationSize)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxMemoryAllocationSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan11Properties, undefined).maxMemoryAllocationSize) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceVulkan11Properties.maxMemoryAllocationSize type");
    if (@sizeOf(c.VkPhysicalDeviceVulkan12Properties) != 736 or @alignOf(c.VkPhysicalDeviceVulkan12Properties) != 8 or @typeInfo(c.VkPhysicalDeviceVulkan12Properties).Struct.fields.len != 54) @compileError("Pinned VkPhysicalDeviceVulkan12Properties layout");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "sType") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).sType)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.sType");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).sType) != c.VkStructureType) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.sType type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "pNext") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).pNext)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.pNext");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).pNext) != ?*anyopaque) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.pNext type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "driverID") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).driverID)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.driverID");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).driverID) != c.VkDriverId) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.driverID type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "driverName") != 20 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).driverName)) != 256) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.driverName");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).driverName) != [256]u8) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.driverName type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "driverInfo") != 276 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).driverInfo)) != 256) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.driverInfo");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).driverInfo) != [256]u8) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.driverInfo type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "conformanceVersion") != 532 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).conformanceVersion)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.conformanceVersion");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).conformanceVersion) != c.VkConformanceVersion) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.conformanceVersion type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "denormBehaviorIndependence") != 536 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).denormBehaviorIndependence)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.denormBehaviorIndependence");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).denormBehaviorIndependence) != c.VkShaderFloatControlsIndependence) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.denormBehaviorIndependence type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "roundingModeIndependence") != 540 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).roundingModeIndependence)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.roundingModeIndependence");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).roundingModeIndependence) != c.VkShaderFloatControlsIndependence) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.roundingModeIndependence type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderSignedZeroInfNanPreserveFloat16") != 544 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSignedZeroInfNanPreserveFloat16)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat16");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSignedZeroInfNanPreserveFloat16) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat16 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderSignedZeroInfNanPreserveFloat32") != 548 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSignedZeroInfNanPreserveFloat32)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat32");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSignedZeroInfNanPreserveFloat32) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat32 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderSignedZeroInfNanPreserveFloat64") != 552 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSignedZeroInfNanPreserveFloat64)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat64");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSignedZeroInfNanPreserveFloat64) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat64 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderDenormPreserveFloat16") != 556 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormPreserveFloat16)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat16");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormPreserveFloat16) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat16 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderDenormPreserveFloat32") != 560 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormPreserveFloat32)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat32");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormPreserveFloat32) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat32 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderDenormPreserveFloat64") != 564 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormPreserveFloat64)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat64");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormPreserveFloat64) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat64 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderDenormFlushToZeroFloat16") != 568 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormFlushToZeroFloat16)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat16");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormFlushToZeroFloat16) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat16 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderDenormFlushToZeroFloat32") != 572 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormFlushToZeroFloat32)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat32");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormFlushToZeroFloat32) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat32 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderDenormFlushToZeroFloat64") != 576 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormFlushToZeroFloat64)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat64");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderDenormFlushToZeroFloat64) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat64 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTEFloat16") != 580 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTEFloat16)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat16");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTEFloat16) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat16 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTEFloat32") != 584 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTEFloat32)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat32");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTEFloat32) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat32 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTEFloat64") != 588 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTEFloat64)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat64");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTEFloat64) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat64 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTZFloat16") != 592 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTZFloat16)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat16");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTZFloat16) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat16 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTZFloat32") != 596 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTZFloat32)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat32");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTZFloat32) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat32 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderRoundingModeRTZFloat64") != 600 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTZFloat64)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat64");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderRoundingModeRTZFloat64) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat64 type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxUpdateAfterBindDescriptorsInAllPools") != 604 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxUpdateAfterBindDescriptorsInAllPools)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxUpdateAfterBindDescriptorsInAllPools");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxUpdateAfterBindDescriptorsInAllPools) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxUpdateAfterBindDescriptorsInAllPools type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderUniformBufferArrayNonUniformIndexingNative") != 608 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderUniformBufferArrayNonUniformIndexingNative)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderUniformBufferArrayNonUniformIndexingNative");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderUniformBufferArrayNonUniformIndexingNative) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderUniformBufferArrayNonUniformIndexingNative type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderSampledImageArrayNonUniformIndexingNative") != 612 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSampledImageArrayNonUniformIndexingNative)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSampledImageArrayNonUniformIndexingNative");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderSampledImageArrayNonUniformIndexingNative) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderSampledImageArrayNonUniformIndexingNative type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderStorageBufferArrayNonUniformIndexingNative") != 616 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderStorageBufferArrayNonUniformIndexingNative)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderStorageBufferArrayNonUniformIndexingNative");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderStorageBufferArrayNonUniformIndexingNative) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderStorageBufferArrayNonUniformIndexingNative type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderStorageImageArrayNonUniformIndexingNative") != 620 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderStorageImageArrayNonUniformIndexingNative)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderStorageImageArrayNonUniformIndexingNative");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderStorageImageArrayNonUniformIndexingNative) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderStorageImageArrayNonUniformIndexingNative type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "shaderInputAttachmentArrayNonUniformIndexingNative") != 624 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderInputAttachmentArrayNonUniformIndexingNative)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderInputAttachmentArrayNonUniformIndexingNative");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).shaderInputAttachmentArrayNonUniformIndexingNative) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.shaderInputAttachmentArrayNonUniformIndexingNative type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "robustBufferAccessUpdateAfterBind") != 628 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).robustBufferAccessUpdateAfterBind)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.robustBufferAccessUpdateAfterBind");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).robustBufferAccessUpdateAfterBind) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.robustBufferAccessUpdateAfterBind type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "quadDivergentImplicitLod") != 632 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).quadDivergentImplicitLod)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.quadDivergentImplicitLod");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).quadDivergentImplicitLod) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.quadDivergentImplicitLod type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxPerStageDescriptorUpdateAfterBindSamplers") != 636 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindSamplers)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindSamplers");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindSamplers) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindSamplers type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxPerStageDescriptorUpdateAfterBindUniformBuffers") != 640 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindUniformBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindUniformBuffers) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxPerStageDescriptorUpdateAfterBindStorageBuffers") != 644 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindStorageBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindStorageBuffers) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxPerStageDescriptorUpdateAfterBindSampledImages") != 648 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindSampledImages)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindSampledImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindSampledImages) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindSampledImages type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxPerStageDescriptorUpdateAfterBindStorageImages") != 652 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindStorageImages)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindStorageImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindStorageImages) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindStorageImages type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxPerStageDescriptorUpdateAfterBindInputAttachments") != 656 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindInputAttachments)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindInputAttachments");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageDescriptorUpdateAfterBindInputAttachments) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindInputAttachments type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxPerStageUpdateAfterBindResources") != 660 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageUpdateAfterBindResources)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageUpdateAfterBindResources");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxPerStageUpdateAfterBindResources) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxPerStageUpdateAfterBindResources type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindSamplers") != 664 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindSamplers)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSamplers");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindSamplers) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSamplers type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindUniformBuffers") != 668 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindUniformBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindUniformBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindUniformBuffers) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindUniformBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindUniformBuffersDynamic") != 672 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindUniformBuffersDynamic)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindUniformBuffersDynamic) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindStorageBuffers") != 676 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindStorageBuffers)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffers");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindStorageBuffers) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffers type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindStorageBuffersDynamic") != 680 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindStorageBuffersDynamic)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindStorageBuffersDynamic) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindSampledImages") != 684 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindSampledImages)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSampledImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindSampledImages) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSampledImages type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindStorageImages") != 688 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindStorageImages)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageImages");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindStorageImages) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageImages type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxDescriptorSetUpdateAfterBindInputAttachments") != 692 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindInputAttachments)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindInputAttachments");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxDescriptorSetUpdateAfterBindInputAttachments) != u32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindInputAttachments type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "supportedDepthResolveModes") != 696 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).supportedDepthResolveModes)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.supportedDepthResolveModes");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).supportedDepthResolveModes) != c.VkResolveModeFlags) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.supportedDepthResolveModes type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "supportedStencilResolveModes") != 700 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).supportedStencilResolveModes)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.supportedStencilResolveModes");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).supportedStencilResolveModes) != c.VkResolveModeFlags) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.supportedStencilResolveModes type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "independentResolveNone") != 704 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).independentResolveNone)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.independentResolveNone");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).independentResolveNone) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.independentResolveNone type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "independentResolve") != 708 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).independentResolve)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.independentResolve");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).independentResolve) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.independentResolve type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "filterMinmaxSingleComponentFormats") != 712 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).filterMinmaxSingleComponentFormats)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.filterMinmaxSingleComponentFormats");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).filterMinmaxSingleComponentFormats) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.filterMinmaxSingleComponentFormats type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "filterMinmaxImageComponentMapping") != 716 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).filterMinmaxImageComponentMapping)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.filterMinmaxImageComponentMapping");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).filterMinmaxImageComponentMapping) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.filterMinmaxImageComponentMapping type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "maxTimelineSemaphoreValueDifference") != 720 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxTimelineSemaphoreValueDifference)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxTimelineSemaphoreValueDifference");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).maxTimelineSemaphoreValueDifference) != u64) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.maxTimelineSemaphoreValueDifference type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan12Properties, "framebufferIntegerColorSampleCounts") != 728 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).framebufferIntegerColorSampleCounts)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.framebufferIntegerColorSampleCounts");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan12Properties, undefined).framebufferIntegerColorSampleCounts) != c.VkSampleCountFlags) @compileError("Pinned VkPhysicalDeviceVulkan12Properties.framebufferIntegerColorSampleCounts type");
    if (@sizeOf(c.VkPhysicalDeviceVulkan13Properties) != 216 or @alignOf(c.VkPhysicalDeviceVulkan13Properties) != 8 or @typeInfo(c.VkPhysicalDeviceVulkan13Properties).Struct.fields.len != 47) @compileError("Pinned VkPhysicalDeviceVulkan13Properties layout");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "sType") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).sType)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.sType");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).sType) != c.VkStructureType) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.sType type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "pNext") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).pNext)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.pNext");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).pNext) != ?*anyopaque) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.pNext type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "minSubgroupSize") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).minSubgroupSize)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.minSubgroupSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).minSubgroupSize) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.minSubgroupSize type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxSubgroupSize") != 20 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxSubgroupSize)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxSubgroupSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxSubgroupSize) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxSubgroupSize type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxComputeWorkgroupSubgroups") != 24 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxComputeWorkgroupSubgroups)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxComputeWorkgroupSubgroups");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxComputeWorkgroupSubgroups) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxComputeWorkgroupSubgroups type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "requiredSubgroupSizeStages") != 28 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).requiredSubgroupSizeStages)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.requiredSubgroupSizeStages");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).requiredSubgroupSizeStages) != c.VkShaderStageFlags) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.requiredSubgroupSizeStages type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxInlineUniformBlockSize") != 32 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxInlineUniformBlockSize)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxInlineUniformBlockSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxInlineUniformBlockSize) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxInlineUniformBlockSize type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxPerStageDescriptorInlineUniformBlocks") != 36 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxPerStageDescriptorInlineUniformBlocks)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxPerStageDescriptorInlineUniformBlocks");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxPerStageDescriptorInlineUniformBlocks) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxPerStageDescriptorInlineUniformBlocks type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks") != 40 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxDescriptorSetInlineUniformBlocks") != 44 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxDescriptorSetInlineUniformBlocks)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxDescriptorSetInlineUniformBlocks");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxDescriptorSetInlineUniformBlocks) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxDescriptorSetInlineUniformBlocks type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxDescriptorSetUpdateAfterBindInlineUniformBlocks") != 48 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxDescriptorSetUpdateAfterBindInlineUniformBlocks)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxDescriptorSetUpdateAfterBindInlineUniformBlocks");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxDescriptorSetUpdateAfterBindInlineUniformBlocks) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxDescriptorSetUpdateAfterBindInlineUniformBlocks type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxInlineUniformTotalSize") != 52 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxInlineUniformTotalSize)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxInlineUniformTotalSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxInlineUniformTotalSize) != u32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxInlineUniformTotalSize type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct8BitUnsignedAccelerated") != 56 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct8BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct8BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct8BitSignedAccelerated") != 60 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct8BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct8BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct8BitMixedSignednessAccelerated") != 64 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct8BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct8BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct4x8BitPackedUnsignedAccelerated") != 68 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct4x8BitPackedUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct4x8BitPackedUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct4x8BitPackedSignedAccelerated") != 72 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct4x8BitPackedSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct4x8BitPackedSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct4x8BitPackedMixedSignednessAccelerated") != 76 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct4x8BitPackedMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct4x8BitPackedMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct16BitUnsignedAccelerated") != 80 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct16BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct16BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct16BitSignedAccelerated") != 84 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct16BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct16BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct16BitMixedSignednessAccelerated") != 88 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct16BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct16BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct32BitUnsignedAccelerated") != 92 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct32BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct32BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct32BitSignedAccelerated") != 96 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct32BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct32BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct32BitMixedSignednessAccelerated") != 100 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct32BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct32BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct64BitUnsignedAccelerated") != 104 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct64BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct64BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct64BitSignedAccelerated") != 108 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct64BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct64BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProduct64BitMixedSignednessAccelerated") != 112 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct64BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProduct64BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating8BitUnsignedAccelerated") != 116 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating8BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating8BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating8BitSignedAccelerated") != 120 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating8BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating8BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated") != 124 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated") != 128 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated") != 132 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated") != 136 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating16BitUnsignedAccelerated") != 140 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating16BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating16BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating16BitSignedAccelerated") != 144 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating16BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating16BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated") != 148 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating32BitUnsignedAccelerated") != 152 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating32BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating32BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating32BitSignedAccelerated") != 156 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating32BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating32BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated") != 160 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating64BitUnsignedAccelerated") != 164 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating64BitUnsignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating64BitUnsignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating64BitSignedAccelerated") != 168 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating64BitSignedAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitSignedAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating64BitSignedAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitSignedAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated") != 172 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "storageTexelBufferOffsetAlignmentBytes") != 176 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).storageTexelBufferOffsetAlignmentBytes)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetAlignmentBytes");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).storageTexelBufferOffsetAlignmentBytes) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetAlignmentBytes type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "storageTexelBufferOffsetSingleTexelAlignment") != 184 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).storageTexelBufferOffsetSingleTexelAlignment)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetSingleTexelAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).storageTexelBufferOffsetSingleTexelAlignment) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetSingleTexelAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "uniformTexelBufferOffsetAlignmentBytes") != 192 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).uniformTexelBufferOffsetAlignmentBytes)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetAlignmentBytes");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).uniformTexelBufferOffsetAlignmentBytes) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetAlignmentBytes type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "uniformTexelBufferOffsetSingleTexelAlignment") != 200 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).uniformTexelBufferOffsetSingleTexelAlignment)) != 4) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetSingleTexelAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).uniformTexelBufferOffsetSingleTexelAlignment) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetSingleTexelAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceVulkan13Properties, "maxBufferSize") != 208 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxBufferSize)) != 8) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxBufferSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceVulkan13Properties, undefined).maxBufferSize) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceVulkan13Properties.maxBufferSize type");
    if (@sizeOf(c.VkPhysicalDeviceRobustness2PropertiesEXT) != 32 or @alignOf(c.VkPhysicalDeviceRobustness2PropertiesEXT) != 8 or @typeInfo(c.VkPhysicalDeviceRobustness2PropertiesEXT).Struct.fields.len != 4) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT layout");
    if (@offsetOf(c.VkPhysicalDeviceRobustness2PropertiesEXT, "sType") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).sType)) != 4) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.sType");
    if (@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).sType) != c.VkStructureType) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.sType type");
    if (@offsetOf(c.VkPhysicalDeviceRobustness2PropertiesEXT, "pNext") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).pNext)) != 8) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.pNext");
    if (@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).pNext) != ?*anyopaque) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.pNext type");
    if (@offsetOf(c.VkPhysicalDeviceRobustness2PropertiesEXT, "robustStorageBufferAccessSizeAlignment") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).robustStorageBufferAccessSizeAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.robustStorageBufferAccessSizeAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).robustStorageBufferAccessSizeAlignment) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.robustStorageBufferAccessSizeAlignment type");
    if (@offsetOf(c.VkPhysicalDeviceRobustness2PropertiesEXT, "robustUniformBufferAccessSizeAlignment") != 24 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).robustUniformBufferAccessSizeAlignment)) != 8) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.robustUniformBufferAccessSizeAlignment");
    if (@TypeOf(@as(c.VkPhysicalDeviceRobustness2PropertiesEXT, undefined).robustUniformBufferAccessSizeAlignment) != c.VkDeviceSize) @compileError("Pinned VkPhysicalDeviceRobustness2PropertiesEXT.robustUniformBufferAccessSizeAlignment type");
    if (@sizeOf(c.VkPhysicalDeviceMaintenance5Properties) != 40 or @alignOf(c.VkPhysicalDeviceMaintenance5Properties) != 8 or @typeInfo(c.VkPhysicalDeviceMaintenance5Properties).Struct.fields.len != 8) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties layout");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "sType") != 0 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).sType)) != 4) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.sType");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).sType) != c.VkStructureType) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.sType type");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "pNext") != 8 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).pNext)) != 8) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.pNext");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).pNext) != ?*anyopaque) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.pNext type");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "earlyFragmentMultisampleCoverageAfterSampleCounting") != 16 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).earlyFragmentMultisampleCoverageAfterSampleCounting)) != 4) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.earlyFragmentMultisampleCoverageAfterSampleCounting");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).earlyFragmentMultisampleCoverageAfterSampleCounting) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.earlyFragmentMultisampleCoverageAfterSampleCounting type");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "earlyFragmentSampleMaskTestBeforeSampleCounting") != 20 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).earlyFragmentSampleMaskTestBeforeSampleCounting)) != 4) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.earlyFragmentSampleMaskTestBeforeSampleCounting");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).earlyFragmentSampleMaskTestBeforeSampleCounting) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.earlyFragmentSampleMaskTestBeforeSampleCounting type");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "depthStencilSwizzleOneSupport") != 24 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).depthStencilSwizzleOneSupport)) != 4) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.depthStencilSwizzleOneSupport");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).depthStencilSwizzleOneSupport) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.depthStencilSwizzleOneSupport type");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "polygonModePointSize") != 28 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).polygonModePointSize)) != 4) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.polygonModePointSize");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).polygonModePointSize) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.polygonModePointSize type");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "nonStrictSinglePixelWideLinesUseParallelogram") != 32 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).nonStrictSinglePixelWideLinesUseParallelogram)) != 4) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.nonStrictSinglePixelWideLinesUseParallelogram");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).nonStrictSinglePixelWideLinesUseParallelogram) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.nonStrictSinglePixelWideLinesUseParallelogram type");
    if (@offsetOf(c.VkPhysicalDeviceMaintenance5Properties, "nonStrictWideLinesUseParallelogram") != 36 or @sizeOf(@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).nonStrictWideLinesUseParallelogram)) != 4) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.nonStrictWideLinesUseParallelogram");
    if (@TypeOf(@as(c.VkPhysicalDeviceMaintenance5Properties, undefined).nonStrictWideLinesUseParallelogram) != c.VkBool32) @compileError("Pinned VkPhysicalDeviceMaintenance5Properties.nonStrictWideLinesUseParallelogram type");
}

fn payload_size(tag: u32) !usize {
    return switch (tag) {
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES => 116,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES => 744,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES => 192,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT => 16,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR => 24,
        else => error.Invalid,
    };
}
fn reply_size(tags: []const u32) !usize {
    if (tags.len > MaxNodes) return error.Invalid;
    var size: usize = 892;
    for (tags, 0..) |tag, index| {
        size += 12 + try payload_size(tag);
        for (tags[0..index]) |previous| if (previous == tag) return error.Invalid;
    }
    return size;
}
/// Encode a partial typed query. [in] physical_id nonzero translated ID and tags immutable borrowed array0..5.
/// Returns owned packet or Invalid for zero identity, unknown/duplicate/excess tags.
/// No allocation, pointer retention, shared state or native addressing; caller validates negotiated profile separately.
pub fn query(physical_id: u64, tags: []const u32) !render.writer_t {
    if (physical_id == 0) return error.Invalid;
    _ = try reply_size(tags);
    var writer = render.writer_t{};
    // Complete query ceiling96 fits owned writer before any append.
    writer.header(148, physical_id) catch unreachable;
    writer.put(u64, 1) catch unreachable;
    writer.put(u32, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) catch unreachable;
    for (tags) |tag| {
        writer.put(u64, 1) catch unreachable;
        writer.put(u32, tag) catch unreachable;
    }
    writer.put(u64, 0) catch unreachable;
    std.debug.assert(writer.used == 36 + 12 * tags.len);
    return writer;
}
const reader_t = struct {
    bytes: []const u8,
    used: usize = 0,
    fn take(self: *reader_t, comptime scalar_t: type) scalar_t {
        // decode establishes the exact full prefix before any access.
        const value = std.mem.readInt(scalar_t, self.bytes[self.used..][0..@sizeOf(scalar_t)], .little);
        self.used += @sizeOf(scalar_t);
        return value;
    }
    fn native(self: *reader_t, comptime value_t: type, output: *value_t) !void {
        switch (@typeInfo(value_t)) {
            .Int => |integer| {
                output.* = self.take(value_t);
                if (integer.bits == 8) self.used += 3; // Pinned scalar byte stride4; reserved bytes ignored.
            },
            .Float => {
                output.* = @bitCast(self.take(u32));
                if (!std.math.isFinite(output.*)) return error.Corrupt;
            },
            .Array => |array| {
                if (self.take(u64) != array.len) return error.Corrupt;
                if (@sizeOf(array.child) == 1) {
                    @memcpy(std.mem.asBytes(output), self.bytes[self.used..][0..array.len]);
                    self.used += (array.len + 3) & ~@as(usize, 3);
                } else for (output) |*element| try self.native(array.child, element);
            },
            .Struct => |structure| {
                inline for (structure.fields) |field| {
                    if (comptime !std.mem.eql(u8, field.name, "sType") and !std.mem.eql(u8, field.name, "pNext")) {
                        const member = &@field(output, field.name);
                        try self.native(field.type, member);
                        const boolean = comptime boolean: {
                            @setEvalBranchQuota(100000);
                            for (BooleanMembers) |item| if (item[0] == value_t and std.mem.eql(u8, item[1], field.name)) break :boolean true;
                            break :boolean false;
                        };
                        if (boolean and member.* > 1) return error.Corrupt;
                        if (comptime std.mem.eql(u8, field.name, "deviceName") or std.mem.eql(u8, field.name, "driverName") or std.mem.eql(u8, field.name, "driverInfo")) {
                            const bytes = std.mem.asBytes(member);
                            const end = std.mem.indexOfScalar(u8, bytes, 0) orelse return error.Corrupt;
                            @memset(bytes[end..], 0);
                        }
                    }
                }
            },
            else => @compileError("Unsupported pinned payload type"),
        }
    }
};
/// Validate an owned typed payload. [in] value nonnull immutable accessible SDK object, borrowed for call.
/// Returns Corrupt for invalid Boolean/nonfinite float/unnormalized bounded string; never reads native padding.
/// No allocation, retained pointers or shared state; headers are checked by validate_result separately.
pub fn validate_payload(comptime value_t: type, value: *const value_t) !void {
    switch (@typeInfo(value_t)) {
        .Int => {},
        .Float => if (!std.math.isFinite(value.*)) return error.Corrupt,
        .Array => |array| for (value) |*element| try validate_payload(array.child, element),
        .Struct => |structure| {
            inline for (structure.fields) |field| {
                if (comptime !std.mem.eql(u8, field.name, "sType") and !std.mem.eql(u8, field.name, "pNext")) {
                    const member = &@field(value, field.name);
                    try validate_payload(field.type, member);
                    const boolean = comptime boolean: {
                        @setEvalBranchQuota(100000);
                        for (BooleanMembers) |item| if (item[0] == value_t and std.mem.eql(u8, item[1], field.name)) break :boolean true;
                        break :boolean false;
                    };
                    if (boolean and member.* > 1) return error.Corrupt;
                    if (comptime std.mem.eql(u8, field.name, "deviceName") or std.mem.eql(u8, field.name, "driverName") or std.mem.eql(u8, field.name, "driverInfo")) {
                        const bytes = std.mem.asBytes(member);
                        const end = std.mem.indexOfScalar(u8, bytes, 0) orelse return error.Corrupt;
                        for (bytes[end..]) |byte| if (byte != 0) return error.Corrupt;
                    }
                }
            }
        },
        else => @compileError("Unsupported pinned payload type"),
    }
}
/// Validate the complete owned result. [in] tags/value nonnull immutable accessible borrowed data.
/// Returns Invalid for profile/count/tag/header mismatch and Corrupt for invalid named payload data.
/// No allocation, native padding read, pointer retention or shared state; output publication is a separate transaction.
pub fn validate_result(tags: []const u32, value: *const result_t) !void {
    _ = try reply_size(tags);
    if (value.count != tags.len) return error.Invalid;
    for (tags, value.nodes[0..tags.len]) |tag, *node| {
        if (node.type_tag != tag) return error.Invalid;
        switch (tag) {
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES => try validate_node(c.VkPhysicalDeviceVulkan11Properties, &node.data.core11, tag),
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES => try validate_node(c.VkPhysicalDeviceVulkan12Properties, &node.data.core12, tag),
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES => try validate_node(c.VkPhysicalDeviceVulkan13Properties, &node.data.core13, tag),
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT => try validate_node(c.VkPhysicalDeviceRobustness2PropertiesEXT, &node.data.robustness, tag),
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR => try validate_node(c.VkPhysicalDeviceMaintenance5Properties, &node.data.maintenance, tag),
            else => unreachable, // Exact profile validated above.
        }
    }
    try validate_payload(c.VkPhysicalDeviceProperties, &value.properties);
}
fn validate_node(comptime value_t: type, value: *const value_t, tag: u32) !void {
    if (value.sType != tag or value.pNext != null) return error.Invalid;
    try validate_payload(value_t, value);
}
/// Decode one initialized reply prefix. [in] bytes/tags accessible immutable borrowed slices.
/// Returns owned zero-initialized SDK data, Bounds for any short prefix, Invalid for request profile,
/// Corrupt for wire shape/array extent/Boolean/nonfinite float/unbounded string. Extra scratch is ignored.
/// No allocation, partial result exposure, retained pointers or shared state. Recognition does not advertise support.
pub fn decode(bytes: []const u8, tags: []const u32) !result_t {
    const expected = try reply_size(tags);
    if (bytes.len < expected) return error.Bounds;
    var reader = reader_t{ .bytes = bytes[0..expected] };
    if (reader.take(u32) != 148 or reader.take(u64) != 1 or reader.take(u32) != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) return error.Corrupt;
    var result: result_t = undefined;
    @memset(std.mem.asBytes(&result), 0);
    result.count = @intCast(tags.len);
    for (tags, 0..) |tag, index| {
        if (reader.take(u64) != 1 or reader.take(u32) != tag) return error.Corrupt;
        result.nodes[index].type_tag = tag;
    }
    if (reader.take(u64) != 0) return error.Corrupt;
    var remaining = tags.len;
    while (remaining != 0) {
        remaining -= 1;
        const node = &result.nodes[remaining];
        switch (node.type_tag) {
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES => {
                try reader.native(c.VkPhysicalDeviceVulkan11Properties, &node.data.core11);
                node.data.core11.sType = node.type_tag;
            },
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES => {
                try reader.native(c.VkPhysicalDeviceVulkan12Properties, &node.data.core12);
                node.data.core12.sType = node.type_tag;
            },
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES => {
                try reader.native(c.VkPhysicalDeviceVulkan13Properties, &node.data.core13);
                node.data.core13.sType = node.type_tag;
            },
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT => {
                try reader.native(c.VkPhysicalDeviceRobustness2PropertiesEXT, &node.data.robustness);
                node.data.robustness.sType = node.type_tag;
            },
            else => {
                try reader.native(c.VkPhysicalDeviceMaintenance5Properties, &node.data.maintenance);
                node.data.maintenance.sType = node.type_tag;
            },
        }
    }
    try reader.native(c.VkPhysicalDeviceProperties, &result.properties);
    std.debug.assert(reader.used == expected);
    return result;
}

// Test-only fixtures.
extern fn venus_properties_test_query([*]const u32, usize, [*]u8) usize;
extern fn venus_properties_test_fixture([*]const u32, usize, *c.VkPhysicalDeviceProperties, [*]data_t) void;
extern fn venus_properties_test_encode([*]const u32, usize, *const c.VkPhysicalDeviceProperties, [*]const data_t, [*]u8) usize;
fn fixture_check(tags: []const u32) !void {
    var core: c.VkPhysicalDeviceProperties = undefined;
    var nodes: [5]data_t = undefined;
    venus_properties_test_fixture(tags.ptr, tags.len, &core, &nodes);
    var bytes: [4096]u8 = undefined;
    const request = try @call(.never_inline, query, .{ 7, tags });
    const request_size = venus_properties_test_query(tags.ptr, tags.len, &bytes);
    try std.testing.expectEqual(@as(usize, 36 + 12 * tags.len), request_size);
    try std.testing.expectEqualSlices(u8, bytes[0..request_size], request.bytes[0..request.used]);
    const size = venus_properties_test_encode(tags.ptr, tags.len, &core, &nodes, &bytes);
    try std.testing.expectEqual(try reply_size(tags), size);
    const result = try decode(bytes[0..size], tags);
    try validate_result(tags, &result);
    try std.testing.expectEqualDeep(core, result.properties);
    for (tags, 0..) |tag, index| {
        switch (tag) {
            KnownTags[0] => try std.testing.expectEqualDeep(nodes[index].core11, result.nodes[index].data.core11),
            KnownTags[1] => try std.testing.expectEqualDeep(nodes[index].core12, result.nodes[index].data.core12),
            KnownTags[2] => try std.testing.expectEqualDeep(nodes[index].core13, result.nodes[index].data.core13),
            KnownTags[3] => try std.testing.expectEqualDeep(nodes[index].robustness, result.nodes[index].data.robustness),
            else => try std.testing.expectEqualDeep(nodes[index].maintenance, result.nodes[index].data.maintenance),
        }
    }
    for (0..size) |prefix| try std.testing.expectError(error.Bounds, decode(bytes[0..prefix], tags));
    @memset(bytes[size..], 0xa5);
    const scratch = try decode(&bytes, tags);
    try std.testing.expectEqualDeep(result.properties, scratch.properties);
    for (tags, 0..) |tag, index| {
        switch (tag) {
            KnownTags[0] => try std.testing.expectEqualDeep(result.nodes[index].data.core11, scratch.nodes[index].data.core11),
            KnownTags[1] => try std.testing.expectEqualDeep(result.nodes[index].data.core12, scratch.nodes[index].data.core12),
            KnownTags[2] => try std.testing.expectEqualDeep(result.nodes[index].data.core13, scratch.nodes[index].data.core13),
            KnownTags[3] => try std.testing.expectEqualDeep(result.nodes[index].data.robustness, scratch.nodes[index].data.robustness),
            else => try std.testing.expectEqualDeep(result.nodes[index].data.maintenance, scratch.nodes[index].data.maintenance),
        }
    }
}
fn fixture_profiles(tags: *[5]u32, count: usize, visited: *usize) !void {
    try fixture_check(tags[0..count]);
    visited.* += 1;
    if (count == 5) return;
    for (KnownTags) |tag| {
        if (std.mem.indexOfScalar(u32, tags[0..count], tag) != null) continue;
        tags[count] = tag;
        try fixture_profiles(tags, count + 1, visited);
    }
}
test "all326 ordered Properties2 subsets match independent native encoders and every truncation" {
    @setEvalBranchQuota(100000);
    var tags: [5]u32 = undefined;
    var visited: usize = 0;
    try fixture_profiles(&tags, 0, &visited);
    try std.testing.expectEqual(@as(usize, 326), visited);
}
test "Properties2 invalid profiles and each metadata word reject" {
    @setEvalBranchQuota(100000);
    try std.testing.expectError(error.Invalid, query(0, &.{}));
    for ([_][]const u32{ &.{0}, &.{ KnownTags[0], KnownTags[0] }, &.{ KnownTags[0], KnownTags[1], KnownTags[2], KnownTags[3], KnownTags[4], 0 } }) |tags| {
        try std.testing.expectError(error.Invalid, query(7, tags));
        try std.testing.expectError(error.Invalid, decode(&.{}, tags));
    }
    var core: c.VkPhysicalDeviceProperties = undefined;
    var nodes: [5]data_t = undefined;
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    var bytes: [4096]u8 = undefined;
    const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
    for (0..21) |word| {
        var bad = bytes;
        std.mem.writeInt(u32, bad[word * 4 ..][0..4], 99, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
}

test "Properties2 all75 typed Boolean members reject2 and canonical result headers are required" {
    @setEvalBranchQuota(100000);
    var core: c.VkPhysicalDeviceProperties = undefined;
    var nodes: [5]data_t = undefined;
    var bytes: [4096]u8 = undefined;
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.timestampComputeAndGraphics = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.limits), &core.limits));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.strictLines = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.limits), &core.limits));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.standardSampleLocations = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.limits), &core.limits));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyStandard2DBlockShape = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.sparseProperties), &core.sparseProperties));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyStandard2DMultisampleBlockShape = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.sparseProperties), &core.sparseProperties));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyStandard3DBlockShape = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.sparseProperties), &core.sparseProperties));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyAlignedMipSize = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.sparseProperties), &core.sparseProperties));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyNonResidentStrict = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(core.sparseProperties), &core.sparseProperties));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.deviceLUIDValid = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[0].core11), &nodes[0].core11));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.subgroupQuadOperationsInAllStages = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[0].core11), &nodes[0].core11));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.protectedNoFault = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[0].core11), &nodes[0].core11));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSignedZeroInfNanPreserveFloat16 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSignedZeroInfNanPreserveFloat32 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSignedZeroInfNanPreserveFloat64 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormPreserveFloat16 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormPreserveFloat32 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormPreserveFloat64 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormFlushToZeroFloat16 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormFlushToZeroFloat32 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormFlushToZeroFloat64 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTEFloat16 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTEFloat32 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTEFloat64 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTZFloat16 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTZFloat32 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTZFloat64 = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderUniformBufferArrayNonUniformIndexingNative = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSampledImageArrayNonUniformIndexingNative = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderStorageBufferArrayNonUniformIndexingNative = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderStorageImageArrayNonUniformIndexingNative = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderInputAttachmentArrayNonUniformIndexingNative = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.robustBufferAccessUpdateAfterBind = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.quadDivergentImplicitLod = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.independentResolveNone = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.independentResolve = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.filterMinmaxSingleComponentFormats = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.filterMinmaxImageComponentMapping = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[1].core12), &nodes[1].core12));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct8BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct8BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct8BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct4x8BitPackedUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct4x8BitPackedSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct4x8BitPackedMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct16BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct16BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct16BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct32BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct32BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct32BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct64BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct64BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct64BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating8BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating16BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating32BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating64BitSignedAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.storageTexelBufferOffsetSingleTexelAlignment = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.uniformTexelBufferOffsetSingleTexelAlignment = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[2].core13), &nodes[2].core13));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.earlyFragmentMultisampleCoverageAfterSampleCounting = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[4].maintenance), &nodes[4].maintenance));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.earlyFragmentSampleMaskTestBeforeSampleCounting = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[4].maintenance), &nodes[4].maintenance));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.depthStencilSwizzleOneSupport = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[4].maintenance), &nodes[4].maintenance));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.polygonModePointSize = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[4].maintenance), &nodes[4].maintenance));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.nonStrictSinglePixelWideLinesUseParallelogram = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[4].maintenance), &nodes[4].maintenance));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.nonStrictWideLinesUseParallelogram = 2;
    {
        const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
        try std.testing.expectError(error.Corrupt, validate_payload(@TypeOf(nodes[4].maintenance), &nodes[4].maintenance));
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
    var result = try decode(bytes[0..size], &KnownTags);
    result.count = 6;
    try std.testing.expectError(error.Invalid, validate_result(&KnownTags, &result));
    result.count = 5;
    result.nodes[4].type_tag = 0;
    try std.testing.expectError(error.Invalid, validate_result(&KnownTags, &result));
    result.nodes[4].type_tag = KnownTags[4];
    result.nodes[4].data.maintenance.sType = 0;
    try std.testing.expectError(error.Invalid, validate_result(&KnownTags, &result));
    result.nodes[4].data.maintenance.sType = KnownTags[4];
    result.nodes[4].data.maintenance.pNext = @ptrFromInt(8);
    try std.testing.expectError(error.Invalid, validate_result(&KnownTags, &result));
}

test "Properties2 every float rejects NaN and both infinities while negativezero retains its bits" {
    @setEvalBranchQuota(100000);
    var core: c.VkPhysicalDeviceProperties = undefined;
    var nodes: [5]data_t = undefined;
    var bytes: [4096]u8 = undefined;
    for ([_]u32{ 0x7fc00001, 0x7f800000, 0xff800000 }) |bits| {
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.maxSamplerLodBias = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.maxSamplerAnisotropy = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.viewportBoundsRange[0] = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.minInterpolationOffset = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.maxInterpolationOffset = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.timestampPeriod = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.pointSizeRange[0] = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.lineWidthRange[0] = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.pointSizeGranularity = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
        venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
        core.limits.lineWidthGranularity = @bitCast(bits);
        {
            const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
            try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &KnownTags));
            try std.testing.expectError(error.Corrupt, validate_payload(c.VkPhysicalDeviceProperties, &core));
        }
    }
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.timestampPeriod = @bitCast(@as(u32, 0x80000000));
    const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
    const result = try decode(bytes[0..size], &KnownTags);
    try std.testing.expectEqual(@as(u32, 0x80000000), @as(u32, @bitCast(result.properties.limits.timestampPeriod)));
}

test "Properties2 every fixedarray count rejects and strings normalize bounded suffixes" {
    @setEvalBranchQuota(100000);
    var core: c.VkPhysicalDeviceProperties = undefined;
    var nodes: [5]data_t = undefined;
    var bytes: [4096]u8 = undefined;
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    const size = venus_properties_test_encode(&KnownTags, 5, &core, &nodes, &bytes);
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 256), std.mem.readInt(u64, bad[320..][0..8], .little));
        std.mem.writeInt(u64, bad[320..][0..8], 257, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        @memset(bad[328..584], 65);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
        bad[331] = 0;
        var result = try decode(bad[0..size], &KnownTags);
        for (result.nodes[1].data.core12.driverName[3..]) |byte| try std.testing.expectEqual(@as(u8, 0), @as(u8, @bitCast(byte)));
        result.nodes[1].data.core12.driverName[4] = 65;
        try std.testing.expectError(error.Corrupt, validate_result(&KnownTags, &result));
        @memset(&result.nodes[1].data.core12.driverName, 65);
        try std.testing.expectError(error.Corrupt, validate_result(&KnownTags, &result));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 256), std.mem.readInt(u64, bad[584..][0..8], .little));
        std.mem.writeInt(u64, bad[584..][0..8], 257, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        @memset(bad[592..848], 65);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
        bad[595] = 0;
        var result = try decode(bad[0..size], &KnownTags);
        for (result.nodes[1].data.core12.driverInfo[3..]) |byte| try std.testing.expectEqual(@as(u8, 0), @as(u8, @bitCast(byte)));
        result.nodes[1].data.core12.driverInfo[4] = 65;
        try std.testing.expectError(error.Corrupt, validate_result(&KnownTags, &result));
        @memset(&result.nodes[1].data.core12.driverInfo, 65);
        try std.testing.expectError(error.Corrupt, validate_result(&KnownTags, &result));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 16), std.mem.readInt(u64, bad[1060..][0..8], .little));
        std.mem.writeInt(u64, bad[1060..][0..8], 17, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 16), std.mem.readInt(u64, bad[1084..][0..8], .little));
        std.mem.writeInt(u64, bad[1084..][0..8], 17, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 8), std.mem.readInt(u64, bad[1108..][0..8], .little));
        std.mem.writeInt(u64, bad[1108..][0..8], 9, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 256), std.mem.readInt(u64, bad[1196..][0..8], .little));
        std.mem.writeInt(u64, bad[1196..][0..8], 257, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        @memset(bad[1204..1460], 65);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
        bad[1207] = 0;
        var result = try decode(bad[0..size], &KnownTags);
        for (result.properties.deviceName[3..]) |byte| try std.testing.expectEqual(@as(u8, 0), @as(u8, @bitCast(byte)));
        result.properties.deviceName[4] = 65;
        try std.testing.expectError(error.Corrupt, validate_result(&KnownTags, &result));
        @memset(&result.properties.deviceName, 65);
        try std.testing.expectError(error.Corrupt, validate_result(&KnownTags, &result));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 16), std.mem.readInt(u64, bad[1460..][0..8], .little));
        std.mem.writeInt(u64, bad[1460..][0..8], 17, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 3), std.mem.readInt(u64, bad[1700..][0..8], .little));
        std.mem.writeInt(u64, bad[1700..][0..8], 4, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 3), std.mem.readInt(u64, bad[1724..][0..8], .little));
        std.mem.writeInt(u64, bad[1724..][0..8], 4, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 2), std.mem.readInt(u64, bad[1776..][0..8], .little));
        std.mem.writeInt(u64, bad[1776..][0..8], 3, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 2), std.mem.readInt(u64, bad[1792..][0..8], .little));
        std.mem.writeInt(u64, bad[1792..][0..8], 3, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 2), std.mem.readInt(u64, bad[1952..][0..8], .little));
        std.mem.writeInt(u64, bad[1952..][0..8], 3, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
    {
        var bad = bytes;
        try std.testing.expectEqual(@as(u64, 2), std.mem.readInt(u64, bad[1968..][0..8], .little));
        std.mem.writeInt(u64, bad[1968..][0..8], 3, .little);
        try std.testing.expectError(error.Corrupt, decode(bad[0..size], &KnownTags));
    }
}

fn fixture_variant_check(core: *const c.VkPhysicalDeviceProperties, nodes: *const [5]data_t, bytes: *[4096]u8) !void {
    const size = venus_properties_test_encode(&KnownTags, 5, core, nodes, bytes);
    const result = try decode(bytes[0..size], &KnownTags);
    try validate_result(&KnownTags, &result);
    try std.testing.expectEqualDeep(core.*, result.properties);
    try std.testing.expectEqualDeep(nodes[0].core11, result.nodes[0].data.core11);
    try std.testing.expectEqualDeep(nodes[1].core12, result.nodes[1].data.core12);
    try std.testing.expectEqualDeep(nodes[2].core13, result.nodes[2].data.core13);
    try std.testing.expectEqualDeep(nodes[3].robustness, result.nodes[3].data.robustness);
    try std.testing.expectEqualDeep(nodes[4].maintenance, result.nodes[4].data.maintenance);
}
test "Properties2 each named scalar and array varies independently of adjacent SDK fields" {
    @setEvalBranchQuota(100000);
    var core: c.VkPhysicalDeviceProperties = undefined;
    var nodes: [5]data_t = undefined;
    var bytes: [4096]u8 = undefined;
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.apiVersion = core.apiVersion ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.driverVersion = core.driverVersion ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.vendorID = core.vendorID ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.deviceID = core.deviceID ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.deviceType = core.deviceType ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.deviceName[0] = 90;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.pipelineCacheUUID[0] = core.pipelineCacheUUID[0] ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxImageDimension1D = core.limits.maxImageDimension1D ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxImageDimension2D = core.limits.maxImageDimension2D ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxImageDimension3D = core.limits.maxImageDimension3D ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxImageDimensionCube = core.limits.maxImageDimensionCube ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxImageArrayLayers = core.limits.maxImageArrayLayers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTexelBufferElements = core.limits.maxTexelBufferElements ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxUniformBufferRange = core.limits.maxUniformBufferRange ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxStorageBufferRange = core.limits.maxStorageBufferRange ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPushConstantsSize = core.limits.maxPushConstantsSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxMemoryAllocationCount = core.limits.maxMemoryAllocationCount ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxSamplerAllocationCount = core.limits.maxSamplerAllocationCount ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.bufferImageGranularity = core.limits.bufferImageGranularity ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.sparseAddressSpaceSize = core.limits.sparseAddressSpaceSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxBoundDescriptorSets = core.limits.maxBoundDescriptorSets ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPerStageDescriptorSamplers = core.limits.maxPerStageDescriptorSamplers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPerStageDescriptorUniformBuffers = core.limits.maxPerStageDescriptorUniformBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPerStageDescriptorStorageBuffers = core.limits.maxPerStageDescriptorStorageBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPerStageDescriptorSampledImages = core.limits.maxPerStageDescriptorSampledImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPerStageDescriptorStorageImages = core.limits.maxPerStageDescriptorStorageImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPerStageDescriptorInputAttachments = core.limits.maxPerStageDescriptorInputAttachments ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxPerStageResources = core.limits.maxPerStageResources ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetSamplers = core.limits.maxDescriptorSetSamplers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetUniformBuffers = core.limits.maxDescriptorSetUniformBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetUniformBuffersDynamic = core.limits.maxDescriptorSetUniformBuffersDynamic ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetStorageBuffers = core.limits.maxDescriptorSetStorageBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetStorageBuffersDynamic = core.limits.maxDescriptorSetStorageBuffersDynamic ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetSampledImages = core.limits.maxDescriptorSetSampledImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetStorageImages = core.limits.maxDescriptorSetStorageImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDescriptorSetInputAttachments = core.limits.maxDescriptorSetInputAttachments ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxVertexInputAttributes = core.limits.maxVertexInputAttributes ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxVertexInputBindings = core.limits.maxVertexInputBindings ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxVertexInputAttributeOffset = core.limits.maxVertexInputAttributeOffset ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxVertexInputBindingStride = core.limits.maxVertexInputBindingStride ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxVertexOutputComponents = core.limits.maxVertexOutputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationGenerationLevel = core.limits.maxTessellationGenerationLevel ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationPatchSize = core.limits.maxTessellationPatchSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationControlPerVertexInputComponents = core.limits.maxTessellationControlPerVertexInputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationControlPerVertexOutputComponents = core.limits.maxTessellationControlPerVertexOutputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationControlPerPatchOutputComponents = core.limits.maxTessellationControlPerPatchOutputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationControlTotalOutputComponents = core.limits.maxTessellationControlTotalOutputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationEvaluationInputComponents = core.limits.maxTessellationEvaluationInputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTessellationEvaluationOutputComponents = core.limits.maxTessellationEvaluationOutputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxGeometryShaderInvocations = core.limits.maxGeometryShaderInvocations ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxGeometryInputComponents = core.limits.maxGeometryInputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxGeometryOutputComponents = core.limits.maxGeometryOutputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxGeometryOutputVertices = core.limits.maxGeometryOutputVertices ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxGeometryTotalOutputComponents = core.limits.maxGeometryTotalOutputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxFragmentInputComponents = core.limits.maxFragmentInputComponents ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxFragmentOutputAttachments = core.limits.maxFragmentOutputAttachments ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxFragmentDualSrcAttachments = core.limits.maxFragmentDualSrcAttachments ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxFragmentCombinedOutputResources = core.limits.maxFragmentCombinedOutputResources ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxComputeSharedMemorySize = core.limits.maxComputeSharedMemorySize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxComputeWorkGroupCount[0] = core.limits.maxComputeWorkGroupCount[0] ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxComputeWorkGroupInvocations = core.limits.maxComputeWorkGroupInvocations ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxComputeWorkGroupSize[0] = core.limits.maxComputeWorkGroupSize[0] ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.subPixelPrecisionBits = core.limits.subPixelPrecisionBits ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.subTexelPrecisionBits = core.limits.subTexelPrecisionBits ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.mipmapPrecisionBits = core.limits.mipmapPrecisionBits ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDrawIndexedIndexValue = core.limits.maxDrawIndexedIndexValue ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxDrawIndirectCount = core.limits.maxDrawIndirectCount ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxSamplerLodBias = core.limits.maxSamplerLodBias + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxSamplerAnisotropy = core.limits.maxSamplerAnisotropy + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxViewports = core.limits.maxViewports ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxViewportDimensions[0] = core.limits.maxViewportDimensions[0] ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.viewportBoundsRange[0] = core.limits.viewportBoundsRange[0] + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.viewportSubPixelBits = core.limits.viewportSubPixelBits ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.minMemoryMapAlignment = core.limits.minMemoryMapAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.minTexelBufferOffsetAlignment = core.limits.minTexelBufferOffsetAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.minUniformBufferOffsetAlignment = core.limits.minUniformBufferOffsetAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.minStorageBufferOffsetAlignment = core.limits.minStorageBufferOffsetAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.minTexelOffset = core.limits.minTexelOffset ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTexelOffset = core.limits.maxTexelOffset ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.minTexelGatherOffset = core.limits.minTexelGatherOffset ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxTexelGatherOffset = core.limits.maxTexelGatherOffset ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.minInterpolationOffset = core.limits.minInterpolationOffset + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxInterpolationOffset = core.limits.maxInterpolationOffset + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.subPixelInterpolationOffsetBits = core.limits.subPixelInterpolationOffsetBits ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxFramebufferWidth = core.limits.maxFramebufferWidth ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxFramebufferHeight = core.limits.maxFramebufferHeight ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxFramebufferLayers = core.limits.maxFramebufferLayers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.framebufferColorSampleCounts = core.limits.framebufferColorSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.framebufferDepthSampleCounts = core.limits.framebufferDepthSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.framebufferStencilSampleCounts = core.limits.framebufferStencilSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.framebufferNoAttachmentsSampleCounts = core.limits.framebufferNoAttachmentsSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxColorAttachments = core.limits.maxColorAttachments ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.sampledImageColorSampleCounts = core.limits.sampledImageColorSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.sampledImageIntegerSampleCounts = core.limits.sampledImageIntegerSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.sampledImageDepthSampleCounts = core.limits.sampledImageDepthSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.sampledImageStencilSampleCounts = core.limits.sampledImageStencilSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.storageImageSampleCounts = core.limits.storageImageSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxSampleMaskWords = core.limits.maxSampleMaskWords ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.timestampComputeAndGraphics = core.limits.timestampComputeAndGraphics ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.timestampPeriod = core.limits.timestampPeriod + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxClipDistances = core.limits.maxClipDistances ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxCullDistances = core.limits.maxCullDistances ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.maxCombinedClipAndCullDistances = core.limits.maxCombinedClipAndCullDistances ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.discreteQueuePriorities = core.limits.discreteQueuePriorities ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.pointSizeRange[0] = core.limits.pointSizeRange[0] + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.lineWidthRange[0] = core.limits.lineWidthRange[0] + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.pointSizeGranularity = core.limits.pointSizeGranularity + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.lineWidthGranularity = core.limits.lineWidthGranularity + 1.5;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.strictLines = core.limits.strictLines ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.standardSampleLocations = core.limits.standardSampleLocations ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.optimalBufferCopyOffsetAlignment = core.limits.optimalBufferCopyOffsetAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.optimalBufferCopyRowPitchAlignment = core.limits.optimalBufferCopyRowPitchAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.limits.nonCoherentAtomSize = core.limits.nonCoherentAtomSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyStandard2DBlockShape = core.sparseProperties.residencyStandard2DBlockShape ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyStandard2DMultisampleBlockShape = core.sparseProperties.residencyStandard2DMultisampleBlockShape ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyStandard3DBlockShape = core.sparseProperties.residencyStandard3DBlockShape ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyAlignedMipSize = core.sparseProperties.residencyAlignedMipSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    core.sparseProperties.residencyNonResidentStrict = core.sparseProperties.residencyNonResidentStrict ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.deviceUUID[0] = nodes[0].core11.deviceUUID[0] ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.driverUUID[0] = nodes[0].core11.driverUUID[0] ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.deviceLUID[0] = nodes[0].core11.deviceLUID[0] ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.deviceNodeMask = nodes[0].core11.deviceNodeMask ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.deviceLUIDValid = nodes[0].core11.deviceLUIDValid ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.subgroupSize = nodes[0].core11.subgroupSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.subgroupSupportedStages = nodes[0].core11.subgroupSupportedStages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.subgroupSupportedOperations = nodes[0].core11.subgroupSupportedOperations ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.subgroupQuadOperationsInAllStages = nodes[0].core11.subgroupQuadOperationsInAllStages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.pointClippingBehavior = nodes[0].core11.pointClippingBehavior ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.maxMultiviewViewCount = nodes[0].core11.maxMultiviewViewCount ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.maxMultiviewInstanceIndex = nodes[0].core11.maxMultiviewInstanceIndex ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.protectedNoFault = nodes[0].core11.protectedNoFault ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.maxPerSetDescriptors = nodes[0].core11.maxPerSetDescriptors ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[0].core11.maxMemoryAllocationSize = nodes[0].core11.maxMemoryAllocationSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.driverID = nodes[1].core12.driverID ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.driverName[0] = 90;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.driverInfo[0] = 90;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.conformanceVersion.major = nodes[1].core12.conformanceVersion.major ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.conformanceVersion.minor = nodes[1].core12.conformanceVersion.minor ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.conformanceVersion.subminor = nodes[1].core12.conformanceVersion.subminor ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.conformanceVersion.patch = nodes[1].core12.conformanceVersion.patch ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.denormBehaviorIndependence = nodes[1].core12.denormBehaviorIndependence ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.roundingModeIndependence = nodes[1].core12.roundingModeIndependence ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSignedZeroInfNanPreserveFloat16 = nodes[1].core12.shaderSignedZeroInfNanPreserveFloat16 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSignedZeroInfNanPreserveFloat32 = nodes[1].core12.shaderSignedZeroInfNanPreserveFloat32 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSignedZeroInfNanPreserveFloat64 = nodes[1].core12.shaderSignedZeroInfNanPreserveFloat64 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormPreserveFloat16 = nodes[1].core12.shaderDenormPreserveFloat16 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormPreserveFloat32 = nodes[1].core12.shaderDenormPreserveFloat32 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormPreserveFloat64 = nodes[1].core12.shaderDenormPreserveFloat64 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormFlushToZeroFloat16 = nodes[1].core12.shaderDenormFlushToZeroFloat16 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormFlushToZeroFloat32 = nodes[1].core12.shaderDenormFlushToZeroFloat32 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderDenormFlushToZeroFloat64 = nodes[1].core12.shaderDenormFlushToZeroFloat64 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTEFloat16 = nodes[1].core12.shaderRoundingModeRTEFloat16 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTEFloat32 = nodes[1].core12.shaderRoundingModeRTEFloat32 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTEFloat64 = nodes[1].core12.shaderRoundingModeRTEFloat64 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTZFloat16 = nodes[1].core12.shaderRoundingModeRTZFloat16 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTZFloat32 = nodes[1].core12.shaderRoundingModeRTZFloat32 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderRoundingModeRTZFloat64 = nodes[1].core12.shaderRoundingModeRTZFloat64 ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxUpdateAfterBindDescriptorsInAllPools = nodes[1].core12.maxUpdateAfterBindDescriptorsInAllPools ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderUniformBufferArrayNonUniformIndexingNative = nodes[1].core12.shaderUniformBufferArrayNonUniformIndexingNative ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderSampledImageArrayNonUniformIndexingNative = nodes[1].core12.shaderSampledImageArrayNonUniformIndexingNative ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderStorageBufferArrayNonUniformIndexingNative = nodes[1].core12.shaderStorageBufferArrayNonUniformIndexingNative ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderStorageImageArrayNonUniformIndexingNative = nodes[1].core12.shaderStorageImageArrayNonUniformIndexingNative ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.shaderInputAttachmentArrayNonUniformIndexingNative = nodes[1].core12.shaderInputAttachmentArrayNonUniformIndexingNative ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.robustBufferAccessUpdateAfterBind = nodes[1].core12.robustBufferAccessUpdateAfterBind ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.quadDivergentImplicitLod = nodes[1].core12.quadDivergentImplicitLod ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxPerStageDescriptorUpdateAfterBindSamplers = nodes[1].core12.maxPerStageDescriptorUpdateAfterBindSamplers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxPerStageDescriptorUpdateAfterBindUniformBuffers = nodes[1].core12.maxPerStageDescriptorUpdateAfterBindUniformBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxPerStageDescriptorUpdateAfterBindStorageBuffers = nodes[1].core12.maxPerStageDescriptorUpdateAfterBindStorageBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxPerStageDescriptorUpdateAfterBindSampledImages = nodes[1].core12.maxPerStageDescriptorUpdateAfterBindSampledImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxPerStageDescriptorUpdateAfterBindStorageImages = nodes[1].core12.maxPerStageDescriptorUpdateAfterBindStorageImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxPerStageDescriptorUpdateAfterBindInputAttachments = nodes[1].core12.maxPerStageDescriptorUpdateAfterBindInputAttachments ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxPerStageUpdateAfterBindResources = nodes[1].core12.maxPerStageUpdateAfterBindResources ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindSamplers = nodes[1].core12.maxDescriptorSetUpdateAfterBindSamplers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindUniformBuffers = nodes[1].core12.maxDescriptorSetUpdateAfterBindUniformBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = nodes[1].core12.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindStorageBuffers = nodes[1].core12.maxDescriptorSetUpdateAfterBindStorageBuffers ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = nodes[1].core12.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindSampledImages = nodes[1].core12.maxDescriptorSetUpdateAfterBindSampledImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindStorageImages = nodes[1].core12.maxDescriptorSetUpdateAfterBindStorageImages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxDescriptorSetUpdateAfterBindInputAttachments = nodes[1].core12.maxDescriptorSetUpdateAfterBindInputAttachments ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.supportedDepthResolveModes = nodes[1].core12.supportedDepthResolveModes ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.supportedStencilResolveModes = nodes[1].core12.supportedStencilResolveModes ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.independentResolveNone = nodes[1].core12.independentResolveNone ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.independentResolve = nodes[1].core12.independentResolve ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.filterMinmaxSingleComponentFormats = nodes[1].core12.filterMinmaxSingleComponentFormats ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.filterMinmaxImageComponentMapping = nodes[1].core12.filterMinmaxImageComponentMapping ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.maxTimelineSemaphoreValueDifference = nodes[1].core12.maxTimelineSemaphoreValueDifference ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[1].core12.framebufferIntegerColorSampleCounts = nodes[1].core12.framebufferIntegerColorSampleCounts ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.minSubgroupSize = nodes[2].core13.minSubgroupSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxSubgroupSize = nodes[2].core13.maxSubgroupSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxComputeWorkgroupSubgroups = nodes[2].core13.maxComputeWorkgroupSubgroups ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.requiredSubgroupSizeStages = nodes[2].core13.requiredSubgroupSizeStages ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxInlineUniformBlockSize = nodes[2].core13.maxInlineUniformBlockSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxPerStageDescriptorInlineUniformBlocks = nodes[2].core13.maxPerStageDescriptorInlineUniformBlocks ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = nodes[2].core13.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxDescriptorSetInlineUniformBlocks = nodes[2].core13.maxDescriptorSetInlineUniformBlocks ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxDescriptorSetUpdateAfterBindInlineUniformBlocks = nodes[2].core13.maxDescriptorSetUpdateAfterBindInlineUniformBlocks ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxInlineUniformTotalSize = nodes[2].core13.maxInlineUniformTotalSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct8BitUnsignedAccelerated = nodes[2].core13.integerDotProduct8BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct8BitSignedAccelerated = nodes[2].core13.integerDotProduct8BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct8BitMixedSignednessAccelerated = nodes[2].core13.integerDotProduct8BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct4x8BitPackedUnsignedAccelerated = nodes[2].core13.integerDotProduct4x8BitPackedUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct4x8BitPackedSignedAccelerated = nodes[2].core13.integerDotProduct4x8BitPackedSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct4x8BitPackedMixedSignednessAccelerated = nodes[2].core13.integerDotProduct4x8BitPackedMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct16BitUnsignedAccelerated = nodes[2].core13.integerDotProduct16BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct16BitSignedAccelerated = nodes[2].core13.integerDotProduct16BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct16BitMixedSignednessAccelerated = nodes[2].core13.integerDotProduct16BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct32BitUnsignedAccelerated = nodes[2].core13.integerDotProduct32BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct32BitSignedAccelerated = nodes[2].core13.integerDotProduct32BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct32BitMixedSignednessAccelerated = nodes[2].core13.integerDotProduct32BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct64BitUnsignedAccelerated = nodes[2].core13.integerDotProduct64BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct64BitSignedAccelerated = nodes[2].core13.integerDotProduct64BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProduct64BitMixedSignednessAccelerated = nodes[2].core13.integerDotProduct64BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating8BitSignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating8BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating16BitSignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating16BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating32BitSignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating32BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating64BitSignedAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating64BitSignedAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated = nodes[2].core13.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.storageTexelBufferOffsetAlignmentBytes = nodes[2].core13.storageTexelBufferOffsetAlignmentBytes ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.storageTexelBufferOffsetSingleTexelAlignment = nodes[2].core13.storageTexelBufferOffsetSingleTexelAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.uniformTexelBufferOffsetAlignmentBytes = nodes[2].core13.uniformTexelBufferOffsetAlignmentBytes ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.uniformTexelBufferOffsetSingleTexelAlignment = nodes[2].core13.uniformTexelBufferOffsetSingleTexelAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[2].core13.maxBufferSize = nodes[2].core13.maxBufferSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[3].robustness.robustStorageBufferAccessSizeAlignment = nodes[3].robustness.robustStorageBufferAccessSizeAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[3].robustness.robustUniformBufferAccessSizeAlignment = nodes[3].robustness.robustUniformBufferAccessSizeAlignment ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.earlyFragmentMultisampleCoverageAfterSampleCounting = nodes[4].maintenance.earlyFragmentMultisampleCoverageAfterSampleCounting ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.earlyFragmentSampleMaskTestBeforeSampleCounting = nodes[4].maintenance.earlyFragmentSampleMaskTestBeforeSampleCounting ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.depthStencilSwizzleOneSupport = nodes[4].maintenance.depthStencilSwizzleOneSupport ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.polygonModePointSize = nodes[4].maintenance.polygonModePointSize ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.nonStrictSinglePixelWideLinesUseParallelogram = nodes[4].maintenance.nonStrictSinglePixelWideLinesUseParallelogram ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
    venus_properties_test_fixture(&KnownTags, 5, &core, &nodes);
    nodes[4].maintenance.nonStrictWideLinesUseParallelogram = nodes[4].maintenance.nonStrictWideLinesUseParallelogram ^ 1;
    try fixture_variant_check(&core, &nodes, &bytes);
}

test "Properties2 256 allocator-owned deterministic success and error lifetimes" {
    @setEvalBranchQuota(100000);
    for (0..256) |cycle| {
        const bytes = try std.testing.allocator.alloc(u8, 4096);
        defer std.testing.allocator.free(bytes);
        var tags = KnownTags;
        std.mem.rotate(u32, &tags, cycle % 5);
        var core: c.VkPhysicalDeviceProperties = undefined;
        var nodes: [5]data_t = undefined;
        venus_properties_test_fixture(&tags, 5, &core, &nodes);
        const size = venus_properties_test_encode(&tags, 5, &core, &nodes, bytes.ptr);
        const result = try decode(bytes[0..size], &tags);
        try validate_result(&tags, &result);
        try std.testing.expectEqualDeep(core, result.properties);
        try std.testing.expectError(error.Bounds, decode(bytes[0 .. cycle % size], &tags));
        bytes[0] = 0;
        try std.testing.expectError(error.Corrupt, decode(bytes[0..size], &tags));
    }
}
