# Standalone build of Eglantine's shader translator (GPL-3.0-or-later, third_party/eglantine): Switch 2 SM86
# programs are lowered to Maxwell IR (frontend/ampere) and emitted as SPIR-V for the Vulkan back end.
cmake_minimum_required(VERSION 3.22)
project(nx2_shader_xlate CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(NOT DEFINED DEPS)
  if(DEFINED ENV{DEPS_DIR})
    set(DEPS "$ENV{DEPS_DIR}")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../deps")
    set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../deps")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../projetos/deps")
    set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../../projetos/deps")
  else()
    set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../../projetos/deps")
  endif()
endif()

set(EGL "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/eglantine")
set(BOOST_INCLUDE "${DEPS}/boost_dl/boost_1_91_0")

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/fmt" fmt EXCLUDE_FROM_ALL)

set(SPIRV-Headers_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SPIRV-Headers")
set(SIRIT_USE_SYSTEM_SPIRV_HEADERS ON CACHE BOOL "" FORCE)
set(SIRIT_TESTS OFF CACHE BOOL "" FORCE)
set(SIRIT_INSTALL OFF CACHE BOOL "" FORCE)
set(SIRIT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SIRIT_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/sirit" sirit EXCLUDE_FROM_ALL)

file(GLOB_RECURSE XLATE_SOURCES CONFIGURE_DEPENDS "${EGL}/shader_recompiler/*.cpp")
add_library(shader_recompiler STATIC ${XLATE_SOURCES})
target_include_directories(shader_recompiler PUBLIC
  "${EGL}/compat"   # settings.h, logging.h, assert.h stand-ins come first
  "${EGL}"
  "${BOOST_INCLUDE}")
target_compile_definitions(shader_recompiler PUBLIC BOOST_ALL_NO_LIB _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(shader_recompiler PRIVATE -fbracket-depth=1024 -Wno-everything
  -include "${EGL}/compat/enum_format.h")
target_link_libraries(shader_recompiler PUBLIC fmt::fmt sirit::sirit)

# In-game translator with a C interface (nx2_xlate.h), linked into the host executable
add_library(nx2_xlate STATIC nx2_xlate.cpp)
target_link_libraries(nx2_xlate PUBLIC shader_recompiler)
target_compile_options(nx2_xlate PRIVATE -fbracket-depth=1024 -Wno-everything -include "${EGL}/compat/enum_format.h")

# Offline translator: captured shader stages (work/shaders/*.code) -> SPIR-V
add_executable(xlate_tool xlate_tool.cpp)
target_link_libraries(xlate_tool PRIVATE nx2_xlate)
