# Humanoid animation mapping

Use the Animation Editor's **Humanoid animation mapping** panel to give a rig
reusable body roles without translating its bone names. Roles include the root,
pelvis, spine, neck, head, shoulders, arms, hands, legs, feet, and toes.

1. Open the character prefab in the Animation Editor. If it has several rigs,
   select the body rig using the rig index and joint count in the selector.
2. Expand **Humanoid animation mapping**. Choose the rest-pose Up and Forward
   axes and whether the character's left side is positive on the remaining axis.
3. Click **Detect unassigned body parts**. Detection uses the saved neutral
   rest pose, joint positions, proportions, symmetry, and ancestor chains. Bone
   names and online translation are not required. An upright A or T pose works
   best. Detection preserves existing assignments. Assigning Root first limits
   detection to its descendants.
4. Review the suggestions. **Select assigned bone** reveals a role's joint.
   To correct a role, select the intended bone in the viewport/hierarchy and
   click **Assign selected bone**, or choose its full hierarchy path from the
   dropdown. Filter the dropdowns with the search field. Duplicate assignments,
   missing major roles, and disconnected limb chains produce notices.
5. Click **Save humanoid mapping**. Repeat for the source avatar when exporting
   animations to a different avatar. Then export/re-export the source animation
   and attach the `.anim` or `.animpack` to the target as usual.

Assignments are saved in `.rig` assets. Newly saved/exported `.anim` and
`.animpack` assets carry their source roles and rest skeleton; splitting a pack
preserves these fields. Both source and target profiles allow animation channels
to match by body role even when their names, paths, and model-node indices differ.
Existing assets without profiles retain the existing path/name matching, with
conservative geometric fallback for major limbs when sufficient rest data exists.
For models with multiple armatures, application prefers a rig with saved body
assignments, then the rig with the largest joint palette.

Suggestions need review, especially on rigs with control and deformation bones
at the same positions, unusual proportions, or multiple plausible limb chains.
An ambiguous branch may remain unassigned. Saving a suggestion accepts it as an
explicit assignment; later applications reuse it. Clear assignments before
redetecting with different axes. Hair, tails, props, fingers, and facial channels
still use their existing paths/names or format-specific mappings.

This maps humanoid joints and converts compatible rotation channels using the
existing rest-pose conversion. It does not provide automatic foot planting,
cross-avatar root-motion scaling, or a complete IK retargeting solver. Raw VMD
files do not contain a source rest skeleton: native PMX identity remains the
preferred match, and saved PMX body roles can resolve standard MMD body names.
VMD controller/IK and facial channels still require their format-specific data;
the profile cannot invent missing source information.

For a native PMX character such as Mococo, select its scene instance (or one of
its bones) and drop the `.vmd` onto Properties. Dropping a source-only VMD
`.animpack` works there too: its embedded body, facial, and property tracks are
bound against the character prefab's neutral skeleton. This creates or updates
the instance's AnimationManager, selects the imported body clip, and layers the
facial motion. Save the scene to retain playback; this operation does not change
the source character prefab. Attaching motion leaves editor preview stopped.
Use Scene Play, the Animation Editor's Play control, or the component's explicit
**Play Editor Preview** button to animate; **Stop Editor Preview** stops that
component's edit-mode preview. The component's Playing field controls runtime
playback and does not start an edit-mode preview by itself.
Camera and light tracks belong on their scene
objects. Direct VMD drops onto FBX characters still require the MMD retarget
import workflow.

The optional `HumanoidRigTests` target checks anonymous A/T poses, axes,
co-located parent helpers, ambiguous branches, invalid hierarchies, profile
serialization, selection of the body armature, and role-based `.anim`/`.animpack`
application across unrelated bone names and indices.
