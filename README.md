# EngineProject

A C++17 game engine and editor for Windows x64. It includes scene editing and serialization, C++ gameplay scripts, physics, audio, animation, and DirectX 11, DirectX 12, and Vulkan renderers. The editor can run the bundled Engine Sandbox or create a separate game project.

## Start with a packaged build

1. Download the [current Windows x64 ZIP (v1.0.0)](ReleaseBuilds/EngineProject-v1.0.0-windows-x64.zip?raw=1). Browse [all packaged builds](ReleaseBuilds/) for other versions.
2. Extract the ZIP and keep the complete `v1.0.0` folder together.
3. Run `Editor.exe` inside that folder.

The packaged editor needs no compiler or CMake. DirectX 11 is the default. You can choose DirectX 12 or Vulkan in **File > Project Preferences > Rendering**; Vulkan needs a supported GPU and driver. The folder also contains `Game.exe` for the standalone sandbox. To build C++ project scripts, install the development tools below.

## Build from source

Requirements: Windows 10 or 11, Visual Studio 2022 with **Desktop development with C++** (including the Windows SDK), CMake 3.20+, Git, and network access for the first CMake configure. CMake downloads the third-party sources listed below.

```powershell
git clone https://github.com/TylerKneffler/EngineProject.git
cd EngineProject
cmake --preset debug
cmake --build --preset debug --target Editor --parallel
.\build\Debug\Debug\Editor.exe
```

Opening the editor from the repository starts **Engine Sandbox** with `Engine/Core/Assets`. Use F5 to play the current scene. To open the Project Hub explicitly:

```powershell
.\build\Debug\Debug\Editor.exe --project-hub
```

The Project Hub creates or opens a project and provides a shortcut in each generated project folder. That folder owns its `Assets/` content and `.proj` file. From a generated project folder, you can also build and run manually:

```powershell
cmake --preset debug
cmake --build --preset debug --parallel
.\build\Debug\Engine\Debug\Editor.exe
.\build\Debug\Engine\Debug\Game.exe
```

Set `-DENGINE_ENABLE_VULKAN=OFF` on the configure command if you only need DirectX. For a distributable game, use **File > Project Preferences > Export > Build Portable Export** in the project editor. Build output and startup diagnostics are written to `project-build.log` and `editor-startup.log` in the project folder.

## Engine graphs and game loop

### Startup appearance and Windows icons

Editor and Game show a native splash while their renderer, interface, and startup
scene initialize. The splash has a background, a dark text overlay, and a
progress bar that advances as startup stages finish. Without artwork it uses a
blue gradient. To customize the backgrounds, add these optional PNG files:

```text
Assets/Startup/Editor.png
Assets/Startup/Game.png
```

For the bundled Engine Sandbox, place them under `Engine/Core/Assets/Startup/`.
The images are stretched to the 640 × 360 splash canvas. The colors, overlay,
text, and progress bar are drawn in `Engine/Core/Startup/StartupSplash.cpp`.

Set executable icons at configure time with `.ico` files (relative paths are
resolved from the top-level project directory):

```powershell
cmake --preset debug -DENGINE_EDITOR_ICON=Assets/Startup/Editor.ico -DENGINE_GAME_ICON=Assets/Startup/Game.ico
cmake --build --preset debug --target Editor Game --parallel
```

The icon is embedded into each EXE and used by its native window. If an icon
path is omitted, Windows uses the default application icon. Reconfigure and
rebuild after changing an `.ico` file.

### Startup

```text
[Editor.exe / Game.exe]
          |
          v
[Load Project Settings]
          |
          v
[Create Win32 Window + Selected Renderer]
          |
          v
[Create / Load Scene]
          |
          v
[Game: Start Objects / Editor: Wait for Play]
          |
          v
[Enter Window::Run]
```

### Frame loop

```text
[Input Begin] -> [Drain Win32 Messages] -> [Input End] -> [Apply Pending Resize]
                                                           |
                                                           v
                                               [OnUpdate: Calculate Delta Time]
                                                           |
                              +----------------------------+------------------+
                              |                                               |
                              v                                               v
                    [Game / Editor Play]                               [Editor Edit Mode]
                              |                                               |
                              v                                               v
                      [Scene::Update]                              [Editor Background Work]
                              |                                               |
                              +-----------------------+-----------------------+
                                                      |
                                                      v
                                        [Prepare and Render Frame]
                                                      |
                                                      v
                                        [Present / Wait if Editor Idle]
                                                      |
                                                      +----> back to [Input Begin]
```

The standalone game renders every frame. The editor updates the scene only in Play mode and renders when its frame is marked dirty.

### Scene update

```text
[Scene::Update(deltaTime)]
           |
           v
[Advance Scene Clock]
           |
           v
[Update Objects, Components, and Scripts]
           |
           v
[Update Scene Audio]
           |
           v
[Step Scene Physics]
           |
           v
[Process Portal Traversal After Physics]
           |
           v
[Flush Pending Object Changes]
```

### Render path

```text
[Prepare Scene Render Data]
           |
           v
     <Game or Editor?>
       |           |
      Game       Editor
       |           |
       v           v
[Select Camera] [Render Visible Scene / Game Panels]
       |           |
       v           v
[Render Scene] [Draw Editor UI with ImGui]
       |           |
       +-----+-----+
             |
             v
     [Present Frame]
```

Core scene and component code lives in `Engine/Core/`; the editor, standalone game, and built-in demo scripts live in `Engine/Editor/`, `Engine/Game/`, and `Engine/Core/Assets/Scripts/`. See the [detailed system graphs](.md/engine-system-graph.md) and [feature guide](.md/feature-guide.md).

## Third-party libraries and includes

CMake retrieves these dependencies with `FetchContent`; their versions and options are defined in [CMakeLists.txt](CMakeLists.txt). No separate manual install is needed for the source build.

| Dependency | Used for |
| --- | --- |
| [GLM](https://github.com/g-truc/glm) | Math types and transforms |
| [Dear ImGui](https://github.com/ocornut/imgui) | Editor UI |
| [pugixml](https://github.com/zeux/pugixml) | XML parsing |
| [fastgltf](https://github.com/spnda/fastgltf) and [Assimp](https://github.com/assimp/assimp) | glTF/GLB and FBX import |
| [stb_image](https://github.com/nothings/stb) (from Assimp), [TinyEXR](https://github.com/syoyo/tinyexr), and [KTX-Software](https://github.com/KhronosGroup/KTX-Software) | Texture decoding and KTX2/BasisU |
| [miniaudio](https://github.com/mackron/miniaudio) and its `stb_vorbis` extra | Audio playback, mixing, and decoding |
| [Bullet](https://github.com/bulletphysics/bullet3) | Rigid-body and cloth physics |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), [volk](https://github.com/zeux/volk), and [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Optional Vulkan renderer and SPIR-V shaders |

The Windows SDK supplies Win32 and DirectX headers and libraries. The DirectX 11 and 12 renderers use those system libraries.
