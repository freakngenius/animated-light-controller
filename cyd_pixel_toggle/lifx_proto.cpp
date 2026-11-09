// lifx_proto.cpp - see lifx_proto.h. Pure C++11, no Arduino code, so it also builds on a laptop.
#include "lifx_proto.h"
#include <string.h>

namespace lifxproto {

namespace {

// --- tiny little-endian read/write helpers (lowest byte first) ---------------
inline void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}
inline uint16_t get16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Bit positions inside the 16-bit "frame" field (bytes 2-3).
constexpr uint16_t kProtocolNumber = 1024;           // bits 0-11
constexpr uint16_t kAddressableBit = 1u << 12;
constexpr uint16_t kTaggedBit      = 1u << 13;
// origin = bits 14-15 (must be 0 when we send)

constexpr size_t kOffSize     = 0;
constexpr size_t kOffFrame    = 2;
constexpr size_t kOffSource   = 4;
constexpr size_t kOffTarget   = 8;
constexpr size_t kOffFlags    = 22;
constexpr size_t kOffSequence = 23;
constexpr size_t kOffType     = 32;

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------
size_t buildHeader(uint8_t* out, size_t cap, uint16_t type, size_t payloadLen, bool tagged,
                   uint32_t source, const uint8_t* target, uint8_t flags, uint8_t sequence) {
  const size_t total = kHeaderSize + payloadLen;
  if (out == nullptr || cap < kHeaderSize || total > 0xFFFF) return 0;

  memset(out, 0, kHeaderSize);                       // every "reserved" field stays zero
  put16(out + kOffSize, (uint16_t)total);
  uint16_t frame = (uint16_t)(kProtocolNumber | kAddressableBit | (tagged ? kTaggedBit : 0));
  put16(out + kOffFrame, frame);                     // origin bits stay 0
  put32(out + kOffSource, source);
  if (target != nullptr) memcpy(out + kOffTarget, target, 8);
  out[kOffFlags] = (uint8_t)(flags & (kFlagResRequired | kFlagAckRequired));
  out[kOffSequence] = sequence;
  put16(out + kOffType, type);
  return kHeaderSize;
}

size_t buildGetService(uint8_t* out, size_t cap, uint32_t source, uint8_t sequence, bool tagged) {
  // Target stays all zeros = "any device". No payload.
  return buildHeader(out, cap, kTypeGetService, 0, tagged, source, nullptr, 0, sequence);
}

size_t buildGet(uint8_t* out, size_t cap, uint16_t type, uint32_t source, const uint8_t* target,
                uint8_t sequence) {
  return buildHeader(out, cap, type, 0, false, source, target, 0, sequence);
}

size_t buildSetLightPower(uint8_t* out, size_t cap, uint32_t source, const uint8_t* target,
                          uint8_t sequence, bool on, uint32_t durationMs, bool ackRequired) {
  const size_t payloadLen = 2 + 4;                   // level (u16) + duration (u32)
  if (out == nullptr || cap < kHeaderSize + payloadLen) return 0;
  uint8_t flags = ackRequired ? kFlagAckRequired : 0;   // docs: Set -> ack_required=1, res_required=0
  if (buildHeader(out, cap, kTypeSetLightPower, payloadLen, false, source, target, flags,
                  sequence) == 0) {
    return 0;
  }
  put16(out + kHeaderSize, on ? kPowerOn : kPowerOff);
  put32(out + kHeaderSize + 2, durationMs);
  return kHeaderSize + payloadLen;
}

size_t buildSetColor(uint8_t* out, size_t cap, uint32_t source, const uint8_t* target,
                     uint8_t sequence, uint16_t hue, uint16_t saturation, uint16_t brightness,
                     uint16_t kelvin, uint32_t durationMs, bool ackRequired) {
  const size_t payloadLen = 1 + 2 + 2 + 2 + 2 + 4;   // reserved + hue + sat + brightness + kelvin + duration
  if (out == nullptr || cap < kHeaderSize + payloadLen) return 0;
  uint8_t flags = ackRequired ? kFlagAckRequired : 0;   // res_required stays 0: we never ask for a State back
  if (buildHeader(out, cap, kTypeSetColor, payloadLen, false, source, target, flags, sequence) == 0) {
    return 0;
  }
  uint8_t* p = out + kHeaderSize;
  p[0] = 0;                                          // the one reserved byte
  put16(p + 1, hue);
  put16(p + 3, saturation);
  put16(p + 5, brightness);
  put16(p + 7, kelvin);
  put32(p + 9, durationMs);
  return kHeaderSize + payloadLen;
}

// ---------------------------------------------------------------------------
// Parsing (always check the length before touching a byte)
// ---------------------------------------------------------------------------
bool parseHeader(const uint8_t* buf, size_t len, Header& out) {
  if (buf == nullptr || len < kHeaderSize) return false;

  const uint16_t size = get16(buf + kOffSize);
  if (size < kHeaderSize || size > len) return false;     // lying or cut-off packet

  const uint16_t frame = get16(buf + kOffFrame);
  if ((frame & 0x0FFF) != kProtocolNumber) return false;   // not a LIFX packet

  out.size        = size;
  out.protocol    = (uint16_t)(frame & 0x0FFF);
  out.addressable = (frame & kAddressableBit) != 0;
  out.tagged      = (frame & kTaggedBit) != 0;
  out.origin      = (uint8_t)(frame >> 14);
  out.source      = get32(buf + kOffSource);
  memcpy(out.target, buf + kOffTarget, 8);
  out.flags       = buf[kOffFlags];
  out.sequence    = buf[kOffSequence];
  out.type        = get16(buf + kOffType);
  return true;
}

bool parseStateService(const uint8_t* buf, size_t len, const Header& h, uint8_t& service,
                       uint32_t& port) {
  if (h.type != kTypeStateService) return false;
  if (h.size < kHeaderSize + 5 || len < h.size) return false;   // need 1 + 4 payload bytes
  service = buf[kHeaderSize];
  port = get32(buf + kHeaderSize + 1);
  return true;
}

bool parseStatePower(const uint8_t* buf, size_t len, const Header& h, uint16_t& level) {
  if (h.type != kTypeStatePower && h.type != kTypeStateLightPower) return false;
  if (h.size < kHeaderSize + 2 || len < h.size) return false;   // need 2 payload bytes
  level = get16(buf + kHeaderSize);
  return true;
}

// ---------------------------------------------------------------------------
// Bulb id helpers
// ---------------------------------------------------------------------------
bool parseMacHex(const char* text, uint8_t mac[6]) {
  if (text == nullptr) return false;
  uint8_t tmp[6];
  int digits = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == ':' || *p == '-') continue;          // allow d0:73:d5:aa:bb:cc
    const int v = hexValue(*p);
    if (v < 0) return false;                       // anything else is an error
    if (digits >= 12) return false;                // too long
    if ((digits & 1) == 0) {
      tmp[digits / 2] = (uint8_t)(v << 4);
    } else {
      tmp[digits / 2] = (uint8_t)(tmp[digits / 2] | v);
    }
    ++digits;
  }
  if (digits != 12) return false;                  // too short (or empty)
  memcpy(mac, tmp, 6);
  return true;
}

void macToHex(const uint8_t mac[6], char out[13]) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 6; ++i) {
    out[i * 2]     = digits[mac[i] >> 4];
    out[i * 2 + 1] = digits[mac[i] & 0x0F];
  }
  out[12] = '\0';
}

bool targetIsZero(const uint8_t target[8]) {
  for (int i = 0; i < 8; ++i) {
    if (target[i] != 0) return false;
  }
  return true;
}

}  // namespace lifxproto
