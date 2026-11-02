# Stage 13: asynchronous scene transition ownership

`SceneManager::LoadSceneAsync` previously called `Scene::Load` from a detached thread. That thread could clear live objects, create components and graphics resources, and invoke callbacks while the simulation or renderer still used the scene.

The asynchronous worker now reads only file bytes. One reader thread accepts the latest pending request. `ProcessPendingSceneLoad` applies a completed load and runs its callbacks on the scene owner at the existing transition boundary. Synchronous requests, cancellation, active-scene changes, and shutdown invalidate older results. The native game and editor clear their active scene before destruction.

Scene deserialization and graphics resource creation still run together on the scene owner. Splitting those stages and making graphics work render-owned remain open tasks.

Verification: Debug Game, Editor, and GrassPerformanceTests build. The regression executable passes an asynchronous scene load, verifies that callbacks run on the scene owner after replacement, checks that a missing file leaves the scene intact, and confirms an active-scene change cancels a stale result.
