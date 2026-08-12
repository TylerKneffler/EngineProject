cmake_minimum_required(VERSION 3.20)
project(EngineProject C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This CMake project can be built directly for engine development or consumed
# by a generated game project via add_subdirectory(). Generated projects set
# ENGINE_PROJECT_DIR and ENGINE_PROJECT_FILE before referencing the engine.
set(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT DEFINED ENGINE_PROJECT_DIR)
    set(ENGINE_PROJECT_DIR "" CACHE PATH "Game project root")
endif()
if(NOT DEFINED ENGINE_PROJECT_FILE)
    set(ENGINE_PROJECT_FILE "" CACHE FILEPATH "Game project .proj file")
endif()

if(ENGINE_PROJECT_DIR)
    cmake_path(ABSOLUTE_PATH ENGINE_PROJECT_DIR NORMALIZE)
    set(ENGINE_RUNTIME_WORKING_DIRECTORY "${ENGINE_PROJECT_DIR}")
    file(GLOB_RECURSE ASSET_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_PROJECT_DIR}/Assets/*.cpp"
    )
else()
    set(ENGINE_RUNTIME_WORKING_DIRECTORY "${ENGINE_ROOT}")
    file(GLOB_RECURSE ASSET_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_ROOT}/Engine/Core/Assets/*.cpp"
    )
    message(STATUS "[Project] Engine-only build. Editor.exe will open the Engine Sandbox from the engine root.")
endif()

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

option(ENGINE_ENABLE_VULKAN "Build the Vulkan renderer" ON)
option(ENGINE_PORTABLE_EXPORT "Link the game with the static MSVC runtime for portable exports" OFF)
option(ENGINE_DISTRIBUTION_BUILD "Use paths relative to a packaged engine release" OFF)
if(MSVC AND ENGINE_PORTABLE_EXPORT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(ENGINE_VULKAN_ENABLED OFF)

if(ENGINE_DISTRIBUTION_BUILD)
    set(ENGINE_COMPILED_ROOT_PATH ".")
    set(ENGINE_COMPILED_ASSETS_PATH "Engine/Core/Assets/")
    set(ENGINE_COMPILED_SHADERS_PATH "Engine/Core/Shaders/")
    set(ENGINE_COMPILED_BUILD_DIR ".")
    set(ENGINE_COMPILED_GAME_PATH "Game.exe")
else()
    set(ENGINE_COMPILED_ROOT_PATH "${ENGINE_ROOT}")
    set(ENGINE_COMPILED_ASSETS_PATH "${ENGINE_ROOT}/Engine/Core/Assets/")
    set(ENGINE_COMPILED_SHADERS_PATH "${ENGINE_ROOT}/Engine/Core/Shaders/")
    set(ENGINE_COMPILED_BUILD_DIR "${CMAKE_BINARY_DIR}")
    set(ENGINE_COMPILED_GAME_PATH "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Game.exe")
endif()

if(ENGINE_ENABLE_VULKAN)
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG        v1.4.341
    )
    FetchContent_MakeAvailable(VulkanHeaders)

    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG        vulkan-sdk-1.4.341.0
    )
    FetchContent_MakeAvailable(volk)
    target_link_libraries(volk PUBLIC Vulkan::Headers)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)

    set(DXC_SPIRV_EXECUTABLE "" CACHE FILEPATH "DXC executable with SPIR-V CodeGen")
    if(NOT DXC_SPIRV_EXECUTABLE)
        FetchContent_Declare(
            dxc_spirv
            URL      https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip
            URL_HASH SHA256=cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(dxc_spirv)
        set(DXC_SPIRV_EXECUTABLE "${dxc_spirv_SOURCE_DIR}/bin/x64/dxc.exe")
    endif()

    if(EXISTS "${DXC_SPIRV_EXECUTABLE}")
        set(ENGINE_VULKAN_ENABLED ON)
        message(STATUS "[Renderer] Vulkan enabled; SPIR-V DXC: ${DXC_SPIRV_EXECUTABLE}")
    else()
        message(WARNING "[Renderer] Vulkan disabled: SPIR-V DXC was not found. Set DXC_SPIRV_EXECUTABLE to override it.")
    endif()
endif()

# Don't re-check for updates on packages already downloaded — prevents
# MSBuild-triggered cmake re-runs from failing on network/subbuild issues.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------------------
# Source auto-discovery — CONFIGURE_DEPENDS re-runs CMake configure when
# files are added or removed, keeping targets up to date automatically.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Core/*.cpp"
)
# Engine/Core/Assets contains project template content, not engine code.
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "[/\\\\]Assets[/\\\\]")
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Core/*.h"
    "${ENGINE_ROOT}/Engine/Core/*.hpp"
    "${ENGINE_ROOT}/Engine/Core/*.inl"
)
file(GLOB_RECURSE ENGINE_RESOURCE_FILES CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Core/Assets/*"
    "${ENGINE_ROOT}/Engine/Core/Shaders/*"
)
# Built-in asset scripts are compiled into the root development Editor/Game.
# Keep only non-source assets in the Engine library's browse-only list.
list(FILTER ENGINE_RESOURCE_FILES EXCLUDE REGEX "\\.cpp$")
list(APPEND ENGINE_HEADERS "${ENGINE_ROOT}/Engine/pch.h")
set_source_files_properties(${ENGINE_RESOURCE_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Editor/*.cpp"
)
file(GLOB_RECURSE EDITOR_HEADERS CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Editor/*.h"
    "${ENGINE_ROOT}/Engine/Editor/*.hpp"
    "${ENGINE_ROOT}/Engine/Editor/*.inl"
)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Game/*.cpp"
)
file(GLOB_RECURSE GAME_HEADERS CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Game/*.h"
    "${ENGINE_ROOT}/Engine/Game/*.hpp"
    "${ENGINE_ROOT}/Engine/Game/*.inl"
)

set(PROJECT_BROWSE_FILES)
set(PROJECT_ROOT_FILES)
if(ENGINE_PROJECT_DIR)
    file(GLOB_RECURSE PROJECT_BROWSE_FILES CONFIGURE_DEPENDS
        "${ENGINE_PROJECT_DIR}/Assets/*"
    )
    list(FILTER PROJECT_BROWSE_FILES EXCLUDE REGEX "\\.cpp$")
    set_source_files_properties(${PROJECT_BROWSE_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
    set(PROJECT_ROOT_FILES
        "${ENGINE_PROJECT_DIR}/CMakeLists.txt"
        "${ENGINE_PROJECT_FILE}"
    )
    set_source_files_properties(${PROJECT_ROOT_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

# ---------------------------------------------------------------------------
# GLM — Git library 
# ---------------------------------------------------------------------------
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# ---------------------------------------------------------------------------
# IMGUI — Git library
# ---------------------------------------------------------------------------

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
if(ENGINE_VULKAN_ENABLED)
    list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
endif()
add_library(ImGui STATIC ${IMGUI_SOURCES})
target_include_directories(ImGui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(ImGui PUBLIC IMGUI_ENABLE_DOCKING)
if(ENGINE_VULKAN_ENABLED)
    target_link_libraries(ImGui PUBLIC volk::volk Vulkan::Headers)
    target_compile_definitions(ImGui PUBLIC VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES)
endif()

message(STATUS "[Editor UI] Dear ImGui")

# ---------------------------------------------------------------------------
# pugixml — XML parsing library
# ---------------------------------------------------------------------------
FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG        v1.14
)
FetchContent_MakeAvailable(pugixml)

# ---------------------------------------------------------------------------
# fastgltf — editor-time glTF/GLB import
# ---------------------------------------------------------------------------
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG        v0.9.0
)
FetchContent_MakeAvailable(fastgltf)

# ---------------------------------------------------------------------------
# Assimp - editor-time FBX decoding. Imported data is immediately converted to
# the engine's format-neutral ImportedModel representation.
# ---------------------------------------------------------------------------
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
)
FetchContent_MakeAvailable(assimp)

# Texture codecs. stb_image is supplied by the pinned Assimp checkout;
# TinyEXR handles floating-point EXR and libktx handles KTX2/BasisU.
set(TINYEXR_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG        v1.0.12
)
FetchContent_MakeAvailable(tinyexr)

set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_KTX1 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_KTX2 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_ETC_UNPACK ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ktx
    GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
    GIT_TAG        v4.4.2
)
FetchContent_MakeAvailable(ktx)

# Audio decoding, mixing and spatialization. miniaudio includes native WAV/MP3
# decoding; its pinned stb_vorbis extra supplies OGG/Vorbis decoding.
set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_LIBOPUS ON CACHE BOOL "" FORCE)
set(MINIAUDIO_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.25
)
FetchContent_MakeAvailable(miniaudio)
set_source_files_properties(${miniaudio_SOURCE_DIR}/extras/stb_vorbis.c PROPERTIES
    COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/w>"
    SKIP_PRECOMPILE_HEADERS ON
)

# Runtime rigid-body collision and dynamics.
set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET3 OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "" FORCE)
set(USE_MSVC_DISABLE_RTTI OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    bullet
    GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git
    GIT_TAG        3.25
)
FetchContent_MakeAvailable(bullet)

# ---------------------------------------------------------------------------

# Engine — static library
# ---------------------------------------------------------------------------
add_library(Engine STATIC
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
    ${ENGINE_RESOURCE_FILES}
    ${miniaudio_SOURCE_DIR}/extras/stb_vorbis.c
)
source_group(TREE "${ENGINE_ROOT}" FILES
    ${ENGINE_SOURCES} ${ENGINE_HEADERS} ${ENGINE_RESOURCE_FILES})
target_include_directories(Engine PUBLIC
    ${ENGINE_ROOT}
    ${ENGINE_ROOT}/Engine
)
target_include_directories(Engine PRIVATE
    ${assimp_SOURCE_DIR}/contrib/stb
    ${imgui_SOURCE_DIR}
    ${tinyexr_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}/extras
    ${bullet_SOURCE_DIR}/src
)
target_precompile_headers(Engine PRIVATE Engine/pch.h)
target_link_libraries(Engine PUBLIC
    d3d12.lib 
    dxgi.lib 
    d3dcompiler.lib 
    d3d11.lib 
    d2d1.lib 
    dwrite.lib
    windowscodecs.lib
    glm::glm
    pugixml
    tinyexr
    ktx_read
    miniaudio
    BulletDynamics
    BulletCollision
    BulletSoftBody
    LinearMath
)
target_compile_definitions(Engine PUBLIC
    ENGINE_SHADERS_PATH="${ENGINE_COMPILED_SHADERS_PATH}"
    ENGINE_ASSETS_PATH="${ENGINE_COMPILED_ASSETS_PATH}"
)

if(ENGINE_VULKAN_ENABLED)
    set(VULKAN_SHADER_DIR "${CMAKE_BINARY_DIR}/VulkanShaders")
    set(VULKAN_SHADER_OUTPUTS)
    foreach(SHADER_NAME Grid Object ObjectOutline Skybox UI)
        foreach(STAGE VS PS)
            if(STAGE STREQUAL "VS")
                set(SHADER_PROFILE vs_6_0)
                set(SHADER_ENTRY VSMain)
            else()
                set(SHADER_PROFILE ps_6_0)
                set(SHADER_ENTRY PSMain)
            endif()
            set(SHADER_SOURCE
                "${ENGINE_ROOT}/Engine/Core/Shaders/${SHADER_NAME}/${SHADER_NAME}.hlsl")
            set(SHADER_OUTPUT "${VULKAN_SHADER_DIR}/${SHADER_NAME}.${STAGE}.spv")
            add_custom_command(
                OUTPUT "${SHADER_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${VULKAN_SHADER_DIR}"
                COMMAND "${DXC_SPIRV_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.1
                        -fvk-use-dx-layout -D VULKAN=1 -T ${SHADER_PROFILE}
                        -E ${SHADER_ENTRY} -Fo "${SHADER_OUTPUT}" "${SHADER_SOURCE}"
                DEPENDS "${SHADER_SOURCE}"
                VERBATIM
                COMMENT "Compiling ${SHADER_NAME}.${STAGE} for Vulkan"
            )
            list(APPEND VULKAN_SHADER_OUTPUTS "${SHADER_OUTPUT}")
        endforeach()
    endforeach()
    add_custom_target(VulkanShaders DEPENDS ${VULKAN_SHADER_OUTPUTS})
    add_dependencies(Engine VulkanShaders)
    target_link_libraries(Engine PUBLIC volk::volk Vulkan::Headers)
    target_compile_definitions(Engine PUBLIC
        ENGINE_VULKAN_ENABLED
        VK_USE_PLATFORM_WIN32_KHR
        VK_NO_PROTOTYPES
        ENGINE_VULKAN_SHADER_PATH="$<IF:$<BOOL:${ENGINE_DISTRIBUTION_BUILD}>,VulkanShaders/,${VULKAN_SHADER_DIR}/>"
    )
endif()

# ---------------------------------------------------------------------------
# Compiler flags (MSVC)
# ---------------------------------------------------------------------------
if(MSVC)
    # /FS is required globally when MSBuild builds targets in parallel — without
    # it multiple cl.exe processes race to write the same .pdb and corrupt it.
    add_compile_options(/FS)
    target_compile_definitions(Engine PUBLIC
        UNICODE
        _UNICODE
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<CONFIG:Release>:NDEBUG>
    )
    target_compile_options(Engine PRIVATE
        /W4
        /MP          # parallel compilation
        /FS          # force synchronous PDB writes (required with /MP)
        $<$<CONFIG:Release>:/O2 /GL>
    )
    target_link_options(Engine INTERFACE
        $<$<CONFIG:Release>:/LTCG>
    )
endif()

# ---------------------------------------------------------------------------
# Editor — primary executable (hosts the game view)
# ---------------------------------------------------------------------------
add_executable(Editor WIN32
    ${EDITOR_SOURCES}
    ${EDITOR_HEADERS}
    ${ASSET_SOURCES}
    ${PROJECT_BROWSE_FILES}
    ${PROJECT_ROOT_FILES}
)

# Project scripts are also built as a reloadable module. The editor loads a
# versioned copy so this canonical output remains writable on the next edit.
add_library(ProjectScripts SHARED
    ${ASSET_SOURCES}
    "${ENGINE_ROOT}/Engine/Editor/HotReload/ProjectScriptModule.cpp"
)
target_link_libraries(ProjectScripts PRIVATE Engine)
target_include_directories(ProjectScripts PRIVATE
    ${ENGINE_ROOT}
    ${ENGINE_ROOT}/Engine
    ${ENGINE_ROOT}/Engine/Editor
    ${ENGINE_ROOT}/Engine/Core/Assets
    ${ENGINE_PROJECT_DIR}/Assets
)
set_target_properties(ProjectScripts PROPERTIES PREFIX "" OUTPUT_NAME "ProjectScripts")
target_precompile_headers(ProjectScripts REUSE_FROM Engine)
add_dependencies(Editor ProjectScripts)
source_group(TREE "${ENGINE_ROOT}" FILES ${EDITOR_SOURCES} ${EDITOR_HEADERS})
if(ENGINE_PROJECT_DIR)
    source_group(TREE "${ENGINE_PROJECT_DIR}" PREFIX "Project" FILES
        ${ASSET_SOURCES} ${PROJECT_BROWSE_FILES} ${PROJECT_ROOT_FILES})
endif()
target_link_libraries(Editor PRIVATE Engine ImGui fastgltf::fastgltf assimp::assimp shell32.lib ole32.lib comdlg32.lib)
target_precompile_headers(Editor REUSE_FROM Engine)
target_include_directories(Editor PRIVATE
    ${ENGINE_ROOT}/Engine
    ${ENGINE_ROOT}/Engine/Editor
    ${ENGINE_ROOT}/Engine/Editor/Core
    ${ENGINE_ROOT}/Engine/Core/Assets
    ${ENGINE_PROJECT_DIR}/Assets
)
target_compile_definitions(Editor PRIVATE
    ASSETS_PATH="${ENGINE_PROJECT_DIR}/Assets/"
    PROJECT_FILE="${ENGINE_PROJECT_FILE}"
    ENGINE_BUILD_DIR="${ENGINE_COMPILED_BUILD_DIR}"
    ENGINE_ASSETS_PATH="${ENGINE_COMPILED_ASSETS_PATH}"
    ENGINE_ROOT_PATH="${ENGINE_COMPILED_ROOT_PATH}"
    GAME_EXECUTABLE_PATH="${ENGINE_COMPILED_GAME_PATH}"
    PROJECT_SCRIPTS_PATH="$<TARGET_FILE:ProjectScripts>"
    ENGINE_EDITOR_UI_IMGUI
    $<$<NOT:$<BOOL:${ENGINE_PROJECT_DIR}>>:ENGINE_BUILTIN_ASSET_SCRIPTS>
)
set_target_properties(Editor PROPERTIES
    VS_DEBUGGER_COMMAND "$<TARGET_FILE:Editor>"
    VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_RUNTIME_WORKING_DIRECTORY}"
)

# Make Visual Studio's green Play button / F5 build and launch the editor by
# default. Setting this on the top-level directory also applies when the engine
# is referenced by a generated project's add_subdirectory().
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Editor)

# ---------------------------------------------------------------------------
# Game — standalone runtime built when pressing Play in the editor
# ---------------------------------------------------------------------------
add_executable(Game WIN32
    ${GAME_SOURCES}
    ${GAME_HEADERS}
    ${ASSET_SOURCES}
    ${PROJECT_BROWSE_FILES}
)
source_group(TREE "${ENGINE_ROOT}" FILES ${GAME_SOURCES} ${GAME_HEADERS})
if(ENGINE_PROJECT_DIR)
    source_group(TREE "${ENGINE_PROJECT_DIR}" PREFIX "Project" FILES
        ${ASSET_SOURCES} ${PROJECT_BROWSE_FILES})
endif()
target_link_libraries(Game PRIVATE Engine comctl32.lib)
target_precompile_headers(Game REUSE_FROM Engine)
target_include_directories(Game PRIVATE
    ${ENGINE_ROOT}/Engine
    ${ENGINE_ROOT}/Engine/Core/Assets
    ${ENGINE_PROJECT_DIR}/Assets
)
target_compile_definitions(Game PRIVATE
    ASSETS_PATH="${ENGINE_PROJECT_DIR}/Assets/"
    PROJECT_FILE="${ENGINE_PROJECT_FILE}"
    ENGINE_ASSETS_PATH="${ENGINE_COMPILED_ASSETS_PATH}"
    $<$<NOT:$<BOOL:${ENGINE_PROJECT_DIR}>>:ENGINE_BUILTIN_ASSET_SCRIPTS>
)
set_target_properties(Game PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_RUNTIME_WORKING_DIRECTORY}"
)

if(NOT ENGINE_PROJECT_DIR AND
   EXISTS "${ENGINE_ROOT}/Engine/Tests/RotateLifecycleTests.cpp")
    enable_testing()
    add_executable(EngineLifecycleTests
        "${ENGINE_ROOT}/Engine/Tests/RotateLifecycleTests.cpp"
        "${ENGINE_ROOT}/Engine/Core/Assets/Scripts/Rotate.cpp"
    )
    target_link_libraries(EngineLifecycleTests PRIVATE Engine)
    target_include_directories(EngineLifecycleTests PRIVATE
        "${ENGINE_ROOT}/Engine"
        "${ENGINE_ROOT}/Engine/Core/Assets"
    )
    target_compile_definitions(EngineLifecycleTests PRIVATE
        ENGINE_ASSETS_PATH="${ENGINE_ROOT}/Engine/Core/Assets/"
    )
    add_test(NAME RotateDefaultSceneLifecycle COMMAND EngineLifecycleTests)
    set_tests_properties(RotateDefaultSceneLifecycle PROPERTIES
        WORKING_DIRECTORY "${ENGINE_ROOT}"
    )
endif()

if(NOT ENGINE_PROJECT_DIR AND
   EXISTS "${ENGINE_ROOT}/Engine/Tests/TexturePipelineTests.cpp")
    enable_testing()
    add_executable(TexturePipelineTests
        "${ENGINE_ROOT}/Engine/Tests/TexturePipelineTests.cpp")
    target_link_libraries(TexturePipelineTests PRIVATE Engine)
    target_include_directories(TexturePipelineTests PRIVATE "${ENGINE_ROOT}/Engine")
    add_test(NAME TextureDecodingAndMipGeneration COMMAND TexturePipelineTests
        "${assimp_SOURCE_DIR}/test/models/Collada/duckCM.tga"
        "${ktx_SOURCE_DIR}/external/astc-encoder/Test/Data/Tiles/hdr.hdr"
        "${tinyexr_SOURCE_DIR}/test/unit/regression/2by2.exr"
        "${ktx_SOURCE_DIR}/tests/cts/clitests/golden/create/compare/output_uastc_0_psnr_2d_r8g8b8a8_unorm.ktx2")
    set_tests_properties(TextureDecodingAndMipGeneration PROPERTIES
        WORKING_DIRECTORY "${ENGINE_ROOT}")
endif()

if(NOT ENGINE_PROJECT_DIR AND
   EXISTS "${ENGINE_ROOT}/Engine/Tests/AudioPipelineTests.cpp")
    enable_testing()
    add_executable(AudioPipelineTests
        "${ENGINE_ROOT}/Engine/Tests/AudioPipelineTests.cpp")
    target_link_libraries(AudioPipelineTests PRIVATE Engine)
    target_include_directories(AudioPipelineTests PRIVATE "${ENGINE_ROOT}/Engine")
    target_link_options(AudioPipelineTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/INCREMENTAL:NO>)
    add_test(NAME AudioDecodingMixingAndSerialization COMMAND AudioPipelineTests
        "${ENGINE_ROOT}/Engine/Core/Assets/Audio/TestTone.wav"
        "${ENGINE_ROOT}/Engine/Core/Assets/Audio/TestTone.ogg"
        "${ENGINE_ROOT}/Engine/Core/Assets/Audio/TestTone.mp3")
    set_tests_properties(AudioDecodingMixingAndSerialization PROPERTIES
        WORKING_DIRECTORY "${ENGINE_ROOT}")
endif()

if(NOT ENGINE_PROJECT_DIR AND
   EXISTS "${ENGINE_ROOT}/Engine/Tests/PhysicsPipelineTests.cpp")
    enable_testing()
    add_executable(PhysicsPipelineTests
        "${ENGINE_ROOT}/Engine/Tests/PhysicsPipelineTests.cpp")
    target_link_libraries(PhysicsPipelineTests PRIVATE Engine)
    target_include_directories(PhysicsPipelineTests PRIVATE "${ENGINE_ROOT}/Engine")
    target_link_options(PhysicsPipelineTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/INCREMENTAL:NO>)
    add_test(NAME RigidBodyAndColliderSimulation COMMAND PhysicsPipelineTests
        "${ENGINE_ROOT}/Engine/Core/Assets/Mesh/cube.obj")
    set_tests_properties(RigidBodyAndColliderSimulation PROPERTIES
        WORKING_DIRECTORY "${ENGINE_ROOT}")
endif()

if(NOT ENGINE_PROJECT_DIR AND
   EXISTS "${ENGINE_ROOT}/Engine/Tests/UILayoutTests.cpp")
    enable_testing()
    add_executable(UILayoutTests
        "${ENGINE_ROOT}/Engine/Tests/UILayoutTests.cpp")
    target_link_libraries(UILayoutTests PRIVATE Engine)
    target_include_directories(UILayoutTests PRIVATE "${ENGINE_ROOT}/Engine")
    target_link_options(UILayoutTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/INCREMENTAL:NO>)
    add_test(NAME RetainedUILayoutAndSerialization COMMAND UILayoutTests)
    set_tests_properties(RetainedUILayoutAndSerialization PROPERTIES
        WORKING_DIRECTORY "${ENGINE_ROOT}")
endif()
