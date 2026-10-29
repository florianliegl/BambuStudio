# Automatic magnet pockets

In **Prepare**, select one keychain object, right-click it and choose
**Magnet pockets...**. This is available in the custom Studio desktop build.

The tool is for **round disc magnets**. Choose a preset or enter the diameter and
thickness, set the number of magnets, and choose the number of closing layers.
The count control accepts 1–1000; the requested pockets must fit the actual part.
The default is **8 mm diameter x 2 mm thick**, with **0.20 mm added to the total
pocket diameter**, **0.10 mm added to its depth**, and **two closing layers**.
The diameter allowance is not an extra 0.20 mm on each side.

For a 3 mm part using 0.10 mm layers, the result matches the supplied dimensions:

| Property | Result |
| --- | --- |
| Negative cylinder size | 8.20 x 8.20 x 2.10 mm |
| Pocket bottom / top | Z 0.70 / 2.80 mm |
| Cylinder center | Z 1.75 mm |
| Insertion pause | Before layer 29 of 30, print Z 2.90 mm |
| Plastic printed after insertion | Two 0.10 mm layers |

The nozzle diameter does not determine the layer height. The tool uses Studio's
native layer-generation code, including the first layer and variable layer
profile, to find the first closing layer. A two-layer cover can therefore have a
different thickness with another process profile.

## Placement and editing

The placement preview shows the real cross-section of the selected object and
the pocket locations. Auto placement checks the printable solid across the
pocket depth, subtracts existing negative parts, reserves side/bottom walls and
keeps the pockets separated. Layouts use symmetric pairs; odd counts include a
central magnet. If an irregular outline cannot accommodate the requested layout,
the tool reports that before changing the model. Placement is a deterministic
heuristic, not an exhaustive packing solver.

Choose **Apply pockets & pause**. Each pocket is a normal negative cylinder in
the Objects list. Select an individual pocket and use Studio's **Move** tool to
adjust it. Renaming a pocket retains its metadata. Changes apply to all copies
of that model object, following the usual negative-part behavior.

To change size, spacing, layer height or cover count later, reopen the tool.
**Keep my existing pocket positions** updates size/height without rearranging
the X/Y positions. Clear it to generate a fresh symmetric layout. If changing the
count, use a fresh layout. **Remove pockets** removes this feature's cylinders
and insertion pauses. Both Apply and Remove have an ordinary Undo snapshot.

The checkbox **Keep the cavities empty: disable supports on this object** is on
by default. It changes only the selected object's support setting. A cavity
filled by internal supports would not accept the magnet. Magnet export checks
reject a support-enabled pocket object; separate support-dependent geometry into
another object when needed.

## Printing and pauses

Slice after creating or moving the pockets. Studio creates a normal native
insertion pause immediately before the first closing layer. Insert all magnets
for that height, make sure they sit below the pocket ceiling, then Resume on the
printer. The active filament stays loaded during a magnet insertion pause.

Objects sharing the same insertion height use one pause. Different heights get
separate pauses. Existing user pauses are preserved; a user pause at exactly the
same height is reused and its original message is restored if pockets are removed.
An unrelated custom command at that height must be moved first.

The generated negative-part metadata and native pause survive editable 3MF
save/reload. A `.gcode.3mf` is a fixed slice and does not contain editable model
geometry. Changing the layer profile, pocket Z, scaling or insertion tick can
make a saved pause unsafe; the custom build validates the actual pocket and roof
layers before export and asks you to update the pockets instead of silently
printing a closed cavity. Keep using the custom build for these checks.

The feature currently requires a flat object placed on the bed, printing by
layer without a raft. It rejects pockets that cannot preserve the requested
walls or fit the available thickness. Use a smaller magnet or a thicker part.

## With Extended AMS Lite

Add the magnet pockets first, then **Slice -> Extended AMS Lite -> Prepare
print**. A magnet insertion pause and a spool-swap visit are separate events.
The final AMS guide lists magnet heights; keep the spools loaded at those pauses.
The native pause notification cannot identify the pause's purpose automatically.

Changing pockets after preparing an AMS plan marks that plan as needing
preparation again. Printing is blocked until you slice and prepare the updated
plan. The command validator checks both kinds of pauses. No automatic Resume or
automatic visit advancement is performed.

## Verification

The dedicated real-slicer tests cover exact screenshot dimensions, selectable
counts and symmetry, outline holes and clearances, insufficient thickness,
updating layer profiles while retaining positions, real negative-volume cavity
slices, a pause before two roof layers, native 3MF round trips, movement and
renaming, preserving user pauses, stale pause rejection, and combined eight-color
AMS execution with an independent magnet insertion pause.

Compilation, headless slicing and a responsive app startup do not validate a
physical magnet insertion print. No screen inspection or UI automation is used.

Local Release verification on 2026-10-29: **38,325 standalone checks**, **336
assertions in seven real-slicer cases**, and **50 assertions** while exporting
the editable demo. The complete app compiled and linked. The magnet demo loaded
in a responsive native window with no error/fatal entries in the startup log.
The desktop **Extended AMS Lite** shortcut uses the separate magnet-tool runtime;
**Magnet pockets demo** opens the editable sample and its adjacent instructions.
