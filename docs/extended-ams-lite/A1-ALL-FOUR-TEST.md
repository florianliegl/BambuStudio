# Experimental A1 all-four-spool replacement test

Use **A1 EXPERIMENTAL active-unload v2.3mf** through the updated desktop shortcut.
The first editable version was replaced after its custom printer macro was lost
in GUI preset reconciliation; the user's sent job contained zero pause commands.
That print did not exercise active unloading or establish firmware pause behavior.

The user requires **one pause per visit** and confirmed the first eight-color
job will use **PLA in all eight colors**. Two-stage visits were declined. The
previous inactive-slot test passed on A1 firmware 01.08.01.00 with a 0.2 mm nozzle
and LAN-only/Developer Mode. This next attended test checks active unloading
before one pause, all four spool replacements, and normal loading afterward.

## Physical result

On 2026-10-29 the user confirmed that the corrected v2 test paused during layer 4
and worked. This is the reported successful active unload, all-four replacement
in one pause and normal loading/resume test on firmware 01.08.01.00 with the
installed 0.2 mm nozzle. It is the hardware basis for the native execution hook.
The full eight-virtual-identity/multiple-visit job remains a separate acceptance
print. The instructions below describe the original probe for reproducibility.

## What is different

This experimental project uses the exact `M620 S255` through `M621 S255` unload
block from the bundled A1 end profile, including its positioning and `T255`.
It runs inside the normal filament-change macro at the first slot-1 to slot-2
change at Z=0.4 mm, after the prime tower finishes extruding with slot 1. It lifts
with the same `max_layer_z + 3.0` expression as the normal A1 change macro, waits
for unloading, then performs one native `M400 U1` pause.

The end profile's heater shutdown, motor-current changes, timelapse shutdown,
and final end-of-job parking are excluded. After Resume, the unmodified normal
A1 filament-change macro loads and purges slot 2. Further normal changes use
slots 3, 4, and finally 1. Mid-print use of this block was confirmed by the user in the v2 probe.
Source reuse and successful slicing alone do not establish firmware behavior
on other configurations.

The coupon is **12 x 8 x 1.5 mm**, with five 0.3 mm bands and 0.1 mm layers:

1. Original slot 1 color, layers 1–3.
2. Replacement slot 2 color, layers 4–6.
3. Replacement slot 3 color, layers 7–9.
4. Replacement slot 4 color, layers 10–12.
5. Replacement slot 1 color, layers 13–15.

There is one pause during layer 4. The preview colors identify four physical
slots, so the original and replacement in slot 1 share a preview identity.
This is a hardware probe using four normal virtual selections, not the finished
eight-identity execution backend.

## Run the attended test

1. Open the desktop **AMS Lite all-four test** shortcut. It opens the editable
   **v2.3mf** in the checked custom Studio build. Close older test windows to avoid
   using the earlier project. Adjust the prime tower or plate setting as needed,
   then slice. Retain its modified printer G-code, A1 0.2 mm nozzle, 0.1 mm layers
   and geometry. Confirm one pause during layer 4 after slicing.
   The `.gcode.3mf` companion is a fixed slice without model geometry: changing a
   setting invalidates its toolpaths and leaves an empty plate. Use the editable
   `.3mf` whenever changing settings.
   Studio may show its normal **Modified G-codes** notice: this test intentionally
   modifies the filament-change macro. Retain the included printer settings.
2. Keep the printer in LAN-only/Developer Mode. Confirm all four loaded spools
   and the four replacement spools really are PLA, and their printer material
   settings say PLA. Select the actual build plate in the send options.
3. Map project filaments **1→slot 1, 2→slot 2, 3→slot 3, 4→slot 4**. Start the
   small print while beside the printer. Do not send an eight-color project.
4. It should finish the original slot-1 band, move to the unload position,
   retract the active slot-1 filament into the AMS Lite, and then pause.
5. Confirm slot 1 has actually retracted out of the toolhead and all four slots
   are inactive. Only then replace all four spools with the four other PLA
   colors. Release/stage each feeder normally, as in the successful inactive-slot
   test. No spool needs to be manually fed into the nozzle.
6. If slot 1 remains engaged, withdrawal resists, the printer reports an unload
   error, or normal staging fails, stop the test and report it. Do not force the
   filament or bypass the printer's error controls; use normal Cancel if needed.
7. Resume normally. Observe loading and purge from slot 2, then the new colors
   from slots 2, 3, 4, and 1 in that order. No further swap pause is expected.

Report whether the active filament retracted before the pause, whether all four
spools could be replaced in that one pause, and whether all four replacement
colors printed after Resume. Include any exact printer error code.

## Software verification

The real slicer suite passes **187 assertions in four cases**, including both
probe variants. Checks cover the actual extrusion immediately after the pause,
unload-before-pause ordering, native selections 2→3→4→1 after Resume, exactly one
pause, project save/reload/reslice and a sliced archive with all four PLA records.
The active macro is inserted at a tool-change boundary because an earlier
layer-level placement left old-color prime-tower extrusion after unloading.
Line-based checks exclude escaped macro text in the configuration header.
An editable-project regression changes the prime-tower width after reload,
reslices, and verifies all five meshes, the single layer-4 pause and the first
post-pause extrusion from slot 2 survive. The desktop shortcut opens that
editable project rather than the geometry-free fixed slice.

The regression now also uses the GUI's `PresetBundle::load_config_model` path,
checks the reconstructed full config, edits the process preset's tower width,
and reslices. The old fixture failed that test: it changed raw full config but
omitted `change_filament_gcode` from the printer's `different_settings_to_system`
metadata. GUI import consequently restored the system macro. V2 generates this
metadata from the edited printer preset using the normal full-config exporter.

The checked build validates this named probe's real G-code before exporting a
model-free sliced job for LAN/Connect submission. It requires the scoped unload,
one pause, subsequent slot-2 selection and no extrusion while unloaded. Missing
sequences are rejected before submission; saving an editable project or backup
remains possible. The standalone suite passes 38,305 checks, including malformed,
missing and duplicate pause/unload cases and escaped-header false positives.

The upstream cooling editor logs special `T255` as an out-of-range virtual tool,
retains the command and leaves its tracked virtual identity unchanged. The
G-code processor explicitly handles `T255` as a special Bambu command. The tests
therefore do not interpret its virtual-ID state as proof of physical unloading.
There is no batch execution, automatic resume, or firmware certification yet.
