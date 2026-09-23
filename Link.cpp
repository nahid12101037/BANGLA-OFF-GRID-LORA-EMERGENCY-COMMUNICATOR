#include "Link.h"
#include <Preferences.h>
#ifndef LINK_HOST_TEST
#include <esp_system.h>
#endif

// Header: magic, type, source, destination, 32-bit session (big endian).
// Broadcast text has destination 255 and session zero. Private text requires
// the exact peer and session. Never accept bare text or the old protocol.
enum FrameType { FLEET_PKT_TEXT=0, FLEET_PKT_REQUEST, FLEET_PKT_ACCEPT, FLEET_PKT_CONFIRM,
                 FLEET_PKT_READY, FLEET_PKT_LEAVE, FLEET_PKT_BUSY, FLEET_PKT_PING,
                 FLEET_PKT_PONG,
                 // Identity announcements. These two are the only frames a
                 // module with no ID is allowed to send or act on.
                 FLEET_PKT_WHOIS, FLEET_PKT_CLAIM };

static const uint32_t REQUEST_MS=90000UL, HANDSHAKE_MS=60000UL;
static const uint32_t RETRY_MS=8000UL, HEARTBEAT_MS=25000UL, LEASE_MS=180000UL;
// A CLAIM is re-broadcast this often even with nobody asking, so a module
// switched on much later still learns the fleet without a scan.
static const uint32_t CLAIM_BEACON_MS=600000UL;

static uint8_t selfId=0, peer=0;
static bool    committed=false;
static uint32_t ownerTag=0;
static uint32_t takenMask=0;            // bit (id-1) = owned by another module
static uint32_t session=0, deadline=0, nextRetry=0, lastHeard=0, revision=0;
static LinkState state=LINK_PUBLIC;
static bool initiator=false, event=false;
static const char *notice="Choose your ID";
static LinkMsg noticeCode=LMSG_CHOOSE_ID;

// Disconnect retries survive returning to broadcast mode.
static uint8_t leavePeer=0, leaveCount=0;
static uint32_t leaveSession=0, leaveAt=0;

// Delayed response: no radio calls inside HTTP handlers or the RX parser.
static uint8_t responseType=0, responsePeer=0;
static uint32_t responseSession=0, responseAt=0;

// Identity discovery. claimAt is deliberately a SEPARATE slot from the
// responseX trio above: an incoming WHOIS must never cancel a pending
// BUSY or READY that a half-built private session is waiting on.
static uint32_t scanUntil=0, nextWhois=0, claimAt=0, claimBeacon=0;
static uint8_t  whoisLeft=0, claimLeft=0;

static uint32_t random32() {
#ifdef LINK_HOST_TEST
  return (uint32_t)rand()*2654435761u+(uint32_t)rand();
#else
  return esp_random();
#endif
}
static uint32_t deviceTag() {
#ifdef LINK_HOST_TEST
  extern uint32_t linkHostTag;
  return linkHostTag;
#else
  uint64_t mac=ESP.getEfuseMac();
  uint32_t t=(uint32_t)(mac^(mac>>32));
  return t ? t : 1;              // zero is reserved for "unknown"
#endif
}
static bool validId(uint8_t id) { return id>=1 && id<=LINK_FLEET_SIZE; }
static bool due(uint32_t now,uint32_t when) { return (int32_t)(now-when)>=0; }

static void changed(const char *message, LinkMsg code) {
  notice=message; noticeCode=code; ++revision; event=true;
}
static void resetLink(const char *message, LinkMsg code) {
  state=LINK_PUBLIC; peer=0; session=0; deadline=0; initiator=false;
  responsePeer=0; changed(message,code);
}
static void leaveLater(uint8_t target,uint32_t token) {
  if (!target || !token) return;
  leavePeer=target; leaveSession=token; leaveCount=4; leaveAt=millis();
}
static void respond(uint8_t type,uint8_t target,uint32_t token) {
  if (responsePeer && type==FLEET_PKT_BUSY) return;
  responseType=type; responsePeer=target; responseSession=token;
  responseAt=millis()+200+(random32()%401);
}

// ------------------------------------------------------------
// Persistence. Everything identity-related lives in one namespace and
// is read back after writing: a silent NVS failure that leaves the
// module asking for an ID at every boot is exactly the bug this
// guards against.
// ------------------------------------------------------------
static void loadIdentity() {
  Preferences p;
  selfId=0; committed=false; takenMask=0;
  if (!p.begin("fleet20",true)) return;   // namespace absent = first ever boot
  uint8_t id=p.getUChar("id",0);
  committed=p.getUChar("set",0)==1;
  takenMask=p.getUInt("taken",0);
  p.end();
  if (validId(id) && committed) selfId=id;
  else { selfId=0; committed=false; }
}

static bool storeIdentity(uint8_t id, bool commit) {
  Preferences p;
  if (!p.begin("fleet20",false)) return false;
  p.putUChar("id",id);
  p.putUChar("set",commit ? 1 : 0);
  // Read back through the same handle: putUChar can report success and
  // still lose the value if the NVS partition is full or unformatted.
  uint8_t back=p.getUChar("id",0), flag=p.getUChar("set",0);
  p.end();
  return back==id && flag==(commit ? 1 : 0);
}

static void storeTaken() {
  Preferences p;
  if (!p.begin("fleet20",false)) return;
  p.putUInt("taken",takenMask);
  p.end();
}

static void noteClaim(uint8_t id) {
  if (!validId(id)) return;
  uint32_t bit=1UL<<(id-1);
  if (takenMask & bit) return;
  takenMask|=bit;
  storeTaken();
  ++revision; event=true;              // the chooser repaints the freed/taken row
}

// ------------------------------------------------------------
// Frames
// ------------------------------------------------------------
static size_t frame(uint8_t type,uint8_t dst,uint32_t token,
                    const uint8_t *data,size_t len,uint8_t *out,size_t cap) {
  // WHOIS is the one frame a module with no ID may send, so it is the one
  // frame allowed to carry source 0.
  if (type!=FLEET_PKT_WHOIS && !validId(selfId)) return 0;
  if (cap<LINK_HDR_LEN+len) return 0;
  out[0]=LINK_MAGIC; out[1]=type; out[2]=selfId; out[3]=dst;
  out[4]=(uint8_t)(token>>24); out[5]=(uint8_t)(token>>16);
  out[6]=(uint8_t)(token>>8); out[7]=(uint8_t)token;
  if (len) memcpy(out+LINK_HDR_LEN,data,len);
  return LINK_HDR_LEN+len;
}
static void sendControl(uint8_t type,uint8_t dst,uint32_t token) {
  uint8_t out[LINK_HDR_LEN];
  size_t n=frame(type,dst,token,NULL,0,out,sizeof(out));
  if (n) apiRawTransmit(out,n);
}
// CLAIM carries the sender's 32-bit hardware tag so two modules that
// somehow pick the same ID at the same moment can settle it without a
// human: lower tag keeps the ID, higher tag stands down.
static void sendClaim() {
  if (!validId(selfId)) return;
  uint8_t out[LINK_HDR_LEN+4];
  uint8_t tag[4]={(uint8_t)(ownerTag>>24),(uint8_t)(ownerTag>>16),
                  (uint8_t)(ownerTag>>8),(uint8_t)ownerTag};
  size_t n=frame(FLEET_PKT_CLAIM,LINK_BROADCAST,0,tag,4,out,sizeof(out));
  if (n) apiRawTransmit(out,n);
  claimBeacon=millis()+CLAIM_BEACON_MS;
}
static void sendWhois() {
  uint8_t out[LINK_HDR_LEN];
  size_t n=frame(FLEET_PKT_WHOIS,LINK_BROADCAST,0,NULL,0,out,sizeof(out));
  if (n) apiRawTransmit(out,n);
}
// Answer a WHOIS after a random delay inside the asking module's listening
// window, so twenty modules do not all key up in the same millisecond.
static void scheduleClaim() {
  if (!validId(selfId)) return;
  if (claimLeft) return;                       // one answer per question
  claimLeft=1;
  claimAt=millis()+300+(random32()%(LINK_SCAN_MS-1500));
}

void linkBegin() {
  loadIdentity();
  ownerTag=deviceTag();
  leaveCount=0; responsePeer=0; claimLeft=0; whoisLeft=0; scanUntil=0;
  claimBeacon=millis()+CLAIM_BEACON_MS;
  if (selfId) {
    // Already ours from a previous boot: announce it so any module that
    // is choosing right now strikes it off its list, then carry on.
    claimLeft=1; claimAt=millis()+500+(random32()%1500);
    resetLink("Broadcast mode",LMSG_BROADCAST);
  } else {
    resetLink("Choose your ID",LMSG_CHOOSE_ID);
  }
}

uint8_t linkNodeId() { return selfId; }
bool linkIdentityCommitted() { return committed && validId(selfId); }

String linkName(uint8_t id) {
  if (!validId(id)) return "--";
  char name[3]={(char)('A'+(id-1)/10),(char)('0'+(id-1)%10),0};
  return String(name);
}
uint8_t linkIdFromName(const String &name) {
  if (name.length()!=2 || (name[0]!='A' && name[0]!='B') || name[1]<'0' || name[1]>'9') return 0;
  return (uint8_t)((name[0]-'A')*10+(name[1]-'0')+1);
}

uint32_t linkTakenMask() { return takenMask; }
bool linkIdTaken(uint8_t id) {
  return validId(id) && (takenMask & (1UL<<(id-1)))!=0;
}
int linkFreeCount() {
  int n=0;
  for (uint8_t id=1;id<=LINK_FLEET_SIZE;++id) if (!linkIdTaken(id)) ++n;
  return n;
}
void linkForgetClaims() {
  takenMask=0; storeTaken(); ++revision; event=true;
}
void linkStartScan() {
  scanUntil=millis()+LINK_SCAN_MS;
  whoisLeft=3; nextWhois=millis();
  changed("Scanning for used IDs",LMSG_SEARCHING);
}
bool linkScanning() { return scanUntil && !due(millis(),scanUntil); }

bool linkSetIdentity(uint8_t id) {
  if (!validId(id) || state!=LINK_PUBLIC || leaveCount) return false;
  if (linkIdTaken(id)) { changed("That ID is in use",LMSG_ID_BUSY); return false; }
  if (!storeIdentity(id,true)) {
    // Do not pretend. Without this the module would look configured until
    // the next reboot and then ask again, which is impossible to diagnose.
    Serial.println("[link] NVS write FAILED - identity not saved");
    changed("Could not save ID",LMSG_ID_BUSY);
    return false;
  }
  selfId=id; committed=true;
  claimLeft=3; claimAt=millis();          // tell the fleet three times
  changed("ID saved; broadcast",LMSG_ID_SAVED);
  return true;
}

bool linkResetIdentity() {
  if (!storeIdentity(0,false)) return false;
  selfId=0; committed=false;
  leaveCount=0; claimLeft=0;
  resetLink("Choose your ID",LMSG_CHOOSE_ID);
  return true;
}

LinkState linkState() { return state; }
const char *linkStateName() {
  switch(state) {
    case LINK_OUTGOING:return "Request sent";
    case LINK_INCOMING:return "Incoming request";
    case LINK_ACCEPTING:case LINK_CONNECTING:return "Connecting";
    case LINK_PRIVATE:return "Private";
    default:return selfId ? "Broadcast" : "Choose ID";
  }
}
LinkMsg linkNoticeCode() { return noticeCode; }
uint8_t linkPeer() { return peer; }
bool linkIsPaired() { return state==LINK_PRIVATE; }
bool linkCanSend() { return selfId && (state==LINK_PUBLIC || state==LINK_PRIVATE); }
uint32_t linkSession() { return session; }
uint32_t linkRevision() { return revision; }
const char *linkNotice() { return notice; }
uint32_t linkSecondsLeft() {
  if (state==LINK_PUBLIC || state==LINK_PRIVATE) return 0;
  int32_t left=(int32_t)(deadline-millis());
  return left>0 ? ((uint32_t)left+999)/1000 : 0;
}

bool linkRequest(uint8_t target) {
  if (!selfId || !validId(target) || target==selfId || state!=LINK_PUBLIC) return false;
  peer=target; session=random32(); if (!session) session=1;
  initiator=true; state=LINK_OUTGOING;
  deadline=millis()+REQUEST_MS; nextRetry=millis();
  changed("Waiting for approval",LMSG_WAITING); return true;
}
bool linkAccept() {
  if (state!=LINK_INCOMING || due(millis(),deadline)) return false;
  state=LINK_ACCEPTING; deadline=millis()+HANDSHAKE_MS; nextRetry=millis();
  changed("Connecting",LMSG_CONNECTING); return true;
}
void linkDisconnect() {
  leaveLater(peer,session);
  resetLink(selfId ? "Broadcast mode" : "Choose your ID",
            selfId ? LMSG_BROADCAST : LMSG_CHOOSE_ID);
}
bool linkTakeEvent() { bool result=event; event=false; return result; }

void linkTick() {
  uint32_t now=millis();

  // ---- identity traffic first; it must work even with no ID ----
  if (whoisLeft && due(now,nextWhois)) {
    sendWhois(); --whoisLeft; nextWhois=now+1800; return;
  }
  if (claimLeft && due(now,claimAt)) {
    sendClaim(); --claimLeft;
    claimAt=now+600+(random32()%600);
    return;
  }
  if (selfId && state==LINK_PUBLIC && due(now,claimBeacon)) { sendClaim(); return; }
  if (scanUntil && due(now,scanUntil)) { scanUntil=0; ++revision; event=true; }

  if (state!=LINK_PUBLIC && state!=LINK_PRIVATE && due(now,deadline)) {
    leaveLater(peer,session); resetLink("Request timed out",LMSG_TIMED_OUT);
  } else if (state==LINK_PRIVATE && now-lastHeard>=LEASE_MS) {
    leaveLater(peer,session); resetLink("Peer lost; broadcast",LMSG_PEER_LOST);
  }
  if (leaveCount && due(now,leaveAt)) {
    sendControl(FLEET_PKT_LEAVE,leavePeer,leaveSession); --leaveCount;
    leaveAt=millis()+RETRY_MS; return;
  }
  if (responsePeer && due(now,responseAt)) {
    uint8_t target=responsePeer; responsePeer=0;
    sendControl(responseType,target,responseSession); return;
  }
  if (!due(now,nextRetry)) return;
  switch(state) {
    case LINK_OUTGOING: sendControl(FLEET_PKT_REQUEST,peer,session); break;
    case LINK_ACCEPTING: sendControl(FLEET_PKT_ACCEPT,peer,session); break;
    case LINK_CONNECTING: sendControl(FLEET_PKT_CONFIRM,peer,session); break;
    case LINK_PRIVATE:
      if (initiator) sendControl(FLEET_PKT_PING,peer,session);
      break;
    default:return;
  }
  nextRetry=millis()+(state==LINK_PRIVATE ? HEARTBEAT_MS : RETRY_MS)+(random32()%1201);
}

size_t linkBuildText(const String &text,uint8_t *out,size_t cap) {
  if (!linkCanSend() || !text.length() || text.length()>MAX_TX_BYTES) return 0;
  return frame(FLEET_PKT_TEXT,linkIsPaired() ? peer : LINK_BROADCAST,
               linkIsPaired() ? session : 0,
               (const uint8_t*)text.c_str(),text.length(),out,cap);
}

LinkRx linkParse(const uint8_t *buf,size_t len,int rssi,String &textOut,uint8_t &srcOut) {
  (void)rssi; textOut=""; srcOut=0;
  if (len<LINK_HDR_LEN || len>LINK_MAX_FRAME || buf[0]!=LINK_MAGIC) return LINK_RX_IGNORE;
  uint8_t type=buf[1],src=buf[2],dst=buf[3];
  uint32_t token=((uint32_t)buf[4]<<24)|((uint32_t)buf[5]<<16)|((uint32_t)buf[6]<<8)|buf[7];

  // ---- Identity announcements ------------------------------------
  // Deliberately handled ahead of every other check, including the
  // "do I have an ID" test: a module with no ID has to be able to hear
  // who already owns what, otherwise it cannot pick a free one.
  if (type==FLEET_PKT_CLAIM) {
    if (!validId(src) || len<LINK_HDR_LEN+4) return LINK_RX_IGNORE;
    uint32_t tag=((uint32_t)buf[LINK_HDR_LEN]<<24)|((uint32_t)buf[LINK_HDR_LEN+1]<<16)|
                 ((uint32_t)buf[LINK_HDR_LEN+2]<<8)|buf[LINK_HDR_LEN+3];
    if (src==selfId) {
      // Someone else is announcing OUR id. Whoever has the lower tag
      // keeps it; the other one gives the ID up and chooses again.
      if (tag && tag!=ownerTag && tag<ownerTag) {
        Serial.println("[link] ID collision - standing down");
        storeIdentity(0,false);
        selfId=0; committed=false; claimLeft=0;
        noteClaim(src);
        resetLink("ID clash; choose again",LMSG_ID_BUSY);
      }
      return LINK_RX_CONTROL;
    }
    noteClaim(src);
    return LINK_RX_CONTROL;
  }
  if (type==FLEET_PKT_WHOIS) {
    scheduleClaim();                 // no-op when this module has no ID
    return LINK_RX_CONTROL;
  }

  if (!selfId) return LINK_RX_IGNORE;
  if (!validId(src) || src==selfId || (dst!=selfId && dst!=LINK_BROADCAST)) return LINK_RX_IGNORE;
  bool match=src==peer && token && token==session;
  if (type==FLEET_PKT_TEXT) {
    if (len==LINK_HDR_LEN) return LINK_RX_IGNORE;
    if (dst==LINK_BROADCAST) {
      if (token || state==LINK_PRIVATE || state==LINK_ACCEPTING || state==LINK_CONNECTING) return LINK_RX_IGNORE;
    } else {
      if (state!=LINK_PRIVATE || !match) return LINK_RX_IGNORE;
      lastHeard=millis();
    }
    // Hearing a module is also proof it owns that ID.
    noteClaim(src);
    for (size_t i=LINK_HDR_LEN;i<len;++i) textOut+=(char)buf[i];
    srcOut=src; return LINK_RX_TEXT;
  }
  if (dst!=selfId || !token || len!=LINK_HDR_LEN || type>FLEET_PKT_PONG) return LINK_RX_IGNORE;
  noteClaim(src);
  uint32_t now=millis();
  if (type==FLEET_PKT_REQUEST) {
    if (src==leavePeer && token==leaveSession) { respond(FLEET_PKT_LEAVE,src,token); return LINK_RX_CONTROL; }
    if (state==LINK_PUBLIC) {
      peer=src; session=token; initiator=false;
      state=LINK_INCOMING; deadline=now+REQUEST_MS; changed("Select to accept",LMSG_REQ_IN);
    } else if (!match) { respond(FLEET_PKT_BUSY,src,token); }
    return LINK_RX_CONTROL; // Repeated requests never extend the decision window.
  }
  if (!match) return LINK_RX_IGNORE;
  if (type==FLEET_PKT_LEAVE || (type==FLEET_PKT_BUSY && state==LINK_OUTGOING)) {
    leavePeer=src; leaveSession=token;
    if (type==FLEET_PKT_BUSY) resetLink("Peer busy; broadcast",LMSG_PEER_BUSY);
    else                      resetLink("Peer ended session",LMSG_PEER_LEFT);
    return LINK_RX_CONTROL;
  }
  if (type==FLEET_PKT_ACCEPT && initiator && (state==LINK_OUTGOING || state==LINK_CONNECTING)) {
    if (state==LINK_OUTGOING) {
      state=LINK_CONNECTING; deadline=now+HANDSHAKE_MS; changed("Connecting",LMSG_CONNECTING);
    }
    nextRetry=now;
  } else if (type==FLEET_PKT_CONFIRM && !initiator && (state==LINK_ACCEPTING || state==LINK_PRIVATE)) {
    if (state==LINK_ACCEPTING) { state=LINK_PRIVATE; changed("Private connected",LMSG_PRIVATE); }
    lastHeard=now; respond(FLEET_PKT_READY,src,token);
  } else if (type==FLEET_PKT_READY && initiator && (state==LINK_CONNECTING || state==LINK_PRIVATE)) {
    if (state==LINK_CONNECTING) { state=LINK_PRIVATE; changed("Private connected",LMSG_PRIVATE); }
    lastHeard=now; nextRetry=now+HEARTBEAT_MS;
  } else if (type==FLEET_PKT_PING && !initiator && state==LINK_PRIVATE) {
    lastHeard=now; respond(FLEET_PKT_PONG,src,token);
  } else if (type==FLEET_PKT_PONG && initiator && state==LINK_PRIVATE) { lastHeard=now; }
  return LINK_RX_CONTROL;
}
