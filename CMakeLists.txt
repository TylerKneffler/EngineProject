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
    Engine/Core/Renderers/Game/DX12GameRenderer.h
    Engine/Core/Renderers/Editor/DX12EditorRenderer.h
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
# Engine — static library
# ---------------------------------------------------------------------------
add_library(Engine STATIC
    Engine/Core/Window.cpp
    Engine/Core/Renderers/Game/DX12GameRenderer.cpp
    Engine/Core/Renderers/Editor/DX12EditorRenderer.cpp
    Engine/Core/component.cpp
    Engine/Core/Object.cpp
    Engine/Core/Scene/Scene.cpp
    Engine/Core/Compoonents/Transform.cpp
    Engine/Core/Compoonents/Mesh.cpp
    Engine/Core/Compoonents/Material.cpp
    Engine/Core/Compoonents/Camera.cpp
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
    glm::glm
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
add_executable(Editor WIN32
    Engine/Editor/Main.cpp
    Engine/Editor/Core/View/View.cpp
    Engine/Editor/Core/View/View.h
    Engine/Editor/Core/View/Views/SceneViewport.cpp
    Engine/Editor/Core/View/Views/SceneViewport.h
)
target_link_libraries(Editor PRIVATE Engine)
target_precompile_headers(Editor REUSE_FROM Engine)
target_include_directories(Editor PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Editor
    ${CMAKE_SOURCE_DIR}/Engine/Editor/Core
)
target_compile_definitions(Editor PRIVATE
    ASSETS_PATH="${CMAKE_SOURCE_DIR}/Assets/"
)
set_target_properties(Editor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
