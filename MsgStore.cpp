#include "MsgStore.h"

#include <LittleFS.h>
#include <Preferences.h>

// ------------------------------------------------------------
// On-disk layout
//
//   [ MsgHdr : 16 B ][ MsgRec 0 ][ MsgRec 1 ] ... [ MsgRec cap-1 ]
//
// The record area is preallocated at creation and never resized, so a
// write is always seek + 184 bytes at a known offset.
// ------------------------------------------------------------

#define MSG_PATH     "/inbox.bin"
#define MSG_TMP      "/inbox.new"
#define MSG_MAGIC    0x314C4D53UL     // "SML1"
#define MSG_VERSION  1

struct __attribute__((packed)) MsgHdr {
  uint32_t magic;
  uint16_t version;
  uint16_t capacity;    // records the file was built for
  uint16_t count;       // 0..capacity
  uint16_t nextIdx;     // slot the next message goes into
  uint32_t reserved;
};

struct __attribute__((packed)) MsgRec {
  uint16_t len;                  // bytes used in text[]
  int16_t  rssi;                 // dBm at reception
  char     text[MAX_TX_BYTES];
};

static const size_t HDR_SZ = sizeof(MsgHdr);
static const size_t REC_SZ = sizeof(MsgRec);

static bool    ready = false;
static MsgHdr  hdr;

static inline size_t recOffset(int slot) { return HDR_SZ + (size_t)slot * REC_SZ; }

// ------------------------------------------------------------
// Header read/write
// ------------------------------------------------------------

static bool readHdr(MsgHdr &h) {
  File f = LittleFS.open(MSG_PATH, "r");
  if (!f) return false;
  bool ok = (f.read((uint8_t *)&h, HDR_SZ) == (int)HDR_SZ);
  f.close();
  return ok;
}

static bool writeHdr() {
  File f = LittleFS.open(MSG_PATH, "r+");
  if (!f) return false;
  f.seek(0);
  bool ok = (f.write((const uint8_t *)&hdr, HDR_SZ) == HDR_SZ);
  f.close();
  return ok;
}

// ------------------------------------------------------------
// Creation
// ------------------------------------------------------------

// Builds a fresh, fully preallocated file. Zero-fills the record area
// so that later writes are pure overwrites - seeking past EOF and
// writing does not reliably fill the gap.
static bool createFile(const char *path, uint16_t capacity) {
  File f = LittleFS.open(path, "w");
  if (!f) { Serial.println("[msgstore] create failed"); return false; }

  MsgHdr h;
  h.magic    = MSG_MAGIC;
  h.version  = MSG_VERSION;
  h.capacity = capacity;
  h.count    = 0;
  h.nextIdx  = 0;
  h.reserved = 0;
  f.write((const uint8_t *)&h, HDR_SZ);

  MsgRec blank;
  memset(&blank, 0, REC_SZ);
  for (uint16_t i = 0; i < capacity; i++) {
    if (f.write((const uint8_t *)&blank, REC_SZ) != REC_SZ) {
      Serial.printf("[msgstore] preallocate failed at record %u\n", i);
      f.close();
      return false;
    }
  }
  f.close();
  Serial.printf("[msgstore] created %s: %u slots, %u bytes\n",
                path, capacity, (unsigned)(HDR_SZ + (size_t)capacity * REC_SZ));
  return true;
}

// ------------------------------------------------------------
// One-time import of the old 15-message NVS ring buffer, so upgrading
// the firmware does not throw away messages already on the device.
// ------------------------------------------------------------

#define OLD_NS         "msgstore"
#define OLD_CAPACITY   15

static void importFromNvs() {
  Preferences p;
  if (!p.begin(OLD_NS, true)) return;

  int count   = p.getInt("count", 0);
  int nextIdx = p.getInt("nextIdx", 0);
  if (count <= 0) { p.end(); return; }
  if (count > OLD_CAPACITY) count = OLD_CAPACITY;

  // Walk oldest -> newest so the new ring ends up in the same order.
  int imported = 0;
  for (int n = count - 1; n >= 0; n--) {
    int idx = (nextIdx - 1 - n + OLD_CAPACITY) % OLD_CAPACITY;
    String s = p.getString(("msg" + String(idx)).c_str(), "");
    if (s.length() > 0) {
      msgStoreAdd(s, 0);      // no RSSI was recorded by the old code
      imported++;
    }
  }
  p.end();
  Serial.printf("[msgstore] imported %d message(s) from the old NVS store\n", imported);
}

// ------------------------------------------------------------
// Capacity change: rebuild the file, keeping the newest messages that
// still fit. Copies record by record so peak RAM stays at one record.
// ------------------------------------------------------------

static bool rebuildForNewCapacity(const MsgHdr &old, uint16_t newCap) {
  Serial.printf("[msgstore] capacity changed %u -> %u, migrating\n",
                old.capacity, newCap);

  if (LittleFS.exists(MSG_TMP)) LittleFS.remove(MSG_TMP);
  if (!createFile(MSG_TMP, newCap)) return false;

  int keep = old.count;
  if (keep > newCap) keep = newCap;

  File src = LittleFS.open(MSG_PATH, "r");
  File dst = LittleFS.open(MSG_TMP, "r+");
  if (!src || !dst) { if (src) src.close(); if (dst) dst.close(); return false; }

  MsgRec rec;
  int written = 0;
  for (int n = keep - 1; n >= 0; n--) {                 // oldest kept -> newest
    int slot = (old.nextIdx - 1 - n + old.capacity) % old.capacity;
    if (!src.seek(recOffset(slot))) continue;
    if (src.read((uint8_t *)&rec, REC_SZ) != (int)REC_SZ) continue;
    if (rec.len == 0 || rec.len > MAX_TX_BYTES) continue;
    if (!dst.seek(recOffset(written))) break;
    if (dst.write((const uint8_t *)&rec, REC_SZ) != REC_SZ) break;
    written++;
  }

  MsgHdr nh;
  nh.magic    = MSG_MAGIC;
  nh.version  = MSG_VERSION;
  nh.capacity = newCap;
  nh.count    = written;
  nh.nextIdx  = (written >= newCap) ? 0 : written;
  nh.reserved = 0;
  dst.seek(0);
  dst.write((const uint8_t *)&nh, HDR_SZ);

  src.close();
  dst.close();

  LittleFS.remove(MSG_PATH);
  if (!LittleFS.rename(MSG_TMP, MSG_PATH)) {
    Serial.println("[msgstore] rename failed - starting empty");
    return createFile(MSG_PATH, newCap);
  }
  hdr = nh;
  Serial.printf("[msgstore] migrated, kept %d message(s)\n", written);
  return true;
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

bool msgStoreBegin() {
  if (!LittleFS.begin(true)) {          // true = format if unformatted
    Serial.println("[msgstore] LittleFS mount FAILED - inbox disabled.");
    Serial.println("[msgstore] check Partition Scheme has a SPIFFS/data partition.");
    ready = false;
    return false;
  }

  bool fresh = false;

  if (!LittleFS.exists(MSG_PATH)) {
    if (!createFile(MSG_PATH, MAX_STORED_MESSAGES)) return false;
    fresh = true;
  }

  MsgHdr on;
  if (!readHdr(on) || on.magic != MSG_MAGIC || on.version != MSG_VERSION) {
    Serial.println("[msgstore] inbox unreadable or wrong version - recreating");
    LittleFS.remove(MSG_PATH);
    if (!createFile(MSG_PATH, MAX_STORED_MESSAGES)) return false;
    readHdr(on);
    fresh = true;
  }

  hdr   = on;
  ready = true;

  if (hdr.capacity != MAX_STORED_MESSAGES) {
    if (!rebuildForNewCapacity(on, MAX_STORED_MESSAGES)) { ready = false; return false; }
  }
  if (hdr.count > hdr.capacity) hdr.count = hdr.capacity;   // paranoia

  if (fresh) importFromNvs();

  Serial.printf("[msgstore] ready: %u/%u stored, %u bytes on flash\n",
                hdr.count, hdr.capacity, (unsigned)msgStoreBytes());
  return true;
}

bool msgStoreReady() { return ready; }

int msgStoreCount() { return ready ? (int)hdr.count : 0; }

uint32_t msgStoreBytes() {
  if (!ready) return 0;
  return HDR_SZ + (uint32_t)hdr.capacity * REC_SZ;
}

void msgStoreAdd(const String &text, int rssi) {
  if (!ready || text.length() == 0) return;

  MsgRec rec;
  memset(&rec, 0, REC_SZ);
  size_t n = text.length();
  if (n > MAX_TX_BYTES) n = MAX_TX_BYTES;      // truncate rather than corrupt
  memcpy(rec.text, text.c_str(), n);
  rec.len  = (uint16_t)n;
  rec.rssi = (int16_t)rssi;

  File f = LittleFS.open(MSG_PATH, "r+");
  if (!f) { Serial.println("[msgstore] open for write failed"); return; }
  if (!f.seek(recOffset(hdr.nextIdx))) { f.close(); return; }
  if (f.write((const uint8_t *)&rec, REC_SZ) != REC_SZ) {
    Serial.println("[msgstore] record write failed");
    f.close();
    return;
  }

  hdr.nextIdx = (hdr.nextIdx + 1) % hdr.capacity;
  if (hdr.count < hdr.capacity) hdr.count++;

  f.seek(0);
  f.write((const uint8_t *)&hdr, HDR_SZ);
  f.close();

  Serial.printf("[msgstore] saved (%u/%u stored)\n", hdr.count, hdr.capacity);
}

bool msgStoreEntry(int n, String &text, int &rssi) {
  text = "";
  rssi = 0;
  if (!ready || n < 0 || n >= (int)hdr.count) return false;

  int slot = (hdr.nextIdx - 1 - n + hdr.capacity) % hdr.capacity;

  File f = LittleFS.open(MSG_PATH, "r");
  if (!f) return false;
  MsgRec rec;
  bool ok = f.seek(recOffset(slot)) && f.read((uint8_t *)&rec, REC_SZ) == (int)REC_SZ;
  f.close();
  if (!ok) return false;

  if (rec.len == 0 || rec.len > MAX_TX_BYTES) return false;
  text.reserve(rec.len + 1);
  for (uint16_t i = 0; i < rec.len; i++) text += rec.text[i];
  rssi = rec.rssi;
  return true;
}

String msgStoreGet(int n) {
  String t;
  int r;
  msgStoreEntry(n, t, r);
  return t;
}

void msgStoreClear() {
  if (!ready) return;
  hdr.count   = 0;
  hdr.nextIdx = 0;
  writeHdr();          // records stay on disk but are unreachable, and
                       // get overwritten as new messages arrive
  Serial.println("[msgstore] cleared");
}
