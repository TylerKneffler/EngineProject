cmake_minimum_required(VERSION 3.20)
project(EngineProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(ALL_HEADERS
    Engine/pch.h
    Engine/Core/Window.h
    Engine/Renderers/Game/DX12GameRenderer.h
    Engine/Renderers/Editor/DX12EditorRenderer.h
)

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
# Engine — static library
# ---------------------------------------------------------------------------
add_library(Engine STATIC
    Engine/Core/Window.cpp
    Engine/Renderers/Game/DX12GameRenderer.cpp
    Engine/Renderers/Editor/DX12EditorRenderer.cpp
    ${ALL_HEADERS}
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
)

# ---------------------------------------------------------------------------
# Compiler flags (MSVC)
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_definitions(Engine PUBLIC
        UNICODE
        _UNICODE
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<CONFIG:Release>:NDEBUG>
    )
    target_compile_options(Engine PRIVATE
        /W4
        /MP          # parallel compilation
        $<$<CONFIG:Release>:/O2 /GL>
    )
    target_link_options(Engine INTERFACE
        $<$<CONFIG:Release>:/LTCG>
    )
endif()

# ---------------------------------------------------------------------------
# Editor — primary executable (hosts the game view)
# ---------------------------------------------------------------------------
add_executable(Editor WIN32 Editor/Main.cpp)
target_link_libraries(Editor PRIVATE Engine)
target_precompile_headers(Editor REUSE_FROM Engine)
