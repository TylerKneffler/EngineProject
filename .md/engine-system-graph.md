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
