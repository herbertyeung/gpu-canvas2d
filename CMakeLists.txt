cmake_minimum_required(VERSION 3.24)

project(gpu_2d_lab VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_SUPPRESS_REGENERATION ON)

option(GPU2D_USE_SKIA "Build the optional Skia renderer plugin" OFF)
option(BUILD_TESTING "Build the GPU 2D lab tests" ON)
set(GPU2D_SKIA_ROOT "" CACHE PATH "Skia package prefix containing share/unofficial-skia")
set(GPU2D_QJS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg_installed/x64-windows"
    CACHE PATH "QuickJS-ng package prefix containing share/qjs")

if(NOT GPU2D_SKIA_ROOT AND DEFINED ENV{GPU2D_SKIA_ROOT})
  set(GPU2D_SKIA_ROOT "$ENV{GPU2D_SKIA_ROOT}" CACHE PATH
      "Skia package prefix containing share/unofficial-skia" FORCE)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use: cmake --preset default")
endif()

if(NOT WIN32)
  message(FATAL_ERROR "gpu_2d_lab currently requires Windows and Direct3D 12")
endif()

if(GPU2D_QJS_ROOT)
  list(PREPEND CMAKE_PREFIX_PATH "${GPU2D_QJS_ROOT}")
endif()
find_package(qjs CONFIG REQUIRED)

set(GPU2D_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${GPU2D_BIN_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${GPU2D_BIN_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
foreach(CONFIG_TYPE IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER "${CONFIG_TYPE}" CONFIG_TYPE_UPPER)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${GPU2D_BIN_DIR}/${CONFIG_TYPE}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${GPU2D_BIN_DIR}/${CONFIG_TYPE}")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${CMAKE_CURRENT_BINARY_DIR}/lib/${CONFIG_TYPE}")
endforeach()

function(gpu2d_configure_target TARGET_NAME)
  target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
  target_compile_definitions(${TARGET_NAME} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
  endif()
endfunction()

function(gpu2d_add_headers TARGET_NAME)
  target_sources(${TARGET_NAME} PRIVATE ${ARGN})
  source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files"
               FILES ${ARGN})
endfunction()

add_library(gpu2d_core STATIC
  src/display_list.cpp
  src/frame_planner.cpp
)
gpu2d_add_headers(gpu2d_core
  include/gpu2d/canvas_frame.hpp
  include/gpu2d/display_list.hpp
  include/gpu2d/frame_planner.hpp
  include/gpu2d/math.hpp
  include/gpu2d/render_types.hpp
)
target_include_directories(gpu2d_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(gpu2d_core PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(gpu2d_core PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()

add_library(gpu2d_script STATIC
  src/builtin_examples.cpp
  src/javascript_page.cpp
)
gpu2d_add_headers(gpu2d_script
  include/gpu2d/builtin_examples.hpp
  include/gpu2d/javascript_page.hpp
)
gpu2d_configure_target(gpu2d_script)
target_link_libraries(gpu2d_script PUBLIC gpu2d_core PRIVATE qjs)

add_executable(gpu_2d_demo
  src/main.cpp
  src/renderer_plugin_host.cpp
  src/win32_application.cpp
)
gpu2d_add_headers(gpu_2d_demo
  include/gpu2d/renderer_plugin_api.h
  include/gpu2d/renderer_plugin_host.hpp
  include/gpu2d/win32_application.hpp
)
gpu2d_configure_target(gpu_2d_demo)
target_link_libraries(gpu_2d_demo PRIVATE gpu2d_core gpu2d_script user32 gdi32 shell32)

add_library(gpu2d_renderer_d3d12 SHARED
  src/d3d12_renderer.cpp
  src/renderer_plugin_d3d12.cpp
)
gpu2d_add_headers(gpu2d_renderer_d3d12
  include/gpu2d/d3d12_renderer.hpp
  include/gpu2d/renderer_plugin_api.h
)
target_sources(gpu2d_renderer_d3d12 PRIVATE src/renderer_plugin_common.hpp)
source_group("Header Files\\Private" FILES src/renderer_plugin_common.hpp)
gpu2d_configure_target(gpu2d_renderer_d3d12)
target_link_libraries(gpu2d_renderer_d3d12 PRIVATE gpu2d_core d3d12 dxgi dxguid d3dcompiler user32)
set_target_properties(gpu2d_renderer_d3d12 PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${GPU2D_BIN_DIR}/renderers"
  LIBRARY_OUTPUT_DIRECTORY "${GPU2D_BIN_DIR}/renderers"
  OUTPUT_NAME "gpu2d_renderer_d3d12"
)
foreach(CONFIG_TYPE IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER "${CONFIG_TYPE}" CONFIG_TYPE_UPPER)
  set_target_properties(gpu2d_renderer_d3d12 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${GPU2D_BIN_DIR}/${CONFIG_TYPE}/renderers"
    LIBRARY_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${GPU2D_BIN_DIR}/${CONFIG_TYPE}/renderers"
  )
endforeach()

if(GPU2D_USE_SKIA)
  if(GPU2D_SKIA_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${GPU2D_SKIA_ROOT}")
  endif()
  find_package(unofficial-skia CONFIG REQUIRED)
  add_library(gpu2d_renderer_skia SHARED
    src/d3d12_renderer.cpp
    src/renderer_plugin_skia.cpp
    src/skia_rasterizer.cpp
  )
  gpu2d_add_headers(gpu2d_renderer_skia
    include/gpu2d/d3d12_renderer.hpp
    include/gpu2d/renderer_plugin_api.h
    include/gpu2d/skia_rasterizer.hpp
  )
  target_sources(gpu2d_renderer_skia PRIVATE src/renderer_plugin_common.hpp)
  source_group("Header Files\\Private" FILES src/renderer_plugin_common.hpp)
  gpu2d_configure_target(gpu2d_renderer_skia)
  target_link_libraries(gpu2d_renderer_skia PRIVATE
    gpu2d_core unofficial::skia::skia d3d12 dxgi dxguid d3dcompiler user32)
  set_target_properties(gpu2d_renderer_skia PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${GPU2D_BIN_DIR}/renderers"
    LIBRARY_OUTPUT_DIRECTORY "${GPU2D_BIN_DIR}/renderers"
    OUTPUT_NAME "gpu2d_renderer_skia"
  )
  foreach(CONFIG_TYPE IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${CONFIG_TYPE}" CONFIG_TYPE_UPPER)
    set_target_properties(gpu2d_renderer_skia PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${GPU2D_BIN_DIR}/${CONFIG_TYPE}/renderers"
      LIBRARY_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${GPU2D_BIN_DIR}/${CONFIG_TYPE}/renderers"
    )
  endforeach()

  get_target_property(SKIA_INCLUDE_DIRS unofficial::skia::skia INTERFACE_INCLUDE_DIRECTORIES)
  list(GET SKIA_INCLUDE_DIRS 0 SKIA_INCLUDE_DIR)
  get_filename_component(SKIA_PREFIX "${SKIA_INCLUDE_DIR}/../.." ABSOLUTE)
  set(SKIA_RUNTIME_RELEASE_DIR "${SKIA_PREFIX}/bin")
  set(SKIA_RUNTIME_DEBUG_DIR "${SKIA_PREFIX}/debug/bin")
  if(NOT EXISTS "${SKIA_RUNTIME_RELEASE_DIR}/skia.dll" OR
     NOT EXISTS "${SKIA_RUNTIME_DEBUG_DIR}/skia.dll")
    message(FATAL_ERROR "Skia was found, but its Debug/Release runtime directory is incomplete")
  endif()
  add_custom_command(TARGET gpu2d_renderer_skia POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=$<IF:$<CONFIG:Debug>,${SKIA_RUNTIME_DEBUG_DIR},${SKIA_RUNTIME_RELEASE_DIR}>"
            "-DDESTINATION_DIR=$<TARGET_FILE_DIR:gpu2d_renderer_skia>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake"
    VERBATIM
  )
endif()

foreach(RENDERER_TARGET IN ITEMS gpu2d_renderer_d3d12 gpu2d_renderer_skia)
  if(TARGET ${RENDERER_TARGET})
    add_custom_command(TARGET ${RENDERER_TARGET} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${RENDERER_TARGET}>/shaders"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${CMAKE_CURRENT_SOURCE_DIR}/shaders/quad.hlsl"
              "$<TARGET_FILE_DIR:${RENDERER_TARGET}>/shaders/quad.hlsl"
      VERBATIM
    )
  endif()
endforeach()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT gpu_2d_demo)

if(BUILD_TESTING)
  add_executable(gpu2d_tests tests/javascript_page_tests.cpp)
  gpu2d_add_headers(gpu2d_tests
    include/gpu2d/builtin_examples.hpp
    include/gpu2d/canvas_frame.hpp
    include/gpu2d/javascript_page.hpp
  )
  gpu2d_configure_target(gpu2d_tests)
  target_link_libraries(gpu2d_tests PRIVATE gpu2d_script gpu2d_core)
endif()
