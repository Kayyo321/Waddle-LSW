/** @file features_oracle.h @brief Native typed feature-chain fixtures, no
 * production storage. */
#ifndef WaddleFeaturesOracleH
/** @brief Guard; no runtime storage or ownership. */
#define WaddleFeaturesOracleH
#include <assert.h>
#include <string.h>
#include <vulkan/vulkan.h>
/** @brief Caller-owned actual native feature structures; active member follows
 * requested type tag. */
typedef union feature_node_t {
  VkPhysicalDeviceVulkan11Features core; /**< Owned native Vulkan11 flags. */
  VkPhysicalDeviceShaderDrawParametersFeatures
      draw; /**< Owned native draw flags. */
  VkPhysicalDeviceHostQueryResetFeatures
      query; /**< Owned native query flags. */
  VkPhysicalDeviceTransformFeedbackFeaturesEXT
      feedback; /**< Owned native feedback flags. */
} feature_node_t;
/** @brief Initialize four-or-fewer typed native nodes with deterministic flags
 * and borrowed links.
 * @param[out] nodes Nonnull private four-node storage, lifetime includes
 * encoding call.
 * @param[in] tags Nonnull borrowed array[count] of recognized unique Vulkan
 * type tags.
 * @param[in] count At most four, zero allowed.
 * @note No allocation; single fixture thread; writes actual native types, never
 * wire casts.
 */
static inline void feature_nodes_init(feature_node_t *nodes,
                                      const uint32_t *tags, size_t count) {
  assert(nodes && tags && count <= 4);
  VkBool32 flags[12];
  for (size_t index = 0; index < 12; index++)
    flags[index] = (VkBool32)(index % 2);
  memset(nodes, 0, sizeof(*nodes) * 4);
  for (size_t index = 0; index < count; index++) {
    void *next = index + 1 < count ? &nodes[index + 1] : NULL;
    switch (tags[index]) {
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
#endif
