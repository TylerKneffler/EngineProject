# Frame Rendering Flow

## Editor frame flow

The editor renders on demand: changes mark the renderer dirty, and `RenderIfNeeded()` skips the frame when nothing needs drawing. The exact submission steps vary by backend (DX11, DX12, or Vulkan), but the editor path follows this general sequence:

1. **Update and prepare:** The editor updates the scene, draws the UI, and prepares each visible Scene or Game view's scene once. Preparation publishes ready assets and shader pipelines, processes pending graphics work, selects the frame's scene buffers, and builds render data.
2. **Start GPU work:** The renderer begins a frame and exposes its current command buffer and main render target. If the play pipeline is active, it records particle simulation and view draw commands for replay; otherwise, rendering records directly into the current command buffer.
3. **Render each view:** Visible Scene and Game panels render shadows and scene geometry into their own offscreen targets. Each view then composes its result into the main window target, where the UI is rendered over it.
4. **Submit and present:** The renderer submits the recorded work and presents the window. DX12 uses a fence and swap-chain back-buffer index; DX11 uses an event query for completion; Vulkan acquires and submits a swap-chain image.

The main editor loop and view dispatch are in `Engine/Editor/EditorApplication.cpp`. Scene preparation is in `Engine/Core/Scene/Rendering/SceneRenderer/SceneFramePreparation.cpp`.

## Buffers and render targets

- **Window buffers:** DX12 has two swap-chain back buffers, with a command allocator per slot. The current back buffer is transitioned to render-target state, drawn into, transitioned back to present state, then presented.
- **Per-view targets:** Each Scene/Game view renders to its own color texture and depth buffer, then makes the color texture available to the UI for display. On DX12, the view runs its pre-draw work, scene draw callback, and composition pass before returning to the window target.
- **Scene data buffers:** The scene keeps GPU buffers for object records, lights, bones, shadows, portals, and clustered lighting. Some native frame resources—such as grid, skybox, light, bone, cluster, and portal buffers—are selected by frame slot to avoid overwriting data still in use.
- **UI and view descriptors:** DX12's shader-visible descriptor heap reserves slot 0 for UI and view slots after it; a tail region is used for editor texture previews. Views use descriptors so the UI can sample their rendered textures.

In short: **scene preparation → shadow and scene rendering into per-view targets → compose views into the window back buffer → UI overlay → submit and present**.

## Editor Game View versus standalone game

The editor Game View and standalone game share scene rendering code, but they do not use the same frame loop.

- **Editor Game View:** Runs inside the editor's dirty-gated frame. It renders to its own offscreen view target, then the editor composites that texture into the window and draws the editor UI over it.
- **Standalone game:** Has its own continuous, FPS-paced loop. A simulation loop prepares scene snapshots and shadow/scene command packets; a render worker consumes them asynchronously, replays shadow commands, clears the scene target, replays scene commands, and ends the frame. There is no editor UI or per-panel Game View composition in this path.

The standalone path still uses the same general scene preparation, GPU scene buffers, shadow/scene rendering, and post-processing concepts. Its renderer composes the scene into the game window's swap-chain image and presents it. DX12 uses a double-buffered swap chain with fences; the other backends use their own submission mechanisms.

Relevant source: `Engine/Game/Application/GameApplication.cpp` for the standalone render worker and loop, and `Engine/Core/Renderers/DX12/DX12GameRenderer.cpp` for DX12 frame setup and submission.

> The active backend for a particular run depends on startup settings; this document describes the available code paths, not a verified runtime selection.
