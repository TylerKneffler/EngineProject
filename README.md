# EngineProject

EngineProject is a C++17 Windows game engine with a package-neutral editor UI, scene serialization, a standalone game runtime, and support for DirectX 11, DirectX 12, and Vulkan renderers.

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.20+
- Git and an internet connection for the first build

DirectX is included with Windows. Vulkan build dependencies are downloaded automatically; running Vulkan requires a supported GPU and current driver.

## Build and start the engine

```powershell
git clone https://github.com/TylerKneffler/EngineProject.git
cd EngineProject
cmake --preset debug
cmake --build --preset debug --target Editor --parallel
.\build\Debug\Debug\Editor.exe
```

The first build may take a few minutes while dependencies download.

When started from the engine repository, the Editor opens **Engine Sandbox** using `Engine/Core/Assets`. This allows F5 and direct engine testing without creating a project.

## Create or open a project

Open the Project Hub from the engine repository with:

```powershell
.\build\Debug\Debug\Editor.exe --project-hub
```

Outside the engine repository, starting the Editor without a project also opens the Project Hub. It can:

- Create, build, and open a new project.
- Open recent or existing `.proj` files.
- Remove projects from the recent list.
- Permanently delete generated projects after confirmation.

Project creation runs in the background and displays setup progress. Build output is saved as `project-build.log` in the project folder.

Each new project contains its own `Assets/` directory and an `Open <Project Name> Editor.lnk` shortcut. Double-click the shortcut to reopen the project.

## Build a project manually

Run these commands from the generated project folder:

```powershell
cmake --preset debug
cmake --build --preset debug --parallel
```

Start the project Editor or standalone game:

```powershell
.\build\Debug\Engine\Debug\Editor.exe
.\build\Debug\Engine\Debug\Game.exe
```

For a distributable build, open **File > Project Preferences > Export** and select **Build Portable Export**. The packaged game is written to the project's `Export/` folder with its assets, settings, and shaders. Build details are saved in `export-build.log`.

Open the repository or generated project folder directly in Visual Studio 2022. Visual Studio detects `CMakePresets.json`, and the generated solution remains in `build/Debug`. The CMake project sets `Editor` as the startup target. VS Code provides **Engine: Start Editor** and **Engine: Open Project Hub** tasks.

## Assets

- `Engine/Core/Assets/` contains engine-owned starter content.
- A new project receives a copy in its own `Assets/` directory.
- Add project scenes, meshes, textures, and C++ scripts only to the project's `Assets/` directory.
- Store asset paths relative to the project root, such as `Assets/Mesh/player.obj`.

## Renderers

Choose the editor and game renderers under **File > Project Preferences > Rendering**:

- DirectX 11
- DirectX 12
- Vulkan

Unavailable renderers are disabled and show the reason. Restart the Editor after changing its renderer.

## Editor UI backends

Editor UI packages live under `Engine/Editor/UI`. `IEditorUi` is the shared
widget/layout facade used by every panel in `Engine/Editor/Core/View`, while
`IEditorUiBackend` owns package lifecycle, input, and frame submission. Engine
renderers know only package-neutral frame and texture callbacks; `Engine/Core`
does not include or link a UI library.

Dear ImGui remains the default:

```powershell
cmake --preset debug
cmake --build --preset debug
```

Nuklear is the interchangeable demonstration backend. Both packages are built
into the editor and support DirectX 11, DirectX 12, and Vulkan:

```powershell
cmake --preset nuklear
cmake --build --preset nuklear
```

The CMake option chooses the startup package only. Switch live with the **UI**
menu, Nuklear's **Use ImGui** button, or `Ctrl+Shift+U`; changes are committed at
the frame boundary so viewport textures stay valid.

Add another package by implementing `IEditorUi` plus its `IEditorUiBackend`
integration under `Engine/Editor/UI/<Package>`. UI backends are initialized
before editor panels so APIs that register scene textures during panel
construction are safe.

To build without Vulkan support:

```powershell
cmake --preset debug -DENGINE_ENABLE_VULKAN=OFF
```

## Troubleshooting

- **Build failed:** Check `project-build.log` in the project folder.
- **Editor failed to start:** Check `editor-startup.log` in the project folder.
- **Dependencies failed to download:** Check the network connection and rerun CMake.
- **Assets or shaders are missing:** Start the project Editor from the project folder or use its generated shortcut.
- **Renderer unavailable:** Update Windows and the GPU driver.
- **Corrupted incremental build:** Close the Editor and Visual Studio, delete the project's `build/` directory, then configure and build again.
