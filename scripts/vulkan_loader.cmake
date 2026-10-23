# Test-only native Vulkan loader/header compatibility boundary.
# Vulkan::Headers and VulkanHeaders_VERSION are upstream CMake API names.
# Input: repository-pinned protocol headers; no downloads, allocation or install.
# Failure: stop configure for missing/mismatched headers; no partial vendor edits.
get_filename_component(ProtocolInclude "${CMAKE_CURRENT_LIST_DIR}/../submodules/venus_protocol/include" ABSOLUTE)
if(NOT EXISTS "${ProtocolInclude}/vulkan/vulkan_core.h")
    message(FATAL_ERROR "Initialize the pinned Venus protocol submodule first")
endif()
file(STRINGS "${ProtocolInclude}/vulkan/vulkan_core.h" HeaderVersionLine REGEX "^#define VK_HEADER_VERSION [0-9]+$")
if(NOT HeaderVersionLine STREQUAL "#define VK_HEADER_VERSION 307")
    message(FATAL_ERROR "Native loader verification requires pinned header revision307")
endif()
if(NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED GLOBAL)
    set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${ProtocolInclude}")
endif()
set(VulkanHeaders_VERSION "1.4.307")
