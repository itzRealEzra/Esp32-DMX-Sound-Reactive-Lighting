#include <Arduino.h>
#include <math.h>

// ======================================================
// HARDWARE PIN CONFIGURATION
// ======================================================

#define TXD2 17
#define RXD2 16
#define EN_PIN 21
#define MIC_PIN 34

// ======================================================
// DMX CONFIGURATION
// ======================================================
// Only the first DMX_SEND_SLOTS channels go on the wire.
// 33 bytes x 44 us = ~1.5 ms per frame instead of ~22.7 ms for
// all 512. If a fixture misbehaves with short frames, raise this.
// ======================================================

#define DMX_CHANNELS 512
#define DMX_SEND_SLOTS 32
#define DMX_FRAME_MS 20  // 50 Hz, paced by the dmx task
uint8_t dmx[DMX_CHANNELS + 1];

// ======================================================
// PAR LIGHT CHANNEL MAPPING (RGB FIXTURE)
// ======================================================

#define PAR_RED 2
#define PAR_GREEN 3
#define PAR_BLUE 4
#define PAR_DIM 1

// ======================================================
// MOVING HEAD CHANNEL MAPPING
// ======================================================
// ======================================================

#define PAN_CH 17
#define TILT_CH 19
#define DIM_CH 22
#define RED_CH 23
#define GREEN_CH 24
#define BLUE_CH 25

// ======================================================
// MOVEMENT ENGINE (INFINITE FIGURE-8)
// ======================================================
// Smooth continuous motion using sine waves.
// Independent from audio input.
// (Original values, untouched.)
// ======================================================

const float TWO_PI_F = 6.2831853f;

float t = 0;  // time base for oscillation

float pan_s = 127;   // smoothed pan position
float tilt_s = 127;  // smoothed tilt position

float smoothFactor = 0.15;  // movement smoothing factor

int panCenter = 127;   // neutral pan position
int tiltCenter = 240;  // neutral tilt position

int panAmp = 100;  // pan movement range
int tiltAmp = 80;  // tilt movement range

// ======================================================
// dB DETECTION (MAX9814 MICROPHONE)
// ======================================================

const float QUIET_DB = 25.0;
const float HYST = 1.0;
const float RELEASE_DB_PER_SEC = 10.0;
const float FLOOR_FALL_DB_PER_SEC = 1.0;
const float SENSITIVITY = 1.0;

const float ZONE_LO[5] = { -1000, 30, 32, 34, 36 };
const float ZONE_HI[5] = { 30, 32, 34, 36, 1000 };

float floorDb = 0;
float dbSmooth = 0;
float powAvg = 0;
bool dbInit = false;

// zoneReq: what the mic thinks (written by loop)
// zone:    what the lights show (owned by the dmx task)
volatile int zoneReq = 0;
int zone = 0;  // 0 = idle/white, 1 = red, 2 = green, 3 = blue, 4 = RGB cycling
unsigned long zoneSince = 0;

// ======================================================
// COLOR STATE SYSTEM
// ======================================================

float r_s = 255, g_s = 255, b_s = 255;  // shown color
float r_t = 255, g_t = 255, b_t = 255;  // target color
float hue = 0;                          // RGB-cycle position (radians)

// ---- ANIMATION SETTINGS ----
// Color follows its target with a one-pole slew. Retargeting mid-flight
// never restarts anything, so there is no velocity jump and no pulse.
const float COLOR_TAU_MS = 90.0f;        // lower = snappier, higher = softer
const float ZONE_MIN_HOLD_MS = 200.0f;   // max ~5 color changes per second
const float RGB_CYCLE_SECONDS = 3.0f;    // one full rainbow loop
const uint8_t DIM_LEVEL = 255;           // steady dimmer level (strobe flashes below it)

// ---- STROBE (merged into the RGB cycling zone) ----
// At STROBE_DB and up, the RGB zone keeps cycling its colors and the
// dimmer flashes on top. Strobe only shows while the RGB zone is up.
// The dmx task runs at 50 Hz (20 ms frames), so rates that divide 50
// evenly stay crisp: 5, 6.25, 8.33, 10, 12.5, 16.67, 25 Hz.
const float STROBE_DB = 37.0f;     // turns on at this level, off below STROBE_DB - HYST
const float STROBE_HZ = 10.0f;     // flashes per second
const float STROBE_DUTY = 0.4f;    // fraction of each flash that is lit (0.4 = 2 of 5 frames at 10 Hz)
const uint8_t STROBE_LOW = 0;      // dimmer level between flashes (raise for a softer pulse)

volatile bool strobeReq = false;   // written by the mic loop, read by the dmx task
float strobePhase = 0;             // 0..1 position inside one flash cycle

// ======================================================
// DMX BREAK SIGNAL GENERATION
// ======================================================

void sendBreak() {
  Serial2.end();
  pinMode(TXD2, OUTPUT);

  digitalWrite(TXD2, LOW);  // BREAK
  delayMicroseconds(120);

  digitalWrite(TXD2, HIGH);  // MARK AFTER BREAK
  delayMicroseconds(12);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);
}

// ======================================================
// DMX TRANSMISSION
// ======================================================
// The RS485 driver stays enabled permanently (set in setup), so the
// bus never floats between frames.
// ======================================================

void sendDMXFrame() {
  sendBreak();
  Serial2.write(dmx, DMX_SEND_SLOTS + 1);  // start code + channels
  Serial2.flush();
}

// ======================================================
// MICROPHONE PROCESSING (unchanged)
// ======================================================

float rmsWindow(int n) {
  float sum = 0, sumSq = 0;
  for (int i = 0; i < n; i++) {
    float v = analogRead(MIC_PIN) - 2048;
    sum += v;
    sumSq += v * v;
    delayMicroseconds(20);
  }
  float mean = sum / n;
  return sqrt(max(sumSq / n - mean * mean, 1.0f));
}

float measureRMS() {
  float a = rmsWindow(64), b = rmsWindow(64), c = rmsWindow(64);
  return max(min(a, b), min(max(a, b), c));
}

float readDB(float k) {
  float rms = measureRMS();
  float p = rms * rms;
  float dt = 0.0235f * k;

  if (!dbInit) {
    powAvg = p;
    floorDb = 10.0 * log10(max(p, 1.0f));
    dbSmooth = QUIET_DB;
    dbInit = true;
  }

  powAvg += (p - powAvg) * (1.0 - powf(0.8, k));
  float raw = 10.0 * log10(max(powAvg, 1.0f));

  float d = raw - floorDb;
  if (d < 0) {
    floorDb -= fminf(-d, FLOOR_FALL_DB_PER_SEC * dt);
  } else {
    float riseDbPerSec = (d < 6) ? 0.10 : 0.01;
    floorDb += fminf(d, riseDbPerSec * dt);
  }

  float db = (raw - floorDb) * SENSITIVITY + QUIET_DB;

  if (db > dbSmooth) dbSmooth = db;
  else dbSmooth = fmaxf(db, dbSmooth - RELEASE_DB_PER_SEC * dt);

  return dbSmooth;
}

void calibrateMic() {
  float acc = 0;
  const int ROUNDS = 100;

  for (int i = 0; i < ROUNDS; i++) {
    float r = measureRMS();
    acc += r * r;
    delay(5);
  }

  powAvg = acc / ROUNDS;
  floorDb = 10.0 * log10(max(powAvg, 1.0f));
  dbSmooth = QUIET_DB;
  dbInit = true;

  Serial.printf("Mic calibrated: floor %.1f dB\n", floorDb);
}

void printRawStats() {
  int mn = 4095, mx = 0;
  long sum = 0;
  const int N = 400;
  for (int i = 0; i < N; i++) {
    int v = analogRead(MIC_PIN);
    mn = min(mn, v);
    mx = max(mx, v);
    sum += v;
    delayMicroseconds(20);
  }
  Serial.printf("RAW min %d  max %d  mean %ld\n", mn, mx, sum / N);
}

// ======================================================
// ZONE SELECTION (runs in loop, writes zoneReq only)
// ======================================================

void updateZone(float db) {
  int z = zoneReq;

  if (db >= ZONE_LO[z] - HYST && db < ZONE_HI[z] + HYST) return;

  if (db < ZONE_HI[0]) z = 0;
  else if (db < ZONE_HI[1]) z = 1;
  else if (db < ZONE_HI[2]) z = 2;
  else if (db < ZONE_HI[3]) z = 3;
  else z = 4;

  zoneReq = z;
}

// Strobe request with hysteresis: on at STROBE_DB, off below STROBE_DB - HYST,
// so it can't chatter right at the threshold.
void updateStrobe(float db) {
  if (!strobeReq && db >= STROBE_DB) strobeReq = true;
  else if (strobeReq && db < STROBE_DB - HYST) strobeReq = false;
}

// Adopts the requested zone, but no faster than ZONE_MIN_HOLD_MS.
void applyZone() {
  int req = zoneReq;
  if (req == zone) return;
  if (millis() - zoneSince < (unsigned long)ZONE_MIN_HOLD_MS) return;
  zone = req;
  zoneSince = millis();
}

// ======================================================
// LIGHTING CONTROL SYSTEM (runs in the dmx task)
// ======================================================

const uint8_t PALETTE[4][3] = {
  { 255, 255, 255 },  // zone 0: idle white
  { 255, 0, 0 },      // zone 1: red
  { 0, 255, 0 },      // zone 2: green
  { 0, 0, 255 }       // zone 3: blue
};                    // zone 4: RGB cycling

void hueToRGB(float h, float &r, float &g, float &b) {
  float x = h / TWO_PI_F * 6.0f;  // 0..6
  int i = (int)x;
  float f = x - i;
  float up = f * 255.0f;
  float down = (1.0f - f) * 255.0f;

  switch (i % 6) {
    case 0:  r = 255;  g = up;   b = 0;    break;
    case 1:  r = down; g = 255;  b = 0;    break;
    case 2:  r = 0;    g = 255;  b = up;   break;
    case 3:  r = 0;    g = down; b = 255;  break;
    case 4:  r = up;   g = 0;    b = 255;  break;
    default: r = 255;  g = 0;    b = down; break;
  }
}

void getTargetColor() {
  if (zone == 4) {
    hueToRGB(hue, r_t, g_t, b_t);  // RGB cycling
  } else {
    r_t = PALETTE[zone][0];
    g_t = PALETTE[zone][1];
    b_t = PALETTE[zone][2];
  }
}

void updateLighting(float dt) {

  hue += (TWO_PI_F / RGB_CYCLE_SECONDS) * dt;
  if (hue >= TWO_PI_F) hue -= TWO_PI_F;

  applyZone();
  getTargetColor();

  // frame-rate independent one-pole slew toward the target
  float a = 1.0f - expf(-dt / (COLOR_TAU_MS / 1000.0f));
  r_s += (r_t - r_s) * a;
  g_s += (g_t - g_s) * a;
  b_s += (b_t - b_s) * a;

  // strobe rides on the dimmer; the RGB colors above keep cycling untouched
  uint8_t dimOut = DIM_LEVEL;
  if (zone == 4 && strobeReq) {
    strobePhase += STROBE_HZ * dt;
    strobePhase -= floorf(strobePhase);  // wrap to 0..1
    if (strobePhase >= STROBE_DUTY) dimOut = STROBE_LOW;
  } else {
    strobePhase = 0;  // next burst starts on a lit frame
  }
  dmx[DIM_CH] = dimOut;
  dmx[RED_CH] = constrain((int)(r_s + 0.5f), 0, 255);
  dmx[GREEN_CH] = constrain((int)(g_s + 0.5f), 0, 255);
  dmx[BLUE_CH] = constrain((int)(b_s + 0.5f), 0, 255);

  // mirror to PAR
  dmx[PAR_RED] = dmx[RED_CH];
  dmx[PAR_GREEN] = dmx[GREEN_CH];
  dmx[PAR_BLUE] = dmx[BLUE_CH];
  dmx[PAR_DIM] = dmx[DIM_CH];
}

// ======================================================
// MOVEMENT SYSTEM (runs in the dmx task)
// ======================================================

// Original function, verbatim.
void updateMovement(float k) {

  float panTarget = panCenter + sin(t) * panAmp;
  float tiltTarget = tiltCenter + sin(2 * t) * tiltAmp;

  float a = 1.0 - powf(1.0 - smoothFactor, k);
  pan_s += (panTarget - pan_s) * a;
  tilt_s += (tiltTarget - tilt_s) * a;

  dmx[PAN_CH] = constrain((int)pan_s, 0, 255);
  dmx[TILT_CH] = constrain((int)tilt_s, 0, 255);

  t += 0.05 * k;  // motion speed control
}

// ======================================================
// DMX TASK
// ======================================================
// Lighting, movement and DMX output run on a fixed 50 Hz clock,
// independent of how long the mic loop blocks. Same core as loop().
// Frame work is ~1.6 ms, so the mic still gets ~92% of the core.
// ======================================================

void dmxTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t lastUs = micros();

  for (;;) {
    uint32_t nowUs = micros();
    float dt = (nowUs - lastUs) / 1e6f;
    lastUs = nowUs;
    if (dt > 0.1f) dt = 0.1f;  // clamp after any stall

    dmx[0] = 0;  // start code

    updateLighting(dt);
    updateMovement(dt / 0.0235f);  // same k scale the original used

    sendDMXFrame();

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DMX_FRAME_MS));
  }
}

// ======================================================
// SYSTEM SETUP
// ======================================================

void setup() {

  Serial.begin(115200);

  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, HIGH);  // RS485 driver on permanently

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);
  analogReadResolution(12);

  for (int i = 0; i <= DMX_CHANNELS; i++) {
    dmx[i] = 0;
  }

  // start DMX first: fixtures get a signal during the whole boot
  xTaskCreatePinnedToCore(dmxTask, "dmx", 6144, NULL, 2, NULL, 1);

  delay(1200);
  calibrateMic();  // keep the room quiet for these ~1.5 s
}

// ======================================================
// MAIN LOOP (mic only)
// ======================================================

void loop() {

  static unsigned long lastLoop = micros();
  unsigned long now = micros();
  float k = constrain((now - lastLoop) / 1e6f / 0.0235f, 0.2f, 3.0f);
  lastLoop = now;

  float db = readDB(k);
  updateZone(db);
  updateStrobe(db);

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 250) {
    lastPrint = millis();
    Serial.printf("dB: %.1f  floor: %.1f  zoneReq: %d  strobe: %d\n", db, floorDb, (int)zoneReq, (int)strobeReq);
  }

  static unsigned long lastRaw = 0;
  if (millis() - lastRaw > 5000) {
    lastRaw = millis();
    printRawStats();
  }
}
