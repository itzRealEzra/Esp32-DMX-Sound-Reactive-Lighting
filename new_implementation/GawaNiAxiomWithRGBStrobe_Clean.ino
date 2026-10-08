/*
 * ============================================================================
 *  GawaNiAxiom - Sound-Reactive DMX Lighting (ESP32)
 * ============================================================================
 *
 *  WHAT IT DOES
 *    Listens to a MAX9814 microphone, estimates a relative sound level (dB
 *    above the room's noise floor), and uses that level to drive:
 *      - an RGB PAR fixture
 *      - a moving head (pan/tilt figure-8 + RGB color)
 *    Louder sound moves the color through zones; at the loudest level the
 *    colors cycle through the rainbow and the dimmer strobes on top.
 *
 *  LEVEL -> BEHAVIOR (levels are relative, see "LEVEL DETECTION")
 *      below 30 dB    zone 0   white (idle)
 *      30 .. 32 dB    zone 1   red
 *      32 .. 34 dB    zone 2   green
 *      34 .. 36 dB    zone 3   blue
 *      36 dB and up   zone 4   RGB cycling
 *      37 dB and up   strobe flashes on top of zone 4
 *
 *  HARDWARE
 *      GPIO 17 (TXD2)  -> RS485 transceiver DI   (DMX data out)
 *      GPIO 16 (RXD2)  -> RS485 transceiver RO   (not used for receiving)
 *      GPIO 21 (EN)    -> RS485 transceiver DE/RE (held HIGH = always transmit)
 *      GPIO 34 (MIC)   <- MAX9814 output (ADC1, 12-bit)
 *
 *  ARCHITECTURE - TWO THREADS OF EXECUTION, ONE CORE
 *    dmxTask (priority 2)  Fixed 50 Hz clock. Owns ALL light state: zone,
 *                          colors, strobe, movement, and the DMX buffer.
 *    loop()  (priority 1)  Reads the mic (it blocks for several ms per
 *                          pass) and publishes two requests: zoneReq and
 *                          strobeReq.
 *
 *    The mic loop never touches the lights directly. It only writes the two
 *    volatile request flags; the DMX task reads them. Each flag has exactly
 *    one writer, so no mutex is needed, and a slow mic read can never make
 *    the lights stutter.
 *
 *  NO WI-FI / NO OTA
 *    This build has no radio code. Update firmware over USB.
 * ============================================================================
 */

#include <Arduino.h>
#include <math.h>


// ============================================================================
//  CONFIGURATION
// ============================================================================

// ---- Hardware pins ---------------------------------------------------------
#define TXD2     17   // UART2 TX  -> RS485 DI
#define RXD2     16   // UART2 RX  (passed to begin(); unused for DMX output)
#define EN_PIN   21   // RS485 DE/RE, held HIGH so the bus never floats
#define MIC_PIN  34   // MAX9814 analog out (ADC1, safe to use any time)

// ---- DMX output ------------------------------------------------------------
// Only the first DMX_SEND_SLOTS channels are transmitted. A full 512-slot
// frame takes ~22.7 ms; 33 bytes (start code + 32 slots) at 44 us/byte takes
// ~1.5 ms. If a fixture misbehaves with short frames, raise DMX_SEND_SLOTS.
#define DMX_CHANNELS    512   // size of the buffer (slots 1..512)
#define DMX_SEND_SLOTS  32    // slots actually sent each frame (must cover the highest channel used)
#define DMX_FRAME_MS    20    // frame period: 50 Hz, paced by dmxTask

// ---- Fixture channel maps --------------------------------------------------
// RGB PAR
#define PAR_DIM    1
#define PAR_RED    2
#define PAR_GREEN  3
#define PAR_BLUE   4

// Moving head
#define PAN_CH    17
#define TILT_CH   19
#define DIM_CH    22
#define RED_CH    23
#define GREEN_CH  24
#define BLUE_CH   25

// ---- Movement: infinite figure-8 ------------------------------------------
// Pan follows sin(t), tilt follows sin(2t); that 1:2 ratio traces a figure-8.
// Motion is independent of the audio. Values are the original ones.
//
// NOTE: tiltCenter + tiltAmp = 320 exceeds the DMX maximum of 255, so tilt
// sits pinned at 255 for roughly 44% of each loop (the top of the 8 is
// flattened). Kept as-is to preserve the original look; lower tiltCenter
// (e.g. 175) for the full, un-clipped figure.
const float smoothFactor = 0.15f;  // 0..1, higher = snappier follow
const int   panCenter    = 127;    // neutral pan position
const int   tiltCenter   = 240;    // neutral tilt position
const int   panAmp       = 100;    // pan swing, +/- DMX units
const int   tiltAmp      = 80;     // tilt swing, +/- DMX units
const float MOVE_SPEED   = 0.05f;  // radians of t per reference step (see REF_STEP_S)

// ---- Level detection (relative dB) ----------------------------------------
// "dB" here means: level above the learned room floor, plus QUIET_DB. It is
// NOT a calibrated SPL measurement; it is only meant for driving lights.
const float QUIET_DB             = 25.0f;   // value reported in a silent room
const float SENSITIVITY          = 1.0f;    // scales dB above the floor
const float HYST                 = 1.0f;    // dB of hysteresis at every threshold
const float RELEASE_DB_PER_SEC   = 10.0f;   // how fast the displayed level may fall
const float FLOOR_FALL_DB_PER_SEC = 1.0f;   // noise floor follows quieter rooms down at this rate

// Zone boundaries in dB. Zone z is active for ZONE_LO[z] <= db < ZONE_HI[z].
//   zone:           0     1    2    3    4
const float ZONE_LO[5] = { -1000, 30, 32, 34,   36 };
const float ZONE_HI[5] = {    30, 32, 34, 36, 1000 };

// ---- Color animation -------------------------------------------------------
// Shown color chases its target with a one-pole slew. Retargeting mid-flight
// never restarts anything, so there is no velocity jump and no pulse.
const float   COLOR_TAU_MS      = 90.0f;   // slew time constant: lower = snappier
const float   ZONE_MIN_HOLD_MS  = 200.0f;  // min time between zone changes (~5 changes/s max)
const float   RGB_CYCLE_SECONDS = 3.0f;    // one full rainbow loop in zone 4
const uint8_t DIM_LEVEL         = 255;     // steady dimmer level (strobe flashes below it)

// Colors for zones 0..3. Zone 4 is generated by the rainbow cycle instead.
const uint8_t PALETTE[4][3] = {
  { 255, 255, 255 },  // zone 0: idle white
  { 255,   0,   0 },  // zone 1: red
  {   0, 255,   0 },  // zone 2: green
  {   0,   0, 255 }   // zone 3: blue
};

// ---- Strobe ----------------------------------------------------------------
// Active only while zone 4 is showing. The colors keep cycling; only the
// dimmer flashes. The DMX task runs at 50 Hz, so rates that divide 50 evenly
// stay crisp: 5, 6.25, 8.33, 10, 12.5, 16.67, 25 Hz.
//
// SAFETY: flash rates of roughly 3-30 Hz can trigger seizures in
// photosensitive people. For any public use, add a way to disable the strobe
// and warn the audience.
const float   STROBE_DB   = 37.0f;  // turns on at this level, off below STROBE_DB - HYST
const float   STROBE_HZ   = 10.0f;  // flashes per second
const float   STROBE_DUTY = 0.4f;   // lit fraction of each flash (0.4 = 2 of 5 frames at 10 Hz)
const uint8_t STROBE_LOW  = 0;      // dimmer level between flashes (raise for a softer pulse)


// ============================================================================
//  SHARED CONSTANTS
// ============================================================================

const float TWO_PI_F = 6.2831853f;

// The original code was tuned with a ~23.5 ms loop, and several rates are
// expressed as "per reference step". Time deltas are divided by this value
// to get a unitless step count k (k = 1 means one reference step elapsed).
const float REF_STEP_S = 0.0235f;


// ============================================================================
//  STATE
// ============================================================================

// ---- DMX buffer: [0] = start code, [1..512] = channels (dmxTask only) -----
uint8_t dmx[DMX_CHANNELS + 1];

// ---- Cross-thread requests (each has ONE writer) --------------------------
volatile int  zoneReq   = 0;      // written by loop():    zone the mic wants
volatile bool strobeReq = false;  // written by loop():    strobe the mic wants

// ---- Level detection state (loop() only) ----------------------------------
float floorDb  = 0;      // learned noise floor, in raw dB
float dbSmooth = 0;      // displayed level with fast attack / slow release
float powAvg   = 0;      // smoothed signal power
bool  dbInit   = false;  // true once the detector is seeded

// ---- Lighting state (dmxTask only) ----------------------------------------
int           zone      = 0;   // zone shown on the lights (follows zoneReq)
unsigned long zoneSince = 0;   // millis() of the last zone change
float r_s = 255, g_s = 255, b_s = 255;   // color currently shown
float r_t = 255, g_t = 255, b_t = 255;   // color being chased
float hue = 0;                           // rainbow position, radians 0..2*pi
float strobePhase = 0;                   // position inside one flash, 0..1

// ---- Movement state (dmxTask only) ----------------------------------------
float t      = 0;     // oscillator time base, radians
float pan_s  = 127;   // smoothed pan position
float tilt_s = 127;   // smoothed tilt position


// ============================================================================
//  DMX OUTPUT
// ============================================================================

// Generates the DMX BREAK and MARK-AFTER-BREAK by taking the TX pin away from
// the UART and bit-banging it, then restarts the UART for the data bytes.
//   BREAK 120 us  (spec minimum 88 us)
//   MAB    12 us  (spec minimum  8 us)
// Serial2 is torn down and rebuilt every frame; this works but reinstalls the
// UART driver 50 times a second. A hardware break (e.g. the esp_dmx library)
// would be more robust for very long uptimes.
void sendBreak() {
  Serial2.end();
  pinMode(TXD2, OUTPUT);

  digitalWrite(TXD2, LOW);   // BREAK
  delayMicroseconds(120);

  digitalWrite(TXD2, HIGH);  // MARK AFTER BREAK
  delayMicroseconds(12);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);  // DMX512: 250 kbaud, 8 data bits, 2 stop bits
}

// Sends one complete frame: break, start code, then DMX_SEND_SLOTS channels.
// The RS485 driver is enabled permanently (see setup), so the bus never
// floats between frames.
void sendDMXFrame() {
  sendBreak();
  Serial2.write(dmx, DMX_SEND_SLOTS + 1);  // start code + channels
  Serial2.flush();                          // wait until the last byte has left
}


// ============================================================================
//  LEVEL DETECTION  (runs in loop())
// ============================================================================
//
//  analogRead -> RMS (median of 3 windows) -> smoothed power -> dB
//    -> subtract adaptive noise floor -> add QUIET_DB -> fast-attack /
//       slow-release smoothing -> dbSmooth
//
//  The noise floor falls quickly when the room gets quieter, but rises very
//  slowly, so sustained loud music does not drag the baseline up with it.
// ============================================================================

// RMS of n ADC samples with the DC offset (mean) removed.
// Takes about n * (ADC read + 20 us), so a window is only a few milliseconds
// long: low-frequency energy (e.g. kick drums) is under-represented.
float rmsWindow(int n) {
  float sum = 0, sumSq = 0;

  for (int i = 0; i < n; i++) {
    float v = analogRead(MIC_PIN) - 2048;  // centered on mid-scale of a 12-bit ADC
    sum   += v;
    sumSq += v * v;
    delayMicroseconds(20);
  }

  float mean = sum / n;
  return sqrt(max(sumSq / n - mean * mean, 1.0f));  // variance, floored to avoid log(0)
}

// Median of three RMS windows: rejects single-window clicks and pops.
float measureRMS() {
  float a = rmsWindow(64), b = rmsWindow(64), c = rmsWindow(64);
  return max(min(a, b), min(max(a, b), c));
}

// Returns the current relative level in dB. k = elapsed time in reference steps.
float readDB(float k) {
  float rms = measureRMS();
  float p   = rms * rms;                 // instantaneous power
  float dt  = REF_STEP_S * k;            // elapsed seconds

  // Lazy seed in case calibrateMic() was skipped.
  if (!dbInit) {
    powAvg   = p;
    floorDb  = 10.0f * log10(max(p, 1.0f));
    dbSmooth = QUIET_DB;
    dbInit   = true;
  }

  // Smooth the power (time-constant-correct for any k).
  powAvg += (p - powAvg) * (1.0f - powf(0.8f, k));
  float raw = 10.0f * log10(max(powAvg, 1.0f));

  // Adaptive floor: down fast, up slow (faster while close to the floor).
  float d = raw - floorDb;
  if (d < 0) {
    floorDb -= fminf(-d, FLOOR_FALL_DB_PER_SEC * dt);
  } else {
    float riseDbPerSec = (d < 6) ? 0.10f : 0.01f;
    floorDb += fminf(d, riseDbPerSec * dt);
  }

  float db = (raw - floorDb) * SENSITIVITY + QUIET_DB;

  // Fast attack, slow release.
  if (db > dbSmooth) dbSmooth = db;
  else               dbSmooth = fmaxf(db, dbSmooth - RELEASE_DB_PER_SEC * dt);

  return dbSmooth;
}

// Measures the room for ~1.5 s to seed the noise floor. Keep the room quiet.
void calibrateMic() {
  const int ROUNDS = 100;
  float acc = 0;

  for (int i = 0; i < ROUNDS; i++) {
    float r = measureRMS();
    acc += r * r;
    delay(5);
  }

  powAvg   = acc / ROUNDS;
  floorDb  = 10.0f * log10(max(powAvg, 1.0f));
  dbSmooth = QUIET_DB;
  dbInit   = true;

  Serial.printf("Mic calibrated: floor %.1f dB\n", floorDb);
}

// Debug helper: prints min / max / mean of 400 raw ADC samples.
// Use it to confirm the mic is biased near mid-scale and not clipping.
void printRawStats() {
  const int N = 400;
  int  mn = 4095, mx = 0;
  long sum = 0;

  for (int i = 0; i < N; i++) {
    int v = analogRead(MIC_PIN);
    mn = min(mn, v);
    mx = max(mx, v);
    sum += v;
    delayMicroseconds(20);
  }

  Serial.printf("RAW min %d  max %d  mean %ld\n", mn, mx, sum / N);
}


// ============================================================================
//  REQUESTS FROM THE MIC  (run in loop(); write zoneReq / strobeReq only)
// ============================================================================

// Picks the zone for a level. Stays in the current zone while the level is
// inside that zone's range widened by HYST, so it cannot chatter at a border.
void updateZone(float db) {
  int z = zoneReq;

  if (db >= ZONE_LO[z] - HYST && db < ZONE_HI[z] + HYST) return;  // still inside current zone

  if      (db < ZONE_HI[0]) z = 0;
  else if (db < ZONE_HI[1]) z = 1;
  else if (db < ZONE_HI[2]) z = 2;
  else if (db < ZONE_HI[3]) z = 3;
  else                      z = 4;

  zoneReq = z;
}

// Strobe request with hysteresis: on at STROBE_DB, off below STROBE_DB - HYST.
void updateStrobe(float db) {
  if      (!strobeReq && db >= STROBE_DB)        strobeReq = true;
  else if ( strobeReq && db <  STROBE_DB - HYST) strobeReq = false;
}


// ============================================================================
//  LIGHTING  (runs in dmxTask)
// ============================================================================

// Adopts the requested zone, but no faster than ZONE_MIN_HOLD_MS.
void applyZone() {
  int req = zoneReq;

  if (req == zone) return;
  if (millis() - zoneSince < (unsigned long)ZONE_MIN_HOLD_MS) return;

  zone      = req;
  zoneSince = millis();
}

// Fully saturated rainbow. h is in radians, 0..2*pi; outputs are 0..255.
void hueToRGB(float h, float &r, float &g, float &b) {
  float x = h / TWO_PI_F * 6.0f;   // 0..6, six color sectors
  int   i = (int)x;
  float f = x - i;                 // position inside the sector
  float up   = f * 255.0f;
  float down = (1.0f - f) * 255.0f;

  switch (i % 6) {
    case 0:  r = 255;  g = up;   b = 0;    break;  // red     -> yellow
    case 1:  r = down; g = 255;  b = 0;    break;  // yellow  -> green
    case 2:  r = 0;    g = 255;  b = up;   break;  // green   -> cyan
    case 3:  r = 0;    g = down; b = 255;  break;  // cyan    -> blue
    case 4:  r = up;   g = 0;    b = 255;  break;  // blue    -> magenta
    default: r = 255;  g = 0;    b = down; break;  // magenta -> red
  }
}

// Sets the target color for the current zone.
void getTargetColor() {
  if (zone == 4) {
    hueToRGB(hue, r_t, g_t, b_t);        // rainbow cycling
  } else {
    r_t = PALETTE[zone][0];
    g_t = PALETTE[zone][1];
    b_t = PALETTE[zone][2];
  }
}

// One lighting frame. dt = seconds since the previous frame.
void updateLighting(float dt) {

  // Advance the rainbow (runs continuously, even when not displayed).
  hue += (TWO_PI_F / RGB_CYCLE_SECONDS) * dt;
  if (hue >= TWO_PI_F) hue -= TWO_PI_F;

  applyZone();
  getTargetColor();

  // Frame-rate-independent one-pole slew toward the target color.
  float a = 1.0f - expf(-dt / (COLOR_TAU_MS / 1000.0f));
  r_s += (r_t - r_s) * a;
  g_s += (g_t - g_s) * a;
  b_s += (b_t - b_s) * a;

  // Strobe rides on the dimmer only; the colors above keep cycling untouched.
  uint8_t dimOut = DIM_LEVEL;
  if (zone == 4 && strobeReq) {
    strobePhase += STROBE_HZ * dt;
    strobePhase -= floorf(strobePhase);              // wrap to 0..1
    if (strobePhase >= STROBE_DUTY) dimOut = STROBE_LOW;
  } else {
    strobePhase = 0;   // next burst starts on a lit frame
  }

  // Moving head
  dmx[DIM_CH]   = dimOut;
  dmx[RED_CH]   = constrain((int)(r_s + 0.5f), 0, 255);
  dmx[GREEN_CH] = constrain((int)(g_s + 0.5f), 0, 255);
  dmx[BLUE_CH]  = constrain((int)(b_s + 0.5f), 0, 255);

  // PAR mirrors the moving head
  dmx[PAR_RED]   = dmx[RED_CH];
  dmx[PAR_GREEN] = dmx[GREEN_CH];
  dmx[PAR_BLUE]  = dmx[BLUE_CH];
  dmx[PAR_DIM]   = dmx[DIM_CH];
}


// ============================================================================
//  MOVEMENT  (runs in dmxTask)
// ============================================================================

// One movement frame. k = elapsed time in reference steps (see REF_STEP_S).
void updateMovement(float k) {

  // Figure-8 targets: pan at frequency 1, tilt at frequency 2.
  float panTarget  = panCenter  + sin(t)     * panAmp;
  float tiltTarget = tiltCenter + sin(2 * t) * tiltAmp;

  // Smooth follow, corrected for the actual elapsed time.
  float a = 1.0f - powf(1.0f - smoothFactor, k);
  pan_s  += (panTarget  - pan_s)  * a;
  tilt_s += (tiltTarget - tilt_s) * a;

  dmx[PAN_CH]  = constrain((int)pan_s,  0, 255);
  dmx[TILT_CH] = constrain((int)tilt_s, 0, 255);

  t += MOVE_SPEED * k;   // advance the oscillator
}


// ============================================================================
//  DMX TASK  (50 Hz, pinned to core 1)
// ============================================================================
//
//  Lighting, movement and DMX output all run on a fixed clock, independent of
//  how long the mic loop blocks. Same core as loop(). A frame costs about
//  1.6 ms of CPU, so the mic still gets ~92% of the core.
// ============================================================================

void dmxTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t   lastUs   = micros();

  for (;;) {
    // Measured frame time; clamped so a stall cannot cause a visual jump.
    uint32_t nowUs = micros();
    float dt = (nowUs - lastUs) / 1e6f;
    lastUs = nowUs;
    if (dt > 0.1f) dt = 0.1f;

    dmx[0] = 0;                            // start code: standard dimmer data

    updateLighting(dt);
    updateMovement(dt / REF_STEP_S);       // movement is tuned in reference steps

    sendDMXFrame();

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DMX_FRAME_MS));   // fixed 50 Hz
  }
}


// ============================================================================
//  SETUP / LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);

  // RS485 driver on permanently so the bus never floats between frames.
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, HIGH);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);
  analogReadResolution(12);

  for (int i = 0; i <= DMX_CHANNELS; i++) dmx[i] = 0;

  // Start DMX first so fixtures get a signal during the whole boot.
  xTaskCreatePinnedToCore(dmxTask, "dmx", 6144, NULL, 2, NULL, 1);

  delay(1200);       // settle time before measuring
  calibrateMic();    // keep the room quiet for these ~1.5 s
}

// Mic loop: measure, publish requests, print debug output.
// It never touches the lights; dmxTask picks the requests up on its own clock.
void loop() {
  // Elapsed time since the previous pass, in reference steps (clamped).
  static unsigned long lastLoop = micros();
  unsigned long now = micros();
  float k = constrain((now - lastLoop) / 1e6f / REF_STEP_S, 0.2f, 3.0f);
  lastLoop = now;

  float db = readDB(k);
  updateZone(db);
  updateStrobe(db);

  // Status line, 4 times a second.
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 250) {
    lastPrint = millis();
    Serial.printf("dB: %.1f  floor: %.1f  zoneReq: %d  strobe: %d\n",
                  db, floorDb, (int)zoneReq, (int)strobeReq);
  }

  // Raw ADC sanity check, every 5 seconds.
  static unsigned long lastRaw = 0;
  if (millis() - lastRaw > 5000) {
    lastRaw = millis();
    printRawStats();
  }
}
