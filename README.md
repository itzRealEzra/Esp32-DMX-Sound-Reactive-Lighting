# ESP32 DMX Sound Reactive Lighting Controller

A real-time ESP32-based DMX512 lighting controller that reacts to sound using a MAX9814 microphone module. Controls PAR lights and a moving head with smooth motion and audio-reactive RGB effects.

---

## Features

- DMX512 output (512 channels)
- Audio-reactive RGB via MAX9814 microphone
- Envelope + peak detection for smooth response and fast taps/claps
- Automatic ambient noise calibration
- Warm white idle mode with smooth fade to/from sound mode
- Independent infinite figure-8 pan/tilt motion

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
| VDD | 3.3V or 5V |
| GND | GND |
| OUT | GPIO34 (ADC input) |

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

The channel `#define`s in the sketch are DMX channel offsets, not GPIO pins — they only work if each fixture's DMX start address (set via its own DIP switches/menu) matches:

| Fixture | Start Address | Channels Used |
| --- | --- | --- |
| PAR light | 1 | 1 (Red), 2 (Green), 3 (Blue) |
| Moving head | 17 | 17 (Pan), 19 (Tilt), 22 (Dimmer), 23 (Red), 24 (Green), 25 (Blue) |

If you change a fixture's start address, shift its `#define`s in the sketch to match — e.g. moving the PAR to address 10 means updating `PAR_RED/GREEN/BLUE` to 10/11/12.

> Note: `PAR_RED` (channel 1) and `PAN_CH` (channel 17) are DMX channel numbers, unrelated to GPIO pin numbers like `TXD2` (GPIO17) — same digits, different meaning.

---

## Lighting Behavior

| Mode | Description |
| --- | --- |
| Idle | Stable warm white |
| Sound | RGB reactive rainbow effect |
| Transition | Smooth fade between states |

---

## Configuration Parameters

- `soundHold` — duration sound-active state is held
- `panAmp` / `tiltAmp` — movement range
- `smoothFactor` — motion smoothness
- Microphone thresholds — sensitivity control

---

## License

Open-source; modify for personal or commercial use.

---

## Notes

- Set MAX9814 gain to low sensitivity (40dB if available)
- Use a stable power supply for ESP32 and DMX module
- Keep microphone away from speakers to reduce feedback
