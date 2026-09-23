#ifndef LINK_H
#define LINK_H
#include <Arduino.h>
#include "NodeApi.h"

// ============================================================
// Link.h  -  fleet identity + addressing
//
// IDs 1..10 = A0..A9, 11..20 = B0..B9; zero means not configured.
// All modules need this firmware. Address filtering is not encryption.
//
// IDENTITY IS CHOSEN ONCE. The chosen ID and a separate "committed"
// flag are both written to NVS and read back to confirm, so the ID
// screen appears on the very first boot only. Every later boot, every
// wake and every received message goes straight to the normal screens.
// linkResetIdentity() is the only way back (serial "IDRESET", or the
// button on the portal's radio tab) - see README_WEBPORTAL.md, because
// re-flashing a sketch does NOT by itself erase NVS.
//
// ID COLLISION IS PREVENTED ON THE AIR. Before the chooser is shown,
// the module broadcasts a WHOIS; every module that already owns an ID
// answers with a CLAIM. Claimed IDs are then struck off the list, so
// an ID that another module has taken is never offered here. Claims
// are remembered in NVS, so this survives a reboot too.
// ============================================================

#define LINK_FLEET_SIZE 20
#define LINK_BROADCAST 255
#define LINK_MAGIC 0xB2
#define LINK_HDR_LEN 8
#define LINK_MAX_FRAME (LINK_HDR_LEN + MAX_TX_BYTES)

// How long the chooser listens for CLAIM answers before it trusts the
// list. Claims that arrive later still update it live.
#define LINK_SCAN_MS 6000UL

enum LinkRx { LINK_RX_IGNORE, LINK_RX_TEXT, LINK_RX_CONTROL };
enum LinkState { LINK_PUBLIC, LINK_OUTGOING, LINK_INCOMING,
                 LINK_ACCEPTING, LINK_CONNECTING, LINK_PRIVATE };

// A screen-independent code for whatever linkNotice() is describing,
// so the sketch can blit the matching Bangla bitmap instead of printing
// the English string. linkNotice() is still there for the web portal.
enum LinkMsg { LMSG_NONE, LMSG_CHOOSE_ID, LMSG_BROADCAST, LMSG_ID_SAVED,
               LMSG_ID_BUSY, LMSG_WAITING, LMSG_REQ_IN, LMSG_CONNECTING,
               LMSG_PRIVATE, LMSG_TIMED_OUT, LMSG_PEER_LOST, LMSG_PEER_BUSY,
               LMSG_PEER_LEFT, LMSG_SEARCHING };

void linkBegin();
uint8_t linkNodeId();
String linkName(uint8_t id);
uint8_t linkIdFromName(const String &name);
bool linkSetIdentity(uint8_t id);

// ---- Identity, chosen once ----
bool linkIdentityCommitted();   // true from the first successful choice on
bool linkResetIdentity();       // deliberate clear; returns to the chooser

// ---- Which IDs other modules already own ----
uint32_t linkTakenMask();            // bit (id-1) set = owned by someone else
bool     linkIdTaken(uint8_t id);
int      linkFreeCount();            // IDs still selectable
void     linkStartScan();            // broadcast WHOIS and collect answers
bool     linkScanning();
void     linkForgetClaims();         // clear the remembered taken list

LinkState linkState();
const char *linkStateName();
LinkMsg linkNoticeCode();
uint8_t linkPeer();
bool linkIsPaired();
bool linkCanSend();
uint32_t linkSession();
uint32_t linkSecondsLeft();
uint32_t linkRevision();
const char *linkNotice();
bool linkRequest(uint8_t target);
bool linkAccept();
void linkDisconnect();
void linkTick();
bool linkTakeEvent();
size_t linkBuildText(const String &text, uint8_t *out, size_t outCap);
LinkRx linkParse(const uint8_t *buf, size_t len, int rssi,
                 String &textOut, uint8_t &srcOut);
#endif
