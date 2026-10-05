#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

// ======================================================
// WIFI / OTA CONFIGURATION
// ======================================================
// Credentials live in secrets.h (gitignored). Copy secrets.h.example
// to secrets.h and fill it in. Never commit secrets.h.
#include "secrets.h"

const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* OTA_HOSTNAME  = SECRET_OTA_HOSTNAME;
const char* OTA_PASSWORD  = SECRET_OTA_PASSWORD;

bool otaStarted = false;

// ======================================================
// HARDWARE PIN CONFIGURATION
// ======================================================
#define TXD2    17
#define RXD2    16
#define EN_PIN  21
#define MIC_PIN 34

// ======================================================
// DMX CONFIGURATION
// ======================================================
// Full universe buffer is kept (512 + start code), but only
// DMX_SLOTS are transmitted. Highest used channel is 25, so
// 32 slots (start code + ch1..31) shortens a frame from
// ~22.7 ms to ~1.7 ms and frees the loop for audio sampling.
// ======================================================
#define DMX_CHANNELS 512
#define DMX_SLOTS    32
#define TICK_MS      20      // 50 Hz fixed update tick

uint8_t dmx[DMX_CHANNELS + 1];

// ======================================================
// PAR LIGHT CHANNEL MAPPING (RGB FIXTURE)
// ======================================================
#define PAR_RED    2
#define PAR_GREEN  3
#define PAR_BLUE   4
#define PAR_DIM    1

// ======================================================
// MOVING HEAD CHANNEL MAPPING
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
float t = 0;                // time base, wrapped at TWO_PI

float pan_s = 127;
float tilt_s = 127;

float smoothFactor = 0.15f;

int panCenter = 127;
int tiltCenter = 175;       // 175 +/- 80 -> 95..255, no clipping

int panAmp = 100;           // 27..227
int tiltAmp = 80;

// ======================================================
// AUDIO PROCESSING (MAX9814 MICROPHONE)
// ======================================================
float envelope = 0;
float peak = 0;
float noiseFloor = 20;

unsigned long lastSoundTime = 0;
const unsigned long soundHold = 350;

// ======================================================
// COLOR STATE SYSTEM
// ======================================================
float r_s = 255, g_s = 255, b_s = 255;
float r_t = 255, g_t = 255, b_t = 255;

float hue = 0;

// ======================================================
// DMX BREAK SIGNAL GENERATION
// ======================================================
void sendBreak() {
  Serial2.end();
  pinMode(TXD2, OUTPUT);

  digitalWrite(TXD2, LOW);    // BREAK (>= 88 us)
  delayMicroseconds(120);

  digitalWrite(TXD2, HIGH);   // MARK AFTER BREAK (>= 8 us)
  delayMicroseconds(12);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);
}

// ======================================================
// DMX TRANSMISSION
// ======================================================
// Sends one short frame. Pacing is done by the tick gate in loop().
// ======================================================
void sendDMXFrame() {
  digitalWrite(EN_PIN, HIGH);

  sendBreak();
  Serial2.write(dmx, DMX_SLOTS);
  Serial2.flush();

  digitalWrite(EN_PIN, LOW);
}

// ======================================================
// OTA SETUP
// ======================================================
void setupOTA() {

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    dmx[DIM_CH] = 0;
    dmx[PAR_DIM] = 0;
    sendDMXFrame();
    Serial.println("OTA start");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA done");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error %u\n", (unsigned)error);
  });
}

// Starts OTA once WiFi is up, then keeps it serviced.
// Called every pass of loop(). Never block loop() for long.
void handleOTA() {

  if (WiFi.status() != WL_CONNECTED) return;

  if (!otaStarted) {
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.print("OTA ready, IP: ");
    Serial.println(WiFi.localIP());
  }

  ArduinoOTA.handle();
}

// ======================================================
// MICROPHONE SAMPLING
// ======================================================
// Burst-samples the ADC and returns half of the peak-to-peak
// swing. No DC-center assumption, so the MAX9814 bias point
// does not matter.
// ======================================================
float sampleAmp() {
  int lo = 4095;
  int hi = 0;

  for (int i = 0; i < 128; i++) {
    int v = analogRead(MIC_PIN);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }

  return (hi - lo) * 0.5f;
}

// ======================================================
// MICROPHONE CALIBRATION
// ======================================================
void calibrateMic() {

  float sum = 0;

  for (int i = 0; i < 50; i++) {
    sum += sampleAmp();
    delay(5);
  }

  noiseFloor = sum / 50.0f;

  if (noiseFloor < 20) noiseFloor = 20;
}

// ======================================================
// MICROPHONE PROCESSING
// ======================================================
// Updates peak (fast transient) and envelope (smooth energy).
// Returns normalized level 0..1.
// ======================================================
float readMic() {

  float amp = sampleAmp();

  if (amp < noiseFloor * 1.3f)
    amp = 0;

  peak *= 0.85f;
  if (amp > peak)
    peak = amp;

  envelope = envelope * 0.75f + amp * 0.25f;

  // adaptive floor: creeps only during silence
  if (amp == 0) {
    noiseFloor += (sampleAmp() - noiseFloor) * 0.002f;
    if (noiseFloor < 20) noiseFloor = 20;
  }

  return constrain(envelope / (noiseFloor * 3.2f), 0.0f, 1.0f);
}

// ======================================================
// LIGHTING CONTROL SYSTEM
// ======================================================
void updateLighting(bool soundActive) {

  if (soundActive) {

    hue += 0.04f;
    if (hue > TWO_PI) hue -= TWO_PI;

    r_t = sinf(hue) * 127 + 128;
    g_t = sinf(hue + 2.094f) * 127 + 128;
    b_t = sinf(hue + 4.188f) * 127 + 128;

  } else {

    r_t = 255;
    g_t = 255;
    b_t = 255;
  }

  float smooth = 0.11f;

  r_s += (r_t - r_s) * smooth;
  g_s += (g_t - g_s) * smooth;
  b_s += (b_t - b_s) * smooth;

  dmx[DIM_CH] = 255;

  dmx[RED_CH]   = constrain((int)r_s, 0, 255);
  dmx[GREEN_CH] = constrain((int)g_s, 0, 255);
  dmx[BLUE_CH]  = constrain((int)b_s, 0, 255);

  dmx[PAR_RED]   = dmx[RED_CH];
  dmx[PAR_GREEN] = dmx[GREEN_CH];
  dmx[PAR_BLUE]  = dmx[BLUE_CH];
  dmx[PAR_DIM]   = dmx[DIM_CH];
}

// ======================================================
// MOVEMENT SYSTEM
// ======================================================
void updateMovement() {

  float panTarget  = panCenter  + sinf(t) * panAmp;
  float tiltTarget = tiltCenter + sinf(2 * t) * tiltAmp;

  pan_s  += (panTarget  - pan_s)  * smoothFactor;
  tilt_s += (tiltTarget - tilt_s) * smoothFactor;

  dmx[PAN_CH]  = constrain((int)pan_s,  0, 255);
  dmx[TILT_CH] = constrain((int)tilt_s, 0, 255);

  t += 0.05f;
  if (t > TWO_PI) t -= TWO_PI;   // sin(t), sin(2t) both repeat at 2*pi
}

// ======================================================
// SYSTEM SETUP
// ======================================================
void setup() {

  Serial.begin(115200);

  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);

  Serial2.begin(250000, SERIAL_8N2, RXD2, TXD2);
  analogReadResolution(12);

  for (int i = 0; i <= DMX_CHANNELS; i++) {
    dmx[i] = 0;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  setupOTA();

  // wait up to 8 s for WiFi; carry on if it never connects
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000) {
    delay(250);
  }

  delay(1200);      // hardware stabilization
  calibrateMic();
}

// ======================================================
// MAIN LOOP
// ======================================================
// OTA is serviced on every pass. Everything else runs on a
// fixed 20 ms tick so all smoothing constants stay valid.
// ======================================================
void loop() {

  handleOTA();   // keep this line in every version

  static unsigned long lastTick = 0;
  if (millis() - lastTick < TICK_MS) return;
  lastTick = millis();

  dmx[0] = 0;    // DMX start code

  readMic();     // updates peak + envelope

  if (peak > noiseFloor * 2.5f) {
    lastSoundTime = millis();
  }

  bool soundActive = (millis() - lastSoundTime < soundHold);

  updateLighting(soundActive);
  updateMovement();
  sendDMXFrame();
}
