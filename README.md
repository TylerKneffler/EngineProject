# EngineProject

EngineProject is a C++17 Windows game engine with a package-neutral editor UI, scene serialization, a standalone game runtime, and support for DirectX 11, DirectX 12, and Vulkan renderers.

## Quick start — release build

No manual compilation is required to run the packaged Windows x64 editor:

1. [Download EngineProject v1.0.0 for Windows x64](ReleaseBuilds/EngineProject-v1.0.0-windows-x64.zip?raw=1).
2. Extract the complete `v1.0.0` folder from the ZIP.
3. Run `Editor.exe` from inside that folder.

Keep the extracted folder together because `Engine/`, `ProjectTemplate/`,
`VulkanShaders/`, and `imgui.ini` are runtime and tooling dependencies. DirectX
11 is the default renderer; DirectX 12 and Vulkan can be selected in Project
Preferences. Vulkan requires a supported GPU and current driver.

The prebuilt editor does not require Visual Studio or CMake just to run. Creating
projects is supported, but compiling project C++ scripts or rebuilding the engine
requires the development tools listed below. All downloadable builds are under
[`ReleaseBuilds/`](ReleaseBuilds/).

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

Maintainers can produce a new versioned folder and download ZIP with the VS Code
task **Engine: Package Versioned Release**. The task prompts for a version and
writes both artifacts under `ReleaseBuilds/`.

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
- Textures generate a complete mip chain on load. PNG, JPEG, BMP, DDS, TGA,
  Radiance HDR, OpenEXR, and KTX2 are supported; HDR/EXR remain floating-point
  through upload, and Basis Universal KTX2 images are transcoded by libktx.
- A scene skybox can drive HDRI lighting and reflections. With the Scene root
  selected, enable **Use Skybox for Lighting** and tune intensity, exposure,
  and rotation. Materials expose environment diffuse and reflection strengths.
  `Engine/Core/Assets/Scenes/hdri_showcase.scene` demonstrates the feature.
- A material can override only its reflections without changing the skybox.
  Enable **Custom Reflection Environment**, assign a Radiance HDR or OpenEXR
  file to **Reflection HDRI**, then tune its strength, exposure, and rotation.
- WAV, OGG/Vorbis, and MP3 files can be imported and assigned to an Audio Source.

### Audio sources

Add an **AudioSource** from an object's **Add Component** menu and assign its
`audioPath`. Enable `playOnStart` or use the component's **Play** button; scripts
can trigger the same voice with `audioSource->Play()`, then `Pause()` or `Stop()`.
`loop`, `volume`, and `pitch` apply per source. Enable `spatial` for camera-relative
3D audio and tune `minDistance`, `maxDistance`, `rolloff`, `dopplerFactor`, and the
attenuation model (`Inverse`, `Linear`, `Exponential`, or `None`). Enable
`directional` to use the object's local +Z direction and configure its inner and
outer cone angles/gain. Sources route through a named bus such as `SFX`, whose
level can be controlled with `AudioMixer::Get().SetBusVolume("SFX", 0.8f)`; the
master level uses `SetMasterVolume()`.

Each Scene owns its `Audio` coordinator, which selects and updates one listener
after component updates. `AudioMixer` remains process-wide because it owns the
single output device and named buses.

### Component references

Inspector component-reference fields behave like Unity object fields. Their empty
state uses the documented conventional component (usually one on the same object),
while dropping a compatible component header assigns an override from the same or
another scene object. **Clear** returns to the default. References serialize with
the scene and retain both hierarchy and object-name information so ordinary object
renames or hierarchy moves can still resolve when one identity remains valid.

This is used by Sprite animation managers, audio listener cameras, skinned-mesh
mesh/morph/skeleton inputs, animation hierarchy/source inputs, mesh colliders, and
Cloth simulation/render meshes.

### Physics

Rigid-body motion and collision use separate components. Add a **RigidBody** for
`Dynamic`, `Kinematic`, or `Static` motion, then add one or more colliders to the
same object. **PrimitiveObjectCollider** supports `Box`/`Cube`, `Sphere`/`Circle`,
`Capsule`, and `Cylinder`, with configurable center and dimensions.
**MeshObjectCollider** accepts an independent Mesh component reference from any
scene object, then `meshPath`, then the same-object Mesh default. Drag a Mesh
component header onto Mesh Reference to override the default. Convex colliders support
dynamic bodies; concave triangle meshes are used by static and kinematic bodies.

Rigid bodies expose mass, gravity scale, linear/angular damping, friction,
restitution, triggers, continuous collision detection, initial velocities,
collision layer/mask, and per-axis position/rotation locks. Runtime scripts can
call `AddForce()`, `AddTorque()`, `AddImpulse()`, `SetLinearVelocity()`, and query
`IsColliding()` or `IsGrounded()`. Physics advances during Editor Play and in the
standalone Game runtime; edit-mode transforms are not simulated.
The owning Scene advances its `Physics` instance through `Scene::Update()`.

Add **Cloth** to an object with a triangle Mesh to deform that render mesh in real
time. Cloth has independent Simulation Mesh and Render Mesh component references,
so a different object or lower-detail triangle cage can drive the visible mesh;
empty references preserve the same-object default. `meshPath` remains available
as an asset-only simulation source. Cloth supports mass,
stretch/bending stiffness, damping, drag,
friction, gravity scaling, solver iterations, rigid-body collision, optional
self-collision, and directional wind. `pinMode` can hold the mesh's `Top`,
`Bottom`, `Left`, or `Right` boundary in object space; `None` leaves it fully
free. `pinThreshold` controls how wide that pinned boundary is. The original
render mesh is restored when play stops or the component is removed, and scripts
can call `ResetSimulation()` after changing its setup.

### Native screen UI

Screen UI uses retained scene components and is rendered by the engine after the
3D scene. Add **Canvas** to a root object, then add child objects with **UIObject**
and **UIText**. An optional UIObject on the Canvas root can arrange its immediate
children as a `Row` or `Column`; nested UIObjects build the rest of the hierarchy.

UIObject provides top-left-origin anchors, pivot, anchored position, size delta,
minimum/maximum size, margin, padding, flex growth, spacing, clipping, visibility,
and z-order. Row/Column containers support start/center/end/space-between
justification and start/center/end/stretch alignment. Canvas supports constant
logical sizing or scale-with-screen behavior against a configurable reference
resolution.

UIText provides size, RGBA color, horizontal/vertical alignment, word wrapping,
line spacing, and visible/clip/ellipsis overflow. Leave Font Path empty to use the
Windows UI-font fallback, or assign a `.ttf`/`.otf` file through the inspector.
The initial SDF atlas covers printable ASCII; unsupported characters render as
`?`. The same components render in the editor scene view and standalone game.

### Imported animated characters

Importing a Blender glTF/GLB or FBX creates skeleton, animation, animation-manager,
morph-target, and skinned-mesh components automatically. The renderer performs
skinning on the GPU on DirectX 11, DirectX 12, and Vulkan, with up to eight
influences per vertex and 256 joints per rendered primitive.

The editor routes all supported model formats through `ModelImporter`. FBX is
decoded into the format-neutral `ImportedModel` document (nodes, primitives,
materials, skins, morphs, and clips), and every import produces the same
engine-native `.prefab`, `.mesh`, and `.material` references. Use
`--import-model` for headless imports; the older `--import-gltf` spelling
remains available for compatibility.

Call `AnimationManager::Play("Run", 0.2f)` to crossfade the base clip. Add
`AnimationManager::Layer` entries for override or additive animation. A layer's
`nodeMask` restricts it to the listed imported `ModelNode::nodeIndex` values; an
empty mask affects the full character. Layers can also be edited on the
Animation Manager component in the Properties panel.

## Renderers

Choose the editor and game renderers under **File > Project Preferences > Rendering**:

- DirectX 11
- DirectX 12
- Vulkan

Unavailable renderers are disabled and show the reason. Restart the Editor after changing its renderer.

## Editor UI backends

The editor UI lives under `Engine/Editor/UI/ImGui`. `IEditorUi` is the shared
widget/layout facade used by every panel in `Engine/Editor/Core/View`, while
`IEditorUiBackend` owns ImGui lifecycle, input, and frame submission. Engine
renderers expose package-neutral frame and texture callbacks; `Engine/Core`
does not include or link the UI library.

```powershell
cmake --preset debug
cmake --build --preset debug
```

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
