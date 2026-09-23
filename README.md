# ESP32 DMX Sound Reactive Lighting Controller

A real-time ESP32-based DMX512 lighting controller that reacts to sound input using a MAX9814 microphone module.  
It controls stage lighting fixtures such as PAR lights and moving heads with smooth motion and audio-reactive RGB effects.

---

## Features

- Real-time audio reactivity using MAX9814 microphone
- DMX512 output (512 channels supported)
- RGB sound-reactive lighting effects
- Stable idle mode with warm white output
- Smooth infinite figure-8 moving head motion
- Fast peak detection for taps and claps
- Envelope-based sound processing with noise filtering
- Automatic ambient noise calibration
- Designed for stage, DJ, and performance lighting systems

---

## How It Works

- Microphone input is read via ESP32 ADC (GPIO34)
- Audio signal is processed using:
  - Envelope follower for smooth intensity tracking
  - Peak detection for instant sound response
  - Noise floor calibration for environment adaptation

- When sound is detected:
  - System switches to RGB reactive mode
  - Moving head continues independent motion

- When no sound is present:
  - System returns to warm white idle mode

---

## Hardware Requirements

- ESP32 development board
- MAX9814 microphone amplifier module
- DMX512 interface (RS485 module)
- RGB PAR lights
- DMX-controlled moving head fixture

---

## Wiring Overview

### MAX9814 Microphone Module
- VDD → 3.3V or 5V
- GND → GND
- OUT → GPIO34 (ADC input)

### DMX (RS485 Module)
- TXD2 → DI
- RXD2 → RO
- EN_PIN → DE/RE control
- VCC → 5V
- GND → GND

---

## Lighting Behavior

| Mode | Description |
|------|------------|
| Idle Mode | Stable warm white lighting |
| Sound Mode | RGB reactive rainbow effect |
| Transition | Smooth fade between states |

---

## Motion System

- Continuous infinite movement
- Figure-8 (infinity) pan/tilt pattern
- Independent of audio input
- Smooth interpolation for natural motion

---

## Configuration Parameters

You can tune the system by adjusting:

- soundHold: duration of active sound state
- panAmp / tiltAmp: movement range
- smoothFactor: motion smoothness
- microphone thresholds: sensitivity control

---

## Use Cases

- DJ lighting systems
- Stage performances
- Live events
- Interactive installations
- Ambient reactive lighting setups

---

## Project Name

ESP32 DMX Sound Reactive Lighting Controller

---

## License

This project is open-source and can be modified for personal or commercial use.

---

## Notes

For best performance:
- Set MAX9814 gain to low sensitivity (40dB if available)
- Use a stable power supply for ESP32 and DMX module
- Keep microphone away from speakers to reduce feedback
