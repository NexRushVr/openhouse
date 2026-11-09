// The PC's Bluetooth LE radio, as much of it as base stations need: scan adverts,
// connect, read and write a characteristic. One backend per platform (radio_winrt.cpp
// on Windows; others report that they aren't supported yet), and tests use fakes.
#pragma once
#include "basestation/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lh::basestation {

struct Advert {
  uint64_t address = 0;
  std::string name;               // may be empty: some adverts carry only data
  int rssi = 0;                   // dBm
  bool hasValveData = false;      // manufacturer data under kValveCompanyId
  std::vector<uint8_t> valveData;
};

// One connected device. Destroying it disconnects.
class GattLink {
 public:
  virtual ~GattLink() = default;
  virtual bool Read(const Uuid& service, const Uuid& characteristic, std::vector<uint8_t>* value, std::string* error) = 0;
  // A write with response: true once the device acknowledged it.
  virtual bool Write(const Uuid& service, const Uuid& characteristic, const std::vector<uint8_t>& value,
                     std::string* error) = 0;
};

class Radio {
 public:
  virtual ~Radio() = default;
  // Listens for `duration`, calling `onAdvert` for every advert (repeats included) from
  // the radio's thread; returning false from it ends the scan early.
  virtual bool Scan(std::chrono::milliseconds duration, const std::function<bool(const Advert&)>& onAdvert,
                    std::string* error) = 0;
  // timeout: for connecting altogether (opening the link, and finding its services on
  // the first read or write); a read or write on the open link takes seconds at most.
  virtual std::unique_ptr<GattLink> Connect(uint64_t address, std::chrono::milliseconds timeout, std::string* error) = 0;
};

// nullptr (and why) when this platform or PC has no usable Bluetooth LE.
std::unique_ptr<Radio> OpenPlatformRadio(std::string* error);

}  // namespace lh::basestation
