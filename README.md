# Powerstation ESP32 — fully local, no cloud, no freeze

Replace the Quectel/Acceleronix Wi‑Fi module inside a **Landecia / Wonderfree S2400** power station with an **ESP32 running [ESPHome](https://esphome.io)**. The ESP32 takes the module's place on the serial bus and speaks the **Tuya‑MCU protocol directly to the mainboard**, so all telemetry and control stay **100 % local**:

- no vendor cloud, no account, no app login
- no BLE provisioning, no TLS‑PSK, no device "bind"
- and the infamous **~40‑minute freeze is gone at the root** — because there is no TCP‑to‑cloud session on the serial wire to stall.

Verified running continuously well past the 42‑minute mark (1 h 40+), telemetry updating every ~5 s, zero freezes.

> This is a **hardware mod** (you open the unit and swap a module). It is fully reversible — unplug the ESP, plug the original module back in.

---

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

On the stock module, the station's telemetry runs over a TCP session to the Acceleronix cloud (or the LAN fallback that still depends on a cloud session key). That session dies on its own after ~40 minutes and the data stalls until you force a reconnect. Chasing it through fake‑cloud brokers, BLE provisioning and TLS‑PSK is a deep rabbit hole.

**The serial wire has none of that.** The mainboard just wants *a module* on its UART that answers the handshake and keeps it in "connected" state. So we become the module. No cloud session exists, so nothing can time out.

---

## How it works

The custom component (`powerstation_module.h`, included via `esphome: includes:`) is a **response‑gated responder + TTLV parser**:

1. **Handshake.** Send the Tuya heartbeat in INIT until the MCU replies `55AA 03 00 00 01 01 04`; answer product‑info `0x01`; **send net‑status `0x03 = 0x04` ("connected to cloud")** — that byte turns on the Wi‑Fi icon and **unlocks the `0x07` telemetry reports**. Also answer the Quectel‑specific `0x2B` / `0x35` exchange, SNTP time `0x1C`, MAC `0x2D`, IP `0x90`.
2. **Two report families.** A **fast set** (PV / grid / AC / DC) arrives only after an `0x08` "read‑all" query; a **slow set** (battery / cells / temps) is pushed by the MCU on its own every ~2–3 min. **Hammering `0x08` suppresses the slow set**, so instead we turn on **high‑frequency reporting** (`DP 19 = 1`): the MCU then pushes *everything* every ~5 s by itself. It drops back after ~20 s, so the firmware **re‑arms it every 10 s**.
3. **Parser.** TTLV: `id(2B) | type(1) | len(2B) | value`; `0x11` = double (8 B, no scale), `0x02/0x04` = int, `0x12` = struct → recurse. Sensor key = top‑level id, or `(group<<8)|sub` inside a struct/array.
4. **Writes.** Switches / numbers / selects write back with `cmd 0x06`, same `id|type|len|value` encoding.
5. **Onboard status LED.** The ESP32‑S3 RGB LED is exposed as a normal light (`Spia`) you can color/dim; it flips to **red** automatically if the serial link to the mainboard ever goes mute.

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
| `powerstation.yaml` | the ESPHome config: UART, Wi‑Fi/API/OTA, SNTP, ~60 template sensors, switches/numbers/selects, the status‑LED light, and the two `interval:` blocks (12 ms drives the component, 1 s drives the LED) |
| `powerstation_module.h` | the C++ component: responder state machine, TTLV parser, sensor registration, command writes |
| `secrets.yaml.example` | template for `secrets.yaml` (Wi‑Fi, API key, OTA password) |

> ⚠️ In `powerstation_module.h` the `mac_[6]` array is **zeroed out** — put **your** station's Wi‑Fi MAC / deviceKey there (from the BLE advertisement or the label). The MCU only echoes it back, but it should be yours.

---

## Install

1. Copy the three files into your ESPHome folder (e.g. `/config/esphome/`), and create `secrets.yaml` from the example.
2. Set your `mac_[6]` in `powerstation_module.h`.
3. Compile & upload (first flash over USB, then OTA over Wi‑Fi):
   ```bash
   esphome run powerstation.yaml
   ```
4. In Home Assistant the device is adopted via the **ESPHome** integration (API encryption key from `secrets.yaml`). ~60 entities appear: battery SOC/V/A/W/temp, 13 cell voltages, AC / PV / grid, every DC/USB/Type‑C rail, board temperatures, status & fault (localized), uptime, plus switches (AC/DC out, grid feed, charge limit, beep, LED), numbers (output power, update interval) and selects (mode, reserve, reporting mode, screen timeout).

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
