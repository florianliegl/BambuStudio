# Architecture and capability investigation

Investigation targets stable `v02.08.02.61`, commit
`926a7192574bcb9b3a732e1ec59a46d79cb45466`.
Source links below use that tag so future master changes do not silently change
the evidence. This is a source audit, not an A1 firmware certification.

## Existing components

| Concern | Source / class | Finding |
| --- | --- | --- |
| Virtual filament profiles, colors, materials | `src/libslic3r/PresetBundle.hpp`, `PresetBundle::filament_presets`; `PrintConfig.hpp` | Existing filament profile list and per-filament options are reused |
| Toolpath ordering | `src/libslic3r/GCode/ToolOrdering.{hpp,cpp}`, `PartPlate` | Batch proposals use native sequence settings and normal reslicing |
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

## Native execution

The user reported successful inactive-slot replacement, followed by successful
active unload/all-four replacement/resume in one pause during layer 4 of the v2
probe on A1 firmware 01.08.01.00, 0.2 mm nozzle, PLA, LAN-only Developer Mode.
See [A1-ALL-FOUR-TEST.md](A1-ALL-FOUR-TEST.md) for the exact tested sequence.
A complete eight-identity, multiple-visit job is the next physical acceptance
boundary; it is not established by those two tests or software compilation. The
first attempt lost extrusion after its first swap; see
[FIRST-EIGHT-COLOR-PRINT.md](FIRST-EIGHT-COLOR-PRINT.md).

The backend uses a fixed virtual-to-slot mapping for the complete job. It does
not remap the running printer. The user's original brief describes using repeated
AMS slot assignments manually. Native submission now supplies those aliases from
the verified plan, using the existing mapping v0/v1/info payloads. Virtual tool
IDs, filament-specific temperatures and the normal A1 purge path remain intact.

Preparation first applies native layer orders and slices. It then replans from
actual processed selections with the same fixed assignments and regenerates the
G-code with execution enabled. A scoped `M620 S255` / `T255` / `M621 S255` block
from the standard A1 end profile unloads the active filament at the normal
filament-change boundary, followed by one `M400 U1`. The original change macro
loads and purges after the user resumes. End-of-job heater/motor shutdown is
excluded. A layer-level pause would be incorrect because prime-tower extrusion
can still use the old color before the actual tool-change boundary.

ExecutionSession checks actual initial/next virtual IDs, layer positions and
loading states. It rejects missing/unconsumed visits or stale selections. Audit
markers identify job/selections/visits; pre-export validation also inspects real
Tn, native unload scope and M400 U1 commands, and rejects positive extrusion while
unloaded. Header comments containing escaped macro text do not satisfy it.
Process-note changes already invalidate native G-code export. Existing model
custom G-code and the old experimental probe macro are rejected in execution mode.

The bounded tagged specification is persisted through edited print-preset
metadata, retaining user notes outside the block. A preparation marker blocks
submission before both passes complete. The disable control removes these tags
and reslices. Per-plate native sequence settings keep ordering undoable/editable;
the current implementation prepares one plate at a time. Multi-plate exports
must each match the active plan and otherwise fail validation.

Native LAN submission requires a connected info-ready N2S/A1 with one AMS Lite
and PLA configured in every slot. It confirms initial physical loading, reapplies
the fixed aliases at send time and prevents manual alias changes in the dialog.
Connect and storage-only routes are blocked for prepared jobs because they do not
enforce these aliases. Ordinary printing remains available after disabling swaps.
Developer Mode is a printer prerequisite; Studio does not silently change it.

## Notifications and limits

A saved local plan and target printer ID are bound to the submitted `EAMS-<id>`
job name, including when MakerWorld metadata provides a different title. The
Preview timer observes native status rather than elapsed print time. A connected,
info-ready selected printer reporting that matching job in PAUSE gets a native
notification with a swap-guide link. Missing files, wrong printer/job, invalid
metadata or unknown connection state do not produce swap instructions.

PAUSE alone cannot identify an exact visit or exclude a manual/error pause.
The user selects the visit in the guide and checks printer status. There is no
automatic visit advancement, automatic resume or unsupported Handy custom-message
integration. Guides remain available after app restart/reconnect via local
metadata. Cloud/Handy are unavailable while the printer stays in LAN-only mode.

## Verification

`PauseTimeline.hpp` reads the actual EAMS visit / magnet audit markers in command
order and cross-checks the native processor's pause list for layers and remaining
time. The sender stores a bounded `.timeline` next to `.plan` / `.target` and a
new `.run` nonce also included in the submitted job name. The private
`ExtendedAMSPauseOverview` component on the Device printing page verifies printer,
job and submission before attaching local `.progress`. Re-sends cannot reuse an
older run's acknowledgements. The component caches the parsed plan and only checks
the small run token during status updates.

Acknowledgement requires a connected, info-ready, matching paused job without a
reported print error and at the planned layer. It increments exactly one local
event and latches until a matching RUNNING status is observed. Layer/time/generic
PAUSE never increments progress. Multiple visits within one layer remain separate;
magnet events retain their exact interleaving. A progress-adjustment dialog lets
the user select the next uncompleted stop after reconnect or missed confirmation.
Modal acceptance rechecks current identity/status/progress after the nested event
loop; it cannot acknowledge a different selected printer or run. The new UI has
no printer-command calls. Native Resume remains separate. Older sidecars fall
back to spool visits only, with magnet heights listed separately and no fabricated
magnet layers or countdowns.

Standalone tests exercise exhaustive optimum comparisons and randomized plan
replay, bounded serialization, current-order replay and real-command audit
rejection of missing pauses/extrusion while unloaded. Dedicated integration tests
load real A1 0.2 profiles and exercise native ordering, all eight colors on every
layer, four generated visits, editable project round trips through GUI preset
reconciliation, and normal prime-tower edits followed by reslicing. Neither suite
commands a printer. No screen inspection or UI automation is used.

Changes are isolated in ExtendedAMSLite core/UI files plus narrow hooks in
GCode.cpp, Preview, Plater, SelectMachine and PrintJob. Printer profiles, purge
algorithms and firmware remain upstream. Future profile changes must retain or
revalidate the tested unload sequence; successful compilation is insufficient.
