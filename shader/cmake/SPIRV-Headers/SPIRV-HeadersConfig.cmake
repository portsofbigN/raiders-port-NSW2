# SPIR-V headers from the installed Vulkan SDK, for sirit (SIRIT_USE_SYSTEM_SPIRV_HEADERS).
if(NOT TARGET SPIRV-Headers::SPIRV-Headers)
  add_library(SPIRV-Headers::SPIRV-Headers INTERFACE IMPORTED)
  set_target_properties(SPIRV-Headers::SPIRV-Headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "C:/VulkanSDK/1.4.363.0/Include")
endif()
set(SPIRV-Headers_FOUND TRUE)
