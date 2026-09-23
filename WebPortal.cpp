// ============================================================
// WebPortal.cpp
//
// A synchronous WebServer is used on purpose rather than
// ESPAsyncWebServer:
//   * it is part of the ESP32 Arduino core, so there is no extra
//     library (and no AsyncTCP) to install;
//   * it uses noticeably less heap, which matters a lot here because
//     the Bluedroid BLE stack is already holding ~65 KB.
// The usual objection to the sync server is that it blocks. That is
// handled by never doing slow work inside a handler: /api/send only
// queues the message, and loop() does the actual LoRa transmit and
// e-paper refresh after the HTTP response has already gone out.
// ============================================================

#include "WebPortal.h"      // must come first: it defines PORTAL_CAPTIVE
#include "NodeApi.h"
#include "PortalPage.h"
#include "Link.h"

#include <WiFi.h>
#include <WebServer.h>
#if PORTAL_CAPTIVE
  #include <DNSServer.h>
#endif

static WebServer server(80);
#if PORTAL_CAPTIVE
static DNSServer  dns;
#endif

static bool          active      = false;
static bool          pageServed  = false;   // a browser actually fetched "/"
static unsigned long lastRequest = 0;
static IPAddress     apIp;
static String        apSsid;

// ------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------

// Escapes a UTF-8 String for embedding in a JSON string literal.
// Multi-byte Bangla sequences are valid JSON as-is, so they pass
// through untouched - only the structural characters need care.
static String jsonEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char b[7];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

static void touchActivity() { lastRequest = millis(); }

// Every request is logged. If the phone says the site is unreachable, the
// serial output settles the question immediately: lines here mean the
// request reached the ESP32 and the problem is the response; silence means
// it never arrived, and the problem is the Wi-Fi link or the phone.
static void logRequest(const char *what) {
  Serial.printf("[portal] %s  from %s  heap=%u\n", what,
                server.client().remoteIP().toString().c_str(),
                (unsigned)ESP.getFreeHeap());
}

static void sendJson(const String &body) {
  touchActivity();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", body);
}

// The raw request body. The ESP32 WebServer stashes a non-form body
// under the pseudo-argument "plain", which is why the page posts with
// Content-Type: text/plain.
static String bodyText() {
  return server.hasArg("plain") ? server.arg("plain") : String("");
}

// ------------------------------------------------------------
// Handlers
// ------------------------------------------------------------

static void hRoot() {
  touchActivity();
  logRequest("GET /");
  server.sendHeader("Cache-Control", "no-store");
  // send_P streams from flash: no big String copy on the heap.
  server.send_P(200, "text/html; charset=utf-8", PORTAL_HTML);

  // The page is out. Tell the sketch so the e-paper can stop showing the
  // Wi-Fi instructions - but only raise a flag, never draw from here: an
  // e-paper refresh inside this handler would hold the socket open for the
  // best part of a second after the body has been written.
  if (!pageServed) {
    pageServed = true;
    apiPortalPageServed();
  }
}

static void hStatus() {
  NodeStatus st;  apiGetStatus(st);
  LoRaCfg    cf;  apiGetLoRaCfg(cf);

  String j = "{";
  j += "\"batt\":"  + String(st.battPercent);
  j += ",\"mv\":"   + String(st.battVolts, 3);
  j += ",\"rssi\":" + String(st.lastRssi);
  j += ",\"snr\":"  + String(st.lastSnr, 1);
  j += ",\"tx\":"   + String(st.txCount);
  j += ",\"rx\":"   + String(st.rxCount);
  j += ",\"up\":"   + String(st.uptimeS);
  j += ",\"heap\":" + String(st.freeHeap);
  j += ",\"freq\":" + String(cf.freqHz);
  j += ",\"sf\":"   + String(cf.sf);
  j += ",\"bw\":"   + String(cf.bw);
  j += ",\"cr\":"   + String(cf.cr);
  j += ",\"pwr\":"  + String(cf.txPower);
  j += ",\"sync\":" + String(cf.syncWord);
  j += ",\"left\":"  + String(portalSecondsLeft());
  j += ",\"sc\":"    + String(st.storedCount);
  j += ",\"scap\":"  + String(st.storedCap);
  j += ",\"sbytes\":"+ String(st.storedBytes);
  j += ",\"nid\":" + String(linkNodeId());
  j += ",\"peer\":" + String(linkPeer());
  j += ",\"mode\":" + String((int)linkState());
  j += ",\"session\":" + String(linkSession());
  j += ",\"pleft\":" + String(linkSecondsLeft());
  j += ",\"notice\":\"" + String(linkNotice()) + "\"";
  j += ",\"taken\":" + String(linkTakenMask());      // bit 0 = A0 ... bit 19 = B9
  j += ",\"idset\":" + String(linkIdentityCommitted() ? 1 : 0);
  j += ",\"scan\":"  + String(linkScanning() ? 1 : 0);
  j += ",\"pe\":";
  j += (PORTAL_ALLOW_PRESET_EDIT ? "true" : "false");
  j += "}";
  sendJson(j);
}

// Streamed, not assembled. At MAX_STORED_MESSAGES=100 the full JSON is
// ~18 KB; building that as one String would mean a ~36 KB peak on a heap
// that already has Bluedroid and the Wi-Fi stack on it. Chunks of ~1 KB
// keep the peak flat no matter how high capacity goes.
static void hInbox() {
  touchActivity();
  int n = apiStoredCount();

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");

  String buf;
  buf.reserve(1200);
  buf = "{\"items\":[";

  for (int i = 0; i < n; i++) {
    String text;
    int rssi;
    if (!apiStoredEntry(i, text, rssi)) continue;
    if (i) buf += ',';
    buf += "{\"t\":\"" + jsonEsc(text) + "\",\"r\":" + String(rssi) + "}";
    if (buf.length() > 1000) { server.sendContent(buf); buf = ""; }
  }

  buf += "]}";
  server.sendContent(buf);
  server.sendContent("");        // terminate the chunked response
}

static void hInboxClear() {
  apiClearStored();
  touchActivity();
  server.send(200, "text/plain", "ok");
}

static void hExport() {
  touchActivity();
  int n = apiStoredCount();

  server.sendHeader("Content-Disposition", "attachment; filename=inbox.txt");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");

  String buf;
  buf.reserve(1200);
  buf = "LoRa Communicator - stored messages\n";
  buf += "newest first, " + String(n) + " of " + String(MAX_STORED_MESSAGES) + "\n\n";

  for (int i = 0; i < n; i++) {
    String text;
    int rssi;
    if (!apiStoredEntry(i, text, rssi)) continue;
    buf += String(i + 1) + ". [" + String(rssi) + " dBm] " + text + "\n";
    if (buf.length() > 1000) { server.sendContent(buf); buf = ""; }
  }

  server.sendContent(buf);
  server.sendContent("");
}

static void hPresets() {
  String j = "{\"items\":[";
  for (int i = 0; i < NUM_PREDEFINED; i++) {
    if (i) j += ',';
    j += '"' + jsonEsc(apiPreset(i)) + '"';
  }
  j += "],\"edited\":[";
  for (int i = 0; i < NUM_PREDEFINED; i++) {
    if (i) j += ',';
    j += (apiPresetIsEdited(i) ? "true" : "false");
  }
  j += "]}";
  sendJson(j);
}

static void hPresetSet() {
  touchActivity();
#if !PORTAL_ALLOW_PRESET_EDIT
  server.send(403, "text/plain", "presets are read-only in this build");
  return;
#endif
  if (!server.hasArg("i")) { server.send(400, "text/plain", "no index"); return; }
  int i = server.arg("i").toInt();
  if (!linkCanSend()) { server.send(409,"text/plain","Choose ID or finish private request first"); return; }
  String t = bodyText();
  t.trim();
  if (i < 0 || i >= NUM_PREDEFINED) { server.send(400, "text/plain", "bad index"); return; }
  if (t.length() == 0 || t.length() > MAX_TX_BYTES) { server.send(400, "text/plain", "bad text"); return; }
  apiSetPreset(i, t);
  server.send(200, "text/plain", "ok");
}

static void hPresetReset() {
  touchActivity();
#if !PORTAL_ALLOW_PRESET_EDIT
  server.send(403, "text/plain", "presets are read-only in this build");
  return;
#endif
  apiResetPresets();
  server.send(200, "text/plain", "ok");
}

// Fleet controls only update state; linkTick() handles radio transmission.
static void hIdentity() {
  touchActivity();
  uint8_t id=linkIdFromName(server.arg("id"));
  bool ok=linkSetIdentity(id);
  server.send(ok ? 200 : 409,"text/plain",ok ? "ok" : "Choose A0-B9; disconnect and wait before changing ID");
}
static void hPrivateRequest() {
  touchActivity();
  bool ok=linkRequest(linkIdFromName(server.arg("id")));
  server.send(ok ? 200 : 409,"text/plain",ok ? "ok" : "Choose another ID; finish current session first");
}
static void hPrivateAccept() {
  touchActivity();
  bool same=server.arg("id")==linkName(linkPeer()) && server.arg("session")==String(linkSession());
  bool ok=same && linkAccept();
  server.send(ok ? 200 : 409,"text/plain",ok ? "ok" : "No pending request");
}
static void hPrivateDisconnect() {
  touchActivity(); linkDisconnect(); server.send(200,"text/plain","ok");
}

static void hSend() {
  touchActivity();
  if (!linkCanSend()) { server.send(409,"text/plain","Choose ID or finish private request first"); return; }
  String t = bodyText();
  t.trim();
  if (t.length() == 0)            { server.send(400, "text/plain", "empty"); return; }
  if (t.length() > MAX_TX_BYTES)  { server.send(413, "text/plain", "too long"); return; }
  if (!apiQueueOutgoing(t)) { server.send(409,"text/plain","Transmit busy or destination changed"); return; }
  server.send(200, "text/plain", "queued");
}

static void hConfig() {
  touchActivity();
  if (linkState()!=LINK_PUBLIC) { server.send(409,"text/plain","Disconnect before changing radio settings"); return; }
  // Re-tuning the SX1278 while it is mid-packet corrupts the transmission
  // and can leave the radio in a state that needs a reset.
  if (apiRadioBusy()) { server.send(409,"text/plain","Radio is transmitting - try again in a moment"); return; }
  LoRaCfg c;  apiGetLoRaCfg(c);

  if (server.hasArg("freq")) c.freqHz   = server.arg("freq").toInt();
  if (server.hasArg("sf"))   c.sf       = server.arg("sf").toInt();
  if (server.hasArg("bw"))   c.bw       = server.arg("bw").toInt();
  if (server.hasArg("cr"))   c.cr       = server.arg("cr").toInt();
  if (server.hasArg("pwr"))  c.txPower  = server.arg("pwr").toInt();
  if (server.hasArg("sync")) c.syncWord = server.arg("sync").toInt();

  // Validate before touching the radio - a bad value here means a
  // node that can no longer be reached over the air.
  bool ok = c.freqHz >= 410000000L && c.freqHz <= 525000000L
         && c.sf >= 7  && c.sf <= 12
         && (c.bw == 62500 || c.bw == 125000 || c.bw == 250000 || c.bw == 500000)
         && c.cr >= 5  && c.cr <= 8
         && c.txPower >= 2 && c.txPower <= 20;

  if (!ok) { server.send(400, "text/plain", "range"); return; }

  apiSetLoRaCfg(c);
  server.send(200, "text/plain", "ok");
}

// Captive-portal probe URLs + everything unmatched: 302 to the page.
// Phones use the redirect to decide "this network needs sign-in" and
// pop a browser window on their own.
static void hRedirect() {
  touchActivity();
  server.sendHeader("Location", "http://" + apIp.toString() + "/", true);
  server.send(302, "text/plain", "");
}

// A browser asking for a favicon on a captive portal must not be sent
// round the redirect loop - some of them treat that as the page itself
// failing. 204 ends it in one round trip.
static void hNoContent() {
  server.send(204, "text/plain", "");
}

// Clears the stored identity so the module asks for an ID once more.
// Deliberately a separate endpoint from /api/identity: choosing an ID is
// routine, un-choosing one is not.
static void hIdentityReset() {
  touchActivity();
  if (linkState()!=LINK_PUBLIC) {
    server.send(409,"text/plain","Disconnect the private session first");
    return;
  }
  bool ok=linkResetIdentity();
  server.send(ok ? 200 : 500,"text/plain",
              ok ? "ok" : "NVS write failed - identity not cleared");
}

// Forgets which IDs other modules reported, then re-asks over the air.
static void hIdentityScan() {
  touchActivity();
  if (server.arg("forget")=="1") linkForgetClaims();
  linkStartScan();
  server.send(200,"text/plain","scanning");
}

// ------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------

void portalStart() {
  if (active) return;

  Serial.println("[portal] starting AP...");

  // ---- DO NOT "IMPROVE" THIS SEQUENCE -----------------------------
  // These four lines are exactly the bring-up from the build that was
  // known to work, and they are that way because every addition I tried
  // broke association:
  //
  //   * WiFi.softAPConfig() called before softAP() leaves the DHCP
  //     server stopped on ESP32 core 3.x, so the phone associates and
  //     then never gets an address. Android reports that as an
  //     authentication problem, i.e. "password incorrect".
  //   * WiFi.setSleep(false) before any interface is started is applied
  //     to an interface that does not exist yet.
  //   * the five-argument softAP() overload is not the same call the
  //     working firmware made.
  //
  // The SSID is the one deliberate difference: it carries this module's
  // ID so twenty handsets do not all advertise the same name. The SSID
  // is an input to the WPA2 key derivation, but both sides derive from
  // the name they can see, so it cannot cause an auth failure.
#if PORTAL_SSID_PER_MODULE
  if (linkNodeId()) apSsid=String(PORTAL_SSID)+linkName(linkNodeId());
  else {
    uint64_t mac=ESP.getEfuseMac();
    char setupName[24];
    snprintf(setupName,sizeof(setupName),"Setup-%04X%08X",(unsigned)((mac>>32)&0xFFFF),(unsigned)(mac&0xFFFFFFFF));
    apSsid=setupName;
  }
#else
  apSsid = PORTAL_SSID_FIXED;
#endif

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), PORTAL_PASSWORD, PORTAL_CHANNEL);
  delay(120);
  apIp = WiFi.softAPIP();

  server.on("/",                   HTTP_GET,  hRoot);
  server.on("/api/status",         HTTP_GET,  hStatus);
  server.on("/api/inbox",          HTTP_GET,  hInbox);
  server.on("/api/inbox/clear",    HTTP_POST, hInboxClear);
  server.on("/api/export",         HTTP_GET,  hExport);
  server.on("/api/presets",        HTTP_GET,  hPresets);
  server.on("/api/preset",         HTTP_POST, hPresetSet);
  server.on("/api/presets/reset",  HTTP_POST, hPresetReset);
  server.on("/api/send",           HTTP_POST, hSend);
  server.on("/api/config",         HTTP_POST, hConfig);
  server.on("/api/identity",          HTTP_POST, hIdentity);
  server.on("/api/identity/reset",    HTTP_POST, hIdentityReset);
  server.on("/api/identity/scan",     HTTP_POST, hIdentityScan);
  server.on("/api/private/request",   HTTP_POST, hPrivateRequest);
  server.on("/api/private/accept",    HTTP_POST, hPrivateAccept);
  server.on("/api/private/disconnect",HTTP_POST, hPrivateDisconnect);

  // Same four probe handlers, answering the same way, as the working build.
  server.on("/generate_204",       HTTP_GET,  hRedirect);  // Android
  server.on("/gen_204",            HTTP_GET,  hRedirect);
  server.on("/hotspot-detect.html",HTTP_GET,  hRedirect);  // iOS / macOS
  server.on("/ncsi.txt",           HTTP_GET,  hRedirect);  // Windows
  server.onNotFound(hRedirect);

  server.begin();

#if PORTAL_CAPTIVE
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIp);
#endif

  active      = true;
  pageServed  = false;
  lastRequest = millis();

  Serial.printf("[portal] up  SSID=%s  pass=%s  http://%s  freeHeap=%u\n",
                apSsid.c_str(), PORTAL_PASSWORD, apIp.toString().c_str(),
                (unsigned)ESP.getFreeHeap());
  Serial.println("[portal] open the address WITH http:// in front of it");

  apiPortalStateChanged(true);
}

void portalStop() {
  if (!active) return;

#if PORTAL_CAPTIVE
  dns.stop();
#endif
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  active = false;
  Serial.printf("[portal] down  freeHeap=%u\n", (unsigned)ESP.getFreeHeap());

  apiPortalStateChanged(false);
}

void portalToggle() { active ? portalStop() : portalStart(); }

bool portalIsActive() { return active; }

String portalIpString() { return active ? apIp.toString() : String("-"); }
String portalSsidString() { return apSsid; }

uint32_t portalSecondsLeft() {
  if (!active) return 0;
  unsigned long gone = millis() - lastRequest;
  if (gone >= PORTAL_IDLE_MS) return 0;
  return (PORTAL_IDLE_MS - gone) / 1000UL;
}

int  portalClientCount()  { return active ? WiFi.softAPgetStationNum() : 0; }
bool portalPageWasServed() { return pageServed; }

// The page polls /api/status every 3 s. A 1.2 s window after each request
// therefore leaves a comfortable 1.8 s gap in which a transmission can be
// started, while still covering the moment right after a request when the
// browser is waiting on a reply.
bool portalBusyWithBrowser() {
  return active && (millis() - lastRequest) < 1200UL;
}


void portalLoop() {
  if (!active) return;

  server.handleClient();
#if PORTAL_CAPTIVE
  dns.processNextRequest();
#endif

  if (millis() - lastRequest > PORTAL_IDLE_MS) {
    Serial.println("[portal] idle timeout - shutting Wi-Fi down");
    portalStop();
  }
}
