# Bangla Off-Grid LoRa Emergency Communicator

Handheld radios that send and receive **Bangla text messages with no mobile network, no
SIM, no internet and no router** — just two devices and the air between them.

Each unit is an ESP32 with an SX1278 LoRa radio, a 1.54" e-paper screen and three buttons.
It works on its own for the twenty preset messages. When you need to type something that
isn't in the list, the unit raises its own Wi-Fi hotspot and your phone becomes the Bangla
keyboard — the phone never needs a connection either.

> Built for **EEE 416 — Microprocessor and Embedded Systems Laboratory**, Department of
> Electrical and Electronic Engineering, BUET.

<p align="center">
  <img src="docs/screens/received.png" width="180" alt="Incoming message on the e-paper">
  <img src="docs/screens/viewmsg.png"  width="180" alt="Bangla message text">
  <img src="docs/screens/stored.png"   width="180" alt="Stored message list">
  <img src="docs/screens/pair_priv.png" width="180" alt="Private session established">
</p>

---

## Why

During a flood or a cyclone the towers are the first thing to go, and they stay down
exactly as long as people need to reach each other. Satellite phones solve it for a
budget nobody has. This is the cheap version: a pair of handhelds, about ৳1,900 each,
that keep working when everything else has stopped — and that speak Bangla, because a
relief message in English is no message at all to most of the people who need it.

## What it does

| | |
|---|---|
| **Text over LoRa** | SX1278 at 433 MHz. Kilometres of range in the open, no infrastructure of any kind. |
| **Bangla on a 200×200 e-paper** | Real conjuncts and vowel signs, not transliteration. The screen holds its last image with the power off. |
| **Three buttons, nine actions** | Single, double and long press on NAV / SEL / FN cover the whole interface. |
| **20 preset messages** | Pre-shaped Bangla bitmaps for the messages you actually need in an emergency. |
| **Free Bangla from a phone** | Long-press FN → the unit becomes a Wi-Fi hotspot with a captive portal at `192.168.4.1`. Type in the phone's own Bangla keyboard. |
| **20-device fleet with addressing** | IDs `A0`–`A9` and `B0`–`B9`. Broadcast by default; request a private one-to-one session with any peer. |
| **Persistent inbox** | 100 received messages in a fixed-record file on LittleFS. Survives power loss; written before the message is ever displayed. |
| **BLE serial link** | Nordic UART Service, for debugging and for phone apps. |
| **Battery gauge** | Divided cell voltage on GPIO 34, shown in the status strip on every screen. |

## Hardware

| Part | Notes |
|---|---|
| ESP32 DevKit (CP2102, 30-pin) | 520 KB SRAM, no PSRAM on this module |
| SX1278 / Ra-02 LoRa module | 433 MHz, spring antenna |
| 1.54" e-paper, GxEPD2_154_D67 | 200 × 200, black and white |
| 3 × tactile push button | active-low, internal pull-ups |
| Li-ion cell + boost converter | battery sense through a **2:1 divider** into GPIO 34 |

### Pin map

```
E-paper            LoRa (VSPI, shared)       Buttons          Battery
  CS    GPIO 5       NSS   GPIO 15             NAV  GPIO 13     SENSE  GPIO 34
  DC    GPIO 26      RST   GPIO 4              SEL  GPIO 14            (via 2:1 divider)
  RST   GPIO 25      DIO0  GPIO 2              FN   GPIO 27
  BUSY  GPIO 33      SCK   GPIO 18
                     MISO  GPIO 19
                     MOSI  GPIO 23
```

The e-paper and the radio share the VSPI bus; only the chip selects differ.

> **GPIO 34 is input-only and tops out at 3.6 V.** The firmware assumes
> `BATT_DIV_RATIO = 2.0` — do not feed a boost-converter output into it directly.

## Building

**Arduino IDE**, ESP32 board package, board *ESP32 Dev Module*.

> ### ⚠ Set the partition scheme first
> **Tools → Partition Scheme → `Huge APP (3MB No OTA/1MB SPIFFS)`**
>
> The firmware is ~1.96 MB. BLE, Wi-Fi and the web server together do not fit in the
> default 1.31 MB app partition, and the build fails with
> `text section exceeds available space in board`.

Libraries:

- [`LoRa`](https://github.com/sandeepmistry/arduino-LoRa) — Sandeep Mistry
- [`GxEPD2`](https://github.com/ZinggJM/GxEPD2) — Jean-Marc Zingg
- `Adafruit GFX Library` (GxEPD2 dependency)

Everything else — BLE, `Preferences`, `WebServer`, `DNSServer`, `LittleFS` — ships with
the ESP32 core.

Put **every `.h` and `.cpp` file in the same sketch folder** as
`Combine_BLE_EPaper.ino` and upload.

## First boot

1. The ID chooser appears. The unit broadcasts a `WHOIS`; every unit that already owns an
   ID answers with a `CLAIM`, and those IDs are struck off the list — so you cannot pick a
   duplicate.
2. Choose an ID with NAV, confirm with SEL. It is written to NVS and read back, and the
   chooser never appears again.
3. To change it later: send `IDRESET` over serial, or use the button on the portal's radio
   tab. **Re-flashing does not erase NVS.**

<p align="center">
  <img src="docs/screens/fleet_assign.png" width="200" alt="ID chooser">
  <img src="docs/screens/predef_top.png"   width="200" alt="Preset messages">
  <img src="docs/screens/portal.png"       width="200" alt="Portal info screen">
</p>

## Using it

**Buttons**

| | Single | Double | Long |
|---|---|---|---|
| **NAV** (13) | down | up | — |
| **SEL** (14) | enter / send | back to main | sleep / wake |
| **FN** (27) | peer list | unpair | Wi-Fi portal on / off |

Thirty idle seconds blanks the panel. Nothing is lost — e-paper keeps its image unpowered.

**Pairing**

Broadcast is the default: everyone hears everyone. For a private conversation, pick a peer
from the list and send a request. Once they accept, both ends agree on a 4-byte session
token, and every frame that doesn't carry the right destination *and* the right token is
dropped before it is ever stored or shown.

This is addressing, **not encryption**. Anyone with the same firmware can listen to
broadcast traffic.

**The Wi-Fi portal**

Long-press FN. The unit raises `LoRaComm-<ID>` (password `bangla1234`) and serves a
five-tab page at `http://192.168.4.1`, which most phones pop up by themselves. Send free
Bangla, read the inbox, edit presets, retune the radio, manage pairing. It shuts itself
down after five idle minutes, because Wi-Fi costs ~90–130 mA against ~45 mA idle.

<p align="center">
  <img src="docs/portal/portal_send.png"  width="240" alt="Portal — send">
  <img src="docs/portal/portal_pair.png"  width="240" alt="Portal — pairing">
  <img src="docs/portal/portal_inbox.png" width="240" alt="Portal — inbox">
</p>

## How it works

### Bangla without a font engine

An ESP32 cannot run a text shaper. Bangla needs one: `ক + ্ + ষ` is a single glyph `ক্ষ`,
and vowel signs move to the *left* of the consonant they follow.

So the shaping happens on a PC, once, at build time. **HarfBuzz** with **Noto Sans
Bengali** lays out every string the device can display, and the result is baked into the
firmware:

- `BanglaAssets.h` — 38 pixel-perfect bitmaps for the preset messages
- `BanglaUi.h` — 45 bitmaps for menus, labels and status text
- `BanglaFallbackFont.h` — 500+ pre-joined shapes, including 24 conjuncts, which render
  arbitrary Bangla arriving from the phone

The device never reasons about Bangla. It blits.

### Frame format

```
┌────┬──────┬─────┬─────┬─────────────┬───────────────────────────┐
│ B2 │ type │ src │ dst │ session ×4  │ UTF-8 Bangla, up to 180 B │
└────┴──────┴─────┴─────┴─────────────┴───────────────────────────┘
  8-byte header                         payload
```

`dst` is a peer ID or `255` for broadcast. Frame types cover text plus the session
control traffic — `REQUEST`, `ACCEPT`, `CONFIRM`, `PING`, `PONG`, `LEAVE`, `BUSY`,
`WHOIS`, `CLAIM`.

Payload is capped at 180 bytes on purpose: at SF12 / BW 125 kHz / CR 4:8 a 200-byte packet
is already about 11 seconds on the air.

### Flash layout

With the *Huge APP* scheme:

| Region | Size | Used for |
|---|---|---|
| App | 3 MB | the 1.96 MB firmware (62%) |
| NVS | 20 KB | presets, radio config, identity — ~60 of 504 entries |
| LittleFS | 1 MB | `/inbox.bin`: a 16-byte header + N × 184-byte records |
| OTA | — | not allocated; the app needs the space |

The inbox used to live in NVS. It doesn't any more: NVS charges `1 + ceil(len/32)` entries
per string, and 80 full-length messages need 633 of the 504 available — it would have
worked for months and then started dropping writes silently. Fixed-size records on
LittleFS give one seek per message, no fragmentation, and no entry limit. 100 messages is
18 KB, 1.8% of the partition.

## Repository layout

```
Combine_BLE_EPaper/
├── Combine_BLE_EPaper.ino   main sketch — UI, buttons, radio, e-paper, BLE
├── Link.h / Link.cpp        fleet identity, addressing, private sessions
├── MsgStore.h / .cpp        LittleFS inbox
├── WebPortal.h / .cpp       access point, captive DNS, 15 REST endpoints
├── PortalPage.h             the whole web page, compiled into flash
├── NodeApi.h                the seam: portal ↔ sketch, nothing else crosses
├── BanglaAssets.h           preset-message bitmaps
├── BanglaUi.h               menu and status bitmaps
├── BanglaFallbackFont.h     pre-joined glyph shapes for arbitrary Bangla
└── README_WEBPORTAL.md      portal details, build settings, troubleshooting
```

## Limitations

- **No encryption.** Address filtering keeps other people's messages off your screen; it
  does not keep your messages off theirs.
- **No delivery receipt.** The sender is told the packet went out, not that it arrived.
- At SF12 a 20-character Bangla message takes about 3.8 seconds on the air. SF7 brings
  that under half a second, at the cost of range.
- Browsing hundreds of stored messages with two buttons is unpleasant. The storage ceiling
  is 5,698 messages; the usable one is much lower.
- Not a certified radio device. 433 MHz ISM use is subject to local regulation.


