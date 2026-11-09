// test_lifx_proto.cpp - checks lifx_proto.cpp on your laptop (no board needed).
//
// What it proves:
//   1. The packets we BUILD match hand-computed byte tables taken from the official LIFX docs.
//   2. The replies we PARSE give the right numbers, and bad/short/lying packets are refused
//      instead of being read past their end.
//
// Run it with:   ./build_lifx_test.sh
// It prints one line per failed check and exits with code 0 only if everything passed.
//
// Doc sources for the byte tables (https://lan.developer.lifx.com):
//   [H] /docs/packet-contents          header layout, tagged/addressable bits, flags, byte order
//   [E] /docs/encoding-a-packet        worked example: SetColor (102) -> 49 bytes, quoted below
//                                      (header AND payload: 00 | 5555 | ffff | ffff | ac0d | 00000000)
//   [Q] /docs/querying-the-device-for-data   GetService (2), GetLightPower (116) have no payload
//   [C] /docs/changing-a-device        SetLightPower (117): level u16 + duration u32 (ms);
//                                      SetColor (102): reserved u8, hue u16, saturation u16,
//                                      brightness u16, kelvin u16, duration u32 (ms) = 13 bytes
//   [I] /docs/information-messages     StateService (3): service u8 + port u32; StateLightPower (118):
//                                      level u16; Acknowledgement (45): header only
//   [F] /docs/field-types              Services enum: 1 = UDP
#include "../cyd_pixel_toggle/lifx_proto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace lifxproto;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_fails;                                                                 \
      printf("FAIL  line %d: %s\n", __LINE__, #cond);                            \
    }                                                                            \
  } while (0)

static void printHex(const char* label, const uint8_t* p, size_t n) {
  printf("      %s (%zu bytes):", label, n);
  for (size_t i = 0; i < n; ++i) printf(" %02x", p[i]);
  printf("\n");
}

// Compare produced bytes against the expected table; print both on a mismatch.
static void expectBytes(const char* name, const uint8_t* got, size_t gotLen, const uint8_t* want,
                        size_t wantLen) {
  ++g_checks;
  if (gotLen == wantLen && memcmp(got, want, wantLen) == 0) return;
  ++g_fails;
  printf("FAIL  %s: bytes differ\n", name);
  printHex("expected", want, wantLen);
  printHex("produced", got, gotLen);
}

#define EXPECT_BYTES(name, got, gotLen, ...)                                     \
  do {                                                                           \
    static const uint8_t want_[] = {__VA_ARGS__};                                \
    expectBytes(name, got, gotLen, want_, sizeof(want_));                        \
  } while (0)

// Byte 3 of a tagged packet: protocol bits 0x04 (1024 = 0x400), addressable 0x10, tagged 0x20 -> 0x34.
// "./build_lifx_test.sh broken" swaps in 0x14 (the UNtagged value) to prove this test can fail.
#ifdef BREAK_ON_PURPOSE
static const uint8_t kExpectedTaggedFrameByte = 0x14;   // deliberately wrong
#else
static const uint8_t kExpectedTaggedFrameByte = 0x34;
#endif

// The bulb id used in most examples: d0:73:d5:aa:bb:cc (+ two zero bytes in the target field).
static const uint8_t kBulbTarget[8] = {0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00};

// ---------------------------------------------------------------------------
// 1. Building packets
// ---------------------------------------------------------------------------
static void testBuild() {
  uint8_t pkt[kMaxBuildSize];
  size_t n;

  // [E] The official worked example's 36-byte header (SetColor 102, to d073d5001337, source 2,
  //     sequence 1, ack_required). Doc text: "3100001402000000 d073d50013370000 0000000000000201
  //     0000000000000000 66000000 ..." - size 49, frame 00 14, target + 2 zero bytes, flags 02, seq 01.
  {
    static const uint8_t docTarget[8] = {0xd0, 0x73, 0xd5, 0x00, 0x13, 0x37, 0x00, 0x00};
    n = buildHeader(pkt, sizeof(pkt), /*type*/ 102, /*payloadLen*/ 13, /*tagged*/ false,
                    /*source*/ 2, docTarget, kFlagAckRequired, /*sequence*/ 1);
    EXPECT_BYTES("doc example header (SetColor)", pkt, n,
                 0x31, 0x00, 0x00, 0x14, 0x02, 0x00, 0x00, 0x00,    // size 49 | frame | source 2
                 0xd0, 0x73, 0xd5, 0x00, 0x13, 0x37, 0x00, 0x00,    // target
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01,    // reserved x6 | flags | sequence
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    // reserved x8
                 0x66, 0x00, 0x00, 0x00);                           // type 102 | reserved x2
  }

  // [H][Q] GetService, broadcast: size 36 (0x24), frame 00 34 (tagged), source 0x12345678,
  //        target all zeros, flags 0, sequence 7, type 2.
  n = buildGetService(pkt, sizeof(pkt), 0x12345678u, 7, /*tagged*/ true);
  EXPECT_BYTES("GetService tagged", pkt, n,
               0x24, 0x00, 0x00, kExpectedTaggedFrameByte, 0x78, 0x56, 0x34, 0x12,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x02, 0x00, 0x00, 0x00);

  // Same but untagged: only the frame bytes change (00 14).
  n = buildGetService(pkt, sizeof(pkt), 0x12345678u, 7, /*tagged*/ false);
  EXPECT_BYTES("GetService untagged", pkt, n,
               0x24, 0x00, 0x00, 0x14, 0x78, 0x56, 0x34, 0x12,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x02, 0x00, 0x00, 0x00);

  // [Q] GetLightPower (116 = 0x74) to d073d5aabbcc, source 0x01020304, sequence 10, no flags.
  n = buildGet(pkt, sizeof(pkt), kTypeGetLightPower, 0x01020304u, kBulbTarget, 10);
  EXPECT_BYTES("GetLightPower", pkt, n,
               0x24, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
               0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x74, 0x00, 0x00, 0x00);

  // GetPower (20 = 0x14) with no target (direct-by-IP use): everything zero except the basics.
  n = buildGet(pkt, sizeof(pkt), kTypeGetPower, 0xFFFFFFFEu, nullptr, 255);
  EXPECT_BYTES("GetPower, zero target, seq 255", pkt, n,
               0x24, 0x00, 0x00, 0x14, 0xfe, 0xff, 0xff, 0xff,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x14, 0x00, 0x00, 0x00);

  // [C] SetLightPower (117 = 0x75) ON, fade 300 ms (0x0000012c), ack_required.
  //     size 42 (0x2a); payload = level ff ff, duration 2c 01 00 00.
  n = buildSetLightPower(pkt, sizeof(pkt), 0x01020304u, kBulbTarget, 0xFE, true, 300, true);
  EXPECT_BYTES("SetLightPower ON 300ms ack", pkt, n,
               0x2a, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
               0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xfe,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x75, 0x00, 0x00, 0x00,
               0xff, 0xff, 0x2c, 0x01, 0x00, 0x00);

  // SetLightPower OFF, no fade, no ack, no target (a bulb reached by IP only).
  n = buildSetLightPower(pkt, sizeof(pkt), 0xFFFFFFFEu, nullptr, 0, false, 0, false);
  EXPECT_BYTES("SetLightPower OFF 0ms no-ack no-target", pkt, n,
               0x2a, 0x00, 0x00, 0x14, 0xfe, 0xff, 0xff, 0xff,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x75, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

  // Little-endian check on a long duration: 0x01020304 ms -> 04 03 02 01.
  n = buildSetLightPower(pkt, sizeof(pkt), 2, kBulbTarget, 1, true, 0x01020304u, true);
  CHECK(n == 42);
  CHECK(pkt[38] == 0x04 && pkt[39] == 0x03 && pkt[40] == 0x02 && pkt[41] == 0x01);

  // [E] SetColor: the official worked example in full, header + 13 payload bytes = 49. Same
  //     values as the docs: hue 21845 (0x5555), saturation 65535, brightness 65535, kelvin 3500
  //     (0x0dac), duration 0; ack_required, source 2, sequence 1, target d073d5001337.
  {
    static const uint8_t docTarget[8] = {0xd0, 0x73, 0xd5, 0x00, 0x13, 0x37, 0x00, 0x00};
    n = buildSetColor(pkt, sizeof(pkt), 2, docTarget, 1, 21845, 65535, 65535, 3500, 0, true);
    EXPECT_BYTES("SetColor, the docs' worked example", pkt, n,
                 0x31, 0x00, 0x00, 0x14, 0x02, 0x00, 0x00, 0x00,    // size 49 | frame | source 2
                 0xd0, 0x73, 0xd5, 0x00, 0x13, 0x37, 0x00, 0x00,    // target
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01,    // reserved x6 | flags | sequence
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    // reserved x8
                 0x66, 0x00, 0x00, 0x00,                            // type 102 | reserved x2
                 0x00,                                              // payload: reserved
                 0x55, 0x55,                                        // hue 0x5555
                 0xff, 0xff,                                        // saturation 0xffff
                 0xff, 0xff,                                        // brightness 0xffff
                 0xac, 0x0d,                                        // kelvin 0x0dac = 3500
                 0x00, 0x00, 0x00, 0x00);                           // duration 0
  }

  // SetColor the way lifx.cpp sends it: to d073d5aabbcc, source 0x01020304, sequence 0x2a, no flags at
  // all (no ack, no state reply), hue 0xabcd (43981), full saturation, brightness 0x0102 (258),
  // kelvin 3500, fade 100 ms (0x64). Every multi-byte number is lowest byte first.
  n = buildSetColor(pkt, sizeof(pkt), 0x01020304u, kBulbTarget, 0x2a, 0xabcd, 65535, 0x0102, 3500, 100, false);
  EXPECT_BYTES("SetColor hue abcd bri 0102, 100ms, no flags", pkt, n,
               0x31, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
               0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x66, 0x00, 0x00, 0x00,
               0x00,
               0xcd, 0xab,
               0xff, 0xff,
               0x02, 0x01,
               0xac, 0x0d,
               0x64, 0x00, 0x00, 0x00);

  // Different numbers again, so a swapped or shifted field cannot pass by luck: every field gets
  // its own value (hue 0x1122, saturation 0x3344, brightness 0x5566, kelvin 2500 = 0x09c4,
  // duration 0x01020304), ack_required on, no target (all zeros), source 0xFFFFFFFE, sequence 255.
  n = buildSetColor(pkt, sizeof(pkt), 0xFFFFFFFEu, nullptr, 255, 0x1122, 0x3344, 0x5566, 2500, 0x01020304u, true);
  EXPECT_BYTES("SetColor all different numbers, ack, no target", pkt, n,
               0x31, 0x00, 0x00, 0x14, 0xfe, 0xff, 0xff, 0xff,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x66, 0x00, 0x00, 0x00,
               0x00,
               0x22, 0x11,
               0x44, 0x33,
               0x66, 0x55,
               0xc4, 0x09,
               0x04, 0x03, 0x02, 0x01);

  // The scale ends: hue 0 and 65535, brightness 0 and 65535 (65535 must not turn into 0xff 0x00 etc.).
  n = buildSetColor(pkt, sizeof(pkt), 7, kBulbTarget, 0, 0, 65535, 0, 3500, 100, false);
  CHECK(n == 49 && pkt[37] == 0x00 && pkt[38] == 0x00 && pkt[41] == 0x00 && pkt[42] == 0x00);
  n = buildSetColor(pkt, sizeof(pkt), 7, kBulbTarget, 0, 65535, 65535, 65535, 3500, 100, false);
  CHECK(n == 49 && pkt[37] == 0xff && pkt[38] == 0xff && pkt[41] == 0xff && pkt[42] == 0xff);
  CHECK(pkt[36] == 0x00);                                // the reserved payload byte is always zero
  CHECK(kTypeSetColor == 102);
  CHECK(kMaxBuildSize >= 49);                            // the biggest packet we build must fit our buffers

  // Packets never write past the space we offer, and refuse when it is too small.
  {
    uint8_t guard[kMaxBuildSize + 8];
    memset(guard, 0xAA, sizeof(guard));
    n = buildHeader(guard, 35, kTypeGetService, 0, true, 5, nullptr, 0, 0);
    CHECK(n == 0);
    n = buildSetLightPower(guard, 41, 5, kBulbTarget, 0, true, 300, true);   // needs 42
    CHECK(n == 0);
    n = buildGetService(guard, 0, 5, 0, true);
    CHECK(n == 0);
    n = buildGet(nullptr, 100, kTypeGetPower, 5, nullptr, 0);
    CHECK(n == 0);
    bool untouched = true;
    for (size_t i = 0; i < sizeof(guard); ++i) untouched = untouched && guard[i] == 0xAA;
    CHECK(untouched);

    // Exact fit works and leaves the guard bytes after it alone.
    memset(guard, 0xAA, sizeof(guard));
    n = buildSetLightPower(guard, 42, 5, kBulbTarget, 0, true, 300, true);
    CHECK(n == 42);
    bool tailUntouched = true;
    for (size_t i = 42; i < sizeof(guard); ++i) tailUntouched = tailUntouched && guard[i] == 0xAA;
    CHECK(tailUntouched);

    // SetColor needs 49 bytes: one byte less is refused and writes nothing; exactly 49 works.
    memset(guard, 0xAA, sizeof(guard));
    n = buildSetColor(guard, 48, 5, kBulbTarget, 0, 1, 65535, 2, 3500, 100, false);
    CHECK(n == 0);
    n = buildSetColor(nullptr, 100, 5, kBulbTarget, 0, 1, 65535, 2, 3500, 100, false);
    CHECK(n == 0);
    bool colorUntouched = true;
    for (size_t i = 0; i < sizeof(guard); ++i) colorUntouched = colorUntouched && guard[i] == 0xAA;
    CHECK(colorUntouched);
    n = buildSetColor(guard, 49, 5, kBulbTarget, 0, 1, 65535, 2, 3500, 100, false);
    CHECK(n == 49);
    bool colorTailUntouched = true;
    for (size_t i = 49; i < sizeof(guard); ++i) colorTailUntouched = colorTailUntouched && guard[i] == 0xAA;
    CHECK(colorTailUntouched);
  }

  // Only the two defined flag bits are ever written.
  n = buildHeader(pkt, sizeof(pkt), 2, 0, false, 5, nullptr, 0xFF, 0);
  CHECK(pkt[22] == 0x03);
}

// ---------------------------------------------------------------------------
// 2. Parsing replies
// ---------------------------------------------------------------------------

// Sample reply bytes, laid out like a real bulb sends them (frame 00 14, our source echoed back).
// [I] StateService: service 1 (UDP), port 56700 = 0xDD7C -> 7c dd 00 00.
static const uint8_t kStateService[] = {
    0x29, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
    0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00,
    0x01, 0x7c, 0xdd, 0x00, 0x00};

// [I] StateLightPower (118 = 0x76), size 38, level ffff (on).
static const uint8_t kStateLightPowerOn[] = {
    0x26, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
    0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x76, 0x00, 0x00, 0x00,
    0xff, 0xff};

// Same, level 0 (off).
static const uint8_t kStateLightPowerOff[] = {
    0x26, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
    0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x76, 0x00, 0x00, 0x00,
    0x00, 0x00};

// Mid-fade level 0x1234 = 4660 (docs: the level can be anything between 0 and 65535 during a fade).
static const uint8_t kStateLightPowerMid[] = {
    0x26, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
    0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x76, 0x00, 0x00, 0x00,
    0x34, 0x12};

// [I] StatePower (22 = 0x16), size 38, level ffff.
static const uint8_t kStatePowerOn[] = {
    0x26, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
    0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x00, 0x00,
    0xff, 0xff};

// [I] Acknowledgement (45 = 0x2d): header only, sequence 0xFE.
static const uint8_t kAck[] = {
    0x24, 0x00, 0x00, 0x14, 0x04, 0x03, 0x02, 0x01,
    0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2d, 0x00, 0x00, 0x00};

static void testParseGood() {
  Header h;

  CHECK(parseHeader(kStateService, sizeof(kStateService), h));
  CHECK(h.size == 41);
  CHECK(h.protocol == 1024);
  CHECK(h.addressable && !h.tagged);
  CHECK(h.origin == 0);
  CHECK(h.source == 0x01020304u);
  CHECK(h.flags == 0);
  CHECK(h.sequence == 0x0b);
  CHECK(h.type == kTypeStateService);
  CHECK(memcmp(h.target, kBulbTarget, 8) == 0);
  CHECK(!targetIsZero(h.target));
  uint8_t service = 0;
  uint32_t port = 0;
  CHECK(parseStateService(kStateService, sizeof(kStateService), h, service, port));
  CHECK(service == kServiceUdp);
  CHECK(port == 56700);

  uint16_t level = 123;
  CHECK(parseHeader(kStateLightPowerOn, sizeof(kStateLightPowerOn), h));
  CHECK(h.type == kTypeStateLightPower && h.sequence == 0x0c);
  CHECK(parseStatePower(kStateLightPowerOn, sizeof(kStateLightPowerOn), h, level));
  CHECK(level == 65535);

  CHECK(parseHeader(kStateLightPowerOff, sizeof(kStateLightPowerOff), h));
  CHECK(parseStatePower(kStateLightPowerOff, sizeof(kStateLightPowerOff), h, level));
  CHECK(level == 0);

  CHECK(parseHeader(kStateLightPowerMid, sizeof(kStateLightPowerMid), h));
  CHECK(parseStatePower(kStateLightPowerMid, sizeof(kStateLightPowerMid), h, level));
  CHECK(level == 0x1234);

  CHECK(parseHeader(kStatePowerOn, sizeof(kStatePowerOn), h));
  CHECK(h.type == kTypeStatePower);
  CHECK(parseStatePower(kStatePowerOn, sizeof(kStatePowerOn), h, level));
  CHECK(level == 65535);

  CHECK(parseHeader(kAck, sizeof(kAck), h));
  CHECK(h.type == kTypeAcknowledgement && h.size == 36 && h.sequence == 0xfe);
  // An Acknowledgement is not a power or service reply.
  CHECK(!parseStatePower(kAck, sizeof(kAck), h, level));
  CHECK(!parseStateService(kAck, sizeof(kAck), h, service, port));

  // Extra bytes after the packet's own size are ignored (the size field wins).
  uint8_t padded[80];
  memset(padded, 0x55, sizeof(padded));
  memcpy(padded, kStateLightPowerOn, sizeof(kStateLightPowerOn));
  CHECK(parseHeader(padded, sizeof(padded), h));
  CHECK(h.size == 38);
  CHECK(parseStatePower(padded, sizeof(padded), h, level) && level == 65535);

  // Round trip: what we build, we can read back.
  uint8_t pkt[kMaxBuildSize];
  size_t n = buildSetLightPower(pkt, sizeof(pkt), 0xCAFEF00Du, kBulbTarget, 200, true, 300, true);
  CHECK(parseHeader(pkt, n, h));
  CHECK(h.source == 0xCAFEF00Du && h.sequence == 200 && h.type == kTypeSetLightPower);
  CHECK(h.flags == kFlagAckRequired && h.size == 42 && !h.tagged && h.addressable);
  CHECK(memcmp(h.target, kBulbTarget, 8) == 0);
  n = buildGetService(pkt, sizeof(pkt), 99, 3, true);
  CHECK(parseHeader(pkt, n, h));
  CHECK(h.tagged && targetIsZero(h.target) && h.type == kTypeGetService);
  n = buildSetColor(pkt, sizeof(pkt), 0xCAFEF00Du, kBulbTarget, 201, 100, 65535, 200, 3500, 100, false);
  CHECK(parseHeader(pkt, n, h));
  CHECK(h.source == 0xCAFEF00Du && h.sequence == 201 && h.type == kTypeSetColor);
  CHECK(h.flags == 0 && h.size == 49 && n == 49 && !h.tagged && h.addressable && h.origin == 0);
  CHECK(h.size - kHeaderSize == 13);                     // the payload is exactly 13 bytes
  CHECK(memcmp(h.target, kBulbTarget, 8) == 0);
}

static void testParseBad() {
  Header h;
  uint8_t service;
  uint32_t port;
  uint16_t level;

  CHECK(!parseHeader(nullptr, 100, h));
  CHECK(!parseHeader(kStateService, 0, h));
  CHECK(!parseHeader(kStateService, 35, h));            // one byte short of a header

  // Size field larger than what arrived (cut-off packet).
  CHECK(!parseHeader(kStateService, sizeof(kStateService) - 1, h));

  // Size field lies: claims 100 bytes, only 38 arrived.
  {
    uint8_t p[sizeof(kStateLightPowerOn)];
    memcpy(p, kStateLightPowerOn, sizeof(p));
    p[0] = 100;
    CHECK(!parseHeader(p, sizeof(p), h));
  }
  // Size field smaller than a header.
  {
    uint8_t p[sizeof(kStateLightPowerOn)];
    memcpy(p, kStateLightPowerOn, sizeof(p));
    p[0] = 30;
    CHECK(!parseHeader(p, sizeof(p), h));
    p[0] = 0;
    p[1] = 0;
    CHECK(!parseHeader(p, sizeof(p), h));
  }
  // Wrong protocol number (1023 instead of 1024): frame bytes ff 13.
  {
    uint8_t p[sizeof(kStateLightPowerOn)];
    memcpy(p, kStateLightPowerOn, sizeof(p));
    p[2] = 0xff;
    p[3] = 0x13;
    CHECK(!parseHeader(p, sizeof(p), h));
  }
  // Header is fine but the payload is too short for the message: size says 36 for a StatePower.
  {
    uint8_t p[36];
    memcpy(p, kStatePowerOn, 36);
    p[0] = 36;
    CHECK(parseHeader(p, sizeof(p), h));
    CHECK(h.type == kTypeStatePower);
    CHECK(!parseStatePower(p, sizeof(p), h, level));     // no level bytes
  }
  // StateService with only 4 of its 5 payload bytes (size 40).
  {
    uint8_t p[40];
    memcpy(p, kStateService, 40);
    p[0] = 40;
    CHECK(parseHeader(p, sizeof(p), h));
    CHECK(!parseStateService(p, sizeof(p), h, service, port));
  }
  // A parse function handed a Header whose size is bigger than the buffer we pass (misuse)
  // must still not read past `len`.
  {
    CHECK(parseHeader(kStateService, sizeof(kStateService), h));
    CHECK(!parseStateService(kStateService, 38, h, service, port));
    CHECK(parseHeader(kStateLightPowerOn, sizeof(kStateLightPowerOn), h));
    CHECK(!parseStatePower(kStateLightPowerOn, 37, h, level));
  }
  // Wrong message type for the parser.
  CHECK(parseHeader(kStateLightPowerOn, sizeof(kStateLightPowerOn), h));
  CHECK(!parseStateService(kStateLightPowerOn, sizeof(kStateLightPowerOn), h, service, port));
}

// Feed every prefix of every sample (and random garbage) to the parsers, each in a buffer that is
// EXACTLY as long as the data, so that address sanitizer (see build_lifx_test.sh) would catch any read
// past the end. The results are not checked here: the point is "never crashes, never over-reads".
static void parseEverything(const uint8_t* data, size_t len) {
  uint8_t* exact = (uint8_t*)malloc(len == 0 ? 1 : len);
  if (len) memcpy(exact, data, len);
  Header h;
  if (parseHeader(exact, len, h)) {
    uint8_t s;
    uint32_t p;
    uint16_t l;
    (void)parseStateService(exact, len, h, s, p);
    (void)parseStatePower(exact, len, h, l);
  }
  free(exact);
}

static void testNoOverreads() {
  const uint8_t* samples[] = {kStateService, kStateLightPowerOn, kStateLightPowerOff,
                              kStateLightPowerMid, kStatePowerOn, kAck};
  const size_t sizes[] = {sizeof(kStateService), sizeof(kStateLightPowerOn),
                          sizeof(kStateLightPowerOff), sizeof(kStateLightPowerMid),
                          sizeof(kStatePowerOn), sizeof(kAck)};
  for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
    for (size_t len = 0; len <= sizes[s]; ++len) parseEverything(samples[s], len);
  }
  // Random garbage, plus random single-byte damage to valid packets.
  srand(12345);
  uint8_t buf[128];
  for (int round = 0; round < 20000; ++round) {
    size_t len = (size_t)(rand() % 100);
    for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)rand();
    parseEverything(buf, len);

    const int which = round % 6;
    memcpy(buf, samples[which], sizes[which]);
    buf[rand() % sizes[which]] = (uint8_t)rand();
    parseEverything(buf, sizes[which]);
  }
  CHECK(true);   // reaching this line without a crash/sanitizer report is the result
}

// ---------------------------------------------------------------------------
// 3. Bulb id helpers
// ---------------------------------------------------------------------------
static void testMac() {
  uint8_t mac[6];
  static const uint8_t want[6] = {0xd0, 0x73, 0xd5, 0xaa, 0xbb, 0xcc};

  CHECK(parseMacHex("d073d5aabbcc", mac) && memcmp(mac, want, 6) == 0);
  CHECK(parseMacHex("D073D5AABBCC", mac) && memcmp(mac, want, 6) == 0);
  CHECK(parseMacHex("d0:73:d5:aa:bb:cc", mac) && memcmp(mac, want, 6) == 0);
  CHECK(parseMacHex("d0-73-d5-aa-bb-cc", mac) && memcmp(mac, want, 6) == 0);

  static const uint8_t keep[6] = {1, 2, 3, 4, 5, 6};
  memcpy(mac, keep, 6);
  CHECK(!parseMacHex("", mac));
  CHECK(!parseMacHex("d073d5aabb", mac));          // too short
  CHECK(!parseMacHex("d073d5aabbccdd", mac));      // too long
  CHECK(!parseMacHex("d073d5aabbcg", mac));        // not hex
  CHECK(!parseMacHex("d073 d5aabbcc", mac));       // space
  CHECK(!parseMacHex(nullptr, mac));
  CHECK(memcmp(mac, keep, 6) == 0);                // failed parses leave the output alone

  char text[13];
  macToHex(want, text);
  CHECK(strcmp(text, "d073d5aabbcc") == 0);

  static const uint8_t zero8[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(targetIsZero(zero8));
  CHECK(!targetIsZero(kBulbTarget));
}

int main() {
  testBuild();
  testParseGood();
  testParseBad();
  testNoOverreads();
  testMac();

  printf("%d checks, %d failed\n", g_checks, g_fails);
  if (g_fails == 0) printf("ALL OK\n");
  return g_fails == 0 ? 0 : 1;
}
