// sim_lifx.cpp - runs the REAL cyd_pixel_toggle/lifx.cpp on your laptop against a pretend LIFX bulb.
//
// Nothing here touches a network or a board. A fake clock advances in 10 ms steps, in lock-step with
// the network task (so every run gives the same result), and a fake bulb answers the way the LIFX docs
// say a real one does: it acknowledges, fades over the requested time (so a poll in the middle of a
// fade sees an in-between level), can lose packets, can disappear, and can change its IP address.
// It also takes colour changes (SetColor) without ever touching its power state, and the simulator
// writes down every SetColor the board puts on the wire (with the time) so the tests can count them.
//
// Usage:  sim_lifx <scenario> [clock-start-in-ms]
//   The clock-start lets us begin just before millis() wraps around (49 days) to prove nothing breaks.
// Build + run everything:  ../build_lifx_sim.sh
#include "Arduino.h"
#include "WiFi.h"
#include "WiFiUdp.h"
#include "esp_system.h"
#include "lifx.h"
#include "lifx_proto.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace lifxproto;

// ===========================================================================
// Fake clock + lock-step task runner
// ===========================================================================
static std::atomic<uint32_t> g_now(0);
static std::mutex g_m;
static std::condition_variable g_cv;
static bool g_taskRunning = false;
static bool g_resume = false;

uint32_t millis() { return g_now.load(); }

void vTaskDelay(uint32_t) {
  std::unique_lock<std::mutex> lk(g_m);
  g_taskRunning = false;
  g_cv.notify_all();
  g_cv.wait(lk, [] { return g_resume; });
  g_resume = false;
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char*, uint32_t, void* arg, int, void*,
                                   int) {
  {
    std::lock_guard<std::mutex> lk(g_m);
    g_taskRunning = true;
  }
  std::thread(fn, arg).detach();
  std::unique_lock<std::mutex> lk(g_m);
  g_cv.wait(lk, [] { return !g_taskRunning; });   // wait until the task reaches its first delay
  return pdPASS;
}

UBaseType_t uxTaskGetStackHighWaterMark(void*) { return 4000; }

static void stepOnce() {   // advance 10 ms and let the task run exactly one loop
  std::unique_lock<std::mutex> lk(g_m);
  g_now += 10;
  g_resume = true;
  g_taskRunning = true;
  g_cv.notify_all();
  g_cv.wait(lk, [] { return !g_taskRunning; });
}

uint32_t esp_random() {
  static uint32_t x = 0x1234567u;
  x = x * 1664525u + 1013904223u;
  return x | 2u;
}

// ===========================================================================
// Log capture
// ===========================================================================
static std::vector<std::string> g_log;
static bool g_verbose = false;

void HardwareSerialMock::print(const char* s) {
  g_log.push_back(s);
  if (g_verbose) fputs(s, stdout);
}
HardwareSerialMock Serial;

static bool logHas(const char* needle) {
  for (size_t i = 0; i < g_log.size(); ++i) {
    if (g_log[i].find(needle) != std::string::npos) return true;
  }
  return false;
}

// ===========================================================================
// Fake Wi-Fi
// ===========================================================================
static bool g_linkAvail = true;          // is the router reachable?
static uint32_t g_availSince = 0;
static bool g_begun = false, g_forcedDown = false;
static uint32_t g_beginAt = 0;
static const uint32_t kConnectMs = 2000;   // how long joining takes

void WiFiMock::begin(const char*, const char*) {
  g_begun = true;
  g_forcedDown = false;
  g_beginAt = millis();
}
bool WiFiMock::disconnect() {
  g_forcedDown = true;
  return true;
}
wl_status_t WiFiMock::status() {
  if (!g_begun || g_forcedDown || !g_linkAvail) return WL_DISCONNECTED;
  const uint32_t now = millis();
  const uint32_t e1 = now - g_beginAt, e2 = now - g_availSince;
  return ((e1 < e2 ? e1 : e2) >= kConnectMs) ? WL_CONNECTED : WL_DISCONNECTED;
}
WiFiMock WiFi;

static void setLink(bool up) {
  g_linkAvail = up;
  if (up) g_availSince = millis();
}

// ===========================================================================
// Fake bulbs + fake network
// ===========================================================================
static uint32_t ip4(int a, int b, int c, int d) {
  return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}
static uint32_t subnetBroadcast() { return ip4(192, 168, 1, 255); }
static uint32_t limitedBroadcast() { return 0xFFFFFFFFu; }

struct SetRecord {
  bool on;
  uint32_t duration;
  bool ack;
  bool targetMatches;   // packet carried the bulb's id (vs all zeros)
  uint8_t seq;
};

struct ColorRecord {   // one SetColor that REACHED the bulb (the ones it "lost" are not listed)
  uint16_t hue, saturation, brightness, kelvin;
  uint32_t duration;
  bool targetMatches;   // packet carried the bulb's id (vs all zeros)
  uint32_t at;          // clock value when it arrived
};

struct Bulb {
  uint8_t mac[6];
  uint32_t ip;
  bool online;
  bool hearsBroadcast;
  bool answersGetService;
  bool onlyTagged;       // ignores un-tagged GetService
  bool onlyUntagged;     // ignores tagged GetService
  int dropAcks;          // next N acknowledgements are lost (the Set still takes effect)
  int dropSets;          // next N SetLightPower packets are lost completely
  int dropReplies;       // next N Get replies are lost
  int dropColors;        // next N SetColor packets are lost completely
  double levelFrom, levelTo;
  uint32_t fadeStart, fadeDur;
  std::vector<SetRecord> sets;          // SetLightPower (power) packets that arrived
  std::vector<ColorRecord> colors;      // SetColor packets that arrived
  uint16_t hue, saturation, brightness, kelvin;   // the colour the bulb shows now (power is separate)

  uint16_t level(uint32_t now) const {
    const uint32_t el = now - fadeStart;
    if (fadeDur == 0 || el >= fadeDur) return (uint16_t)levelTo;
    return (uint16_t)(levelFrom + (levelTo - levelFrom) * ((double)el / (double)fadeDur));
  }
};

static std::vector<Bulb> g_bulbs;
static bool g_broadcastWorks = true;
static int g_protocolViolations = 0;
static std::map<int, int> g_sentByType;        // how many packets of each LIFX type the board sent
static std::map<uint32_t, int> g_sentByIp;     // ... and how many to each IP address

struct ColorWire {     // one SetColor exactly as the board put it on the wire (whether or not a bulb heard it)
  uint32_t at;         // clock value when it was sent
  uint32_t ip;         // where it was sent
  uint16_t size;
  bool tagged;
  uint8_t flags, seq, reserved;
  uint16_t hue, saturation, brightness, kelvin;
  uint32_t duration;
};
static std::vector<ColorWire> g_colorWire;

struct Delivery {
  uint32_t at, from;
  std::vector<uint8_t> data;
};
static std::deque<Delivery> g_inbox;

static Bulb& addBulb(uint8_t macLast, int ipLast) {
  Bulb b = Bulb();   // all numbers start at zero / false
  const uint8_t m[6] = {0xd0, 0x73, 0xd5, 0xaa, 0xbb, macLast};
  memcpy(b.mac, m, 6);
  b.ip = ip4(192, 168, 1, ipLast);
  b.online = true;
  b.hearsBroadcast = true;
  b.answersGetService = true;
  g_bulbs.push_back(b);
  return g_bulbs.back();
}

static void queueReply(const Bulb& b, uint16_t type, const Header& req, const uint8_t* payload,
                       size_t plen) {
  uint8_t pkt[64];
  uint8_t target[8] = {b.mac[0], b.mac[1], b.mac[2], b.mac[3], b.mac[4], b.mac[5], 0, 0};
  size_t n = buildHeader(pkt, sizeof(pkt), type, plen, false, req.source, target, 0, req.sequence);
  if (plen) memcpy(pkt + n, payload, plen);
  Delivery d;
  d.at = millis() + 5;               // 5 ms network + bulb time
  d.from = b.ip;
  d.data.assign(pkt, pkt + n + plen);
  g_inbox.push_back(d);
}

static void bulbReceive(Bulb& b, uint32_t dest, const std::vector<uint8_t>& data) {
  Header h;
  if (!parseHeader(data.data(), data.size(), h)) return;
  const bool broadcast = (dest == subnetBroadcast() || dest == limitedBroadcast());
  if (!b.online) return;
  if (broadcast && !(g_broadcastWorks && b.hearsBroadcast)) return;
  if (!broadcast && dest != b.ip) return;
  if (!targetIsZero(h.target) && memcmp(h.target, b.mac, 6) != 0) return;
  const uint32_t now = millis();

  switch (h.type) {
    case kTypeGetService: {
      if (!b.answersGetService) return;
      if (b.onlyTagged && !h.tagged) return;
      if (b.onlyUntagged && h.tagged) return;
      const uint8_t payload[5] = {kServiceUdp, 0x7c, 0xdd, 0x00, 0x00};   // UDP, port 56700
      queueReply(b, kTypeStateService, h, payload, 5);
      break;
    }
    case kTypeGetLightPower:
    case kTypeGetPower: {
      if (b.dropReplies > 0) {
        --b.dropReplies;
        return;
      }
      const uint16_t lv = b.level(now);
      const uint8_t payload[2] = {(uint8_t)(lv & 0xFF), (uint8_t)(lv >> 8)};
      queueReply(b, h.type == kTypeGetLightPower ? kTypeStateLightPower : kTypeStatePower, h,
                 payload, 2);
      break;
    }
    case kTypeSetLightPower: {
      if (h.size < 42) return;
      if (b.dropSets > 0) {
        --b.dropSets;
        return;
      }
      const uint16_t lv = (uint16_t)(data[36] | (data[37] << 8));
      const uint32_t dur = (uint32_t)data[38] | ((uint32_t)data[39] << 8) |
                           ((uint32_t)data[40] << 16) | ((uint32_t)data[41] << 24);
      b.levelFrom = b.level(now);
      b.levelTo = lv;
      b.fadeStart = now;
      b.fadeDur = dur;
      SetRecord r;
      r.on = lv != 0;
      r.duration = dur;
      r.ack = (h.flags & kFlagAckRequired) != 0;
      r.targetMatches = !targetIsZero(h.target);
      r.seq = h.sequence;
      b.sets.push_back(r);
      if (r.ack) {
        if (b.dropAcks > 0) --b.dropAcks;
        else queueReply(b, kTypeAcknowledgement, h, nullptr, 0);
      }
      break;
    }
    case kTypeSetColor: {
      if (h.size < 49) return;
      if (b.dropColors > 0) {
        --b.dropColors;
        return;
      }
      ColorRecord r;
      r.hue = (uint16_t)(data[37] | (data[38] << 8));
      r.saturation = (uint16_t)(data[39] | (data[40] << 8));
      r.brightness = (uint16_t)(data[41] | (data[42] << 8));
      r.kelvin = (uint16_t)(data[43] | (data[44] << 8));
      r.duration = (uint32_t)data[45] | ((uint32_t)data[46] << 8) | ((uint32_t)data[47] << 16) |
                   ((uint32_t)data[48] << 24);
      r.targetMatches = !targetIsZero(h.target);
      r.at = now;
      b.colors.push_back(r);
      // Like a real bulb: the colour changes, but whether it is ON or OFF does not (levelFrom,
      // levelTo and the fade are deliberately left alone).
      b.hue = r.hue;
      b.saturation = r.saturation;
      b.brightness = r.brightness;
      b.kelvin = r.kelvin;
      if ((h.flags & kFlagAckRequired) != 0) queueReply(b, kTypeAcknowledgement, h, nullptr, 0);
      break;
    }
    default:
      break;
  }
}

void sim_udp_send(uint32_t ip, uint16_t port, const uint8_t* data, size_t len) {
  // Every packet the board sends must be a well-formed LIFX packet.
  Header h;
  if (port != kUdpPort || !parseHeader(data, len, h) || h.size != len || h.source < 2 ||
      !h.addressable || h.origin != 0 || h.protocol != 1024) {
    ++g_protocolViolations;
    printf("PROTOCOL VIOLATION: bad packet from the board (len %zu)\n", len);
    return;
  }
  bool reservedZero = true;
  for (size_t i = 16; i < 22; ++i) reservedZero = reservedZero && data[i] == 0;
  for (size_t i = 24; i < 32; ++i) reservedZero = reservedZero && data[i] == 0;
  reservedZero = reservedZero && data[34] == 0 && data[35] == 0 && (data[22] & ~0x03) == 0;
  if (!reservedZero) {
    ++g_protocolViolations;
    printf("PROTOCOL VIOLATION: reserved bytes not zero\n");
  }
  if (h.tagged && !(h.type == kTypeGetService && targetIsZero(h.target))) {
    ++g_protocolViolations;
    printf("PROTOCOL VIOLATION: tagged packet that is not a broadcast GetService\n");
  }
  if (h.type == kTypeSetColor) {
    // A colour packet must be exactly the 49 bytes the docs describe, untagged, asking for no
    // acknowledgement and no state reply, with the reserved payload byte zero.
    if (len != 49 || h.tagged || (h.flags & 0x03) != 0 || data[36] != 0) {
      ++g_protocolViolations;
      printf("PROTOCOL VIOLATION: malformed SetColor (len %zu, tagged %d, flags 0x%02x)\n", len,
             (int)h.tagged, (unsigned)h.flags);
    }
    if (len >= 49) {
      ColorWire w;
      w.at = millis();
      w.ip = ip;
      w.size = h.size;
      w.tagged = h.tagged;
      w.flags = h.flags;
      w.seq = h.sequence;
      w.reserved = data[36];
      w.hue = (uint16_t)(data[37] | (data[38] << 8));
      w.saturation = (uint16_t)(data[39] | (data[40] << 8));
      w.brightness = (uint16_t)(data[41] | (data[42] << 8));
      w.kelvin = (uint16_t)(data[43] | (data[44] << 8));
      w.duration = (uint32_t)data[45] | ((uint32_t)data[46] << 8) | ((uint32_t)data[47] << 16) |
                   ((uint32_t)data[48] << 24);
      g_colorWire.push_back(w);
    }
  }
  ++g_sentByType[h.type];
  ++g_sentByIp[ip];
  const std::vector<uint8_t> copy(data, data + len);
  for (size_t i = 0; i < g_bulbs.size(); ++i) bulbReceive(g_bulbs[i], ip, copy);
}

bool sim_udp_pop(uint32_t* fromIp, std::vector<uint8_t>* data) {
  if (g_inbox.empty() || (int32_t)(g_inbox.front().at - millis()) > 0) return false;
  *fromIp = g_inbox.front().from;
  *data = g_inbox.front().data;
  g_inbox.pop_front();
  return true;
}

// ===========================================================================
// Test helpers
// ===========================================================================
static int g_fails = 0, g_checks = 0;
static const char* g_scenario = "";

#define EXPECT(cond, ...)                                                        \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_fails;                                                                 \
      printf("FAIL  [%s] line %d: %s  --  ", g_scenario, __LINE__, #cond);       \
      printf(__VA_ARGS__);                                                       \
      printf("\n");                                                              \
    }                                                                            \
  } while (0)

struct Watch {
  bool active = false, sawDone = false, expectConfirm = true, wantOn = false;
  uint32_t seqAtTap = 0, startAt = 0, doneAt = 0;
} g_w;
static std::vector<NetStatus> g_statusHist;
static bool g_cpLast = false;                                  // last colorPending() value the monitor saw
static std::vector<std::pair<uint32_t, bool> > g_cpEdges;      // (clock, new value) each time it changed
static uint32_t g_startAt = 0;          // clock value when the board started
static bool g_seenOnline = false;
static uint32_t g_firstOnlineAt = 0;    // ms after start when status first became Online

static void monitor() {
  // Order matters (and is what the drawing code should do): read "pending" first, then the Reading.
  const bool pending = lifxnet::writePending();
  const lifxnet::Reading r = lifxnet::reading();
  const NetStatus s = lifxnet::status();
  const bool cp = lifxnet::colorPending();
  if (cp != g_cpLast) {
    g_cpLast = cp;
    g_cpEdges.push_back(std::make_pair(millis(), cp));
  }
  if (g_statusHist.empty() || g_statusHist.back() != s) {
    g_statusHist.push_back(s);
    if (s == NetStatus::Online && !g_seenOnline) {
      g_seenOnline = true;
      g_firstOnlineAt = millis() - g_startAt;
    }
  }
  if (g_w.active && !g_w.sawDone) {
    if (pending) {
      EXPECT(r.seq == g_w.seqAtTap, "Reading changed (seq %u -> %u) while a tap was pending",
             (unsigned)g_w.seqAtTap, (unsigned)r.seq);
    } else {
      g_w.sawDone = true;
      g_w.doneAt = millis();
      if (g_w.expectConfirm) {
        EXPECT(r.valid && r.on == g_w.wantOn && r.seq > g_w.seqAtTap,
               "pending ended but Reading is on=%d seq=%u (wanted on=%d, seq > %u)", (int)r.on,
               (unsigned)r.seq, (int)g_w.wantOn, (unsigned)g_w.seqAtTap);
      }
    }
  }
}

static void runMs(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    stepOnce();
    monitor();
  }
}

// Run until `cond` is true; returns ms waited, or -1 if it never became true.
template <typename F>
static int waitFor(F cond, uint32_t maxMs) {
  for (uint32_t t = 0; t <= maxMs; t += 10) {
    if (cond()) return (int)t;
    stepOnce();
    monitor();
  }
  return -1;
}

static void startBoard(uint32_t clockStart) {
  g_startAt = clockStart;
  g_now = clockStart;
  setLink(true);
  g_availSince = clockStart;
  lifxnet::begin();
}

static bool isOnline() { return lifxnet::status() == NetStatus::Online; }

static void tap(bool on, bool expectConfirm = true) {
  g_w.active = true;
  g_w.sawDone = false;
  g_w.expectConfirm = expectConfirm;
  g_w.wantOn = on;
  g_w.seqAtTap = lifxnet::reading().seq;
  g_w.startAt = millis();
  lifxnet::setDesired(on);
  EXPECT(lifxnet::writePending(), "writePending() must be true right after setDesired()");
}

static bool tapDone() { return g_w.sawDone; }

static void expectCleanProtocol() {
  EXPECT(g_protocolViolations == 0, "%d protocol violations", g_protocolViolations);
}

static void expectOnlineWithin(uint32_t ms) {
  const int t = waitFor(isOnline, ms);
  EXPECT(t >= 0, "never reached Online within %u ms", (unsigned)ms);
}

// ===========================================================================
// Scenarios
// ===========================================================================
static void sDiscover() {
  addBulb(0xcc, 42);
  runMs(12000);
  EXPECT(lifxnet::status() == NetStatus::Online, "status not Online");
  const lifxnet::Reading r = lifxnet::reading();
  EXPECT(r.valid && !r.on && r.seq >= 1, "reading valid=%d on=%d seq=%u", r.valid, r.on, r.seq);
  EXPECT(logHas("LIFX device d073d5aabbcc at 192.168.1.42"), "device line missing");
  EXPECT(logHas("Wi-Fi connected"), "no Wi-Fi line");
  // History: WifiDown (initial) -> SearchingBulb -> Online, nothing else (no flapping).
  EXPECT(g_statusHist.size() == 3 && g_statusHist[0] == NetStatus::WifiDown &&
             g_statusHist[1] == NetStatus::SearchingBulb && g_statusHist[2] == NetStatus::Online,
         "status history has %zu entries", g_statusHist.size());
  EXPECT(g_sentByType[kTypeGetService] > 0 && g_sentByType[kTypeGetLightPower] > 0, "no traffic");
  printf("      Online %u ms after start\n", (unsigned)g_firstOnlineAt);
  EXPECT(g_firstOnlineAt <= 4500, "took %u ms to go Online (expected about 3500)", (unsigned)g_firstOnlineAt);
  expectCleanProtocol();
}

static void sDiscoverTaggedOnly() {
  Bulb& b = addBulb(0xcc, 42);
  b.onlyTagged = true;
  expectOnlineWithin(15000);
  expectCleanProtocol();
}

static void sDiscoverUntaggedOnly() {
  Bulb& b = addBulb(0xcc, 42);
  b.onlyUntagged = true;
  expectOnlineWithin(15000);
  expectCleanProtocol();
}

static void sTwoBulbs() {
  addBulb(0xcc, 42);
  addBulb(0xdd, 43);
  expectOnlineWithin(15000);
  runMs(8000);
  EXPECT(logHas("LIFX device d073d5aabbcc at 192.168.1.42"), "first device not logged");
  EXPECT(logHas("LIFX device d073d5aabbdd at 192.168.1.43"), "second device not logged");
  EXPECT(g_sentByIp[ip4(192, 168, 1, 42)] > 2, "should poll the first bulb found");
  EXPECT(g_sentByIp[ip4(192, 168, 1, 43)] == 0, "must not talk to the second bulb");
}

static void sTap() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(2000);
  // Asking for what the bulb already is does nothing at all.
  lifxnet::setDesired(false);
  EXPECT(!lifxnet::writePending(), "no-op request should not be pending");
  runMs(1000);
  EXPECT(b.sets.empty(), "no-op request must not send anything");
  tap(true);
  const int t = waitFor(tapDone, 3000);
  EXPECT(t >= 0, "tap never confirmed");
  printf("      tap ON confirmed after %d ms\n", t);
  EXPECT(b.sets.size() == 1, "expected exactly 1 Set, got %zu", b.sets.size());
  if (!b.sets.empty()) {
    EXPECT(b.sets[0].on && b.sets[0].duration == 300 && b.sets[0].ack && b.sets[0].targetMatches,
           "Set contents wrong (on=%d dur=%u ack=%d tgt=%d)", b.sets[0].on, b.sets[0].duration,
           b.sets[0].ack, b.sets[0].targetMatches);
  }
  EXPECT(b.level(millis()) == 65535, "bulb not fully on");
  EXPECT(lifxnet::status() == NetStatus::Online, "status should stay Online during a tap");
  // Bulb is changed by someone else: the poll must show it (Reading follows reality again).
  b.levelFrom = b.levelTo = 0;
  b.fadeDur = 0;
  runMs(4000);
  EXPECT(!lifxnet::reading().on, "poll did not notice the bulb was switched off elsewhere");
  // The Reading now says OFF, so asking for OFF is a no-op again.
  const size_t setsAtResync = b.sets.size();
  lifxnet::setDesired(false);
  EXPECT(!lifxnet::writePending(), "OFF while already OFF should not be pending");
  runMs(1000);
  EXPECT(b.sets.size() == setsAtResync, "OFF while already OFF must not send anything");
  // Spam the same request: must not restart delivery or send piles of packets.
  const size_t setsBefore = b.sets.size();
  tap(true);
  for (int i = 0; i < 50; ++i) {
    lifxnet::setDesired(true);
    runMs(10);
  }
  EXPECT(waitFor(tapDone, 3000) >= 0, "tap ON (spam) never confirmed");
  EXPECT(b.sets.size() - setsBefore == 1, "spamming setDesired(true) caused %zu Sets",
         b.sets.size() - setsBefore);
  expectCleanProtocol();
}

// A poll answer that is already on its way when the user taps must not be shown as the truth.
static void sTapDuringPoll() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  const int polls = g_sentByType[kTypeGetLightPower];
  waitFor([polls] { return g_sentByType[kTypeGetLightPower] > polls; }, 4000);   // a poll was just sent
  tap(true);                                                                       // its (stale) answer is in flight
  EXPECT(waitFor(tapDone, 3000) >= 0, "tap never confirmed");
  EXPECT(b.level(millis()) == 65535, "bulb should be on");
  runMs(4000);
  EXPECT(lifxnet::reading().on, "Reading should show the bulb on");
}

static void sLostAcks() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  b.dropAcks = 2;
  tap(true);
  EXPECT(waitFor(tapDone, 5000) >= 0, "tap never confirmed");
  EXPECT(b.sets.size() == 3, "expected 3 sends (1 + 2 resends), got %zu", b.sets.size());
  EXPECT(b.sets.size() >= 2 && b.sets[0].seq == b.sets[1].seq, "resends must reuse the sequence");
  EXPECT(logHas("sending again"), "retry not logged");
}

static void sNoAcksAtAll() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  b.dropAcks = 1000;
  tap(true);
  EXPECT(waitFor(tapDone, 6000) >= 0, "tap never confirmed");
  EXPECT(b.sets.size() == 4, "expected 4 sends, got %zu", b.sets.size());
  EXPECT(b.level(millis()) == 65535, "bulb should be on");
  EXPECT(logHas("never acknowledged"), "give-up-on-ack not logged");
  EXPECT(lifxnet::status() == NetStatus::Online, "answers kept arriving, status must stay Online");
}

static void sLostSets() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  b.dropSets = 2;
  tap(true);
  EXPECT(waitFor(tapDone, 5000) >= 0, "tap never confirmed");
  EXPECT(b.sets.size() == 1, "only the 3rd send should arrive, got %zu", b.sets.size());
  EXPECT(b.level(millis()) == 65535, "bulb should be on");
}

static void sLostVerify() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  tap(true);
  runMs(100);
  b.dropReplies = 2;      // the first two "are you on?" answers after the fade are lost
  EXPECT(waitFor(tapDone, 6000) >= 0, "tap never confirmed");
  EXPECT(b.level(millis()) == 65535, "bulb should be on");
}

// The bulb ends up in a different state than asked (e.g. someone changed it at the same moment).
static void sMismatchRetry() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  tap(true);
  runMs(200);                       // first Set has been applied
  b.levelFrom = b.levelTo = 0;      // someone switches it off again behind our back
  b.fadeDur = 0;
  EXPECT(waitFor(tapDone, 6000) >= 0, "tap never confirmed");
  EXPECT(b.sets.size() >= 2, "should have sent again after the mismatch");
  EXPECT(b.level(millis()) == 65535, "bulb should end up on");
  EXPECT(logHas("sending again"), "mismatch retry not logged");
}

static void sSupersede() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  tap(true);
  runMs(100);
  tap(false);                       // the user changed their mind
  g_w.wantOn = false;
  EXPECT(waitFor(tapDone, 4000) >= 0, "newest tap never confirmed");
  EXPECT(!b.sets.empty() && !b.sets.back().on, "the LAST Set must be OFF");
  EXPECT(b.level(millis()) == 0, "bulb should be off");
  int ons = 0;
  for (size_t i = 0; i < b.sets.size(); ++i) ons += b.sets[i].on ? 1 : 0;
  EXPECT(ons <= 1, "old wish kept being sent (%d ON sets)", ons);
}

static void sSilentTimeout() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  const lifxnet::Reading before = lifxnet::reading();
  b.online = false;
  tap(true, /*expectConfirm=*/false);
  const int t = waitFor(tapDone, 30000);
  EXPECT(t >= 19900 && t <= 20300, "gave up after %d ms (want ~20000)", t);
  EXPECT(lifxnet::reading().seq == before.seq && !lifxnet::reading().on,
         "reading must not change when the tap is dropped");
  EXPECT(logHas("giving up"), "give-up not logged");
  EXPECT(lifxnet::status() == NetStatus::SearchingBulb, "status should be SearchingBulb");
  // The bulb comes back: we find it again, polling resyncs the Reading.
  b.online = true;
  b.levelFrom = b.levelTo = 65535;
  b.fadeDur = 0;
  expectOnlineWithin(12000);
  runMs(3500);
  EXPECT(lifxnet::reading().on && lifxnet::reading().seq > before.seq, "reading did not resync");
}

static void sSilentThenRecovers() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  b.online = false;
  tap(true);
  runMs(8000);
  EXPECT(lifxnet::writePending(), "tap should still be pending while the bulb is away");
  b.online = true;
  const int t = waitFor(tapDone, 12000);
  EXPECT(t >= 0, "tap was not delivered after the bulb came back");
  EXPECT(b.level(millis()) == 65535, "bulb should be on");
}

static void sIpChange() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(3000);
  b.ip = ip4(192, 168, 1, 60);      // DHCP gave it a new address
  const int lost = waitFor([] { return lifxnet::status() == NetStatus::SearchingBulb; }, 15000);
  EXPECT(lost >= 8500 && lost <= 12500, "Searching after %d ms (want 9-12 s: 3 missed polls)", lost);
  expectOnlineWithin(15000);
  EXPECT(logHas("stopped answering"), "re-search not logged");
  EXPECT(g_sentByIp[ip4(192, 168, 1, 60)] > 0, "never talked to the new address");
  runMs(10000);
  EXPECT(lifxnet::status() == NetStatus::Online, "should stay Online");
}

static void sWifiDrop() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  setLink(false);
  runMs(30);
  EXPECT(lifxnet::status() == NetStatus::WifiDown, "status should be WifiDown at once");
  runMs(2000);
  tap(true);                        // made while offline: stays queued
  runMs(8000);
  EXPECT(lifxnet::writePending(), "tap should wait for Wi-Fi");
  setLink(true);                    // router is back (reconnects ~1.5 s later)
  const int t = waitFor(tapDone, 15000);
  EXPECT(t >= 0, "tap never delivered after Wi-Fi came back");
  EXPECT(b.level(millis()) == 65535, "bulb should be on");
  EXPECT(logHas("Wi-Fi lost"), "drop not logged");
  expectOnlineWithin(15000);
}

static void sWifiLongOutage() {
  addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  setLink(false);
  runMs(500);
  tap(true, /*expectConfirm=*/false);
  const int t = waitFor(tapDone, 30000);
  EXPECT(t >= 19900 && t <= 20300, "gave up after %d ms (want ~20000)", t);
  runMs(30000);                      // the link stays down; nothing may crash or spam
  EXPECT(lifxnet::status() == NetStatus::WifiDown, "still WifiDown");
  setLink(true);
  expectOnlineWithin(20000);
}

static void sSoak() {
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  bool on = false;
  int confirmed = 0, taps = 0;
  for (int i = 0; i < 70; ++i) {           // ~29 simulated minutes
    runMs(25000);
    on = !on;
    tap(on);
    ++taps;
    const int t = waitFor(tapDone, 3000);
    if (t >= 0) ++confirmed;
  }
  EXPECT(confirmed == taps, "%d of %d taps confirmed", confirmed, taps);
  EXPECT(b.level(millis()) == (on ? 65535 : 0), "final bulb state wrong");
  EXPECT(g_statusHist.size() == 3, "status flapped: %zu changes", g_statusHist.size());
  EXPECT(!logHas("giving up") && !logHas("stopped answering") && !logHas("Wi-Fi lost"),
         "unexpected trouble in the log");
  expectCleanProtocol();
}

// ---- scenarios that need a different config.h (see build_lifx_sim.sh) ----
static void sFallback() {                  // LIFX_FALLBACK_IP "192.168.1.42", broadcasts blocked
  g_broadcastWorks = false;
  addBulb(0xcc, 42);
  expectOnlineWithin(15000);
  EXPECT(logHas("LIFX device d073d5aabbcc at 192.168.1.42"), "fixed-address bulb not reported");
  expectCleanProtocol();
}

static void sFallbackBlind() {             // bulb never answers GetService at all
  Bulb& b = addBulb(0xcc, 42);
  b.answersGetService = false;
  g_broadcastWorks = false;
  expectOnlineWithin(20000);
  EXPECT(logHas("LIFX_FALLBACK_IP"), "fallback adoption not logged");
  EXPECT(logHas("has id d073d5aabbcc"), "bulb id not learned from its first reply");
  EXPECT(g_firstOnlineAt >= 4500 + 2000, "adopted the fallback too early (%u)", (unsigned)g_firstOnlineAt);
}

static void sTarget() {                    // LIFX_TARGET_MAC "d073d5aabb02"
  addBulb(0x01, 42);
  addBulb(0x02, 43);
  expectOnlineWithin(15000);
  runMs(8000);
  EXPECT(logHas("LIFX device d073d5aabb01 at 192.168.1.42"), "device 1 not logged");
  EXPECT(logHas("LIFX device d073d5aabb02 at 192.168.1.43"), "device 2 not logged");
  EXPECT(g_sentByIp[ip4(192, 168, 1, 43)] > 2, "should poll the target bulb");
  EXPECT(g_sentByIp[ip4(192, 168, 1, 42)] == 0, "must not talk to the other bulb");
}

static void sTargetMissing() {
  addBulb(0x01, 42);
  runMs(30000);
  EXPECT(lifxnet::status() == NetStatus::SearchingBulb, "must keep searching");
  EXPECT(logHas("no device above has the id d073d5aabb02"), "warning missing");
  EXPECT(g_sentByType[kTypeGetLightPower] == 0, "must not poll a bulb that is not the target");
}

// ---- colour scenarios (setDesiredColor / SetColor) ----
// Rules being checked (the numbers match the list in lifx.h / lifx.cpp section 11):
//   1 SetColor never changes power and the colour code never sends SetLightPower
//   2 at most one SetColor per 100 ms, however often setDesiredColor() is called
//   3 the newest value is always the one sent, including the very last value of a drag
//   4 a value that stops changing is sent two more times (about 150 ms and 400 ms after its first send)
//   5 nothing is sent while Wi-Fi is down or the bulb is unknown (the value is kept), and nothing
//     is sent at all if setDesiredColor() was never called
//   6 colorPending() is true from the call until the last repeat has gone out
// Every scenario runs three times (see build_lifx_sim.sh) with the clock starting at 0, 5 s before
// millis() wraps, and 17 s before it wraps. alignToWrap() lines the interesting part of a scenario up
// with the wrap (when the clock is near one), so "100 ms since the last packet" is really checked
// across the roll-over from 4294967295 to 0.

static const uint32_t kBulbIp = (192u) | (168u << 8) | (1u << 16) | (42u << 24);   // 192.168.1.42 (see addBulb)

// Run until `msBefore` ms before millis() wraps (only if that is less than 30 s away).
static void alignToWrap(uint32_t msBefore) {
  const uint32_t toWrap = (uint32_t)(0u - g_now.load());
  if (toWrap > msBefore && toWrap <= 30000) runMs(((toWrap - msBefore) / 10) * 10);
}

static std::vector<ColorWire> colorSince(size_t base) {
  return std::vector<ColorWire>(g_colorWire.begin() + (long)base, g_colorWire.end());
}

// The parts of a SetColor that must be the same on every packet.
static void expectColorPacket(const ColorWire& w, uint16_t hue, uint16_t bright) {
  EXPECT(w.hue == hue && w.brightness == bright, "packet carries hue %u brightness %u (want %u / %u)",
         (unsigned)w.hue, (unsigned)w.brightness, (unsigned)hue, (unsigned)bright);
  EXPECT(w.saturation == 65535 && w.kelvin == 3500 && w.duration == 100,
         "saturation %u kelvin %u duration %u (want 65535 / 3500 / 100)", (unsigned)w.saturation,
         (unsigned)w.kelvin, (unsigned)w.duration);
  EXPECT(w.size == 49 && !w.tagged && w.flags == 0 && w.reserved == 0,
         "size %u tagged %d flags 0x%02x reserved %u", (unsigned)w.size, (int)w.tagged, (unsigned)w.flags,
         (unsigned)w.reserved);
  EXPECT(w.ip == kBulbIp, "colour packet went to ip 0x%08x, not straight to the bulb", (unsigned)w.ip);
}

static uint32_t gapMs(const ColorWire& later, const ColorWire& earlier) { return (uint32_t)(later.at - earlier.at); }

// When the clock starts near the roll-over (the 2nd and 3rd run of every scenario), the packets of a
// scenario must really be spread over the wrap: the last one then carries a SMALLER clock value than
// the first. If that is not so, the scenario has stopped testing what it claims to test.
static void expectStraddlesWrap(const std::vector<ColorWire>& w) {
  if (g_startAt == 0 || w.size() < 2) return;
  EXPECT(w.back().at < w.front().at, "the clock start is near the wrap but the packets never crossed it");
}

// Rule 1: nothing about power may have happened.
static void expectNoPowerTraffic(const Bulb& b) {
  EXPECT(g_sentByType[kTypeSetLightPower] == 0 && g_sentByType[kTypeSetPower] == 0,
         "a power message was sent (SetLightPower %d, SetPower %d)", g_sentByType[kTypeSetLightPower],
         g_sentByType[kTypeSetPower]);
  EXPECT(b.sets.empty(), "the bulb received %zu power Sets", b.sets.size());
}

// Rule 2 on a list of packets: any two are at least 100 ms apart, and no 1-second window holds more than 11.
static void expectRate(const std::vector<ColorWire>& w) {
  uint32_t minGap = 100000;
  size_t worst = 0;
  for (size_t i = 0; i < w.size(); ++i) {
    if (i > 0 && gapMs(w[i], w[i - 1]) < minGap) minGap = gapMs(w[i], w[i - 1]);
    size_t inWindow = 0;
    for (size_t j = i; j < w.size() && gapMs(w[j], w[i]) < 1000; ++j) ++inWindow;
    if (inWindow > worst) worst = inWindow;
  }
  EXPECT(w.size() < 2 || minGap >= 100, "two colour packets only %u ms apart (limit: 100)", (unsigned)minGap);
  EXPECT(worst <= 11, "%zu colour packets inside one second (limit: 11)", worst);
}

static void sColorSingle() {                // rules 1, 4, and 6 (start)
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(350);                         // the clock wraps between the 2nd and 3rd packet (250 ms after the 1st is when the 3rd could first go out)
  const size_t base = g_colorWire.size();
  const uint32_t t0 = millis();
  lifxnet::setDesiredColor(21845, 40000);
  EXPECT(lifxnet::colorPending(), "colorPending() must be true right after setDesiredColor()");
  runMs(1000);
  const std::vector<ColorWire> w = colorSince(base);
  EXPECT(w.size() == 3, "one call must give exactly 3 packets, got %zu", w.size());
  if (w.size() == 3) {
    for (size_t i = 0; i < 3; ++i) expectColorPacket(w[i], 21845, 40000);
    EXPECT((uint32_t)(w[0].at - t0) <= 20, "first packet %u ms after the call", (unsigned)(w[0].at - t0));
    EXPECT(gapMs(w[1], w[0]) >= 140 && gapMs(w[1], w[0]) <= 160, "2nd packet %u ms after the 1st (want ~150)",
           (unsigned)gapMs(w[1], w[0]));
    EXPECT(gapMs(w[2], w[0]) >= 390 && gapMs(w[2], w[0]) <= 410, "3rd packet %u ms after the 1st (want ~400)",
           (unsigned)gapMs(w[2], w[0]));
    EXPECT(w[0].seq != w[1].seq && w[1].seq != w[2].seq, "colour packets must not reuse a sequence number");
  }
  EXPECT(!lifxnet::colorPending(), "colorPending() still true after the last repeat");
  EXPECT(b.colors.size() == 3, "bulb heard %zu packets", b.colors.size());
  if (!b.colors.empty()) EXPECT(b.colors[0].targetMatches, "colour packet must carry the bulb's id");
  EXPECT(b.hue == 21845 && b.saturation == 65535 && b.brightness == 40000 && b.kelvin == 3500,
         "bulb shows hue %u sat %u bri %u kelvin %u", (unsigned)b.hue, (unsigned)b.saturation,
         (unsigned)b.brightness, (unsigned)b.kelvin);
  expectNoPowerTraffic(b);
  EXPECT(b.level(millis()) == 0 && !lifxnet::reading().on, "the bulb was off and must still be off");
  runMs(3000);                              // no further packets, ever, once the value is settled
  EXPECT(g_colorWire.size() - base == 3, "extra packets after the repeats: %zu", g_colorWire.size() - base);
  expectStraddlesWrap(w);
  expectCleanProtocol();
}

static void sColorDrag() {                  // rules 2, 3, 4, 6, with a 3 second drag
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(1500);                        // the clock wraps in the middle of the drag
  const size_t base = g_colorWire.size();
  const size_t edgesBase = g_cpEdges.size();
  uint16_t lastHue = 0, lastBri = 0;
  for (int i = 0; i < 300; ++i) {           // 3 s, one call every 10 ms, like a finger on a slider
    lastHue = (uint16_t)(1000 + i * 211);
    lastBri = (uint16_t)(65535 - i * 150);
    lifxnet::setDesiredColor(lastHue, lastBri);
    runMs(10);
  }
  EXPECT(lifxnet::colorPending(), "must still be pending right after the last call of the drag");
  runMs(2000);
  const std::vector<ColorWire> w = colorSince(base);
  printf("      %zu colour packets for a 3 s drag with 300 calls\n", w.size());
  EXPECT(w.size() >= 28 && w.size() <= 34, "expected about 30-32 packets for a 3 s drag, got %zu", w.size());
  expectRate(w);
  expectStraddlesWrap(w);
  for (size_t i = 0; i < w.size(); ++i) {
    EXPECT(w[i].saturation == 65535 && w[i].kelvin == 3500 && w[i].duration == 100 && w[i].flags == 0,
           "packet %zu has wrong fixed fields", i);
  }
  if (w.size() >= 4) {
    const ColorWire& last = w[w.size() - 1];
    EXPECT(last.hue == lastHue && last.brightness == lastBri, "LAST packet on the wire has hue %u bri %u (want %u / %u)",
           (unsigned)last.hue, (unsigned)last.brightness, (unsigned)lastHue, (unsigned)lastBri);
    // the final value is repeated twice, and the value before it is a different one
    const ColorWire& l2 = w[w.size() - 2];
    const ColorWire& l3 = w[w.size() - 3];
    const ColorWire& l4 = w[w.size() - 4];
    EXPECT(l2.hue == lastHue && l3.hue == lastHue && l2.brightness == lastBri && l3.brightness == lastBri,
           "the final value must be sent 3 times in a row");
    EXPECT(l4.hue != lastHue, "the final value was sent more than 3 times");
    EXPECT(gapMs(l2, l3) >= 140 && gapMs(l2, l3) <= 160 && gapMs(last, l3) >= 390 && gapMs(last, l3) <= 410,
           "repeats of the final value came %u and %u ms after its first send (want ~150 and ~400)",
           (unsigned)gapMs(l2, l3), (unsigned)gapMs(last, l3));
    EXPECT(b.hue == lastHue && b.brightness == lastBri, "the bulb ended on hue %u bri %u", (unsigned)b.hue,
           (unsigned)b.brightness);
  }
  EXPECT(!lifxnet::colorPending(), "still pending at the end");
  EXPECT(g_cpEdges.size() == edgesBase + 2 && g_cpEdges.size() >= 2 && g_cpEdges[edgesBase].second &&
             !g_cpEdges[edgesBase + 1].second,
         "colorPending() should go true once and false once, but changed %zu times",
         g_cpEdges.size() - edgesBase);
  expectNoPowerTraffic(b);
  expectCleanProtocol();
}

static void sColorLastValue() {             // rule 3: latest wins, even when set at the very last moment before a send
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(250);
  const size_t base = g_colorWire.size();
  lifxnet::setDesiredColor(1000, 2000);                  // A: sent at once
  EXPECT(waitFor([base] { return g_colorWire.size() > base; }, 200) >= 0, "A was never sent");
  const uint32_t tA = millis();
  lifxnet::setDesiredColor(3000, 4000);                  // B: replaced before it can be sent
  runMs(30);
  lifxnet::setDesiredColor(5000, 6000);                  // C: replaced as well
  runMs(60);                                             // now 90 ms after A: the next send is on the very next tick
  lifxnet::setDesiredColor(7000, 8000);                  // D: set at the last moment, goes out at +100 ms
  runMs(10);
  lifxnet::setDesiredColor(9000, 10000);                 // E: set right AFTER D went out, must wait for +200 ms
  runMs(10);
  lifxnet::setDesiredColor(11000, 12000);                // F: replaces E
  runMs(1500);
  const std::vector<ColorWire> w = colorSince(base);
  static const uint16_t wantHue[] = {1000, 7000, 11000, 11000, 11000};
  static const uint16_t wantBri[] = {2000, 8000, 12000, 12000, 12000};
  static const uint32_t wantAt[] = {0, 100, 200, 350, 600};    // ms after A
  EXPECT(w.size() == 5, "expected A, D, F, F, F on the wire, got %zu packets", w.size());
  for (size_t i = 0; i < w.size() && i < 5; ++i) {
    EXPECT(w[i].hue == wantHue[i] && w[i].brightness == wantBri[i], "packet %zu: hue %u bri %u (want %u / %u)", i,
           (unsigned)w[i].hue, (unsigned)w[i].brightness, (unsigned)wantHue[i], (unsigned)wantBri[i]);
    const uint32_t at = (uint32_t)(w[i].at - tA);
    EXPECT(at + 10 >= wantAt[i] && at <= wantAt[i] + 10, "packet %zu went out %u ms after A (want ~%u)", i, (unsigned)at,
           (unsigned)wantAt[i]);
  }
  expectRate(w);
  expectStraddlesWrap(w);
  EXPECT(b.hue == 11000 && b.brightness == 12000, "the bulb ended on hue %u bri %u", (unsigned)b.hue, (unsigned)b.brightness);
  expectNoPowerTraffic(b);
  expectCleanProtocol();
}

static void sColorRestart() {               // rule 4: a new value restarts the repeats and the old value stops being repeated
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(300);
  const size_t base = g_colorWire.size();
  lifxnet::setDesiredColor(1000, 60000);                 // A
  EXPECT(waitFor([base] { return g_colorWire.size() > base; }, 200) >= 0, "A was never sent");
  const uint32_t tA = millis();
  runMs(150);                                            // A's first repeat goes out at +150
  lifxnet::setDesiredColor(2000, 30000);                 // B arrives straight after it
  runMs(1500);
  const std::vector<ColorWire> w = colorSince(base);
  static const uint16_t wantHue[] = {1000, 1000, 2000, 2000, 2000};
  static const uint32_t wantAt[] = {0, 150, 250, 400, 650};    // B is held back 100 ms after A's repeat, then repeats at +150 / +400
  EXPECT(w.size() == 5, "expected A, A, B, B, B (A's 3rd send must be cancelled), got %zu packets", w.size());
  for (size_t i = 0; i < w.size() && i < 5; ++i) {
    EXPECT(w[i].hue == wantHue[i], "packet %zu has hue %u (want %u)", i, (unsigned)w[i].hue, (unsigned)wantHue[i]);
    const uint32_t at = (uint32_t)(w[i].at - tA);
    EXPECT(at + 10 >= wantAt[i] && at <= wantAt[i] + 10, "packet %zu went out %u ms after A (want ~%u)", i, (unsigned)at,
           (unsigned)wantAt[i]);
  }
  expectRate(w);
  expectStraddlesWrap(w);
  EXPECT(b.hue == 2000 && b.brightness == 30000, "the bulb ended on hue %u bri %u", (unsigned)b.hue, (unsigned)b.brightness);
  expectNoPowerTraffic(b);
}

static void sColorSameValue() {             // calling again with the same value is free; hue 0 / brightness 0 counts
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(300);
  const size_t base = g_colorWire.size();
  for (int i = 0; i < 200; ++i) {                        // 2 s of the very same value, first value ever is (0, 0)
    lifxnet::setDesiredColor(0, 0);
    runMs(10);
  }
  EXPECT(g_colorWire.size() - base == 3, "the very first call (0, 0) must produce exactly 3 packets, got %zu",
         g_colorWire.size() - base);
  EXPECT(!lifxnet::colorPending(), "calling again with the same value must not keep it pending");
  const size_t afterFirst = g_colorWire.size();
  lifxnet::setDesiredColor(0, 0);                        // settled value again: nothing at all
  EXPECT(!lifxnet::colorPending(), "same value after it settled must not become pending");
  runMs(1000);
  EXPECT(g_colorWire.size() == afterFirst, "same value after it settled sent %zu extra packets",
         g_colorWire.size() - afterFirst);

  // A -> B -> A inside one 10 ms step: the bulb already has A, so nothing may be sent, and pending must not get stuck.
  lifxnet::setDesiredColor(500, 500);
  runMs(1000);                                           // (500, 500) settles: 3 more packets
  EXPECT(g_colorWire.size() == afterFirst + 3, "(500, 500) gave %zu packets", g_colorWire.size() - afterFirst);
  const size_t settled = g_colorWire.size();
  lifxnet::setDesiredColor(900, 900);
  lifxnet::setDesiredColor(500, 500);
  EXPECT(lifxnet::colorPending(), "a change was recorded, so it is pending until the task has looked at it");
  runMs(1000);
  EXPECT(!lifxnet::colorPending(), "pending got stuck after A -> B -> A");
  EXPECT(g_colorWire.size() == settled, "A -> B -> A before the task looked sent %zu packets", g_colorWire.size() - settled);
  EXPECT(b.hue == 500 && b.brightness == 500, "bulb should still show (500, 500)");
  expectStraddlesWrap(g_colorWire);
  expectNoPowerTraffic(b);
  expectCleanProtocol();
}

static void sColorPendingLifecycle() {      // rule 6
  addBulb(0xcc, 42);
  EXPECT(!lifxnet::colorPending(), "colorPending() must be false before anything was asked");
  expectOnlineWithin(12000);
  runMs(500);
  EXPECT(!lifxnet::colorPending(), "colorPending() must stay false when setDesiredColor() was never called");
  alignToWrap(200);
  const size_t base = g_colorWire.size();
  const size_t edgesBase = g_cpEdges.size();
  lifxnet::setDesiredColor(40000, 65535);
  EXPECT(lifxnet::colorPending(), "true right after the call");
  runMs(1000);
  const std::vector<ColorWire> w = colorSince(base);
  EXPECT(w.size() == 3, "expected 3 packets, got %zu", w.size());
  EXPECT(g_cpEdges.size() == edgesBase + 2 && g_cpEdges[edgesBase].second && !g_cpEdges[edgesBase + 1].second,
         "colorPending() should go true, then false, once (changed %zu times)", g_cpEdges.size() - edgesBase);
  if (w.size() == 3 && g_cpEdges.size() >= edgesBase + 2) {
    const uint32_t fell = g_cpEdges[edgesBase + 1].first;
    EXPECT((uint32_t)(fell - w[2].at) <= 10, "pending ended %d ms after the last repeat went out (want 0..10)",
           (int)(uint32_t)(fell - w[2].at));
    EXPECT((int32_t)(fell - w[2].at) >= 0, "pending ended BEFORE the last repeat went out");
  }
  expectStraddlesWrap(w);
}

static void sColorNeverCalled() {           // rule 5: no colour traffic unless setDesiredColor() was called
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(20000);
  EXPECT(g_sentByType[kTypeSetColor] == 0 && g_colorWire.empty(), "%d colour packets were sent without any call",
         g_sentByType[kTypeSetColor]);
  EXPECT(!lifxnet::colorPending(), "colorPending() must be false");
  expectNoPowerTraffic(b);
  // Power taps must not produce colour packets either.
  tap(true);
  EXPECT(waitFor(tapDone, 3000) >= 0, "tap never confirmed");
  runMs(2000);
  EXPECT(g_sentByType[kTypeSetColor] == 0, "a power tap produced colour packets");
}

static void sColorBeforeBulbFound() {       // rule 5: set before Wi-Fi/bulb exist, sent after discovery
  Bulb& b = addBulb(0xcc, 42);
  b.online = false;                         // the bulb cannot be found yet
  lifxnet::setDesiredColor(30000, 50000);   // at the very start: Wi-Fi is not even joined
  EXPECT(lifxnet::colorPending(), "must be pending");
  runMs(1000);
  EXPECT(g_sentByType[kTypeSetColor] == 0, "a colour was sent while Wi-Fi was still joining");
  runMs(5000);                              // Wi-Fi is up (after 2 s) and searching, but there is no bulb
  EXPECT(lifxnet::status() == NetStatus::SearchingBulb, "board should be searching for the bulb");
  EXPECT(g_sentByType[kTypeGetService] > 0, "board should be shouting 'who is there?'");
  EXPECT(g_sentByType[kTypeSetColor] == 0, "%d colour packets were sent before any bulb was known",
         g_sentByType[kTypeSetColor]);
  EXPECT(lifxnet::colorPending(), "must stay pending while there is no bulb");
  lifxnet::setDesiredColor(31000, 51000);   // changes its mind while waiting: the newest value is the one kept
  runMs(500);
  EXPECT(g_sentByType[kTypeSetColor] == 0, "still no bulb, still no colour");
  b.online = true;                          // the bulb appears
  const uint32_t tOnline = millis();
  EXPECT(waitFor([] { return !lifxnet::colorPending(); }, 15000) >= 0, "colour was never sent once the bulb was found");
  const std::vector<ColorWire> w = colorSince(0);
  EXPECT(w.size() == 3, "expected 3 packets, got %zu", w.size());
  for (size_t i = 0; i < w.size() && i < 3; ++i) {
    expectColorPacket(w[i], 31000, 51000);
    EXPECT((int32_t)(w[i].at - tOnline) > 0, "packet %zu was sent before the bulb was there", i);
  }
  EXPECT(b.hue == 31000 && b.brightness == 51000, "bulb ended on hue %u bri %u", (unsigned)b.hue, (unsigned)b.brightness);
  expectNoPowerTraffic(b);
  expectCleanProtocol();
}

static void sColorWifiDown() {              // rule 5 and 6: nothing sent while Wi-Fi is down, newest value kept
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  setLink(false);
  runMs(30);
  EXPECT(lifxnet::status() == NetStatus::WifiDown, "status should be WifiDown at once");
  lifxnet::setDesiredColor(10000, 20000);   // made while offline
  runMs(300);
  lifxnet::setDesiredColor(12000, 22000);   // and changed while offline: this is the one that must arrive
  runMs(5000);
  EXPECT(g_sentByType[kTypeSetColor] == 0, "%d colour packets were sent with Wi-Fi down", g_sentByType[kTypeSetColor]);
  EXPECT(lifxnet::colorPending(), "must stay pending while Wi-Fi is down");
  setLink(true);
  EXPECT(waitFor([] { return !lifxnet::colorPending(); }, 20000) >= 0, "colour never sent after Wi-Fi came back");
  runMs(1000);
  const std::vector<ColorWire> w = colorSince(0);
  EXPECT(w.size() == 3, "expected 3 packets of the newest value, got %zu", w.size());
  for (size_t i = 0; i < w.size() && i < 3; ++i) expectColorPacket(w[i], 12000, 22000);
  EXPECT(b.hue == 12000 && b.brightness == 22000, "bulb ended on hue %u bri %u", (unsigned)b.hue, (unsigned)b.brightness);
  expectOnlineWithin(15000);

  // Part 2: Wi-Fi drops AFTER the first send, before the repeats. They must wait, and still go out afterwards.
  const size_t base = g_colorWire.size();
  lifxnet::setDesiredColor(40000, 41000);
  EXPECT(waitFor([base] { return g_colorWire.size() > base; }, 200) >= 0, "first packet of part 2 never sent");
  setLink(false);
  runMs(4000);
  EXPECT(g_colorWire.size() - base == 1, "%zu packets while Wi-Fi was down after the first send", g_colorWire.size() - base);
  EXPECT(lifxnet::colorPending(), "repeats are still owed, so it must be pending");
  setLink(true);
  EXPECT(waitFor([] { return !lifxnet::colorPending(); }, 20000) >= 0, "repeats never sent after Wi-Fi came back");
  const std::vector<ColorWire> w2 = colorSince(base);
  EXPECT(w2.size() == 3, "expected 3 packets in total for part 2, got %zu", w2.size());
  for (size_t i = 0; i < w2.size() && i < 3; ++i) expectColorPacket(w2[i], 40000, 41000);
  expectRate(w2);
  expectNoPowerTraffic(b);
  expectCleanProtocol();
}

static void sColorNoPower() {               // rule 1: colours never switch the bulb on or off
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(500);
  for (int i = 0; i < 100; ++i) {           // a 1 s drag while the bulb is OFF
    lifxnet::setDesiredColor((uint16_t)(2000 + i * 300), (uint16_t)(65535 - i * 400));
    runMs(10);
  }
  runMs(1500);
  EXPECT(g_colorWire.size() >= 10, "the drag sent only %zu packets", g_colorWire.size());
  EXPECT(b.level(millis()) == 0, "an OFF bulb turned on (level %u)", (unsigned)b.level(millis()));
  EXPECT(!lifxnet::reading().on && !lifxnet::writePending(), "Reading/writePending changed by colour traffic");
  expectNoPowerTraffic(b);
  EXPECT(b.brightness == (uint16_t)(65535 - 99 * 400) && b.hue == (uint16_t)(2000 + 99 * 300), "bulb colour not the last value");

  // Now really turn it on with a tap: exactly one power Set. Colours afterwards must not turn it off.
  tap(true);
  EXPECT(waitFor(tapDone, 3000) >= 0, "tap never confirmed");
  EXPECT(g_sentByType[kTypeSetLightPower] == 1 && b.sets.size() == 1, "expected exactly 1 SetLightPower, got %d",
         g_sentByType[kTypeSetLightPower]);
  for (int i = 0; i < 100; ++i) {
    lifxnet::setDesiredColor((uint16_t)(40000 - i * 100), (uint16_t)(1000 + i * 500));
    runMs(10);
  }
  runMs(1500);
  EXPECT(b.level(millis()) == 65535, "an ON bulb changed power (level %u)", (unsigned)b.level(millis()));
  EXPECT(g_sentByType[kTypeSetLightPower] == 1 && b.sets.size() == 1, "colour traffic added power Sets (%d)",
         g_sentByType[kTypeSetLightPower]);
  EXPECT(b.hue == (uint16_t)(40000 - 99 * 100), "bulb colour not the last value");
  EXPECT(lifxnet::reading().on, "Reading should still say ON");
  expectStraddlesWrap(g_colorWire);
  expectCleanProtocol();
}

static void sColorWithTap() {               // a tap in the middle of a drag is still delivered and confirmed
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(600);
  bool tapped = false;
  for (int i = 0; i < 150; ++i) {
    lifxnet::setDesiredColor((uint16_t)(5000 + i * 100), (uint16_t)(60000 - i * 100));
    if (i == 40) {
      tap(true);
      tapped = true;
    }
    runMs(10);
  }
  EXPECT(tapped, "test bug: no tap made");
  EXPECT(waitFor(tapDone, 3000) >= 0, "tap never confirmed while colours were being sent");
  runMs(1500);
  EXPECT(b.sets.size() == 1 && b.sets[0].on, "expected exactly one ON Set, got %zu Sets", b.sets.size());
  EXPECT(b.level(millis()) == 65535, "bulb should be on");
  EXPECT(lifxnet::reading().on, "Reading should say ON");
  EXPECT(b.hue == (uint16_t)(5000 + 149 * 100) && b.brightness == (uint16_t)(60000 - 149 * 100),
         "bulb colour is not the last value of the drag");
  EXPECT(!lifxnet::colorPending() && !lifxnet::writePending(), "something is still pending");
  expectRate(g_colorWire);
  expectStraddlesWrap(g_colorWire);
  expectCleanProtocol();
}

static void sColorLossCover() {             // rule 4: the repeats are there so a lost UDP packet does not matter
  Bulb& b = addBulb(0xcc, 42);
  expectOnlineWithin(12000);
  runMs(1000);
  alignToWrap(100);
  b.dropColors = 1;                         // the first packet is lost
  lifxnet::setDesiredColor(15000, 25000);
  runMs(1000);
  EXPECT(g_colorWire.size() == 3, "board should still send 3 packets, sent %zu", g_colorWire.size());
  EXPECT(b.colors.size() == 2, "bulb should hear the 2 repeats, heard %zu", b.colors.size());
  EXPECT(b.hue == 15000 && b.brightness == 25000, "bulb missed the colour although 2 repeats arrived");
  b.dropColors = 2;                         // the first two are lost: only the last repeat gets through
  lifxnet::setDesiredColor(16000, 26000);
  runMs(1000);
  EXPECT(g_colorWire.size() == 6, "board should have sent 6 packets in all, sent %zu", g_colorWire.size());
  EXPECT(b.hue == 16000 && b.brightness == 26000, "bulb missed the colour although the 3rd packet arrived");
  EXPECT(!lifxnet::colorPending(), "still pending");
  expectStraddlesWrap(g_colorWire);
  expectNoPowerTraffic(b);
  expectCleanProtocol();
}

struct Scenario {
  const char* name;
  void (*fn)();
};
static const Scenario kScenarios[] = {
    {"discover", sDiscover},
    {"discover_tagged_only", sDiscoverTaggedOnly},
    {"discover_untagged_only", sDiscoverUntaggedOnly},
    {"two_bulbs", sTwoBulbs},
    {"tap", sTap},
    {"tap_during_poll", sTapDuringPoll},
    {"lost_acks", sLostAcks},
    {"no_acks_at_all", sNoAcksAtAll},
    {"lost_sets", sLostSets},
    {"lost_verify", sLostVerify},
    {"mismatch_retry", sMismatchRetry},
    {"supersede", sSupersede},
    {"silent_timeout", sSilentTimeout},
    {"silent_then_recovers", sSilentThenRecovers},
    {"ip_change", sIpChange},
    {"wifi_drop", sWifiDrop},
    {"wifi_long_outage", sWifiLongOutage},
    {"soak", sSoak},
    {"fallback", sFallback},
    {"fallback_blind", sFallbackBlind},
    {"target", sTarget},
    {"target_missing", sTargetMissing},
    {"color_single", sColorSingle},
    {"color_drag", sColorDrag},
    {"color_last_value", sColorLastValue},
    {"color_restart", sColorRestart},
    {"color_same_value", sColorSameValue},
    {"color_pending_lifecycle", sColorPendingLifecycle},
    {"color_never_called", sColorNeverCalled},
    {"color_before_bulb_found", sColorBeforeBulbFound},
    {"color_wifi_down", sColorWifiDown},
    {"color_no_power", sColorNoPower},
    {"color_with_tap", sColorWithTap},
    {"color_loss_cover", sColorLossCover},
};

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: sim_lifx <scenario|list> [clock-start-ms]\n");
    return 2;
  }
  if (strcmp(argv[1], "list") == 0) {
    for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); ++i) printf("%s\n", kScenarios[i].name);
    return 0;
  }
  const uint32_t clockStart = argc > 2 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
  g_verbose = getenv("SIM_VERBOSE") != nullptr;
  for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); ++i) {
    if (strcmp(argv[1], kScenarios[i].name) != 0) continue;
    g_scenario = kScenarios[i].name;
    startBoard(clockStart);
    kScenarios[i].fn();
    if (g_fails == 0) {
      printf("PASS  %s (clock start %u, %d checks)\n", g_scenario, (unsigned)clockStart, g_checks);
    } else {
      printf("FAIL  %s: %d of %d checks failed\n", g_scenario, g_fails, g_checks);
      for (size_t k = g_log.size() > 25 ? g_log.size() - 25 : 0; k < g_log.size(); ++k) {
        printf("   log| %s", g_log[k].c_str());
      }
    }
    fflush(stdout);
    _Exit(g_fails == 0 ? 0 : 1);   // the task thread is still looping; just leave
  }
  printf("unknown scenario %s\n", argv[1]);
  return 2;
}
