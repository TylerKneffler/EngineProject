# Stage 10: serial scene owner edits

The editor and existing script callbacks retain direct object and component field access on the scene owner thread. Scene structural APIs, object lifecycle and component configuration revision updates reject calls from other threads. Worker jobs use stable-ID scene commands, which execute at `CommitPendingObjectChanges`.

Live hierarchy parent changes now use `Scene::SetParentLocal`; it validates scene ownership and cycles, maintains both sides of the link, and preserves local transform. Editor and serializer component-list mutations assert scene ownership and notify the scene after structural changes. Custom editor property controls mark modified components dirty so the next snapshot contains their new configuration.

`Start`, `Update`, and `PrepareRenderFrame` publish at commit boundaries. Published snapshots remain unchanged between those boundaries. Direct script edits to public component fields must call `MarkConfigurationDirty` so component configuration snapshots refresh. The direct field path is serial and is not safe for worker jobs.

Scene construction and `ClearObjects` now use `CommitPendingObjectChanges` as well. `PublishSceneSnapshot` rejects publication outside that function, and `ClearObjects` rejects calls during `Update` so it cannot invalidate the active traversal.

Verification: Debug Editor and GrassPerformanceTests build; GrassPerformance covers worker rejection, queued commands, serial hierarchy edits, direct component revision edits, and snapshot publication.
