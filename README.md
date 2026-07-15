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

## Build

Run all commands from the repository root.

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

The first configure may take a few minutes while dependencies are downloaded. Later configurations use the cached dependency sources.

## Start the engine editor

The editor must be started with the repository root as its working directory because project, shader, scene, and asset paths are resolved from there.

Debug:

```powershell
.\build\Debug\Debug\Editor.exe
```

Release:

```powershell
.\build\Release\Release\Editor.exe
```

You can also open `build/Debug/EngineProject.sln` in Visual Studio, select `Editor` as the startup project, and ensure its working directory is the repository root.

## Start the standalone game

Build the `Game` target first, then run:

```powershell
.\build\Debug\Debug\Game.exe
```

For a Release build:

```powershell
.\build\Release\Release\Game.exe
```

The editor's **File** menu also provides build-and-run commands for running in the editor or launching the standalone game.

## Select a renderer

Open **File > Project Preferences > Rendering** and choose separate renderers for the editor and game. Available options are:

- DirectX 12
- DirectX 11
- Vulkan

An unavailable renderer is disabled and displays the reason. Save the project settings after changing a selection. Changing the editor renderer requires restarting the editor.

The same options can be configured directly in `Example_Proj.proj`:

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

The repository includes tasks under `.vscode/tasks.json` for configuring and building Debug or Release configurations. Run **Tasks: Run Build Task** from the command palette to configure and build Debug. To build and launch the editor with the correct working directory, run **Tasks: Run Task > Engine: Start Editor (Debug)**.

## Common problems

### CMake cannot download dependencies

Check the network connection and rerun the configure command. Once downloaded, dependencies are cached under the selected build directory.

### A renderer is unavailable

Update the GPU driver and install current Windows updates. DirectX 12 requires a Direct3D 12-capable GPU; Vulkan requires a Vulkan-capable GPU driver that installs the Windows Vulkan loader. The full Vulkan SDK is not required. The editor's build console and debugger output include the detected status and failure reason for each renderer.

### Shaders, assets, or the project file cannot be found

Start `Editor.exe` or `Game.exe` from the repository root. Starting an executable with another working directory prevents relative asset and shader paths from resolving.

### Reconfigure from scratch

Close Visual Studio and the running editor, remove the affected `build/Debug` or `build/Release` directory, then repeat the configure and build commands above.
