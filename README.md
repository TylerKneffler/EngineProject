# EngineProject

EngineProject is a Windows game-engine and editor project written in C++17. It includes a dockable ImGui editor, scene serialization, a standalone game runtime, and selectable Direct3D 12, Direct3D 11, and Vulkan rendering backends.

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2022 with the **Desktop development with C++** workload
- A recent Windows SDK
- CMake 3.20 or newer
- Git
- An internet connection during the first CMake configure so CMake can download GLM, Dear ImGui, and pugixml

No separately installed graphics SDK is required. Direct3D is provided by Windows, and Vulkan headers, volk, and a SPIR-V-capable DXC package are downloaded automatically during the first CMake configure. Running Vulkan requires a Vulkan-capable GPU and a current driver that provides `vulkan-1.dll`. The editor checks the runtime, GPU, and graphics driver before allowing a renderer to be selected.

## Clone

```powershell
git clone https://github.com/TylerKneffler/EngineProject.git
cd EngineProject
```

If Git reports `detected dubious ownership` for an existing checkout, trust only this repository path:

```powershell
git config --global --add safe.directory "C:/path/to/EngineProject"
```

## Create a project

The engine repository contains no root `Assets` directory or embedded game project. Build and start the engine editor:

```powershell
cmake -S . -B build/Debug -G "Visual Studio 17 2022" -A x64
cmake --build build/Debug --config Debug --target Editor --parallel
.\build\Debug\Debug\Editor.exe
```

When no valid `.proj` exists in the working directory, `Editor.exe` opens the Project Hub. From there you can:

- Create a project in a new folder.
- Open an existing `.proj` file.
- Reopen projects recorded in the recent-project list.
- Remove an entry from the recent-project list without touching its files, or permanently delete a generated project after confirmation.

New projects reference this engine checkout from their generated `CMakeLists.txt` and `.proj` file. Starter content from `Engine/Core/Assets` is copied into the new project's `Assets` directory, where it becomes project-owned.

After Create or Open is selected, the Project Hub launches that project's own Debug `Editor.exe`. If it has not been built yet, the hub configures and builds it in the background first. Build output is written to `project-build.log` in the project folder; the hub remains open and reports the log path if configuration or compilation fails.

While a project is being created, the Project Setup view shows progress through asset copying, project-file generation, CMake configuration, and the initial Editor build.

Each newly created project also has an `Open <Project Name> Editor.lnk` shortcut in its root folder. Double-click it to start that project's Editor directly with the correct project file and working directory.

If exactly one `.proj` exists in the working directory, the editor opens it automatically. You can also pass a project explicitly:

```powershell
.\Editor.exe "C:\Projects\MyGame\MyGame.proj"
```

After creating a project, build from its folder so project-owned C++ scripts are linked into its Editor and Game executables.

## Build a project

Run these commands from the generated project folder.

### Debug

Configure a Visual Studio x64 build:

```powershell
cmake -S . -B build/Debug -G "Visual Studio 17 2022" -A x64
```

Build the editor and standalone game:

```powershell
cmake --build build/Debug --config Debug --parallel
```

To build only one executable:

```powershell
cmake --build build/Debug --config Debug --target Editor --parallel
cmake --build build/Debug --config Debug --target Game --parallel
```

### Release

```powershell
cmake -S . -B build/Release -G "Visual Studio 17 2022" -A x64
cmake --build build/Release --config Release --parallel
```

The first configure may take a few minutes while dependencies are downloaded. Later configurations use the project build directory's cached dependency sources.

## Start the engine editor

The editor must be started with the generated project folder as its working directory because project scenes and assets are resolved from there.

Debug:

```powershell
.\build\Debug\Engine\Debug\Editor.exe
```

Release:

```powershell
.\build\Release\Engine\Release\Editor.exe
```

You can also open the generated solution under `build/Debug`. `Editor` is configured as the startup project automatically: Visual Studio's green Play button or F5 builds it when needed and launches it with the generated project folder as its working directory. An engine-only solution launches the Project Hub instead.

## Start the standalone game

Build the `Game` target first, then run:

```powershell
.\build\Debug\Engine\Debug\Game.exe
```

For a Release build:

```powershell
.\build\Release\Engine\Release\Game.exe
```

The editor's **File** menu also provides build-and-run commands for running in the editor or launching the standalone game.

## Project and engine assets

Each generated project owns its own `Assets/` directory. Add project scenes, meshes, textures, and scripts there. The generated `.proj` points the editor at this directory, and the generated CMake build compiles project-owned `.cpp` scripts.

`Engine/Core/Assets/` contains immutable starter content owned by the engine. The Project Hub copies it into each new project's `Assets/` directory. Project-specific content should never be added to the engine copy.

Asset paths stored in scene files must be relative to the project working directory. For example:

```text
Assets/Mesh/player.obj
Assets/Mesh/cube.obj
```

## Select a renderer

Open **File > Project Preferences > Rendering** and choose separate renderers for the editor and game. Available options are:

- DirectX 12
- DirectX 11
- Vulkan

An unavailable renderer is disabled and displays the reason. Save the project settings after changing a selection. Changing the editor renderer requires restarting the editor.

The same options can be configured directly in the generated `<ProjectName>.proj` file:

```xml
<EditorRenderingAPI>DirectX12</EditorRenderingAPI>
<GameRenderingAPI>DirectX12</GameRenderingAPI>
```

Use `DirectX11` instead to select the Direct3D 11 backend.

Use `Vulkan` to select the Vulkan backend. To omit all Vulkan build dependencies, configure with:

```powershell
cmake -S . -B build/Debug -G "Visual Studio 17 2022" -A x64 -DENGINE_ENABLE_VULKAN=OFF
```

## VS Code tasks

Every generated project includes `.vscode/tasks.json`. Open the generated project folder in VS Code, then run **Tasks: Run Task > Engine: Start Editor** to configure, build, and launch with the correct working directory.

## Common problems

### CMake cannot download dependencies

Check the network connection and rerun the configure command. Once downloaded, dependencies are cached under the selected build directory.

### A renderer is unavailable

Update the GPU driver and install current Windows updates. DirectX 12 requires a Direct3D 12-capable GPU; Vulkan requires a Vulkan-capable GPU driver that installs the Windows Vulkan loader. The full Vulkan SDK is not required. The editor's build console and debugger output include the detected status and failure reason for each renderer.

### Shaders, assets, or the project file cannot be found

Start `Editor.exe` or `Game.exe` from the generated project folder. Starting an executable with another working directory prevents project-relative asset paths from resolving.

### Reconfigure from scratch

Close Visual Studio and the running editor, remove the affected `build/Debug` or `build/Release` directory, then repeat the configure and build commands above.
