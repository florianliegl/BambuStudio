# Extended AMS Lite

Batch planning and native layer ordering for a single-extruder project using
eight or more virtual filaments and four physical AMS Lite slots.

**Printing with automatic spool swaps remains unavailable.** The software does
not yet have a verified mid-print A1 unload, pause, replacement, reload and resume
procedure for the user's firmware. No executable swap pauses or send-time AMS
mapping are generated. A successful build or simulated replay does not validate
these firmware operations.

## Use the Windows application

From this repository in PowerShell:

```powershell
.\Run-Extended-AMS-Lite.cmd
```

The launcher prefers `build/extended-ams-connect/bambu-studio.exe` when available,
otherwise `build/extended-ams-app/src/Release/bambu-studio.exe`, with the
separate settings directory `build/extended-ams-userdata`.

1. Open an editable 3MF project, select the A1, configure and paint the virtual
   colors normally, and slice the plate.
2. In **Preview**, select **Extended AMS Lite: batch swaps...**.
3. Leave **Optimize layers** selected to prepare batches and reuse the last batch
   on the next layer. Four initial slot cards show what to load. The visit selector
   shows one entry per human visit; its four cards show every replacement together.
4. Expand **First-layer order and slot settings** for manual first-layer color
   ordering. Check **Use my first-layer order**, select a color, and use **Up** or
   **Down**. Initial slot overrides and spool locks are also available there.
5. Choose **Apply order & slice** to save the proposed order through Studio's
   normal per-layer sequence settings and regenerate the toolpaths. This is an
   undoable project edit. Reopen the planner after slicing; deselect **Optimize
   layers** to inspect the actual resulting selection order and positions.
6. **Export plan...** saves a visual HTML report or a text report.

Imported G-code can be analyzed but cannot be reordered. Mixed-filament and
multiple-extruder inputs are rejected. Sequential object printing cannot use
layer optimization. Source profiles and materials remain separate identities,
even if their displayed colors are identical. Color names are approximate labels;
the original hexadecimal colors and virtual IDs identify the actual palette.

Do not send an eight-color project to four slots based on this planner. Applying
layer order regenerates ordinary virtual-filament G-code; it does not enable the
missing physical swap execution backend. Ordinary Studio send behavior is unchanged.

For ordinary printable jobs, **Preview → Open in Bambu Connect...** submits the
custom app's sliced file through Bambu's supported cloud interface. This avoids
the unsigned executable restriction on the proprietary Print button while keeping
cloud access. It does not execute eight-color AMS Lite swaps. See
[CLOUD-PRINTING.md](CLOUD-PRINTING.md) for setup, checks and limitations.

## Batch behavior

The objective prioritizes fewer human visits, then reduces color changes where
possible. Eight colors across four full layers can use:

```text
Layer 1: batch A -> one visit -> batch B
Layer 2: batch B -> one visit -> batch A
Layer 3: batch A -> one visit -> batch B
Layer 4: batch B -> one visit -> batch A
```

This example needs **4 visits and 16 spool replacements**, rather than 16 visits
for one-spool-at-a-time handling. Keeping the last color at the start of the next
layer gives 28 machine color changes in the synthetic eight-color/four-layer case.
Actual models, sparse layers, support, overrides and locks can produce different
results. The bank/order heuristic is deterministic; it is not a proof of the
globally best palette partition or purge volume.

Every virtual identity has one fixed physical slot throughout a batch plan.
At a missing color, the planner prepares the longest upcoming run that can fit
without assigning two different identities to the same slot. It combines all
needed replacements into that visit. For a fixed slot mapping and initial load,
this greedy window policy minimizes visits; an independent exhaustive solver
tests that property. It may replace the currently active spool, which is one
reason hardware unloading must be verified before execution is enabled.

The UI snapshots actual processed moves, preserving layer boundaries even when
the color stays the same. Layer proposals retain each layer's required identities,
group them into banks, prefer the carried bank first and preserve a shared boundary
color. Manual first-layer order is exact and may increase visits. Proposal
positions never masquerade as real G-code line numbers.

Applying an order uses `PartPlate`'s existing first/other-layer sequence APIs and
normal slicing. It does not rearrange already emitted motion commands. Native
sequence records include unused filaments after the required prefix to preserve
Studio's fixed-width serialization. Identical adjacent ranges are compressed.
Studio remains responsible for support ordering, purge, temperatures and toolpaths;
the resliced trace is the source of truth.

The old Belady demand-replacement planner remains available as a core reference
and in regression tests. The application and demo now use batch planning.

## Build and checks

The standalone core needs C++17 and CMake, without wxWidgets or printer access:

```powershell
cmake -S tests/extended_ams_lite -B build/extended-ams-lite -A x64
cmake --build build/extended-ams-lite --config Release
ctest --test-dir build/extended-ams-lite -C Release --output-on-failure
.\build\extended-ams-lite\Release\extended_ams_lite_demo.exe
```

On Linux/macOS omit `-A x64`, add `-DCMAKE_BUILD_TYPE=Release` and use the binaries
directly in the build directory.

The demo is a synthetic four-layer/eight-color model of selections. It now emits
batch proposals rather than the original seven-stop Belady example. Example
exports: [HTML](demo-schedule.html), [text](demo-schedule.txt).

Local standalone Release verification: **38,296 checks**, including 20,480 cache
optimality comparisons, 4,096 fixed-mapping batch optimum comparisons and 1,300
randomized replays. Tests include exact manual order, sparse layers, locks,
overrides, stable slot identity, repeatable reslicing, retained layer palettes, corrupt plans, unknown
colors and escaped exports.

The dedicated `extended_ams_lite_integration` target links the actual slicer.
It tests real processed virtual selections, retained layer/material/color metadata,
native sequence serialization/callbacks, and a real eight-color coupon slice and
G-code export. All eight colors are checked on every exported layer, with four
grouped visits and sixteen replacements. A fourth case loads the real A1 0.2 mm
presets, validates and slices an attended inactive-slot replacement probe, saves
and reloads its native project, and confirms its pause survives reslicing.
**118 assertions across 4 integration test cases pass locally.**

```powershell
# After configuring the full application with dependencies and SLIC3R_BUILD_TESTS=ON:
cmake --build build/extended-ams-app --config Release --target extended_ams_lite_integration
# Include dependency bin and bin/occt in PATH before running:
ctest --test-dir build/extended-ams-app -C Release -R '^extended_ams_lite_integration$' --output-on-failure
```

For the full application, see [WINDOWS-BUILD.md](WINDOWS-BUILD.md) and the official
[Windows compilation guide](https://github.com/bambulab/BambuStudio/wiki/Windows-Compile-Guide).
The local build uses VS 2022/MSVC 19.44, the official dependency bundle and five
locally rebuilt libraries. Upstream warnings, including a CRT linker warning,
remain. The unrelated aggregate OBJ/rotation/raster tests are not claimed as passing.
The standalone GitHub workflow covers Windows, Linux and macOS.

No screen automation or screen inspection is used for this update. Compilation,
unit/integration checks and a process/window-responsiveness startup check do not
constitute visual QA or physical printer validation.

## Execution boundary and upstream

The user's workflow requires the active filament to be pulled back as during a
normal AMS change before replacing its spool. The A1 profile contains normal
tool-change macros and an end-of-print `T255` unload path. An end-of-print macro
does not establish safe mid-print reuse, reload, thermal state or resume behavior.
The user reported firmware **01.08.01.00** and an installed **0.2 mm nozzle**.
The [small attended test](A1-01.08.01.00-TEST.md) is prepared and software-validated;
its physical spool replacement and resume still need observation.
See [ARCHITECTURE.md](ARCHITECTURE.md) for source evidence and remaining boundaries.

Base: stable [`v02.08.02.61`](https://github.com/bambulab/BambuStudio/releases/tag/v02.08.02.61),
commit `926a7192574bcb9b3a732e1ec59a46d79cb45466`, verified on 2026-10-28.
Feature branch: `codex/extended-ams-lite`. [UPSTREAM.md](UPSTREAM.md) describes
updates. Preserve the project's AGPLv3 license and upstream copyright notices.
