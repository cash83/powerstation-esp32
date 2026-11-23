# Powerstation ESP32 — fully local, no cloud, no freeze

Replace the Quectel/Acceleronix Wi‑Fi module inside a **Landecia / Wonderfree S2400** power station with an **ESP32 running [ESPHome](https://esphome.io)**. The ESP32 takes the module's place on the serial bus and speaks the **Tuya‑MCU protocol directly to the mainboard**, so all telemetry and control stay **100 % local**:

- no vendor cloud, no account, no app login
- no BLE provisioning, no TLS‑PSK, no device "bind"
- and the infamous **~40‑minute freeze is gone** — the ESP closes the mainboard's periodic "network check" pause within a few seconds at most, where the stock module only got out of it by reconnecting (see [The ~40‑minute freeze, explained](#the-40minute-freeze-explained)).

Current version (23 November 2026): 8 of these pauses in a row closed on their own in 0.4–3.1 s, no "data stopped" alarm all night, telemetry every few seconds.

> This is a **hardware mod** (you open the unit and swap a module). It is fully reversible — unplug the ESP, plug the original module back in.

---

> **The files in the root are the current version (2026‑11‑23)** — see [What's new](#whats-new-20261123) · [italiano](LEGGIMI.md). The earlier beta is kept for reference in [`beta-2026-11-11/`](beta-2026-11-11/); the version of 4 November is in the git history.

## The power station

| | |
|---|---|
| **Model** | Landecia / Wonderfree **S2400** — also sold as **Newsmy / Necespow S2400**; same Quectel + Acceleronix platform as several **Oukitel** S2400‑class units |
| **Capacity / inverter** | 2048 Wh LiFePO₄, **13S** pack · 2400 W inverter |
| **Cloud platform** | Quectel + Acceleronix · product key `p11tpn` |
| **Wi‑Fi module replaced** | **FC41D** (Beken BK7231N) |
| **Tested on** | this one unit only — treat everything here as a starting point, not a general rule |

The datapoint IDs on the serial bus are the **same** as the vendor's LAN/cloud protocol (TTLV over `0xAAAA` frames), so the semantics match the excellent reverse‑engineering in the `ha-oukitel` project (see [Credits](#credits)).

---

## Why bother — the freeze

On the stock module the station's data stops after ~40 minutes and only comes back when the module reconnects. Chasing it through fake‑cloud brokers, BLE provisioning and TLS‑PSK is a deep rabbit hole.

### The ~40‑minute freeze, explained

Capturing the serial wire (16 pauses out of 16) showed that **the freeze starts in the mainboard, not in the cloud**:

1. Every ~606 s the mainboard asks the module for the Wi‑Fi signal (`0x24`). **Every 4th time** (≈ 2426 s, the "40 minutes") it enters a *network check*: it asks `0x2B` ("how is the network?") and `0x35` **every second**, and stops sending reports.
2. Answering those questions correctly is **not enough**: the ESP answered `0x2B = 04` ("connected to the cloud") for 8 minutes in a row and the pause did not end.
3. The pause ends **only when the module announces the network status on its own**, `0x03 = 04`:
   ```
   ESP  -> 55 AA 00 03 00 01 04 07   (unsolicited: "I am connected to the cloud")
   MCU  -> 55 AA 03 03 00 00 05      (ack) — reports resume ~110 ms later
   ```
4. The stock FC41D sends `0x03` **only when its connection state changes** (seen in its firmware dump). So it stays silent, the pause never ends, and the data comes back only when the module drops and reconnects — that is the freeze.

**The cure** (in `powerstation_module.h`, `case 0x2B`): when a second `0x2B` arrives within 3 s, the ESP immediately announces `0x03 = 04`, and nothing else. If 5 s after the announcement the mainboard is still asking, it falls back to a simulated Wi‑Fi off/on (at most once every 2 minutes). The log shows `PAUSA DEI 40 MINUTI …` and then `PAUSA CHIUSA dopo X ms (bastato l'annuncio 0x03=04)`.

---

## How it works

The custom component (`powerstation_module.h`, included via `esphome: includes:`) is a **response‑gated responder + TTLV parser**:

1. **Handshake.** Send the Tuya heartbeat in INIT until the MCU replies `55AA 03 00 00 01 01 04`; answer product‑info `0x01`; **send net‑status `0x03 = 0x04` ("connected to cloud")** — that byte turns on the Wi‑Fi icon and **unlocks the `0x07` telemetry reports**. Also answer the Quectel‑specific `0x2B` / `0x35` exchange, time `0x1C` / `0x0C` (taken from Home Assistant), MAC `0x2D`, IP `0x90`, Wi‑Fi signal `0x24`, free memory `0x0F`, synchronous report `0x22` (acked with `0x23`).
2. **Two report families.** A **fast set** (PV / grid / AC / DC) arrives only after an `0x08` "read‑all" query; a **slow set** (battery / cells / temps) is pushed by the MCU on its own every ~2–3 min. **Hammering `0x08` suppresses the slow set**, so instead we turn on **high‑frequency reporting** (`DP 19 = 1`): the MCU then pushes *everything* every few seconds by itself. It switches itself off after ~30 s and announces it with a short report `DP 19 = 0`; the firmware re‑arms it **only when that announcement arrives** (at most once every 5 s).
3. **Parser.** TTLV: `id(2B) | type(1) | len(2B) | value`; `0x11` = double (8 B, no scale), `0x02/0x04` = int, `0x12` = struct → recurse. Sensor key = top‑level id, or `(group<<8)|sub` inside a struct/array.
4. **Writes.** Switches / numbers / selects write back with `cmd 0x06`, same `id|type|len|value` encoding.
5. **Onboard status LED.** The ESP32‑S3 RGB LED is exposed as a normal light (`Spia`) you can color/dim; it flips to **red** automatically if the serial link to the mainboard ever goes mute.

---

## What's new (2026‑11‑23)

Compared to the 4 November version (the 11 November beta already had points 2, 3 and part of 6):

1. **The ~40‑minute freeze is closed** by the unsolicited `0x03 = 04` announcement — see [above](#the-40minute-freeze-explained).
2. **Serial layer like the original module:** at least 50 ms between frames; product‑info `0x01` and network status `0x03` wait for the mainboard's ack and are retransmitted up to 3 times; network status sent only when it changes; heartbeat every 15 s, full restart of the handshake after 5 missed heartbeats.
3. **Every command the original module answers is answered** (`0x22`, `0x82`, `0x81`, `0x24`, `0x0C`, `0x1C`, `0x0F`, `0x90`, `0x2B`, `0x35`, `0x2D`); anything else is logged as a warning.
4. **Never talk over the mainboard:** frames the ESP sends on its own initiative wait for 300 ms of silence on the RX line (at most 3 s). Answers to the mainboard's questions go out immediately. A summary line every 10 minutes says how many frames had to wait.
5. **Robust receiver:** checksum checked while receiving, resync on the next `55 AA`, up to 1017 data bytes per frame, a half‑received frame is kept for up to 3 s (during a heavy block the mainboard sent frames in slow bursts). UART RX buffer raised to 8192 bytes.
6. **Data‑stall watchdog:** counts only reports that carry the battery group (DP 3). If they are missing for 25 s (or query interval + 15 s), it waits for a quiet moment on the line, then simulates Wi‑Fi off (`0x02`, 3 s) → router (`0x03`, 3 s) → cloud (`0x04`) + query. After 3 attempts without data: full restart of the handshake.
7. **New controls in Home Assistant:**
   - **Chiedi i dati** (switch, default on): off = the ESP never sends the `0x08` query and keeps only the reports the mainboard pushes on its own, like the original module (the pause cure stays active).
   - **Ricollega Wi‑Fi** (button): the simulated Wi‑Fi off/on, by hand.
   - **Registro seriale completo** (switch, default off): logs every frame in both directions in hex. ⚠ The log then contains your device key (in the `0x2D` reply): remove it before sharing.
   - **Dati fermi** (binary sensor): on when no real report has arrived for over a minute.
8. **`powerstation.yaml`:** time from Home Assistant instead of SNTP; Wi‑Fi `output_power: 15dB`, `power_save_mode: light`; logger at WARN and no USB logging (fewer CPU cycles, cooler chip); "Frequenza invio dati" and "Intervallo aggiornamento" are restored at boot (minimum interval 5 s); `flash_chip: gd` under `esp32 > framework > advanced` matches the board used here — **remove it if your flash chip is different**.

**What was measured:** one unit, 22–23 November 2026. Every 40‑minute pause from the evening of the 22nd to the afternoon of the 23rd closed by the announcement alone (0.41 / 0.6 / 0.65 / 0.96 / 1.3 / 2.4 / 2.8 / 3.1 s), the fallback was never needed, the ESP did not reboot, and the "Dati fermi" alarm stayed off all night while the station went from discharging to solar charging.

**Not solved:** a few times (10 and 17–18 November) the mainboard got into a *deep* block where it served the module port only now and then; only switching the station off and on fixed it. That is a different problem from the 40‑minute pause, and the watchdog's last steps are still untested in the field for it.

---

## Hardware & wiring

- **Board:** ESP32‑S3 (config uses `esp32-s3-devkitc-1`). An ESP32‑S3‑WROOM‑1 N16R8 works well.
- **UART:** `GPIO17 (TX) → MAIN_TXD`, `GPIO16 (RX) → MAIN_RXD`, `GND → GND`, **115200 8N1, 3.3 V**.
- **Power:** feed the ESP from the module connector's **3.3 V (VBAT) and GND** — the same rail that powered the FC41D. **Never 5 V.**

> Pin numbers on the module connector are unit‑specific. On this unit the data connector carried `VBAT 3V3`, `GND`, `MAIN_RXD`, `MAIN_TXD` — **verify yours with a multimeter before connecting.** A single common ground is essential (a ground loop makes the ESP unstable).

Unplug the original FC41D module first; the ESP32 takes its place on the connector.

---

## Files

| file | what it is |
|---|---|
| `powerstation.yaml` | the ESPHome config: UART, Wi‑Fi/API/OTA, time from Home Assistant, ~60 template sensors, switches/numbers/selects/buttons, the status‑LED light, and the `interval:` blocks that drive the component and the LED |
| `powerstation_module.h` | the C++ component: responder state machine, TTLV parser, sensor registration, command writes |
| `secrets.yaml.example` | template for `secrets.yaml` (Wi‑Fi, API key, OTA password) |

### Setting `mac_[6]` (optional — the device key)

The firmware sends a 6‑byte identifier to the mainboard in the net‑status reply (Tuya `cmd 0x2D`). On this unit the MCU **just echoes it back and does not validate it**, so you can **leave it zeroed and it still works** — the ESP *is* the module now, and nothing talks to the cloud.

If you'd rather it carry your station's real identity (its *device key*, which on this platform equals the module's Wi‑Fi MAC), get it one of these ways:

- **BLE advertisement** (most reliable): the station advertises as `QUEC_BLE_*` (manufacturer id `0x5551`) with the `deviceKey` in clear in the manufacturer data — see [ha-oukitel #6](https://github.com/VictorCV-DAM/ha-oukitel/issues/6) for the exact byte layout.
- **Your router**, *before* you remove the stock module: the module's MAC in the DHCP leases is the device key.
- **The vendor app**: the device ID / SN screen.

It is 12 hex digits (e.g. `aabbccddeeff`). Split them into 6 bytes and write each as `0x..`:

```cpp
// device key   aa bb cc dd ee ff   ->
uint8_t mac_[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
```

---

## Install

1. Copy the three files into your ESPHome folder (e.g. `/config/esphome/`), and create `secrets.yaml` from the example.
2. *(Optional)* set `mac_[6]` to your station's device key — see [Setting `mac_[6]`](#setting-mac_6-optional--the-device-key). Leaving it zeroed works fine.
3. Compile & upload (first flash over USB, then OTA over Wi‑Fi):
   ```bash
   esphome run powerstation.yaml
   ```
4. In Home Assistant the device is adopted via the **ESPHome** integration (API encryption key from `secrets.yaml`). ~60 entities appear: battery SOC/V/A/W/temp, 13 cell voltages, AC / PV / grid, every DC/USB/Type‑C rail, board temperatures, status & fault (localized), uptime, plus switches (AC/DC out, grid feed, charge limit, beep, LED, Chiedi i dati, Registro seriale completo), numbers (output power, update interval), selects (mode, reserve, reporting mode, screen timeout), buttons (Riavvia ESP, Ricollega Wi‑Fi) and the "Dati fermi" alarm.

---

## Limitations (read these)

- **It's a hardware mod.** Opening the unit, unplugging the module, wiring the ESP. Reversible, but it's soldering/wiring — not a config toggle.
- **You lose the vendor app** — the cloud radio is gone. That's the point.
- **No‑load auto‑off.** When the station powers itself off for having no load, it cuts power to the module connector, so the ESP powers down too. It **boots back and reconnects on its own** (no router cycle) the moment the station wakes. So telemetry exists whenever the station is actually on — which is the only time there's anything to report.
- **One model only.** Verified on a single Landecia/Wonderfree S2400 (`p11tpn`). Other models/firmwares may differ (switch encodings especially — some report their outputs as structs rather than flat booleans).

---

## Credits

The serial protocol is the **same TTLV / `0xAAAA` scheme as the vendor LAN/cloud protocol**. Huge thanks to **[@VictorCV-DAM](https://github.com/VictorCV-DAM)** and the **[ha-oukitel](https://github.com/VictorCV-DAM/ha-oukitel)** integration for the LAN protocol work, the TTLV parser and the model definitions — this project stands on that groundwork.

Background and the software ("fake‑cloud" MQTT) approach, with full reverse‑engineering notes, are in **[ha-oukitel issue #6](https://github.com/VictorCV-DAM/ha-oukitel/issues/6)**. This repo is the **other door**: a hardware swap for whoever wants permanent cloud‑free operation.

---

## License

MIT — see [LICENSE](LICENSE). Reverse‑engineering of **your own** hardware for interoperability. No affiliation with Landecia, Wonderfree, Newsmy, Oukitel, Quectel or Acceleronix. Use at your own risk; opening a battery unit can be dangerous.
