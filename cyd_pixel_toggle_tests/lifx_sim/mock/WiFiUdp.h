// WiFiUdp.h - stand-in for WiFiUDP (simulator only). Copies the real class's behaviour that matters:
// parsePacket() returns 0 while an earlier packet has not been fully read or flushed.
#pragma once
#include "Arduino.h"
#include <vector>

void sim_udp_send(uint32_t ip, uint16_t port, const uint8_t* data, size_t len);
bool sim_udp_pop(uint32_t* fromIp, std::vector<uint8_t>* data);   // next packet that has "arrived"

class WiFiUDP {
 public:
  uint8_t begin(uint16_t) { open_ = true; return 1; }
  void stop() { open_ = false; rx_.clear(); rxPos_ = 0; haveRx_ = false; }
  int beginPacket(IPAddress ip, uint16_t port) {
    if (!open_ || port == 0) return 0;
    destIp_ = ip; destPort_ = port; tx_.clear();
    return 1;
  }
  size_t write(const uint8_t* b, size_t n) { tx_.insert(tx_.end(), b, b + n); return n; }
  int endPacket() {
    if (!open_) return 0;
    sim_udp_send((uint32_t)destIp_, destPort_, tx_.data(), tx_.size());
    return 1;
  }
  int parsePacket() {
    if (!open_ || haveRx_) return 0;
    uint32_t from;
    if (!sim_udp_pop(&from, &rx_)) return 0;
    remote_ = IPAddress(from); rxPos_ = 0; haveRx_ = true;
    return (int)rx_.size();
  }
  int read(unsigned char* buf, size_t len) {
    if (!haveRx_) return 0;
    size_t n = rx_.size() - rxPos_;
    if (n > len) n = len;
    memcpy(buf, rx_.data() + rxPos_, n);
    rxPos_ += n;
    if (rxPos_ >= rx_.size()) { haveRx_ = false; rx_.clear(); }
    return (int)n;
  }
  void flush() { haveRx_ = false; rx_.clear(); rxPos_ = 0; }
  IPAddress remoteIP() { return remote_; }
 private:
  bool open_ = false, haveRx_ = false;
  std::vector<uint8_t> tx_, rx_;
  size_t rxPos_ = 0;
  IPAddress destIp_, remote_;
  uint16_t destPort_ = 0;
};
