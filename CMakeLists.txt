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
    set(ASSET_SOURCES)
    message(STATUS "[Project] Engine-only build. Editor.exe will open the project launcher.")
endif()

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

option(ENGINE_ENABLE_VULKAN "Build the Vulkan renderer" ON)
set(ENGINE_VULKAN_ENABLED OFF)

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
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Editor/*.cpp"
)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/Engine/Game/*.cpp"
)

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

# Engine — static library
# ---------------------------------------------------------------------------
add_library(Engine STATIC
    ${ENGINE_SOURCES}
)
target_include_directories(Engine PUBLIC
    ${ENGINE_ROOT}
    ${ENGINE_ROOT}/Engine
)
target_precompile_headers(Engine PRIVATE Engine/pch.h)
target_link_libraries(Engine PUBLIC
    d3d12.lib 
    dxgi.lib 
    d3dcompiler.lib 
    d3d11.lib 
    d2d1.lib 
    dwrite.lib
    ImGui
    glm::glm
    pugixml
)
target_compile_definitions(Engine PUBLIC
    ENGINE_SHADERS_PATH="${ENGINE_ROOT}/Engine/Core/Shaders/"
)

if(ENGINE_VULKAN_ENABLED)
    set(VULKAN_SHADER_DIR "${CMAKE_BINARY_DIR}/VulkanShaders")
    set(VULKAN_SHADER_OUTPUTS)
    foreach(SHADER_NAME Grid Object)
        foreach(STAGE VS PS)
            if(STAGE STREQUAL "VS")
                set(SHADER_PROFILE vs_6_0)
                set(SHADER_ENTRY VSMain)
            else()
                set(SHADER_PROFILE ps_6_0)
                set(SHADER_ENTRY PSMain)
            endif()
            set(SHADER_SOURCE "${ENGINE_ROOT}/Engine/Core/Shaders/${SHADER_NAME}.hlsl")
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
        ENGINE_VULKAN_SHADER_PATH="${VULKAN_SHADER_DIR}/"
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
    ${ASSET_SOURCES}
)
target_link_libraries(Editor PRIVATE Engine shell32.lib ole32.lib comdlg32.lib)
target_precompile_headers(Editor REUSE_FROM Engine)
target_include_directories(Editor PRIVATE
    ${ENGINE_ROOT}/Engine
    ${ENGINE_ROOT}/Engine/Editor
    ${ENGINE_ROOT}/Engine/Editor/Core
    ${ENGINE_PROJECT_DIR}/Assets
)
target_compile_definitions(Editor PRIVATE
    ASSETS_PATH="${ENGINE_PROJECT_DIR}/Assets/"
    PROJECT_FILE="${ENGINE_PROJECT_FILE}"
    ENGINE_BUILD_DIR="${CMAKE_BINARY_DIR}"
    ENGINE_ASSETS_PATH="${ENGINE_ROOT}/Engine/Core/Assets/"
    ENGINE_ROOT_PATH="${ENGINE_ROOT}"
    GAME_EXECUTABLE_PATH="${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Game.exe"
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
    ${ASSET_SOURCES}
)
target_link_libraries(Game PRIVATE Engine)
target_precompile_headers(Game REUSE_FROM Engine)
target_include_directories(Game PRIVATE
    ${ENGINE_ROOT}/Engine
    ${ENGINE_PROJECT_DIR}/Assets
)
target_compile_definitions(Game PRIVATE
    ASSETS_PATH="${ENGINE_PROJECT_DIR}/Assets/"
    PROJECT_FILE="${ENGINE_PROJECT_FILE}"
    ENGINE_ASSETS_PATH="${ENGINE_ROOT}/Engine/Core/Assets/"
)
set_target_properties(Game PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_RUNTIME_WORKING_DIRECTORY}"
)


