# ESP32 DMX Sound Reactive Lighting

[![ESP32](https://img.shields.io/badge/MCU-ESP32-red)](https://www.espressif.com/en/products/socs/esp32) [![DMX512](https://img.shields.io/badge/Lighting-DMX512-blue)](https://en.wikipedia.org/wiki/DMX512) [![License](https://img.shields.io/badge/License-Open%20Source-green)](#license)

A real-time **ESP32 sound-reactive DMX lighting controller** using a **MAX9814 microphone** and an **RS485/DMX512 interface**.

The project contains the original lighting sketches, an OTA-enabled variant, and a newer `new_implementation` based on a fixed 50 Hz DMX task with sound-reactive RGB cycling and strobe effects.

> **Recommended starting point:** `new_implementation/GawaNiAxiomWithRGBStrobe_Clean.ino`

---

## What It Does

The ESP32 listens to a MAX9814 microphone and converts the measured microphone energy into a **relative sound-level value** above the room's learned noise floor.

That level controls:

- RGB color zones
- RGB rainbow cycling at high sound levels
- Strobe effects at the highest level
- Moving-head pan/tilt figure-8 motion
- RGB output for both the PAR and moving head
- DMX output timing

The newer implementation keeps microphone processing separate from the lighting/DMX task so a slow ADC read does not directly control the timing of the lights.

> **Important:** The `new_implementation` dB value is a **relative control value above the learned room noise floor**. It is not a calibrated SPL measurement in decibels.

---

## Project Structure

| Path | Purpose |
| --- | --- |
| `sound_reactive_dmx.ino` | Earlier calibrated dB-based implementation |
| `original_pure_white.ino` | Original sound-reactive implementation |
| `ota_sound_reactive_dmx.ino` | Wi-Fi / ArduinoOTA variant |
| `secrets.h.example` | Example Wi-Fi/OTA credential configuration |
| `new_implementation/GawaNiAxiomWithRGBStrobe_Clean.ino` | **Recommended newer implementation**, fully documented |
| `new_implementation/GawaNiAxiomWithRGBStrobe_NoWiFi.ino` | No-Wi-Fi version of the newer implementation |
| `.gitignore` | Ignores local/private files such as `secrets.h` |

---

## Recommended New Implementation

### Files

- [`GawaNiAxiomWithRGBStrobe_Clean.ino`](new_implementation/GawaNiAxiomWithRGBStrobe_Clean.ino)
- [`GawaNiAxiomWithRGBStrobe_NoWiFi.ino`](new_implementation/GawaNiAxiomWithRGBStrobe_NoWiFi.ino)

These sketches implement the newer **GawaNiAxiom** lighting system.

### Sound-Level Zones

| Relative level | Zone | Lighting behavior |
| --- | --- | --- |
| Below 30 dB | 0 | White / idle |
| 30–32 dB | 1 | Red |
| 32–34 dB | 2 | Green |
| 34–36 dB | 3 | Blue |
| 36 dB and above | 4 | RGB rainbow cycling |
| 37 dB and above | Strobe | Strobe is added on top of zone 4 |

The zones use **1 dB hysteresis** so the lighting does not rapidly switch back and forth around a threshold.

A minimum zone hold time of **200 ms** also prevents extremely rapid zone changes.

### RGB Cycling

At zone 4, the controller continuously cycles through the RGB rainbow.

- Full cycle: **3 seconds**
- Color transitions use a smooth one-pole slew
- Changing sound level does not restart the rainbow animation

### Strobe

The strobe is only active while the controller is in the RGB-cycling zone.

| Setting | Value |
| --- | --- |
| Strobe threshold | 37 dB |
| Flash rate | 10 Hz |
| Duty | 40% |
| Low dimmer level | 0 |

The strobe operates through the fixture dimmer, so the RGB colors continue cycling underneath the effect.

> **Safety:** flashing lights in roughly the 3–30 Hz range can trigger seizures in people with photosensitive epilepsy. Add a way to disable the strobe before using the effect in public spaces.

---

## Hardware

### Required

- ESP32 development board
- MAX9814 microphone amplifier
- RS485 transceiver suitable for DMX512
- RGB PAR fixture
- DMX-controlled moving-head fixture
- Stable power supply
- DMX cable and suitable fixture termination for your setup

---

## Wiring

### MAX9814

| MAX9814 | ESP32 |
| --- | --- |
| VDD | 3.3V |
| GND | GND |
| OUT | GPIO34 |
| GAIN | VDD |
| A/R | GND |

The newer implementation uses **GPIO34 (ADC1)** for the microphone signal.

### RS485 / DMX

| RS485 module | ESP32 |
| --- | --- |
| DI | GPIO17 / TXD2 |
| RO | GPIO16 / RXD2 |
| DE/RE | GPIO21 |
| VCC | 5V |
| GND | GND |

The controller keeps the RS485 driver enabled for DMX transmission.

---

## DMX Channel Mapping

The current `new_implementation` uses the following channel map.

### RGB PAR

| DMX Channel | Function |
| ---: | --- |
| 1 | Dimmer |
| 2 | Red |
| 3 | Green |
| 4 | Blue |

### Moving Head

| DMX Channel | Function |
| ---: | --- |
| 17 | Pan |
| 19 | Tilt |
| 22 | Dimmer |
| 23 | Red |
| 24 | Green |
| 25 | Blue |

The moving head should therefore use **DMX start address 17**, while the PAR uses **DMX start address 1**.

> DMX channel numbers are not GPIO numbers. For example, moving-head channel 17 and ESP32 GPIO17 are unrelated values.

---

## DMX Transmission

The newer implementation keeps a 512-channel DMX buffer but transmits only the first **32 channels**, which is enough to cover the highest channel currently used (25).

This reduces the data time per frame compared with transmitting all 512 channels.

### Timing

- DMX baud rate: **250000**
- Serial format: **8N2**
- Lighting update rate: **50 Hz**
- DMX send slots: **32**
- DMX frame period: **20 ms**

Each frame manually generates the DMX **BREAK** and **MARK-AFTER-BREAK** before sending the start code and channel data.

---

## Architecture

The `GawaNiAxiomWithRGBStrobe_Clean.ino` implementation separates the microphone and lighting responsibilities.

### `loop()`

The main loop:

1. Samples the microphone
2. Calculates the relative sound level
3. Updates the requested lighting zone
4. Updates the requested strobe state
5. Prints diagnostic information

The microphone loop does **not** directly update DMX lighting state.

### `dmxTask`

A FreeRTOS task runs at a fixed **50 Hz** and owns:

- Current lighting zone
- RGB color state
- Rainbow animation
- Strobe state
- Moving-head movement
- DMX buffer
- DMX transmission

This prevents microphone sampling delays from directly causing visible lighting timing changes.

---

## Sound Detection

The MAX9814 signal is sampled from the ESP32 ADC.

The newer implementation uses:

1. RMS measurement
2. Three RMS windows
3. Median-of-three selection
4. Smoothed signal power
5. Adaptive room-noise floor
6. Fast attack / slower release
7. Relative dB calculation

The result is a stable sound-reactive control signal rather than a laboratory-grade SPL measurement.

### Relative dB Model

The controller learns the quiet-room microphone floor during startup.

Conceptually:

`relativeLevel = measuredLevel - learnedNoiseFloor + QUIET_DB`

The default quiet reference is:

`QUIET_DB = 25`

This means the displayed value is intended for **lighting control**, not accurate acoustic measurement.

---

## Startup Calibration

The newer sketches automatically calibrate the microphone during startup.

At boot:

1. The DMX task starts.
2. The controller waits about 1.2 seconds.
3. The microphone is sampled for approximately 1.5 seconds.
4. The quiet-room noise floor is calculated.
5. Normal sound-reactive operation begins.

### During Startup Calibration

Keep the room relatively quiet.

Do not clap, play music, or speak directly into the microphone while the startup calibration is running, because the controller is trying to learn the background noise floor.

The Serial Monitor reports the measured floor, for example:

`Mic calibrated: floor 41.7 dB`

---

## Serial Monitor

Use:

`115200 baud`

The newer implementation prints status information about:

- Current relative dB level
- Learned floor
- Requested zone
- Strobe request

Example:

`dB: 34.8  floor: 42.1  zoneReq: 3  strobe: 0`

It also periodically prints raw ADC statistics:

`RAW min 1870  max 2215  mean 2044`

This is useful for checking microphone bias and detecting clipping or abnormal ADC readings.

---

## Movement System

The moving head uses an infinite **figure-8** pattern.

Pan and tilt are generated from two sine waves:

- Pan: `sin(t)`
- Tilt: `sin(2t)`

The 1:2 frequency relationship produces the figure-8 motion.

### Current Movement Settings

| Setting | Value |
| --- | ---: |
| Pan center | 127 |
| Pan amplitude | 100 |
| Tilt center | 240 |
| Tilt amplitude | 80 |
| Smooth factor | 0.15 |
| Movement speed | 0.05 per reference step |

### Tilt Range Warning

The current values produce a theoretical tilt range of:

`240 - 80 = 160`

to

`240 + 80 = 320`

DMX values cannot exceed **255**, so the upper portion is clipped.

The code currently keeps this behavior intentionally, but a full unclipped figure-8 can be achieved by lowering the tilt center. For example:

`tiltCenter = 175`

with:

`tiltAmp = 80`

gives a usable range of approximately **95–255**.

---

## Important Configuration

The main configuration values are near the top of the `GawaNiAxiomWithRGBStrobe_Clean.ino` sketch.

| Setting | Purpose |
| --- | --- |
| `QUIET_DB` | Quiet-room reference value |
| `SENSITIVITY` | Scales the relative sound level |
| `HYST` | Zone/strobe hysteresis |
| `ZONE_LO[]` | Lower boundary of each zone |
| `ZONE_HI[]` | Upper boundary of each zone |
| `COLOR_TAU_MS` | RGB transition smoothness |
| `RGB_CYCLE_SECONDS` | Rainbow cycle speed |
| `STROBE_DB` | Strobe activation threshold |
| `STROBE_HZ` | Strobe flash rate |
| `STROBE_DUTY` | Lit portion of each strobe cycle |
| `panCenter` | Pan center |
| `tiltCenter` | Tilt center |
| `panAmp` | Pan range |
| `tiltAmp` | Tilt range |
| `smoothFactor` | Movement smoothing |
| `MOVE_SPEED` | Movement speed |

---

## Uploading the New Implementation

### USB / No Wi-Fi

For a simple USB upload, use:

`new_implementation/GawaNiAxiomWithRGBStrobe_NoWiFi.ino`

This version contains no Wi-Fi or OTA code.

### Recommended Documented Version

For development and tuning, use:

`new_implementation/GawaNiAxiomWithRGBStrobe_Clean.ino`

It contains the same core lighting system with substantially more documentation and configuration comments.

---

## Older Implementations

The repository keeps the older sketches so previous behavior is not lost.

### `sound_reactive_dmx.ino`

The earlier implementation uses calibrated dB-style thresholds and a simpler white/red/blue/green lighting system.

### `original_pure_white.ino`

The original sound-reactive version.

### `ota_sound_reactive_dmx.ino`

Adds Wi-Fi and **ArduinoOTA** firmware updates to the older lighting implementation.

This variant is separate from the `new_implementation` folder.

For OTA use:

1. Copy `secrets.h.example` to `secrets.h`.
2. Enter the required Wi-Fi and OTA credentials.
3. Upload the sketch once over USB.
4. Use OTA for subsequent firmware updates.

> `secrets.h` should remain local and should not be committed to the repository.

---

## Troubleshooting

| Problem | Possible cause / solution |
| --- | --- |
| Lights do not respond | Check DMX start addresses, RS485 wiring, fixture mode, and DMX cable connections. |
| Wrong fixture channels react | Verify the fixture's DMX start address and channel mapping. |
| Zone changes too easily | Increase `HYST` or adjust `ZONE_LO[] / ZONE_HI[]`. |
| Zone does not change enough | Lower the zone thresholds or increase `SENSITIVITY`. |
| Strobe activates too often | Increase `STROBE_DB`. |
| Strobe is too aggressive | Reduce `STROBE_HZ` or increase `STROBE_DUTY`. |
| Microphone readings look wrong | Check MAX9814 power, GPIO34 wiring, ADC bias, and microphone gain configuration. |
| Raw ADC values are near 0 or 4095 | The microphone signal may be incorrectly powered, wired, or clipping. |
| Moving head tilt looks flattened | Lower `tiltCenter`; the current 240 + 80 range exceeds DMX 255. |
| DMX fixture does not like short frames | Increase `DMX_SEND_SLOTS` so more channels are transmitted. |
| Lights stutter while audio is being sampled | Use the newer `GawaNiAxiomWithRGBStrobe_Clean.ino`, which separates mic processing from the DMX task. |

---

## Safety

- Do not point moving lights directly at people's eyes.
- Keep the microphone away from speakers to reduce acoustic feedback.
- Use appropriate electrical isolation and safe mains practices for lighting equipment.
- High sound-pressure levels can damage hearing.
- The strobe effect may create a seizure risk for photosensitive individuals.
- Add a physical or software strobe disable before public/event use.

---

## Notes

The MAX9814 includes automatic gain control (AGC), so the microphone response can change depending on the source material and environment. The newer implementation is therefore designed primarily as a **sound-reactive lighting detector**, not an accurate SPL meter.

Use a stable power supply and proper DMX cabling for reliable operation.

---

## License

Open-source; modify for personal or commercial use.

---

## Credits

Built with:

- **ESP32** for processing and DMX control
- **MAX9814** for microphone input and automatic gain control
- **RS485 transceiver** for DMX physical-layer communication
- **DMX512 fixtures** for lighting output

---

⭐ If this project is useful for your lighting setup or experimentation, consider starring the repository.
