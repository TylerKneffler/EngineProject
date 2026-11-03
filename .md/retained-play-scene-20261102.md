# Retained edit scene during Play

Play now constructs a separate runtime Scene from the authored snapshot and
retains the original Scene and objects in memory. Stop switches back to that
original graph without deserializing prefabs or rebuilding its first component
configuration snapshot. This applies to all project scenes.

Scene/Game viewports, hierarchy, properties, asset browser, Performance view,
new-panel factory, application loop and SceneManager follow the active scene.
Independent document scenes keep their bindings. Scene switches clear transient
gestures, inspector reference pickers and pending imports that contain old graph
pointers. Main mesh/skeleton edit sessions remain with the retained edit scene.
Dirty state, selection and undo/redo history survive Play.

Scene switching happens outside frame submission. Pending scene loads are
cancelled, component jobs are fenced, and GPU completion precedes runtime
destruction. Shutdown and hot reload restore the edit scene before script unload.
Failed runtime construction leaves the original scene attached. The first
simulation delta excludes runtime-copy loading time.

## Validation

- Debug Editor and EditorPlaySessionTests build successfully.
- Full editor default-scene smoke completed with exit code 0: 60 recorded
  frames including 30 Play frames, followed by Stop/shutdown. This used a
  hidden test window and is a lifecycle check, not a frame-rate benchmark.
- EditorPlaySession integration test passes three complete cycles, including
  runtime scene replacement, new panels during Play, original graph/object
  identity, selection/dirty-state restoration, and usable pre-play undo/redo.
- Small-scene Stop measured 15–29 ms across two runs; instrumented scene rebind
  and editor-state restore measured 0.21–0.30 ms, with cleanup taking the rest.
- Full grass scene integration audit passed: Stop took 1,344.76 ms, including
  runtime destruction, and the retained authored snapshot remained unchanged.
  Previous synchronous restore audit spent 42,120.6 ms deserializing and
  12,422.9 ms publishing; the user's prior recording showed a 55.2-second Stop.
  These are idle-GPU integration measurements; a live editor can additionally
  wait for submitted GPU work.

## Remaining cost

The two scene graphs coexist during Play, increasing memory use. Runtime-copy
construction is synchronous and still expensive: the grass audit's complete
copy/Start/Update/Stop cycle took 70.16 seconds, excluding initial scene loading.
The editor presents a preparation overlay before the copy, but cannot service
UI while constructing it. Incremental runtime preparation, cached prefab
documents/configuration snapshots and bounded runtime destruction remain open
in todo.md. This change targets Stop latency, not steady-state frame rate.

Timing logs distinguish runtime-copy preparation, edit-scene restoration and
runtime cleanup in the editor Console. Per-stage lifecycle trace fields remain
separate follow-up work.
