// lifx_proto.h - the raw bytes of LIFX network packets, and NOTHING else.
//
// Why this is a separate file: it contains no Arduino code at all, only plain
// C++ that turns numbers into bytes and bytes back into numbers. That means it
// can be tested on a laptop (see cyd_pixel_toggle_tests/test_lifx_proto.cpp)
// long before it ever runs on the board. lifx.cpp does the Wi-Fi/timing work and
// calls these functions to make and read packets.
//
// Source of truth: LIFX LAN protocol docs, https://lan.developer.lifx.com
//   - "What is in a LIFX message" (/docs/packet-contents): the 36-byte header
//   - "Encoding a LIFX packet" (/docs/encoding-a-packet): a worked example
//   - "Querying the device for data" / "Changing a device" / "Information messages"
//
// Every LIFX packet = a 36-byte header + a payload (which may be empty).
// All numbers inside a packet are LITTLE-ENDIAN (lowest byte first).
//
//   byte  0-1   size       whole packet length in bytes, including this field
//   byte  2-3   frame      bits 0-11 protocol (always 1024), bit 12 addressable (always 1),
//                          bit 13 tagged (1 only for broadcast discovery), bits 14-15 origin (0)
//                          -> on the wire: "00 14" normal, "00 34" tagged
//   byte  4-7   source     a number WE pick; the bulb copies it into its replies
//   byte  8-15  target     bulb serial number (6 bytes, e.g. d0 73 d5 aa bb cc) + 2 zero bytes.
//                          All zeros = "any device" (used for broadcast)
//   byte 16-21  reserved   zeros
//   byte 22     flags      bit 0 = res_required, bit 1 = ack_required
//   byte 23     sequence   a counter WE pick (0-255, wraps); the bulb copies it into its replies
//   byte 24-31  reserved   zeros
//   byte 32-33  type       which message this is (see the k...Type constants below)
//   byte 34-35  reserved   zeros
//   byte 36...  payload
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace lifxproto {

// ---------------------------------------------------------------------------
// Fixed numbers from the docs
// ---------------------------------------------------------------------------
constexpr uint16_t kUdpPort    = 56700;   // every LIFX device listens on this UDP port
constexpr size_t   kHeaderSize = 36;      // bytes in every packet before the payload
constexpr size_t   kMaxBuildSize = 64;    // big enough for every packet this file can build (the biggest, SetColor, is 49)

// Message types ("type" field of the header).
constexpr uint16_t kTypeGetService       = 2;    // "who is out there?" (broadcast)
constexpr uint16_t kTypeStateService     = 3;    // reply: which service + port the device offers
constexpr uint16_t kTypeGetPower         = 20;   // "are you on?"      (works on every LIFX device)
constexpr uint16_t kTypeSetPower         = 21;   // "turn on/off"      (no fade time)
constexpr uint16_t kTypeStatePower       = 22;   // reply: power level
constexpr uint16_t kTypeGetLabel         = 23;   // "what is your name?"
constexpr uint16_t kTypeStateLabel       = 25;   // reply: name
constexpr uint16_t kTypeAcknowledgement  = 45;   // "got your message" (only if ack_required was set)
constexpr uint16_t kTypeSetColor         = 102;  // "change colour + brightness, fading over N milliseconds"
constexpr uint16_t kTypeGetLightPower    = 116;  // "are you on?"      (light bulbs)
constexpr uint16_t kTypeSetLightPower    = 117;  // "turn on/off, fading over N milliseconds"
constexpr uint16_t kTypeStateLightPower  = 118;  // reply: power level

constexpr uint8_t kServiceUdp = 1;         // StateService "service" value meaning "UDP"

// Bits of the header "flags" byte.
constexpr uint8_t kFlagResRequired = 0x01; // ask for a State reply (Get messages answer anyway)
constexpr uint8_t kFlagAckRequired = 0x02; // ask for an Acknowledgement packet

constexpr uint16_t kPowerOff = 0;
constexpr uint16_t kPowerOn  = 65535;

// ---------------------------------------------------------------------------
// A decoded header (what parseHeader() fills in)
// ---------------------------------------------------------------------------
struct Header {
  uint16_t size;          // whole packet length in bytes
  uint16_t protocol;      // should be 1024
  bool     addressable;
  bool     tagged;
  uint8_t  origin;
  uint32_t source;        // the number we put in our request
  uint8_t  target[8];     // device serial (first 6 bytes) + 2 zero bytes
  uint8_t  flags;         // kFlagResRequired / kFlagAckRequired
  uint8_t  sequence;      // the counter we put in our request
  uint16_t type;          // one of the kType... numbers
};

// ---------------------------------------------------------------------------
// Making packets.  Every builder:
//   - writes into `out` (which has room for `cap` bytes),
//   - returns the number of bytes written,
//   - returns 0 and writes NOTHING if `cap` is too small.
// `target` is 8 bytes (6-byte serial + 2 zero bytes). Pass nullptr for "all zeros"
// (which a bulb accepts when we talk to it directly by IP address).
// ---------------------------------------------------------------------------

// Just the 36 header bytes (payload length is only used to fill in the size field).
// Mostly useful to the builders below and to the tests.
size_t buildHeader(uint8_t* out, size_t cap, uint16_t type, size_t payloadLen, bool tagged,
                   uint32_t source, const uint8_t* target, uint8_t flags, uint8_t sequence);

// GetService (type 2): "everyone, tell me your address". `tagged` should be true for a
// broadcast (packet-contents page); the querying page says untagged also works, so lifx.cpp
// sends both kinds.
size_t buildGetService(uint8_t* out, size_t cap, uint32_t source, uint8_t sequence, bool tagged);

// Any "Get" message that has an empty payload (GetPower 20, GetLightPower 116, GetLabel 23).
// Flags are 0: the docs say a Get needs neither ack_required nor res_required.
size_t buildGet(uint8_t* out, size_t cap, uint16_t type, uint32_t source, const uint8_t* target,
                uint8_t sequence);

// SetLightPower (type 117): turn on (level 65535) or off (level 0), fading over `durationMs`.
// With ackRequired the bulb sends back an Acknowledgement carrying the same sequence number.
size_t buildSetLightPower(uint8_t* out, size_t cap, uint32_t source, const uint8_t* target,
                          uint8_t sequence, bool on, uint32_t durationMs, bool ackRequired);

// SetColor (type 102): change the colour and brightness, fading over `durationMs`.
// Colours are written as "HSBK" and every number uses the LIFX scale 0..65535:
//   hue         0..65535 = one full turn round the colour wheel (65536 would be 360 degrees)
//   saturation  65535 = full colour, 0 = white
//   brightness  0..65535
//   kelvin      2500..9000 warm/cool white; the bulb ignores it when saturation is 65535
// Payload = exactly 13 bytes: 1 reserved zero byte, then hue, saturation, brightness, kelvin
// (each u16) and the duration (u32). SetColor does NOT turn the bulb on or off: that is only
// SetLightPower's job. With ackRequired the bulb sends back an Acknowledgement carrying the
// same sequence number; lifx.cpp leaves it off for colours (it sends them again instead).
// Checked against https://lan.developer.lifx.com/docs/encoding-a-packet (worked example).
size_t buildSetColor(uint8_t* out, size_t cap, uint32_t source, const uint8_t* target,
                     uint8_t sequence, uint16_t hue, uint16_t saturation, uint16_t brightness,
                     uint16_t kelvin, uint32_t durationMs, bool ackRequired);

// ---------------------------------------------------------------------------
// Reading packets.  Every parser checks the length FIRST, so a truncated, empty
// or random packet can never make it read outside the buffer. They return false
// for anything that does not look right.
// ---------------------------------------------------------------------------

// Decode the header of the `len` bytes in `buf`. Rejects: fewer than 36 bytes, a size field
// smaller than 36 or larger than what actually arrived, a protocol number other than 1024.
bool parseHeader(const uint8_t* buf, size_t len, Header& out);

// StateService (type 3): 1 byte "service" + 4 byte "port".
bool parseStateService(const uint8_t* buf, size_t len, const Header& h, uint8_t& service,
                       uint32_t& port);

// StatePower (22) or StateLightPower (118): a 2 byte power level. 0 = off, anything else = on
// (the level can be in between while the bulb is fading).
bool parseStatePower(const uint8_t* buf, size_t len, const Header& h, uint16_t& level);

// ---------------------------------------------------------------------------
// Small helpers for the bulb id ("serial number" / MAC), e.g. d073d5aabbcc
// ---------------------------------------------------------------------------

// "d073d5aabbcc" (also accepts capitals and ':' or '-' between bytes) -> 6 bytes.
// False if it is not exactly 12 hex digits.
bool parseMacHex(const char* text, uint8_t mac[6]);

// 6 bytes -> "d073d5aabbcc" plus the end-of-text zero (so `out` needs 13 chars).
void macToHex(const uint8_t mac[6], char out[13]);

// True if all 8 target bytes are zero.
bool targetIsZero(const uint8_t target[8]);

}  // namespace lifxproto
