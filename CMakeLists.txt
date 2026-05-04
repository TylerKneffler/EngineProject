cmake_minimum_required(VERSION 3.20)
project(EngineProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Don't re-check for updates on packages already downloaded — prevents
# MSBuild-triggered cmake re-runs from failing on network/subbuild issues.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------------------
# Source auto-discovery — CONFIGURE_DEPENDS re-runs CMake configure when
# files are added or removed, keeping targets up to date automatically.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/Core/*.cpp"
)
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/Editor/*.cpp"
)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/Game/*.cpp"
)
file(GLOB_RECURSE ASSET_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Assets/*.cpp"
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

add_library(ImGui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(ImGui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(ImGui PUBLIC IMGUI_ENABLE_DOCKING)

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
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
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
    ENGINE_SHADERS_PATH="${CMAKE_SOURCE_DIR}/Engine/Core/Shaders/"
)

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
target_link_libraries(Editor PRIVATE Engine shell32.lib)
target_precompile_headers(Editor REUSE_FROM Engine)
target_include_directories(Editor PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Editor
    ${CMAKE_SOURCE_DIR}/Engine/Editor/Core
    ${CMAKE_SOURCE_DIR}/Assets
)
target_compile_definitions(Editor PRIVATE
    ASSETS_PATH="${CMAKE_SOURCE_DIR}/Assets/"
    PROJECT_FILE="${CMAKE_SOURCE_DIR}/Example_Proj.proj"
    ENGINE_BUILD_DIR="${CMAKE_BINARY_DIR}"
    ENGINE_ASSETS_PATH="${CMAKE_SOURCE_DIR}/Engine/Core/Assets/"
)
set_target_properties(Editor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

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
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Assets
)
target_compile_definitions(Game PRIVATE
    ASSETS_PATH="${CMAKE_SOURCE_DIR}/Assets/"
    PROJECT_FILE="${CMAKE_SOURCE_DIR}/Example_Proj.proj"
    ENGINE_ASSETS_PATH="${CMAKE_SOURCE_DIR}/Engine/Core/Assets/"
)
set_target_properties(Game PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)


