// ============================================================
// Handheld LoRa Communicator  -  ESP32 + SX1278 + 1.54" e-paper
// Bangla messaging, BLE phone link, and an on-demand Wi-Fi portal.
//
// Fleet of 20 modules: broadcast by default; addressed private requests.
// Assign one unique ID A0-B9 per module using the buttons or webpage.
//
// IMPORTANT BUILD SETTING - read README_WEBPORTAL.md first:
//   Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"
// The default 1.31 MB app partition cannot hold BLE + Wi-Fi + the
// web server together. You will get "text section exceeds available
// space in board" without this.
// ============================================================

#include <SPI.h>
#include <LoRa.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include "BanglaAssets.h"        // put all .h files in the same sketch folder
#include "BanglaFallbackFont.h"
#include "BanglaUi.h"            // Bangla labels for the ID, Wi-Fi and link screens
#include "NodeApi.h"             // [WEB] shared sizes + the .ino <-> portal seam
#include "MsgStore.h"            // [WEB] inbox on LittleFS, not NVS
#include "Link.h"                // Broadcast and approved private sessions
#include "WebPortal.h"           // [WEB]

// ============================================================
// E-Paper
// ============================================================

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>

// ============================================================
// Button/long-press types - deliberately placed immediately after
// the includes, before anything else. Arduino auto-generates
// prototypes for pollButton()/pollLongPress() and hoists them near
// the top of the file; if ButtonEvent/ButtonState/LongPress were
// still defined further down (as originally), that hoisted
// prototype would reference a not-yet-declared type and fail to
// compile ("does not name a type"). Keeping the types here, ahead
// of the hoist point, avoids that regardless of where Arduino
// places the auto-generated prototypes.
// ============================================================

enum ButtonEvent { EVT_NONE, EVT_SINGLE, EVT_DOUBLE };

struct ButtonState {
  int pin;
  bool lastReading = HIGH;
  unsigned long lastDebounceTime = 0;
  int clickCount = 0;
  unsigned long firstClickTime = 0;
};

struct LongPress {
  int pin;
  unsigned long start = 0;
  bool wasPressed = false;
  bool fired = false;
};

// MenuState lives up here with the other types for exactly the same
// reason: needsFullPaint(MenuState) takes one as a parameter, so the
// auto-generated prototype for it names MenuState near the top of the
// file. Declared any further down and that prototype would not compile.
enum MenuState {
  MENU_NONE,           // idle, nothing shown
  MENU_RECEIVE_TOP,    // "Message Received" + 3 options
  MENU_VIEW_RECEIVED,  // shows the actual received text (option 1)
  MENU_SENT_TOP,       // "Message Sent" + 2 options
  MENU_PREDEFINED,     // browsing the 20 predefined messages
  MENU_MEMORY,         // browsing the stored received messages
  MENU_PORTAL,         // [WEB] Wi-Fi portal info screen
  MENU_PAIR,           // Private request / connection status
  MENU_FLEET,          // ID chooser: our own on first boot, a peer after
  MENU_LINK_STATUS     // Current mode and identity
};

#define EPD_CS   5
#define EPD_DC   26
#define EPD_RST  25
#define EPD_BUSY 33

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// Display stays powered/initialized continuously (no hibernate) - see
// earlier notes: hibernate/reinit was unreliable on this panel.
#define DISPLAY_HOLD_MS 30000   // auto-exit menu after this much idle time

// [BATT] Top 18 px of every screen are now a status strip (battery +
// Wi-Fi indicator), so all content starts below it.
#define CONTENT_TOP 22

// ============================================================
// [PARTIAL] Screen layout
//
// Every list screen is built from four fixed bands so that a partial
// update can touch exactly one of them:
//
//    y   0 .. 18   status strip  (ID, battery, Wi-Fi marker)
//    y  21 .. 41   title bitmap
//    y  44 ..168   list area  -  LIST_ROWS rows of LIST_ROW_H px
//    y 172 ..199   button hints
//
// Only the list area is repainted while you scroll, which is what makes
// scrolling take a fraction of a second instead of the ~2 s a
// whole-screen refresh costs on this panel.
// ============================================================
#define SCREEN_W      200
#define SCREEN_H      200
#define STATUS_H      19
#define TITLE_Y       21
#define LIST_TOP      44
#define LIST_ROW_H    25
#define LIST_ROWS      5
#define LIST_H        (LIST_ROW_H * LIST_ROWS)     // 125 -> ends at y=169
#define HINT_Y        172
#define HINT_H        (SCREEN_H - HINT_Y)

// E-paper partial updates leave a faint trace of what was there before.
// After this many of them the next repaint is promoted to a full
// refresh, which clears the panel properly. 24 partials is roughly two
// full passes through a 20-item list.
#define FULL_REFRESH_EVERY 24

bool showingMessage = false;
unsigned long messageShownAt = 0;

// ============================================================
// Forward declarations
// (Arduino generates these automatically, but being explicit keeps
//  the NodeApi.h signatures and the definitions below in lockstep.)
// ============================================================

void drawReceiveTopMenu();
void drawViewReceivedMessage();
void drawSentTopMenu();
void drawPredefinedMenu();
void drawMemoryMenu();
void drawPortalScreen();
void drawPairScreen();
void drawFleetMenu();
void drawLinkStatusScreen();
void redrawCurrentScreen();
void clearDisplay();
void exitMenu();
void handleIncomingMessage(const String &text, int rssi);
void enterSentTopMenu(const String &sentText);
bool transmitLoRaMessage(String message);
void refreshBattery(bool force);

// [PARTIAL] region painters and list plumbing
void beginFullPaint();
void beginRegion(int x, int y, int w, int h);
bool regionNextPage();
void endPaint();
void drawStatusBar();
void repaintStatusBar();
bool repaintedInFull();
void drawListArea();
void repaintListArea();
void repaintListRows(int firstRow, int lastRow);
void drawHintBar();
int  listTotal();
void listMoveBy(int delta);
bool identityNeeded();
void openIdentityChooser();
void drawHintsForScreen();
void repaintHintBar();
void flashHint(const uint8_t *bmp, int w, int h, unsigned long ms);
void serviceHintFlash();
void goHome();
void goHomeIn(unsigned long ms);
void serviceScheduledHome();
void handleBackPress();
void servicePortalScreen();
// (drawUTF8Text is intentionally NOT forward-declared here - its default
//  argument lives on the definition, same as in the original sketch, so
//  the IDE's auto-generated prototype cannot clash with it.)

// ============================================================
// [BATT] Battery monitor
//
// MUST be an ADC1 pin. ADC2 (GPIO 0/2/4/12-15/25-27) is physically
// unavailable to the CPU whenever the Wi-Fi radio is running, so a
// battery sense pin on ADC2 would read garbage exactly when the
// portal is up. GPIO 34 is ADC1, input-only, and free on this build.
//
// Wiring:  BAT+ --[ 100k ]--+--[ 100k ]-- GND
//                           |
//                        GPIO 34        (optional 100 nF to GND)
//
// 100k/100k halves the voltage (4.2 V -> 2.1 V, comfortably inside
// the 11 dB ADC range) and draws ~21 uA. If that idle drain matters,
// use 1M/1M and fit the 100 nF cap - the ESP32 ADC needs a low
// source impedance and the cap supplies the sampling charge.
// ============================================================

#define BATT_ADC_PIN    34
#define BATT_DIV_RATIO  2.0f     // (R_top + R_bottom) / R_bottom
#define BATT_SAMPLES    16
#define BATT_MIN_MV     400      // below this: assume no divider fitted
#define BATT_CACHE_MS   10000UL

float         battVolts   = 0.0f;
int           battPercent = -1;   // -1 = not present / not wired yet
unsigned long battReadAt  = 0;

// Single-cell Li-ion discharge curve (resting voltage -> %). A linear
// 3.0-4.2 V map is badly wrong in the middle of the range, where the
// cell sits near 3.8 V for most of its life.
struct BattPoint { float v; int p; };
static const BattPoint battCurve[] = {
  {4.20f,100},{4.15f, 95},{4.11f, 90},{4.08f, 85},{4.02f, 80},
  {3.98f, 75},{3.95f, 70},{3.91f, 65},{3.87f, 60},{3.85f, 55},
  {3.84f, 50},{3.82f, 45},{3.80f, 40},{3.79f, 35},{3.77f, 30},
  {3.75f, 25},{3.73f, 20},{3.71f, 15},{3.69f, 10},{3.61f,  5},
  {3.27f,  0}
};

int liionPercent(float v) {
  const int n = sizeof(battCurve) / sizeof(battCurve[0]);
  if (v >= battCurve[0].v)     return 100;
  if (v <= battCurve[n - 1].v) return 0;
  for (int i = 1; i < n; i++) {
    if (v >= battCurve[i].v) {
      float span = battCurve[i - 1].v - battCurve[i].v;
      float f    = (span > 0.0f) ? (v - battCurve[i].v) / span : 0.0f;
      return battCurve[i].p + (int)lroundf(f * (battCurve[i - 1].p - battCurve[i].p));
    }
  }
  return 0;
}

void refreshBattery(bool force) {
  if (!force && battReadAt != 0 && (millis() - battReadAt) < BATT_CACHE_MS) return;
  battReadAt = millis();

  uint32_t sum = 0;
  for (int i = 0; i < BATT_SAMPLES; i++) {
    sum += analogReadMilliVolts(BATT_ADC_PIN);   // uses the eFuse ADC calibration
    delayMicroseconds(300);
  }
  float mv = (float)sum / BATT_SAMPLES;

  if (mv < BATT_MIN_MV) {          // nothing connected to the divider
    battVolts   = 0.0f;
    battPercent = -1;
    return;
  }
  battVolts   = mv * BATT_DIV_RATIO / 1000.0f;
  battPercent = liionPercent(battVolts);
}

// Converts an integer to Bangla numerals (U+09E6 + digit) as UTF-8.
// These live in the fallback font's isolated-glyph table with correct
// baseline offsets, so drawUTF8Text() renders them properly - no new
// bitmap assets needed.
String toBanglaDigits(int value) {
  String in  = String(value);
  String out = "";
  for (int i = 0; i < (int)in.length(); i++) {
    char c = in.charAt(i);
    if (c >= '0' && c <= '9') {
      uint32_t cp = 0x09E6 + (c - '0');
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += c;
    }
  }
  return out;
}

// ============================================================
// Push Buttons
//   BUTTON_NAV : single = DOWN, double = UP
//   BUTTON_SEL : single = ENTER/SEND, double = BACK,
//                LONG   = display sleep/wake toggle
//   BUTTON_FN  : single = open/scroll fleet list,
//                double = disconnect/cancel private session,
//                LONG   = toggle the Wi-Fi portal   (moved here from NAV)
// Wiring: one leg to the GPIO, other leg to GND. No resistor
// needed - internal pull-up is enabled in code.
// ============================================================

#define BUTTON_NAV 13
#define BUTTON_SEL 14

// [FLEET] Third button. GPIO 27 is a plain digital pin with an internal
// pull-up, no strapping role, and free in this design. (GPIO 32 works
// equally well if 27 is more convenient on your board.)
// Wire it exactly like the other two: one leg to GPIO 27, other to GND.
#define BUTTON_FN  27

#define DEBOUNCE_MS            40
#define DOUBLE_CLICK_WINDOW_MS 350

ButtonState navButton  = { BUTTON_NAV };
ButtonState selButton  = { BUTTON_SEL };
ButtonState fnButton   = { BUTTON_FN };   // [FLEET]

ButtonEvent pollButton(ButtonState &btn) {
  bool reading = digitalRead(btn.pin);
  ButtonEvent event = EVT_NONE;

  // Detect a press (falling edge, active LOW) with simple debounce
  if (reading == LOW && btn.lastReading == HIGH &&
      (millis() - btn.lastDebounceTime > DEBOUNCE_MS)) {
    btn.lastDebounceTime = millis();
    btn.clickCount++;
    if (btn.clickCount == 1) {
      btn.firstClickTime = millis();
    }
  }
  btn.lastReading = reading;

  // Only resolve once the button is released AND the double-click
  // window has passed - prevents a still-held long-press from also
  // resolving as a short click partway through the hold.
  if (btn.clickCount > 0 && reading == HIGH &&
      (millis() - btn.firstClickTime > DOUBLE_CLICK_WINDOW_MS)) {
    event = (btn.clickCount == 1) ? EVT_SINGLE : EVT_DOUBLE;
    btn.clickCount = 0;
  }

  return event;
}

void resetButtons() {
  navButton.clickCount = 0;
  selButton.clickCount = 0;
  fnButton.clickCount  = 0;
}

// ============================================================
// Menu State Machine
// ============================================================

MenuState menuState  = MENU_NONE;
MenuState parentMenu = MENU_RECEIVE_TOP;  // which top menu opened the current sub-menu

int topMenuIndex   = 0;
int predefinedIndex = 0;
int memoryIndex     = 0;
int fleetIndex = 0;

// ============================================================
// [PARTIAL] What is currently on the glass, and where the list is
// scrolled to.
//
// listTop is the index drawn in the first visible row and listSel is
// the highlighted one. Keeping them apart is what allows "the top item
// vanishes and a new one appears at the bottom" to be a single partial
// update of the list band rather than a whole-screen repaint.
// ============================================================
MenuState paintedScreen  = MENU_NONE;   // screen the panel is showing
int  listTop = 0;                       // first visible item
int  listSel = 0;                       // highlighted item
int  partialsSinceFull = 0;             // ghosting budget
bool fleetAssignMode = false;           // true = picking OUR id, not a peer

String lastReceivedText = "";
String lastSentText = "";

// [WEB] link telemetry, surfaced on the portal's Radio tab
int      lastRssiVal = 0;
float    lastSnrVal  = 0.0f;
uint32_t txCountVal  = 0;
uint32_t rxCountVal  = 0;

// ============================================================
// Long presses
//   SEL long  (800 ms)  -> display sleep/wake toggle
//   FN long (1200 ms)   -> Wi-Fi portal on/off
// FN is given the longer hold so an impatient double-tap while
// navigating a list can never accidentally light up the radio.
// ============================================================

#define LONG_PRESS_MS      800
#define FN_LONG_PRESS_MS  1200

LongPress selLP = { BUTTON_SEL };
LongPress fnLP  = { BUTTON_FN };   // [FLEET] Wi-Fi portal moved to FN

// Returns true exactly once, at the moment the hold crosses holdMs.
bool pollLongPress(LongPress &lp, unsigned long holdMs) {
  bool pressed = (digitalRead(lp.pin) == LOW);
  bool hit = false;

  if (pressed && !lp.wasPressed) {
    lp.start = millis();
    lp.fired = false;
  }
  if (pressed && !lp.fired && (millis() - lp.start > holdMs)) {
    lp.fired = true;
    hit = true;
  }
  lp.wasPressed = pressed;
  return hit;
}

void checkLongPresses() {
  // ---- SEL long press: display sleep/wake ----
  if (pollLongPress(selLP, LONG_PRESS_MS)) {
    selButton.clickCount = 0;

    if (menuState == MENU_NONE) {
      Serial.println("Long press: waking display.");
      // The ID chooser appears here on the first wake after flashing and
      // never again: once an ID is committed to NVS, identityNeeded() is
      // false for the life of the device (see Link.h).
      if (identityNeeded()) { openIdentityChooser(); return; }
      menuState = MENU_PREDEFINED;
      predefinedIndex = 0;
      listSel = 0;
      listTop = 0;
      parentMenu = MENU_RECEIVE_TOP;
      resetButtons();
      drawPredefinedMenu();
    } else {
      Serial.println("Long press: putting display to sleep.");
      exitMenu();
    }
  }
}

// ============================================================
// LoRa Configuration
// ============================================================

#define SS   15
#define RST  4
#define DIO0 2

#define LORACFG_NAMESPACE "loracfg"

// [WEB] Radio settings are now runtime-changeable from the portal and
// persisted in NVS. These are the compile-time defaults / first boot.
LoRaCfg loraCfg = { 433000000L, 12, 125000L, 8, 20, 0x12 };

// ============================================================
// BLE Nordic UART Service (NUS)
// ============================================================

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ============================================================
// BLE Objects
// ============================================================

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
BLECharacteristic *pRxCharacteristic = NULL;

volatile bool deviceConnected = false;
bool oldDeviceConnected = false;

// ============================================================
// Message Buffers
// ============================================================

String btMessage = "";
char pendingLoRaText[MAX_TX_BYTES+1] = {0};
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool loRaTransmitPending = false;
uint32_t pendingRouteRevision = 0;

// ============================================================
// BLE Server Callbacks
// ============================================================

class ServerCallbacks : public BLEServerCallbacks {

  void onConnect(BLEServer *pServer) override {
    deviceConnected = true;
    Serial.println("BLE client connected.");
  }

  void onDisconnect(BLEServer *pServer) override {
    deviceConnected = false;
    Serial.println("BLE client disconnected.");
    // Do NOT restart advertising here - handled safely in loop().
  }
};

// ============================================================
// BLE RX Callback
// ============================================================

class RxCallbacks : public BLECharacteristicCallbacks {

  void onWrite(BLECharacteristic *pCharacteristic) override {

    String rxValue = pCharacteristic->getValue();
    if (rxValue.length() == 0) return;

    String message = "";
    for (int i = 0; i < rxValue.length(); i++) {
      char c = rxValue.charAt(i);
      if (c != '\n' && c != '\r') message += c;
    }

    if (message.length() > 0) {
      // IMPORTANT: do NOT touch LoRa here - just queue it.
      bool queued=apiQueueOutgoing(message);
      Serial.println(queued ? "BLE message queued" : "BLE send rejected: busy or no route");
    }
  }
};

// ============================================================
// Persistent Storage (NVS via Preferences)
// ============================================================

Preferences prefs;

// ---- Received messages ----
// [WEB] These used to be a 15-slot NVS ring buffer. NVS could not
// hold MAX_STORED_MESSAGES=100 safely (measured: 80 messages at the
// 180-byte cap needs 633 of the 504 available entries and overflows),
// so the inbox now lives in a fixed-record file on LittleFS. See
// MsgStore.h for the reasoning. These are thin wrappers so the rest
// of the sketch is unchanged.

void saveReceivedMessage(const String &msg, int rssi) {
  msgStoreAdd(msg, rssi);
}

// n = 0 is the most recently received message
String getStoredMessage(int n) { return msgStoreGet(n); }
int    getStoredMessageCount() { return msgStoreCount(); }

void clearStoredMessages() {
  msgStoreClear();
  memoryIndex = 0;
}

// Only the newest few - printing 100 lines at every boot is noise.
void printAllStoredMessages() {
  int count = getStoredMessageCount();
  int show  = (count < 10) ? count : 10;

  Serial.println("---- Stored messages (" + String(count) + " of " +
                 String(MAX_STORED_MESSAGES) + ", newest first) ----");
  for (int i = 0; i < show; i++) {
    Serial.println(String(i + 1) + ": " + getStoredMessage(i));
  }
  if (count > show) Serial.println("... and " + String(count - show) + " more");
  Serial.println("--------------------------------");
}

// ---- Predefined messages: fixed set of 20 ----
// Bump PREDEFINED_DEFAULTS_VERSION whenever you edit defaults[] below -
// that's what forces flash to be rewritten instead of keeping old data
// left over from earlier testing.
// (NUM_PREDEFINED / NUM_PREDEFINED_WITH_BITMAP now live in NodeApi.h)
#define PREDEFINED_NAMESPACE "predefmsgs"
#define PREDEFINED_DEFAULTS_VERSION 5

// [WEB] Bit i set => preset i was edited over the web, so its
// pixel-perfect bitmap no longer matches the text and must not be used.
uint32_t presetEditedMask = 0;

static const char* const predefinedDefaults[NUM_PREDEFINED] = {
  "আমার সাহায্য দরকার",
  "আপনি কোথায়?",
  "এখানে আসুন",
  "সব ঠিক আছে",
  "আমাকে কল করুন",
  "আমি নিরাপদে আছি",
  "আমি আটকা পড়েছি",
  "খাবার দরকার",
  "পানি দরকার",
  "ওষুধ দরকার",
  "আহত হয়েছি",
  "তাড়াতাড়ি আসুন",
  "বিপদ আছে",
  "সামনে বাধা",
  "যোগাযোগ করুন",
  "আমি আসছি",
  "রাস্তা বন্ধ",
  "সবাই নিরাপদ",
  "আশ্রয় দরকার",
  "সংকেত পাঠান"
};

// [PARTIAL] Presets are held in RAM as well as NVS.
//
// The list screen draws five rows at a time, and deciding how to draw a
// row asks "is this text one of the 20 presets?". Answering that from
// flash meant up to 20 Preferences open/read/close calls PER ROW - a
// hundred per repaint, which turns a 0.3 s partial update into a
// visibly slow one. Twenty strings is about 1.2 KB of heap; the flash
// copy is still the authority, this is just a read cache.
String presetCache[NUM_PREDEFINED];

void loadPresetCache() {
  prefs.begin(PREDEFINED_NAMESPACE, true);
  for (int i = 0; i < NUM_PREDEFINED; i++) {
    presetCache[i] = prefs.getString(("p" + String(i)).c_str(), predefinedDefaults[i]);
  }
  prefs.end();
}

void initializePredefinedMessages(bool force) {
  prefs.begin(PREDEFINED_NAMESPACE, false);

  int storedVersion = prefs.getInt("version", 0);

  if (force || storedVersion != PREDEFINED_DEFAULTS_VERSION) {
    for (int i = 0; i < NUM_PREDEFINED; i++) {
      prefs.putString(("p" + String(i)).c_str(), predefinedDefaults[i]);
    }
    prefs.putInt("version", PREDEFINED_DEFAULTS_VERSION);
    prefs.putUInt("edited", 0);          // all bitmaps valid again
    presetEditedMask = 0;
    Serial.println("Predefined messages (re)initialized in flash.");
  } else {
    presetEditedMask = prefs.getUInt("edited", 0);
  }

  prefs.end();
  loadPresetCache();
}

String getPredefinedMessage(int index) {
  if (index < 0 || index >= NUM_PREDEFINED) return "";
  return presetCache[index];
}

// [WEB] overwrite one preset and mark its bitmap as stale
void setPredefinedMessage(int index, const String &text) {
  if (index < 0 || index >= NUM_PREDEFINED) return;

  prefs.begin(PREDEFINED_NAMESPACE, false);
  prefs.putString(("p" + String(index)).c_str(), text);

  if (text == predefinedDefaults[index]) presetEditedMask &= ~(1UL << index);
  else                                   presetEditedMask |=  (1UL << index);

  prefs.putUInt("edited", presetEditedMask);
  prefs.end();

  presetCache[index] = text;      // keep the cache and flash in step

  Serial.println("Preset " + String(index) + " set to: " + text);
}

// Bitmap lookup for all 20 predefined messages - all pixel-perfect
// (index-aligned with predefinedDefaults[0..19] above).
const uint8_t* const predefinedBitmaps[NUM_PREDEFINED_WITH_BITMAP] = {
  bn_predef0, bn_predef1, bn_predef2, bn_predef3, bn_predef4,
  bn_predef5, bn_predef6, bn_predef7, bn_predef8, bn_predef9,
  bn_predef10, bn_predef11, bn_predef12, bn_predef13, bn_predef14,
  bn_predef15, bn_predef16, bn_predef17, bn_predef18, bn_predef19
};
const int predefinedBitmapW[NUM_PREDEFINED_WITH_BITMAP] = {
  bn_predef0_w, bn_predef1_w, bn_predef2_w, bn_predef3_w, bn_predef4_w,
  bn_predef5_w, bn_predef6_w, bn_predef7_w, bn_predef8_w, bn_predef9_w,
  bn_predef10_w, bn_predef11_w, bn_predef12_w, bn_predef13_w, bn_predef14_w,
  bn_predef15_w, bn_predef16_w, bn_predef17_w, bn_predef18_w, bn_predef19_w
};
const int predefinedBitmapH[NUM_PREDEFINED_WITH_BITMAP] = {
  bn_predef0_h, bn_predef1_h, bn_predef2_h, bn_predef3_h, bn_predef4_h,
  bn_predef5_h, bn_predef6_h, bn_predef7_h, bn_predef8_h, bn_predef9_h,
  bn_predef10_h, bn_predef11_h, bn_predef12_h, bn_predef13_h, bn_predef14_h,
  bn_predef15_h, bn_predef16_h, bn_predef17_h, bn_predef18_h, bn_predef19_h
};

// [WEB] true only if this preset still has a valid pixel-perfect bitmap
bool presetHasBitmap(int i) {
  return i >= 0 && i < NUM_PREDEFINED_WITH_BITMAP && !(presetEditedMask & (1UL << i));
}

// Returns 0-19 if text exactly matches a predefined message, else -1
int matchPredefinedIndex(const String &text) {
  for (int i = 0; i < NUM_PREDEFINED; i++) {
    if (getPredefinedMessage(i) == text) return i;
  }
  return -1;
}

// ============================================================
// E-Paper Drawing Helpers
// ============================================================

// Simple word-wrap so long messages don't run off the 200px-wide screen
void printWrapped(const String &text, int x, int startY, int maxCharsPerLine) {
  int lineHeight = 20;
  int line = 0;
  int start = 0;

  while (start < (int)text.length()) {
    int end = min(start + maxCharsPerLine, (int)text.length());

    if (end < (int)text.length()) {
      int lastSpace = text.lastIndexOf(' ', end);
      if (lastSpace > start) end = lastSpace;
    }

    String lineText = text.substring(start, end);
    lineText.trim();

    display.setCursor(x, startY + (line * lineHeight));
    display.print(lineText);

    start = end;
    while (start < (int)text.length() && text.charAt(start) == ' ') start++;
    line++;
  }
}

void markScreenActive() {
  showingMessage = true;
  messageShownAt = millis();
}

// ---- Mixed Bangla + English text renderer (fallback for custom text) ----
// NOTE: draws each Bangla character as an independent glyph - matra
// reordering and conjunct-cluster merging are NOT performed. This is
// a deliberate, known trade-off: it's readable for many words but not
// typographically correct for all of them (see BanglaFallbackFont.h).

int asciiAdvance = 11;   // measured once in setup() via measureAsciiMetrics()

void measureAsciiMetrics() {
  display.setFont(&FreeMonoBold9pt7b);
  int16_t tbx, tby;
  uint16_t tbw, tbh;
  display.getTextBounds("M", 0, 0, &tbx, &tby, &tbw, &tbh);
  asciiAdvance = tbw + 2;
}

// Decodes one UTF-8 codepoint starting at byte index i, advances i
// past the bytes consumed. Handles 1-byte (ASCII) and 3-byte (Bangla
// block, and other BMP characters) sequences.
uint32_t decodeUTF8(const String &s, int &i) {
  uint8_t b0 = (uint8_t)s.charAt(i);

  if (b0 < 0x80) {
    i += 1;
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0 && i + 1 < (int)s.length()) {
    uint8_t b1 = (uint8_t)s.charAt(i + 1);
    uint32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
    i += 2;
    return cp;
  }
  if ((b0 & 0xF0) == 0xE0 && i + 2 < (int)s.length()) {
    uint8_t b1 = (uint8_t)s.charAt(i + 1);
    uint8_t b2 = (uint8_t)s.charAt(i + 2);
    uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    i += 3;
    return cp;
  }

  i += 1;   // unknown/unsupported sequence - skip one byte
  return '?';
}

// Some phone keyboards type ড়/ঢ়/য় as two separate codepoints (a plain
// consonant + a separate nukta dot mark, U+09BC) instead of the single
// combined letter. Detect that pattern and fold it into the equivalent
// precomposed letter so it flows into the normal cluster lookup below,
// instead of leaving a bare nukta mark to render as its own isolated
// (dotted-circle) glyph.
//
// [WEB] This path matters much more now: web-typed text comes straight
// from the phone's Bangla IME, which is exactly where decomposed
// nukta sequences come from.
uint32_t normalizeNukta(uint32_t cp, const String &text, int &i) {
  if (i >= (int)text.length()) return cp;

  int peek = i;
  uint32_t nextCp = decodeUTF8(text, peek);

  if (nextCp != 0x09BC) return cp;   // no nukta follows - nothing to do

  uint32_t precomposed = 0;
  if (cp == 0x09A1) precomposed = 0x09DC;        // ড + nukta -> ড়
  else if (cp == 0x09A2) precomposed = 0x09DD;   // ঢ + nukta -> ঢ়
  else if (cp == 0x09AF) precomposed = 0x09DF;   // য + nukta -> য়

  if (precomposed != 0) {
    i = peek;   // consume the nukta as well
    return precomposed;
  }
  return cp;
}

// Looks up a codepoint's index in the consonant table, or -1
int consonantIndexOf(uint32_t cp) {
  for (int i = 0; i < NUM_BN_CONSONANTS; i++) {
    if (pgm_read_word(&bnConsonants[i]) == cp) return i;
  }
  return -1;
}

// Looks up a codepoint's index in the matra (vowel sign) table, or -1
int matraIndexOf(uint32_t cp) {
  for (int i = 0; i < NUM_BN_MATRAS; i++) {
    if (pgm_read_word(&bnMatras[i]) == cp) return i;
  }
  return -1;
}

// Looks up a consonant+virama+consonant pair in the 24-entry known
// conjunct table, or -1 if this specific pair isn't covered
int conjunctIndexOf(uint32_t c1, uint32_t c2) {
  for (int i = 0; i < NUM_BN_CONJUNCTS; i++) {
    if (pgm_read_word(&bnConjunctC1[i]) == c1 && pgm_read_word(&bnConjunctC2[i]) == c2) return i;
  }
  return -1;
}

// Draws UTF-8 text (ASCII + Bangla block) with simple pixel-width
// wrapping. `y` is the BASELINE of the first line.
//
// maxLines (0 = unlimited) caps how far the text may grow downwards.
// [WEB] This matters now: a web-typed message can be up to
// MAX_TX_BYTES (~60 Bangla characters), which is four or five wrapped
// lines - enough to run straight through the menu rows underneath.
// When text is cut short, ".." is drawn at the end of the last line.
//
// Rendering priority for Bangla:
//   1. consonant + virama + consonant, IF the pair is one of the 24
//      known conjuncts -> properly shaped merged ligature
//   2. consonant + vowel-sign (matra) -> properly shaped cluster
//   3. anything else in the Bangla block -> isolated glyph (fallback)
void drawUTF8Text(const String &text, int x, int y, int maxWidth, int lineHeight,
                  bool wrap = true, int maxLines = 0) {
  display.setFont(&FreeMonoBold9pt7b);

  int cursorX = x;
  int cursorY = y;
  int i = 0;
  int len = text.length();
  int line = 1;
  bool truncated = false;

  // Wraps to the next line, or reports that we've run out of room.
  auto nextLine = [&]() -> bool {
    if (!wrap) { truncated = true; return false; }
    if (maxLines > 0 && line >= maxLines) { truncated = true; return false; }
    cursorX = x;
    cursorY += lineHeight;
    line++;
    return true;
  };

  while (i < len) {
    int afterFirst = i;
    uint32_t cp = decodeUTF8(text, afterFirst);

    if (cp >= BN_GLYPH_FIRST && cp <= BN_GLYPH_LAST) {
      cp = normalizeNukta(cp, text, afterFirst);   // fold decomposed ড়/ঢ়/য় if present

      int consIdx = consonantIndexOf(cp);

      // Peek ahead for a known consonant+virama+consonant conjunct
      int conjIdx = -1;
      int afterConjunct = afterFirst;
      if (consIdx >= 0 && afterFirst < len) {
        int peek1 = afterFirst;
        uint32_t maybeVirama = decodeUTF8(text, peek1);
        if (maybeVirama == 0x09CD && peek1 < len) {
          int peek2 = peek1;
          uint32_t secondCons = decodeUTF8(text, peek2);
          conjIdx = conjunctIndexOf(cp, secondCons);
          if (conjIdx >= 0) afterConjunct = peek2;
        }
      }

      if (conjIdx >= 0) {
        int gw = pgm_read_byte(&bnConjunctW[conjIdx]);
        int gh = pgm_read_byte(&bnConjunctH[conjIdx]);
        int adv = pgm_read_byte(&bnConjunctXAdv[conjIdx]);

        if (cursorX + adv > x + maxWidth) {
          if (!nextLine()) break;
        }
        if (gw > 0 && gh > 0) {
          int offset = pgm_read_word(&bnConjunctOffset[conjIdx]);
          display.drawBitmap(cursorX + (int8_t)pgm_read_byte(&bnConjunctXOff[conjIdx]),
                              cursorY + (int8_t)pgm_read_byte(&bnConjunctYOff[conjIdx]),
                              &bnConjunctBitmap[offset], gw, gh, GxEPD_BLACK);
        }
        cursorX += adv;
        i = afterConjunct;   // consumed all three codepoints
        continue;
      }

      // Peek the next codepoint to check for a consonant+matra cluster
      int matraIdx = -1;
      int afterSecond = afterFirst;
      if (consIdx >= 0 && afterFirst < len) {
        uint32_t nextCp = decodeUTF8(text, afterSecond);
        matraIdx = matraIndexOf(nextCp);
      }

      if (consIdx >= 0 && matraIdx >= 0) {
        int ci = consIdx * NUM_BN_MATRAS + matraIdx;
        int gw = pgm_read_byte(&bnClusterW[ci]);
        int gh = pgm_read_byte(&bnClusterH[ci]);
        int adv = pgm_read_byte(&bnClusterXAdv[ci]);

        if (cursorX + adv > x + maxWidth) {
          if (!nextLine()) break;
        }
        if (gw > 0 && gh > 0) {
          int offset = pgm_read_word(&bnClusterOffset[ci]);
          display.drawBitmap(cursorX + (int8_t)pgm_read_byte(&bnClusterXOff[ci]),
                              cursorY + (int8_t)pgm_read_byte(&bnClusterYOff[ci]),
                              &bnClusterBitmap[offset], gw, gh, GxEPD_BLACK);
        }
        cursorX += adv;
        i = afterSecond;   // consumed both codepoints

      } else {
        int gi = cp - BN_GLYPH_FIRST;
        int gw = bnGlyphW[gi];
        int gh = bnGlyphH[gi];
        int adv = bnGlyphXAdv[gi];

        if (cursorX + adv > x + maxWidth) {
          if (!nextLine()) break;
        }
        if (gw > 0 && gh > 0) {
          int offset = bnGlyphOffset[gi];
          display.drawBitmap(cursorX + bnGlyphXOff[gi], cursorY + bnGlyphYOff[gi],
                              &bnGlyphBitmap[offset], gw, gh, GxEPD_BLACK);
        }
        cursorX += adv;
        i = afterFirst;   // consumed just this one codepoint
      }

    } else {
      char c = (char)cp;
      int adv = asciiAdvance;

      if (cursorX + adv > x + maxWidth) {
        if (!nextLine()) break;
      }

      // CHANGED: was cursorY + 12. Adafruit_GFX treats the cursor y as
      // the baseline, and the Bangla glyph table above is also
      // baseline-relative - so +12 pushed ASCII 12 px below the Bangla
      // on the same line. Mixed Bangla/English now sits on one
      // baseline. (Revert to cursorY + 12 if you preferred the old
      // English-only vertical position.)
      display.setCursor(cursorX, cursorY);
      display.print(c);
      cursorX += adv;
      i = afterFirst;
    }
  }

  // Text did not fit: mark it so the reader knows there is more.
  if (truncated && i < len) {
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(x + maxWidth - 2 * asciiAdvance, cursorY);
    display.print("..");
  }
}

// Draws a message body. `y` is the TOP of the text band in BOTH
// branches now: the bitmap branch takes a top-left origin, and the
// fallback renderer wants a baseline, so +16 is added there.
// maxLines is how many 24 px lines the caller has room for before it
// runs into whatever is drawn below (a menu strip, or the bottom edge).
void drawMessageBody(const String &text, int x, int y, int maxLines) {
  int idx = matchPredefinedIndex(text);
  if (idx >= 0 && presetHasBitmap(idx)) {
    display.drawBitmap(x, y, predefinedBitmaps[idx], predefinedBitmapW[idx], predefinedBitmapH[idx], GxEPD_BLACK);
  } else {
    drawUTF8Text(text, x, y + 16, 180, 24, true, maxLines);
  }
}

// ============================================================
// [PARTIAL] Painting primitives
//
// This panel (SSD1681) supports two kinds of update:
//
//   full    - the controller flashes the whole screen black/white a
//             few times and redraws it. Clean, but ~2 s, and visually
//             a hard flicker.
//   partial - only the addressed window is rewritten. ~0.3 s, no
//             flicker, but it leaves a faint ghost of the old pixels
//             that builds up over many updates.
//
// So: a full update whenever the screen you are looking at CHANGES,
// and a partial update for everything that happens while you stay on
// it - scrolling a list, the cursor moving, the battery ticking down,
// the Wi-Fi marker appearing. FULL_REFRESH_EVERY partials force one
// full pass to wipe the accumulated ghosting.
//
// The SSD1681 addresses its RAM a byte at a time horizontally, so a
// partial window is snapped outwards to an 8 px boundary. Doing that
// here rather than letting GxEPD2 round on its own keeps what is
// erased and what is redrawn identical, which is what stops a partial
// update from clipping the left edge of a glyph.
// ============================================================

void beginFullPaint() {
  display.setFullWindow();
  display.firstPage();
  partialsSinceFull = 0;
}

void beginRegion(int x, int y, int w, int h) {
  int x0 = x & ~7;
  int x1 = (x + w + 7) & ~7;
  if (x0 < 0) x0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  if (y < 0) y = 0;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  display.setPartialWindow(x0, y, x1 - x0, h);
  display.firstPage();
}

bool regionNextPage() { return display.nextPage(); }

void endPaint() { partialsSinceFull++; }

// True when the next repaint of the CURRENT screen should be promoted
// to a full refresh: either we have just arrived on a different screen,
// or enough partial updates have stacked up to be worth clearing.
bool needsFullPaint(MenuState target) {
  return paintedScreen != target || partialsSinceFull >= FULL_REFRESH_EVERY;
}

// Spends the ghosting budget.
//
// The region painters below are what run while you hold a button down
// scrolling a list, and they are the ONLY thing running - the screen
// functions that consult needsFullPaint() are not called at all. Without
// this check a long scroll would be an unbroken run of partial updates
// and the ghosting would keep building. Returns true when it has already
// repainted the whole screen, in which case the caller has nothing to do.
bool repaintedInFull() {
  if (partialsSinceFull < FULL_REFRESH_EVERY) return false;
  redrawCurrentScreen();     // needsFullPaint() is true, so this goes full
  return true;
}

// ============================================================
// [BATT] Status strip - drawn at the top of every screen
// Battery percentage in Bangla numerals + a battery icon, the module's
// own ID, and a "WiFi" marker while the portal is up.
// Must be called INSIDE a firstPage()/nextPage() loop.
// ============================================================

void drawStatusBar() {
  const int bx = 170, by = 4, bw = 24, bh = 11;

  display.drawRect(bx, by, bw, bh, GxEPD_BLACK);          // body
  display.fillRect(bx + bw, by + 3, 3, 5, GxEPD_BLACK);   // terminal nub

  if (battPercent >= 0) {
    int fill = (int)(((bw - 4) * battPercent) / 100.0f + 0.5f);
    if (fill > 0) display.fillRect(bx + 2, by + 2, fill, bh - 4, GxEPD_BLACK);

    String d = toBanglaDigits(battPercent);
    int digits = d.length() / 3;              // Bangla numerals are 3 bytes each
    int w = digits * 12;                      // 12 px advance per numeral
    drawUTF8Text(d, bx - 8 - w, 15, w + 6, 16, false);
  } else {
    // No divider fitted yet: show an empty cell with a dash
    display.drawLine(bx + 5, by + bh / 2, bx + bw - 5, by + bh / 2, GxEPD_BLACK);
  }

  // The ID itself stays Latin: it is what the Wi-Fi network is named after
  // and what you read out loud to whoever is holding the other handset, so
  // it has to look exactly like what the portal shows. Everything around
  // it is Bangla.
  display.setFont(NULL); display.setTextSize(1);
  display.setCursor(2, 6);
  display.print(linkNodeId() ? linkName(linkNodeId()) : String("--"));

  int x = 20;
  if (linkIsPaired()) {
    display.setCursor(x, 6);
    display.print(">");
    display.print(linkName(linkPeer()));
    x += 24;
  } else {
    display.drawBitmap(x, 4, bn_ui_sb_all, bn_ui_sb_all_w, bn_ui_sb_all_h, GxEPD_BLACK);
    x += bn_ui_sb_all_w + 6;
  }
  if (portalIsActive()) {
    display.drawBitmap(x, 4, bn_ui_sb_wifi, bn_ui_sb_wifi_w, bn_ui_sb_wifi_h, GxEPD_BLACK);
  }

  display.drawLine(0, 18, SCREEN_W - 1, 18, GxEPD_BLACK);
}

// Repaints just the top strip. Used when only the battery reading, the
// Wi-Fi marker or the peer changed - there is no reason to disturb the
// rest of the screen for that.
void repaintStatusBar() {
  if (menuState == MENU_NONE) return;
  if (repaintedInFull()) return;
  beginRegion(0, 0, SCREEN_W, STATUS_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();
  } while (regionNextPage());
  endPaint();
}

// ============================================================
// Button hints
//
// Three short Bangla labels along the bottom, one per button, so the
// controls are discoverable without a manual. Latin GPIO numbers were
// what used to be here ("27/13: next"); they meant nothing to anyone
// who had not read the source.
// ============================================================

void drawHintPair(int x, const uint8_t *bmp, int w, int h) {
  display.drawBitmap(x, HINT_Y + (HINT_H - h) / 2, bmp, w, h, GxEPD_BLACK);
}

// [FLASH] A short message that takes over the hint strip for a moment.
//
// Pressing SEL on a message used to give no feedback at all until the whole
// screen changed several seconds later, which reads as "the button did not
// work". Now the strip says পাঠানো হচ্ছে while the packet is going out and
// পাঠানো হয়েছে when it is gone, then puts the normal hints back. The strip
// is 28 px tall, so each of these is one small partial update.
const uint8_t *hintFlashBmp = NULL;
int            hintFlashW = 0, hintFlashH = 0;
unsigned long  hintFlashUntil = 0;

// The hints depend on the screen: a list offers next/select/back, the
// pairing screen offers accept/reject, and so on.
void drawHintsForScreen() {
  display.drawLine(0, HINT_Y - 3, SCREEN_W - 1, HINT_Y - 3, GxEPD_BLACK);

  if (hintFlashBmp) {                       // transient message wins
    drawHintPair((SCREEN_W - hintFlashW) / 2, hintFlashBmp, hintFlashW, hintFlashH);
    return;
  }

  switch (menuState) {
    case MENU_PAIR:
    case MENU_LINK_STATUS:
      if (linkState() == LINK_INCOMING) {
        drawHintPair(4,   bn_ui_hint_accept, bn_ui_hint_accept_w, bn_ui_hint_accept_h);
        drawHintPair(110, bn_ui_hint_reject, bn_ui_hint_reject_w, bn_ui_hint_reject_h);
      } else if (linkIsPaired()) {
        drawHintPair(4,   bn_ui_hint_msgs, bn_ui_hint_msgs_w, bn_ui_hint_msgs_h);
        drawHintPair(110, bn_ui_hint_off,  bn_ui_hint_off_w,  bn_ui_hint_off_h);
      } else if (linkState() != LINK_PUBLIC) {
        drawHintPair(4,   bn_ui_wait,        bn_ui_wait_w,        bn_ui_wait_h);
        drawHintPair(110, bn_ui_hint_reject, bn_ui_hint_reject_w, bn_ui_hint_reject_h);
      } else {
        drawHintPair(4,   bn_ui_hint_msgs, bn_ui_hint_msgs_w, bn_ui_hint_msgs_h);
        drawHintPair(110, bn_ui_hint_wifi, bn_ui_hint_wifi_w, bn_ui_hint_wifi_h);
      }
      break;

    case MENU_PORTAL:
      if (portalClientCount() > 0) {
        drawHintPair(4, bn_ui_phone_on, bn_ui_phone_on_w, bn_ui_phone_on_h);
      } else {
        drawHintPair(4, bn_ui_phone_off, bn_ui_phone_off_w, bn_ui_phone_off_h);
      }
      break;

    case MENU_FLEET:
      if (linkScanning()) {
        drawHintPair(4, bn_ui_searching, bn_ui_searching_w, bn_ui_searching_h);
      } else if (fleetAssignMode && linkFreeCount() == 0) {
        drawHintPair(4, bn_ui_no_free_id, bn_ui_no_free_id_w, bn_ui_no_free_id_h);
      } else {
        drawHintPair(4,   bn_ui_hint_next,   bn_ui_hint_next_w,   bn_ui_hint_next_h);
        drawHintPair(70,  bn_ui_hint_select, bn_ui_hint_select_w, bn_ui_hint_select_h);
        if (!fleetAssignMode)
          drawHintPair(150, bn_ui_hint_back, bn_ui_hint_back_w, bn_ui_hint_back_h);
      }
      break;

    default:
      drawHintPair(4,   bn_ui_hint_next,   bn_ui_hint_next_w,   bn_ui_hint_next_h);
      drawHintPair(70,  bn_ui_hint_select, bn_ui_hint_select_w, bn_ui_hint_select_h);
      drawHintPair(150, bn_ui_hint_back,   bn_ui_hint_back_w,   bn_ui_hint_back_h);
      break;
  }
}

void drawHintBar() { drawHintsForScreen(); }

// Repaints only the hint strip - 31 px of the 200, so it is quick and
// leaves whatever you are reading above it untouched.
void repaintHintBar() {
  if (menuState == MENU_NONE) return;
  beginRegion(0, HINT_Y - 3, SCREEN_W, HINT_H + 3);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHintsForScreen();
  } while (regionNextPage());
  endPaint();
}

void flashHint(const uint8_t *bmp, int w, int h, unsigned long ms) {
  hintFlashBmp = bmp; hintFlashW = w; hintFlashH = h;
  hintFlashUntil = millis() + ms;
  repaintHintBar();
}

// Puts the normal hints back once the flash has had its moment.
void serviceHintFlash() {
  if (!hintFlashBmp || (long)(millis() - hintFlashUntil) < 0) return;
  hintFlashBmp = NULL;
  repaintHintBar();
}

// ============================================================
// List model
//
// MENU_PREDEFINED, MENU_MEMORY and MENU_FLEET are all "a list of
// things you scroll with one button and act on with the other", so
// they share one scroll position and one painter. listSel is the
// absolute index; listTop is the index sitting in the first visible
// row. LIST_ROWS items are on screen at once instead of the single
// item the old stored-message screen showed.
// ============================================================

int listTotal() {
  switch (menuState) {
    case MENU_PREDEFINED: return NUM_PREDEFINED;
    case MENU_MEMORY:     return getStoredMessageCount();
    case MENU_FLEET:      return LINK_FLEET_SIZE;
    default:              return 0;
  }
}

// Keeps listTop such that listSel is visible, scrolling by whole rows.
// Returns true when the window itself moved, i.e. the whole list band
// has to be repainted rather than just the two rows the cursor touched.
bool clampListWindow() {
  int total = listTotal();
  int oldTop = listTop;
  if (total <= LIST_ROWS) {
    listTop = 0;
  } else {
    if (listSel < listTop)                listTop = listSel;
    if (listSel > listTop + LIST_ROWS - 1) listTop = listSel - LIST_ROWS + 1;
    if (listTop > total - LIST_ROWS)      listTop = total - LIST_ROWS;
    if (listTop < 0)                      listTop = 0;
  }
  return listTop != oldTop;
}

// Draws one row's content at its absolute screen position.
// `row` is 0..LIST_ROWS-1, `idx` the item it shows.
void drawListRow(int row, int idx) {
  int y = LIST_TOP + row * LIST_ROW_H;
  int total = listTotal();
  if (idx < 0 || idx >= total) return;

  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(4, y + 17);
  display.print(idx == listSel ? ">" : " ");

  switch (menuState) {

    case MENU_PREDEFINED: {
      if (presetHasBitmap(idx)) {
        int h = predefinedBitmapH[idx];
        display.drawBitmap(20, y + (LIST_ROW_H - h) / 2,
                           predefinedBitmaps[idx], predefinedBitmapW[idx], h, GxEPD_BLACK);
      } else {
        // Edited over the web, so the pixel-perfect bitmap no longer
        // matches: fall back to the on-device renderer, one line only.
        drawUTF8Text(getPredefinedMessage(idx), 20, y + 17, 176, LIST_ROW_H, false);
      }
      break;
    }

    case MENU_MEMORY: {
      String text = getStoredMessage(idx);
      int p = matchPredefinedIndex(text);
      if (p >= 0 && presetHasBitmap(p)) {
        int h = predefinedBitmapH[p];
        display.drawBitmap(20, y + (LIST_ROW_H - h) / 2,
                           predefinedBitmaps[p], predefinedBitmapW[p], h, GxEPD_BLACK);
      } else {
        // wrap=false truncates with ".." at the right edge, so a long
        // message occupies exactly one row like every other item.
        drawUTF8Text(text, 20, y + 17, 176, LIST_ROW_H, false);
      }
      break;
    }

    case MENU_FLEET: {
      uint8_t id = (uint8_t)(idx + 1);
      display.setFont(&FreeMonoBold12pt7b);
      display.setCursor(22, y + 19);
      display.print(linkName(id));                 // A0..B9 stays Latin

      if (id == linkNodeId()) {
        // This handset's own row. Same position and treatment as the
        // ব্যবহৃত marker beside a taken ID, so the column reads evenly.
        display.drawBitmap(70, y + (LIST_ROW_H - bn_ui_me_h) / 2,
                           bn_ui_me, bn_ui_me_w, bn_ui_me_h, GxEPD_BLACK);
      } else if (linkIdTaken(id)) {
        // Already owned by another module: labelled, and skipped by
        // the cursor when this screen is assigning our own ID.
        display.drawBitmap(70, y + (LIST_ROW_H - bn_ui_taken_h) / 2,
                           bn_ui_taken, bn_ui_taken_w, bn_ui_taken_h, GxEPD_BLACK);
      }
      break;
    }

    default: break;
  }
}

// Repaints a contiguous run of visible rows. One partial update covering
// the union of the changed rows beats one update per row: on this panel
// each update costs about the same no matter how tall the window is.
void repaintListRows(int firstRow, int lastRow) {
  if (repaintedInFull()) return;
  if (firstRow > lastRow) { int t = firstRow; firstRow = lastRow; lastRow = t; }
  if (firstRow < 0) firstRow = 0;
  if (lastRow > LIST_ROWS - 1) lastRow = LIST_ROWS - 1;

  int y = LIST_TOP + firstRow * LIST_ROW_H;
  int h = (lastRow - firstRow + 1) * LIST_ROW_H;

  beginRegion(0, y, SCREEN_W, h);
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int row = firstRow; row <= lastRow; row++) drawListRow(row, listTop + row);
  } while (regionNextPage());
  endPaint();
  markScreenActive();
}

// The stored-message screen carries a "5/37" counter beside its title
// that changes with the cursor, so its repaint band has to start at the
// title rather than at the first row. It is still one partial update.
void repaintListArea() {
  if (repaintedInFull()) return;
  if (menuState == MENU_MEMORY) {
    beginRegion(0, TITLE_Y, SCREEN_W, LIST_TOP + LIST_H - TITLE_Y);
    do {
      display.fillScreen(GxEPD_WHITE);
      display.drawBitmap(10, TITLE_Y, bn_menu_stored,
                         bn_menu_stored_w, bn_menu_stored_h, GxEPD_BLACK);
      int total = listTotal();
      if (total > 0) {
        String pos = toBanglaDigits(listSel + 1) + "/" + toBanglaDigits(total);
        drawUTF8Text(pos, 130, TITLE_Y + 15, 68, 18, false);
      }
      drawListArea();
    } while (regionNextPage());
    endPaint();
    markScreenActive();
    return;
  }
  repaintListRows(0, LIST_ROWS - 1);
}

// Called from inside a page loop - draws every visible row plus the
// "nothing here" placeholder.
void drawListArea() {
  int total = listTotal();
  if (total == 0) {
    display.drawBitmap(12, LIST_TOP + 16, bn_ui_empty_list,
                       bn_ui_empty_list_w, bn_ui_empty_list_h, GxEPD_BLACK);
    return;
  }
  for (int row = 0; row < LIST_ROWS; row++) drawListRow(row, listTop + row);
}

// Moves the cursor and repaints the smallest region that changed.
// Items that another module already owns are stepped over while this
// screen is being used to assign our own ID.
void listMoveBy(int delta) {
  int total = listTotal();
  if (total <= 0) return;

  int oldSel = listSel, oldTop = listTop;

  for (int guard = 0; guard < total; guard++) {
    listSel = (listSel + delta + total) % total;
    if (menuState != MENU_FLEET) break;
    uint8_t id = (uint8_t)(listSel + 1);
    if (fleetAssignMode) { if (!linkIdTaken(id)) break; }
    else                 { if (id != linkNodeId()) break; }
  }
  if (listSel == oldSel) return;

  bool scrolled = clampListWindow();

  if (scrolled || listTop != oldTop || menuState == MENU_MEMORY) {
    // The window itself moved: the row that was at the top is gone and
    // a new one has appeared at the bottom, so the whole band changes.
    // (MENU_MEMORY always takes this path - its counter changes too.)
    repaintListArea();
  } else {
    // Only the cursor moved. Repaint the span covering the old and new
    // selection, which is normally two adjacent rows.
    repaintListRows(oldSel - listTop, listSel - listTop);
  }
}

// ============================================================
// Screens
//
// Each screen paints itself fully on arrival and then hands over to the
// partial painters above for anything that changes while you stay on it.
// ============================================================

// Primary screen shown when the e-paper wakes because a message was
// RECEIVED: header only (no text yet) + a 3-item menu strip.
void drawReceiveTopMenu() {
  refreshBattery(false);
  bool full = needsFullPaint(MENU_RECEIVE_TOP);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();

    display.drawBitmap(10, CONTENT_TOP, bn_hdr_received, bn_hdr_received_w, bn_hdr_received_h, GxEPD_BLACK);

    const uint8_t* labelBmp[3] = { bn_menu_received, bn_menu_predefined, bn_menu_stored };
    const int labelW[3] = { bn_menu_received_w, bn_menu_predefined_w, bn_menu_stored_w };
    const int labelH[3] = { bn_menu_received_h, bn_menu_predefined_h, bn_menu_stored_h };

    display.setFont(&FreeMonoBold9pt7b);
    for (int i = 0; i < 3; i++) {
      int y = 68 + i * 30;
      display.setCursor(10, y + 16);
      display.print(i == topMenuIndex ? ">" : " ");
      display.drawBitmap(28, y, labelBmp[i], labelW[i], labelH[i], GxEPD_BLACK);
    }
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_RECEIVE_TOP;
  markScreenActive();
}

// Sub-screen: shows the actual received message text
void drawViewReceivedMessage() {
  refreshBattery(false);
  bool full = needsFullPaint(MENU_VIEW_RECEIVED);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();
    display.drawBitmap(10, CONTENT_TOP, bn_hdr_viewmsg, bn_hdr_viewmsg_w, bn_hdr_viewmsg_h, GxEPD_BLACK);
    drawMessageBody(lastReceivedText, 10, CONTENT_TOP + 34, 5);
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_VIEW_RECEIVED;
  markScreenActive();
}

// Screen shown when the e-paper wakes because a message was SENT
void drawSentTopMenu() {
  refreshBattery(false);
  bool full = needsFullPaint(MENU_SENT_TOP);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();

    display.drawBitmap(10, CONTENT_TOP, bn_hdr_sent, bn_hdr_sent_w, bn_hdr_sent_h, GxEPD_BLACK);
    drawMessageBody(lastSentText, 10, CONTENT_TOP + 32, 3);

    const uint8_t* labelBmp[2] = { bn_menu_predefined, bn_menu_stored };
    const int labelW[2] = { bn_menu_predefined_w, bn_menu_stored_w };
    const int labelH[2] = { bn_menu_predefined_h, bn_menu_stored_h };

    display.setFont(&FreeMonoBold9pt7b);
    for (int i = 0; i < 2; i++) {
      int y = 134 + i * 30;
      display.setCursor(10, y + 16);
      display.print(i == topMenuIndex ? ">" : " ");
      display.drawBitmap(28, y, labelBmp[i], labelW[i], labelH[i], GxEPD_BLACK);
    }
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_SENT_TOP;
  markScreenActive();
}

// Browses the 20 predefined messages, LIST_ROWS at a time.
void drawPredefinedMenu() {
  refreshBattery(false);
  listSel = predefinedIndex;
  clampListWindow();

  bool full = needsFullPaint(MENU_PREDEFINED);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();
    display.drawBitmap(10, TITLE_Y, bn_menu_predefined,
                       bn_menu_predefined_w, bn_menu_predefined_h, GxEPD_BLACK);
    drawListArea();
    drawHintBar();
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_PREDEFINED;
  markScreenActive();
}

// Browses the stored received messages, LIST_ROWS at a time.
void drawMemoryMenu() {
  refreshBattery(false);
  listSel = memoryIndex;
  clampListWindow();

  bool full = needsFullPaint(MENU_MEMORY);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();
    display.drawBitmap(10, TITLE_Y, bn_menu_stored,
                       bn_menu_stored_w, bn_menu_stored_h, GxEPD_BLACK);

    int total = listTotal();
    if (total > 0) {
      // Position counter in Bangla numerals, right-aligned against the title.
      String pos = toBanglaDigits(listSel + 1) + "/" + toBanglaDigits(total);
      drawUTF8Text(pos, 130, TITLE_Y + 15, 68, 18, false);
    }
    drawListArea();
    drawHintBar();
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_MEMORY;
  markScreenActive();
}

// ============================================================
// [WEB] Wi-Fi portal info screen
//
// Labels are Bangla bitmaps; the network name, the password and the
// address stay Latin, because they are strings you retype on a phone
// keyboard and any translation of them would be wrong.
// ============================================================

void drawPortalScreen() {
  refreshBattery(false);
  bool full = needsFullPaint(MENU_PORTAL);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();

    display.drawBitmap(10, TITLE_Y, bn_ui_wifi_on,
                       bn_ui_wifi_on_w, bn_ui_wifi_on_h, GxEPD_BLACK);

    // Three things to copy onto a phone, each a Bangla label with the
    // literal value underneath in the mono font. The values stay Latin:
    // they are typed character for character, so they must look exactly
    // like what the phone's keyboard will produce.
    int y = 48;
    display.drawBitmap(8, y, bn_ui_net_name, bn_ui_net_name_w, bn_ui_net_name_h, GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(14, y + 30);
    display.print(portalSsidString());

    y = 88;
    display.drawBitmap(8, y, bn_ui_password, bn_ui_password_w, bn_ui_password_h, GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(14, y + 29);
    display.print(PORTAL_PASSWORD);

    y = 126;
    display.drawBitmap(8, y, bn_ui_address, bn_ui_address_w, bn_ui_address_h, GxEPD_BLACK);
    // "http://" is spelled out beside the label. A browser that assumes
    // https for a bare address reports the site as unreachable, which is
    // the commonest reason the page appears not to open at all.
    display.setFont(NULL); display.setTextSize(1);
    display.setCursor(62, y + 5);
    display.print("http://");
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(14, y + 32);
    display.print(portalIpString());

    drawHintsForScreen();
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_PORTAL;
  markScreenActive();
}

// ============================================================
// Identity / private-session screens
// ============================================================

void drawFleetMenu() {
  refreshBattery(false);
  listSel = fleetIndex;

  // Once the scan has settled, park the cursor on something you can
  // actually choose. Starting it on a taken ID would mean the first
  // press of SEL does nothing but complain.
  if (fleetAssignMode && !linkScanning() && linkIdTaken((uint8_t)(listSel + 1))) {
    for (int i = 0; i < LINK_FLEET_SIZE; i++) {
      int cand = (listSel + i) % LINK_FLEET_SIZE;
      if (!linkIdTaken((uint8_t)(cand + 1))) { listSel = cand; break; }
    }
    fleetIndex = listSel;
  }
  clampListWindow();

  bool full = needsFullPaint(MENU_FLEET);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();

    if (fleetAssignMode) {
      display.drawBitmap(8, TITLE_Y, bn_ui_choose_id,
                         bn_ui_choose_id_w, bn_ui_choose_id_h, GxEPD_BLACK);
    } else {
      display.drawBitmap(8, TITLE_Y, bn_ui_peer_id,
                         bn_ui_peer_id_w, bn_ui_peer_id_h, GxEPD_BLACK);
    }
    drawListArea();
    // While the scan runs the hint strip says খোঁজা হচ্ছে instead of the
    // usual next/select, so nobody picks an ID before the answers are in.
    drawHintsForScreen();
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_FLEET;
  markScreenActive();
}

// Maps whatever Link.cpp last reported into a Bangla bitmap. Returning
// the pointer plus its size keeps the caller a three-line blit.
const uint8_t *noticeBitmap(int &w, int &h) {
  switch (linkNoticeCode()) {
    case LMSG_CHOOSE_ID: w = bn_ui_choose_id_w; h = bn_ui_choose_id_h; return bn_ui_choose_id;
    case LMSG_BROADCAST: w = bn_ui_broadcast_w; h = bn_ui_broadcast_h; return bn_ui_broadcast;
    case LMSG_ID_SAVED:  w = bn_ui_id_saved_w;  h = bn_ui_id_saved_h;  return bn_ui_id_saved;
    case LMSG_ID_BUSY:   w = bn_ui_id_busy_w;   h = bn_ui_id_busy_h;   return bn_ui_id_busy;
    case LMSG_WAITING:   w = bn_ui_req_sent_w;  h = bn_ui_req_sent_h;  return bn_ui_req_sent;
    case LMSG_REQ_IN:    w = bn_ui_req_in_w;    h = bn_ui_req_in_h;    return bn_ui_req_in;
    case LMSG_CONNECTING:w = bn_ui_connecting_w;h = bn_ui_connecting_h;return bn_ui_connecting;
    case LMSG_PRIVATE:   w = bn_ui_private_w;   h = bn_ui_private_h;   return bn_ui_private;
    case LMSG_TIMED_OUT: w = bn_ui_timed_out_w; h = bn_ui_timed_out_h; return bn_ui_timed_out;
    case LMSG_PEER_LOST: w = bn_ui_peer_lost_w; h = bn_ui_peer_lost_h; return bn_ui_peer_lost;
    case LMSG_PEER_BUSY: w = bn_ui_peer_busy_w; h = bn_ui_peer_busy_h; return bn_ui_peer_busy;
    case LMSG_PEER_LEFT: w = bn_ui_peer_left_w; h = bn_ui_peer_left_h; return bn_ui_peer_left;
    case LMSG_SEARCHING: w = bn_ui_searching_w; h = bn_ui_searching_h; return bn_ui_searching;
    default:             w = 0; h = 0; return NULL;
  }
}

void drawPairScreen() {
  refreshBattery(false);
  bool full = needsFullPaint(MENU_PAIR);
  if (full) beginFullPaint(); else beginRegion(0, 0, SCREEN_W, SCREEN_H);
  do {
    display.fillScreen(GxEPD_WHITE);
    drawStatusBar();

    // "My ID"  A0
    display.drawBitmap(8, TITLE_Y, bn_ui_my_id, bn_ui_my_id_w, bn_ui_my_id_h, GxEPD_BLACK);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(118, TITLE_Y + 17);
    display.print(linkName(linkNodeId()));

    // Who this module is talking to
    int y = 62;
    if (linkPeer()) {
      display.drawBitmap(8, y, bn_ui_peer, bn_ui_peer_w, bn_ui_peer_h, GxEPD_BLACK);
      display.setFont(&FreeMonoBold12pt7b);
      display.setCursor(48, y + 16);
      display.print(linkName(linkPeer()));
    } else {
      display.drawBitmap(8, y, bn_ui_broadcast,
                         bn_ui_broadcast_w, bn_ui_broadcast_h, GxEPD_BLACK);
    }

    // Current state, in Bangla. Skipped when it would just repeat the
    // line above - in broadcast mode "to everyone" is both the
    // destination and the state, and printing it twice looks like a bug.
    int nw, nh;
    const uint8_t *nb = noticeBitmap(nw, nh);
    bool redundant = (linkPeer() == 0 && linkNoticeCode() == LMSG_BROADCAST);
    if (nb && !redundant) display.drawBitmap(8, 98, nb, nw, nh, GxEPD_BLACK);

    drawHintsForScreen();
  } while (display.nextPage());
  if (!full) endPaint();
  paintedScreen = MENU_PAIR;
  markScreenActive();
}

void drawLinkStatusScreen() { drawPairScreen(); }

// Repaints whatever screen is currently active.
void redrawCurrentScreen() {
  switch (menuState) {
    case MENU_RECEIVE_TOP:   drawReceiveTopMenu();      break;
    case MENU_VIEW_RECEIVED: drawViewReceivedMessage(); break;
    case MENU_SENT_TOP:      drawSentTopMenu();         break;
    case MENU_PREDEFINED:    drawPredefinedMenu();      break;
    case MENU_MEMORY:        drawMemoryMenu();          break;
    case MENU_PORTAL:        drawPortalScreen();        break;
    case MENU_PAIR:          drawPairScreen();          break;
    case MENU_FLEET:         drawFleetMenu();           break;
    case MENU_LINK_STATUS:   drawLinkStatusScreen();    break;
    default: break;
  }
}

// Blank screen for idle / auto-timeout. Always a full update: this is
// the moment to clear whatever ghosting the partial updates left, and
// nobody is watching a blank screen flicker.
void clearDisplay() {
  beginFullPaint();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());

  showingMessage = false;
  paintedScreen  = MENU_NONE;
}

void exitMenu() {
  clearDisplay();
  menuState = MENU_NONE;
}

// ============================================================
// Identity gate
//
// The ID chooser is shown when, and only when, no ID has ever been
// committed to flash. Once one has, every wake - by button, by an
// incoming message, or by the Wi-Fi portal - goes straight to the
// normal screens. Getting back to the chooser is deliberate: the
// serial command IDRESET, or the button on the portal's radio tab.
// ============================================================

bool identityNeeded() { return !linkIdentityCommitted(); }

// ============================================================
// Home screen and timed transitions
//
// "Home" is the top menu for whatever this handset last did: the
// "message sent" menu after a transmission, the "message received" menu
// after one arrives. Before either has happened there is nothing to show
// on those screens, so home is the preset list - which is also what a
// long press on SEL wakes into.
//
// A double press on SEL returns here from ANY screen. On home itself it
// puts the display back to sleep, so backing out repeatedly always ends
// somewhere sensible rather than in a dead end.
// ============================================================

MenuState     homeMenu   = MENU_PREDEFINED;
unsigned long goHomeAt   = 0;      // 0 = nothing scheduled

void goHome() {
  goHomeAt = 0;
  hintFlashBmp = NULL;
  menuState = homeMenu;
  if (menuState == MENU_PREDEFINED) { predefinedIndex = 0; listSel = 0; listTop = 0; }
  topMenuIndex = 0;
  parentMenu = (homeMenu == MENU_SENT_TOP) ? MENU_SENT_TOP : MENU_RECEIVE_TOP;
  resetButtons();
  redrawCurrentScreen();
}

// Show something for a moment, then go home by itself. Used for the
// "connected" and "page loaded" confirmations, which are worth reading but
// not worth leaving on screen.
void goHomeIn(unsigned long ms) { goHomeAt = millis() + ms; }

void serviceScheduledHome() {
  if (!goHomeAt || (long)(millis() - goHomeAt) < 0) return;
  goHome();
}

// SEL double press. Everywhere except the ID chooser, which cannot be
// left until an ID exists - without one the module cannot transmit at all.
void handleBackPress() {
  if (menuState == MENU_FLEET && fleetAssignMode) return;
  if (menuState == homeMenu) { exitMenu(); return; }   // already home: sleep
  goHome();
}

void openIdentityChooser() {
  fleetAssignMode = true;
  menuState  = MENU_FLEET;
  fleetIndex = 0;
  listSel    = 0;
  listTop    = 0;
  // Ask the air who already owns what before offering anything.
  linkStartScan();
  resetButtons();
  drawFleetMenu();
}

// ============================================================
// BLE Setup
// ============================================================

void setupBLE() {
  BLEDevice::init("ESP32_LoRa_Node_RX");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pService->start();
  pServer->getAdvertising()->start();

  Serial.println("BLE started - advertising as ESP32_LoRa_Node_RX.");
}

// ============================================================
// LoRa Setup
// ============================================================

// [WEB] Radio config now lives in NVS so the portal can change it.
void loadLoRaCfg() {
  prefs.begin(LORACFG_NAMESPACE, true);
  loraCfg.freqHz   = prefs.getLong ("freq", loraCfg.freqHz);
  loraCfg.sf       = prefs.getUChar("sf",   loraCfg.sf);
  loraCfg.bw       = prefs.getLong ("bw",   loraCfg.bw);
  loraCfg.cr       = prefs.getUChar("cr",   loraCfg.cr);
  loraCfg.txPower  = prefs.getUChar("pwr",  loraCfg.txPower);
  loraCfg.syncWord = prefs.getUChar("sync", loraCfg.syncWord);
  prefs.end();
}

void saveLoRaCfg() {
  prefs.begin(LORACFG_NAMESPACE, false);
  prefs.putLong ("freq", loraCfg.freqHz);
  prefs.putUChar("sf",   loraCfg.sf);
  prefs.putLong ("bw",   loraCfg.bw);
  prefs.putUChar("cr",   loraCfg.cr);
  prefs.putUChar("pwr",  loraCfg.txPower);
  prefs.putUChar("sync", loraCfg.syncWord);
  prefs.end();
}

void applyLoRaCfg() {
  LoRa.setFrequency(loraCfg.freqHz);
  LoRa.setSpreadingFactor(loraCfg.sf);
  LoRa.setSignalBandwidth(loraCfg.bw);
  LoRa.setCodingRate4(loraCfg.cr);
  LoRa.setTxPower(loraCfg.txPower, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setSyncWord(loraCfg.syncWord);
  LoRa.enableCrc();   // reject corrupted packets instead of storing garbage

  Serial.printf("LoRa: %.3f MHz  SF%u  BW%ld  CR4/%u  %u dBm  sync 0x%02X\n",
                loraCfg.freqHz / 1e6, loraCfg.sf, loraCfg.bw,
                loraCfg.cr, loraCfg.txPower, loraCfg.syncWord);
}

void setupLoRa() {
  LoRa.setPins(SS, RST, DIO0);

  Serial.println("Starting LoRa...");
  while (!LoRa.begin(loraCfg.freqHz)) {
    Serial.println("LoRa init failed, retrying...");
    delay(500);
  }

  applyLoRaCfg();

  Serial.println("LoRa ready - receiver always active.");
}

// ============================================================
// Send Message Through LoRa
// ============================================================

// [FLEET] Raw byte transmit - used by both the text path below and the
// private-session control packets in Link.cpp.
// True while a packet is being handed to the radio.
bool radioBusy = false;
bool apiRadioBusy() { return radioBusy; }

bool apiRawTransmit(const uint8_t *buf, size_t len) {
  if (len == 0) return false;

  // [WEB] Why this is a plain blocking endPacket() and not the async form.
  //
  // At SF12 a full-length packet occupies the radio for over ten seconds,
  // and nothing else in the sketch runs during that time - including the
  // web server. The obvious remedy is endPacket(true) plus a wait loop
  // that services the server, but the only way to ask "is it gone yet?"
  // is LoRaClass::isTransmitting(), which the library declares PRIVATE.
  //
  // The public alternative, onTxDone(), attaches an interrupt to DIO0 and
  // hands it to a handler that clears the radio's IRQ flags. This sketch
  // polls parsePacket() for reception, which reads those same flags, so an
  // interrupt handler clearing them behind its back would silently drop
  // incoming messages. Trading reliable reception for a more responsive
  // web page is a bad bargain in an emergency communicator.
  //
  // So the transmit blocks, and the starvation is dealt with where it
  // actually bites: handlePendingTransmission() holds a queued message
  // back while a browser is mid-conversation with the portal. Nothing in
  // here touches the web server - reaching into it from the middle of a
  // radio operation was another of the changes that broke the portal.
  radioBusy = true;
  LoRa.beginPacket();
  LoRa.write(buf, len);
  int result = LoRa.endPacket();
  radioBusy = false;

  if (result != 1) Serial.println("LoRa TX FAILED");
  else             txCountVal++;

  LoRa.receive();   // Keep listening during e-paper refreshes.
  return result == 1;
}

bool transmitLoRaMessage(String message) {
  if (!linkCanSend() || message.length()==0 || message.length()>MAX_TX_BYTES) {
    Serial.println("Send blocked: choose ID / finish request / check length");
    if (identityNeeded()) openIdentityChooser();
    else { menuState = MENU_PAIR; drawPairScreen(); }
    return false;
  }

  Serial.println("Transmitting: " + message);

  // Every transmission has an explicit broadcast or private destination.
  uint8_t frame[LINK_MAX_FRAME];
  size_t n = linkBuildText(message, frame, sizeof(frame));
  if (n == 0) { Serial.println("LoRa TX: frame build failed"); return false; }

  if (!apiRawTransmit(frame,n)) return false;
  Serial.println("LoRa TX to " + (linkIsPaired() ? linkName(linkPeer()) : String("ALL")));
  return true;
}

// Puts the e-paper + menu into the "Message Sent" top state
void enterSentTopMenu(const String &sentText) {
  lastSentText = sentText;
  homeMenu  = MENU_SENT_TOP;      // transmitting makes "message sent" home
  menuState = MENU_SENT_TOP;
  topMenuIndex = 0;
  resetButtons();
  drawSentTopMenu();
}

// ============================================================
// Receive LoRa Message
// ============================================================

void receiveLoRaMessage() {
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;

  // Read into a byte buffer, not a String - the first 8 bytes are
  // a binary header now, and 0x00 inside them would truncate a String.
  uint8_t buf[LINK_MAX_FRAME];
  size_t  len = 0;
  while (LoRa.available()) {
    int b = LoRa.read();
    if (b < 0) break;
    if (len < sizeof(buf)) buf[len++] = (uint8_t)b;
  }

  lastRssiVal = LoRa.packetRssi();
  lastSnrVal  = LoRa.packetSnr();
  LoRa.receive();
  if (packetSize>(int)sizeof(buf)) return; // Never accept a truncated frame.

  String received;
  uint8_t src = 0;
  LinkRx verdict = linkParse(buf, len, lastRssiVal, received, src);

  if (verdict != LINK_RX_TEXT) {
    // Control frame, traffic for another module, or our own ID coming
    // back at us. Not counted as a received message, not stored.
    return;
  }

  rxCountVal++;
  Serial.println("Received from " + linkName(src) + ": " + received +
                 " (RSSI " + String(lastRssiVal) +
                 ", SNR " + String(lastSnrVal, 1) + ")");

  handleIncomingMessage(received, lastRssiVal);

  if (deviceConnected && pTxCharacteristic != NULL) {
    pTxCharacteristic->setValue(received.c_str());
    pTxCharacteristic->notify();
  }
}

// Shared logic for "a message just arrived"
void handleIncomingMessage(const String &text, int rssi) {
  saveReceivedMessage(text, rssi);

  lastReceivedText = text;
  homeMenu = MENU_RECEIVE_TOP;    // receiving makes "message received" home
  // Never interrupt a decision the user is in the middle of. The ID
  // chooser counts: taking the screen away mid-selection is how you end
  // up with two modules on the same ID.
  if (menuState==MENU_FLEET || linkState()==LINK_INCOMING || linkState()==LINK_OUTGOING) return;
  menuState = MENU_RECEIVE_TOP;
  topMenuIndex = 0;
  resetButtons();
  drawReceiveTopMenu();
}

// ============================================================
// TEMPORARY: single-unit test helper
// Type "TEST:<message>" into the Serial Monitor to simulate a
// received packet. Safe to delete once both units are available.
// ============================================================

void checkSerialTestInput() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.startsWith("TEST:")) {
      String fakeMessage = line.substring(5);
      Serial.println("[TEST] Simulating received message: " + fakeMessage);
      handleIncomingMessage(fakeMessage, -90);
    }
    // [WEB] handy while bench-testing: type WIFI to toggle the portal
    else if (line.equalsIgnoreCase("WIFI")) {
      portalToggle();
    }
    else if (line.startsWith("ID:")) {
      linkSetIdentity(linkIdFromName(line.substring(3)));
    } else if (line.startsWith("PRIVATE:")) {
      linkRequest(linkIdFromName(line.substring(8)));
    } else if (line.equalsIgnoreCase("ACCEPT")) {
      linkAccept();
    } else if (line.equalsIgnoreCase("UNPAIR")) {
      linkDisconnect();
    }
    // ---- Identity maintenance -------------------------------------
    // The ID screen is shown once and then never again, so these are
    // the deliberate ways back to it. Note that re-uploading the sketch
    // does NOT clear NVS on its own - use IDRESET, or tick
    // "Erase All Flash Before Sketch Upload" in the Tools menu.
    else if (line.equalsIgnoreCase("IDRESET")) {
      Serial.println(linkResetIdentity() ? "Identity cleared - choose a new ID"
                                         : "Identity clear FAILED (NVS write error)");
      openIdentityChooser();
    } else if (line.equalsIgnoreCase("IDSCAN")) {
      linkStartScan();
      Serial.println("Asking the fleet which IDs are in use...");
    } else if (line.equalsIgnoreCase("IDFORGET")) {
      linkForgetClaims();
      Serial.println("Forgot which IDs were taken; run IDSCAN to rebuild");
    } else if (line.equalsIgnoreCase("LINK")) {
      Serial.println(linkName(linkNodeId())+" "+linkStateName()+" peer "+linkName(linkPeer()));
      Serial.printf("  committed=%s  free IDs=%d  takenMask=0x%05X\n",
                    linkIdentityCommitted() ? "yes" : "NO",
                    linkFreeCount(), (unsigned)linkTakenMask());
      String taken = "";
      for (uint8_t id = 1; id <= LINK_FLEET_SIZE; ++id)
        if (linkIdTaken(id)) taken += linkName(id) + " ";
      Serial.println("  in use elsewhere: " + (taken.length() ? taken : String("(none heard)")));
    }
  }
}

// ============================================================
// Handle Pending LoRa Transmission (from BLE / web / custom message)
// ============================================================

void handlePendingTransmission() {
  char text[MAX_TX_BYTES+1];
  uint32_t route;

  // [WEB] Hold the message back briefly while a browser is mid-conversation.
  //
  // A transmit blocks everything for up to eleven seconds at SF12, and the
  // worst moment to start one is while the browser is waiting on a reply.
  // So wait for a gap in the HTTP traffic - but only for so long. The page
  // polls every 3 s, and a deferral with no deadline would mean a message
  // sent from the web page never went out at all while that page stayed
  // open. After 3 s it goes regardless and the page misses one poll.
  static unsigned long deferredSince = 0;
  bool pending;
  portENTER_CRITICAL(&pendingMux);
  pending = loRaTransmitPending;
  portEXIT_CRITICAL(&pendingMux);
  if (!pending) { deferredSince = 0; return; }

  if (portalBusyWithBrowser()) {
    if (!deferredSince) deferredSince = millis();
    if (millis() - deferredSince < 3000UL) return;
    Serial.println("[portal] no quiet moment in 3 s - transmitting anyway");
  }
  deferredSince = 0;

  portENTER_CRITICAL(&pendingMux);
  if (!loRaTransmitPending) { portEXIT_CRITICAL(&pendingMux); return; }
  memcpy(text,pendingLoRaText,sizeof(text));
  route=pendingRouteRevision;
  loRaTransmitPending=false;
  portEXIT_CRITICAL(&pendingMux);
  if (route!=linkRevision()) {
    Serial.println("Queued message cancelled: destination changed");
    return;
  }
  String messageToSend(text);
  if (!transmitLoRaMessage(messageToSend)) return;
  enterSentTopMenu(messageToSend);

  if (deviceConnected && pTxCharacteristic != NULL) {
    String confirmation = "Sent: " + messageToSend;
    pTxCharacteristic->setValue(confirmation.c_str());
    pTxCharacteristic->notify();
  }
}

// ============================================================
// Menu Navigation
// ============================================================

void handleMenuButtons() {
  if (menuState == MENU_NONE) return;   // nothing to navigate when idle

  ButtonEvent navEvt = pollButton(navButton);
  ButtonEvent selEvt = pollButton(selButton);
  if (linkState()==LINK_INCOMING && menuState!=MENU_PAIR) {
    menuState=MENU_PAIR; resetButtons(); drawPairScreen(); return;
  }

  switch (menuState) {

    // ---------------------------------------------------
    case MENU_RECEIVE_TOP: {
      if (navEvt == EVT_SINGLE) {
        topMenuIndex = (topMenuIndex + 1) % 3;
        drawReceiveTopMenu();
      } else if (navEvt == EVT_DOUBLE) {
        topMenuIndex = (topMenuIndex + 2) % 3;   // -1 mod 3
        drawReceiveTopMenu();
      }

      if (selEvt == EVT_DOUBLE) { handleBackPress(); break; }
      if (selEvt == EVT_SINGLE) {
        if (topMenuIndex == 0) {
          menuState = MENU_VIEW_RECEIVED;
          resetButtons();
          drawViewReceivedMessage();
        } else if (topMenuIndex == 1) {
          parentMenu = MENU_RECEIVE_TOP;
          menuState = MENU_PREDEFINED;
          predefinedIndex = 0;
          resetButtons();
          drawPredefinedMenu();
        } else {
          parentMenu = MENU_RECEIVE_TOP;
          menuState = MENU_MEMORY;
          memoryIndex = 0;
          resetButtons();
          drawMemoryMenu();
        }
      }
      break;
    }

    // ---------------------------------------------------
    case MENU_VIEW_RECEIVED: {
      if (selEvt == EVT_DOUBLE) handleBackPress();
      break;
    }

    // ---------------------------------------------------
    case MENU_SENT_TOP: {
      if (navEvt == EVT_SINGLE || navEvt == EVT_DOUBLE) {
        topMenuIndex = (topMenuIndex + 1) % 2;
        drawSentTopMenu();
      }

      if (selEvt == EVT_DOUBLE) { handleBackPress(); break; }
      if (selEvt == EVT_SINGLE) {
        parentMenu = MENU_SENT_TOP;
        if (topMenuIndex == 0) {
          menuState = MENU_PREDEFINED;
          predefinedIndex = 0;
        } else {
          menuState = MENU_MEMORY;
          memoryIndex = 0;
        }
        resetButtons();
        (menuState == MENU_PREDEFINED) ? drawPredefinedMenu() : drawMemoryMenu();
      }
      break;
    }

    // ---------------------------------------------------
    // [PARTIAL] The two message lists behave identically now: NAV steps
    // the cursor and listMoveBy() repaints only the rows that changed,
    // so walking a 100-message inbox never triggers a full refresh.
    case MENU_PREDEFINED: {
      if (navEvt == EVT_SINGLE)      listMoveBy(1);
      else if (navEvt == EVT_DOUBLE) listMoveBy(-1);
      predefinedIndex = listSel;

      if (selEvt == EVT_SINGLE) {
        String msg = getPredefinedMessage(predefinedIndex);
        // The strip says so before the radio starts, because at SF12 the
        // packet takes several seconds and silence reads as a dead button.
        flashHint(bn_ui_sending, bn_ui_sending_w, bn_ui_sending_h, 30000);
        bool ok = transmitLoRaMessage(msg);
        flashHint(ok ? bn_ui_sent_ok : bn_ui_send_fail,
                  ok ? bn_ui_sent_ok_w : bn_ui_send_fail_w,
                  ok ? bn_ui_sent_ok_h : bn_ui_send_fail_h, 1200);
        if (ok) enterSentTopMenu(msg);
      } else if (selEvt == EVT_DOUBLE) {
        handleBackPress();
      }
      break;
    }

    // ---------------------------------------------------
    case MENU_MEMORY: {
      int total = getStoredMessageCount();

      if (navEvt == EVT_SINGLE && total > 0)      listMoveBy(1);
      else if (navEvt == EVT_DOUBLE && total > 0) listMoveBy(-1);
      memoryIndex = listSel;

      if (selEvt == EVT_SINGLE && total > 0) {
        String msg = getStoredMessage(memoryIndex);
        flashHint(bn_ui_sending, bn_ui_sending_w, bn_ui_sending_h, 30000);
        bool ok = transmitLoRaMessage(msg);
        flashHint(ok ? bn_ui_sent_ok : bn_ui_send_fail,
                  ok ? bn_ui_sent_ok_w : bn_ui_send_fail_w,
                  ok ? bn_ui_sent_ok_h : bn_ui_send_fail_h, 1200);
        if (ok) enterSentTopMenu(msg);
      } else if (selEvt == EVT_DOUBLE) {
        handleBackPress();
      }
      break;
    }

    // ---------------------------------------------------
    case MENU_PORTAL: {                                       // [WEB]
      // NAV opens the predefined list so the handheld still works
      // normally while the portal is up. SEL double-press blanks the
      // screen (the portal itself keeps running - long-press FN
      // is what turns Wi-Fi off).
      if (navEvt == EVT_SINGLE || navEvt == EVT_DOUBLE) {
        parentMenu = MENU_RECEIVE_TOP;
        menuState = MENU_PREDEFINED;
        predefinedIndex = 0;
        resetButtons();
        drawPredefinedMenu();
      } else if (selEvt == EVT_DOUBLE) {
        handleBackPress();
      }
      break;
    }

    // ---------------------------------------------------
    // One screen, two jobs: assigning this module's own ID (first boot
    // only) and picking a peer for a private session. fleetAssignMode
    // says which, and the cursor skips IDs that are not selectable in
    // that mode - taken ones when assigning, our own when connecting.
    case MENU_FLEET: {
      if (navEvt == EVT_SINGLE)      listMoveBy(1);
      else if (navEvt == EVT_DOUBLE) listMoveBy(-1);
      fleetIndex = listSel;

      if (selEvt == EVT_SINGLE) {
        uint8_t id = (uint8_t)(fleetIndex + 1);
        if (fleetAssignMode) {
          if (linkScanning()) {
            // Still listening for CLAIM answers: committing now could
            // pick an ID that is about to be reported as taken.
            Serial.println("Still scanning - wait for the list to settle");
          } else if (linkSetIdentity(id)) {
            fleetAssignMode = false;
            menuState = MENU_PAIR;
            resetButtons();
            drawPairScreen();
          } else {
            drawFleetMenu();          // shows "this ID is in use"
          }
        } else if (id != linkNodeId() && linkRequest(id)) {
          menuState = MENU_PAIR;
          resetButtons();
          drawPairScreen();
        }
      } else if (selEvt == EVT_DOUBLE) {
        handleBackPress();
      }
      break;
    }
    case MENU_PAIR:
    case MENU_LINK_STATUS: {
      if (linkState()==LINK_INCOMING && selEvt==EVT_SINGLE) {
        linkAccept(); resetButtons();
        flashHint(bn_ui_wait, bn_ui_wait_w, bn_ui_wait_h, 1500);
      } else if (selEvt==EVT_DOUBLE) {
        // A half-built session is abandoned on the way out; an established
        // one is left alone, because FN double-tap is what ends those.
        if (linkState()!=LINK_PUBLIC && !linkIsPaired()) linkDisconnect();
        handleBackPress();
      } else if (navEvt!=EVT_NONE && linkCanSend()) {
        parentMenu=MENU_RECEIVE_TOP; menuState=MENU_PREDEFINED;
        predefinedIndex=0; resetButtons(); drawPredefinedMenu();
      }
      break;
    }

    default:
      break;
  }
}

// ============================================================
// [FLEET] FN button
// Polled every loop() - unlike NAV/SEL it must respond even when the
// screen is blank, since that is when you would reach for pairing or
// the Wi-Fi portal.
// ============================================================

void handleFnButton() {
  // Long press -> Wi-Fi portal (this is the command that moved off NAV)
  if (pollLongPress(fnLP, FN_LONG_PRESS_MS)) {
    fnButton.clickCount = 0;    // cancel the click from this same press
    Serial.println(portalIsActive() ? "FN long: Wi-Fi off." : "FN long: Wi-Fi on.");
    portalToggle();
    resetButtons();
    return;
  }

  ButtonEvent e = pollButton(fnButton);

  if (e==EVT_DOUBLE) {
    linkDisconnect();
  } else if (e==EVT_SINGLE) {
    if (identityNeeded()) {
      // No ID has ever been committed, so there is only one thing this
      // button can usefully do.
      openIdentityChooser();
      return;
    }
    if (linkState()!=LINK_PUBLIC) {
      menuState=MENU_PAIR; drawPairScreen();
    } else {
      // Peer chooser. Never the ID-assign screen: that one is behind
      // identityNeeded() and IDRESET, so it cannot reappear on a wake.
      fleetAssignMode = false;
      if (menuState != MENU_FLEET) { fleetIndex = 0; listSel = 0; listTop = 0; }
      menuState = MENU_FLEET;
      drawFleetMenu();
    }
    resetButtons();
  }
}

// Something changed in Link.cpp - a request arrived, a session opened
// or closed, another module claimed an ID.
//
// This used to drag the screen to the pairing view on EVERY such event,
// which is why the link screen kept appearing unbidden. Now it only
// repaints what the user is already looking at, and it only takes the
// screen over for the two cases that genuinely need a decision: no ID
// yet, or an incoming request waiting for an answer.
LinkState lastLinkState = LINK_PUBLIC;

void handlePairEvents() {
  if (!linkTakeEvent()) return;
  // Fixed-size queue is shared with the BLE callback on the other core.
  portENTER_CRITICAL(&pendingMux);
  if (loRaTransmitPending && pendingRouteRevision!=linkRevision()) loRaTransmitPending=false;
  portEXIT_CRITICAL(&pendingMux);

  // The moment a session actually opens: show that it worked, leave it up
  // long enough to read, then hand the screen back to the message menu.
  // Dropping straight to the menu makes people wonder whether it paired at
  // all; leaving the pairing screen up forever makes them press buttons to
  // get out of it.
  LinkState now = linkState();
  bool justPaired = (now == LINK_PRIVATE && lastLinkState != LINK_PRIVATE);
  lastLinkState = now;

  if (justPaired) {
    menuState = MENU_PAIR;
    resetButtons();
    drawPairScreen();
    flashHint(bn_ui_connected, bn_ui_connected_w, bn_ui_connected_h, 2200);
    goHomeIn(2000);
    return;
  }

  if (identityNeeded()) {
    if (menuState != MENU_FLEET || !fleetAssignMode) openIdentityChooser();
    else drawFleetMenu();                 // scan finished, or a CLAIM landed
    return;
  }

  if (linkState() == LINK_INCOMING) {     // needs a yes/no from the user
    menuState = MENU_PAIR;
    resetButtons();
    drawPairScreen();
    return;
  }

  switch (menuState) {
    case MENU_NONE:
      break;                              // asleep: leave the screen blank
    case MENU_PAIR:
    case MENU_LINK_STATUS:
    case MENU_FLEET:
      redrawCurrentScreen();              // these screens show link state
      break;
    default:
      repaintStatusBar();                 // [PARTIAL] only the top strip changed
      break;
  }
}

// ============================================================
// Handle BLE Reconnection / Advertising
// ============================================================

void handleBLEConnection() {
  // [WEB] Don't fight the Wi-Fi radio: while the portal is up, BLE
  // advertising stays paused (see apiPortalStateChanged).
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    if (pServer != NULL && !portalIsActive()) {
      pServer->getAdvertising()->start();
      Serial.println("BLE advertising restarted.");
    }
    oldDeviceConnected = false;
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;
  }
}

// ============================================================
// Handle E-Paper / Menu Idle Timeout
// ============================================================

void handleDisplayTimeout() {
  // Nothing sleeps while a decision is outstanding. That now covers the
  // whole pairing negotiation, not just an incoming request: the display
  // going dark halfway through a handshake leaves both people guessing
  // whether it worked.
  if (identityNeeded() || portalIsActive()) return;
  if (linkState() != LINK_PUBLIC && linkState() != LINK_PRIVATE) return;
  if (goHomeAt) return;               // a confirmation is on screen
  if (showingMessage && (millis() - messageShownAt > DISPLAY_HOLD_MS)) {
    exitMenu();
  }
}

// ============================================================
// [PARTIAL] Keep the battery figure honest without disturbing the page
//
// refreshBattery() re-samples every 10 s. When the displayed percentage
// actually moves, repaint the 19 px status strip and nothing else - a
// whole-screen refresh every time the battery ticks down a point would
// be both slow and, on a screen you are reading, rude.
// ============================================================

int lastShownBattery = -2;

void refreshStatusStrip() {
  if (menuState == MENU_NONE) return;
  if (battPercent == lastShownBattery) return;
  lastShownBattery = battPercent;
  repaintStatusBar();
}

// ============================================================
// [WEB] NodeApi implementations - everything WebPortal.cpp calls
// ============================================================

bool apiQueueOutgoing(const String &utf8) {
  uint32_t route=linkRevision();
  if (!linkCanSend() || utf8.length()==0 || utf8.length()>MAX_TX_BYTES) return false;
  portENTER_CRITICAL(&pendingMux);
  if (loRaTransmitPending || route!=linkRevision()) {
    portEXIT_CRITICAL(&pendingMux); return false;
  }
  memcpy(pendingLoRaText,utf8.c_str(),utf8.length());
  pendingLoRaText[utf8.length()]=0;
  pendingRouteRevision=route;
  loRaTransmitPending=true;
  portEXIT_CRITICAL(&pendingMux);
  return true;
}

int    apiStoredCount()            { return getStoredMessageCount(); }
String apiStoredMessage(int n)     { return getStoredMessage(n); }
void   apiClearStored()            { clearStoredMessages(); }

bool apiStoredEntry(int n, String &text, int &rssi) {
  return msgStoreEntry(n, text, rssi);
}

String apiPreset(int i)                          { return getPredefinedMessage(i); }
void   apiSetPreset(int i, const String &text)   { setPredefinedMessage(i, text); }
void   apiResetPresets()                         { initializePredefinedMessages(true); }
bool   apiPresetIsEdited(int i)                  { return (presetEditedMask & (1UL << i)) != 0; }

void apiGetLoRaCfg(LoRaCfg &out) { out = loraCfg; }

void apiSetLoRaCfg(const LoRaCfg &cfg) {
  linkDisconnect(); // The other peer expires if the channel changes before LEAVE arrives.
  loraCfg = cfg;
  applyLoRaCfg();
  saveLoRaCfg();
}

void apiGetStatus(NodeStatus &out) {
  refreshBattery(false);
  out.battPercent = battPercent;
  out.battVolts   = battVolts;
  out.lastRssi    = lastRssiVal;
  out.lastSnr     = lastSnrVal;
  out.txCount     = txCountVal;
  out.rxCount     = rxCountVal;
  out.uptimeS     = millis() / 1000UL;
  out.freeHeap    = ESP.getFreeHeap();
  out.storedCount = msgStoreCount();
  out.storedCap   = MAX_STORED_MESSAGES;
  out.storedBytes = msgStoreBytes();
}

String apiLastReceived() { return lastReceivedText; }
String apiLastSent()     { return lastSentText; }

// ============================================================
// [WEB] Portal screen state
//
// Three things are reported from the HTTP side, all as flags rather than
// as direct drawing: an e-paper refresh inside a request handler would
// hold the socket open for the best part of a second, right at the moment
// the browser is waiting to finish loading the page.
// ============================================================

volatile bool portalPageFlag = false;   // a browser fetched the page
volatile bool portalFailFlag = false;   // the access point would not start
int  portalShownClients = -1;           // what the screen currently says

void apiPortalPageServed() { portalPageFlag = true; }
void apiPortalFailed()     { portalFailFlag = true; }

// Called every loop while the Wi-Fi screen is up.
void servicePortalScreen() {
  if (portalFailFlag) {
    portalFailFlag = false;
    menuState = MENU_PORTAL;
    flashHint(bn_ui_page_fail, bn_ui_page_fail_w, bn_ui_page_fail_h, 3500);
    goHomeIn(3500);
    return;
  }

  if (portalPageFlag) {
    portalPageFlag = false;
    // The page is on the phone, so the SSID and password on the e-paper
    // have done their job. Confirm, then step aside - the portal itself
    // keeps running in the background.
    Serial.println("[portal] page delivered - handing the screen back");
    if (menuState == MENU_PORTAL) {
      flashHint(bn_ui_page_open, bn_ui_page_open_w, bn_ui_page_open_h, 2200);
      goHomeIn(2000);
    }
    return;
  }

  // Whether a phone has actually joined the access point is the single most
  // useful thing to show while the page is not loading: it separates "the
  // Wi-Fi did not connect" from "the Wi-Fi connected but the page did not".
  if (menuState != MENU_PORTAL || !portalIsActive()) return;
  int n = portalClientCount();
  if (n != portalShownClients) {
    portalShownClients = n;
    if (!hintFlashBmp) repaintHintBar();
  }
}

void apiPortalStateChanged(bool nowActive) {
  if (nowActive) {
    // BLE advertising is stopped first. Bluedroid and the Wi-Fi AP share
    // one 2.4 GHz radio, and leaving both beaconing is a large part of
    // why the portal used to be slow to appear or refuse to load at all.
    if (pServer != NULL) pServer->getAdvertising()->stop();
    portalPageFlag = false;
    portalFailFlag = false;
    portalShownClients = -1;
    goHomeAt = 0;
    menuState = MENU_PORTAL;
    resetButtons();
    drawPortalScreen();
  } else {
    if (pServer != NULL && !deviceConnected) pServer->getAdvertising()->start();
    if (menuState == MENU_PORTAL)  goHome();
    else if (showingMessage)       repaintStatusBar();   // drop the "WiFi" marker
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("ESP32 LoRa + BLE + Wi-Fi + E-Paper node starting...");

  // ---- Buttons ----
  pinMode(BUTTON_NAV, INPUT_PULLUP);
  pinMode(BUTTON_SEL, INPUT_PULLUP);
  pinMode(BUTTON_FN,  INPUT_PULLUP);   // [FLEET]

  // ---- [BATT] ADC1, 11 dB attenuation -> ~0..3.1 V usable range ----
  analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);
  refreshBattery(true);

  // ---- E-Paper: initialize once, stays powered/awake ----
  display.init(115200);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());
  Serial.println("E-paper initialized.");

  measureAsciiMetrics();

  setupBLE();
  delay(100);

  loadLoRaCfg();              // [WEB] before setupLoRa, so saved values apply
  setupLoRa();
  linkBegin();                // Load assigned fleet ID; boot into broadcast

  initializePredefinedMessages(false);

  msgStoreBegin();            // [WEB] mount LittleFS inbox (imports the old
                              // NVS messages once, on first boot after upgrade)
  printAllStoredMessages();   // confirm what survived from before this boot

  Serial.printf("Battery: %.2f V (%d%%)  freeHeap=%u\n",
                battVolts, battPercent, (unsigned)ESP.getFreeHeap());

  // ---- Identity ----
  // This is the ONLY place the chooser can open by itself, and only
  // when nothing was ever committed. Every later boot prints the saved
  // ID and leaves the screen blank until something happens.
  if (identityNeeded()) {
    Serial.println("No ID stored yet - opening the chooser (this happens once).");
    openIdentityChooser();
  } else {
    Serial.println("Module ID " + linkName(linkNodeId()) +
                   " loaded from flash; the ID screen stays closed.");
    Serial.println("  Type IDRESET to choose a different one.");
  }

  Serial.println("System ready.");
  Serial.println("  SEL (GPIO14): hold = wake/sleep the screen, tap = select, double-tap = back");
  Serial.println("  NAV (GPIO13): tap = next item, double-tap = previous item");
  Serial.println("  FN  (GPIO27): tap = peer list, double-tap = disconnect, hold = Wi-Fi portal");
  Serial.println("  Serial: TEST: WIFI ID: PRIVATE: ACCEPT UNPAIR LINK IDSCAN IDFORGET IDRESET");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  receiveLoRaMessage();
  linkTick();
  handlePairEvents();
  handlePendingTransmission();
  handleBLEConnection();
  checkLongPresses();          // SEL hold = display sleep/wake
  handleFnButton();            // [FLEET] FN tap/double/hold - always polled
  handleMenuButtons();
  handlePairEvents();          // Update physical UI after web/radio changes
  handleDisplayTimeout();
  checkSerialTestInput();
  portalLoop();                // [WEB] no-op while Wi-Fi is off
  servicePortalScreen();       // [WEB] page loaded / AP failed / phone joined
  refreshBattery(false);       // [BATT] cached, actually samples every 10 s
  refreshStatusStrip();        // [PARTIAL] top 19 px only, when it changed
  serviceHintFlash();          // [FLASH] put the normal hints back
  serviceScheduledHome();      // return to the message menu after a confirmation

  // The delay stays, portal or not. Dropping it while Wi-Fi was up - to
  // "service the server more often" - turned loop() into a busy wait that
  // starved the LwIP and Wi-Fi tasks it shares the core with, and a
  // starved Wi-Fi task fails associations. That is what produced
  // "authentication problem" and "incorrect password" on the phone.
  delay(2);
}
