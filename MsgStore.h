#ifndef MSG_STORE_H
#define MSG_STORE_H

// ============================================================
// MsgStore.h  -  received-message ring buffer on LittleFS
//
// WHY NOT NVS (which is where these used to live):
// NVS is a 20 KB key/value store with 504 usable 32-byte entries, and
// a string costs 1 + ceil(len/32) of them. At 15 messages that was
// fine. At 80 it depends on how long the messages are - measured, 80
// messages at the 180-byte cap needs 633 entries and OVERFLOWS. It
// would appear to work for months and then start dropping writes
// silently the first time someone sent long messages.
//
// So the inbox moved to LittleFS, using the 1 MB data partition that
// the "Huge APP (3MB No OTA/1MB SPIFFS)" scheme already allocates and
// that this project otherwise leaves completely empty. Records are a
// FIXED size, so:
//   * message n is one seek + one 184-byte read - no scanning
//   * there is no fragmentation and no entry limit
//   * the file never grows or shrinks after creation
//   * flash wear is one record write + one 16-byte header write
//
// Storage is 16 + capacity * 184 bytes. 100 messages = 18 KB, 1.8% of
// the partition. The hard ceiling is 5,698 messages; the real limit is
// that browsing hundreds of messages with two buttons is unpleasant,
// which is a UI question, not a storage one.
//
// NVS is left holding only the presets and radio config (~60 entries,
// 12% of the partition), which is what it is actually good at.
// ============================================================

#include <Arduino.h>
#include "NodeApi.h"

// Mounts LittleFS and opens/creates the inbox. Safe to call once from
// setup(). Returns false if the filesystem could not be mounted, in
// which case every other call below degrades to "empty inbox" rather
// than crashing.
bool   msgStoreBegin();

bool   msgStoreReady();
int    msgStoreCount();                       // 0..MAX_STORED_MESSAGES
void   msgStoreAdd(const String &text, int rssi);
String msgStoreGet(int n);                    // n = 0 is newest
bool   msgStoreEntry(int n, String &text, int &rssi);
void   msgStoreClear();
uint32_t msgStoreBytes();                     // file size, for diagnostics

#endif  // MSG_STORE_H
