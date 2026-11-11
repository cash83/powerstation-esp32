# Beta 2026-11-11 — stall watchdog, module-faithful serial layer, event-driven HFR

This folder is a **copy**: the files in the repository root are untouched and remain the
version that ran from 4 to 10 November 2026. Use this one if you want the changes below.

To install it, copy `powerstation.yaml` and `powerstation_module.h` from this folder over the
ones in your ESPHome config directory and flash as usual.
Then set `mac_[6]` in `powerstation_module.h` to your station's device key, as explained in the main README (leaving it zeroed works too).

## What changed

**1. Transmission like the original module** (from analysis of the original module's firmware)
- Frames are never sent back to back: at least 50 ms between two frames.
- Product-info (`0x01`) and network status (`0x03`) wait for the mainboard's acknowledgement and
  are retransmitted up to 3 times, 1 s apart.
- Network status is sent only when it changes, not on a timer.

**2. Every command the original module answers is now answered**
`0x22` synchronous report (acknowledged with `0x23`), `0x82`, `0x81`, `0x24` Wi-Fi signal,
`0x0C` GMT time, `0x1C` local time, `0x0F` free memory, `0x90` IP address, `0x2B`, `0x35`, `0x2D`.
Any other command from the mainboard is written to the log as a warning.

**3. Heartbeat like the original module**
Every 15 s once online. After 5 heartbeats in a row with no answer and no report, the whole
start-up sequence is repeated (heartbeat, product-info, network status, query), which is what the
original module does by rebooting itself.

**4. Data-stall watchdog**
If no real report (50 bytes or more) arrives for 30 s while online:
1. twice: quick network-status cycle (`0x02` "not connected", 2 s later the real status plus a query);
2. twice: simulated Wi-Fi off and on (`0x02` held for 10 s in silence, then `0x03`, 3 s later the
   real status plus a query);
3. full restart of the start-up sequence.

**5. Receiver that re-synchronises**
The checksum is verified while receiving. If header, length or checksum are wrong, only the bad
header is dropped and the next `55 AA` is searched in the bytes already received, so the good
frame that follows is not lost. Maximum payload 1017 bytes, as in the original module. A frame left
half-received for more than 0.5 s is dropped. Dropped frames are counted and logged (at most one
line every 30 s).

**6. High-frequency reporting (HFR, datapoint 19) re-armed on the mainboard's announcement**
Measured on the wire:
- the mainboard switches HFR off **30.3 s after it was switched on** (13 times out of 13);
- writing 1 again while it is already 1 does **not** extend it;
- when it expires, the mainboard announces it with a short report: datapoint 19 = 0.

The old firmware rewrote HFR every 10 s: all those writes were useless, and HFR stayed off for up
to 10 s out of every 30. Now HFR is written when the link comes up, and then only when the
mainboard announces the zero (at most once every 3 s), with a 45 s safety net in case an
announcement is lost. One write every 30 s instead of three or four, and HFR is off only for a few
tenths of a second.

**7. Log**
Short reports (under 50 bytes) are logged with their content in hex:
`id (2 bytes) · type (1) · length (2) · value`.

**8. `powerstation.yaml`**
- The "update interval" slider is applied at boot (before, only after touching it).
- Wi-Fi `output_power: 12dB`.
- `esp32 > framework > advanced > flash_chip: gd`. This matches the flash chip of the board used
  here (ESPHome suggests it in the boot log to save IRAM). **Remove these two lines if your board has
  a different flash chip.**

## What was measured, and what was not

- With the slider at 7 s: one complete set of reports per query, battery group included, no gaps,
  no warnings, no dropped frames.
- These checks are log captures of a few minutes each, on one unit, during the night of
  11 November 2026. This version has **not** yet run for days like the one in the root.
- On 10 November the mainboard got into a degraded state (it served the module port about once a
  minute). Only switching the power station off and on fixed it, with the same firmware. The cause
  is unknown; the watchdog is there to recover by itself if it happens again, but that case has
  not occurred since, so the watchdog's last steps are untested in the field.
