# Stage 12: standalone render owner

The standalone game now routes frame graphics submission, resize, and idle waits through one render thread. The handoff carries the published scene generation and viewport dimensions, accepts one packet at a time, and finishes each render before simulation may advance. The render worker also completes `WaitIdle` before shutdown.

Native UI button hit testing and click callbacks now run during the simulation update from the captured window input. `UIRenderer::Render` skips interaction callbacks on the render worker; editor rendering retains its serial owner behavior.

Native UI layout is also resolved on the simulation owner. After the update commit, the scene prepares computed rectangles and text ordering for the render handoff; a scene transition refreshes the layout before rendering its first frame. Clearing objects invalidates the prepared layout. The renderer still reads live UI components while drawing, protected by the completed-frame barrier.

This is an ownership foundation, not yet a pipelined renderer. `Scene::Render` still reads live object and component pointers prepared by simulation. The one-frame completion barrier protects those pointers, so CPU simulation and graphics submission remain serialized. The parent render-thread TODO stays open until draw packets own their data and GPU resource retirement follows frame completion fences.

Verification: Debug Game, Editor, and GrassPerformanceTests build; the scene regression covers native UI callbacks on the scene owner. A hidden native Game startup, resize, and WM_CLOSE shutdown passed with exit code 0.

Follow-up verification: Debug Game, Editor, and GrassPerformanceTests rebuild after the UI layout handoff; GrassPerformanceTests passes with a prepared-layout and invalidation check.
