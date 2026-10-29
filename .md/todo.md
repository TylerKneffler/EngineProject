# Engine TODO

Last updated: 2026-10-29.

Status convention:

- `[x]` Feature implemented (record failed checks separately)
- `[ ]` Implementation or follow-up work remains

## Next priorities

### Distant terrain and large view distances

- [ ] Replace the fixed resident-cell square plus two far-tile levels with a single distance-driven terrain LOD policy in `TerrainManager`: use multiple tile resolutions (for example 1/2/4/8/16 sample strides) selected by projected screen error, stream progressively coarser tiles outward, and keep full terrain objects/colliders only in the interactive radius. Reuse one request queue and upload/memory budgets; keep old geometry until replacements are ready.
  - [x] Add a quarter-resolution 128-cell far-tile level above the existing half-resolution 64-cell and detailed 32-cell tiles. Select all three levels recursively by projected error and near coverage, and retain the previous representation during transitions. The camera far plane bounds far-tile requests; tile stride is selected internally rather than exposed as a view-distance setting.
  - [x] Retire resident full cell objects outside the interactive radius only after a visible far tile covers them; keep them loaded during tile preparation.
  - [ ] Extend the hierarchy to larger maps and unify near/far request priority so added view distance does not multiply cell objects.
  - [ ] Expose the interactive radius, per-level screen-error thresholds, and maximum view distance as independent settings. Log visible coverage, tile count, draw count, per-level resident memory, and frame times for each distance band.
  - [ ] Prove gap-free coverage while moving and turning, then compare at equal camera clip distances against the current radius-based path before reducing the default resident-cell radius.
