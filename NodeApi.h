#ifndef NODE_API_H
#define NODE_API_H

// ============================================================
// NodeApi.h  -  the seam between the main sketch and WebPortal
//
// Everything declared here is IMPLEMENTED in Combine_BLE_EPaper.ino
// and CALLED from WebPortal.cpp. Keeping the boundary in one header
// means the web server never touches LoRa, NVS or the e-paper
// directly - it only asks the sketch to do things.
// ============================================================

#include <Arduino.h>

// ---- Shared sizes (moved here from the .ino so both files agree) ----
#define NUM_PREDEFINED              20
#define NUM_PREDEFINED_WITH_BITMAP  20
// Received-message capacity. These now live in a fixed-record file on
// LittleFS (see MsgStore.h), not in NVS, so this is cheap to raise:
// storage is 16 + N*184 bytes out of the 1 MB data partition.
//   100 -> 18 KB (1.8%)     500 -> 92 KB (8.8%)     5698 -> the ceiling
// The practical limit is browsing them with two buttons, not flash.
#define MAX_STORED_MESSAGES        100

// Hard cap on one LoRa payload. SX1278 allows 255, but at SF12/BW125/CR4:8
// a 200-byte packet is already ~11 s of airtime. Keep this modest.
#define MAX_TX_BYTES               180

struct LoRaCfg {
  long    freqHz;    // 410E6 .. 525E6 for SX1278
  uint8_t sf;        // 6..12
  long    bw;        // 7800 .. 500000 Hz
  uint8_t cr;        // 5..8  =>  4/5 .. 4/8
  uint8_t txPower;   // 2..20 dBm (PA_BOOST)
  uint8_t syncWord;  // must match on every module
};

struct NodeStatus {
  int      battPercent;   // -1 when no battery divider detected
  float    battVolts;
  int      lastRssi;
  float    lastSnr;
  uint32_t txCount;
  uint32_t rxCount;
  uint32_t uptimeS;
  uint32_t freeHeap;
  int      storedCount;
  int      storedCap;
  uint32_t storedBytes;
};

// ---- Outgoing messages -------------------------------------
// Queues text for LoRa TX. Does NOT transmit inline: the actual
// send happens in loop(), so an HTTP handler never blocks for the
// several seconds a slow-SF packet takes.
bool   apiQueueOutgoing(const String &utf8);

// Puts raw bytes on the air immediately. Used by the framed text path
// and by the private-session control frames in Link.cpp.
bool   apiRawTransmit(const uint8_t *buf, size_t len);

// ---- Received-message store (NVS ring buffer) --------------
int    apiStoredCount();
String apiStoredMessage(int n);                        // n = 0 is newest
bool   apiStoredEntry(int n, String &text, int &rssi); // one read, text + RSSI
void   apiClearStored();

// ---- Predefined messages -----------------------------------
String apiPreset(int i);
void   apiSetPreset(int i, const String &text);
void   apiResetPresets();            // back to factory defaults
bool   apiPresetIsEdited(int i);     // true => pixel-perfect bitmap no longer valid

// ---- LoRa radio configuration ------------------------------
void   apiGetLoRaCfg(LoRaCfg &out);
void   apiSetLoRaCfg(const LoRaCfg &cfg);   // applies live + saves to NVS

// ---- Telemetry ---------------------------------------------
void   apiGetStatus(NodeStatus &out);
String apiLastReceived();
String apiLastSent();

// ---- Called by WebPortal when the portal turns on/off, so the
//      sketch can repaint the e-paper and pause/resume BLE ads ----
void   apiPortalStateChanged(bool nowActive);

// True while a LoRa packet is actually on the air.
//
// Transmission is asynchronous now: apiRawTransmit() starts the packet and
// then waits, servicing the web server while it waits, because at SF12 a
// full-length packet occupies the radio for over ten seconds and a phone
// waiting on the portal page gives up long before that. Anything that
// would disturb the radio mid-packet has to check this first.
bool   apiRadioBusy();

// Called from the HTTP handler the first time a browser actually fetches
// the page. It only raises a flag - loop() does the repaint, because
// drawing on the e-paper from inside a request handler would block the
// very response that proves the page loaded.
void   apiPortalPageServed();

// Called when the access point could not be started at all.
void   apiPortalFailed();

#endif  // NODE_API_H
