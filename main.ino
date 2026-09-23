#include <Arduino.h>
#include <math.h>

// ======================================================
// HARDWARE PIN CONFIGURATION
// ======================================================
// Defines ESP32 GPIO connections used for:
// - DMX512 output (UART2)
// - MAX9814 microphone input (ADC)
// ======================================================

#define TXD2 17
#define RXD2 16
#define EN_PIN 21
#define MIC_PIN 34

// ======================================================
// DMX CONFIGURATION
// ======================================================
// DMX512 buffer contains 512 channels + 1 start code
// ======================================================

#define DMX_CHANNELS 512
uint8_t dmx[DMX_CHANNELS + 1];

// ======================================================
// PAR LIGHT CHANNEL MAPPING (RGB FIXTURE)
// ======================================================

#define PAR_RED    1
#define PAR_GREEN  2
#define PAR_BLUE   3

// ======================================================
// MOVING HEAD CHANNEL MAPPING
// ======================================================
// Controls pan, tilt, dimmer, and RGB output
// ======================================================

#define PAN_CH     17
#define TILT_CH    19
#define DIM_CH     22
#define RED_CH     23
#define GREEN_CH   24
#define BLUE_CH    25

// ======================================================
// MOVEMENT ENGINE (INFINITE FIGURE-8)
// ======================================================
// Generates smooth continuous motion using sine waves
// Independent from audio input
// ======================================================

float t = 0;              // time base for oscillation

float pan_s = 127;        // smoothed pan position
float tilt_s = 127;       // smoothed tilt position

float smoothFactor = 0.15; // movement smoothing factor

int panCenter = 127;      // neutral pan position
int tiltCenter = 240;     // neutral tilt position

int panAmp = 100;         // pan movement range
int tiltAmp = 80;         // tilt movement range

// ======================================================
// AUDIO PROCESSING (MAX9814 MICROPHONE)
// ======================================================
// Handles noise filtering, envelope tracking, and peak detection
// ======================================================

float envelope = 0;                // smooth audio amplitude
float peak = 0;                    // fast transient detector

float noiseFloor = 0;              // ambient noise baseline

unsigned long lastSoundTime = 0;   // sound hold timer
const int soundHold = 350;         // keeps RGB active after sound

// ======================================================
// COLOR STATE SYSTEM
// ======================================================
// r_s/g_s/b_s = current output color (smoothed)
// r_t/g_t/b_t = target color (sound or idle mode)
// ======================================================

float r_s = 255, g_s = 210, b_s = 120;
float r_t = 255, g_t = 210, b_t = 120;

float hue = 0; // used for RGB cycling in sound mode

// ======================================================
// DMX BREAK SIGNAL GENERATION
// ======================================================
// Required DMX512 protocol sequence:
// 1. BREAK (low signal)
// 2. MARK AFTER BREAK
// 3. UART restart
// ======================================================

void sendBreak() {
  Serial2.end();              // reset UART for BREAK timing
  pinMode(TXD2, OUTPUT);

  digitalWrite(TXD2, LOW);    // BREAK signal
  delayMicroseconds(120);

  digitalWrite(TXD2, HIGH);   // MARK AFTER BREAK
  delayMicroseconds(12);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);
}

// ======================================================
// DMX TRANSMISSION
// ======================================================
// Sends full DMX frame (~50Hz refresh rate)
// ======================================================

void sendDMX() {
  static unsigned long lastDMX = 0;

  if (millis() - lastDMX < 20) return;
  lastDMX = millis();

  digitalWrite(EN_PIN, HIGH);  // enable RS485 driver

  sendBreak();                 // DMX sync
  Serial2.write(dmx, DMX_CHANNELS + 1);
  Serial2.flush();

  digitalWrite(EN_PIN, LOW);   // disable transmitter
}

// ======================================================
// MICROPHONE CALIBRATION
// ======================================================
// Measures ambient noise level at startup to reduce false triggers
// ======================================================

void calibrateMic() {

  long sum = 0;

  // sample environment noise
  for (int i = 0; i < 200; i++) {
    sum += abs(analogRead(MIC_PIN) - 2048);
    delay(2);
  }

  noiseFloor = sum / 200.0;

  // safety minimum threshold
  if (noiseFloor < 20) noiseFloor = 20;
}

// ======================================================
// MICROPHONE PROCESSING
// ======================================================
// Converts raw ADC signal into normalized audio level (0–1)
// Includes filtering, envelope smoothing, and peak detection
// ======================================================

float readMic() {

  int raw = analogRead(MIC_PIN);
  int centered = abs(raw - 2048);

  // ignore low-level ambient noise
  if (centered < noiseFloor * 1.3)
    centered = 0;

  // fast peak tracking (for taps/claps)
  peak *= 0.85;
  if (centered > peak)
    peak = centered;

  // envelope follower (smooth audio energy)
  envelope = envelope * 0.75 + centered * 0.25;

  float level = envelope / (noiseFloor * 6.0);
  level = constrain(level, 0.0, 1.0);

  return level;
}

// ======================================================
// LIGHTING CONTROL SYSTEM
// ======================================================
// Switches between:
// - Sound Mode (RGB animation)
// - Idle Mode (warm white)
// ======================================================

void updateLighting(bool soundActive) {

  // SOUND MODE: RGB CYCLING
  if (soundActive) {

    hue += 0.04;
    if (hue > 6.28318) hue = 0;

    r_t = sin(hue) * 127 + 128;
    g_t = sin(hue + 2.094) * 127 + 128;
    b_t = sin(hue + 4.188) * 127 + 128;

  }
  // IDLE MODE: WARM WHITE
  else {

    r_t = 255;
    g_t = 210;
    b_t = 120;
  }

  // smooth color transition
  float smooth = 0.08;

  r_s += (r_t - r_s) * smooth;
  g_s += (g_t - g_s) * smooth;
  b_s += (b_t - b_s) * smooth;

  // apply to moving head RGB
  dmx[DIM_CH] = 255;

  dmx[RED_CH]   = constrain((int)r_s, 0, 255);
  dmx[GREEN_CH] = constrain((int)g_s, 0, 255);
  dmx[BLUE_CH]  = constrain((int)b_s, 0, 255);

  // mirror output to PAR light
  dmx[PAR_RED]   = dmx[RED_CH];
  dmx[PAR_GREEN] = dmx[GREEN_CH];
  dmx[PAR_BLUE]  = dmx[BLUE_CH];
}

// ======================================================
// MOVEMENT SYSTEM
// ======================================================
// Creates smooth infinite figure-8 motion using sine waves
// ======================================================

void updateMovement() {

  float panTarget = panCenter + sin(t) * panAmp;
  float tiltTarget = tiltCenter + sin(2 * t) * tiltAmp;

  pan_s += (panTarget - pan_s) * smoothFactor;
  tilt_s += (tiltTarget - tilt_s) * smoothFactor;

  dmx[PAN_CH]  = constrain((int)pan_s, 0, 255);
  dmx[TILT_CH] = constrain((int)tilt_s, 0, 255);

  t += 0.05; // motion speed control
}

// ======================================================
// SYSTEM SETUP
// ======================================================
// Initializes DMX, ADC, and microphone calibration
// ======================================================

void setup() {

  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);
  analogReadResolution(12);

  // clear DMX buffer
  for (int i = 0; i <= DMX_CHANNELS; i++) {
    dmx[i] = 0;
  }

  delay(1200);       // hardware stabilization
  calibrateMic();    // noise baseline calibration
}

// ======================================================
// MAIN LOOP
// ======================================================
// Runs real-time system:
// - reads audio
// - detects sound
// - updates lighting
// - updates movement
// - sends DMX output
// ======================================================

void loop() {

  dmx[0] = 0; // DMX start code

  float level = readMic();

  bool soundActive = false;

  // detect strong audio peaks
  if (peak > noiseFloor * 2.5) {
    lastSoundTime = millis();
  }

  // hold sound state for smooth transitions
  if (millis() - lastSoundTime < soundHold) {
    soundActive = true;
  }

  updateLighting(soundActive);
  updateMovement();
  sendDMX();
}
