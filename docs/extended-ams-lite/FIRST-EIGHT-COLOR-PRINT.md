# First eight-color print (2026-10-30): extrusion lost after the first swap

Job `321fecff58bc4efb`, A1 firmware 01.08.01.00, 0.2 mm nozzle, PLA in all eight
colors, LAN-only Developer Mode, image keychain with one magnet pocket.
**Status: failed; cause not established. The workflow is not physically validated.**

## Hardware observations (reported by the user)

- The first four colors printed. At the first batch pause the replacement spool in
  slot 1 was gripped by the AMS and the new brown filament came out of the nozzle
  during the purge.
- Afterwards the toolhead moved as if printing but no filament came out, for both
  the keychain and the prime tower. The user thinks the slot 1 feeder did not move.
  No printer error was noticed. The job was then stopped.
- Every color printed after the swap (brown, light blue, blue, black) failed to
  extrude, starting right after the first purge. It was not limited to one slot.
- Resuming after replacing only slot 1, and replacing slots 2-4 afterwards, was
  deliberate. This is not the tested procedure (all four replaced before Resume).

## Studio log (what the printer reported, not a diagnosis)

- Printing ran continuously from the send (09:39) until the pause at 10:11:05
  (code `03008013`). The remaining-time readout shows no earlier pause.
- Slot 1 was removed 10:11:49 and a spool was inserted by 10:12:22.
- Studio sent **Resume at 10:13:10**, with only slot 1 replaced.
- Slots 2, 3 and 4 were removed and reinserted at 10:13:23-10:13:50,
  10:13:54-10:14:32 and 10:14:45-10:15:20, i.e. after Resume, while the printer was
  loading and purging brown.
- The tested all-four procedure replaced every spool before Resume.
- The log has no printer status after about 10:11, because the Device page was not
  open, so the time at which extrusion stopped is unknown.

## Exact G-code (retrieved from the printer's storage)

The 3mf's MD5 matches the value the printer recorded for the job; the embedded
G-code is identical to the copy in the printer's cache. Static findings:

- Four `EAMS_VISIT` blocks plus a fifth at selection 33, each with the tested
  unload block, one `M400 U1`, then the unmodified A1 change macro. No failed
  template markers.
- Visit 0 sits at the tool change before virtual filament 1 (brown). After the
  macro the nozzle returns to Z 0.1 and positive extrusion follows for virtual
  filaments 1, 6, 3 and 0.
- Tool changes before the first visit use the same macro and flush values as the
  visits.
- Nothing in the file explains missing extrusion. A static read cannot exclude a
  firmware interaction, a feed problem or a nozzle problem.

## Hypotheses, not conclusions

1. Spools in slots 2-4 were handled while the printer was loading and purging
   (the last one was finished at 10:15:20, roughly during the purge). Whether this
   disturbs the AMS Lite feed is untested. Failure of all four colors, starting
   with the first, makes a slot-specific cause less likely.
2. A partial clog, extruder slip or feed problem at the 0.2 mm nozzle that allows
   the slow purge but not normal print flow. This fits "all colors failed" best.
   It can be checked on the printer by hand (see below) without any new print.
3. A firmware interaction with the new mapping (several virtual filaments per slot,
   virtual IDs above 3). The earlier tests used four IDs and four slots.

## Checks the owner can do on the printer first (no new print)

- With the nozzle hot, extrude about 10 mm from the printer's own screen with a
  filament that is loaded normally. Note whether the stream is thin, curling,
  intermittent or absent.
- Look at the extruder gear area for ground or shaved filament, and listen for
  clicking when extruding.
- Report what is seen. A weak or absent stream points to the nozzle or extruder
  and away from this software.

## Minimal coupon result (hardware, 2026-10-30) and leading hypothesis

The user ran the one-layer, one-swap coupon (white square, one spool swap in slot 1,
green square). The first color printed, the swap pause worked, green purged, but
the green square and the prime tower received no plastic, apart from occasional
fine strands. This reproduces the failure without eight colors.

Leading hypothesis (not yet confirmed on hardware): the job relies on relative
extrusion (`M83`, set once in the start G-code). The inserted unload block issued
`G90` and nothing restored `M83` before or after `M400 U1`. If the firmware leaves
extrusion absolute after this, the small repeated E values would net almost no
filament (simulated on this slice: about 0.2 mm instead of 32 mm after the swap),
while the first long purge move still shows plastic. This matches the symptoms but
is an inference from the G-code, not a firmware observation. The earlier probe
report said only that it paused, unloaded, allowed replacement and resumed; it did
not record that the following bands printed correctly.

Change made: the swap block now emits `M83` after `G90` and again after `M400 U1`,
and `execution_gcode_error` rejects a slice that extrudes after a swap without an
explicit `M83`. Software tests pass; the hardware result is still to be observed.

## Quick swap coupon (prepared, not yet run on the printer)

`A1 quick one-swap coupon...` in `tests/libslic3r/test_extended_ams_lite.cpp` builds a
two-layer project: four colors on layer 1, one visit replacing all four spools, four
other colors on layer 2 (eight virtual identities, native unload/pause/macro path,
about 45 minutes). Set `EXTENDED_AMS_QUICK_OUTPUT` to a new folder to write the
editable and fixed projects plus the swap guide. Software checks pass; this proves
nothing about printer behavior until the coupon has been run.

`A1 minimal coupon swaps one spool inside a single layer` is the smaller variant: two
chips on one 0.1 mm layer, both identities forced onto physical slot 1 with
`plan_fixed_batches`, one visit with one replacement, the failed job's 280 purge
setting (L99.05 at the swap), about 17 minutes. `EXTENDED_AMS_MINIMAL_OUTPUT` writes
its files. The earlier four-plus-four coupon uses a 160 purge at its swap (about
half the failed job's), so it is not an exact replica in that respect.

## Proposed discriminating test (needs the owner's go-ahead)

Repeat a short job with the same mapping style, replacing **all four spools before
Resume** and touching nothing afterwards. Success points away from hypotheses 2
and 3 but does not prove them wrong. A repeat of the failure with all four
replaced first would remove hypothesis 1.
