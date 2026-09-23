#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

// ============================================================
// WebPortal.h  -  on-demand Wi-Fi configuration / messaging portal
//
// The ESP32 becomes its own Wi-Fi hotspot. A phone joins it and
// opens http://192.168.4.1 - no router, no internet, no app.
// The whole point: the phone already has a Bangla keyboard, so
// arbitrary Bangla text entry becomes possible without adding any
// hardware to the handheld.
//
// POWER: Wi-Fi costs roughly 90-130 mA average while the AP is up,
// against ~45 mA for the node idling. It is therefore strictly
// on-demand: long-press GPIO 27 to start it, and it shuts itself down
// after PORTAL_IDLE_MS with no HTTP activity.
// ============================================================

#include <Arduino.h>

// ---- AP identity. Password must be >= 8 chars (WPA2 requirement). ----
#define PORTAL_SSID      "LoRaComm-"
#define PORTAL_PASSWORD  "bangla1234"
#define PORTAL_CHANNEL   6

// 1 = the network name carries this module's ID (LoRaComm-A0), so twenty
//     handsets do not all advertise the same name.
// 0 = one fixed name for every module, exactly as the build that was known
//     to work. Flip this to 0 if the phone ever refuses the password: it
//     should make no difference, because both sides derive the WPA2 key
//     from the name they can see, but it removes the last thing that
//     differs from that build.
#define PORTAL_SSID_PER_MODULE 1
#define PORTAL_SSID_FIXED      "LoRaComm-01"

// Auto-shutdown after this long with no HTTP request. Any page load,
// poll or POST resets the timer.
#define PORTAL_IDLE_MS   (5UL * 60UL * 1000UL)

// 1 = presets are editable from the web page. Set to 0 to keep all 20
// presets exactly as their offline-shaped pixel-perfect bitmaps: the
// Presets tab then becomes read-only and the server refuses writes.
#define PORTAL_ALLOW_PRESET_EDIT 1

// 1 = run a DNS server that answers every lookup with the AP's own IP,
// so phones pop the page automatically ("sign in to network").
// Set to 0 to save a few KB of RAM; you then type 192.168.4.1 by hand.
#define PORTAL_CAPTIVE   1

void     portalStart();
void     portalStop();
void     portalToggle();
bool     portalIsActive();
void     portalLoop();            // call every loop(); cheap when inactive


// How many phones are currently associated with the access point. Shown on
// the e-paper so a failure to load the page can be told apart from a
// failure to join the network at all.
int      portalClientCount();

// True once a browser has actually fetched the page since the portal came up.
bool     portalPageWasServed();

// True if an HTTP request arrived within the last couple of seconds, i.e.
// a browser is mid-conversation with the portal right now. Used to hold a
// queued LoRa transmission back for a moment, because a transmit blocks
// everything else for several seconds at slow spreading factors.
bool     portalBusyWithBrowser();
String   portalIpString();
String   portalSsidString();
uint32_t portalSecondsLeft();

#endif  // WEB_PORTAL_H
