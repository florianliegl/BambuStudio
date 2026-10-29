# Printing from the custom app with Bambu Cloud

The custom app keeps slicing, native layer ordering, and the swap planner.
In Preview, **Open in Bambu Connect...** exports the current sliced plate and
opens it in Bambu Connect. Select the printer, actual plate and AMS slots there,
then start the print with Connect's normal Send control. Cloud/Handy can stay
enabled. Opening a file in Connect does not itself start a print.

This uses Bambu's documented public
[import-file interface](https://wiki.bambulab.com/en/software/third-party-integration).
The original Studio Print button still calls the proprietary plugin and can
produce the unsigned-build restriction; use the new Preview action instead.

The action requires a valid, printable current project slice. Studio's normal
sliced-3MF writer preserves G-code, pause markers, filament/nozzle metadata and
thumbnails. Each file stays under `<custom data directory>/connect_jobs` with a
unique name, allowing asynchronous import. Old files can be deleted manually
after printing. Each UTF-8 URL parameter is encoded separately. A missing Windows
handler offers the official download page; failed launch reports the saved path.

For A1/A1 mini, this action rejects jobs with more than four actually extruding
virtual identities. Connect supplies authorized submission, not physical batch
swap execution. Eight-color plans remain proposals pending that backend and
hardware validation.

## Local verification

- Standalone suite: 38,305 checks, including the official URL example, Windows
  paths, UTF-8, delimiters, invalid inputs and A1 capacity gating.
- Real slicer suite: 118 assertions in four cases, including a sliced probe
  archive with a layer-7 native pause and two PLA filament records. The later
  experimental active-unload variant and editable prime-tower regression bring
  the suite to 187 assertions; see
  [A1-ALL-FOUR-TEST.md](A1-ALL-FOUR-TEST.md).
- Custom GUI/DLL compile and link. A separate `build/extended-ams-connect`
  runtime avoids replacing the older open app's DLL. The launcher prefers it
  when present. Its resources junction points to the repository resources.
- Bambu Connect 2.5.0-beta.15 was installed from the official Wiki's Windows
  link on 2026-10-29. Installer and executable signatures validate as Shanghai
  Lunkuo Technology Co., Ltd. Installer SHA256:
  `60008eeaf2ccafc4767c3e510fa3b79326633c905f17512ee6c7137dcb35bb8f`.
- Windows registers the `bambu-connect` handler. The import URI was dispatched;
  Connect and the new custom app open/respond. No screen inspection or UI
  automation was used.

Import acceptance, account/printer selection, submission and the physical print
remain manual checks. No printer command was sent by this task. Passing URL and
archive checks does not prove a print started.

## LAN fallback

Bambu documents [Developer Mode](https://wiki.bambulab.com/en/knowledge-sharing/enable-developer-mode)
for third-party local control inside LAN-only mode, where Cloud/Handy are
unavailable. LAN-only is a printer setting and can be turned off again to return
to cloud connectivity; reconnect/bind the normal Bambu account if requested.
Switch modes with no active print. No printer mode was changed during this work.

The user prefers cloud, so Connect is the chosen submission route. Direct local
submission from this unsigned build has not been tested; Developer Mode's
existence does not verify the proprietary plugin's local submission path.
