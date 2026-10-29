# Extended AMS Lite

For image keychains with embedded magnets, see
[Automatic magnet pockets](MAGNET-POCKETS.md): fitted cylindrical negatives,
selectable counts, symmetric placement and a pause before two closing layers.

Print eight or more virtual PLA colors on a Bambu Lab A1 with four AMS Lite slots.
The planner prioritizes fewer visits, groups spool replacements into one pause,
and carries the last loaded batch into the next layer. Virtual colors retain
their separate profiles, temperatures, purge calculations and preview identities.

The user confirmed the inactive-slot test and the active-unload/all-four-spool
single-pause test on **A1 firmware 01.08.01.00 with a 0.2 mm nozzle**, in LAN-only
Developer Mode, on 2026-10-29. The complete eight-identity job still needs its
first attended physical acceptance print. Software checks do not establish that
all firmware versions, materials, modified macros or printer models work.

## Use

1. Open **Extended AMS Lite** from the desktop, or run
   `Run-Extended-AMS-Lite.cmd`. Open an editable project, choose the standard A1
   nozzle profile, configure all virtual colors as PLA and slice by layer.
2. In Preview, choose **Extended AMS Lite: batch swaps...**. Leave layer
   optimization enabled to reuse the loaded batch on the next layer.
3. Under **First-layer order and slot settings**, optionally select a manual
   first-layer color order using Up/Down. You can override initial slots or lock
   spools that must remain loaded. These constraints can increase visits.
4. Choose **Prepare print**. Studio applies native layer sequences, slices the
   real order, builds the fixed slot plan, then regenerates and checks executable
   unload/pause/load sequences. The final guide shows the initial loading and all
   visits. Export the guide if useful. **Apply order & slice** remains available
   for ordering without enabling physical swaps.
5. Load the four initial colors exactly as shown. In **Print plate**, select the
   connected A1 in LAN-only Developer Mode. All four slots must be configured as
   PLA. Studio applies the plan's fixed slot assignments automatically; multiple
   virtual identities share each physical slot. Confirm the initial loading.
6. At a planned swap pause the active filament has unloaded. Replace all spools
   marked Insert for that visit, stage each replacement at its AMS inlet, then
   press Resume on the printer. One visit has one pause. Keep unlisted spools.
   If the active filament is still engaged or an error appears, use the printer's
   normal error controls; do not force the filament.
7. Keep the guide open and choose the visit manually. A native notification is
   shown for the matching submitted job's reported PAUSE state. It cannot identify
   an exact visit or distinguish a swap from an error/manual pause. Studio never
   advances a visit or resumes the printer automatically.

Use **Disable spool swaps** to return to an ordinary editable slice or rebuild a
plan after changing geometry, palette or ordering. Normal process changes such
as prime-tower width can be resliced when the actual color order/layers still
match. Stale schedules fail generation or pre-export validation. Geometry stays
in editable `.3mf` projects; `.gcode.3mf` companions contain fixed toolpaths and
are unsuitable for editing settings.

The desktop **AMS Lite eight-color test** opens an editable four-layer, eight-chip
coupon for the installed 0.2 mm nozzle. It requires four visits, one per layer,
with all four spools replaced at each visit. Initial White-first ordering is
included. Its adjacent HTML guide lists the exact colors for each slot/visit.
Stay beside the printer for this first complete eight-color acceptance print.

## Supported scope

The execution path currently requires a single-nozzle **A1**, printing **by
layer**, **PLA throughout**, one four-slot AMS Lite and native Studio LAN
submission with Developer Mode enabled. The original A1 change macro handles
reload and purge. Existing manual/custom layer G-code must be removed first,
except managed insertion pauses from the magnet-pocket tool.
A named experimental probe macro must not be combined with execution mode.

Imported G-code can be analyzed but cannot be prepared. Other printers, mixed
filaments, sequential object printing and additional AMS units are outside this
execution path. Batch optimization is deterministic and heuristic; it is not a
proof of the globally best palette partition or lowest purge volume. Manual
first-layer order is exact when the native slicer supports that sequence.

Prepared jobs cannot be handed to Bambu Connect or uploaded alone to storage
through this UI: those routes do not enforce this job's fixed alias mapping.
Ordinary jobs of up to four colors retain the Connect cloud option. See
[CLOUD-PRINTING.md](CLOUD-PRINTING.md). Cloud can be restored by leaving LAN-only
mode after the job has finished; Handy is unavailable while LAN-only is enabled.

## Implementation

`Planner.hpp/.cpp` implements both the original demand-replacement reference and
fixed-mapping batch planning. `Execution.hpp` provides bounded plan serialization,
native selection replay and generated-command validation. A tagged block in the
existing process-notes field stores the execution specification using ordinary
edited-preset difference metadata, preserving it in editable projects. User notes
outside that block are retained. The preparing marker blocks submission until
the two slicing passes complete.

`GCode.cpp` hooks normal start/change macro expansion. It matches actual virtual
selections and layers, inserts the tested scoped A1 unload and one native pause
at visit boundaries, then retains the original change macro. It does not reorder
motion commands or rewrite virtual tool IDs. `SelectMachine.cpp` submits repeated
physical slot aliases and persists a local guide bound to printer/job identity.
`Plater.cpp` checks the real generated command stream before sliced export/send.

The guide uses visit/layer/color identity rather than baseline G-code line
positions, which change when executable pauses are inserted. Native notifications
require a connected, info-ready selected printer, the submitted job token and the
saved matching target. They never infer a visit from elapsed time or layer alone.

## Build and verification

```powershell
cmake -S tests/extended_ams_lite -B build/extended-ams-lite -A x64
cmake --build build/extended-ams-lite --config Release
ctest --test-dir build/extended-ams-lite -C Release --output-on-failure
```

The independent `extended_ams_lite_integration` target links the real slicer and
loads the official A1 profiles. It covers native ordering, all eight virtual
identities on every layer, four executable batch pauses, active-unload sequencing,
editable-project save/import/preset reconciliation and prime-tower edits. The
standalone suite includes exhaustive optimum comparisons, randomized replay,
serialization and corrupt/missing pause guards.

Local Release results with magnet pockets: **38,325 standalone checks** and
**336 assertions in seven real-slicer cases**, before optional fixture export
assertions. The magnet demo export passed another **50 assertions**.
The earlier eight-color fixture export also passed **38 assertions**. The latest
runtime compiled, linked, loaded the editable magnet demo and opened a responsive native
window; its startup log contained no error/fatal entries. Existing Studio windows
were preserved. Desktop shortcuts point to the separate magnet-tool runtime and
settings directory under `build/extended-ams-magnets*`.

The native CoolingBuffer logs the special unload tool `T255` as an invalid virtual
tool during slicing. It retains the command; the execution validator checks the
actual emitted unload/pause/load stream. This existing diagnostic is distinct from
a printer-reported error.

For full Windows builds see [WINDOWS-BUILD.md](WINDOWS-BUILD.md). Upstream compiler
and CRT warnings remain. Unrelated aggregate OBJ/rotation/raster tests are not
claimed as passing. No screen inspection or UI automation is used: compilation,
headless integration and a process/window-responsiveness startup check do not
constitute visual QA or a complete physical eight-color print.

Base: stable `v02.08.02.61`, commit
`926a7192574bcb9b3a732e1ec59a46d79cb45466`. Feature branch:
`codex/extended-ams-lite`. Preserve the project's AGPLv3 license and notices.
See [ARCHITECTURE.md](ARCHITECTURE.md) and [UPSTREAM.md](UPSTREAM.md).
