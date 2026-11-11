# Flocked Up — Roadmap

Feature backlog for the crow-themed T-Embed CC1101 Plus firmware.

**Legend:** ✅ works on the T-Embed as-is · 📱 needs the phone + Flying Flocks app (BLE) · 🌐 needs internet (join Wi-Fi/hotspot, or via phone) · 🔌 needs a bolt-on module · ⛔ won't do (illegal/unethical)

## Sub-GHz / RF (CC1101: 300–348, 387–464, 779–928 MHz)
- ✅ Free-tune frequency — interactive encoder dial, live RSSI bar + LED, step select **(v0.10)**
- ✅ Frequency Analyzer — find the active frequency of a remote **(v0.5)**
- ✅ 433 MHz spectrum / waterfall (streams DATA:SPECTRUM, LED peak meter) **(v0.9)**
- ✅ Full-band waterfall — coarse 300–928 MHz sweep, 64 bins, antenna band-select per bin **(v0.10)**
- ✅ RSSI "fox-hunt" meter (hot/cold signal finder)
- ✅ More decoders: CAME, NICE, Holtek, Princeton; more TPMS (Toyota/Ford/Schrader); more weather (Acurite/Oregon/LaCrosse/Ambient)
- ✅ Record → SD (.sub) → replay (gated); browse the 20k-file library in The Nest
- 📱🌐 Geo-tagged RF map (phone GPS stamps every capture)
- ⛔ Jammers

## Wi-Fi (2.4 GHz) incl. hotspot / local network
- ✅🌐 Join home Wi-Fi — creds from SD `/flockedup/wifi.txt` (ssid,pass,lat,lon) **(v0.10)**
- ✅🌐 Live weather — current temp via open-meteo, streams DATA:WX **(v0.10)**
- ✅🌐 NTP clock — syncs on "Clock & Weather", streams DATA:TIME **(v0.10)**
- 🌐 OTA firmware update from the hub — deferred (brick risk on a Launcher multi-boot board; do it by reflash for now)
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

## BadUSB / HID
- ✅ Ducky-script runner (.dd in The Nest → Play, Authorize-gated): STRING/ENTER/TAB/DELAY/GUI/CTRL/ALT/SHIFT **(v0.10)** — needs USB Mode "USB-OTG (TinyUSB)" at flash time; compiles out otherwise
- ⬜ On-screen payload picker; larger key map

## Crow mascot, themes & visuals
- ✅ More scenes + per-screen contextual crows
- ✅ Animated crow — fluid pixel loop (idle breathing bob + wing-flap, scan flap, hit hop, flip) **(v0.8)**
- ✅ Screen border + 3-crow corner accent **(v0.8)**
- ✅ Deeper idle loop — two glances + a double wing-flap + lazy side-to-side sway **(v0.9)**
- ⬜ Multi-frame hand-drawn sequences (drop more crow frames to deepen each loop)
- ✅ Theme accent colors; LED shows (signal meter **(v0.5)**, pulse, rainbow)
- ✅ Screensaver; day/night auto-theme

## Settings & system
- ✅ Screen brightness/auto-dim; rotation/flip (done); sleep timeout (done)
- ✅ Battery %; units (°C/°F, metric/imperial)
- 🌐 Saved Wi-Fi networks; clock/timezone
- ✅ Data logging to SD (+GPS); setting profiles; diagnostics; factory reset; favorites

## Fleet telemetry & health
- ✅ Live health heartbeat — `DATA:HEALTH:<heap>,<uptime>,<sd>,<cc1101>,<nfc>,<link>` every ~3s over BLE **and** USB-CDC **(v0.9)**
- ✅ Richer STATUS JSON (uptime + per-radio up/down) so the app's Fleet Health view can tell a board is working **(v0.9)**
- 📱 Triangulation: same signal heard by ≥2 boards → strongest board is closest (app-side, from the relay log) **(v0.9)**
- ✅🔌 Per-board battery % — wired into STATUS + DATA:HEALTH; disabled until the BAT ADC pin is confirmed (reports -1/unknown, never a fake reading) **(v0.10)**

## Mesh / fleet
- 📱 Mesh messages on-screen (relayed from the XIAO Meshtastic node via the app — CC1101 can't do LoRa)
- 📱 WDG Wars tie-in

## Add-on modules
- 🔌 NRF24 (2.4 GHz scan/analysis) · external GPS (UART) · BME280 (I²C weather) · LoRa (real Meshtastic)

## Scripts & automation (.fu "Tricks")
- ✅ On-device `.fu` macro engine: `caw` `wait` `led` `nfc` `ir` `irfile` `sub` `tx` (TX verbs gated by the Authorize screen)
- ✅ Native capture replay: Flipper RAW `.sub` over CC1101, parsed/raw `.ir` over the IR LED
- ✅ Importer: drop `.sub`/`.ir`/`.fu` into `/flockedup/import` → wrapped into runnable Scripts
- ✅ On-device move: copy any capture in The Nest into a stash (collection), new or existing
- ✅📱 Phone can `CMD:import` / `CMD:scriptlist` / `CMD:script:run:<name>`
- ⛔ Jam/flood/noise — refused in the engine (federal offence, no own-network exception)

---
Shipped: v0.1 crow UI · v0.2 BLE scan + OOK decode · v0.3 pixel-art mascot · v0.4 rotation/speed/sleep/settings · v0.5 Frequency Analyzer + LED signal meter · v0.6 Collections (Hoard) + 80 MHz UI · v0.7 Scripts engine, capture replay, importer, on-device move · v0.8 fluid animated crow + screen border + 3-crow accent · v0.9 live health heartbeat + 433 waterfall + deeper crow loop · **v0.10 free-tune dial + full-band waterfall + Wi-Fi clock/weather + BadUSB + battery-ready**
