// Base station operations on top of a Radio: find the 2.0 bases in range, power them
// on or down, blink them, change a channel, read their details.
//
// Every operation scans first (well under a second when the bases are in range): that
// finds their addresses and says what state each is in, so a base already in the asked
// state needs no connection at all. Then each base gets its own thread.
#pragma once
#include "basestation/protocol.hpp"
#include "basestation/radio.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace lh::basestation {

struct Station {
  std::string serial;  // "LHB-1A2B3C4D"
  uint64_t address = 0;
  int rssi = 0;
  int channel = 0;  // from its advert; 0 unknown
  Power power = Power::Unknown;
  uint8_t powerRaw = 0;
  bool heard = false;  // advertised in the last scan (else known from the address cache)
};

struct Outcome {
  std::string serial;
  bool ok = false;
  bool skipped = false;  // already in the asked state by its advert: nothing was sent
  std::string error;
  Power power = Power::Unknown;  // what the base said afterwards
  uint8_t powerRaw = 0;
  int channel = 0;
  double seconds = 0;
};

struct Details {
  std::string manufacturer, model, hardwareSerial, hardwareRevision;
  Firmware firmware;
  int channel = 0;
  Power power = Power::Unknown;
  uint8_t powerRaw = 0;
};

struct Options {
  std::chrono::milliseconds scan{4000};      // the longest wait for the bases' adverts
  std::chrono::milliseconds timeout{20000};  // connecting to one base, per attempt
  std::chrono::milliseconds wait{0};         // power: keep reading until the state is reached
  int attempts = 2;
  bool force = false;  // power: write even when the advert says it's already there;
                       // channel: even when another base in range is on it
};

class Manager {
 public:
  // cachePath: where base addresses are remembered between runs ("" for nowhere).
  Manager(Radio& radio, std::string cachePath);

  // Every 2.0 base station heard within `duration`; stops early once all of `wanted`
  // were heard with their state.
  std::vector<Station> Scan(std::chrono::milliseconds duration, const std::set<std::string>& wanted = {},
                            std::string* error = nullptr);
  std::vector<Outcome> SetPower(const std::vector<std::string>& serials, PowerCommand command,
                                const Options& options = {});
  std::vector<Outcome> Identify(const std::vector<std::string>& serials, const Options& options = {});
  Outcome SetChannel(const std::string& serial, int channel, const Options& options = {});
  std::vector<std::pair<Outcome, Details>> ReadDetails(const std::vector<std::string>& serials,
                                                       const Options& options = {});

  // What the last scan heard, by serial.
  std::map<std::string, Station> Heard() const;
  // Why the last scan couldn't listen at all (Bluetooth off or absent); "" when it could.
  // The operations above then fail every base with it, without trying to connect.
  std::string RadioError() const;
  static std::string DefaultCachePath();  // <config>/basestations.json

 private:
  template <class Work>
  std::vector<Outcome> ForEach(const std::vector<std::string>& serials, const Work& work);
  uint64_t AddressOf(const std::string& serial) const;
  void LoadCache();
  void SaveCache() const;

  Radio& radio_;
  std::string cachePath_;
  mutable std::mutex mutex_;
  std::map<std::string, Station> heard_;
  std::map<std::string, uint64_t> addresses_;  // from scans and the cache file
  std::string radioError_;
};

}  // namespace lh::basestation
