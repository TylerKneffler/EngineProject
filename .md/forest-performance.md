# Forest performance investigation

Target: **100+ FPS at 1920x1080**, a **10 ms frame budget**, on the current machine with an optimized build. The latest shadow-enabled moving run averages 100.26 FPS, but P95 is 12.42 ms; sustained 100+ FPS remains open.

## Completed cold rebuild follow-up: resolved materials and DX12 commands (2026-11-07)

Generated prefab batches now copy the state of their already-resolved prototype
material through `Material::CopyResolvedStateFrom`. Registered values are written
through the destination component's own field bindings; custom shader/colors,
asset identity and all ten texture owners are copied explicitly. This avoids
reopening the material file and resolving/acquiring each texture for every spatial
batch. The destination keeps its own Owner, lifecycle, revision and preparation
state, normalizes values, and runs the existing deferred graphics hook. Textures
retain shared ownership after the temporary prototype scene is destroyed.
Ordinary file deserialization still loads current files and resolves paths.
Prototypes remain temporary; no duplicate source geometry is retained.

DX12 static uploads now lease one cached allocator/list/fence/event bundle per
buffer factory. A bundle returns to the cache only after its monotonically
increasing fence value completes; the allocator/list can then reset safely.
Concurrent uploads use independently owned local bundles when the idle slot is
empty, and retain at most one completed bundle afterward. Staging remains capped
at 16 MiB. The queue lock covers submission/signal only, every returned destination
is ready, and all staging/command resources remain owned through the wait. Errors
do not return a partially used bundle to the cache. Device-removal fence values
are rejected. No deferred upload contract or removal of waits was needed.

The regression compares resolved material copies with ordinary serialized
reconstruction, checks all texture owners and non-reflected values, edits the
material file mid-copy, verifies subsequent normal loads see the edit, edits the
destination independently, destroys the prototype, and replaces/self-copies the
material. DX12 static-buffer coverage adds 32 uploads from four concurrent
callers, retaining and fully reading back every destination. Optional GPU
validation rejects error/corruption messages, alongside existing sequential
readback/resource-state and writable-mesh fallback checks.

Automated frame traces now tag `play_stop` on the request, restoration and the two
following update samples, covering cold rendering and delayed interval reporting.
Those samples have `recurring_frame=false`. The FrameActivity regression checks
combined Start/Stop/Load tags and consumption. Baseline captures predate the new
Stop tag: the comparison identifies their Stop boundary from `playing`, excludes
all stopped frames explicitly, and does not rewrite historical recordings.

Three baseline processes were captured before rebuilding, followed by three
candidate processes. Each uses Release/DX12 at 1920x1080, scale 1, dynamic scaling
and temporal upscaling disabled, four shadow cascades and a 2048 atlas; 30 measured
Play frames, 10 warmup and two stopped frames. All **54 non-executable inputs**,
resolved references and the explicit fixed-layout seed match across all six.
Every manifest is complete/unchanged internally, each version's executable hash
is consistent, effective settings and relevant call counts validate, and all 30
measured Play samples are recurring with no activity tags. No build or test ran
alongside these captures. The authored grass remains 324,147 placements with no
bake and unchanged file identity. These manifests still exclude implicit
mesh/material/texture dependencies, which this work did not edit.

Times below are **median [minimum-maximum] milliseconds**, three processes per
version. The forest measurements refer to the last cold rebuild after Stop,
including 51 prototype definitions, 85 source parts and 340 generated batches.

| Scope | Before | After |
| --- | ---: | ---: |
| Cold forest rebuild | 1082.730 [1059.140-1124.200] | 870.159 [828.950-899.261] |
| Nested prototype definitions | 478.003 | 475.700 |
| Batch instance construction | 210.910 | 24.993 |
| Nested batch material work | 201.612 [191.228-209.950] | 16.729 [15.456-17.275] |
| DX12 command recording/creation or reset | 24.316 [23.939-26.267] | 4.230 [4.189-4.599] |
| DX12 submission | 5.679 | 5.056 |
| DX12 fence waits | 20.361 | 31.036 |
| Backend buffer creation parent | 122.491 | 100.370 |
| Stop restoration callback | 2198.740 [2080.050-2425.730] | 2230.990 [2037.130-2409.030] |
| Full first stopped frame | 3610.110 [3308.180-3859.600] | 3417.680 [3038.150-3631.950] |
| First stopped frame prefab preparation | 1138.550 | 921.048 |

Material work is **91.7% lower**, command recording **82.6% lower**, and cold
forest rebuilding **19.6% lower** by these medians. All candidate samples for
those three scopes are below all corresponding baseline samples. Each candidate
cold rebuild reuses a completed command bundle for all 85 uploads, with no fresh
bundle creation in that scope. Fence waits remain noisy CPU wall time, including
scheduling, rather than isolated GPU transfer duration. Submission/waits are
preserved; further upload batching was evaluated and is unnecessary for this
bounded reduction.

The first stopped-frame median is 5.3% lower, but its ranges overlap, and the
restoration callback has no demonstrated improvement. This is a small sequential
baseline-then-candidate sample under uncontrolled background load; do not claim a
reliable callback speedup, an exclusive end-to-end attribution or steady FPS gain.
Nested scopes overlap and are not additive. Grass baseline/publication snapshots
are preserved; recurring signature/path/stat polling remains separate. Definition
costs are now measured rather than hidden with retained prototype geometry.

Evidence: `.local/performance-logs/cold-followup-comparison.json`
(`comparison_valid=true`), `cold-followup-{before,after}-release-{1,2,3}.*`,
corresponding `.local/*-stages.log`, and `.local/analyze_cold_followup.py`.
The source variants used for the baseline are retained as
`.local/cold-followup-baseline-*`.

Validation: Release Editor, OakBackendCapture, ForestPerformanceAudit,
PerformanceRecordingTests, AsyncPrefabTests and PrefabPlacementBenchmark build;
Debug OakBackendCapture, ForestPerformanceAudit and PerformanceRecordingTests
build. All 13 selected Release checks pass across the main suite and clean
relinked-target rerun: JSON construction/writing, input identity, performance
recording, shared-prefab edits/restoration/relocation, async loading, document
cache, DX11/DX12/Vulkan oak rendering, DX12 GPU validation, and DX12/Vulkan static
geometry. The standalone concurrent static-upload test also passes with GPU
validation and no error/corruption messages. Debug recording and static geometry
checks pass. The full Debug shared-prefab regression passes directly in
123.717 seconds, including 2,048 placements, 85 sources and 340 batches.

Preserved diagnostic limitations: the first Release document-cache launch collided
with relinking its executable (resource busy); both relinked targets pass after
build completion. The Debug shared-prefab CTest run hit its existing 60-second
limit during the large forest portion; its direct 240-second-limit rerun passes
without a code change. Neither event was an assertion failure. The earlier grass
phase accumulation diagnostic did not recur, and no fix for its unknown cause is
claimed. Debug Editor was not rebuilt.

Logs: `.local/cold-followup-{release,final-release,async-release,debug}-build.log`,
`.local/cold-followup-final-release-tests.log`,
`.local/cold-followup-relinked-release-tests.log`,
`.local/cold-followup-dx12-gpu-validation.log`,
`.local/cold-followup-debug-tests.log`, and
`.local/cold-followup-debug-shared-direct.{log,json}`.

## Cold Stop callers, XML conversion and batch costs (2026-11-07)

The opt-in prefab profiler now identifies each placement serialization by its
nearest enclosing stage, layer name and placement count. `scene_snapshot_publish`
covers the immutable component publication at the commit boundary. This confirms
that Stop's three calls are **two 324,147-placement grass serializations and one
2,048-placement forest serialization**, rather than three identical grass copies.
One grass call captures the prefab source baseline; the other builds independently
owned component values in `Scene::PublishSceneSnapshot` after reconstruction.
Both snapshots remain intact. Patch construction has its own caller scope too.
No persistent placement/prototype cache or signature/path/stat polling change
was introduced, and prefab edit/relocation detection and runtime ownership remain
unchanged.

`LoadPrefabDocument` separates file allocation/read, XML parsing and XML-to-JSON
conversion. Worker reads emit a separate `[document conversion]` profile because
the existing parent profiler is thread-local; those worker scopes overlap the
owner's `read_parse` wait and must not be added to it. Synchronous reads join the
active profile. No per-node timers were added.

The current authored grass input remains 113,905,987 bytes, SHA-256
`47db145ebd50f3b692133f910335e40c0bd4cba3225a4206949b803800dcbb44`,
with 324,147 placements and no authored bake. The XML reader now counts element
children during its existing classification scan and reserves object member
storage for those children plus attributes. This reuses the existing object hint;
array allocation and component-qualified-node handling are unchanged. Duplicate
keys still use `Set` replacement, and retained values still own their storage.

A short matched-input pair uses Release/DX12 at 1920x1080, scale 1, dynamic
scaling/temporal upscaling disabled, four shadow cascades and a 2048 atlas;
30 measured Play frames, 10 warmup and two Stop records. All 54 non-executable
inputs, resolved references and effective settings match, with complete unchanged
identities within each capture. The explicit fixed layout seed is shared.

| Grass document scope | Original reader | Reserved XML objects |
| --- | ---: | ---: |
| Initial file allocation/read | 108.597 ms | 114.866 ms |
| Initial XML parse | 94.737 ms | 92.141 ms |
| Initial XML-to-JSON | 731.708 ms | 588.221 ms |
| Stop file allocation/read | 91.733 ms | 98.922 ms |
| Stop XML parse | 83.206 ms | 85.857 ms |
| Stop XML-to-JSON | 668.392 ms | 634.769 ms |
| Complete Stop callback | 2193.400 ms | 2455.370 ms |

This **single pair does not establish an overall Stop speedup**. Source capture,
encoding, signatures and backend work vary substantially; the candidate callback
is slower. The conversion samples support further controlled investigation, not
a precise causal gain. Aggregate Stop placement records are 477.358 / 551.081 ms,
prepared output 149.307 / 195.467 ms. In the candidate, grass source capture is
448.205 ms, grass publication serialization 296.240 ms and forest publication
serialization 2.180 ms. Publication's 420.092 ms parent includes other components
and bookkeeping. Scopes overlap. These are **cold transitions**, excluded from
steady-play analysis; no FPS improvement is claimed.

The new batch scopes separate placement transform/bounds construction, generated
object setup and material copying. In the candidate's last cold forest rebuild,
85 source parts generate 340 batches: transform/bounds work is 2.860 ms,
object setup including materials 237.266 ms, and nested material copying
228.523 ms, out of 240.542 ms total instance construction. Prototype definitions
cost 552.230 ms across 51 instantiations, including 364.566 ms deserialization
and 312.634 ms nested Mesh work. Source geometry costs 497.797 ms including
120.018 ms validation/bounds and 320.756 ms buffer creation. These measurements
make material reconstruction and definition Mesh construction useful next
targets; retaining duplicate prototype geometry remains excluded.

Reusable allocator/list/fence/event resources are a plausible bounded follow-up:
85 synchronous uploads each create command resources. The original last cold
build records 21.382 ms command creation/recording, 4.813 ms submission and
32.204 ms waits; the candidate has 32.387 / 6.429 / 119.618 ms respectively.
The wait spike is wall time, including scheduling, and is not isolated GPU copy
time. Reuse would address creation/teardown, while batching could also amortize
submission/waits. Batching requires an explicit completion boundary retaining all
destinations, staging and command resources until the fence completes; the current
factory promises a ready buffer on return. No waits were removed or resources
recycled before completion. Prioritize the larger measured material/definition
work, then measure bounded command-resource reuse before introducing a deferred
batch-upload contract. This evaluation is separate from recurring polling work.

A final diagnostic with the same 54 non-executable inputs/settings separates the
material work further. In its last cold rebuild, total batch material work is
206.456 ms: serialization 9.426 ms, **deserialization 189.231 ms**, and the
`OnAfterDeserialize` graphics hook 0.125 ms, each across 340 calls. Definitions
cost 500.380 ms, including 302.061 ms nested Mesh deserialization; instance
transform/bounds work is 2.641 ms and the instance parent 217.011 ms. Focus the
next investigation on `Material::Deserialize` (including its material-file load
and texture setters) before changing upload submission. Their individual causes
are not yet isolated. This is construction, not recurring signature/stat polling.
Its Stop grass XML conversion is 609.198 ms; callback 2257.170 ms, placement
records 490.113 ms and prepared output 189.632 ms. These are additional diagnostic
samples, not a balanced repeated before/after experiment.

Evidence: `.local/performance-logs/cold-stop-investigation.json`,
`cold-stop-{callers,xml-capacity}-release.*` manifests/captures beside it and
corresponding `.local/*-stages.log`; final material subphases are in
`cold-stop-material-phases-release.*`. The analyzer verifies input resolution,
identity and effective settings. Manifests still exclude implicit dependencies.
Six targeted Release checks pass: TerrainSharedPrefabs, AsyncPrefab,
PrefabDocumentCache, JsonObjectConstruction, JsonWriterCompatibility and
PerformanceInputIdentity (`.local/cold-stop-final-release-tests.log`); final Release Editor and
OakBackendCapture builds pass (`.local/cold-stop-final-release-build.log`).
Debug was not rebuilt for this stage. The earlier
intermittent Debug grass diagnostic remains unresolved; this stage does not
establish a grass timing fix.

## Completed fixed-layout placement validation (2026-11-07)

`Scripts/profile_tree_memory.py` accepts `--layout-seed path`, defaulting to the
workspace `imgui.ini`. It copies the chosen bytes to the Editor's working layout
and retains an exact `.layout-seed.ini` sidecar. The manifest records seed source,
snapshot path and SHA-256, fingerprints the chosen source, and checks that the
copied bytes match the pre-launch fingerprint. The seed cannot alias the working
layout or its snapshot. Explicit seeds let the user continue editing their
workspace layout without changing this capture input. Changing the selected seed
during a capture still invalidates identity; summarize-only leaves history alone.

Nine `PerformanceInputIdentity` tests pass, including explicit seed preservation,
workspace/working-layout edits, selected-seed mutation rejection and output alias
guards. Evidence: `.local/fixed-layout-ctest.log`. To reuse one layout across runs:

```powershell
Copy-Item imgui.ini .local/performance-logs/experiment-layout.ini
python Scripts/profile_tree_memory.py .local/performance-logs/experiment.jsonl --editor build/Debug/Release/Editor.exe --renderer dx12 --layout-seed .local/performance-logs/experiment-layout.ini --frames 30 --warmup 10 --stop-frames 2
```

Completed three baseline captures with the original placement constructor, then
three with six-member reservation. Saved placement source variants differ only
in `MakeObject()` versus `MakeObject(6)`; the optimized source was restored and
the final Release Editor rebuilt. No build or benchmark ran alongside captures.
All six manifests have complete, unchanged internal identities; all **54
non-executable inputs** match across runs, including 53 scene/prefab documents
and the fixed seed. Seed snapshots, per-version executable hashes, resolved
references and profile call counts also validate. All frames agree on
Release/DX12, 1920x1080, scale 1, dynamic scaling/temporal upscaling disabled,
shadows enabled with four cascades and a 2048 atlas. Each process records 30
measured Play, 10 warmup and two Stop frames in the same static scene.

Times below are **median [minimum-maximum] milliseconds**, three processes per
version. Placement record construction improved in every candidate sample
relative to every baseline sample in these groups.

| Scope | Original constructor | Six-member reservation |
| --- | --- | --- |
| Initial grass placement records, one call | 326.499 [318.898-616.595] | 244.781 [228.677-295.882] |
| Stop placement records, three calls aggregated | 580.805 [580.774-645.549] | 491.792 [470.943-496.026] |
| Stop callback | 2646.220 [2294.440-2686.450] | 2277.870 [2122.880-2286.320] |
| Stop prefab read/parse | 971.091 [838.989-1121.170] | 871.244 [835.313-877.745] |

The target record scope's median is **25.0% lower at initial load** and **15.3%
lower across Stop serializations**. The callback median is 13.9% lower in this
sample, but parsing and other work vary too; do not attribute its entire change
to object reservation. The runs are sequential baseline-then-candidate, with
uncontrolled background activity and only three samples each. Nested scopes
overlap, aggregate Stop calls are not solely the prefab source snapshot, and
these short transition captures establish no steady FPS gain.

Evidence: `.local/performance-logs/capacity-fixed-comparison.json`
(`comparison_valid=true`), `capacity-fixed-{before,after}-release-{1,2,3}.*`,
corresponding `.local/*-stages.log`, build logs, and saved baseline/candidate
placement source files under `.local`. The analyzer verifies input/settings/call
counts before producing the comparison. Fingerprints exclude implicit runtime
dependencies. The authored grass asset and workspace layout were preserved.
This completes the matched-input validation task; repeated serialization callers
and remaining cold Stop work stay open. The earlier rejected layout-mismatched
pair remains invalid historical evidence.

## Reserve members for fixed-size placement JSON objects (2026-11-07)

`JsonValue::MakeObject(memberCapacity)` creates an empty object with an explicit
allocation hint. Existing no-argument construction is unchanged. Placement
serialization alone uses a six-member hint, matching its id, asset, position,
normal, yaw and scale fields. This avoids repeatedly growing each record's
member vector; outer placement arrays and position/normal arrays are unchanged.
The hint is not a limit and does not create fields. Serialized values, insertion
order, replacement/erase, deep-copy and move ownership retain their semantics.

The optional `JsonConstructionTests` target checks these behaviors, growth beyond
the hint, and exact pretty/compact output equality for default versus reserved
placement records. Its `--benchmark` alternates the two paths in one executable,
with two warmup rounds and seven samples each. Each sample constructs 65,536
six-field records; construction and destruction are timed separately:

| Configuration | Default construction median | Reserved construction median | Default / reserved destruction median |
| --- | --- | --- | --- |
| Release | 64.960 ms | 59.370 ms | 24.129 / 26.021 ms |
| Debug | 1362.710 ms | 1019.150 ms | 219.135 / 215.553 ms |

Release construction samples range **56.189-99.044 ms** default and
**46.632-77.870 ms** reserved. Debug ranges **1352.530-1930.680 ms** and
**971.516-1443.550 ms**. This is an isolated construction comparison, not a
scene restoration or FPS measurement. Logs are
`.local/json-object-capacity-{debug,release}-benchmark.log`.

Debug/Release Editors build, and both standalone JSON regressions pass in each
configuration. The full shared-prefab regression passes in Release and on a
Debug diagnostic rerun. The first Debug run failed the pre-existing grass phase
accumulation assertion before reaching placement checks. That failure is
preserved in `.local/json-object-capacity-debug-test.log`; a diagnostic rebuild
and full rerun pass in `.local/json-object-capacity-debug-diagnostic-test.log`.
The test now emits actual first/second/scene/phase totals if the assertion fails,
and exposes `grass-phase-timing` for isolated repetition. That mode passes 1,000
checks in both configurations (`.local/json-object-capacity-*-grass-stress.log`).
No cause or production grass fix has been established; retain this evidence if
the intermittent assertion recurs.

Short Release/DX12 1920x1080 captures are retained as
`.local/performance-logs/json-object-capacity-{before,after}-release.*`.
Each run has complete, unchanged input identity internally, and all 53
scene/prefab document identities match across runs. However, the user layout
seed `imgui.ini` changed between runs (12,842 to 12,843 bytes, different SHA-256).
The comparison is therefore **rejected for matched timing claims**. The user
layout was preserved. `.local/performance-logs/json-object-capacity-comparison.json`
records `comparison_valid=false` and the changed input. Do not claim a Stop
speedup or steady FPS improvement from these captures. The retained candidate's
measured evidence is the controlled paired construction benchmark above.

## Placement snapshot phases and single-pass restore signature (2026-11-07)

The existing opt-in prefab profile now separates `placement_snapshot_prepared`
(tile preparation/binary and hex output), `placement_snapshot_records` (JSON
placement records) and their `placement_snapshot_total` parent. It also reports
`placement_restore_signature` and `placement_restore_baked`. These are scopes
around bulk operations, not timers inside each placement loop.

`Deserialize` already normalizes placement IDs and computes the signature. It
now passes that local result to baked validation instead of building the ID set
and hashing all placements again. No placements change between those operations;
the signature algorithm, bake schema and validation checks remain unchanged.
There is no new persistent signature cache or change to polling/edit detection.
Regression coverage checks missing/duplicate ID normalization, valid baked
round-trips with exact serialized values, stale-bake rejection and rebuild,
duplicate baked-index rejection and legacy data without a bake. Debug/Release
Editors build and both shared-prefab regressions pass; logs use
`.local/placement-signature-{debug,release}-{build,test}.log`.

Streaming inspection of the current authored grass prefab finds **324,147
placements and no bakedInstances field**. Identity is the same 113,905,987-byte
file recorded below; evidence is
`.local/performance-logs/placement-signature-asset-shape.json`. The asset was
preserved. Thus the removed duplicate validation pass principally helps baked
layers, not the current unbaked grass input. The grass layer still needs ID
normalization/signature computation and builds its derived bake for serialization.

Short Release/DX12 1920x1080 captures use 30 measured Play frames, 10 warmup
and two Stop records. Inputs are complete/unchanged within each run; all 54
non-executable fingerprinted inputs match across the pair. Stop layer
deserialization is **165.587 / 137.201 ms** and total callback
**2446.39 / 2399.37 ms**, but this single pair does not isolate a causal gain:
read/parse also varies **837.796 / 852.778 ms**, and source capture
**460.351 / 428.816 ms**. Do not attribute the full differences to signature
reuse or claim a steady FPS gain.

After-change Stop signature time is **64.769 ms across two calls**, and baked
load is **1.165 ms across one call**. Placement serialization totals
**782.886 ms across three calls**, comprising **147.010 ms** prepared output
and **635.810 ms** JSON records, with a small remainder. These aggregate every
layer serialization inside the Stop profile, not only `source_snapshot_capture`;
do not add them to parent scopes or interpret three calls as three identical
grass copies. The initial grass load has one snapshot call: **379.952 ms**,
including prepared output **106.991 ms** and records **272.945 ms**. Placement
record construction and identifying repeated serialization callers remain useful
next investigations; the earlier rejected array-reservation experiment is not
evidence that all allocation changes would help.

Evidence: `.local/performance-logs/placement-signature-comparison.json`,
`placement-stages-before-release.*`, `placement-signature-after-release.*`,
and corresponding `.local/*-stages.log` files. Fingerprint scope still excludes
implicit runtime dependencies.

## Parse the owned prefab XML buffer in place (2026-11-07)

The cold prefab reader now uses pugixml's non-owning `load_buffer_inplace` on its
existing mutable file buffer. The source string outlives the XML document;
`ReadNode` produces independently owned JSON values before either is destroyed.
JSON detection/fallback and pugixml's encoding conversion remain unchanged.
This removes the extra full-file parser buffer/copy for native UTF-8 XML; other
encodings may still require a converted buffer. No source file is modified.

The current UTF-8 meadow-grass prefab is **113,905,987 bytes**, SHA-256
`47db145ebd50f3b692133f910335e40c0bd4cba3225a4206949b803800dcbb44`.
It was preserved. Format regression coverage includes JSON, whitespace XML,
UTF-8 BOM, UTF-16 little/big-endian, UTF-32 little-endian, empty/malformed input,
unchanged file bytes, entity decoding and retained large string values after
parser destruction. Debug/Release Editors build. Release shared-prefab,
asynchronous prefab and document-cache regressions pass in both configurations,
including the full 2,048-placement forest. Logs use `.local/xml-inplace-*-build.log`,
`.local/xml-inplace-release-tests.log` and `.local/xml-inplace-debug-*.log`.

Short Release/DX12 1920x1080 pair: 30 measured Play frames, 10 warmup and two
Stop records per run. Each input manifest is complete/unchanged, and all 54
non-executable fingerprinted inputs match across runs (implicit runtime
dependencies remain outside this manifest scope). Stop prefab read/parse is
**852.597 to 822.980 ms**; callback **2355.69 to 2278.89 ms**. Snapshot capture
also varies **419.531 to 387.779 ms**, while encoding is similar
**294.804 / 296.178 ms**. One pair does not isolate every stage's variation;
do not attribute the entire callback difference to this change or claim a steady
FPS gain. Scopes overlap. Process lifetime peak working set is
**2,257,682,432 / 2,223,415,296 bytes**; it includes other allocations and does
not measure the isolated parser allocation removed.

Evidence: `.local/performance-logs/xml-inplace-{before,after}-release.*`,
`.local/performance-logs/xml-inplace-comparison.json`, and corresponding
`.local/xml-inplace-*-stages.log` files. Remaining placement value-tree
construction, XML-to-JSON conversion, definition/batch construction and command
upload work remain open.

## Append ordinary JSON string spans in bulk (2026-11-07)

`Json.cpp` now scans strings for the existing escape characters, appending the
ordinary spans in bulk instead of appending each byte individually. This applies
to both pretty and compact output, including object keys. It preserves the
writer's byte output and does not change the prefab or baseline format.

The optional standalone `JsonWriterTests` target checks an escape golden,
leading/trailing/consecutive escapes, UTF-8, every byte value (including embedded
nulls), object keys, empty strings and large payloads. It also round-trips a large
escaped string. `JsonWriterCompatibility` passes in Debug and Release. Both
Editors build and both shared-prefab integration regressions pass, including all
2,048 forest placements, 85 sources and 340 batches. Logs are
`.local/json-span-{debug,release}-test.log`.

Isolated eight-write averages (`JsonWriterTests --benchmark`), with no concurrent
build or benchmark; these measure JSON writing, not scene restoration:

| Input | Release before / after | Debug before / after |
| --- | --- | --- |
| 16 MiB unescaped payload | 28.679 / 14.355 ms | 133.023 / 52.110 ms |
| 32,768 placement paths | 3.161 / 2.672 ms | 12.483 / 8.203 ms |
| Dense escapes, 589,826 output bytes | 1.332 / 0.973 ms | 4.780 / 5.353 ms |

The Debug dense-escape case regresses slightly; the intended large baked payload
and placement-path cases improve in both configurations. Raw logs are
`.local/json-span-{before,after}-{debug,release}-benchmark.log`.

A short Release/DX12 1920x1080 Edit/Play/Stop pair (30 measured Play frames,
10 warmup, two Stop records) retains complete, unchanged identities within each
run. All 54 non-executable fingerprinted files, including scene/prefab documents
and layout seed, match across runs; executable identity changes for the rebuild.
The input scope excludes implicit runtime dependencies. The authored grass prefab
is preserved. Stop source snapshot encoding is **336.713 to 286.592 ms**
(14.9% lower); Stop callback is **2394.48 to 2336.12 ms**. Read/parse varies
**841.022 to 887.717 ms** and capture **443.161 to 407.992 ms**. These are one
pair of nested measurements, not independent additive costs or a steady FPS
claim. Evidence: `.local/performance-logs/json-span-comparison.json`,
`xml-borrow-current-release.*`, `json-span-current-release.*`, and corresponding
`.local/*-stages.log` files. The XML borrowing change preceding this pair has no
matched before/after timing here.

## Borrow XML scalar and numeric-array input (2026-11-07)

The XML reader now classifies scalar text through a borrowed string view and
parses encoded numeric arrays directly from pugixml's null-terminated storage.
Previously both helpers first constructed a temporary owned string. This also
copied large baked placement payloads before creating the retained JSON string.
Retained strings still own their storage; number conversion, overflow handling,
explicit string typing and the prefab schema remain unchanged. The current
authored meadow-grass prefab was preserved.

The shared-prefab regression includes a 1 MiB attribute whose value is checked
after the XML document has been destroyed, plus booleans, exponent numbers,
overflow and partially numeric strings, empty text and encoded numeric arrays.
Debug and Release Editors build and both shared-prefab regressions pass. Validation logs are
`.local/xml-borrow-{debug,release}-{build,test}.log`.
No matched before/after timing or steady FPS improvement is claimed; remaining
cold Stop construction, encoding and upload work is still open.

## Automatic pre/post capture input identity (2026-11-07)

`Scripts/profile_tree_memory.py` now writes an adjacent `.inputs.json` before
launching the Editor. It recursively discovers literal `.scene`/`.prefab`
references in XML/JSON, handles cycles, and records resolved paths, byte sizes,
mtime and SHA-256. It also fingerprints the executable, layout seed and optional
repeatable `--input path` files (for example a route or settings file). Large XML
documents are streamed, and hashing runs outside the recorded Editor lifetime.

After Editor exit, inputs are rediscovered and fingerprinted again. The sidecar
records changed contents/metadata, changed resolutions, missing/malformed inputs
and capture success independently. A changed or incomplete identity makes the
capture command fail comparison validation while preserving raw data/summary.
The before/after evidence is retained on subprocess failure too. Summarize-only
does not fingerprint today's assets or overwrite a historical manifest.

This scope covers scene/prefab documents and the explicit files above, not a
complete runtime mesh/material/texture/script dependency manifest. Those can be
added explicitly with `--input`; identity alone does not establish matched camera
route coverage or effective quality settings. The older post-capture-only hash
artifacts are not retroactively promoted to pre-capture evidence.

Seven regressions cover XML/JSON recursion and cycles, same-size/same-timestamp
content edits, resolution overrides, missing/malformed files, relocation/encoding,
failed capture evidence, successful capture rejection on edits and summarize-only
preservation. CTest `PerformanceInputIdentity` passes. A real Release DX12 capture
with 30 measured / 10 warmup / two stopped frames records 55 files, complete and
unchanged before/after. Evidence:
`.local/performance-logs/input-identity-validation.inputs.json`, capture/summary
beside it, and `.local/input-identity-tests.log`. No rendering speedup is claimed.

## Mark Play-start work and its delayed interval sample (2026-11-07)

Added `play_start` to frame activities. Play requests, pending-start updates and
`Scene::Start` mark the current record. Two following updates carry the tag:
one for the first simulation/recording work, and one for the next record that
reports that work's update interval. Stop clears pending followup state. Tagged
records have `recurring_frame=false`; the tag does not replace independent
warmup/transition-inclusive reporting or establish that all streaming is warm.

Both Editor configurations build and their PerformanceRecording tests pass,
including combined activity bits and consume/clear behavior. Runtime validation
uses 60 measured Play frames, zero warmup and two stopped frames in each build.
Each capture tags the first three Play records, with the remaining 57 recurring.
Debug startup work is 202.12 ms and the subsequent interval is 203.83 ms; both
are tagged. Release startup work 74.39 ms and its following interval 74.51 ms
are also tagged. This alignment prevents a transition's interval from leaking
into the first recurring FPS sample. It does not change rendering performance.

Evidence: `.local/performance-logs/play-start-final-{debug,release}.jsonl` and
`play-start-validation.json`; build/test logs:
`.local/play-start-final-{debug,release}-{build,tests}.log`.

## Complete grass Refresh timing coverage (2026-11-07)

Added exclusive CPU wall-time phases for reference/preview setup, source loading,
placement configuration/indexing, visibility/tile/material/LOD updates, retirement,
scheduling/BeginBuild, build/activation, resource accounting and other work. JSONL
exports `grass_{references,sources,placement,visibility,retirement,scheduling,build,
accounting,other}_ms`; the Performance panel shows these separately from existing
nested scan/generation/upload timings. Add the exclusive phases to obtain total;
do not also add the nested diagnostics. Wall time includes scheduling/preemption.

Refresh now publishes timing for missing-reference/source and empty-authored-data
early returns, which previously skipped scene aggregation. Tile behavior, work
budgets, density, fades and quality are unchanged. Regression covers early-return
publication and aggregation without losing or double-counting phase time.

Release Editor/standalone build and shared-prefab regression pass. Validation:
`Scripts/profile_tree_memory.py .local/performance-logs/grass-phases-release.jsonl
--editor build/Debug/Release/Editor.exe --renderer dx12 --frames 600 --warmup 120
--stop-frames 2 --timeout 300`. All 752 recorded frame rows have nonnegative phase
times and phase sums equal total within floating-point rounding. The primary scene
and grass-prefab hashes were recorded before capture and rechecked afterward,
both unchanged; this is not a complete dependency manifest. Evidence:
`.local/performance-logs/grass-phases-analysis.json` and `grass-phases-input.json`.

Measured Play averages about 0.167 ms of grass Refresh wall time; maximum is
0.4625 ms, including visibility/tile work 0.3163 ms. This short automated Release
route does not reproduce the 40-49 ms Debug user spikes and establishes no fix
or speedup. Reproduce the user's Debug route with the new phase fields before
choosing an optimization.

Debug Editor and standalone builds also pass, as does the direct Debug
shared-prefab regression with the new phase tests. A second automated 1080p DX12
capture uses 600 measured / 120 warmup / two stopped frames. Its 752 records have
nonnegative phases summing to total within 0.0005 ms of exported rounding. Primary
scene/grass hashes match before/after. Measured Play grass total averages 0.801 ms
and peaks at 1.2232 ms; average visibility/tile work is 0.437 ms. This also does
not reproduce the user's 2560x1369 Debug route spikes. Evidence:
`.local/performance-logs/grass-phases-debug{,-analysis,-input}.json*` and
`.local/grass-phases-debug-tests.log`. Both updated Editor configurations are
available for further recording; no timing improvement is claimed.

## User capture at 20:56 (2026-11-07)

`performance-20261107-205641-769.jsonl` confirms Debug Editor/assertions enabled,
DX12, 2560x1369, scale 1, dynamic/temporal scaling off, four shadow cascades and
a 2048 atlas. The file contains 1366 full `frame_details` rows and 1366 summary
`frame` rows for the same updates; do not double count them. Full samples contain
29 Edit and 1337 Play updates, with no scene Stop transition. Recorder shutdown
does not measure Stop restoration. All activity flags are empty/recurring,
including the Play-start spike.

All Play averages 33.13 FPS from the mean update interval, P95 38.57 ms and P99
48.62 ms. Excluding the first 120 Play updates as an explicit warmup sensitivity
check leaves 1217 updates: 33.33 FPS, P95 38.43 ms, P99 45.71 ms. This is not a
matched route or proof of steady workload; no asset hashes are recorded.

Warmed command recording averages 14.04 ms, of which view recording is 13.62 ms.
Physics averages 3.45 ms, editor UI 1.36 ms. All warmed updates have unique valid
GPU samples: instrumented total 11.82 ms, opaque 9.16, shadows 1.86, grass 0.60,
terrain 0.20 ms. CPU/GPU and parent/child scopes overlap; do not sum them into
a frame budget or assume the instrumented regions cover the complete GPU frame.
Simulation GPU waits and pipeline queue waits are zero.

Prefab polling has 36 warmed events above 1 ms, 6.06-6.61 ms, mean 6.27 ms.
The largest warmed work spikes are frames 537/683, 74.62/70.81 ms, with grass
total 48.95/39.67 ms. Scan/upload are zero, generation about 0.0001 ms and zero
tiles are built. Unscoped grass maintenance/task polling/retirement/scheduling
requires investigation before attributing the cost to generation or upload.
Long frame work commonly appears in the following update-interval sample.

Priorities are a same-resolution Release reproduction with asset/route identity,
grass-scope coverage, CPU view recording and opaque leaf/vertex-cost separation.
Preserve authored density and quality. No Stop improvement, matched renderer
regression or sustained 100+ FPS conclusion follows from this recording.
Detailed Markdown/JSON analyses are adjacent to the capture under
`.local/performance-logs/performance-20261107-205641-769-analysis.*`.

## Transfer worker prefab documents without duplicate copies (2026-11-07)

Cold synchronous prefab instantiation previously called the public by-value
cached file reader on a worker. That copied the worker-cache document into the
future result, then copied it again into the scene-owner cache. It now parses
into a transient worker result and uses the existing single-consumer
`PreparedPrefab::Publication` transfer into the owner cache. Parsing remains on
the worker; live components and mutable cache state remain on the owner thread.
No shared mutable document, extra cache or retained prototype geometry is added.
The public by-value reader and existing eight-entry owner cache/invalidation
behavior remain available unchanged.

Important input change: `OakMeadowGrass.prefab` is currently user-modified and
113,905,987 bytes (about 114 MB). It was preserved. The latest pair confirms
Release / assertions off / DX12 at 1080p, 10 warmup / 60 measured / two stopped
frames. Do not compare these costs directly with earlier small-document captures.
Current asset size/mtime/hash were recorded after these captures in
`.local/performance-logs/prefab-transfer-input.json`; no pre-capture hash was taken.

| Latest Release Stop scope | Before | After |
| --- | ---: | ---: |
| Read/parse (includes worker wait/result transfer) | 1112.31 ms | 968.10 ms |
| Object reconstruction parent | 2419.94 ms | 1998.47 ms |
| Source capture (unchanged code in this pair) | 420.68 ms | 442.67 ms |
| Source encoding (unchanged code in this pair) | 303.93 ms | 309.80 ms |
| Complete callback | 2948.21 ms | 2504.57 ms |

These are observed short-transition timings with varying host load and a changed
authored workload relative to earlier evidence. Nested scopes overlap. Do not
assign all callback movement to the transfer fix or infer a steady-play FPS gain.
Evidence: `.local/performance-logs/prefab-transfer-{before,after}-release.jsonl`,
`.local/prefab-transfer-{before,after}-stages.log` and
`.local/performance-logs/prefab-transfer-comparison.json`.

Release Editor and standalone targets build; shared-prefab, asynchronous
publication and document-cache CTests all pass (3/3). Debug standalone targets
build; async publication and document-cache CTests pass (2/2). These cover
owner-thread construction, prepared component consumption, nested references,
independent instances, baseline equivalence and file-edit invalidation. Logs:
`.local/prefab-transfer-{debug,release}-tests.log`.
The direct Debug shared-prefab/forest regression also completes successfully:
`.local/prefab-transfer-debug-shared.log`.

## Current Debug restoration and rejected reservation experiment (2026-11-07)

The Debug Editor linked successfully for a current diagnostic capture. Using
DX12 at 1080p, 10 warmup / 60 measured / two stopped frames and opt-in profiling:

| Current Debug Stop scope | CPU wall time |
| --- | ---: |
| Complete restoration callback | 3262.25 ms |
| Pipeline drain | 11.14 ms |
| Clear objects | 107.21 ms |
| Object reconstruction | 2194.59 ms |
| Nested prefab read/parse | 951.74 ms |
| Nested source value capture | 747.05 ms |
| Nested source encoding | 112.80 ms |

The first stopped frame measures 6483.81 ms, including subsequent rendering
2990.82 ms and nested prefab preparation 2672.58 ms. Cold forest batch profile
total is 2626.86 ms. Do not add nested scopes. This establishes the current
Debug restoration costs, not the cause or magnitude of the earlier historical
4.1 s gap under different code. Evidence: `.local/stop-current-debug-stages.log`
and `.local/performance-logs/stop-current-debug.jsonl`.

Tested reserving exact terrain/placement serialization array capacities. The
repeat Debug capture measures source capture 738.22 ms and reconstruction
2200.84 ms, with first stopped frame 6719.60 ms. No meaningful improvement was
established; the added array-capacity API and its call sites were removed.
Experiment evidence: `.local/array-reserve-debug-stages.log` and
`.local/performance-logs/array-reserve-debug-comparison.json`.

The restoration's prefab reference is `OakMeadowGrass.prefab`, whose placement
records were built through chained temporary JSON expressions. The retained change
moves each completed record and its position/normal arrays into their destination,
avoiding nested deep copies. No timing improvement is claimed for that change
without a new capture. A subsequently running Debug Editor locks its executable,
so final Debug Editor relinking is pending; it was left running.

Final Debug standalone target builds and its direct `shared-prefabs` run completes
successfully, including 2048 placements / 85 sources / 340 batches, serialization,
transforms and collider edits. CTest runs hit the existing 60-second timeout,
including one during a competing full build and another without one. The direct
run does not turn those timed-out runs into passes. Logs:
`.local/placement-move-debug-tests.log` and
`.local/placement-move-debug-direct.log`.
Final Release Editor/standalone builds and the Release shared-prefab CTest pass.
Release test log: `.local/placement-move-release-tests.log`.

## Remaining serializer return audit (2026-11-07)

Extended the direct-local return fix to 12 return paths across 11 files:
animation clips, animation-manager layers, model node paths, rig impulse morphs,
camera/light tracks, skeletons, skinned-mesh joints/weights, material morphs,
both mesh-morph branches, cloth anchors and terrain-manager state. Each function
now completes its field updates before returning its own local value, allowing
move/elision instead of deep-copying through the reference returned by `Set`.
This does not alter serialized values, order, formatting or ownership semantics.

A follow-up `return <identifier>.Set/Push(...)` search finds no remaining
named-local chained return expressions in `Engine/Core`. Temporary fluent
expressions are outside this audit. No asset-specific benchmark was taken and
no additional forest or animation FPS improvement is claimed. The preceding
terrain measurements must not be extrapolated to these assets.
Release Editor/standalone and Debug standalone builds pass; existing shared-prefab
serialization, invalidation and override/revert checks pass in both configurations.
These do not exercise every audited animation/cloth data shape. Logs:
`.local/serializer-return-audit-{debug,release}-tests.log`.

## Avoid deep copies when returning serialized terrain (2026-11-07)

Terrain and placement-layer serializers returned `out.Set(...)`. Because `Set`
returns `JsonValue&`, the return expression was an lvalue and deep-copied the
entire completed tree. They now finish the field updates and return `out`,
allowing return-value elision or a move. Fields, ordering, numeric values and
independent ownership of returned snapshots are unchanged. No cache was added.

Same short profiled Release DX12 1080p protocol: 10 warmup / 60 measured / two
stopped frames. Baseline is `.local/source-snapshot-after-stages.log`; after is
`.local/source-capture-move-stages.log` with capture
`.local/performance-logs/source-capture-move-release.jsonl`. Comparison:
`.local/performance-logs/source-capture-move-comparison.json`.

| Stop restoration scope | Before | After |
| --- | ---: | ---: |
| Source value-tree capture | 55.42 ms | 35.17 ms |
| Source baseline encoding | 20.06 ms | 20.83 ms |
| Source snapshot parent | 83.82 ms | 64.84 ms |
| Object reconstruction parent | 189.53 ms | 177.88 ms |
| Complete Stop callback | 303.02 ms | 270.11 ms |

Capture is 36.5% lower in this pair. Parent scopes overlap; parsing and other
costs vary, so do not assign the entire callback delta to this change. These
are cold-transition measurements, not steady-play FPS gains. Other serializers
with chained lvalue return expressions remain worth auditing.
Release Editor/standalone and Debug standalone builds pass. Shared-prefab
serialization, invalidation and override/revert regressions pass in both
configurations, and the automated Release Play/Stop capture completes. Logs:
`.local/source-capture-move-{debug,release}-tests.log`.

## Compact internal prefab baselines (2026-11-07)

Source-snapshot profiling now separates live-object capture into a value tree
from baseline JSON encoding. Internal prefab source baselines now use
`JsonWriteCompact`, which shares the existing numeric/string writer and omits
formatting whitespace. `JsonWrite` keeps its pretty output, including the final
newline. Saved documents retain their existing formatting. Baselines remain
ordinary JSON consumed by override detection/revert; no prototype cache was added.

Matched short Release DX12 1080p protocol: 10 warmup / 60 measured / two stopped
frames with opt-in profiling. Evidence:
`.local/performance-logs/source-snapshot-{before,after}-release.jsonl`,
`.local/source-snapshot-{before,after}-stages.log` and
`.local/performance-logs/source-snapshot-comparison.json`.

| Stop restoration scope | Before | After |
| --- | ---: | ---: |
| Source value-tree capture | 56.46 ms | 55.42 ms |
| Source baseline encoding | 32.77 ms | 20.06 ms |
| Source snapshot parent | 98.66 ms | 83.82 ms |
| Object reconstruction parent | 205.77 ms | 189.53 ms |
| Complete Stop callback | 318.76 ms | 303.02 ms |

Encoding is 38.8% lower in this pair. Parent scopes include children and other
work; do not sum them or assign every callback change to the writer. These are
short cold-transition measurements, not steady-play FPS gains. Value-tree capture
remains the larger measured source-snapshot child.

Regression checks unchanged pretty output, compact nested-value round trips,
floating-point precision, escaped strings and smaller text, plus actual prefab
override detection and revert from the compact baseline.
Release Editor/standalone and Debug standalone builds pass. The expanded
shared-prefab regression passes in both configurations, and the Release
automated Play/Stop capture completes. Test logs:
`.local/source-snapshot-{debug,release}-tests.log`.

## Single-read prefab loading (2026-11-07)

The prefab file loader previously read through character iterators and tried
JSON before reopening XML files through pugixml. It now sizes and reads the file
once, then parses that buffer. Leading-whitespace XML selects the XML parser
directly; JSON retains its parser, with XML fallback preserving BOM and UTF-16
support. File metadata/resolution cache checks and source-snapshot semantics
are unchanged. No prototype geometry cache was added.

Same short Release DX12 1080p Play/Stop protocol as the preceding restoration
capture: 10 warmup / 60 measured / two stopped frames, opt-in prefab profiling.
Before: `.local/stop-restore-stages.log`; after:
`.local/prefab-read-once-stages.log` and
`.local/performance-logs/prefab-read-once-release.jsonl`.

| Stop restoration scope | Before | After |
| --- | ---: | ---: |
| Prefab read/parse | 111.70 ms | 74.91 ms |
| Object reconstruction parent | 237.39 ms | 203.12 ms |
| Source snapshot | 94.33 ms | 95.62 ms |
| Object clearing | 33.19 ms | 48.81 ms |
| Complete callback | 343.09 ms | 326.18 ms |

Read/parse is 32.9% lower in this pair; teardown and other costs vary, so do not
assign the entire callback delta to this change. Nested scopes overlap.
Source-snapshot construction remains a target. No steady-play FPS claim.
Shared-prefab regression now checks JSON, whitespace XML, UTF-8 BOM XML and
UTF-16 XML, and rejects empty/malformed fixtures.
Release Editor and standalone targets and Debug standalone build; shared-prefab
regressions pass in both configurations. The automated Release Play/Stop capture
completes. Logs: `.local/prefab-read-once-{debug,release}-tests.log`.

## Stop scene restoration scopes (2026-11-07)

Added opt-in scopes around the Stop callback's play-pipeline drain, cancellation
and background fence, and editor restoration. Scene deserialization separately
times clearing live objects and reconstructing authored objects. These scopes
do not alter restoration or teardown behavior. Graphics preparation occurs
after this callback and is measured separately.

Release DX12 at 1920x1080 with 10 warmup / 60 measured / two stopped frames and
`ENGINE_PREFAB_LOAD_PROFILE=1` produces
`.local/performance-logs/stop-restore-release.jsonl`; appended timings are in
`.local/stop-restore-stages.log`.

| Stop restoration scope | CPU wall time |
| --- | ---: |
| Complete callback | 343.09 ms |
| Play pipeline drain | 11.38 ms |
| Cancel/fence background work | 0.02 ms |
| Editor scene restoration | 331.49 ms |
| Nested object clearing | 33.19 ms |
| Nested object reconstruction | 237.39 ms |

Within the root, existing parsing and source-snapshot scopes measure 111.70 and
94.33 ms. They are nested work, not additional contributions to sum with the
parent scopes. Reconstruction's parsing/snapshot work deserves investigation.
This Release result cannot establish the cause of the earlier roughly 4.1 s
outside rendering in Debug. Repeat with the Debug Editor when linking is
available; retain build identity and exclude Stop transitions from steady-play
statistics. No speedup is claimed for this instrumentation.
Release Editor and standalone targets build, the automated Play/Stop capture
completes, and the shared-prefab restoration/invalidation regression passes.
Test log: `.local/stop-restore-release-tests.log`.

## Bounded DX12 staging reuse (2026-11-07)

Split allocation into destination allocation, staging allocation and staging data
copy. Baseline cold forest staging allocation costs 30.07 ms across 85 sources,
versus 62.12 ms for destination allocation and 10.77 ms for copying.

Each DX12 factory now retains at most one completed upload scratch buffer,
capped at 16 MiB. Smaller uploads reuse it; capacity grows geometrically within
the cap, and larger uploads remain temporary. Scratch is removed from the cache
before writing and returned only after a successful fence wait. Concurrent
uploads use separate in-flight scratch and return only the larger eligible
completed allocation. The cache mutex is not held while submitting or waiting.
Final vertex buffers remain GPU-local. The tradeoff is up to 16 MiB of retained
upload-heap memory per factory; no CPU prototype cache or authored quality change.

Before/after Release DX12 captures use 1920x1080, 10 warmup / 60 measured / two
stopped frames with opt-in profiling. Evidence:
`.local/performance-logs/dx12-staging-{before,after}-release.jsonl`,
`.local/dx12-staging-{before,after}-stages.log` and
`.local/performance-logs/dx12-staging-comparison.json`.

| Last cold forest build, 85 sources | Before | After |
| --- | ---: | ---: |
| Destination allocation | 62.12 ms | 46.75 ms |
| Staging allocation/cache acquisition | 30.07 ms | 2.70 ms |
| Staging copy | 10.77 ms | 8.72 ms |
| Command recording/creation | 20.30 ms | 24.35 ms |
| Submission | 5.74 ms | 5.96 ms |
| Fence wait | 19.49 ms | 20.80 ms |
| Backend creation parent | 179.31 ms | 124.52 ms |

The after cold build makes one fresh staging allocation across 85 uploads.
Staging allocation/acquisition drops 91.0%; destination allocation and other
scopes vary, so the full parent delta cannot be assigned to reuse. These are
short profiled transitions, not steady-play FPS or precise total Stop results.
Release Editor and standalone targets and the Debug standalone target build.
Actual DX12 forest checks pass in both configurations, and Release GPU validation
passes. Logs: `.local/dx12-staging-release-tests.log` and
`.local/dx12-staging-debug-tests.log`.

## DX12 static upload breakdown (2026-11-07)

Opt-in prefab profiling now separates static vertex-buffer allocation/staging
copy, command recording, submission (including queue-lock acquisition), and
the synchronous fence wait. Upload behavior and staging lifetimes are unchanged.
Release DX12 capture uses the same 1080p, 10 warmup / 60 measured / two stopped
frame protocol below. Evidence:
`.local/performance-logs/dx12-upload-breakdown-release.jsonl` and the appended
profile `.local/dx12-upload-stages.log`.

| Last cold forest build, 85 sources | CPU wall time |
| --- | ---: |
| Destination + staging allocation and initial copy | 91.93 ms |
| Allocator/list/fence creation and command recording | 23.47 ms |
| Queue submission and signal | 5.88 ms |
| Fence wait | 21.10 ms |
| Backend creation parent | 170.84 ms |

Children do not cover all parent work: event creation/arming, resource and
command teardown, and other bookkeeping remain outside them. The wait scope
includes scheduling delay and is not a GPU transfer duration. These are cold
transition costs, not steady-play timings. Waiting is a smaller measured cost
than allocation/copy; investigate destination allocation versus staging
allocation/copy before selecting a resource-reuse or batching implementation.
No performance improvement is claimed for this instrumentation.
Release Editor and standalone targets build. The actual DX12 forest regression
with GPU validation passes; log: `.local/dx12-upload-validation.log`.

## Shared-buffer hashing (2026-11-07)

Added opt-in hash, cache-probe, CPU-mirror and backend-creation subscopes to
`AcquireMeshBuffer`. The content hash now mixes eight-byte words instead of
performing a dependent multiply for every byte. Unaligned reads use `memcpy`;
remaining bytes are handled without overreading. Hashes are internal bucket
keys, not persisted asset identities. Full byte comparison still determines
reuse, and factory/usage/size/stride remain part of the key. Weak ownership and
CPU mirrors are unchanged; no duplicate prototype cache was introduced.

Before and after use Release DX12, 1920x1080, 10 warmup / 60 measured / two
stopped frames, with `ENGINE_PREFAB_LOAD_PROFILE=1` and the same
`Scripts/profile_tree_memory.py` protocol described below. Captures are
`.local/performance-logs/mesh-buffer-{before,after}-release.jsonl`; appended
profiles are `.local/mesh-buffer-{before,after}-stages.log`. The comparison JSON
is `.local/performance-logs/mesh-buffer-comparison.json`.

| Last cold forest build, 85 sources | Before | After |
| --- | ---: | ---: |
| Hash | 144.45 ms | 20.63 ms |
| Cache probe / equality | 0.09 ms | 0.09 ms |
| CPU mirror | 29.82 ms | 34.51 ms |
| Backend creation | 150.62 ms | 158.43 ms |
| Buffer creation parent | 325.40 ms | 214.12 ms |
| Source geometry parent | 482.06 ms | 377.85 ms |

Hashing drops 85.7%. Backend creation includes allocation and submission/waits,
not isolated GPU execution. Parent scopes include their children. Initial-build
hashing also drops (161.84 to 22.44 ms), while backend startup varies substantially
(530.16 to 420.79 ms). These short transition captures establish no steady-play
FPS gain or precise end-to-end Stop improvement.

Shared-prefab regression now exercises unaligned content sizes 1-63, identical
reuse, and a change at every byte position, checking the resulting buffer bytes.
Release Editor and standalone targets build; Debug standalone builds. Debug and
Release shared-prefab regressions pass, as do Release DX11/DX12/Vulkan forest
checks (four Release tests total). Logs: `.local/mesh-buffer-release-tests.log`
and `.local/mesh-buffer-debug-tests.log`.

## Cold source geometry breakdown (2026-11-07)

Added opt-in subscopes under `batch_source_geometry` for copying/index expansion,
transforms, validation/bounds and buffer creation. The tangent matrix is now
computed once per part, preserving the existing normal/tangent normalization
and fallback behavior. No assets, LODs or retained geometry changed.

Short Release DX12 capture: `Scripts/profile_tree_memory.py
.local/performance-logs/prefab-geometry-release.jsonl --editor
build/Debug/Release/Editor.exe --renderer dx12 --frames 60 --warmup 10
--stop-frames 2 --timeout 240`, with `ENGINE_PREFAB_LOAD_PROFILE=1`.
The appended profile is preserved in `.local/prefab-geometry-release-stages.log`.
The last cold forest build has 85 calls per geometry stage:

| Stage | CPU wall time |
| --- | ---: |
| Copy / index expansion | 35.95 ms |
| Transform | 15.17 ms |
| Validation / bounds | 116.98 ms |
| Buffer creation | 526.91 ms |
| Parent geometry scope | 695.28 ms |

Child scopes are included in the parent. Buffer creation includes content hashing,
cache lookup, allocation and backend submission; it is not a GPU upload timer.
Investigate that path next. This short profiled transition is not a matched
before/after performance comparison and establishes no steady-play FPS gain.
Release Editor and standalone targets build; Release shared-prefab and actual
DX12 forest regressions pass. Debug standalone builds and its shared-prefab
regression passes. Debug Editor linking remains blocked by
`LNK1168` on its executable, so no updated Debug Editor capture was taken.

## Cold prefab construction: local parenting (2026-11-07)

Opt-in construction timings identified runtime-batch creation/attachment as the
largest Debug rebuild stage. New batches previously used world-preserving
`MoveObject(..., AsChild)`, then immediately reset their local transform to
identity. That also requested broad scene-registry reconciliation for each
batch. They now use the existing `SetParentLocal()` operation and its targeted
structure notification. Collider construction retains its subsequent authored
local-transform assignment. No prototype/source-geometry cache was added.

Before/after Debug DX12 captures use 1920x1080, 10 warmup, 60 measured Play
frames and two stopped frames, with opt-in stage logging enabled in both:

| First stopped frame | Before | After |
| --- | ---: | ---: |
| Total CPU frame | 11.13 s | 7.45 s |
| Prefab preparation | 6.62 s | 3.01 s |
| Runtime-batch construction stage | 4.56 s | 0.90 s |
| Shared source-geometry stage | 1.12 s | 1.18 s |
| Prototype definitions stage | 0.76 s | 0.76 s |

Cold preparation falls **54.5%**, and the complete stopped frame falls **33.1%**.
Geometry and definition loading remain. About 4.1 s of this transition lies
outside its recorded render scope; snapshot restoration needs further profiling.
Nested definition/deserialization timers overlap and must not be added twice.

Release automatic captures use the same 120-warmup/1,200-measured-frame protocol
as the preceding refresh optimization. Cold prefab preparation changes from
1.214 to 1.163 s and the stopped frame from 1.706 to 1.665 s. This is a modest
observed difference that includes run-to-run variation; Debug's gain must not
be extrapolated to Release. These are construction/transition checks, not a
steady-frame FPS improvement or a matched recreation of the user's 2560x1369
route. Authored geometry, placements, LOD/shadow/material quality stay unchanged.

Debug/Release Editor and capture targets build. Shared-prefab regressions pass
in both configurations, including identity-local transforms and inheritance
from rotated/nonuniformly scaled layers, plus all prior edit/relocation/collider
checks. Actual forest captures pass on DX11/DX12/Vulkan. With
`ENGINE_PREFAB_LOAD_PROFILE=1`, `prefab-load-profile.log` now records per-layer
`prototype_definition`, `batch_source_geometry`, `batch_instances` and
`batch_colliders` stages. Logging is opt-in; unchanged polls return before
creating a build-profile scope. Existing profile-log contents were preserved.
Evidence: `.local/performance-logs/prefab-cold-comparison.md` / JSON and
`.local/prefab-cold-*` logs.

## Periodic prefab refresh optimization (2026-11-07)

The user's 18:19 Debug capture exposed one-second preparation hitches. The
periodic path rebuilt all placement groups before learning that source files
were unchanged. It now checks the placement signature, a small LOD-definition
snapshot and distinct resolved source identities/timestamps first. Unchanged
polls return before regrouping; source queries are reused for reconstruction.
Tracking resolved identity also detects an appearing original-path override
that preserves the fallback file's timestamp. Placement-signature polling
retains compatibility with direct in-place placement edits. Mesh-only layers
retain their early exit; prototype scenes/geometry are not kept alive.

Automatic Release DX12 captures use 1920x1080, scale 1, four cascades/2048 atlas,
120 warmup and 1,200 measured Play frames before and after the production edit:

| Measurement | Before | Final after |
| --- | ---: | ---: |
| Periodic events >0.5 ms | 12 | 11 |
| Refresh-event mean | 6.95 ms | 2.74 ms |
| Refresh-event range | 6.47–8.43 ms | 2.68–2.82 ms |
| Observed FPS | 101.14 | 105.30 |
| Frame interval P95 / P99 | 11.96 / 18.26 ms | 11.45 / 13.64 ms |

Measured refresh-event mean falls **60.5%**. Different wall-clock durations
produce different poll counts. Overall FPS/percentiles are observations of two
automatic runs, not proof that the entire difference is caused by this change.
Camera/route identity is not logged independently; this is not a replacement
for the matched moving-route acceptance. P95 remains above 10 ms.

The final Debug Editor also completes a 360-frame/60-warmup 1080p capture, with
refresh events at 6.21–7.09 ms (6.53 ms mean). That is not matched to the user's
2560x1369 recording. Its first stopped frame still takes **11.14 s**, including
**6.71 s in prefab preparation**. Release cold prefab preparation is 1.24 s
before and 1.21 s after. Cold reconstruction and remaining polling/signature
cost stay open; this change addresses unchanged periodic refreshes.

Both Debug/Release Editor and regression targets build. Shared-prefab tests
pass in both configurations: unchanged polls preserve generated IDs/buffers;
in-place placement/LOD changes, LOD-only source edits, collider edits,
same-timestamp overrides, fallback and deletion/reappearance stay correct.
Final actual forest captures pass on DX11/DX12/Vulkan. No authored LODs,
placements, shadow distance/resolution or material quality changed. Evidence:
`.local/performance-logs/prefab-refresh-comparison.md` / JSON and
`.local/prefab-refresh-*` logs.

## Matched 1080p entrance capture (2026-11-07)

`ENGINE_OAK_CAPTURE_SCALE=2` runs the existing capture harness at 1920x1080
instead of 960x540, saving entrance artifacts in `entrance/1920x1080`.
The supported scales are 1 and 2; other values fail explicitly. Authored
camera/LOD/caster settings and the directional shadow atlas policy remain
unchanged. These captures wait for submissions and read images for correctness;
their execution time is not an FPS benchmark.

`--world-radius 0.25 --reference-directory <entrance-directory>` makes the
analyzer check matched phases, camera/projection, time, aspect ratio, route,
caster range/fade, enabled oak-wind counts and full-refresh policy, plus exact
exported terrain geometry. It then reuses the lower-resolution run's precise
terrain sample set and rejects missing or newly occluded patches. It does not
select a fresh sample set based on resolution or observed stability. Cache
ages may differ between processes; use the full-refresh phases to compare
resolution without retained-tile age differences.

DX12 reuses all 77 terrain patches. Patches that become subpixel in their minor
direction drop from 75 to 71; only six stay at least one pixel wide throughout.
Twelve shadowed samples have stable unshadowed controls at both resolutions.
Their baseline excursions above 8 RGB levels drop from five to four, and their
largest step drops from 34.80 to 14.99 RGB. Within the 1080p report's common
display/world stable-control subset, two patches stay at least one pixel wide;
neither has a large excursion, but two samples cannot establish visual quality.

DX12 results on the same 77 points, using full cascade refresh:

| Phase | >8 RGB excursions at 960x540 / 1920x1080 |
| --- | --- |
| Authored 1 m fade | 10 / 7 |
| Extended caster range | 12 / 10 |
| Authored range, 4 m fade | 9 / 8 |
| Authored range, 8 m fade | 11 / 8 |

The largest all-sample step falls from 34.80 to 25.95 RGB in each phase.
Resolution affects the measurements, but 1080p still leaves most patches
subpixel and does not remove variation. Do not infer a renderer defect or
accept a wider fade from these images. Direct fixed-world shadow-visibility
measurements or an adequately resolved local view remain useful next checks;
keep the 1 m default and existing atlas/LOD settings.

Optional `OakEntrance1080Capture_*` / `OakEntrance1080Analysis_*` CTests cover
each backend. Resolution-matching checks join the analyzer suite (14 math and
identity tests). Artifacts: `entrance/1920x1080/*-world-ground-analysis.json`;
logs: `.local/shadow-1080-*`.

All three 1080p capture/comparison pairs pass and reuse the same 77 reference
patches per backend. Each has 71 patches flagged subpixel in one direction.
The fully refreshed baseline's excursion counts are 9 on DX11 and 7 on
DX12/Vulkan, down from 11/10/10 respectively in the lower-resolution world
measurements. Extended-range retained/full maximum differences are 0.9504 RGB
on DX11 and zero on DX12/Vulkan. The fixed-camera runtime-wind policy check
also passes with byte-identical ground images on every backend. The Release
capture target builds, all 14 math/identity tests pass, and six existing
lower-resolution display analyses pass. These checks validate capture matching
and reference consistency, not acceptance of all remaining temporal variation.

## Fixed terrain-footprint comparison (2026-11-07)

`Scripts/analyze_shadow_ground.py --world-radius 0.25` now compares nine
fixed points on each terrain triangle's plane in a square with a 0.25 m
half-width. It rejects patches outside their triangle, view or terrain
visibility across the route instead of shrinking them. Selection depends on
geometry, not observed shadow stability. Default 3x3 display sampling remains
available and its reports are preserved; world reports use
`*-world-ground-analysis.json`.

Each world report also evaluates the display patches at the **same accepted
centres**, with a common subset shadowed in both methods and with unshadowed
controls varying by at most 2 RGB levels in both. Projected minor-axis estimates
use singular values so screen rotation does not hide thin/grazing patches.
Reports retain all samples, flag patches below one pixel during any frame and
separately summarize common-control samples that stay at least one pixel wide.
That threshold alone does not establish adequate resolution.

At the entrance, all backends retain 77 terrain patches; 75 become narrower
than one pixel in their minor direction. DX12 has 12 common-control shadowed
samples, **none** of which stays at least one pixel wide throughout the route.
In the extended-range/full-refresh phase, display sampling reports three
>8 RGB excursions on those 12 samples and fixed-terrain sampling reports four.
The latter's largest step is 34.80 RGB versus 7.85 with display patches.
Fixing the quadrature locations therefore does not eliminate the variation.
It also changes smoothing: nine points do not integrate the entire terrain
square, and bilinear reconstruction cannot undo the original raster footprint
or aliasing. These captures cannot cleanly attribute grazing-view variation
to the renderer; obtain adequately resolved captures or direct shadow-visibility
measurements before adjusting bias/filter/projection behavior.

At the overlook, all backends retain 253 patches, with 214 flagged below one
pixel. DX12 has 31 common-control shadowed samples. Both sampling methods
report two baseline excursions and zero extended-range/full-refresh excursions
on that subset. This supports the earlier range attribution for this view,
without extending it to the entrance. All-sample counts differ from that
shadowed subset and remain available in the reports.

Extended-range retained/full maximum paired errors with terrain patches are
0.0042 RGB at the overlook and 1.0449 at the entrance on DX11, and zero on
DX12/Vulkan. Both routes pass the existing 8 RGB reference check. New optional
CTest `OakWorldAnalysis_*` and `OakEntranceWorldAnalysis_*` pairs reuse saved
captures. Thirteen math/identity tests cover sloped surface placement, physical
extent, rejected edges/degenerate patches, invalid radii, a zoom-dependent
screen-footprint example and rotation-independent projected resolution.
Logs: `.local/shadow-world-patch-*`. No authored rendering settings changed and
these are correctness diagnostics, not performance measurements.

## Ground-level walking route (2026-11-07)

The earlier matched walks were already at the elevated overlook camera
(-5, 27, -39), not the authored entrance. `ENGINE_OAK_WALK_VIEW=entrance`
now restores the original scene camera position/target before walking and
saves artifacts under `.local/tree-lod-oak-validation/entrance`. The default
route remains `overlook` and keeps its existing artifact paths. Traces carry
`walk_view`; analysis rejects mixed views, including a route that changes in
lockstep across phases. Nine analyzer math/identity tests pass.

Optional CTest pairs `OakEntranceCapture_{dx11,dx12,vulkan}` and corresponding
`OakEntranceAnalysis_*` run the same authored/extended range, retained/full,
repeat, 4/8 m fade and explicit fixed-camera wind-policy checks. The route
changes neither the authored scene nor any quality settings.

DX12 entrance analysis retains 80 visible world-space terrain samples, with
29 baseline-shadowed samples and 19 whose unshadowed controls vary by at most
2 RGB levels. These are a different sample set from the 256 overlook samples;
their counts are not a point-for-point quality comparison.

| Entrance phase | Maximum step (RGB), all samples | Samples with >8 RGB excursion, all / control-stable shadowed |
| --- | --- | --- |
| 1 m retained | 15.70 | 9 / 5 |
| 1 m full refresh | 14.57 | 9 / 5 |
| Extended range, full refresh | 14.26 | 9 / 4 |
| 4 m retained | 14.79 | 8 / 4 |
| 8 m retained | 14.79 | 8 / 4 |

Extended-range retained/full captures agree exactly. Ground-level variation
persists even with full refresh and extended caster range, including four
control-stable shadowed samples. This view therefore needs further investigation
of projection/filter/receiver behavior; the overlook's range attribution does
not explain every view. This is evidence for investigation, not a confirmed
cause or a visual pass. The current 3x3 display-space sampling patch changes
its world-space footprint as the camera moves, especially at grazing angles;
stable unshadowed terrain does not rule out shadow-edge sampling differences.
Check that measurement limit before changing the renderer. Keep the 1 m default
and the existing resolution/LODs.
Reports: `entrance/*-ground-analysis.json`; run logs: `.local/shadow-entrance-*`.

All three entrance capture/analysis pairs pass. Extended-range retained/full
maximum differences are 0.1111 RGB on DX11 and zero on DX12/Vulkan. At the
entrance, DX11 reports 11 baseline excursions and 12 in its extended/full
reference; DX12/Vulkan each report nine in both. These tests check reference
consistency and report variation; passing does not mean the moving shadows
meet a visual stability threshold. The fixed-camera runtime-wind check enables
340 oak meshes and gives identical ground images on every backend, while
139,847/112,958/112,959 visible color pixels change by >8 RGB levels on
DX11/DX12/Vulkan respectively. No authored quality defaults changed.

## Prefab shadow fade-width experiment (2026-11-07)

Animation scope correction: all 102 authored oak mesh parts currently have
`foliageWindStrength=0`. The walk named `animated` advances the scene clock
with those authored settings; it did not establish moving oak casters. New
traces record the effective number of wind-enabled oak meshes, and analysis
reports that count (unknown for older traces). In addition, `Object.hlsl`
explicitly disables shader wind in the shadow pass. Visible foliage wind and
static shadow geometry are therefore separate policies, even when wind is
enabled. Do not interpret the clock-only walk as animated tree-shadow coverage.

A separate fixed-camera diagnostic enables 0.5 wind strength on runtime oak
meshes, isolates oaks/terrain in the color pass, advances exactly one second,
and requires more than 100 pixels to change by >8 RGB levels. Ground-only
before/after captures fully refresh all shadow cascades and require a maximum
change of at most 2 RGB levels. This checks the existing static-caster policy;
it does not enable animated shadows or edit authored meshes. Captures use the
`*-wind-policy-{color,ground}-{before,after}.ppm` names.

DX12 validation enables wind on 340 runtime meshes and observes 229,748
color pixels changing by more than 8 RGB levels over one second. Its fixed-view
fully refreshed ground captures are byte-identical. The walking trace reports
zero wind-enabled oak meshes under authored settings. Evidence:
`.local/shadow-wind-policy-dx12.log`, the backend capture log and the updated
ground analysis. Candidate fade tests still need moving-view visual acceptance;
wind-deformed shadow geometry would require a separate rendering-policy change.

The same diagnostic passes on DX11 and Vulkan: 263,855 and 229,727 color
pixels respectively change by >8 RGB levels, with 340 wind-enabled oak meshes
and byte-identical ground captures on each backend. All three capture/analysis
pairs and the eight analyzer math tests pass; the Release capture target builds.
Additional evidence: `.local/shadow-wind-policy-other-backends.log` and
`.local/shadow-wind-policy-final-python.log`. These are correctness checks,
not performance measurements.

Terrain object layers now serialize `shadowFadeWidth` and expose **Prefab
shadow fade width (m)** in the inspector. Existing layers retain the 1 m
default. Streaming and regenerated prefab meshes carry the setting; negative
widths clamp to zero, widths cannot exceed the caster range, and non-finite
widths fall back to 1 m before clamping. GrassField tiles retain their existing
independent `distanceFadeWidth` behavior.

Ground diagnostics add four frozen-wind phases: 4 m retained/full refresh and
8 m retained/full refresh. Each verifies the effective generated-mesh width,
records it alongside the caster range, and follows the same camera positions
and frozen time as the baseline. The analyzer reports both temporal variation
and paired differences from the original shadows to expose earlier fading.

DX12 results at the same 256 fixed terrain samples:

| Fade width | Largest step, retained/full (RGB) | Samples with >8 RGB excursion, retained/full |
| --- | --- | --- |
| 1 m | 29.67 / 24.05 | 2 / 2 |
| 4 m | 15.77 / 17.28 | 2 / 2 |
| 8 m | 10.97 / 10.97 | 2 / 2 |

The 4/8 m retained/full maximum paired differences are 5.27/2.66 RGB.
Changing from 1 m also changes the rendered shadows by up to 29.57/31.07 RGB
respectively, including earlier disappearance. Wider fades reduce the worst
sampled step but do not eliminate the remaining excursions. **Keep the 1 m
forest default.** Candidate animated-wind/elevated-view acceptance and matched
shadow-enabled performance measurements remain open; this is not an FPS gain
or a visual acceptance result. Reports are the backend `*-ground-analysis.json`
files in `.local/tree-lod-oak-validation`; validation logs use the
`.local/shadow-fade-*` prefix.

DX11 and Vulkan capture/reference checks also pass. Their retained 1/4/8 m
maximum steps are 26.13/22.34/12.34 and 28.97/15.22/10.96 RGB respectively.
DX11's 8 m candidate increases >8 RGB excursion samples from two to three;
Vulkan retains two. Release Editor and capture-target builds pass, as do the
shared-prefab compatibility/streaming/regeneration regression and all eight
analyzer math checks. No authored scene fade, LOD, range or resolution changed.

## Partial shadow-atlas clearing and matched attribution (2026-11-07)

Partial cascade refreshes preserved the atlas but never cleared the tiles being
redrawn. Old depths survived new projections/caster motion and accumulated dark
shadow trails. Each refreshed tile now clears its own depth rectangle before
drawing; untouched tiles remain cached. DX12 uses rectangular depth clears,
Vulkan clears the depth attachment region, and DX11 uses `ClearView` only when
the driver advertises depth-only support. Unsupported contexts refresh the
whole atlas. DX11's support requirement follows the
[ClearView contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-clearview).

Recorded command packets also carry/replay rectangle clears and inherit the
active factory's capability. This preserves partial updates in normal Editor
Play and the game recording path; unsupported replay is rejected rather than
silently discarding a required clear. A packet regression covers ordering,
preserving other tiles, failed depth passes, capability mismatch and recycling.

The original attribution compares six matched paths: authored-range retained,
extended-range retained, authored-range full refresh, extended-range full
refresh, an authored-range repeat, and animated foliage. The first five run at
identical frozen wind time. The extended range is 100,000 m, applied to the
forest **layer**, and every prepared forest mesh is checked against the
requested range. Mesh-only overrides were rejected because preparation can
restore them. Full references use explicit cache invalidation, verified by the
refreshed-tile mask. The authored scene remains at 90 m with its original
visible LODs, fixed shadow proxy, four cascades and atlas resolution.

At the same 256 terrain samples, the corrected authored-range run has two
frozen-wind excursions above eight RGB levels, versus seven before tile
clearing. The shadowed subset changes from 34 to 32 samples as old depths are
removed. Extended-range retained/full reference error is **0.0242 RGB levels on
DX11 and zero on DX12/Vulkan**, across all samples and six captured positions.
All three support partial clears on this machine. The two remaining excursions
also occur with full refresh at 90 m and disappear in extended-range full
refresh, attributing them to caster-range behavior rather than stale atlas
depth. Fade smoothness, cache age near that boundary and ground-level visual
acceptance remain open; the extended range is a diagnostic, not a new default.

Release Editor, harness and packet test build. All three capture/reference
checks pass; DX12's extended capture also passes GPU validation. Eight math/
analysis checks and the packet test pass. A normal 30-frame Release DX12 Editor
Play smoke confirms packet recording is active and partial masks remain in
use. These are correctness checks and establish no FPS improvement.

Evidence: `.local/shadow-tile-clear-dx12-corrected.log`,
`.local/shadow-tile-clear-backends.log`,
`.local/shadow-tile-clear-gpu-validation.log`,
`.local/shadow-tile-clear-reference-tests.log` and
`.local/performance-logs/shadow-tile-clear-editor-smoke.jsonl`. Updated analysis
reports retain the individual phase statistics, paired errors and worst sample
positions. `OakGroundAnalysis_*` requires the extended-range retained reference
to agree within eight RGB levels; it skips with code 77 on contexts that require
full refresh. Python/numpy-dependent cases are optional at configuration time.

```powershell
ctest --test-dir build/Debug -C Release -R '^(ShadowCommandPacket|ShadowGroundAnalysisMath|OakGround(Capture|Analysis)_(dx11|dx12|vulkan))$' --output-on-failure
```

## Fixed terrain shadow measurements before tile clearing (2026-11-07)

`OakGroundCapture_dx11`, `OakGroundCapture_dx12` and
`OakGroundCapture_vulkan` run the forest harness with
`ENGINE_OAK_GROUND_CAPTURE=1`. Trees and grass remain in the shadow pass;
only terrain meshes enter the diagnostic color pass. Shadow work is submitted
and completed before rebuilding that color draw list, so shared upload buffers
are not overwritten while the GPU reads them. Matched unshadowed images use
the same camera positions and lighting, with shadow casting disabled.

`Scripts/analyze_shadow_ground.py` (numpy required) samples fixed triangle
centers from exported world-space terrain geometry. It projects them into six
images at 0.8 m intervals, rejects samples outside any view or occluded by
terrain, and bilinearly averages 3x3 pixel patches. Subtracting shadowed RGB
from its unshadowed control measures display-space shadow darkening. Reports
include all samples, the shadowed subset, control variation, receiver-band
crossings and the worst world-space outliers. No temporal-quality pass
threshold is applied.

All three captures yield 256 visible samples, 34 with measurable shadows and
three receiver-band crossings. Seven frozen-wind samples have a darkening
excursion greater than eight RGB levels. All 34 shadowed samples have
unshadowed-control excursions of at most two levels. On DX12, shadowed samples
have a 10.21-level P95 step change and 28.05 maximum; Vulkan is very similar.
DX11 has 18.31 P95 and 43.71 maximum. These differences are not a cross-backend
pixel-equivalence claim. Camera-distance caster fading, filter/bias behavior
and retained updates still require controlled attribution; temporal stability
is **not** established by these measurements.

This also exposed a DX11 capture bug: legacy `CaptureFrameRGBA` reads swap-chain
buffer zero after Present, which can return a different flip-model frame.
DX11 now supports `RequestSceneCapture` / `ReadSceneCaptureRGBA`, copying the
composed RGBA8 output before Present only when requested. A red/green/blue
sequence verifies current-frame identity and rejection of reads before a
request completes. The oak harness now uses this path; earlier DX11 walking
images must not be used as frame-accurate temporal evidence. The prior geometric
coverage assertions do not depend on those pixels.

Reproduce (Release harness):

```powershell
ctest --test-dir build/Debug -C Release -R '^OakGroundCapture_(dx11|dx12|vulkan)$' --output-on-failure
python Scripts/analyze_shadow_ground.py .local/tree-lod-oak-validation --backend dx12
python .local/tests/test_shadow_ground.py
```

Images, terrain geometry and detailed `<backend>-ground-analysis.json` reports
are in `.local/tree-lod-oak-validation/`. Five analyzer tests cover matrix
conventions, terrain occlusion/contact, bilinear patches, temporal outliers
and malformed image payloads. Capture logs: `.local/shadow-ground-dx12.log`,
`.local/shadow-ground-backends.log`, `.local/shadow-ground-dx11-final.log`.
The Release Editor and harness build (`.local/shadow-ground-final-build.log`).
The terrain-isolation path also passes DX12 GPU validation
(`.local/shadow-ground-gpu-validation.log`).

## Walking coverage regression (2026-11-07)

The real oak harness now repeats an identical 24-frame, 4.8 m translation from
its elevated diagnostic view with frozen time, then with a 60 Hz animation
clock. Each frame checks all four cascades: retained tiles must cover the
current receiver sphere with the existing safety margin, no cascade may go
more than six frames without refresh, and the run must actually retain tiles.
Fixed geometric probes cross every radial split, including the 90 m split.
The probes are not terrain samples or actual caster-fade measurements.

Every fourth frame is read back immediately, without capture warmup frames
that would conceal the retained-tile state. Twelve images per backend are
published after GPU validation, alongside `<backend>-walk.jsonl` in
`.local/tree-lod-oak-validation/`. The trace records viewport dimensions,
camera position, column-major view-projection matrices, simulation time,
refresh masks, per-cascade age/coverage clearance and fixed radial probes.
Frozen/animated camera positions match; their elapsed times are zero/0.4 s.

Release harness builds. DX12 passes (`.local/shadow-walk-dx12.log`); DX11,
Vulkan and DX12 GPU validation pass (`.local/shadow-walk-backends.log`). Across
all three saved backend traces, every one of 144 frames retains tiles, the
minimum coverage clearance is 0.728 m and maximum cascade age is two frames.
All 36 image files have the expected size and nonuniform color content; this
checks readback content, not visual shadow quality. Machine-readable results:
`.local/tree-lod-oak-validation/walk-validation.json`.

This extends structural walking coverage and prepares temporal image analysis.
Reprojected fixed terrain patches, cascade blending/filter artifacts and the
visual 90 m caster fade still require validation. The harness waits for GPU
completion between frames and does not establish runtime performance.

## Small sun changes and cascade invalidation (2026-11-07)

The real forest regression reproduced a stale-tile case on DX12: rotating the
sun by 0.05 degrees refreshed cascade mask `3` (two of four tiles), while the
other tiles retained their previous light-space orientation. The cache accepted
direction changes with cosine greater than 0.9995, and its shared cached light
direction advanced after a partial update. This allowed mismatched orientations
to persist under continuous slow sun motion.

Cache reuse now tolerates only normalization roundoff (normalized direction
vector distance at most 1e-6). Larger sun changes refresh all cascades together;
unchanged lighting retains the existing staggered camera-motion policy.
`ShadowDebugSnapshot::refreshedCascadeMask` records the tiles actually submitted
by the latest main shadow pass, and frame traces expose it as
`shadow_state_refreshed_cascade_mask`.

The forest regression checks three successive 0.05-degree changes, restoring
the original sun, and reuse under unchanged lighting. This addresses sun
invalidation; fixed-terrain temporal comparisons while walking across cascade
bands and caster-distance fades remain open. It establishes no FPS improvement.

Validation: Release Editor and forest harness build. DX11/DX12 forest tests and
DX12 GPU validation pass (`.local/shadow-sun-fixed-tests.log`); the radial
coverage/upstream-caster/texel-phase executable also passes. The initial Vulkan
run stopped before the regression because the build omitted `GrassFootBend.hlsl`
binaries: its entry points are inherited through `Foliage.hlsl`, which the
previous direct-text discovery skipped. CMake now follows local quoted includes
with cycle protection, records transitive build dependencies and watches their
contents for regeneration. All three wrapper variants build, and the isolated
Vulkan forest rerun passes (`.local/shadow-sun-vulkan-final.log`). Every backend
reports mask `15` for all three sun steps. Initial failing sun regression:
`.local/shadow-sun-before.log`; detailed missing-binary error:
`.local/shadow-sun-vulkan-retry.log`.

## DX12 command, binding and shader correctness

Investigating the recurring shadow-toggle device removal with API/GPU validation revealed five invalid paths:

- Indirect and indexed draws could execute before a primitive topology was set. Both now bind triangle-list topology, matching the ordinary draw path.
- Shadow textures started in DEPTH_WRITE, while their first depth-pass barrier expected PIXEL_SHADER_RESOURCE. Creation now matches the pass contract, including both depth/stencil planes.
- The standalone Game renderer assumed an old interop overlay would transition the swap-chain buffer to PRESENT. It now records that barrier itself after composition/readback.
- Flattened inactive vertex-instancing branches accessed past a one-record (48-byte) placeholder SRV. Explicit control-flow guards avoid reads from instance records absent in ordinary draws.
- `SetTexture(nullptr)` left root arguments untouched, yielding stale or uninitialized shadow-map bindings when shadows were disabled. Each DX12 texture factory now owns a typed null SRV in its own texture heap, retained by its contexts; empty texture bindings explicitly select it without switching heaps. A disabled-shadow filtering guard alone was insufficient, so valid null bindings remain required.

The errors are captured in `.local/forest-dx12-validation.log`, `.local/forest-dx12-gpu-validation.log` and `.local/forest-dx12-shadow-guard-validation.log`. After the corrections, the complete forest repro passes with GPU-based validation (`.local/forest-dx12-null-binding-validation.log`), three consecutive ordinary runs pass (`.local/forest-dx12-correctness-repeat.log`), and the final suite passes **all 13 tests** in 203.97 s (`.local/forest-dx12-correctness-final-tests.log`). The new `OakBackend_dx12_gpu_validation` CTest case checks both CPU and asynchronously reported GPU errors through the final idle wait. It returns skip code 77 when the optional Windows DX12 debug layer/GPU-validation interface is unavailable; it passed on this machine. Normal rendering enables no debug layer.

The Release Editor and test targets build successfully (`.local/forest-dx12-correctness-final-rebuild.log`, plus the final capture-harness rebuild). The final 1920x1080 shadow-enabled Editor route exits normally with **1,200 measured frames**, **100.26 FPS**, **9.97 ms average interval**, **12.42 ms P95**, **17.48 ms P99**, and **1,200 unique resolved GPU samples**. Streaming GPU drains and pipeline queue waits remain zero. GPU means: opaque **6.78 ms**, shadows **2.16 ms**, grass **0.52 ms**, terrain **0.13 ms**; instrumented regions total **9.60 ms**, overlapping CPU work and excluding uninstrumented work.

The average is lower than the preceding 105.62 FPS capture. The rail is fixed-frame/variable-time, and short static results also varied, so this is not a matched attribution of the difference to the correctness fixes. Sustained 100+ FPS still needs work. The opaque pass remains the largest measured GPU cost. Recording: `.local/performance-logs/forest-dx12-correctness-route.jsonl`; report: `.local/forest-dx12-correctness-route-result.md`.

## Opaque isolation and full-coverage fade fast path

Controlled DX12 Release shadows-off isolation identifies leaves as the main forest color cost. At the entrance, GPU means are **5.21 ms leaves**, **1.12 ms bark**, and **5.89 ms trees together** at 1920x1080. At 960x540, leaves still cost **3.40 ms**, bark **1.01 ms**, and trees **3.94 ms**. These visibility experiments are not additive because hiding geometry changes occlusion. Resolution changes mip/depth behavior as well as pixel count; these measurements do not directly separate vertex from fragment execution. Reports and captures: `.local/forest-opaque-baseline` and `.local/forest-full-coverage-dither`.

`ApplyGeometryFade` now returns before calculating its world-space dither hash when its interval covers [0,1]. Since the hash is in [0,1), every fragment in that interval already survives. Partial LOD/caster-range fades retain the existing hash and clips. No authored asset, material, LOD distance or shadow setting changes. The isolated 1080p tree, bark, leaf and terrain screenshots are **byte-for-byte identical** before/after. Full-scene images differ between runs, so no full-scene pixel-equivalence claim is made. Comparison artifact: `.local/forest-full-coverage-dither/image-comparison.json`.

Short static GPU measurements are mixed: full-scene entrance 7.01?6.70 ms, mid 7.59?6.98 ms, overlook 9.10?9.10 ms; some isolated cases regress or vary. Do not infer a uniform gain or attribute the whole route difference to skipping the hash.

The complete shadow-enabled DX12 Release Editor route at 1920x1080 records **1,200 recurring measured frames**, **105.62 FPS**, **9.47 ms average interval**, **11.55 ms P95**, **17.41 ms P99**. All 1,200 measured GPU samples resolve: opaque **6.46 ms**, shadows **1.98 ms**, grass **0.49 ms**, terrain **0.13 ms**, summing to **9.06 ms** of instrumented GPU regions. CPU/GPU scopes overlap; this sum excludes uninstrumented work. Streaming GPU drains and pipeline queue waits remain zero. Average 100+ FPS is achieved here; sustained 100+ FPS remains open. Fixed-frame, variable-time route coverage differs between runs and prevents an exact matched speedup claim.

Recording: `.local/performance-logs/forest-full-coverage-dither-route.jsonl`; report: `.local/forest-full-coverage-dither-route-result.md`. Release build succeeds. Regression results: **11/12 pass** (`.local/forest-full-coverage-dither-tests.log`), including all three LOD tests, DX11/Vulkan forest captures, radial coverage and lifetime/buffer checks. The existing DX12 forest diagnostic again loses the device during overlook shadows-on after the shadows-off capture. It was unresolved at that stage; the subsequent DX12 correctness fixes above pass the repro and GPU validation. The complete Editor route exits normally.

## Radial directional-shadow coverage and resolved GPU timings

Directional cascades now cover concentric receiver spheres around the camera rather than a fitted viewing frustum. The receiver shader chooses bands by radial distance rather than camera-forward depth. Camera rotation and FOV changes therefore leave light projections, receiver band selection and caster coverage unchanged. The light-space texel grid snaps all three axes, with a common depth scale and a small coverage margin. Full orthographic light bounds retain off-screen casters; camera-frustum receiver silhouette culling is bypassed for these radial regions.

Staggered updates are permitted only while every retained tile still covers its whole current radial receiver band. Walking still moves coverage, and distance-band transitions/caster-range fades still depend on position; this is not a promise of identical shadow pixels through every translation or across the range boundary. Fixed LOD1 and the existing visible tree LOD distances are retained.

New tests sample receiver coverage in every direction, test upstream casters and translation on the world texel grid, and compare a fixed color receiver image while rotating the shadow camera sideways/backwards. The final fixed-receiver comparisons (threshold >5 RGB levels) changed 1/14 pixels for side/back turns on DX11, 0/3 on DX12 and 0/2 on Vulkan, out of 518,400 pixels. All **12 Release checks pass**, including real forest captures on all three backends, sphere coverage/upstream casters/texel phase, LODs, retirement and upload/buffer regressions. Log: `.local/forest-stable-shadow-final-tests.log`. This validates rotation; translated receiver images across distance-band transitions still need temporal comparison. Lens-change expectations now require unchanged shadow coverage, since radial bands do not depend on lens/aspect.

The DX12 Editor was missing the context factory's `FinalizeFrame()` call, so timestamp queries were never resolved. It now binds its actual submission command list and resolves queries before closing it; the existing frame-slot fence protects readback. The new complete DX12 Release Editor route capture at 1920x1080 contains **1,200 measured frames / 1,200 unique resolved GPU samples**:

| Measurement | Average | P95 | P99 |
|---|---:|---:|---:|
| Frame interval (**98.44 FPS**) | 10.16 ms | 13.23 ms | 18.29 ms |
| GPU opaque | 6.95 ms | 8.62 ms | 9.46 ms |
| GPU shadow | 2.13 ms | 3.13 ms | 3.53 ms |
| GPU grass | 0.53 ms | 0.87 ms | 1.09 ms |
| GPU terrain | 0.14 ms | 0.32 ms | 0.56 ms |
| Sum of measured GPU regions | 9.75 ms | 11.99 ms | 12.97 ms |

Simulation GPU drains and pipeline queue waits remain zero. Command recording averages 2.56 ms; physics averages 0.022 ms. GPU regions overlap CPU work, and their sum excludes uninstrumented GPU work. Opaque work accounts for ~71% of measured GPU time, shadows ~22%; the next experiment should isolate tree geometry and leaf overdraw within opaque rendering. Sustained 100+ FPS remains unachieved. Route coverage is variable-time/fixed-frame, so this is not an exact speedup comparison with previous runs.

Recording: `.local/performance-logs/forest-stable-shadow-route-complete.jsonl`; report: `.local/forest-stable-shadow-route-complete-result.md`. The earlier `forest-stable-shadow-route.jsonl` was interrupted when the user accidentally closed the Editor; it is incomplete and excluded from the final benchmark. The complete rerun exited normally. At that stage the earlier intermittent DX12 diagnostic device-removal issue remained tracked; the later correctness section above records the validation errors and fixes.

## Earlier streamed render-object retirement baseline

Streamed grass tile removal previously drained the graphics device through `Scene::RemoveObject`. Runtime leaves containing only the exact built-in Mesh/Material types now detach from the scene immediately, retaining the whole object (including inactive LOD geometry and material textures) in the existing fence-tagged graphics retirement queue. Scripted/custom components, physics, authored objects and subtrees retain their synchronous destruction path. Pending resources also drain safely during Clear/shutdown before handoff.

The new 1,200-frame DX12 Release Editor moving run at 1920x1080 averages **96.32 FPS / 10.38 ms**, with **12.46 ms P95** and **17.35 ms P99**. All 1,200 frames have **zero simulation GPU waits and zero pipeline queue waits**; retained pipeline frames never exceed two, and grass tile rebuilding remains active. The prior fixed-proxy run had 178 simulation GPU waits. Sustained 100+ FPS remains open. The fixed-frame, variable-time rail does not guarantee matched route coverage, so these results do not establish an exact speedup percentage.

Recording: `.local/performance-logs/forest-stream-retirement-route.jsonl`; report: `.local/forest-stream-retirement-route-result.md`. Startup (~1.81 s) and Stop (~1.65 s) remain separate nonrecurring costs. `StreamedRenderRetirementTests` covers ownership through fence handoff, inactive LODs, registry/parent detachment, synchronous lifecycle fallbacks, Clear and shutdown.

Validation limitation: the Release build and new lifecycle check pass, and 10/11 checks pass in the regression set. `OakBackend_dx12` intermittently fails with DXGI_ERROR_DEVICE_REMOVED (0x887A0005) when toggling diagnostic shadows. Two isolated repeats passed, but the subsequent full run reproduced it; neither serialized screenshot frames nor delayed image publication eliminated the issue. The harness has deferred scene retirement disabled, so this failure does not execute the new removal path, but its cause remains unresolved. Logs: `.local/forest-stream-retirement-tests.log`, `.local/forest-stream-capture-final.log`, `.local/forest-stream-retirement-final-tests.log`. No claim of complete cross-backend validation is made for this stage.

## User capture at 20:30 and shadow stability

`performance-20261105-203051-791.jsonl` records 860 Play frames at 2560x1369: **33.25 FPS**, **30.07 ms** average interval, **56.91 ms P95**, **100.62 ms P99**. Shadow packet replay averages 11.13 ms (P95 34.49), physics 9.33 ms (P95 25.50), and recording 9.38 ms (P95 11.71). These scopes overlap. Simulation GPU drains and pipeline queue waits remain zero. GPU timings are invalid on all frames; build/backend identity is not established by this capture. This is not a matched comparison with the 1080p Release rail. Detailed report: `.local/performance-logs/performance-20261105-203051-791-analysis.md`.

The user's report of shadows changing with player position/look remains an open visual correctness issue. Source inspection confirms camera-frustum-fitted cascade centers, view-depth receiver selection, and staggered cascade updates; fixed LOD1 does not remove these dependencies. They are investigation targets, not a confirmed diagnosis of the reported artifact. Validate fixed world-space receiver patches under a fixed sun and disabled wind, separating rotation from translation, then audit caster membership (including off-screen trees), cascade transitions, filter/bias scale and retained tile coverage. Existing tests establish drawing/cut/zoom behavior, not temporal pixel stability. Full atlas freezing is not a valid fix when receivers, casters or lighting leave cached coverage.

## User capture at 19:34

`.local/performance-logs/performance-20261105-193441-179.jsonl` contains 461 Play frames at **12.81 FPS**, **78.08 ms average**, **170.96 ms P95**, **370.17 ms P99**. This is substantially worse than the controlled Release results. Game/Scene targets are 2560x1369 (about 1.69 times 1080p pixels per target); 218 frames use Game only, 242 use Scene only, and one uses both. The Game-only subset still averages 11.35 FPS, so simultaneous views alone do not explain the result.

Shadow packet replay averages 36.42 ms, view recording 22.70 ms, and physics 20.47 ms with a median four substeps per frame. These overlap and must not be added. Upload record packing remains fast (0.076 ms), and tile rebuilds have a median of zero, so the previously fixed CPU upload-read/cache problems are not the dominant recurrence. GPU timestamps are unavailable throughout this recording.

A Debug build is suspected, not confirmed: VS Code previously exposed only a Debug executable launch, and the Debug binary was rebuilt at 19:32 shortly before recording. This log has no executable/build identity. Added **Release x64 (performance)** launch and its explicit Release build task, using `build/Debug/Release/Editor.exe`. Record executable/configuration/renderer provenance in future logs before attributing differences between captures to rendering changes. Full report: `.local/performance-logs/performance-20261105-193441-179-analysis.md`.

## Current fixed-proxy tree shadows

The forest now uses **one fixed shadow LOD: LOD1**, independent of camera distance and visible LOD selection. Select **Shadow geometry > Fixed LOD 1** on the terrain object layer. The authored scene and generator both set `shadowLod=1`; other layers default to distance-based shadow LODs (`-1`). Shorter prefab chains clamp to their last level, including a single-level prefab. The setting is serialized, clamped, copied through prepared-layer publication, and propagated into generated meshes. Terrain layers can also author optional `shadowMeshes` chains for dedicated shadow-only proxies; the oak-meadow defaults remain the fixed visible LOD1 path until a measured proxy candidate is accepted.

Non-selected tree bands are excluded before shadow material preparation and submission. Selected proxies bypass GPU LOD rejection and geometry-LOD dithering. They retain leaf alpha testing but disable camera-distance texture/alpha simplification, so crossing a distance threshold cannot reshape their leaf cutouts. Fully covered proxies also skip the unnecessary distance-fade dither hash. The existing 90 m caster range retains its fade at the far boundary.

Visible tree geometry/materials, 2,048 placements, and 28/90 m visible LOD transitions are unchanged. Sun shadows retain the full 2048 atlas and four cascades. CSM tiles still refresh when coverage changes as the camera moves; a fixed proxy stabilizes the caster shape rather than freezing the atlas. Removing unnecessary atlas redraws requires reliable static-caster/light/coverage invalidation and remains future work.

| Latest DX12 Release Editor at 1920x1080 | Frames | FPS | Average interval | P95 | P99 |
|---|---:|---:|---:|---:|---:|
| Fixed proxy, entrance | 600 | 98.83 | 10.12 ms | 11.61 ms | 22.30 ms |
| Fixed proxy, moving rail | 1,200 | 90.28 | 11.08 ms | 25.87 ms | 28.86 ms |

Per refreshed cascade, tree draws dropped from 85 to 34. Stationary performance is close to 100 FPS, but **sustained 100+ FPS remains unachieved**, particularly during movement. These are recorded frame-interval measurements, not smoothed UI FPS; startup, warmup and Stop are excluded. The moving rail is fixed-frame/variable-time and samples different route portions as FPS changes, so route runs are not matched full-loop speedup comparisons. Editor GPU timestamp samples remain unavailable.

Final validation: **all 10 Release checks pass** (`.local/forest-fixed-shadow-final-tests.log`), including DX11/DX12/Vulkan forest captures, fixed-proxy selection, serialization and one/two-level fallback, camera movement/cuts/zoom, GPU LOD reuse and buffer regressions. A fresh DX12 entrance capture was inspected with visible shadows on the ground.

Recordings: `.local/performance-logs/forest-fixed-shadow-final-editor.jsonl` and `.local/performance-logs/forest-fixed-shadow-final-route.jsonl`. Reports: `.local/forest-fixed-shadow-final-editor-result.md` and `.local/forest-fixed-shadow-final-route-result.md`.

## Earlier shadow optimization stage


Full-resolution sun shadows remain enabled: 2048 atlas, four cascades, existing filtering and 90 m tree caster range. All 2,048 placements and visible tree geometry/materials remain intact; visible LOD transitions stay at 28/90 m.

| DX12 Release Editor at 1920x1080 | Frames | FPS | Average interval | P95 | P99 |
|---|---:|---:|---:|---:|---:|
| Restored shadows, before these optimizations, entrance | 600 | 83.60 | 11.96 ms | 13.55 ms | 33.70 ms |
| Optimized shadows, entrance | 600 | 94.12 | 10.62 ms | 12.53 ms | 27.33 ms |
| Optimized shadows, moving rail | 1,200 | 82.00 | 12.20 ms | 27.14 ms | 31.92 ms |

The earlier ~145 FPS results below excluded sun shadows and do not establish the shadow-enabled target. The latest measurements still do **not** meet sustained 100+ FPS. Startup, warmup and Stop are excluded from this table.

The moving rail averaged 55.44 FPS before staggered cascade updates and 82.00 FPS afterward. These are fixed-frame, variable-time runs: they cover different portions of the 18-second loop, so this is indicative rather than a matched full-route speedup. Normal moving-camera shadow draws dropped from roughly 340 to 170 per frame. The latest run still records 209 simulation GPU-wait samples out of 1,200 frames, alongside substantial tail latency. Editor GPU timestamp samples were unavailable; these FPS values come from recorded frame intervals.

Implemented shadow changes:

- Apply shadow LODs to independent prefab meshes, which previously bypassed the shadow policy. Shadow geometry switches at 8.4/54 m for this forest, with matching GPU culling and pixel-dither thresholds. The visible trees retain 28/90 m transitions.
- Use atlas-clamped texture gathers for the existing bilinear and radius-one PCF filter footprints. Skip shadow filtering when a surface has neither front-facing direct light nor subsurface scattering.
- Cull static shadow placements against transformed bounds rather than oversized spheres. Keep the original color-pass spherical envelope for wind-deformed foliage.
- Keep independent tile-cache entries for shadow and color passes. Stable entrance-frame tile rebuilds fell from about 176 to 0.11 per frame. Capture regressions reject widespread static tile rebuilds.
- During small camera movements and turns, update a dirty near cascade and rotate farther updates with at most two tiles per frame. Camera cuts, large turns, cascade-layout changes and changed projection footprints refresh all affected tiles. Preserve unmodified atlas tiles and their sampling matrices.
- Fix standalone DX12 capture timing so readback targets the color pass; reject blank diagnostic captures. Earlier shadow audit images taken between passes were blank and cannot substantiate image-equivalence claims. Earlier three-view shadow audit timings remain diagnostic only: DX12 submits shadow/color as separate renderer frames, so use full Editor captures for production FPS.

A smaller 1536 atlas and reduced filtering for distant surfaces were trialed and rejected; the final scene retains its full atlas and filter settings.

Validation: all **10 Release regression checks pass** (`.local/forest-shadow-final-tests.log`), including forest captures and LOD tests on DX11/DX12/Vulkan, static-buffer readback and upload-cache checks. Forest regressions verify bounded refresh during translation/turns, no far-cascade starvation, full refresh after a cut or zoom, visible shadow darkening, and stable tile-cache reuse. The corrected 1080p DX12 audit produced three nonblank color captures in `.local/forest-shadow-verified`; the entrance image was visually inspected.

Current recordings:

- `.local/performance-logs/forest-shadow-stagger-editor.jsonl` and `.local/forest-shadow-stagger-editor-result.md`.
- `.local/performance-logs/forest-shadow-stagger-route.jsonl` and `.local/forest-shadow-stagger-route-result.md`.
- Original restored-shadow baseline: `.local/performance-logs/forest-release-shadows-isolated.jsonl`.

## Earlier results with sun shadows disabled

DX12 Release Editor, authored forest with 2,048 trees and unchanged tree geometry/materials/LOD distances:

| Capture | Measured frames | Effective FPS | Average interval | P95 | P99 |
|---|---:|---:|---:|---:|---:|
| Full Editor Play, entrance camera | 600 | 144.70 | 6.91 ms | 8.17 ms | 13.92 ms |
| Moving camera rail with grass streaming | 1,200 | 144.54 | 6.92 ms | 15.06 ms | 17.82 ms |

These use frame intervals, not smoothed UI FPS. Startup, warmup and Stop are excluded from recurring measurements. The static capture retains the authored player controller and physics. The route is an ignored scene copy using the existing CameraTrack/SplinePath components; the test camera replaces player control and its rigid body. The route copies the authored scene; the earlier captures in this section used a disabled sun shadow setting. The moving capture samples roughly eight seconds of the continuous 18-second rail, not its entire loop.

The controlled static diagnostic uses 120 measured frames per view at 1920x1080, with shadows disabled and no readback or image creation inside measurements:

| View | Original CPU-profile FPS | After CPU mirror | After coarse groups | Final P95 |
|---|---:|---:|---:|---:|
| Entrance | 37.7 | 135.7 | 151.4 | 7.44 ms |
| Mid-forest | 35.2 | 115.7 | 139.9 | 9.19 ms |
| Elevated | 37.1 | 117.7 | 122.7 | 9.21 ms |

Separate runs can vary with system load. GPU times resolve asynchronously and overlap CPU scopes; do not add them to CPU frame times. Nested and summed worker timings also overlap. Candidate triangles count geometry before GPU visibility/LOD selection, not actual selected throughput.

## Implemented fixes

1. Prefab refresh resolves each distinct asset spelling once per check, instead of repeating path resolution for thousands of placements. The previously unmeasured asset publication, prefab preparation, commits and graphics preparation now have timers.
2. DX12 and Vulkan static vertex buffers now upload once into GPU-local memory. Previously both inherited the upload-memory fallback; DX11 already had a static-buffer implementation. Immutable geometry is fenced during initialization; editable meshes retain their writable-buffer fallback.
3. DX12 Release shaders now compile with optimization level 3. Previously shader compilation unconditionally disabled optimization in every configuration. Debug shaders retain their debugging flags.
4. Object and shadow record change detection now reads a CPU mirror instead of mapped GPU upload memory. Mirrors rotate with their owning frame/pass buffers, reset on buffer replacement, and invalidate after external writes. This avoids slow CPU reads from write-combined upload allocations. Combined record packing fell from roughly 28-33 ms to below 0.4 ms before the grouping change.
5. CPU prefab groups span 256 m instead of 64 m. The forest now generates **340 CPU mesh components**, retaining **85 shared source meshes and all 2,048 placements**. GPU culling, LOD selection and fades still operate per placement. The authored 90 m impostor transition remains in place.

The final static Editor capture averages 0.60 ms scene preparation, 0.37 ms item preparation, 1.64 ms scene rendering/recording and 0.36 ms simulation. These scopes can overlap and are not a frame-time sum.

## Validation

The Release Editor builds at `build/Debug/Release/Editor.exe` (the multi-configuration build tree is named Debug; this executable is Release).

All 10 Release CTest checks pass: UploadRecordCache, shared-prefab geometry/placement/collider editing, real oak captures/shadow comparisons on DX11/DX12/Vulkan, LOD rendering on all three backends, and DX12/Vulkan static-buffer residency/readback/editability checks. Upload-cache coverage includes initially zero records, no mapped-memory access on an unchanged record, separate buffer validity, external overwrites, buffer replacement, stride changes and concurrent disjoint records. Real oak captures verify the coarser groups still render and cast shadows.

No tree asset or material defaults were changed. Large-oak LOD1 remains 7,790-15,139 triangles; far impostors remain 28 triangles. A 55 m transition visibly flattened the elevated canopy and was rejected. `Scripts/Assets/audit_oak_lods.py` inventories all 68 oak prefabs and their materials.

## Historical stalls before streaming retirement

The earlier shadows-off moving capture has 112/1,200 frame intervals above 10 ms, and 107 simulation GPU-wait samples. Several waits exceed 10 ms while scene preparation and recording remain short. `EditorPlayPipeline::Finish` services these requests using `WaitIdle`; streamed object destruction/resource replacement is the next lifecycle path to trace. Preserve GPU references, destruction callbacks and ownership before replacing drains with retirement; do not simply suppress the waits.

Initial preparation and Stop remain separate, nonrecurring costs. The current static Release recording contains approximately 1.92 s startup and 1.65 s Stop/restore blocking work. Earlier 109.8 s Stop measurements were from Debug and are not a matched speedup comparison.

## Reproduce

### Capture identity (2026-11-07)

Performance-panel recordings now begin with a `session` record containing
`executable_path`, `build_configuration` (the actual CMake configuration, not the
build directory name) and `assertions_enabled`. Both Performance-panel frame
details and automated editor frame traces include these fields, plus
`graphics_backend`, `render_scale`, `dynamic_render_scale` and
`temporal_upscaling`. Process identity is cached; backend/settings are sampled
from the active editor renderer. These scale fields describe that renderer;
retain the existing per-view dimensions when comparing rendered workloads.

Use `--renderer dx11`, `--renderer dx12` or `--renderer vulkan` with
`Scripts/profile_tree_memory.py` to choose a backend explicitly. The option is
omitted by default, preserving selection from project settings.

Validation: Release Editor build and PerformanceRecording CTest in Debug and
Release pass. Short 1920x1080 forest captures on all three backends each contain
30 Edit frames, five Play warmup frames, 30 measured Play frames and one stopped
frame. All 198 frame identities match the requested backend and Release
executable/configuration. Captures and machine-readable validation are in
`.local/performance-provenance/`. These are metadata checks, not sustained FPS
acceptance or a reconciliation of the older 2560x1369 capture.

Build and validate:

```powershell
cmake --build build/Debug --config Release --target Editor ForestPerformanceAudit OakBackendCapture TreeLodBackendTests UploadRecordCacheTests StreamedRenderRetirementTests StableShadowCascadeTests -- /m
ctest --test-dir build/Debug -C Release -R '^(StableShadowCascade|StreamedRenderRetirement|UploadRecordCache|TreeLod_.*|DX12StaticGeometry|VulkanStaticGeometry|TerrainSharedPrefabs|OakBackend_.*)$' --output-on-failure
```

Detailed static CPU diagnostic:

```powershell
$env:ENGINE_FOREST_AUDIT_WIDTH='1920'
$env:ENGINE_FOREST_AUDIT_CPU_PROFILE='1'
# Set to include the authored sun shadows; omit for historical shadows-off runs.
$env:ENGINE_FOREST_AUDIT_SHADOWS='1'
$env:ENGINE_FOREST_AUDIT_OUTPUT='C:\repos\EngineProject\.local\forest-cpu-coarse'
& .\build\Debug\Release\ForestPerformanceAudit.exe dx12
python Scripts/summarize_forest_performance.py .local/forest-cpu-coarse --compare .local/forest-cpu-before
```

The diagnostic defaults to 36 measured frames per case; CPU-profile mode uses 120 frames for each of the three authored camera comparisons. It keeps images in memory until all measurements finish, avoiding shader-watcher invalidation caused by artifact creation between cases. No authored scene settings are edited.

Editor captures:

```powershell
python Scripts/profile_tree_memory.py .local/performance-logs/forest-release-coarse-editor.jsonl --editor build/Debug/Release/Editor.exe --frames 600 --warmup 90 --stop-frames 3
python Scripts/Assets/build_forest_benchmark_route.py
python Scripts/profile_tree_memory.py .local/performance-logs/forest-release-route-editor.jsonl --editor build/Debug/Release/Editor.exe --scene .local/forest-benchmark-route.scene --frames 1200 --warmup 90 --stop-frames 3
```

Detailed Editor reports are beside those recordings with `-analysis.md` suffixes. CPU stage data and reference images are in `.local/forest-cpu-before`, `.local/forest-cpu-after` and `.local/forest-cpu-coarse`. Prior static geometry residency measurements are in `.local/forest-performance-before` and `.local/forest-performance`; those older Debug results do not establish optimized target acceptance.


## Initial opaque workload diagnostic (2026-11-07; superseded by completion below)

The Performance panel and frame JSON now split color work into tree bark,
leaf cards (including impostors), and other opaque geometry. `gpu_opaque_ms`
retains the sum of all three for compatibility. Tree classification explicitly
covers the bundled `Models/Woodland/Oaks` sources; transformed shared prefab
meshes retain a transient classification because their baked buffer has no asset
path. It does not depend on wind being enabled. Other tree libraries require an
explicit extension of this classification. Two-sided oak parts use the same
leaf/card grouping as the existing static isolation audit.

Set `ENGINE_GPU_WORKLOAD_COUNTERS=1` before starting a process to opt into DX11
or DX12 pipeline statistics. Recorded per-category counters are VS invocations,
input-assembler primitives, and PS invocations. Results resolve asynchronously
alongside their timestamp sample; stale samples and query-capacity overflow are
excluded. Counters are disabled by default because measurement has overhead.
Vulkan timestamps support the split, but pipeline counters remain unavailable
(`gpu_workload_valid=false`), rather than reporting zero as measured work.

Use the capture wrapper for new controlled comparisons:

```powershell
python Scripts/run_forest_performance.py .local/forest-opaque-new --backend dx12 --width 1920 --counters
python Scripts/summarize_forest_performance.py .local/forest-opaque-new --compare .local/forest-opaque-baseline
```

Each run requires an empty output directory. `provenance.json` records the exact
executable SHA-256, source revision, CMake-cache identity, backend,
platform/display-driver inventory, engine/graphics environment, and hashes of
all bundled assets and engine shaders. It checks content and executable identity
again after capture. Each row records actual build configuration/assertions,
static camera position/target/route version, dimensions, shadow state, impostor
distance, profiling mode, counter mode and warmup/measurement budgets. The
summary refuses matched comparisons with absent/incomplete provenance or changed
content, CMake cache, driver/machine or environment; different per-case settings do not match.
Executable hashes can differ for a before/after build and are displayed in the
comparison. Historical captures can still be summarized descriptively.

The diagnostic now warms each case for 32 frames, exceeding the 16-frame DX11
query ring, before measuring 36 frames (120 in CPU-profile mode). At fixed 90 m
impostor distance it adds `leaves-unlit` (same alpha test, cheaper lighting),
`leaves-solid` (cutoff zero), and `leaves-lod1` (near geometry selects LOD1).
The existing full/half-resolution pair leaves the geometry budget fixed.
These overrides are transient; no authored tree/material defaults change.

Initial Release DX12 shadows-off workload validation at 960?540:

| Leaf case | GPU mean ms | VS invocations | PS invocations |
|---|---:|---:|---:|
| Authored leaves | 2.921 | 4,673,502 | 9,752,812 |
| Same geometry, 480?270 | 2.243 | 4,673,502 | 2,662,844 |
| Unlit, alpha test retained | 2.413 | 4,673,502 | 9,747,270 |
| Cutoff zero | 2.702 | 4,673,502 | 6,201,896 |
| Near LOD1 | 2.407 | 3,283,662 | 7,952,195 |

The full entrance case records about 1.383 ms bark and 3.176 ms leaf/card work.
The forced LOD control reduces executed vertex work by about 30%; the resolution
control holds vertex work constant and reduces executed pixel work by about 73%.
Cutoff zero changes coverage and depth rejection, which can reduce later pixel
invocations despite drawing solid cards. These measurements establish functioning
controls and workload attribution, not independent vertex/fragment milliseconds
or production FPS acceptance. GPU stages overlap internally, and shader/depth
changes affect scheduling and visibility. This initial set did not include a vertex-only control; the completion below adds
that direct ablation. These initial deltas are not pure vertex or fragment elapsed time.
The historical ~71% opaque/~22% shadow split is not a matched comparison to this
shadows-off static diagnostic.

Artifacts: `.local/forest-opaque-dx12-final32/` contains per-frame measurements,
provenance, summaries and post-measurement images. Earlier diagnostic attempts in
`.local/forest-opaque-dx12/` and `.local/forest-opaque-dx12-final/` exposed missing
classification and are excluded from results. The intermediate `*-validated`
captures still used a 12-frame warmup and are superseded by `*-final32` captures. Python comparison regressions cover
missing identity, changed content/driver/environment, different camera/build
settings and stale GPU sample exclusion.


Final validation: all three backends completed 936 measured frames with 936 new
valid GPU samples apiece. DX11 and DX12 each supplied 936 valid workload samples;
Vulkan supplied none and explicitly reported counters unavailable. Bark/leaf
isolation, Release identity, 32-frame warmup, unchanged inputs/executable and
recording hashes passed for each backend. Machine-readable checks are in
`.local/forest-opaque-validation.json`; backend artifacts are in
`.local/forest-opaque-{dx11,dx12,vulkan}-final32/`. Six Python regression tests pass.
The Release audit builds successfully. The running Editor locked its normal
output file, so the editor sources were built and linked as
`build/Debug/Release/EditorOpaqueValidation.exe` without stopping that process.


## Completed opaque cost isolation and route provenance (2026-11-07)

The opaque todo is complete using measured front-end/fragment-work ablations.
`bark-vertex-only` and `leaves-vertex-only` submit the same shader bytecode,
geometry, viewport, camera and LODs as their corresponding color cases, with an
empty scissor. All three backends verified identical VS invocations and IA
primitives, nonzero vertex work, and **zero PS invocations**. The summary rejects
cost pairs with changed geometry, surviving fragments or fewer than 20 completed
workload samples. It reports the directly measured vertex/front-end control and
the full-color-minus-control raster/fragment/depth-color residual in
`cost-isolation.json` and `report.md`.

These are operational costs, not independent hardware execution clocks: vertex
fetch, shading and primitive/front-end work remain in the control; rasterization,
fragment shading, depth/color work and changed pipeline overlap contribute to the
residual. No shader is replaced with a cheaper vertex program. Negative residuals
are preserved as noise/overlap, rather than clamped or presented as savings. The
DX11 480-pixel bark pair produced a -0.078 ms residual, illustrating this limit.

Release shadows-off entrance cases at 960?540, 36 samples per case:

| Backend | Category | Full color ms | Vertex/front-end control ms | Incremental raster/fragment ms |
|---|---|---:|---:|---:|
| DX11 | Bark | 1.013 | 0.746 | 0.267 |
| DX11 | Leaves/cards | 2.818 | 1.737 | 1.081 |
| DX12 | Bark | 0.956 | 0.635 | 0.321 |
| DX12 | Leaves/cards | 3.015 | 1.415 | 1.600 |
| Vulkan | Bark | 0.965 | 0.660 | 0.305 |
| Vulkan | Leaves/cards | 2.795 | 1.794 | 1.001 |

Each bark control executed 1,410,504 VS invocations and 470,168 IA primitives;
each leaf control executed 4,673,502 VS invocations and 1,557,834 IA primitives.
All controls executed zero fragment shaders. Half-resolution controls and the
existing LOD/unlit/alpha-cutoff experiments remain in the same capture. These
short diagnostics establish attribution; they do not establish production FPS
acceptance or a code speedup.

Vulkan now enables `pipelineStatisticsQuery` when supported and collects the
same VS/IA/fragment counters as DX11/DX12. Unsupported devices still report
unavailable counters; a requested diagnostic fails instead of inventing zero
work. Each statistics query wraps one physical draw, so compute-related
render-pass pauses cannot split an active query. Pools reset on the existing
fenced frame slot and resolve without blocking waits. Counter values follow the
[Vulkan pipeline statistics query rules](https://docs.vulkan.org/spec/latest/chapters/queries.html).

Moving Editor captures now record full bundled asset/shader content identity,
route document hash, explicit external-input hashes, executable and loaded DLL
hashes, CMake cache, display-driver/platform inventory, graphics environment,
layout seed, actual backend/build/assertions and all effective quality/render
settings. Each measured frame records simulation time and game-camera view and
projection matrices. Before/after checks reject changed inputs/builds, and the
comparison verifies recording hashes and actual per-frame workload identity.
Legacy or incomplete metadata cannot produce a matched comparison.

`--fixed-step` affects only automated Play profiling, preserving ordinary Editor
simulation timing. It fixes simulation work per frame while frame/FPS metrics
continue to use elapsed wall time. Two 1,080-frame captures at 1/60 s per step
cover a complete 18-second route loop after the 32-frame warmup. Their measured
camera matrices and simulation times matched exactly, with trajectory SHA-256
`7d2dd1db9db4486f5d771e6a2516974b1a5eff4441f1b45399bbe2f76b18b817`.
The same executable measured about 90.0 and 95.4 FPS in these separate runs;
this is run variability, not an optimization result.

Reproduce in fresh output paths:

```powershell
python Scripts/run_forest_performance.py .local/forest-cost-new --backend vulkan --width 960 --counters
python Scripts/Assets/build_forest_benchmark_route.py --output .local/forest-route-new.scene
Copy-Item imgui.ini .local/forest-route-layout.ini
python Scripts/profile_tree_memory.py .local/forest-route-before.jsonl --editor build/Debug/Release/Editor.exe --renderer dx12 --scene .local/forest-route-new.scene --frames 1080 --warmup 32 --stop-frames 3 --fixed-step 0.0166666667 --layout-seed .local/forest-route-layout.ini
python Scripts/profile_tree_memory.py .local/forest-route-after.jsonl --editor build/Debug/Release/Editor.exe --renderer dx12 --scene .local/forest-route-new.scene --frames 1080 --warmup 32 --stop-frames 3 --fixed-step 0.0166666667 --layout-seed .local/forest-route-layout.ini --compare .local/forest-route-before.jsonl
```

Final validation: Release `ForestPerformanceAudit` and normal `Editor` builds
pass. DX11, DX12 and Vulkan each completed 1,080 measured static frames with
1,080 valid GPU/workload samples and four verified cost pairs. Two full moving
captures each contain 1,080 measured Play frames and pass the comparison gate.
Eleven cost/route/comparison regression tests and nine existing input/layout
lifecycle tests pass. Artifacts are `.local/forest-complete-{dx11,dx12,vulkan}/`,
`.local/forest-complete-route-{a,b}.jsonl` and their sidecars; machine-readable
validation is `.local/forest-complete-validation.json`.
