# 20-module broadcast and private messaging

## Upload

1. Extract this ZIP completely. Keep every `.h` and `.cpp` beside `Combine_BLE_EPaper.ino`, inside the folder named `Combine_BLE_EPaper`.
2. Open that `.ino`. Set **Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)** and **Tools → Flash Size → 4MB**. Without this you get `text section exceeds available space in board`: BLE + Wi-Fi + the web server do not fit in the default 1.25 MB app partition.
3. Upload this same sketch to every module. Keep frequency, spreading factor, bandwidth, coding rate and sync word identical on all of them.

## The ID is asked for once — this is what changed

Each module owns one of **A0–A9 / B0–B9**. Previously the chooser could come back on a later wake. It no longer can.

- The chosen ID and a separate "committed" flag are written to NVS and **read back to confirm** before the module accepts them. If the write fails, the module says so on serial instead of pretending it worked and forgetting the ID at the next boot.
- Once committed, `identityNeeded()` is false for the life of the device. Waking the screen, receiving a message, tapping FN and opening the Wi-Fi portal all go straight to the normal screens.
- The only routes back are the serial command **`IDRESET`** and the **আইডি মুছুন** button on the portal's পেয়ার tab.

**Re-uploading the sketch does not clear NVS.** If you want a re-flash to reset the ID, tick **Tools → Erase All Flash Before Sketch Upload**, or just send `IDRESET` over serial.

### Taken IDs disappear from the list

Two modules can no longer be given the same ID by accident.

1. Opening the chooser broadcasts a **WHOIS**. Every module that already owns an ID answers with a **CLAIM**, after a random delay inside the 6-second window so twenty answers do not collide.
2. Claimed IDs are struck off: the list shows them marked **ব্যবহৃত**, the cursor skips them, and choosing one is refused.
3. Claims are remembered in NVS, so a reboot does not forget them. `IDSCAN` re-asks; `IDFORGET` wipes the remembered list first. The portal's **আবার খুঁজুন** button does both.
4. A module also learns an ID is taken simply by hearing any traffic from it.
5. If two modules somehow commit the same ID at the same instant, the CLAIM carries a 32-bit hardware tag: the lower tag keeps the ID, the higher one gives it up and reopens its chooser. No human has to notice.

While the scan is running the screen says **খোঁজা হচ্ছে** and selection is held, so you cannot pick an ID that is about to be reported as taken.

## Bangla on every screen

The ID chooser, the Wi-Fi screen, the private-connection screen and all button hints were English. They are now Bangla bitmaps in `BanglaUi.h` — 32 strings, 5.2 KB of flash — generated the same way as `BanglaAssets.h`: Noto Sans Bengali shaped offline with HarfBuzz, so conjuncts and matras are correct and the ESP32 only blits finished pixels.

Deliberately still Latin, because they are things you read aloud or retype on a phone keyboard:

- module IDs (`A0`, `B3`)
- the Wi-Fi network name (`LoRaComm-A0`)
- the password (`bangla1234`)
- the address (`192.168.4.1`)

## Partial refresh

A full refresh on this panel takes roughly two seconds and flashes the screen black. It is now used only when the screen you are looking at *changes*. Everything that happens while you stay on a screen is a partial update of just the affected band:

| what changed | what is repainted |
|---|---|
| cursor moves between two visible rows | those two rows (~50 px) |
| list scrolls past the top or bottom | the list band (125 px) |
| battery reading, Wi-Fi marker, peer | the status strip (19 px) |
| you arrive on a different screen | everything, full refresh |

Partial updates leave faint ghosting, so **every 24 of them the next repaint is promoted to a full refresh**. That check lives in the region painters, not only in the screen functions — otherwise a long scroll, which never calls a screen function, would never clear.

### Lists show five messages at a time

The stored-message screen used to show one message; the preset screen showed three. Both now show **five rows of 25 px**, with a Bangla counter (`৫/৩৭`) beside the title. Scrolling past the bottom drops the top row and brings a new one in at the bottom, in one partial update.

Presets are also cached in RAM now. Drawing a row asks "is this text one of the 20 presets?", and answering that from NVS meant up to 100 flash reads per repaint — enough to make a 0.3 s update visibly slow. The flash copy is still the authority; the cache is 20 strings, about 1.2 KB.

## Opening the webpage

1. Hold **FN (GPIO 27)** for about 1.2 seconds.
2. Join the hotspot named on the e-paper — `LoRaComm-A0`, or `Setup-…` before an ID is set.
3. Password `bangla1234`, then open **http://192.168.4.1**.

Four things were fixed here:

- **`WiFi.setSleep(false)`.** With power save at its default the AP parks its radio between beacons, and because Bluedroid shares that radio, TCP handshakes get dropped often enough that the browser gives up. This is the single biggest cause of "the page will not open".
- **The e-paper is repainted before the server starts listening.** Drawing blocks for a few hundred milliseconds; doing it after `server.begin()` left a window where a phone could connect and get no answer.
- **`loop()` no longer sleeps while the portal is up,** and services the server three times per pass. The synchronous server only makes progress inside `handleClient()`.
- **Captive-portal probes** for Android, iOS, macOS, Windows and Firefox are all answered, and `/favicon.ico` returns 204 instead of being sent round the redirect loop. Apple and Windows probes get the real page rather than a 302, because the mini-browser that fetches them *is* the portal window the user sees.

If it still does not appear, check the phone has not silently fallen back to mobile data — that is the one failure this firmware cannot fix from its side.

## Waking the screen with a button

Yes, and it is unchanged: **hold SEL (GPIO 14) for about 0.8 s** to wake or sleep the display. Waking goes to the preset list. It does *not* open the ID chooser once an ID is committed.

## Buttons

| button | tap | double-tap | hold |
|---|---|---|---|
| NAV (13) | next item | previous item | — |
| SEL (14) | select / send | back | wake or sleep the screen |
| FN (27) | peer list | disconnect | Wi-Fi portal (1.2 s) |

## Serial commands

`TEST:<text>` · `WIFI` · `ID:<A0>` · `PRIVATE:<B3>` · `ACCEPT` · `UNPAIR` · `LINK` · `IDSCAN` · `IDFORGET` · `IDRESET`

`LINK` now also prints whether the identity is committed, how many IDs are free, and which IDs are known to be in use elsewhere.

## What private means

Destination and session filtering. It does **not** encrypt the payload or authenticate the sender. Other modules running this firmware will not display or store those messages; that is not protection against deliberate interception.

## What changed in this revision

### The top and bottom strips are Bangla now

The status strip used to read `A0 ALL WiFi`. It now reads the ID in Latin followed by Bangla: **সবাই** for broadcast, **ওয়াই-ফাই** when the portal is up, `>B3` when a private session is open. The ID itself stays Latin deliberately — it is what the Wi-Fi network is named after, what the portal shows, and what you read out loud to whoever holds the other handset. Battery percentage was already in Bangla numerals.

### The hint strip answers the button

Pressing SEL on a message used to do nothing visible for several seconds, because at SF12 the packet takes that long to go out. The strip now says **পাঠানো হচ্ছে** while it is going, **পাঠানো হয়েছে** when it is gone (or **পাঠানো যায়নি** if it failed), then puts the normal hints back. Same for **অপেক্ষা করুন** while a pairing handshake runs. Each is one partial update of the bottom 31 px — the list you are reading is untouched.

### Double-press SEL goes back to the message menu, from anywhere

"The message menu" means whichever top menu matches what this handset last did: **বার্তা পাঠানো হলো** after you send, **বার্তা এসেছে** after one arrives. Before either has happened it is the preset list, which is also what a long SEL press wakes into. Pressing back while already there puts the display to sleep, so repeatedly backing out always ends somewhere sensible.

The one exception is the first-boot ID chooser, which cannot be left until an ID exists — without one the module cannot transmit at all.

### The display holds still while something is pending

It no longer sleeps during any part of a pairing negotiation, only during an incoming request as before. When a session actually opens it shows **সংযুক্ত হয়েছে**, holds for two seconds so it can be read, then hands the screen back to the message menu by itself. The Wi-Fi screen does the same: once a browser actually fetches the page, it shows **ওয়েবপেজ চালু হয়েছে** and steps aside two seconds later. The portal keeps running in the background either way.

### "The site can't be reached" — four separate causes addressed

You reported the phone joining the hotspot but the browser failing to load the page. Four things in this build, in order of how likely each is to have been the cause:

1. **The sketch goes deaf for up to eleven seconds while transmitting.** `LoRa.endPacket()` blocks until the packet has left, and at SF12 a full-length message occupies the radio for 10.6 s. During all of that, `server.handleClient()` is never called.

   This cannot be fixed inside the transmit itself. The asynchronous form, `endPacket(true)`, needs `isTransmitting()` to know when the packet is gone — and that method is **private** in the LoRa library. The one public alternative, `onTxDone()`, attaches an interrupt to DIO0 whose handler clears the radio's IRQ flags; this sketch polls `parsePacket()` for reception, which reads those same flags, so the handler would silently swallow incoming messages. Reliable reception matters more than a snappier web page.

   Instead the problem is dealt with where it actually bites: a message queued from the web page is **held back until there is a gap in HTTP traffic**, for at most 3 seconds, so a transmit never starts while the browser is waiting on a reply. The page polls every 3 s and a 1.2 s window after each request is treated as busy, which leaves a comfortable 1.8 s gap to start in.

2. **A browser that assumes https.** Typing `192.168.4.1` into Chrome's address bar can send it to `https://192.168.4.1`, which this server does not speak, and the failure is reported as "site can't be reached" rather than as a protocol error. The e-paper now spells out `http://` beside the address.

3. **Wi-Fi power save.** `WiFi.setSleep(false)` — with power save at its default the AP parks its radio between beacons, and since Bluedroid shares that radio, handshakes get dropped.

4. **Low heap.** The portal now prints free heap at startup and warns below 40 KB, which is roughly where the TCP stack starts refusing connections outright.

**How to find out which one it is on your unit.** The portal logs every HTTP request to serial with the client's address and free heap. Open the Serial Monitor at 115200 and try to load the page:

- Lines appear → the request reached the ESP32, and the problem is in the response.
- Nothing at all → the request never arrived. Check the bottom of the e-paper: it now shows **ফোন যুক্ত হয়েছে** or **ফোন যুক্ত হয়নি**, which separates "the Wi-Fi did not connect" from "the Wi-Fi connected but the page did not load".

Tell me which of those you see and I can narrow it further.

### Library note

Only the public LoRa API is used: `begin`, `beginPacket`, `write`, `endPacket`, `parsePacket`, `available`, `read`, `receive`, `packetRssi`, `packetSnr` and the setters. Nothing here depends on a particular point release of **LoRa by Sandeep Mistry**.

## Verification performed

What was actually run, and what was not.

- **Identity behaviour** — a host harness compiles the real `Link.cpp` three times into separate namespaces with a persisting NVS stub and a shared simulated radio. It proves: the chooser is needed once and stays shut across five reboots; a module that owns an ID answers a newcomer's WHOIS so the newcomer never offers it; a third module sees both taken IDs; two modules grabbing the same ID at the same instant end with exactly one holder; and a failing flash write is reported rather than swallowed. All 25 checks pass.
- **Partial refresh** — the display stub records every window the panel is asked to update, and the real sketch functions are driven through 85 button presses. Confirmed: arriving on a screen is one full refresh; a cursor move inside the window is one 50 px partial; a scroll is one 125 px partial; no partial exceeds the list band; and the ghosting promotion fires about twice in 60 presses. This test caught a real bug — the budget was only checked by the screen functions, so a long scroll never triggered a full refresh.
- **Compilation** — all four translation units compile and link under g++ against stub headers, *after* passing through a simulator of the Arduino IDE's prototype injection. That is what catches the `'X' does not name a type` class of error, which compiling the `.ino` directly does not.
- **Screen layout** — every new screen is rendered in Python from the real bitmap tables using the sketch's own geometry, and checked for overflow past the 200×200 panel and for collisions between bands. None found.
- **The web portal** — the real embedded page was served to headless Chromium against a mock API. All five tabs render, and the identity controls were exercised: taken IDs appear disabled and marked ব্যবহৃত, saving commits, the save control then locks and the reset control enables.
- **Navigation and feedback** — a third harness drives the real sketch: back-press from all eight screens, the ID chooser refusing to be left, home following the last send or receive, the hint flash appearing and expiring within the 31 px strip, the scheduled return home, and the sleep timer being held during a pairing request. 24 checks, all passing.
- **Not run: an Arduino/ESP32 build, and any test on hardware.** The toolchain is not reachable from here. Radio timing, real partial-refresh ghosting and RF collisions are unverified in the physical sense.

Before deploying widely, test three modules: let each pick an ID and confirm the third sees the first two as ব্যবহৃত; power-cycle each and confirm no module asks for an ID again; then broadcast from each and check the other two receive it.
