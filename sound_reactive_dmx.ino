#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

// ======================================================
// MAX9814 WIRING
// ======================================================
//   VDD  -> ESP32 3.3V
//   GND  -> ESP32 GND
//   OUT  -> GPIO34 (ADC1)
//   GAIN -> VDD  (40 dB max gain; GND = 50 dB, floating = 60 dB)
//   A/R  -> GND  (fastest release, about 1.2 s)
// ======================================================

#define TXD2    17
#define RXD2    16
#define EN_PIN  21
#define MIC_PIN 34

// ======================================================
// DMX
// ======================================================
#define DMX_CHANNELS 512
uint8_t dmx[DMX_CHANNELS + 1];

// PAR (RGB, no dimmer channel: brightness is done by scaling RGB)
#define PAR_RED    1
#define PAR_GREEN  2
#define PAR_BLUE   3

// Moving head
#define PAN_CH     17
#define TILT_CH    19
#define DIM_CH     22
#define RED_CH     23
#define GREEN_CH   24
#define BLUE_CH    25

// ======================================================
// FIXTURE BEHAVIOR (per fixture, edit freely)
// ======================================================
// Color always follows the dB zone. These flags add level-reactive extras.
const bool  PAR_LEVEL_BRIGHTNESS  = true;   // PAR pulses with the sound inside a zone
const bool  HEAD_LEVEL_BRIGHTNESS = false;  // head dimmer stays at full
const bool  HEAD_MOVE_REACTIVE    = true;   // figure-8 speeds up with loudness
const float BRIGHT_MIN            = 0.40f;  // brightness floor when reactive (0..1)

// Level scale used for brightness and movement speed (dB SPL -> 0..1)
const float LEVEL_LO_DB = 50.0f;
const float LEVEL_HI_DB = 100.0f;

// ======================================================
// dB ZONES
// ======================================================
// zone 0 = idle (pure white), 1 = RED (60+), 2 = BLUE (80+), 3 = GREEN (100+)
const float ZONE_DB[3] = {60.0f, 80.0f, 100.0f};
const float HYST_DB    = 3.0f;
const int   soundHold  = 350;

const uint8_t ZONE_RGB[4][3] = {
  {255, 255, 255},   // idle
  {255,   0,   0},   // red
  {  0,   0, 255},   // blue
  {  0, 255,   0}    // green
};

// ======================================================
// MOVEMENT (figure-8)
// ======================================================
float t = 0;
float pan_s = 127, tilt_s = 127;
float smoothFactor = 0.15;

int panCenter  = 127;
int panAmp     = 100;   // 27..227
int tiltCenter = 170;
int tiltAmp    = 80;    // 90..250

// ======================================================
// SPL MEASUREMENT (micTask, core 0)
// ======================================================
#define MIC_WINDOW_SAMPLES 64
#define MIC_SAMPLE_GAP_US  80

Preferences prefs;

volatile float g_rawDb   = 0;
volatile float g_rawSlow = 0;
volatile float g_offset  = 0;
volatile bool  g_clip    = false;

void micTask(void *) {
  float env = 1.0f, slow = 0.0f;
  for (;;) {
    int mn = 4095, mx = 0;
    for (int i = 0; i < MIC_WINDOW_SAMPLES; i++) {
      int v = analogRead(MIC_PIN);
      if (v < mn) mn = v;
      if (v > mx) mx = v;
      delayMicroseconds(MIC_SAMPLE_GAP_US);
    }

    g_clip = (mx >= 4000 || mn <= 95);

    float amp = (float)(mx - mn);
    if (amp < 1.0f) amp = 1.0f;

    float a = (amp > env) ? 0.30f : 0.05f;
    env += (amp - env) * a;

    float raw = 20.0f * log10f(env);
    slow += (raw - slow) * 0.01f;

    g_rawDb   = raw;
    g_rawSlow = slow;
    vTaskDelay(1);
  }
}

int zoneFor(float spl, int cur) {
  int z = 0;
  if      (spl >= ZONE_DB[2]) z = 3;
  else if (spl >= ZONE_DB[1]) z = 2;
  else if (spl >= ZONE_DB[0]) z = 1;

  if (z < cur && spl >= ZONE_DB[cur - 1] - HYST_DB) return cur;
  return z;
}

float levelFromSpl(float spl) {
  return constrain((spl - LEVEL_LO_DB) / (LEVEL_HI_DB - LEVEL_LO_DB), 0.0f, 1.0f);
}

// ======================================================
// FIXTURE OUTPUT
// ======================================================
float r_s = 255, g_s = 255, b_s = 255;   // shared smoothed zone color
float parBri_s = 1.0f, headBri_s = 1.0f; // smoothed brightness per fixture

void updateColor(int zone) {
  const float smooth = 0.25f;
  r_s += (ZONE_RGB[zone][0] - r_s) * smooth;
  g_s += (ZONE_RGB[zone][1] - g_s) * smooth;
  b_s += (ZONE_RGB[zone][2] - b_s) * smooth;
}

float targetBrightness(bool reactive, int zone, float level) {
  if (!reactive || zone == 0) return 1.0f;       // idle white stays full
  return BRIGHT_MIN + (1.0f - BRIGHT_MIN) * level;
}

void applyPar(int zone, float level) {
  float target = targetBrightness(PAR_LEVEL_BRIGHTNESS, zone, level);
  parBri_s += (target - parBri_s) * 0.35f;

  dmx[PAR_RED]   = constrain((int)(r_s * parBri_s), 0, 255);
  dmx[PAR_GREEN] = constrain((int)(g_s * parBri_s), 0, 255);
  dmx[PAR_BLUE]  = constrain((int)(b_s * parBri_s), 0, 255);
}

void applyHead(int zone, float level) {
  float target = targetBrightness(HEAD_LEVEL_BRIGHTNESS, zone, level);
  headBri_s += (target - headBri_s) * 0.35f;

  dmx[DIM_CH]   = constrain((int)(255 * headBri_s), 0, 255);
  dmx[RED_CH]   = constrain((int)r_s, 0, 255);
  dmx[GREEN_CH] = constrain((int)g_s, 0, 255);
  dmx[BLUE_CH]  = constrain((int)b_s, 0, 255);

  float panTarget  = panCenter  + sin(t)     * panAmp;
  float tiltTarget = tiltCenter + sin(2 * t) * tiltAmp;

  pan_s  += (panTarget  - pan_s)  * smoothFactor;
  tilt_s += (tiltTarget - tilt_s) * smoothFactor;

  dmx[PAN_CH]  = constrain((int)pan_s,  0, 255);
  dmx[TILT_CH] = constrain((int)tilt_s, 0, 255);

  float speed = HEAD_MOVE_REACTIVE ? (0.03f + 0.06f * level) : 0.05f;
  t += speed;
  if (t > 6.28318f) t -= 6.28318f;
}

// ======================================================
// DMX OUTPUT (baud-switch BREAK: 100us break, 22us MAB)
// ======================================================
void sendDMX() {
  digitalWrite(EN_PIN, HIGH);

  Serial2.updateBaudRate(90000);
  Serial2.write((uint8_t)0x00);
  Serial2.flush();

  Serial2.updateBaudRate(250000);
  Serial2.write(dmx, DMX_CHANNELS + 1);
  Serial2.flush();

  digitalWrite(EN_PIN, LOW);
}

// ======================================================
// SERIAL CALIBRATION (115200 baud)
//   cal 82.5 | reset | show
// ======================================================
void handleCmd(String s) {
  s.trim();
  if (s.startsWith("cal ")) {
    float ref = s.substring(4).toFloat();
    if (ref < 30.0f || ref > 130.0f) {
      Serial.println("cal: give a reference level between 30 and 130 dB");
      return;
    }
    g_offset = ref - g_rawSlow;
    prefs.putFloat("off", g_offset);
    Serial.printf("calibrated: offset %.2f dB (ref %.1f, raw %.1f)\n",
                  (float)g_offset, ref, (float)g_rawSlow);
  } else if (s == "reset") {
    g_offset = 0;
    prefs.putFloat("off", 0);
    Serial.println("offset reset to 0");
  } else if (s == "show") {
    Serial.printf("offset %.2f dB\n", (float)g_offset);
  } else {
    Serial.println("commands: cal <dB> | reset | show");
  }
}

void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) handleCmd(line);
      line = "";
    } else if (line.length() < 32) {
      line += c;
    }
  }
}

// ======================================================
// SETUP / LOOP
// ======================================================
void setup() {
  Serial.begin(115200);

  prefs.begin("spl", false);
  g_offset = prefs.getFloat("off", 0.0f);

  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  memset(dmx, 0, sizeof(dmx));

  delay(1200);
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 1, nullptr, 0);
}

int zone = 0, lastZone = 0;
unsigned long lastSoundTime = 0;

void loop() {
  dmx[0] = 0;
  pollSerial();

  float spl   = g_rawDb + g_offset;
  float level = levelFromSpl(spl);
  zone = zoneFor(spl, zone);

  if (zone > 0) {
    lastZone = zone;
    lastSoundTime = millis();
  }
  int active = zone;
  if (zone == 0 && (millis() - lastSoundTime) < soundHold) active = lastZone;

  updateColor(active);
  applyPar(active, level);
  applyHead(active, level);
  sendDMX();

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 200) {
    lastPrint = millis();
    Serial.printf("SPL %.1f dB  level %.2f  zone %d  %s\n",
                  spl, level, active, g_clip ? "CLIP" : "ok");
  }
}
