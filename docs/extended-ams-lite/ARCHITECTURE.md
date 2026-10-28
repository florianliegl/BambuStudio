# Architecture and capability investigation

Investigation targets stable `v02.08.02.61`, commit
`926a7192574bcb9b3a732e1ec59a46d79cb45466`.
Source links below use that tag so future master changes do not silently change
the evidence. This is a source audit, not an A1 firmware certification.

## Existing components

| Concern | Source / class | Finding |
| --- | --- | --- |
| Virtual filament profiles, colors, materials | `src/libslic3r/PresetBundle.hpp`, `PresetBundle::filament_presets`; `PrintConfig.hpp` | Existing filament profile list and per-filament options are reused |
| Toolpath ordering | `src/libslic3r/GCode/ToolOrdering.{hpp,cpp}` | Orders tools/layers for slicing; planner does not change it |
| G-code generation | `src/libslic3r/GCode.cpp`, `GCodeWriter.cpp` | Existing tool-change and purge/profile paths remain upstream |
| Processed virtual selection identity | `GCodeProcessor::process_filament_change`, `store_move_vertex` | Move field `extruder_id` actually receives virtual `filament_id` |
| Pause generation | `GCode.cpp` custom `PausePrint` handling | Expands configured `machine_pause_gcode`; custom message output is commented out with a firmware FIXME |
| A1 pause configuration | `resources/profiles/BBL/machine/fdm_bbl_3dp_001_common.json` | `machine_pause_gcode` is `M400 U1`, inherited by A1 profiles |
| A1 filament handling | `resources/profiles/BBL/machine/Bambu Lab A1 0.4 nozzle template change_filament_gcode.json` | Normal change macro uses firmware operations, selected virtual tool, temperatures, flushing and motion; this does not establish runtime remapping support |
| Physical AMS mapping UI | `src/slic3r/GUI/AmsMappingPopupUpdate.cpp`, `SelectMachine.cpp::get_ams_mapping_result` | Maps virtual filament identities into physical AMS mappings at send time |
| Send job | `src/slic3r/GUI/Jobs/PrintJob.cpp` | Sends `ams_mapping`, `ams_mapping2`, `ams_mapping_info` through networking job parameters |
| Preview | `src/slic3r/GUI/GUI_Preview.{hpp,cpp}`, `GCodeViewer` | Existing processed result feeds the new separate dialog |
| Device status / pause / resume | `src/slic3r/GUI/DeviceManager.{hpp,cpp}`, `MachineObject` | `command_task_pause`, `command_task_resume`, and `print_status == "PAUSE"` exist |
| Native notifications | `src/slic3r/GUI/NotificationManager.{hpp,cpp}` | Custom UI notification infrastructure exists; it does not establish which hypothetical swap an actual pause represents |
| Proprietary networking boundary | `src/slic3r/Utils/NetworkAgent.*`, `PrintJob.cpp`; repository README | Optional non-free networking component; firmware implementation is not in this repository |

Relevant pinned primary sources:

- [GCodeProcessor.cpp](https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/src/libslic3r/GCode/GCodeProcessor.cpp)
- [GCode.cpp](https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/src/libslic3r/GCode.cpp)
- [A1 inherited base profile](https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/resources/profiles/BBL/machine/fdm_bbl_3dp_001_common.json)
- [SelectMachine.cpp](https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/src/slic3r/GUI/SelectMachine.cpp)
- [DeviceManager.cpp](https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/src/slic3r/GUI/DeviceManager.cpp)
- [PrintJob.cpp](https://github.com/bambulab/BambuStudio/blob/v02.08.02.61/src/slic3r/GUI/Jobs/PrintJob.cpp)

## Confirmed vs unverified

**Confirmed in source:** a configured pause path, normal AMS change macros,
send-time AMS mapping, device-reported paused state, and client pause/resume
operations exist. The planner can read virtual selection order without changing
G-code. These facts do not prove spool replacement is executable at any proposed
position.

**Unverified:** replacing a spool in an occupied AMS Lite slot during a paused
A1 job; supported unloading and loading while paused; replacing the active spool;
changing the virtual-to-slot mapping in a running job; whether resumed selections
use the revised mapping; buffered motion ordering at within-layer pauses; RFIDs
and slot metadata; expected purge and thermal state; exact pause/resume behavior
on the user's A1 firmware version. No firmware version was provided or queried.
The official Wiki could not be retrieved during this session; no conclusion about
hardware support is inferred from those inaccessible pages.

**Unavailable in this implementation:** an execution backend, automatic pauses,
tool/mapping rewrite, automatic resume, automatic pause markers, certified material
compatibility, live swap notifications, and Handy custom messages. No undocumented
G-code or arbitrary G-code substitution has been introduced.

In particular, `M400 U1` is evidence for a normal configured pause, not evidence
for a transactional AMS slot/mapping update. Calling `command_task_resume()`
would not establish that the right filament is loaded. The planner deliberately
has no API for sending or resuming a job.

## Notifications

A future backend can use native `NotificationManager` messages once it can bind
a known job/plate/schedule and exact stop index to verified printer state. A
generic `PAUSE` status is insufficient: a manual pause, runout, error, or an
unrelated job can all require different instructions. Job identity, acknowledged
stop position, reconnect reconciliation, deduplication, and unknown-state handling
must be established first. Do not advance a schedule from elapsed time or layer
number alone: several swaps can occur within the same layer.

Desktop notifications can mirror a reliably correlated native event later.
No source-supported custom Bambu Handy message facility was established; none is
assumed. No cloud service, timer-based notifications or phone integration has
been added. Resume must remain the user's explicit supported printer action
after completing a verified load procedure.

## Changes to existing files

- `src/libslic3r/CMakeLists.txt`: register the isolated core module.
- `src/libslic3r/GCode/GCodeProcessor.hpp`: append retained layer metadata and
  sliced filament material names; copy material metadata with result assignment.
- `src/libslic3r/GCode/GCodeProcessor.cpp`: snapshot material metadata on finalize,
  preserve layer ID before replacing layer-duration placeholder, reset materials.
- `src/slic3r/CMakeLists.txt`: register the new GUI dialog.
- `src/slic3r/GUI/GUI_Preview.cpp`: add an off-by-default dry-run toggle and dialog
  launcher, refusing unsliced or stale current plates.
- `tests/CMakeLists.txt`: register the dependency-free planner tests.
- `tests/libslic3r/CMakeLists.txt`: register the real processor integration test.

New core, GUI, tests, workflow, and documentation live in separate files. There
are no changes to firmware, printer profiles, send jobs, tool ordering, purge,
G-code generation, or networking code. Retained metadata is calculated even when
the planner UI is off; it does not affect generated motion or mapping.

## Validation and safe first use

1. **Software dry run (available now):** compile and run the standalone tests
   and demo. Inspect the initial four slots and all seven demo swaps. No printer
   needs to be connected.
2. **Application validation (pending full dependencies):** build the application
   and real processor test. Slice a small eight-color model; compare every
   planned request with Preview's tool-change order and exported G-code, including
   purge and startup. Reopen after re-slicing and changing plates. Check unknown
   material metadata, duplicate assignments, locks, dark mode, DPI, large reports,
   export cancellation/failure, and scrolling. Verify standard four-color G-code
   is byte-identical with the planning UI disabled/enabled using the same slice.
3. **Capability confirmation (pending):** obtain vendor documentation or a
   supported procedure for the exact A1 firmware to unload/reload a non-active
   and active AMS Lite slot during pause, map subsequent virtual selections,
   retain proper thermal/purge state, and confirm readiness. If unavailable,
   stop execution work and retain this planner as simulation only.
4. **Controlled hardware check (not authorized by this code):** only after step
   3 establishes a supported procedure and a reviewed execution backend exists,
   use a tiny sacrificial two-layer PLA coupon with one swap. Attend the printer,
   use normal user-confirmed resume, verify the expected physical slot, loaded
   filament, purge and first resumed extrusion. Then test one within-layer swap.
   Capture firmware version, settings and observed events; do not infer safety
   from successful G-code generation or a single completed print.
5. **Expansion (pending):** repeated same-layer swaps, active-slot replacement,
   runout, cancel, disconnect/reconnect, unexpected/manual pause, missed events,
   changed RFID data, and different material profiles require separate checks.

The present safe alternatives are an ordinary supported <=4-filament print, or
splitting the design into separately printed/color-grouped parts. A layer-only
external-spool workflow is a separate hardware-dependent project; it is not a
drop-in solution for arbitrary within-layer eight-color toolpaths.
