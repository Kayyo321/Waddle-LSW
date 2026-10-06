/** @file features_oracle.h @brief Native typed feature-chain fixtures, no
 * production storage. */
#ifndef WaddleFeaturesOracleH
/** @brief Guard; no runtime storage or ownership. */
#define WaddleFeaturesOracleH
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
/** @brief Caller-owned actual native feature structures; active member follows
 * requested type tag. */
typedef union feature_node_t {
  VkPhysicalDeviceVulkan12Features
      core12; /**< Owned native47-word Vulkan12 flags. */
  VkPhysicalDeviceVulkan13Features
      core13; /**< Owned native15-word Vulkan13 flags. */
  VkPhysicalDeviceRobustness2FeaturesEXT
      robustness; /**< Owned native3-word robustness2 flags. */
  VkPhysicalDeviceMaintenance5FeaturesKHR
      maintenance; /**< Owned native1-word maintenance5 flag. */
  VkPhysicalDeviceVulkan11Features core; /**< Owned native Vulkan11 flags. */
  VkPhysicalDeviceShaderDrawParametersFeatures
      draw; /**< Owned native draw flags. */
  VkPhysicalDeviceHostQueryResetFeatures
      query; /**< Owned native query flags. */
  VkPhysicalDeviceTransformFeedbackFeaturesEXT
      feedback; /**< Owned native feedback flags. */
} feature_node_t;
_Static_assert(offsetof(VkPhysicalDeviceVulkan11Features,
                        shaderDrawParameters) -
                       offsetof(VkPhysicalDeviceVulkan11Features,
                                storageBuffer16BitAccess) ==
                   11 * sizeof(VkBool32),
               "Exact Vulkan11 native Boolean interval");
_Static_assert(offsetof(VkPhysicalDeviceVulkan12Features,
                        subgroupBroadcastDynamicId) -
                       offsetof(VkPhysicalDeviceVulkan12Features,
                                samplerMirrorClampToEdge) ==
                   46 * sizeof(VkBool32),
               "Exact Vulkan12 native Boolean interval");
_Static_assert(offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) -
                       offsetof(VkPhysicalDeviceVulkan13Features,
                                robustImageAccess) ==
                   14 * sizeof(VkBool32),
               "Exact Vulkan13 native Boolean interval");
_Static_assert(offsetof(VkPhysicalDeviceRobustness2FeaturesEXT,
                        nullDescriptor) -
                       offsetof(VkPhysicalDeviceRobustness2FeaturesEXT,
                                robustBufferAccess2) ==
                   2 * sizeof(VkBool32),
               "Exact robustness2 native Boolean interval");
_Static_assert(offsetof(VkPhysicalDeviceTransformFeedbackFeaturesEXT,
                        geometryStreams) -
                       offsetof(VkPhysicalDeviceTransformFeedbackFeaturesEXT,
                                transformFeedback) ==
                   sizeof(VkBool32),
               "Exact feedback native Boolean interval");
/** @brief Initialize eight-or-fewer typed native nodes with deterministic flags
 * and borrowed links.
 * @param[out] nodes Nonnull private eight-node storage, lifetime includes
 * encoding call.
 * @param[in] tags Nonnull borrowed array[count] of recognized unique Vulkan
 * type tags.
 * @param[in] count At most eight, zero allowed.
 * @note No allocation; single fixture thread; writes actual native types, never
 * wire casts.
 */
static inline void feature_nodes_init(feature_node_t *nodes,
                                      const uint32_t *tags, size_t count) {
  assert(nodes && tags && count <= 8);
  VkBool32 flags[47];
  for (size_t index = 0; index < 47; index++)
    flags[index] = (VkBool32)(index % 2);
  memset(nodes, 0, sizeof(*nodes) * 8);
  for (size_t index = 0; index < count; index++) {
    void *next = index + 1 < count ? &nodes[index + 1] : NULL;
    switch (tags[index]) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
      nodes[index].core12 = (VkPhysicalDeviceVulkan12Features){
          .sType = tags[index], .pNext = next};
      memcpy(&nodes[index].core12.samplerMirrorClampToEdge, flags,
             47 * sizeof(*flags));
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
      nodes[index].core13 = (VkPhysicalDeviceVulkan13Features){
          .sType = tags[index], .pNext = next};
      memcpy(&nodes[index].core13.robustImageAccess, flags,
             15 * sizeof(*flags));
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
      nodes[index].robustness = (VkPhysicalDeviceRobustness2FeaturesEXT){
          .sType = tags[index], .pNext = next};
      memcpy(&nodes[index].robustness.robustBufferAccess2, flags,
             3 * sizeof(*flags));
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
      nodes[index].maintenance = (VkPhysicalDeviceMaintenance5FeaturesKHR){
          .sType = tags[index], .pNext = next};
      nodes[index].maintenance.maintenance5 = flags[0];
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
      nodes[index].core = (VkPhysicalDeviceVulkan11Features){
          .sType = tags[index], .pNext = next};
      memcpy(&nodes[index].core.storageBuffer16BitAccess, flags,
             12 * sizeof(*flags));
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
      nodes[index].draw = (VkPhysicalDeviceShaderDrawParametersFeatures){
          .sType = tags[index], .pNext = next};
      nodes[index].draw.shaderDrawParameters = flags[0];
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
      nodes[index].query = (VkPhysicalDeviceHostQueryResetFeatures){
          .sType = tags[index], .pNext = next};
      nodes[index].query.hostQueryReset = flags[0];
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
      nodes[index].feedback = (VkPhysicalDeviceTransformFeedbackFeaturesEXT){
          .sType = tags[index], .pNext = next};
      nodes[index].feedback.transformFeedback = flags[0];
      nodes[index].feedback.geometryStreams = flags[1];
      break;
    default:
      assert(0);
    }
  }
}
/** @brief Replace every initialized native node flag by a forward-chain one-hot
 * pattern.
 * @param[in,out] nodes Nonnull exclusive native nodes[count], initialized and
 * borrowed for call.
 * @param[in] tags Nonnull recognized tag array[count]. @param[in] count Actual
 * extent0..8.
 * @param[in] position Forward-chain flag index; SIZE_MAX means all node flags
 * false.
 * @note No allocation/retention; native header/links preserved. Compile-time
 * offset assertions bound flag copies.
 */
static inline void feature_nodes_one_hot(feature_node_t *nodes,
                                         const uint32_t *tags, size_t count,
                                         size_t position) {
  for (size_t index = 0; index < count; index++) {
    void *first = NULL;
    size_t flags = 0;
    switch (tags[index]) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
      first = &nodes[index].core.storageBuffer16BitAccess;
      flags = 12;
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
      first = &nodes[index].core12.samplerMirrorClampToEdge;
      flags = 47;
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
      first = &nodes[index].core13.robustImageAccess;
      flags = 15;
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
      first = &nodes[index].robustness.robustBufferAccess2;
      flags = 3;
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
      first = &nodes[index].maintenance.maintenance5;
      flags = 1;
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
      first = &nodes[index].draw.shaderDrawParameters;
      flags = 1;
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
      first = &nodes[index].query.hostQueryReset;
      flags = 1;
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
      first = &nodes[index].feedback.transformFeedback;
      flags = 2;
      break;
    default:
      assert(0);
    }
    memset(first, 0, flags * sizeof(VkBool32));
    if (position < flags) {
      const VkBool32 enabled = VK_TRUE;
      memcpy((unsigned char *)first + position * sizeof(VkBool32), &enabled,
             sizeof(enabled));
      position = SIZE_MAX;
    } else if (position != SIZE_MAX)
      position -= flags;
  }
}
#endif
