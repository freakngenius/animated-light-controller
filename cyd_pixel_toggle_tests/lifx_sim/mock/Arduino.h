// Arduino.h - a tiny stand-in for the ESP32 Arduino core, ONLY for running lifx.cpp on a laptop
// inside the simulator (lifx_sim/sim_lifx.cpp). It is not used when building for the real board.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdPASS 1
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
// The simulator runs the task and the test in lock-step (never at the same time), so no lock is needed.
#define portENTER_CRITICAL(m) do { (void)(m); } while (0)
#define portEXIT_CRITICAL(m) do { (void)(m); } while (0)
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))

uint32_t millis();                      // virtual clock, advanced by the simulator
void vTaskDelay(uint32_t ticks);        // hands control back to the simulator for one 10 ms step
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, uint32_t stackBytes,
                                   void* arg, int priority, void* handle, int core);
UBaseType_t uxTaskGetStackHighWaterMark(void* task);

class IPAddress {
 public:
  IPAddress() : a_(0) {}
  IPAddress(uint32_t a) : a_(a) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
      : a_((uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24)) {}
  operator uint32_t() const { return a_; }
  uint8_t operator[](int i) const { return (uint8_t)((a_ >> (8 * i)) & 0xFF); }
  bool fromString(const char* s) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    a_ = a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
    return true;
  }
 private:
  uint32_t a_;
};

struct HardwareSerialMock {
  void begin(int) {}
  void print(const char* s);            // captured by the simulator
};
extern HardwareSerialMock Serial;
