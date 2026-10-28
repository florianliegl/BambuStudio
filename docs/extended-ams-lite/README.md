# Extended AMS Lite Mode

This branch implements a **dry-run planning milestone**, not executable 8-color
A1 printing. The Windows application is now built locally and has been launched
successfully to its normal Setup Wizard. The standalone planner and real G-code
processor integration checks pass. The complete import/slice/Preview interaction
still needs manual validation after first-run setup; it is not claimed as tested.

## Use the local Windows build

In PowerShell, from this repository:

```powershell
.\Run-Extended-AMS-Lite.cmd
```

The application is already open at the Setup Wizard in this session. Click
**Get Started**, select the A1 during setup, and open your existing eight-color
3MF as a project. Slice it, switch to **Preview**, enable **Extended AMS Lite Mode
(dry run)**, and choose **View swap schedule...**. Keep physical slots at 4 for
standard AMS Lite. This calculates a schedule for your own slice; the separate
demo executable always uses its fixed synthetic example.

The launcher uses a separate configuration directory at
`build/extended-ams-userdata`. The executable is
`build/extended-ams-app/src/Release/bambu-studio.exe`; its resource junction and
runtime DLLs must remain in place. No installer is needed for this local build.
Do not send an eight-color/four-slot job based on this planner: automatic pause,
spool-handling and mapping execution are still unavailable.

Base verified on 2026-10-28 using the official GitHub releases API:
[`v02.08.02.61`](https://github.com/bambulab/BambuStudio/releases/tag/v02.08.02.61),
commit `926a7192574bcb9b3a732e1ec59a46d79cb45466`, published 2026-08-21.
The API marked it neither draft nor prerelease; the release page calls it the
latest public release. Newer beta releases were deliberately excluded.
Feature branch: `codex/extended-ams-lite`.

## Current behavior

| Component | Status |
| --- | --- |
| Isolated C++17 scheduler | Implemented, compiled with MSVC, tested |
| 8+ virtual colors / 4 physical slots | Scheduling implemented; no 8-color limit |
| Actual processed selection order | Preview consumes `GCodeProcessorResult::moves`; includes tool selections and extrusion, not layer palette order |
| Initial assignments and locked slots | Implemented; invalid or impossible configurations produce errors |
| Repeated colors and switches within a layer | Supported by the planner |
| Independent replay verification | Checks availability at every filament run and all replacements |
| Preview toggle and configuration dialog | Compiled into the Windows app; full interaction validation pending |
| Color/material labels and chronological table | Compiled; consumes sliced metadata; full UI validation pending |
| Text and HTML export | Core formatter tested; GUI file dialog validation pending |
| Grouped replacements | Plan model, verifier, and presentation support groups; optimizer currently emits one demand replacement per stop |
| Executable pause generation and AMS remapping | **Unavailable; no execution backend** |
| Live swap notifications / Handy custom messages | **Unavailable; no verified event-to-swap correlation** |
| Physical A1 validation | **Not performed** |

The toggle enables the planning UI for this Preview session only. It is off by
default, does not change a printer preset or 3MF, and is not persisted. Slot
overrides are local to the schedule dialog; changing the slot count clears them.
The dialog snapshots the current result so a schedule cannot silently change
under the user. Re-slice and reopen it after changing the model or profiles.

## Planning workflow

1. Select the normal A1 printer profile. Add eight or more virtual filaments in
   Prepare, choose their profiles/colors/materials, and paint normally.
2. Slice the plate using Bambu Studio's normal virtual filament workflow.
3. In Preview, enable **Extended AMS Lite Mode (dry run)** and choose
   **View swap schedule...**.
4. Leave physical slots at 4 for standard AMS Lite. Optionally select initial
   occupants or lock slots, then calculate the schedule.
5. Review initial loading, chronological stop/layer/slot instructions, color and
   material identities, and source move/line IDs. Export text or HTML if useful.

This feature does not make an eight-filament slice printable on four slots. It
does not change the existing Send dialog, add pauses, remap tools, or provide a
Send/Resume action. Do not use the hypothetical schedule as printer instructions.
The ordinary Send workflow remains upstream behavior and is not certified by
enabling the planner. The schedule table is the current preview of planned
positions; no automatic pause markers are added to the layer slider.

## Algorithm and boundaries

`src/libslic3r/ExtendedAMSLite/Planner.{hpp,cpp}` uses Belady's offline
farthest-next-use replacement rule. Empty initial slots are filled in first-use
order. Explicit initial occupants are honored. A missing required virtual
filament replaces the unlocked occupant needed farthest in the future, with
never-used-again occupants preferred and equal choices resolved by lowest slot.
Locked occupants are never removed. Time is O(requests × slots + filament count),
including input processing, with O(requests + filament count) temporary storage.

This minimizes the number of demand replacements under the model of one required
filament per request, equal replacement cost, freely chosen automatic initial
loads, fixed locked occupants, and no physical restrictions on evicting any
unlocked spool. It does not minimize time, total handling, or grouped pauses.
In particular, the model may evict the spool currently feeding the nozzle. A
future execution backend must enforce supported unloading/parking and active
slot restrictions and may therefore require a different schedule.

Virtual filament identity, physical slot index, and firmware tool selection
remain distinct. The plan contains a physical slot for **each ordered request**,
not a single static mapping per virtual color. Every plan is replayed independently
before display. No identity, nozzle temperature, purge volume, retraction,
extrusion, or G-code tool number is rewritten.

Move `extruder_id` is a historical name: in this release
`GCodeProcessor::store_move_vertex()` fills it with `get_filament_id()`, not the
physical extruder ID. The integration relies on that exact contract. A new
`MoveVertex::layer_id` preserves the original layer number when `finalize()`
turns `layer_duration` into seconds. Layer 0 is shown as startup/unknown. G-code
line IDs are processor IDs after its normal postprocessing; they are inspection
positions, never claimed to be validated insertion points. The snapshot only
knows events recognized by the existing processor; arbitrary custom macros,
external postprocessing, and undocumented firmware conditionals can invalidate
an inferred execution sequence.

Single-extruder input is supported. The dialog rejects mixed-filament and
multi-extruder results. Material names are displayed, not certified compatible.
Equal color or equal material names never merge virtual identities. Mixed
materials, AMS suitability, temperatures, and pair-specific purge behavior need
the normal slicer checks plus future physical execution validation.

## Build and test

The core has no Boost, wxWidgets, printer, network, or cloud dependencies. From a
Visual Studio developer prompt with CMake on PATH:

```powershell
cmake -S tests/extended_ams_lite -B build/extended-ams-lite -A x64
cmake --build build/extended-ams-lite --config Release
ctest --test-dir build/extended-ams-lite -C Release --output-on-failure
& ./build/extended-ams-lite/Release/extended_ams_lite_demo.exe
& ./build/extended-ams-lite/Release/extended_ams_lite_demo.exe --html |
    Set-Content -Encoding utf8 ./build/extended-ams-lite/demo.html
```

On Linux/macOS omit `-A x64`, add `-DCMAKE_BUILD_TYPE=Release`, and find the demo
binary directly under `build/extended-ams-lite`. The synthetic eight-color demo
has 18 filament runs and seven planned stops. It is not a sliced real model.

Standalone tests include four-color/no-swap, eight-color, twelve-color,
same-layer changes, repeated use, alternation, locked and overridden slots,
grouped replacements, bad references, impossible lock configurations, corrupted
plans, retained virtual identities, report escaping, startup and selection-before-
purge trace behavior, 20,480 exhaustive optimality comparisons, and 1,000
deterministic randomized replays. Checks run in Release builds as well as Debug.

`tests/libslic3r/test_extended_ams_lite.cpp` additionally uses the **real**
`GCodeProcessor` with eight virtual filaments mapped to one extruder, tests the
retained layer IDs, and checks result copy/reset. It is built as the dedicated
`extended_ams_lite_integration` target when full dependencies are available.
**All 35 assertions pass locally.** The standalone trace test uses a fixture
with the same fields and is separate from this real integration test.

Local Release result: **21,538 checks passed**, including all exhaustive and
randomized cases above. The exported demo is available as
[`demo-schedule.html`](demo-schedule.html) and
[`demo-schedule.txt`](demo-schedule.txt). Browser security blocked opening a local
HTML file in the available preview tool, so its visual layout remains unverified;
export content and escaping were tested programmatically.

For a complete Windows application build, follow the official
[Windows compilation guide](https://github.com/bambulab/BambuStudio/wiki/Windows-Compile-Guide).
Install Visual Studio C++ tools/Windows SDK, CMake, Git and the dependency-build
prerequisites in that guide, then use the repository's build script:

```powershell
.\build_win.bat -s all-dirty -c Release -r none
```

Build the dependency tree first if using CMake directly; pass its install prefix
through `CMAKE_PREFIX_PATH`. The local VS 2022/MSVC 19.44 and CMake 3.31 build
uses the official stable dependency bundle, relocated metadata, and five locally
rebuilt libraries to match the installed compiler. See
[WINDOWS-BUILD.md](WINDOWS-BUILD.md) for provenance and commands. The application
and runtime DLL copy targets now build successfully. No custom installer or
self-contained distribution package is supplied.

After dependencies are installed, enable the normal test build and run the
integration test (adapt the dependency prefix to your actual build):

```powershell
cmake -S . -B build/extended-ams-app -A x64 `
  -DCMAKE_PREFIX_PATH=C:/path/to/dependencies/usr/local -DSLIC3R_BUILD_TESTS=ON
cmake --build build/extended-ams-app --config Release `
  --target BambuStudio_app_gui BambuStudioDllsCopy extended_ams_lite_integration
# Set PATH to the dependency bin and bin/occt directories before this test:
ctest --test-dir build/extended-ams-app -C Release -R '^extended_ams_lite_integration$' --output-on-failure
```

The upstream aggregate tests currently have unrelated OBJ/rotation/raster API
compile errors; those regressions are not claimed as passing. The dedicated
integration target avoids modifying those older tests. The build also emits
upstream warnings, including a `LIBCMT`/CRT default-library conflict warning;
it is not a warning-free or release-certified build.

Keep the official Studio install until the custom build has passed regression checks.
The added GitHub Actions workflow builds/tests the standalone planner on
Windows, Linux and macOS; it does not claim to build the GUI or validate firmware.

## Firmware investigation and next steps

See [architecture and capabilities](ARCHITECTURE.md) for exact upstream files,
confirmed source behavior, unverified firmware behavior, notification blockers,
and the staged validation procedure. No live A1 commands were sent.

For upstream releases, follow [UPSTREAM.md](UPSTREAM.md). Preserve this project's
AGPLv3 obligations and upstream copyright notices when distributing custom builds.
