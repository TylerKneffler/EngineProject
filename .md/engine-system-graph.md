# Engine System Graph (ASCII Flow)

## 1) Startup Graph

```text
[Process Start]
    |
    v
[Load Project Settings]
    |
    v
[Create Window]
    |
    v
[Create Renderer]
    |
    v
[Create Scene]
    |
    v
[Load Scene]
    |
    v
[Object Start Calls]
    |
    v
[Enter Window Message Loop]
```

## 2) Frame Loop Graph

```text
  +------------------------------ loop ------------------------------+
  |                                                                 |
  v                                                                 |
[Frame Begin] --> [Input Begin] --> [Drain Win32 Messages] --> [Input End]
                                     |
                                     v
                             <Resize Pending?>
                               |          |
                             Yes          No
                               |          |
                               v          v
                           [Apply Resize] [OnUpdate]
                               \          /
                                \        /
                                 v      v
                               [Delta Time]
                                   |
                                   v
                         [Gameplay and Editor Updates]
                                   |
                                   v
                             [Object Script Update]
                                   |
                                   v
                      [Physics Step (Game / Editor Play)]
                                   |
                                   v
                             [Renderer BeginFrame]
                                   |
                                   v
                                [Clear]
                                   |
                                   v
                               [Scene Render]
                                   |
                                   v
                             [UI Render Editor]
                                   |
                                   v
                        [Renderer EndFrame and Present]
                                   |
                                   +---------------> back to [Frame Begin]
```

## 3) Scene Render Pass Graph

```text
[Scene Render Start]
       |
       v
[Validate Context and Pipelines]
       |
       v
[Select Camera]
       |
       v
[Build View and Projection]
       |
       v
[Iterate Objects] <-----------------------------------------------+
       |                                                        |
       v                                                        |
  <Has Mesh and Ready?>                                           |
    |            |                                              |
     No           Yes                                             |
    |            |                                              |
    +------------+----> [Bind Material and Textures]            |
                    |
                    v
            [Write Object Constant Buffer]
                    |
                    v
              [Bind Object Pipeline]
                    |
                    v
                 [Draw Mesh]
                    |
                    v
               <Selected Object?>
                |           |
                 No          Yes
                |           |
                |           v
                |   [Bind Outline Pipeline]
                |           |
                |           v
                |   [Draw Outline Overlay]
                |           |
                +-----------+
                        |
                        v
                    back to [Iterate Objects]

[Iterate Objects Complete]
       |
       v
[Draw Grid Helper]
       |
       v
[Scene Render End]
```

## 4) Editor Panel Render Graph

```text
[RenderIfNeeded]
    |
    v
[Clear]
    |
    v
[Iterate Panels] <----------------------------------+
    |                                             |
    v                                             |
<Panel Open and NeedsRender?>                       |
    |                    |                        |
     No                   Yes                       |
    |                    |                        |
    +--------------------+--> [Panel Render3D] --+

[Panels Complete]
    |
    v
[Draw Editor UI]
    |
    v
[Present]
```

## 5) Physics System Graph

Physics components live under `Engine/Core/Compoonents/Physics`; the scene-level
runtime remains under `Engine/Core/Physics`.

```text
[Game Frame or Editor Play Frame]
                 |
                 v
          [Scene::Update(dt)]
                 |
                 v
     [Scene-owned Physics::Step(dt)]
                 |
                 v
 [Bullet btSoftRigidDynamicsWorld]
       |                         |
       v                         v
[Scan RigidBody Components]  [Scan Cloth Components]
       |                         |
       v                         v
[Resolve Collider Components] [Resolve Simulation Mesh]
       |                         |-- assigned Mesh component (same/other object)
       |                         |-- meshPath asset
       |                         +-- same-object Mesh default
       |
       +--> [PrimitiveObjectCollider]
       |       Box / Sphere / Capsule / Cylinder
       |
       +--> [MeshObjectCollider]
               |-- assigned Mesh component (same/other object)
               |-- meshPath asset
               +-- same-object Mesh default
                       |
                       +--> Convex hull (dynamic compatible)
                       +--> Triangle mesh (static/kinematic)

[Build/Refresh Bullet Rigid Bodies]       [Build/Refresh Bullet Soft Bodies]
                 |                                      |
                 +------------------+-------------------+
                                    |
                                    v
                  [Apply Cloth Pins, Wind, Gravity Scale]
                                    |
                                    v
                        [Bullet Simulation Step]
                                    |
                  +-----------------+------------------+
                  |                                    |
                  v                                    v
       [Sync Rigid Transform]              [Sync Cloth Nodes to Render Mesh]
                  |                                    |
                  v                                    v
       [Collision/Trigger State]       [Upload Vertices + Recalculate Normals]
                  |                                    |
                  +-----------------+------------------+
                                    |
                                    v
                            [Scene Render Uses Results]
```

`Physics::Reset()` removes rigid and soft bodies when its owning scene is cleared
or play mode ends. Cloth restores the render mesh's original vertices when
disabled, removed, or reset.

## 6) Audio System Graph

```text
[Scene::Update(dt)]
          |
          +---------------------> [AudioSource Component Update]
          |                                  |
          |                                  v
          |                         [Source Settings/Transform]
          |
          v
[Scene-owned Audio::Update(dt)]
          |
          v
[Select One Listener Camera]
                  |
                  v
          <Audio File Assigned?>
             |             |
             No            Yes
             |             |
             v             v
           [Idle]    [Resolve Asset Path]
                           |
                           v
                [Decode / Create Data Source]
                  |                    |
                  v                    v
        [WAV / MP3 via miniaudio] [OGG via stb_vorbis]
                  |                    |
                  +---------+----------+
                            |
                            v
                  [AudioMixer::Get()]
                            |
                            v
                  [Get/Create Named Bus]
                            |
                            v
                       [ma_sound]
                            |
          +-----------------+------------------+
          |                 |                  |
          v                 v                  v
 [Playback Settings] [Spatial Settings] [Directional Cone]
 play/pause/stop       distance model      inner/outer angles
 loop, pitch, volume   rolloff, doppler    outer gain
          |                 |                  |
          +-----------------+------------------+
                            |
                            v
          [Assigned Camera Reference]
                       |
                       v
        [Active Game Camera / Editor Fallback]
                            |
                            v
              [Update Listener + Source Transform]
                            |
                            v
               [Named Bus Volume / Master Volume]
                            |
                            v
                 [miniaudio Output Device]
```

Each Scene owns an `Audio` coordinator; the mixer is process-wide and initialized
lazily because it owns the output device. Named buses are created lazily and feed
the master output. AudioSource cleanup stops and releases its sound/data source
when disabled or destroyed.

## 7) Editable Component Reference Graph

```text
[Inspector Component Header Drag]
                |
                v
     [Compatible Reference Field]
                |
                v
 [Capture Object Path + Name + Component Type/Index]
                |
                v
       [Serialize With Component]
                |
                v
 [Resolve Same-Scene Component at Runtime]
                |
       +--------+---------+
       |                  |
       v                  v
[Assigned Override]  [Empty Reference]
                          |
                          v
                [Documented Default Lookup]
```

Current reference users include collider meshes, Cloth simulation/render meshes,
Sprite animation managers, AudioSource listener cameras, skinned-mesh inputs,
Skeleton hierarchy roots, and animation hierarchy/source inputs. Clearing a
reference restores its conventional default rather than storing a raw pointer.

## 8) Native Screen UI System Graph

```text
[Scene Hierarchy]
       |
       v
[Canvas Component] ---> reference resolution / screen scale / sorting order
       |
       v
[UILayout]
       |
       +--> [UIObject anchors + pivot + size constraints]
       |
       +--> [Row / Column flex layout]
       |
       +--> [Nested clipping + z-order]
       |
       v
[Resolved UIText Rects]
       |
       v
[SDF Font Atlas Cache] <--- assigned TTF/OTF or system-font fallback
       |
       v
[UIRenderer Vertex Batch]
       |
       v
[Alpha-Blended UI Pipeline, Depth Disabled]
       |
       v
[Final Scene Target] ---> editor chrome is composed afterward
```

Canvas, UIObject, and UIText serialize through the ordinary component registry.
UIRenderer is backend-neutral and runs after the 3D/grid scene pass on DirectX 11,
DirectX 12, and Vulkan. Text geometry is batched per cached font atlas.
