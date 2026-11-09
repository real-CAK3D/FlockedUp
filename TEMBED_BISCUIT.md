# T-Embed CC1101 (Plus) → Flying Flocks "Biscuit" firmware

A self-contained firmware **image** (not Marauder) that turns a LilyGo T-Embed CC1101
Plus (ESP32-S3) into a Biscuit board: Flying Flocks connects to it over BLE exactly
like any fleet board (service `4fafc201…`), and the same line protocol is mirrored over
USB-CDC serial. Flashed to the board's **internal flash over USB** (not an SD image).

## Build (BUILD ONLY — nothing flashed here)
Flashable config (16 MB flash, huge_app partition) — **39% flash, 18% RAM**:
```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" \
  --warnings none \
  --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" \
  G:/arduino-tools/sketches/tembedbiscuit
```
(The default `esp32:esp32:esp32s3` also builds but fills 94% of the 1.3 MB app slot — use
the 16 MB/huge_app FQBN for a real image.) The linker flag is needed because the firmware
overrides `ieee80211_raw_frame_sanity_check` to allow crafted deauth/beacon frames.

## GATT contract (unchanged — app filters on service UUID)
Service `4fafc201-1fb5-459e-8fcc-c5c9c331914b`; CMD `…b26a8` (Write/WriteNR),
RESPONSE `…b26a9` (Notify + CCCD), STATUS `…b26aa` (Read JSON). Advertises as `Biscuit-TEmbed`.
Framing: newline `TYPE:SUBTYPE:PAYLOAD\n`.
STATUS JSON: `{"status":0,"battery":100,"op":"wardrive","heap":...,"model":"Biscuit-TEmbed","name":"Biscuit-TEmbed"}`

## Link-code pairing gate
At boot the board generates a random **6-digit link code** and shows it on the ST7789
("LINK CODE" + the number). BLE still advertises and connects normally, but the session is
**LOCKED**: every CMD except `CMD:link:<code>` is ignored until the correct code is entered.
- On connect (after notifications enable), while locked, emits `DATA:LINK:need` once; also
  re-emits `DATA:LINK:need` whenever any non-link CMD arrives while locked.
- STATUS JSON carries `"link":"required"` while locked, `"link":"ok"` once linked.
- `CMD:link:<code>` → match: `linked`, emits `DATA:LINK:ok`, all CMDs accepted this session.
  No match: emits `DATA:LINK:bad`, stays locked; after 5 bad tries a 5 s backoff (bad tries
  during backoff just get `DATA:LINK:bad`).
- Linking is **per BLE session** — resets to locked on disconnect/reboot. The code only
  changes on reboot or an **encoder-button** press (regenerate + relock), so the app can
  persist the last code per device and auto-resend `CMD:link:<code>` on reconnect to link
  silently.

New link records: `DATA:LINK:need`, `DATA:LINK:ok`, `DATA:LINK:bad`. STATUS field: `link`.

## Commands (CMD characteristic)
| Command | Effect |
|---|---|
| `CMD:link:<code>` | pairing gate — unlock the session with the 6-digit code shown on screen |
| `CMD:scan` / `CMD:sniff` / `CMD:aplist` | 2.4 GHz AP sweep → `DATA:AP:` (core wardrive feed) |
| `CMD:stationlist` / `CMD:sniffsta` | promiscuous station sniff → `DATA:STA:` |
| `CMD:probesniff` | promiscuous probe sniff → `DATA:PROBE:` |
| `CMD:pktmon` / `CMD:monitor` | promiscuous counters → `DATA:PKT:` (+ STA/PROBE) |
| `CMD:deauth[:<bssid>]` | deauth broadcast or targeted → `DATA:ATK:deauth,…` (app-gated) |
| `CMD:beaconspam` | beacon spam → `DATA:ATK:beacon,…` (app-gated) |
| `CMD:scanall` | **sub-GHz** RSSI sweep + raw decode → `DATA:RF/TPMS/WEATHER/KEYFOB` |
| `CMD:tx:<freqMHz>,<bits>` | **sub-GHz** OOK replay (ALLOW_TX gate; your own devices only) → `DATA:TXDONE:` |
| `CMD:nfcread` | poll PN532 for a tag → `DATA:NFC:` |
| `CMD:irsend:<proto>,<hex>[,<bits>]` | IR transmit (NEC/SONY/SAMSUNG/RC5) → `DATA:IRTX:` |
| `CMD:irsniff` | enable IR receiver → `DATA:IR:` on capture |
| `CMD:stopscan` | stop everything → `DATA:MODE:idle,stop` |

## DATA records (RESPONSE notify + USB serial)
### Shared with the existing Biscuit contract / C5 reference
`DATA:AP:SSID,BSSID,Channel,RSSI,Security` · `DATA:RF:freqMHz,rssi` ·
`DATA:TPMS:freq,id,kPa,tempC,rawhex` · `DATA:WEATHER:freq,stationid,tempC,hum` ·
`DATA:KEYFOB:freq,pulses,bits` · `DATA:TXDONE:freq,nbits`

### NEW subtypes this board adds (for the app side to parse)
| Record | Format | From |
|---|---|---|
| `DATA:STA:`  | `staMAC,apBSSID,rssi` | Wi-Fi promiscuous station sniff |
| `DATA:PROBE:`| `essid,rssi` | Wi-Fi promiscuous probe-request sniff |
| `DATA:PKT:`  | `beacons,deauths,probes,data` | packet-monitor counters (once/sec) |
| `DATA:ATK:`  | `deauth\|beacon,start` | offensive mode started (app-gated) |
| `DATA:NFC:`  | `uidHex,kind` | PN532 tag (kind = MifareClassic/MifareUL-NTAG/ISO14443A/none) |
| `DATA:IR:`   | `protocol,bits,hex` | IR frame received |
| `DATA:IRTX:` | `protocol,hex,ok` | IR transmit ack |
| `DATA:MODE:` | `label,start\|stop` | mode transitions |
| `DATA:ERR:`  | `reason,detail` | e.g. `nfc,absent` / `unknown,<cmd>` |

## Hardware pin map (verified against LilyGO repo examples/utilities.h)
See `tembed-cc1101-pinout.md`. Highlights: CC1101 CS=12/GDO0=3 on shared SPI (SCK11/MOSI9/
MISO10) **+ RF band-select switch SW0=48/SW1=47 set per band** (the bare C5 had none);
ST7789 CS41/DC16/BL21; PN532 I2C SDA8/SCL18; IR TX2/RX1; WS2812 on 14; encoder 4/5/0, user
key 6; **PWR_EN=15 driven HIGH** or the board powers off.

## Status / caveats (build-only; review before flashing)
- Compiles clean; **not flashed, not hardware-tested.** CC1101 decode is ported from the
  working c5biscuit.ino and re-pinned — first on-air run near a car/sensor is a tuning pass.
- CC1101, SD and the ST7789 share one SPI bus; CS discipline is in place but bus timing may
  need tuning on real hardware.
- deauth/beacon do real 802.11 frame injection (gated by the app's ownership confirmation).
- The ST7789 shows connection/scan status only; the app is the real UI.
