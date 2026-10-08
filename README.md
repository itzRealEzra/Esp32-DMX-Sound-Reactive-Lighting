# ESP32 DMX Sound Reactive Lighting

[![ESP32](https://img.shields.io/badge/MCU-ESP32-red)](https://www.espressif.com/en/products/socs/esp32) [![DMX512](https://img.shields.io/badge/Lighting-DMX512-blue)](https://en.wikipedia.org/wiki/DMX512) [![License](https://img.shields.io/badge/License-Open%20Source-green)](#license)

A real-time **ESP32 sound-reactive DMX lighting controller** built around a **MAX9814 microphone** and an **RS485/DMX512 interface**. The controller measures room loudness, converts it into calibrated dB values, and uses those levels to drive an RGB PAR light and a moving-head fixture.

The default behavior is simple: **quiet = pure white**, then **red → blue → green** as the sound level rises. The project also includes an **OTA-enabled variant** for wireless firmware updates.

> Built for practical stage-lighting experiments with the ESP32, DMX512, microphone signal processing, and sound-reactive effects.

---

## Quick Start

1. Connect the MAX9814 microphone and RS485/DMX interface using the wiring tables below.
2. Set the RGB PAR to DMX start address **1**.
3. Set the moving head to DMX start address **17**.
4. Open `sound_reactive_dmx.ino` in Arduino IDE and select your ESP32 board and COM port.
5. Upload the sketch and open Serial Monitor at **115200 baud**.
6. Play steady sound and calibrate with `cal <dB>`, for example `cal 82.5`.
7. Start playing music and the lights will react to the measured loudness.

For wireless updates, use `ota_sound_reactive_dmx.ino` after creating `secrets.h` from `secrets.h.example`.

---

## Project Structure

| Path | Purpose |
| ---- | ------- |
| `sound_reactive_dmx.ino` | Main calibrated dB-based implementation. |
| `original_pure_white.ino` | Original sound-reactive implementation. |
| `ota_sound_reactive_dmx.ino` | WiFi OTA variant with the newer timing/audio path. |
| `secrets.h.example` | Template for WiFi and OTA credentials. |
| `new_implementation/` | Development / implementation files. |
| `.gitignore` | Keeps local/private files such as `secrets.h` out of Git. |

---

## Sketches in This Repo

| File | What it is |
| ---- | ---------- |
| `sound_reactive_dmx.ino` | dB-calibrated version: white idle, then red, blue, green by loudness (described below) |
| `original_pure_white.ino` | Original version: white idle, RGB cycling on sound |
| `ota_sound_reactive_dmx.ino` | Original behavior plus WiFi OTA updates, a fixed 50 Hz tick, and a faster audio path (see [OTA Variant](#ota-variant)) |

---

## Features

- 🎤 MAX9814 microphone input for real-time sound detection
- 💡 DMX512 output for RGB PAR and moving-head fixtures
- 🌈 Sound-reactive color zones: white → red → blue → green
- 🔆 Reactive PAR brightness and moving-head motion
- 🔄 Figure-8 pan/tilt animation with sound-reactive speed
- 🧠 3 dB hysteresis to reduce color flicker around thresholds
- 📈 Calibrated dB readings with the offset saved to ESP32 flash
- ⚠️ Microphone clipping detection
- ⚙️ FreeRTOS audio processing in the standard implementation
- 📡 Optional ArduinoOTA wireless firmware updates

### Technical Details

- DMX512 output (up to 512 channels, ~43 Hz standard frame rate)
- Loudness measured in dB SPL, calibrated over Serial and saved to flash
- dB color zones: pure white idle, then red, blue, green as the level rises
- PAR brightness pulses with the sound inside each color zone
- Moving-head figure-8 motion speeds up with loudness
- Audio sampling runs independently so microphone processing does not block the normal DMX control loop

---

## Features

- DMX512 output (512 channels, ~43 Hz frame rate)
- Loudness measured in dB SPL, calibrated once over Serial and saved to flash
- dB color zones: pure white idle, then red, blue, green as the level rises
- 3 dB hysteresis so a level sitting on a threshold doesn't flicker between colors
- PAR brightness pulses with the sound inside each color zone
- Moving head figure-8 motion that speeds up with loudness
- Clip detection to tell you when the mic gain is too high
- Audio sampling runs in its own FreeRTOS task on core 0, so the DMX output never stalls it

---

## Hardware Requirements

- ESP32 development board
- MAX9814 microphone amplifier module
- DMX512 interface (RS485 module)
- RGB PAR light(s)
- DMX-controlled moving head fixture

---

## Wiring

### MAX9814 Microphone Module

| Pin | Connects to |
| --- | --- |
| VDD | 3.3V |
| GND | GND |
| OUT | GPIO34 (ADC1 input) |
| GAIN | VDD (40 dB max gain) |
| A/R | GND (fastest release) |

**Why these settings matter**

- **GAIN to VDD (40 dB):** GAIN floating gives 60 dB and GND gives 50 dB. At 60 dB the output pins at the rail long before 100 dB, so the 80 and 100 dB zones can't be told apart.
- **A/R to GND:** This is the shortest release time. Floating or VDD keeps the gain pulled down for several seconds after a loud hit, which holds the lights on a lower color.
- **VDD at 3.3V:** The module works from 2.7 to 5.5 V. Using the ESP32's 3.3V rail avoids dragging 5V supply noise into the ADC.

### DMX (RS485 Module)

| Pin | Connects to |
| --- | --- |
| DI | TXD2 (GPIO17) |
| RO | RXD2 (GPIO16) — unused (transmit-only) |
| DE/RE | EN_PIN (GPIO21) |
| VCC | 5V |
| GND | GND |

---

## DMX Addressing

The channel `#define`s in the sketch are DMX channel offsets, not GPIO pins. They only work if each fixture's DMX start address (set via its own DIP switches/menu) matches:

| Fixture | Start Address | Channels Used |
| --- | --- | --- |
| PAR light | 1 | 1 (Red), 2 (Green), 3 (Blue) |
| Moving head | 17 | 17 (Pan), 19 (Tilt), 22 (Dimmer), 23 (Red), 24 (Green), 25 (Blue) |

If you change a fixture's start address, shift its `#define`s in the sketch to match. For example, moving the PAR to address 10 means updating `PAR_RED/GREEN/BLUE` to 10/11/12.

> Note: `PAR_RED` (channel 1) and `PAN_CH` (channel 17) are DMX channel numbers, unrelated to GPIO pin numbers like `TXD2` (GPIO17). Same digits, different meaning.

---

## Lighting Behavior

| Room level | Color (PAR and moving head) |
| --- | --- |
| Under 60 dB | Pure white (idle) |
| 60 dB and up | Red |
| 80 dB and up | Blue |
| 100 dB and up | Green |

Extras on top of the color zones:

| Fixture | Behavior |
| --- | --- |
| PAR light | Brightness pulses with the sound inside a zone (40% to 100%). Idle white stays at full brightness. |
| Moving head | Figure-8 pan/tilt keeps running and speeds up as the room gets louder. Dimmer stays at full by default. |
| Both | The last color is held for 350 ms after the level drops, then fades to white. |

---

## First-Time Setup: Calibration

The ADC only reports counts. Turning them into real dB needs a one-time reference against an SPL meter (a phone dB-meter app works for a first pass).

1. Wire the MAX9814 with GAIN to VDD and A/R to GND (see above).
2. Flash the sketch and open the Serial Monitor at **115200 baud**.
3. Play steady sound (a tone or pink noise) near the mic, with a reference meter beside it, for about 3 seconds.
4. Type `cal` followed by the dB the meter shows, for example:

```
cal 82.5
```

The offset is saved to flash and survives power cycles. Recalibrate if you change the room, the mic position, or the GAIN pin wiring.

**Tips**

- Calibrate with sound playing, not in a silent room. The MAX9814's automatic gain is highest in quiet rooms, so an offset taken in silence is wrong once music starts.
- Pick a calibration level near the middle of your range, around 80 dB.

### Serial Commands

| Command | What it does |
| --- | --- |
| `cal <dB>` | Sets the offset so the current reading equals the reference dB you give (30 to 130) |
| `reset` | Clears the offset back to 0 |
| `show` | Prints the stored offset |

### Serial Output

The sketch prints a status line every 200 ms:

```
SPL 78.4 dB  level 0.57  zone 1  ok
```

| Field | Meaning |
| --- | --- |
| `SPL` | Calibrated loudness in dB |
| `level` | The same loudness scaled 0 to 1 (50 dB to 100 dB), used for PAR brightness and head speed |
| `zone` | 0 idle, 1 red, 2 blue, 3 green |
| `ok` / `CLIP` | `CLIP` means the ADC hit its limit. Lower the mic gain and recalibrate. |

---

## Configuration Parameters

All of these are near the top of the sketch.

| Parameter | What it does |
| --- | --- |
| `ZONE_DB` | The three thresholds (60, 80, 100 dB) |
| `HYST_DB` | How far below a threshold the level must fall to leave a zone (default 3 dB) |
| `soundHold` | How long the last color is held after sound drops (ms) |
| `LEVEL_LO_DB` / `LEVEL_HI_DB` | dB range mapped to 0 to 1 for brightness and speed |
| `BRIGHT_MIN` | Minimum PAR brightness when reactive (0 to 1) |
| `PAR_LEVEL_BRIGHTNESS` | PAR brightness follows the level |
| `HEAD_LEVEL_BRIGHTNESS` | Moving head dimmer follows the level |
| `HEAD_MOVE_REACTIVE` | Figure-8 speed follows the level (false = constant speed) |
| `panAmp` / `tiltAmp` | Movement range |
| `panCenter` / `tiltCenter` | Center point of the figure-8 |
| `smoothFactor` | Motion smoothness |

Keep `tiltCenter ± tiltAmp` and `panCenter ± panAmp` inside 0 to 255, or the figure-8 gets clipped flat at the limit.

---

## Troubleshooting

| Problem | Likely cause and fix |
| --- | --- |
| Colors switch at the wrong volumes | Not calibrated, or calibrated in silence. Run `cal` again with sound playing. |
| 80 dB and 100 dB look the same, `CLIP` shows | Mic gain too high. Tie GAIN to VDD and recalibrate. |
| Colors stay stuck on a lower level after loud hits | A/R pin is floating or on VDD. Tie it to GND. |
| Lights flicker between two colors | Raise `HYST_DB`. |
| Fixtures don't respond | Check the fixture start addresses against the DMX Addressing table and the DE/RE wiring on GPIO21. |

---

## OTA Variant

`ota_sound_reactive_dmx.ino` keeps the white-idle / RGB-cycling behavior of `original_pure_white.ino` and adds wireless updates and timing fixes.

**What changed**

- **ArduinoOTA:** flash over WiFi with `espota.py`. OTA only starts once WiFi connects, so the lights still run with no network. The lights blackout while an update runs.
- **Short DMX frames:** only 32 slots are sent (start code plus channels 1 to 31, covering the highest used channel, 25). A frame drops from about 22.7 ms to about 1.7 ms.
- **Fixed 20 ms tick (50 Hz):** the mic, color smoothing, and movement all run on one steady tick, so every smoothing constant means what it says.
- **Burst-sampled mic:** each tick samples the ADC 128 times and uses half the peak-to-peak swing. This removes the hardcoded 2048 DC center, which was wrong for the MAX9814 bias point.
- **Adaptive noise floor:** the floor creeps toward ambient noise during silence, so a louder room no longer latches sound mode on.
- **Tilt range fixed:** `tiltCenter` is 175, so the figure-8 spans 95 to 255 and no longer clips flat at the top.
- **Wrapped time base:** `t` wraps at 2π so float precision never degrades on long runs.

**Setup**

1. Copy `secrets.h.example` to `secrets.h` in the same folder as the sketch and fill in your 2.4 GHz WiFi details and an OTA password. `secrets.h` is gitignored, so credentials are never committed.
2. Flash once over USB. After that, updates can go over WiFi.
3. Tune clap sensitivity with the `2.5f` peak threshold in `loop()`. Move it in 0.25 steps until claps trigger and normal conversation doesn't.

**OTA security:** ArduinoOTA is password-challenged but not encrypted. Use a long random OTA password and keep the controller on a trusted or isolated network.

---

## Development Notes

The repository intentionally keeps the original and newer implementations side by side. This makes it easier to compare the original lighting behavior with the calibrated dB implementation and the WiFi/OTA variant without losing the earlier version.

The most important values to tune are the sound-zone thresholds, hysteresis, brightness mapping, and moving-head pan/tilt range. Keep the final DMX movement values inside **0–255** to avoid clipping.

---

## Notes

- AGC (automatic gain control) is built into the MAX9814 and compresses loud sound. Expect the dB readings to be approximate and to drift a few dB with the program material.
- Use a stable power supply for the ESP32 and DMX module.
- Keep the microphone away from speakers to reduce feedback.
- **Hearing safety:** 100 dB SPL damages hearing after short exposure. Calibrate at a level you can tolerate and keep your ears away from the loudest zone.

---

## License

Open-source; modify for personal or commercial use.

---

## Credits

Built with:

- **ESP32** for processing and DMX control
- **MAX9814** for microphone amplification and automatic gain control
- **RS485 transceiver** for DMX physical-layer communication
- **DMX512 fixtures** for the lighting output

---

⭐ If this project helps with your lighting setup or experimentation, consider starring the repository.
