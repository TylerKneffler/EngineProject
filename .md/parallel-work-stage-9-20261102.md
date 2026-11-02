# Parallel scene stage 9: snapshot and mutation boundary

`Scene` now publishes a `shared_ptr<const SceneSnapshot>` at explicit object-change commits and after runtime updates. Snapshots own copies of object IDs, parent IDs, names, enabled state, local transforms, component type/revision, and serialized component configuration. Readers can retain an old snapshot after its live objects are removed. Unchanged component configuration is reused by revision to avoid repeated serialization.

Workers can queue creation, removal, reparenting, object field changes, component creation, and typed component writes by stable object ID. The scene owner applies queued commands in order at `CommitPendingObjectChanges()`, then flushes additions/removals and publishes the next snapshot. Runtime `Update()` already invokes this boundary. Pending creations receive `Start()` after their components have been attached, and creation followed by removal in one commit leaves no live object.

The broad todo remains open because legacy code still directly writes public `Object` and component fields on the scene owner, including editor paths. Those paths need migration or a declared serial compatibility policy before every mutation is guaranteed to pass through the commit boundary. Direct component writes must call `MarkConfigurationDirty()` for cached snapshot configuration to refresh, as required by the existing component revision contract.

Verification: Debug Editor and GrassPerformanceTests builds passed. The GrassPerformance regression suite passes, including worker-queued mutations, retained immutable snapshots, component configuration values, lifecycle startup, and create/remove in one commit.

The 240-frame 441-cell DX11 route with snapshots averaged 93.88 ms per frame, 12.85 ms CPU render work, and 67.35 ms grass GPU work. The earlier matched route averaged 88.40 ms, 13.64 ms, and 63.44 ms respectively. The measured frame difference coincides with a higher GPU stage cost, while CPU render work was lower; this capture does not isolate snapshot publication cost.

A direct Debug benchmark with 441 objects and Material components measured 285.8 ms for first publication, then 0.35 ms average for 240 unchanged commits after snapshot reuse. The initial serialization cost remains a scene startup concern; unchanged frame publication is much cheaper than the 2.66 ms measured before reuse.
