# Experimental A1 all-four-spool replacement test

The user requires **one pause per visit** and confirmed the first eight-color
job will use **PLA in all eight colors**. Two-stage visits were declined. The
previous inactive-slot test passed on A1 firmware 01.08.01.00 with a 0.2 mm nozzle
and LAN-only/Developer Mode. This next attended test checks active unloading
before one pause, all four spool replacements, and normal loading afterward.

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
slots 3, 4, and finally 1. Mid-print use of this unload block has **not** yet
been physically validated. Source reuse and successful slicing do not establish
firmware recovery behavior.

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

1. Open the desktop **AMS Lite all-four test** shortcut. It opens the pre-sliced
   `.gcode.3mf` in custom Studio. An editable `.3mf` is also supplied. If reslicing,
   retain its modified printer G-code, A1 0.2 mm nozzle, 0.1 mm layers and geometry.
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

The real slicer suite passes **166 assertions in four cases**, including both
probe variants. Checks cover the actual extrusion immediately after the pause,
unload-before-pause ordering, native selections 2→3→4→1 after Resume, exactly one
pause, project save/reload/reslice and a sliced archive with all four PLA records.
The active macro is inserted at a tool-change boundary because an earlier
layer-level placement left old-color prime-tower extrusion after unloading.
Line-based checks exclude escaped macro text in the configuration header.

The upstream cooling editor logs special `T255` as an out-of-range virtual tool,
retains the command and leaves its tracked virtual identity unchanged. The
G-code processor explicitly handles `T255` as a special Bambu command. The tests
therefore do not interpret its virtual-ID state as proof of physical unloading.
There is no batch execution, automatic resume, or firmware certification yet.
