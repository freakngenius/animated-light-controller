// lifx.cpp - Wi-Fi + LIFX bulb, all inside ONE background task on core 0.
// (The promises this file keeps are written in lifx.h. The packet bytes live in lifx_proto.cpp.)
//
// THE BIG PICTURE
//   Your drawing loop runs on core 1 and must never wait. So everything slow (joining Wi-Fi,
//   searching for the bulb, waiting for answers over the network) happens here, in a separate task
//   that loops every 10 ms. The two sides only share a handful of numbers:
//     - what the user last asked for  (setDesired)   -> g_wish, g_wishAtMs
//     - which request is finished      (task)         -> g_doneId
//     - the colour the user last asked for (setDesiredColor) -> g_colorWish, g_colorId
//     - which colour request is finished   (task)     -> g_colorDoneId
//     - the last answer from the bulb  (task)         -> g_reading
//     - the connection status          (task)         -> g_status
//   Those are std::atomic values (or, for the 4-number Reading, a very short lock), so neither
//   side can ever see a half-written value.
//
// WHAT THE TASK DOES, ROUGHLY EVERY 10 ms, in this order
//   1. wifiTick       join Wi-Fi, notice when it drops, retry.
//   2. pumpRx         read any UDP packets that arrived and react to them.
//   3. discoveryTick  while we do not know the bulb (or it went quiet): shout "who is there?".
//   4. pollTick       every LIFX_POLL_MS ask the bulb "are you on?".
//   5. writeTick      deliver the newest tap, retry until the bulb confirms.
//   6. colorTick      send the newest slider colour (at most 10 packets a second, see section 11).
//   7. updateStatus   publish Online / SearchingBulb / WifiDown for the corner dot.
//
// WHICH LIFX MESSAGES (checked against https://lan.developer.lifx.com):
//   ask  "are you on?"      GetLightPower   (116) -> StateLightPower (118)
//   tell "turn on/off"      SetLightPower   (117), with a fade time -> Acknowledgement (45)
//   tell "this colour"      SetColor        (102), with a fade time; no answer is requested, so
//                           colours are simply sent three times (UDP can lose a packet)
//   find bulbs              GetService      (2)   -> StateService    (3)
//   We chose the "Light" versions (116/117/118) because the bulb is a light and 117 is the only
//   power message that takes a fade time. GetPower (20)/StatePower (22) would work too, so we also
//   accept a StatePower answer.
#include "lifx.h"
#include "config.h"
#include "secrets.h"
#include "lifx_proto.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace lifxnet {
namespace {

using lifxproto::Header;

// ===========================================================================
// 1) Timing knobs that are internal to this file (the ones you may want to
//    change live in config.h)
// ===========================================================================
constexpr uint32_t kLoopMs            = 10;      // how often the task wakes up
constexpr uint32_t kWifiFirstTryMs    = 15000;   // first connection: give up and retry after this
constexpr uint32_t kWifiBackoffMs[]   = {1000, 2000, 4000, 8000, 10000};   // retry spacing after a drop
constexpr uint8_t  kWifiBackoffSteps  = sizeof(kWifiBackoffMs) / sizeof(kWifiBackoffMs[0]);

constexpr uint32_t kDiscoveryEveryMs  = 2000;    // shout "who is there?" this often...
constexpr uint32_t kDiscoveryListenMs = 1500;    // ...and listen for answers this long each time
constexpr uint32_t kFallbackAfterMs   = 4500;    // no bulb found by broadcast: use LIFX_FALLBACK_IP

constexpr uint8_t  kMissesBeforeSearch = 3;      // unanswered polls in a row before we search again

constexpr uint32_t kAckWaitMs         = 350;     // wait this long for "got it" before resending
constexpr uint8_t  kMaxSends          = 4;       // 1 first try + 3 resends
constexpr uint32_t kVerifyExtraMs     = 150;     // wait fade time + this before asking the bulb
constexpr uint32_t kVerifyWaitMs      = 400;     // wait this long for the bulb's answer
constexpr uint8_t  kMaxVerifyQueries  = 3;

constexpr uint8_t  kMaxDevices        = 8;       // how many LIFX devices we remember per search

// Colours from the sliders (section 11). A finger can change the value 60+ times a second, the bulb
// should hear about 10 of those, and the very last value must always get through.
constexpr uint32_t kColorMinGapMs     = 100;     // never more than one SetColor per this many ms
constexpr uint32_t kColorRepeat1Ms    = 150;     // send the same value again this long after its first send...
constexpr uint32_t kColorRepeat2Ms    = 400;     // ...and once more this long after its first send
constexpr uint8_t  kColorSends        = 3;       // 1 first send + 2 repeats, in case a UDP packet is lost
constexpr uint32_t kColorFadeMs       = 100;     // the bulb fades to the new colour over this long
constexpr uint16_t kColorSaturation   = 65535;   // always full colour
constexpr uint16_t kColorKelvin       = 3500;    // white temperature; the bulb ignores it at full saturation

// ===========================================================================
// 2) Logging: one line per event, only when DEBUG_LOG is 1 in config.h
// ===========================================================================
#if DEBUG_LOG
void lifxLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void lifxLog(const char* fmt, ...) {
  char line[200];
  const uint32_t ms = millis();
  int n = snprintf(line, sizeof(line), "[lifx %lu.%lus] ", (unsigned long)(ms / 1000),
                   (unsigned long)((ms % 1000) / 100));
  if (n < 0 || n >= (int)sizeof(line)) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
  va_end(ap);
  Serial.print(line);   // one write per line, so lines from different tasks do not interleave
}
#define LOG(...) lifxLog(__VA_ARGS__)
#else
#define LOG(...) \
  do {           \
  } while (0)
#endif

// An IP address is kept as one 32-bit number (the same layout IPAddress uses). To print it:
//   LOG("... %u.%u.%u.%u ...", IPARG(ip))
#define IPARG(v)                                                                        \
  (unsigned)((v) & 0xFF), (unsigned)(((v) >> 8) & 0xFF), (unsigned)(((v) >> 16) & 0xFF), \
      (unsigned)(((v) >> 24) & 0xFF)

// Both helpers below are only used inside LOG(...), hence "unused" when DEBUG_LOG is 0.
__attribute__((unused)) const char* macText(const uint8_t mac[6]) {   // task-only: one shared buffer
  static char text[13];
  lifxproto::macToHex(mac, text);
  return text;
}

__attribute__((unused)) const char* statusName(NetStatus s) {
  switch (s) {
    case NetStatus::WifiDown:      return "WifiDown";
    case NetStatus::SearchingBulb: return "SearchingBulb";
    case NetStatus::Online:        return "Online";
  }
  return "?";
}

// ===========================================================================
// 3) State shared between the drawing thread (callers of lifx.h) and the task
// ===========================================================================
std::atomic<uint8_t>  g_status{static_cast<uint8_t>(NetStatus::WifiDown)};

// The user's latest wish packed into ONE number so it can never be read half-updated:
//    bits 31..1 = request id (goes up by one per setDesired),  bit 0 = 1 means "on".
std::atomic<uint32_t> g_wish{0};
std::atomic<uint32_t> g_wishAtMs{0};   // millis() when that wish was made
// The id of the newest request the task has finished with (confirmed, or given up on).
// writePending() is simply "newest wish id != finished id".
std::atomic<uint32_t> g_doneId{0};

portMUX_TYPE g_readingLock = portMUX_INITIALIZER_UNLOCKED;   // guards g_reading (4 numbers)
Reading g_reading = {false, false, 0, 0};

inline bool pendingNow() { return (g_wish.load() >> 1) != g_doneId.load(); }

// The colour wish from setDesiredColor(): hue and brightness packed into ONE number so they can
// never be read half-updated:  bits 31..16 = hue,  bits 15..0 = brightness.
std::atomic<uint32_t> g_colorWish{0};
std::atomic<uint32_t> g_colorId{0};      // goes up by one for every NEW wish (it may wrap, that is fine)
std::atomic<uint32_t> g_colorDoneId{0};  // the newest wish id the task has finished sending
std::atomic<bool>     g_colorEver{false};   // has setDesiredColor() been called at least once?
// colorPending() is simply "newest colour id != finished colour id", the same idea as writePending().

// Must be called with g_readingLock held.
void storeReadingLocked(bool on, uint32_t now) {
  g_reading.valid = true;
  g_reading.on    = on;
  g_reading.seq   = g_reading.seq + 1;
  g_reading.atMs  = now;
}

// A poll answer from the bulb. It becomes "the truth" only if no tap is being delivered right
// now; otherwise the screen would flip back to the old state while the bulb is still fading.
// The "is a tap pending?" check and the store happen together under the lock. A tap made at the
// same instant is therefore either seen (the answer is dropped) or made just after (the answer
// really was older than the tap, so storing it is correct).
bool publishIfIdle(bool on, uint32_t now) {
  bool stored = false;
  portENTER_CRITICAL(&g_readingLock);
  if (!pendingNow()) {
    storeReadingLocked(on, now);
    stored = true;
  }
  portEXIT_CRITICAL(&g_readingLock);
  return stored;
}

// A tap is finished. On success (`publish`) the confirmed bulb state is stored in the Reading
// FIRST and only then is writePending() released, in one locked step, so the screen never sees
// "not pending" together with the old state.
void completeWish(uint32_t id, bool publish, bool on, uint32_t now) {
  portENTER_CRITICAL(&g_readingLock);
  if (publish && (g_wish.load() >> 1) == id) storeReadingLocked(on, now);
  g_doneId.store(id);
  portEXIT_CRITICAL(&g_readingLock);
}

// ===========================================================================
// 4) State that only the task touches (no locking needed)
// ===========================================================================
struct Device {            // one LIFX device that answered a "who is there?"
  uint8_t  mac[6];
  uint32_t ip;
  uint16_t port;
  bool     inThisRound;    // answered during the current 2-second search round
};

enum class W : uint8_t {   // where delivery of the current tap stands
  Idle,                    // nothing to deliver
  NeedSend,                // about to send (or resend) SetLightPower
  WaitAck,                 // sent, waiting for "got it"
  FadeWait,                // waiting for the fade to finish before asking the bulb
  VerifyWait               // asked "are you on?", waiting for the answer
};

struct TaskState {
  // identity
  uint32_t source = 0;     // our random id, copied by the bulb into every reply
  uint8_t  seq    = 0;     // message counter, wraps 255 -> 0 by itself

  // settings parsed from config.h at start-up
  bool     targetSet = false;
  uint8_t  targetMac[6] = {0, 0, 0, 0, 0, 0};
  bool     haveFallback = false;
  uint32_t fallbackIp = 0;

  // Wi-Fi
  bool     wifiUp = false;
  bool     udpOpen = false;
  bool     dropped = false;            // true after the link was up and then went away
  uint8_t  backoffIdx = 0;
  uint32_t lastKickMs = 0;
  uint32_t udpRetryMs = 0;

  // the bulb we talk to
  bool     haveBulb = false;
  uint32_t bulbIp = 0;
  uint16_t bulbPort = lifxproto::kUdpPort;
  bool     haveMac = false;
  uint8_t  bulbMac[6] = {0, 0, 0, 0, 0, 0};
  bool     bulbAnswered = false;       // has THIS bulb answered since we picked it?
  int8_t   lastLoggedOn = -1;          // -1 unknown, 0 off, 1 on (only to keep the log quiet)

  // polling
  uint32_t lastPollMs = 0;
  bool     pollNow = false;            // send a poll right away (no miss counted)
  bool     pollOutstanding = false;    // last poll not answered yet
  uint8_t  missed = 0;                 // consecutive silent polls

  // searching
  Device   dev[kMaxDevices];
  uint8_t  devCount = 0;
  bool     devLogged[kMaxDevices] = {false, false, false, false, false, false, false, false};
  bool     episode = false;            // are we in a "searching" period?
  uint32_t episodeStart = 0;
  bool     roundActive = false;
  uint32_t roundStart = 0;
  bool     roundEvaluated = false;
  bool     warnedTarget = false;
  bool     warnedFull = false;

  // delivering the current tap
  W        w = W::Idle;
  uint32_t curId = 0;                  // request id we are working on
  bool     curOn = false;
  uint32_t curStartMs = 0;             // when the user made this wish
  uint8_t  sends = 0;
  uint8_t  rounds = 0;                 // how many full send-and-check rounds this tap has used
  uint32_t sendAtMs = 0;
  uint8_t  setSeq = 0;                 // sequence number of this tap's SetLightPower (kept for resends)
  bool     acked = false;
  bool     loggedQueued = false;
  uint32_t fadeAtMs = 0;
  uint8_t  verifySeq = 0;
  uint8_t  verifyTries = 0;
  uint32_t verifySentMs = 0;
  bool     verifyGot = false;
  bool     verifyOn = false;

  // delivering the current colour (section 11)
  bool     colorSent = false;          // has at least one SetColor gone out since start-up?
  uint32_t colorValue = 0;             // the value (hue<<16 | brightness) of the most recent SetColor
  uint8_t  colorSends = 0;             // how many times in a row that value has been sent (1..kColorSends)
  uint32_t colorFirstAtMs = 0;         // when that value was sent for the first time
  uint32_t colorLastAtMs = 0;          // when the most recent SetColor (of any value) went out
  bool     colorLoggedQueued = false;

  // misc
  NetStatus lastStatus = NetStatus::WifiDown;
  bool     stackLogged = false;

  uint8_t  rx[128];                    // incoming packet (LIFX replies we care about are < 48 bytes)
};

TaskState T;
WiFiUDP   udp;

// ===========================================================================
// 5) Small helpers
// ===========================================================================

// "Has `span` ms passed since `since`?" Works even when millis() wraps around (every ~49 days),
// because it subtracts as unsigned numbers instead of comparing them.
inline bool elapsed(uint32_t now, uint32_t since, uint32_t span) {
  return (uint32_t)(now - since) >= span;
}

inline uint8_t nextSeq() { return (uint8_t)(++T.seq); }   // 255 -> 0 wraps on its own

// Where the bulb is addressed. With a known id: its 6 bytes + 2 zero bytes. Without: all zeros,
// which a bulb accepts when we send straight to its IP address.
void bulbTarget(uint8_t out[8]) {
  memset(out, 0, 8);
  if (T.haveMac) memcpy(out, T.bulbMac, 6);
}

// True when we are looking for the bulb (never found it, or it stopped answering).
inline bool searching() {
  return T.wifiUp && (!T.haveBulb || T.missed >= kMissesBeforeSearch);
}

bool sendPacket(uint32_t ip, uint16_t port, const uint8_t* data, size_t len) {
  if (!T.udpOpen || len == 0) return false;
  if (!udp.beginPacket(IPAddress(ip), port)) return false;
  udp.write(data, len);
  return udp.endPacket() == 1;
}

void bumpMiss() {
  if (T.missed < 255) ++T.missed;
}

// The bulb said something to us, so it is alive.
void markAlive() {
  T.missed = 0;
  T.pollOutstanding = false;
  T.bulbAnswered = true;
}

// ===========================================================================
// 6) Wi-Fi
// ===========================================================================
void openUdp(uint32_t now) {
  if (T.udpOpen) return;
  T.udpRetryMs = now;
  // Port 0 = "any free port". The bulb answers to whatever port our request came from.
  T.udpOpen = (udp.begin(0) == 1);
  if (!T.udpOpen) LOG("could not open the UDP socket (low memory?) - will try again\n");
}

void onWifiUp(uint32_t now) {
  T.wifiUp = true;
  T.dropped = false;
  T.backoffIdx = 0;
  WiFi.setSleep(false);   // no radio power-saving: replies and taps are noticeably faster
  const uint32_t ip = (uint32_t)WiFi.localIP();
  (void)ip;               // only used by the log line (which is empty when DEBUG_LOG is 0)
  LOG("Wi-Fi connected, this board's address is %u.%u.%u.%u\n", IPARG(ip));
  openUdp(now);
  // Start fresh: anything the bulb "missed" while we were offline is not the bulb's fault.
  T.missed = 0;
  T.pollOutstanding = false;
  T.bulbAnswered = false;
  T.pollNow = true;
  T.episode = false;
}

void onWifiDown(uint32_t now) {
  T.wifiUp = false;
  T.dropped = true;
  T.backoffIdx = 0;
  T.lastKickMs = now;
  udp.stop();
  T.udpOpen = false;
  T.episode = false;
  T.devCount = 0;
  LOG("Wi-Fi lost - will keep retrying\n");
}

void kickWifi(uint32_t now) {
  if (T.dropped && (uint8_t)(T.backoffIdx + 1) < kWifiBackoffSteps) ++T.backoffIdx;   // 1,2,4,8,10,10...
  if (T.dropped) {
    LOG("Wi-Fi still down, retrying now (next retry in %lu s)\n",
        (unsigned long)(kWifiBackoffMs[T.backoffIdx] / 1000));
  } else {
    LOG("Wi-Fi not connected after %lu s, trying again\n", (unsigned long)(kWifiFirstTryMs / 1000));
  }
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  T.lastKickMs = now;
}

void wifiTick(uint32_t now) {
  const bool up = (WiFi.status() == WL_CONNECTED);
  if (up && !T.wifiUp) onWifiUp(now);
  if (!up && T.wifiUp) onWifiDown(now);

  if (T.wifiUp) {
    // The socket normally opens the moment Wi-Fi is up; if that failed, try again once a second.
    if (!T.udpOpen && elapsed(now, T.udpRetryMs, 1000)) openUdp(now);
    return;
  }
  const uint32_t wait = T.dropped ? kWifiBackoffMs[T.backoffIdx] : kWifiFirstTryMs;
  if (elapsed(now, T.lastKickMs, wait)) kickWifi(now);
}

// ===========================================================================
// 7) Finding the bulb
// ===========================================================================

// Make the bulb at `ip` the one we talk to. `mac` may be nullptr (only the IP is known).
void adoptBulb(const uint8_t* mac, uint32_t ip, uint16_t port, const char* why) {
  const bool sameIp = T.haveBulb && T.bulbIp == ip && T.bulbPort == port;
  const bool newMac = (mac != nullptr) && (!T.haveMac || memcmp(mac, T.bulbMac, 6) != 0);
  if (sameIp && !newMac) return;          // already using exactly this bulb

  T.haveBulb = true;
  T.bulbIp = ip;
  T.bulbPort = port;
  if (mac != nullptr) {
    memcpy(T.bulbMac, mac, 6);
    T.haveMac = true;
  }
  T.missed = 0;
  T.pollOutstanding = false;
  T.bulbAnswered = false;
  T.lastLoggedOn = -1;
  T.pollNow = true;
  if (T.haveMac) {
    LOG("using bulb %s at %u.%u.%u.%u (%s)\n", macText(T.bulbMac), IPARG(ip), why);
  } else {
    LOG("using bulb at %u.%u.%u.%u (%s)\n", IPARG(ip), why);
  }
}

// A LIFX device answered "who is there?". Remember it, print it once, and adopt it at once if it
// is the one we are looking for.
void noteDevice(const uint8_t mac[6], uint32_t ip, uint16_t port) {
  int idx = -1;
  for (uint8_t i = 0; i < T.devCount; ++i) {
    if (memcmp(T.dev[i].mac, mac, 6) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    if (T.devCount >= kMaxDevices) {
      if (!T.warnedFull) {
        T.warnedFull = true;
        LOG("more than %u LIFX devices answered; ignoring the extra ones\n", (unsigned)kMaxDevices);
      }
      return;
    }
    idx = T.devCount++;
    memcpy(T.dev[idx].mac, mac, 6);
    T.devLogged[idx] = false;
  }
  T.dev[idx].ip = ip;
  T.dev[idx].port = port;
  T.dev[idx].inThisRound = true;
  if (!T.devLogged[idx]) {
    T.devLogged[idx] = true;
    // Copy the id after "LIFX device" into LIFX_TARGET_MAC (config.h) to pick a specific bulb.
    LOG("LIFX device %s at %u.%u.%u.%u\n", macText(mac), IPARG(ip));
  }

  if (T.targetSet) {
    if (memcmp(mac, T.targetMac, 6) == 0) adoptBulb(mac, ip, port, "matches LIFX_TARGET_MAC");
  } else if (T.haveBulb && T.haveMac && memcmp(mac, T.bulbMac, 6) == 0) {
    adoptBulb(mac, ip, port, "same bulb as before, found again");
  }
}

void sendGetService(uint32_t ip, bool tagged) {
  uint8_t pkt[lifxproto::kMaxBuildSize];
  const size_t n = lifxproto::buildGetService(pkt, sizeof(pkt), T.source, nextSeq(), tagged);
  sendPacket(ip, lifxproto::kUdpPort, pkt, n);
}

// The docs disagree on whether a discovery broadcast is "tagged" (packet-contents page: yes;
// querying-the-device page: no), so every address gets both kinds. Devices that answer both are
// merged by their id, so the only cost is two extra tiny packets.
void sendDiscoveryTo(uint32_t ip) {
  sendGetService(ip, true);
  sendGetService(ip, false);
}

void startRound(uint32_t now) {
  T.roundActive = true;
  T.roundStart = now;
  T.roundEvaluated = false;
  for (uint8_t i = 0; i < T.devCount; ++i) T.dev[i].inThisRound = false;

  if (T.haveFallback) sendDiscoveryTo(T.fallbackIp);        // the fixed address goes first
  const uint32_t subnetBroadcast = (uint32_t)WiFi.broadcastIP();
  const uint32_t limitedBroadcast = 0xFFFFFFFFu;             // 255.255.255.255
  if (subnetBroadcast != 0) sendDiscoveryTo(subnetBroadcast);
  if (subnetBroadcast != limitedBroadcast) sendDiscoveryTo(limitedBroadcast);
}

// End of a round's listening time: choose a bulb if none was adopted on the spot.
void evaluateRound() {
  T.roundEvaluated = true;

  // If we are after one particular bulb (LIFX_TARGET_MAC, or "the same one as before"), it was
  // adopted the moment it answered. Nothing to choose here.
  const bool wantSpecific = T.targetSet || (T.haveBulb && T.haveMac);
  if (wantSpecific) {
    if (T.targetSet && T.devCount > 0 && !T.warnedTarget) {
      T.warnedTarget = true;
      LOG("no device above has the id %s from LIFX_TARGET_MAC - check the ids listed above\n",
          macText(T.targetMac));
    }
    return;
  }

  int pick = -1;
  for (uint8_t i = 0; i < T.devCount; ++i) {       // prefer the device at the fixed address
    if (T.dev[i].inThisRound && T.haveFallback && T.dev[i].ip == T.fallbackIp) {
      pick = i;
      break;
    }
  }
  for (uint8_t i = 0; pick < 0 && i < T.devCount; ++i) {   // otherwise the first one found
    if (T.dev[i].inThisRound) pick = i;
  }
  if (pick >= 0) {
    adoptBulb(T.dev[pick].mac, T.dev[pick].ip, T.dev[pick].port,
              T.devCount > 1 ? "first one found" : "only one found");
  }
}

void discoveryTick(uint32_t now) {
  const bool need = searching();
  if (need && !T.episode) {
    T.episode = true;
    T.episodeStart = now;
    T.roundActive = false;
    T.devCount = 0;          // forget the old list so every device is printed again
    T.warnedTarget = false;
    T.warnedFull = false;
    if (!T.haveBulb) {
      LOG("looking for a LIFX bulb on the network...\n");
    } else if (T.haveMac) {
      LOG("bulb %s at %u.%u.%u.%u stopped answering - searching again (its address may have changed)\n",
          macText(T.bulbMac), IPARG(T.bulbIp));
    } else {
      LOG("bulb at %u.%u.%u.%u stopped answering - searching again\n", IPARG(T.bulbIp));
    }
  }
  if (!need) {
    T.episode = false;
    return;
  }
  if (!T.udpOpen) return;

  if (!T.roundActive || elapsed(now, T.roundStart, kDiscoveryEveryMs)) {
    startRound(now);
  } else if (!T.roundEvaluated && elapsed(now, T.roundStart, kDiscoveryListenMs)) {
    evaluateRound();
  }

  // Broadcast found nothing: fall back to the fixed address from config.h, but keep looking.
  if (T.haveFallback && !T.haveBulb && elapsed(now, T.episodeStart, kFallbackAfterMs)) {
    const uint8_t* knownMac = nullptr;
    for (uint8_t i = 0; i < T.devCount; ++i) {
      if (T.dev[i].ip == T.fallbackIp) knownMac = T.dev[i].mac;
    }
    adoptBulb(knownMac, T.fallbackIp, lifxproto::kUdpPort,
              "LIFX_FALLBACK_IP; nothing found by broadcast");
  }
}

// ===========================================================================
// 8) Reading what arrives
// ===========================================================================

// Is this packet from the bulb we talk to? Also learns the bulb's id from its first reply when we
// only knew its IP address.
bool fromOurBulb(const Header& h, uint32_t from) {
  if (!T.haveBulb || from != T.bulbIp) return false;
  if (T.haveMac) return memcmp(h.target, T.bulbMac, 6) == 0;
  if (!lifxproto::targetIsZero(h.target)) {
    memcpy(T.bulbMac, h.target, 6);
    T.haveMac = true;
    LOG("the bulb at %u.%u.%u.%u has id %s (put it in LIFX_TARGET_MAC to always use this one)\n",
        IPARG(from), macText(T.bulbMac));
  }
  return true;
}

void onStateService(const uint8_t* buf, size_t len, const Header& h, uint32_t from) {
  if (!searching()) return;                      // only interesting while we are looking
  uint8_t service = 0;
  uint32_t port = 0;
  if (!lifxproto::parseStateService(buf, len, h, service, port)) return;
  if (service != lifxproto::kServiceUdp) return; // other services (reserved ones) are not for us
  if (port == 0 || port > 65535) return;
  if (lifxproto::targetIsZero(h.target)) return; // a real device always reports its own id
  noteDevice(h.target, from, (uint16_t)port);
}

void onAcknowledgement(const Header& h, uint32_t from) {
  if (!fromOurBulb(h, from)) return;
  markAlive();
  if (T.w == W::WaitAck && h.sequence == T.setSeq) T.acked = true;
}

void onPower(const uint8_t* buf, size_t len, const Header& h, uint32_t from, uint32_t now) {
  uint16_t level = 0;
  if (!lifxproto::parseStatePower(buf, len, h, level)) return;
  if (!fromOurBulb(h, from)) return;
  markAlive();
  const bool on = (level != 0);

  // The answer to the "are you on?" we sent after the fade: this is what confirms a tap.
  if (T.w == W::VerifyWait && h.sequence == T.verifySeq) {
    T.verifyGot = true;
    T.verifyOn = on;
    return;
  }
  // A reply to the SetLightPower itself (some firmware sends one): the level may be mid-fade.
  if (T.w != W::Idle && h.sequence == T.setSeq) return;

  // An ordinary poll answer. Used only if no tap is being delivered (see publishIfIdle).
  if (publishIfIdle(on, now)) {
    if (T.lastLoggedOn != (on ? 1 : 0)) {
      T.lastLoggedOn = on ? 1 : 0;
      LOG("bulb is %s\n", on ? "ON" : "OFF");
    }
  }
}

void handlePacket(const uint8_t* buf, size_t len, uint32_t from, uint32_t now) {
  Header h;
  if (!lifxproto::parseHeader(buf, len, h)) return;   // checks the length first
  if (h.source != T.source) return;                   // not an answer to something WE sent
  switch (h.type) {
    case lifxproto::kTypeStateService:
      onStateService(buf, len, h, from);
      break;
    case lifxproto::kTypeAcknowledgement:
      onAcknowledgement(h, from);
      break;
    case lifxproto::kTypeStateLightPower:
    case lifxproto::kTypeStatePower:
      onPower(buf, len, h, from, now);
      break;
    default:
      break;
  }
}

void pumpRx(uint32_t now) {
  if (!T.udpOpen) return;
  for (int i = 0; i < 8; ++i) {                       // at most 8 packets per 10 ms
    const int n = udp.parsePacket();
    if (n <= 0) return;
    const uint32_t from = (uint32_t)udp.remoteIP();
    const int got = udp.read(T.rx, sizeof(T.rx));
    udp.flush();                                      // throw away the rest of a too-long packet
    if (got > 0) handlePacket(T.rx, (size_t)got, from, now);
  }
}

// ===========================================================================
// 9) Asking the bulb "are you on?" every LIFX_POLL_MS
// ===========================================================================
void sendGetLightPower(uint8_t sequence) {
  uint8_t target[8];
  bulbTarget(target);
  uint8_t pkt[lifxproto::kMaxBuildSize];
  const size_t n = lifxproto::buildGet(pkt, sizeof(pkt), lifxproto::kTypeGetLightPower, T.source,
                                       target, sequence);
  sendPacket(T.bulbIp, T.bulbPort, pkt, n);
}

void pollTick(uint32_t now) {
  if (!T.haveBulb || !T.udpOpen) return;
  if (pendingNow()) return;       // while a tap is being delivered, its own queries do the job
  if (!T.pollNow && !elapsed(now, T.lastPollMs, (uint32_t)LIFX_POLL_MS)) return;

  if (!T.pollNow && T.pollOutstanding) bumpMiss();    // the previous question got no answer
  T.pollNow = false;
  T.pollOutstanding = true;
  T.lastPollMs = now;
  sendGetLightPower(nextSeq());
}

// ===========================================================================
// 10) Delivering taps
// ===========================================================================
void loadWish(uint32_t& wish, uint32_t& atMs) {
  for (int i = 0; i < 4; ++i) {                       // re-read until we got a consistent pair
    wish = g_wish.load();
    atMs = g_wishAtMs.load();
    if (wish == g_wish.load()) return;
  }
}

void sendSet(uint32_t now) {
  uint8_t target[8];
  bulbTarget(target);
  uint8_t pkt[lifxproto::kMaxBuildSize];
  const size_t n = lifxproto::buildSetLightPower(pkt, sizeof(pkt), T.source, target, T.setSeq,
                                                 T.curOn, (uint32_t)LIFX_FADE_MS, true);
  sendPacket(T.bulbIp, T.bulbPort, pkt, n);
  ++T.sends;
  T.sendAtMs = now;
  T.w = W::WaitAck;
}

void startNewDelivery() {                              // fresh Set (new tap, or a retry round)
  if (T.rounds < 255) ++T.rounds;
  T.sends = 0;
  T.setSeq = nextSeq();
  T.acked = false;
  T.w = W::NeedSend;
}

void writeTick(uint32_t now) {
  uint32_t wish = 0, wishAt = 0;
  loadWish(wish, wishAt);
  const uint32_t id = wish >> 1;
  if (id == g_doneId.load()) {                         // nothing waiting
    T.w = W::Idle;
    return;
  }

  // A new tap arrived (possibly while an older one was still being delivered): drop the old one
  // and start on this one right away.
  if (id != T.curId) {
    T.curId = id;
    T.curOn = (wish & 1u) != 0;
    T.curStartMs = wishAt;
    T.loggedQueued = false;
    startNewDelivery();
    T.rounds = 0;
    LOG("tap: bulb should be %s\n", T.curOn ? "ON" : "OFF");
  }

  // Too old: stop trying. Polling will show the bulb's real state again.
  if (elapsed(now, T.curStartMs, (uint32_t)LIFX_INTENT_TIMEOUT_MS)) {
    LOG("giving up on this tap: the bulb did not confirm within %lu ms\n",
        (unsigned long)LIFX_INTENT_TIMEOUT_MS);
    completeWish(id, false, false, now);
    T.w = W::Idle;
    T.pollNow = true;
    return;
  }

  // No Wi-Fi or bulb yet: keep the tap queued; it is sent the moment we can.
  if (!T.wifiUp || !T.udpOpen || !T.haveBulb) {
    if (!T.loggedQueued) {
      T.loggedQueued = true;
      LOG("tap is queued until Wi-Fi and the bulb are available\n");
    }
    if (T.w != W::NeedSend) startNewDelivery();
    return;
  }

  switch (T.w) {
    case W::Idle:       // cannot happen while a request is pending; recover by starting over
      startNewDelivery();
      break;

    case W::NeedSend:
      if (T.sends > 0 && T.rounds == 0) {   // later rounds are summarised by one line each
        LOG("no confirmation yet, sending again (try %u of %u)\n", (unsigned)(T.sends + 1),
            (unsigned)kMaxSends);
      }
      sendSet(now);
      break;

    case W::WaitAck:
      if (T.acked) {
        T.fadeAtMs = now;
        T.w = W::FadeWait;
      } else if (elapsed(now, T.sendAtMs, kAckWaitMs)) {
        if (T.sends < kMaxSends) {
          T.w = W::NeedSend;
        } else {
          if (T.rounds == 0) LOG("the bulb never acknowledged; checking its state anyway\n");
          bumpMiss();
          T.fadeAtMs = now;
          T.w = W::FadeWait;
        }
      }
      break;

    case W::FadeWait:
      if (elapsed(now, T.fadeAtMs, (uint32_t)LIFX_FADE_MS + kVerifyExtraMs)) {
        T.verifySeq = nextSeq();
        T.verifyTries = 1;
        T.verifyGot = false;
        T.verifySentMs = now;
        sendGetLightPower(T.verifySeq);
        T.w = W::VerifyWait;
      }
      break;

    case W::VerifyWait:
      if (T.verifyGot) {
        if (T.verifyOn == T.curOn) {
          LOG("bulb confirmed: it is %s\n", T.curOn ? "ON" : "OFF");
          T.lastLoggedOn = T.curOn ? 1 : 0;
          completeWish(id, true, T.verifyOn, now);   // store the Reading, THEN release pending
          T.w = W::Idle;
          T.lastPollMs = now;
        } else {
          LOG("bulb says it is %s but we want %s - sending again\n", T.verifyOn ? "ON" : "OFF",
              T.curOn ? "ON" : "OFF");
          startNewDelivery();
        }
      } else if (elapsed(now, T.verifySentMs, kVerifyWaitMs)) {
        if (T.verifyTries < kMaxVerifyQueries) {
          ++T.verifyTries;
          T.verifySentMs = now;
          sendGetLightPower(T.verifySeq);
        } else {
          LOG("the bulb is not answering - sending the tap again (round %u)\n", (unsigned)(T.rounds + 1));
          bumpMiss();
          startNewDelivery();
        }
      }
      break;
  }
}

// ===========================================================================
// 11) Sending colours (the hue / brightness sliders)
// ===========================================================================
// How this behaves (all of it is "level-triggered": every 10 ms the task simply compares what the
// user WANTS with what it LAST SENT, it never queues one packet per setDesiredColor() call):
//   - desired value differs from the last one sent  -> send the newest value, but only if at
//     least kColorMinGapMs (100 ms) have passed since the last SetColor. A drag therefore turns
//     into about 10 packets a second, and the last value of the drag is always sent: if it was
//     set too early, the next 10 ms pass will send it as soon as the 100 ms are up.
//   - desired value equals the last one sent        -> it is sent two more times, 150 ms and
//     400 ms after its first send (UDP has no delivery guarantee and we do not ask the bulb for an
//     answer). A different value arriving in the meantime restarts all of this for that value.
//   - no Wi-Fi or no bulb address yet               -> send nothing, keep the wish, send it later.
//   - setDesiredColor() was never called            -> send nothing, ever.
// SetColor never changes whether the bulb is on or off, and this code never sends SetLightPower.
void sendColor(uint32_t now) {
  uint8_t target[8];
  bulbTarget(target);
  uint8_t pkt[lifxproto::kMaxBuildSize];
  const size_t n = lifxproto::buildSetColor(
      pkt, sizeof(pkt), T.source, target, nextSeq(), (uint16_t)(T.colorValue >> 16), kColorSaturation,
      (uint16_t)(T.colorValue & 0xFFFFu), kColorKelvin, kColorFadeMs, /*ackRequired=*/false);
  sendPacket(T.bulbIp, T.bulbPort, pkt, n);
  if (T.colorSends == 0) T.colorFirstAtMs = now;
  ++T.colorSends;
  T.colorLastAtMs = now;
}

void colorTick(uint32_t now) {
  // setDesiredColor() stores the value first and then bumps the id, so read them in the opposite
  // order: the value we get is at least as new as the id we got.
  const uint32_t id = g_colorId.load();
  const uint32_t wish = g_colorWish.load();
  if (id == g_colorDoneId.load()) return;              // nothing waiting (this is also the state at boot)

  // No Wi-Fi or bulb yet: keep the wish; it is sent the moment we can.
  if (!T.wifiUp || !T.udpOpen || !T.haveBulb) {
    if (!T.colorLoggedQueued) {
      T.colorLoggedQueued = true;
      LOG("colour is queued until Wi-Fi and the bulb are available\n");
    }
    return;
  }
  T.colorLoggedQueued = false;

  if (!T.colorSent || wish != T.colorValue) {          // a value the bulb has not heard yet
    if (T.colorSent && !elapsed(now, T.colorLastAtMs, kColorMinGapMs)) return;   // too soon: next time
    T.colorSent = true;
    T.colorValue = wish;
    T.colorSends = 0;
    sendColor(now);
  } else if (T.colorSends < kColorSends) {             // same value again: the two repeats
    const uint32_t due = (T.colorSends == 1) ? kColorRepeat1Ms : kColorRepeat2Ms;
    if (elapsed(now, T.colorFirstAtMs, due) && elapsed(now, T.colorLastAtMs, kColorMinGapMs)) {
      sendColor(now);
    }
  }

  if (T.colorSends >= kColorSends) {                   // the last repeat has gone out
    g_colorDoneId.store(id);
    LOG("colour sent: hue %u, brightness %u\n", (unsigned)(T.colorValue >> 16),
        (unsigned)(T.colorValue & 0xFFFFu));
  }
}

// ===========================================================================
// 12) Status for the corner dot
// ===========================================================================
void updateStatus() {
  NetStatus s;
  if (!T.wifiUp) {
    s = NetStatus::WifiDown;
  } else if (!T.haveBulb || !T.bulbAnswered || T.missed >= kMissesBeforeSearch) {
    s = NetStatus::SearchingBulb;
  } else {
    s = NetStatus::Online;
  }
  if (s != T.lastStatus) {
    LOG("status: %s -> %s\n", statusName(T.lastStatus), statusName(s));
    T.lastStatus = s;
    g_status.store(static_cast<uint8_t>(s));
  }
}

// ===========================================================================
// 13) The task itself
// ===========================================================================
void readSettings() {
  // The bulb id the user wants (optional).
  static const char kTargetMacText[] = LIFX_TARGET_MAC;
  if (kTargetMacText[0] != '\0') {
    if (lifxproto::parseMacHex(kTargetMacText, T.targetMac)) {
      T.targetSet = true;
    } else {
      LOG("LIFX_TARGET_MAC \"%s\" is not 12 hex characters like d073d5a1b2c3 - ignoring it\n",
          kTargetMacText);
    }
  }
  // The fixed bulb address (optional).
  static const char kFallbackIpText[] = LIFX_FALLBACK_IP;
  if (kFallbackIpText[0] != '\0') {
    IPAddress ip;
    if (ip.fromString(kFallbackIpText)) {
      T.fallbackIp = (uint32_t)ip;
      T.haveFallback = true;
    } else {
      LOG("LIFX_FALLBACK_IP \"%s\" is not an address like 192.168.1.42 - ignoring it\n",
          kFallbackIpText);
    }
  }
}

void taskMain(void*) {
  // A random id for this run. The docs say it must not be 0 or 1.
  do {
    T.source = esp_random();
  } while (T.source < 2);
  T.seq = (uint8_t)esp_random();

  readSettings();

  WiFi.persistent(false);            // do not write the Wi-Fi password to flash on every start
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);              // no radio power-saving: faster replies
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  T.lastKickMs = millis();
  LOG("starting; joining Wi-Fi \"%s\"\n", WIFI_SSID);

  for (;;) {
    const uint32_t now = millis();
    wifiTick(now);
    if (T.wifiUp) {
      pumpRx(now);
      discoveryTick(now);
      pollTick(now);
    }
    writeTick(now);                  // also runs with Wi-Fi down, so a stuck tap can time out
    colorTick(now);                  // also runs with Wi-Fi down: it just keeps the wish until it can send
    updateStatus();

#if DEBUG_LOG
    // Once, 30 s after start: how close did this task come to running out of stack?
    if (!T.stackLogged && elapsed(now, 0, 30000)) {
      T.stackLogged = true;
      LOG("task stack: at most %u of 8192 bytes used so far\n",
          (unsigned)(8192 - uxTaskGetStackHighWaterMark(nullptr)));
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(kLoopMs));
  }
}

}  // namespace

// ===========================================================================
// The public functions from lifx.h (the ones the drawing loop calls)
// ===========================================================================
void begin() {
  static bool started = false;
  if (started) return;
  started = true;
  const BaseType_t ok = xTaskCreatePinnedToCore(taskMain, "lifx", 8192, nullptr, 1, nullptr, 0);
  if (ok != pdPASS) {
    started = false;
    LOG("could not start the network task (out of memory)\n");
  }
}

void setDesired(bool on) {
  const uint32_t wantOn = on ? 1u : 0u;
  uint32_t old = g_wish.load();
  const bool inFlight = (old >> 1) != g_doneId.load();

  // Calling this over and over with the same value (even once per frame) must not make the bulb
  // get the same command again and again. So it does nothing when
  //  - the very same wish is still being delivered, or
  //  - nothing is being delivered and the bulb's last answer already says it is in that state.
  if (inFlight) {
    if ((old & 1u) == wantOn) return;
  } else {
    const Reading r = reading();
    if (r.valid && r.on == on) return;
  }

  g_wishAtMs.store(millis());
  uint32_t next;
  do {
    next = (((old >> 1) + 1u) << 1) | wantOn;   // new id, same packed number
  } while (!g_wish.compare_exchange_weak(old, next));
}

bool writePending() { return pendingNow(); }

void setDesiredColor(uint16_t hue, uint16_t brightness) {
  const uint32_t packed = ((uint32_t)hue << 16) | brightness;

  // The same values again (even once per frame) change nothing: no restart of the repeats, no
  // extra packets. The very first call always counts, even for hue 0 / brightness 0.
  if (g_colorEver.load() && g_colorWish.load() == packed) return;

  g_colorWish.store(packed);   // the value first ...
  g_colorId.fetch_add(1);      // ... then the "there is something new" signal (see colorTick())
  g_colorEver.store(true);
}

bool colorPending() { return g_colorId.load() != g_colorDoneId.load(); }

NetStatus status() { return static_cast<NetStatus>(g_status.load()); }

Reading reading() {
  Reading r;
  portENTER_CRITICAL(&g_readingLock);
  r = g_reading;
  portEXIT_CRITICAL(&g_readingLock);
  return r;
}

}  // namespace lifxnet
