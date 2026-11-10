# Flocked Up — Roadmap

Feature backlog for the crow-themed T-Embed CC1101 Plus firmware.

**Legend:** ✅ works on the T-Embed as-is · 📱 needs the phone + Flying Flocks app (BLE) · 🌐 needs internet (join Wi-Fi/hotspot, or via phone) · 🔌 needs a bolt-on module · ⛔ won't do (illegal/unethical)

## Sub-GHz / RF (CC1101: 300–348, 387–464, 779–928 MHz)
- ✅ Free-tune frequency (encoder-dial any freq in band)
- ✅ Frequency Analyzer — find the active frequency of a remote **(v0.5)**
- ✅ Full-band waterfall / spectrum view
- ✅ RSSI "fox-hunt" meter (hot/cold signal finder)
- ✅ More decoders: CAME, NICE, Holtek, Princeton; more TPMS (Toyota/Ford/Schrader); more weather (Acurite/Oregon/LaCrosse/Ambient)
- ✅ Record → SD (.sub) → replay (gated); browse the 20k-file library in The Nest
- 📱🌐 Geo-tagged RF map (phone GPS stamps every capture)
- ⛔ Jammers

## Wi-Fi (2.4 GHz) incl. hotspot / local network
- 🌐 Join hotspot / home Wi-Fi (creds via the app, a config AP, or read from the SD's bruce.conf)
- 🌐 Live weather (current + forecast)
- 🌐 NTP clock (time/date/timezone)
- 🌐 OTA firmware update from the hub
- 🌐 Wi-Fi geolocation (rough location from nearby APs)
- ✅ Wi-Fi analyzer (channel congestion, best channel)
- ✅ Deeper recon: handshake capture, vendor ID, hidden-SSID reveal
- 🌐 Wigle export / online sync to the Flying Flocks layer
- ✅⛔ Evil-portal / deauth / beacon — kept, gated to own gear

## Bluetooth / BLE
- ✅ Device classifier (phones, earbuds, cars, trackers)
- ✅📱 Anti-tracker (AirTag/Tile detection)
- ✅ BLE beacon (iBeacon/Eddystone)

## Phone telemetry & signals (over the app)
- 📱 GPS → radar / compass / locator
- 📱 Speed / heading / altitude HUD
- 📱 Cell signal relay (carrier/bars/tower from the phone)
- 📱 Notifications relay (texts/calls/alerts)
- 📱 Phone battery / clock / weather pushed to the device
- 📱 Media remote; breadcrumb trail

## NFC / RFID (PN532, 13.56 MHz)
- ✅ Read MIFARE Classic (+keys), NTAG, Ultralight, ISO15693
- ✅ Save dumps as Flipper .nfc; emulate / write to own cards
- 🔌 125 kHz LF RFID (needs an LF module; PN532 is HF only)

## Infrared
- ✅ TV-B-Gone (Flipper-IRDB loaded); universal remote; learn + save .ir

## Crow mascot, themes & visuals
- ✅ More scenes + per-screen contextual crows
- ✅ Animated crow (idle bob/blink, scan, hit, sleep)
- ✅ Theme accent colors; LED shows (signal meter **(v0.5)**, pulse, rainbow)
- ✅ Screensaver; day/night auto-theme

## Settings & system
- ✅ Screen brightness/auto-dim; rotation/flip (done); sleep timeout (done)
- ✅ Battery %; units (°C/°F, metric/imperial)
- 🌐 Saved Wi-Fi networks; clock/timezone
- ✅ Data logging to SD (+GPS); setting profiles; diagnostics; factory reset; favorites

## Mesh / fleet
- 📱 Mesh messages on-screen (relayed from the XIAO Meshtastic node via the app — CC1101 can't do LoRa)
- 📱 WDG Wars tie-in

## Add-on modules
- 🔌 NRF24 (2.4 GHz scan/analysis) · external GPS (UART) · BME280 (I²C weather) · LoRa (real Meshtastic)

---
Shipped: v0.1 crow UI · v0.2 BLE scan + OOK decode · v0.3 pixel-art mascot · v0.4 rotation/speed/sleep/settings · **v0.5 Frequency Analyzer + LED signal meter**
