# Stage 11: standalone simulation owner

The standalone `GameApplication` now transfers scene ownership to a dedicated simulation thread after startup. That thread runs `Scene::Update`, pending scene loads, and `PrepareRenderFrame`. The window thread captures a keyboard, focus, and cursor snapshot after message pumping and submits at most one tick. It waits for preparation to finish before graphics submission reads the live scene. Cursor position and visibility requests made by the first-person controller return with the completed tick and are applied by the window thread. Shutdown joins the worker and transfers scene ownership back before scene destruction.

Built-in first-person, Pong, and Snake scripts use the captured key state when a simulation snapshot is active. Editor and other serial contexts retain Win32 input reads. The bounded handoff serializes live scene access; CPU simulation and graphics submission do not run concurrently yet. Buffered immutable render packets and render-thread ownership are separate follow-up items in `todo.md`.

Verification: Debug `Game`, `Editor`, and `GrassPerformanceTests` build; `GrassPerformance` passes scene-owner transfer and input snapshot checks; a hidden native Game startup and WM_CLOSE shutdown exited with code 0.
