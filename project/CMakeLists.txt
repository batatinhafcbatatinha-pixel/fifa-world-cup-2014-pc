# copa2014 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(copa2014 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Optional local codegen with the documented GapFill performance patch.
set(COPA_CODEGEN_EXECUTABLE "" CACHE FILEPATH "Compatible ReXGlue codegen executable")
if(COPA_CODEGEN_EXECUTABLE)
    if(NOT EXISTS "${COPA_CODEGEN_EXECUTABLE}")
        message(FATAL_ERROR "COPA_CODEGEN_EXECUTABLE does not exist")
    endif()
    set_property(TARGET rex::rexglue PROPERTY IMPORTED_LOCATION "${COPA_CODEGEN_EXECUTABLE}")
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_property(TARGET rex::rexglue PROPERTY IMPORTED_LOCATION_${config} "${COPA_CODEGEN_EXECUTABLE}")
    endforeach()
endif()

# Sources. Native graphics system (no GPU plugin) and native XMA audio; the D3D hooks live
# in the executable because the XDK D3D library is linked into default.xex.
set(COPA2014_SOURCES
    src/main.cpp
    src/kernel_extra.cpp
    src/menu_hooks.cpp
    src/pc_settings.cpp
    src/pc_theme.cpp
    src/pc_font.cpp
    src/native/d3d_hooks.cpp
    src/native/renderer.cpp
    src/native/shader_translator.cpp
    src/native/texture_loader.cpp
    src/native/native_audio.cpp
    src/diag_hooks.cpp
)

if(WIN32)
    add_executable(copa2014 WIN32 ${COPA2014_SOURCES})
else()
    add_executable(copa2014 ${COPA2014_SOURCES})
endif()
target_include_directories(copa2014 PRIVATE src src/native)

# No GPU_PLUGINS: the Xenos emulation plugin is intentionally not staged.
rexglue_setup_target(copa2014)

# XMA decoding with the SDK's prebuilt libavcodec (Xenia FFmpeg fork); headers from the
# matching commit in third_party/ffmpeg.
target_include_directories(copa2014 PRIVATE ../third_party/ffmpeg)
find_library(COPA_AVCODEC NAMES libavcodecrd libavcodec PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
find_library(COPA_AVUTIL NAMES libavutilrd libavutil PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
target_link_libraries(copa2014 PRIVATE ${COPA_AVCODEC} ${COPA_AVUTIL})
if(WIN32)
    target_link_libraries(copa2014 PRIVATE d3d11 dxgi xaudio2 ole32 bcrypt)
endif()

# Offline shader translator check (tool; never runs the game).
if(WIN32)
    add_executable(copa_shader_test tools/shader_test.cpp src/native/shader_translator.cpp)
    target_include_directories(copa_shader_test PRIVATE src ${CMAKE_PREFIX_PATH}/include)
    target_compile_definitions(copa_shader_test PRIVATE NOMINMAX)
endif()

# Hooks on functions of the UI module must be compiled into that module.
if(TARGET copa2014_powdllzf_xex)
    target_sources(copa2014_powdllzf_xex PRIVATE src/pow_hooks.cpp src/pc_settings.cpp src/pc_theme.cpp src/pc_font.cpp)
    target_include_directories(copa2014_powdllzf_xex PRIVATE src src/native)
    target_link_libraries(copa2014_powdllzf_xex PRIVATE d3d11 dxgi)
endif()

# DLL sources are emitted by the shared codegen command but are not listed as
# outputs in the SDK's entrypoint-only rule. A normal dependency makes Ninja
# recompile them after generation instead of shipping stale function tables.
foreach(module IN ITEMS FootballCompEngzf_xex powdllzf_xex)
    if(TARGET copa2014_${module})
        get_target_property(module_sources copa2014_${module} SOURCES)
        set_property(SOURCE ${module_sources} APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp")
    endif()
endforeach()

# Vulkan option of the settings menu: DXVK's d3d11.dll/dxgi.dll in vulkan\ next to the
# executable (pc_settings.cpp loads them by full path when "API grafica" is Vulkan).
set(DXVK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dxvk-3.1.1" CACHE PATH
    "DXVK release for Windows (x64/d3d11.dll, x64/dxgi.dll, LICENSE)")
if(WIN32 AND EXISTS "${DXVK_DIR}/x64/d3d11.dll")
    add_custom_command(TARGET copa2014 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:copa2014>/vulkan"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/x64/d3d11.dll" "${DXVK_DIR}/x64/dxgi.dll"
                "$<TARGET_FILE_DIR:copa2014>/vulkan/")
    if(EXISTS "${DXVK_DIR}/LICENSE")
        add_custom_command(TARGET copa2014 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/LICENSE"
                    "$<TARGET_FILE_DIR:copa2014>/vulkan/LICENSE.dxvk.txt")
    endif()
endif()
