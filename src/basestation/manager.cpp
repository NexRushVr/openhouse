#include "basestation/manager.hpp"

#include "platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>

namespace lh::basestation {
namespace {
using Clock = std::chrono::steady_clock;
double Since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
// Connections at once: enough for a room, gentle on the PC's radio.
constexpr size_t kParallel = 4;
}  // namespace

Manager::Manager(Radio& radio, std::string cachePath) : radio_(radio), cachePath_(std::move(cachePath)) { LoadCache(); }

std::string Manager::DefaultCachePath() { return platform::ConfigDirectory() + "/basestations.json"; }

std::map<std::string, Station> Manager::Heard() const {
  std::lock_guard lock(mutex_);
  return heard_;
}

std::string Manager::RadioError() const {
  std::lock_guard lock(mutex_);
  return radioError_;
}

std::vector<Station> Manager::Scan(std::chrono::milliseconds duration, const std::set<std::string>& wanted,
                                   std::string* error) {
  // The name and the manufacturer data can come in different adverts (advert and scan
  // response): put them together per address.
  struct Partial {
    std::string name;
    bool hasStatus = false;
    AdvertStatus status;
  };
  std::mutex m;
  std::map<uint64_t, Partial> partial;
  std::map<std::string, Station> found;
  std::set<std::string> pending = wanted;  // wanted, not heard with a state yet
  auto onAdvert = [&](const Advert& advert) {
    std::lock_guard lock(m);
    Partial& p = partial[advert.address];
    if (!advert.name.empty()) p.name = advert.name;
    if (advert.hasValveData) {
      if (const auto status = ParseAdvert(advert.valveData.data(), advert.valveData.size())) {
        p.status = *status;
        p.hasStatus = true;
      }
    }
    const auto serial = SerialFromName(p.name);
    if (!serial) return true;
    Station& station = found[*serial];
    station.serial = *serial;
    station.address = advert.address;
    station.rssi = advert.rssi;
    station.heard = true;
    if (p.hasStatus) {
      station.channel = p.status.channel;
      station.power = p.status.power;
      station.powerRaw = p.status.powerRaw;
      pending.erase(*serial);
    }
    return wanted.empty() || !pending.empty();
  };
  std::string why;
  const bool listened = radio_.Scan(duration, onAdvert, &why);
  if (!listened && why.empty()) why = "the Bluetooth scan failed";
  if (!listened && error) *error = why;
  std::vector<Station> out;
  {
    std::lock_guard lock(m);
    for (const auto& [serial, station] : found) out.push_back(station);
  }
  std::lock_guard lock(mutex_);
  radioError_ = listened ? std::string() : why;
  heard_.clear();
  bool changed = false;
  for (const auto& station : out) {
    heard_[station.serial] = station;
    uint64_t& cached = addresses_[station.serial];
    if (cached != station.address) {
      cached = station.address;
      changed = true;
    }
  }
  if (changed) SaveCache();
  return out;
}

uint64_t Manager::AddressOf(const std::string& serial) const {
  std::lock_guard lock(mutex_);
  if (const auto heard = heard_.find(serial); heard != heard_.end()) return heard->second.address;
  if (const auto cached = addresses_.find(serial); cached != addresses_.end()) return cached->second;
  return 0;
}

template <class Work>
std::vector<Outcome> Manager::ForEach(const std::vector<std::string>& serials, const Work& work) {
  std::vector<Outcome> out(serials.size());
  std::vector<std::thread> threads;
  std::mutex m;
  std::condition_variable done;
  size_t running = 0;
  for (size_t i = 0; i < serials.size(); ++i) {
    {
      std::unique_lock lock(m);
      done.wait(lock, [&] { return running < kParallel; });
      ++running;
    }
    threads.emplace_back([&, i] {
      const auto start = Clock::now();
      out[i].serial = serials[i];
      work(serials[i], out[i]);
      out[i].seconds = Since(start);
      {
        std::lock_guard lock(m);
        --running;
      }
      done.notify_all();
    });
  }
  for (auto& thread : threads) thread.join();
  return out;
}

std::vector<Outcome> Manager::SetPower(const std::vector<std::string>& serials, PowerCommand command,
                                       const Options& options) {
  Scan(options.scan, std::set<std::string>(serials.begin(), serials.end()));
  const auto heard = Heard();
  const std::string radioError = RadioError();
  return ForEach(serials, [&](const std::string& serial, Outcome& out) {
    if (!radioError.empty()) { out.error = radioError; return; }
    const auto station = heard.find(serial);
    const bool wasHeard = station != heard.end();
    if (wasHeard) {
      out.power = station->second.power;
      out.powerRaw = station->second.powerRaw;
      out.channel = station->second.channel;
      if (!options.force && Settled(command, station->second.power)) {
        out.ok = out.skipped = true;
        return;
      }
    }
    const uint64_t address = AddressOf(serial);
    if (!address) {
      out.error = "not found: out of Bluetooth range, unplugged, or not a 2.0 base station";
      return;
    }
    // One try for a base that didn't advertise: it's probably unplugged.
    const int attempts = wasHeard ? std::max(1, options.attempts) : 1;
    std::string error;
    for (int attempt = 0; attempt < attempts && !out.ok; ++attempt) {
      if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto link = radio_.Connect(address, options.timeout, &error);
      if (!link || !link->Write(kControlService, kPowerCharacteristic, {CommandByte(command)}, &error)) continue;
      out.ok = true;
      out.power = Power::Unknown;  // the advert's state is old news now
      out.powerRaw = 0;
      const auto deadline = Clock::now() + options.wait;
      for (;;) {
        std::vector<uint8_t> value;
        if (link->Read(kControlService, kPowerCharacteristic, &value, &error) && !value.empty()) {
          out.powerRaw = value[0];
          out.power = DecodePower(value[0]);
        }
        if (Settled(command, out.power) || Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      // Asked to wait: the base has to get there, and one that doesn't gets the command again.
      if (options.wait.count() > 0 && !Settled(command, out.power)) {
        out.ok = false;
        error = std::string("still ") + ToString(out.power) + " after waiting";
      }
    }
    if (!out.ok) out.error = error.empty() ? "failed" : error;
  });
}

std::vector<Outcome> Manager::Identify(const std::vector<std::string>& serials, const Options& options) {
  Scan(options.scan, std::set<std::string>(serials.begin(), serials.end()));
  const std::string radioError = RadioError();
  return ForEach(serials, [&](const std::string& serial, Outcome& out) {
    if (!radioError.empty()) { out.error = radioError; return; }
    const uint64_t address = AddressOf(serial);
    if (!address) {
      out.error = "not found: out of Bluetooth range, unplugged, or not a 2.0 base station";
      return;
    }
    std::string error;
    for (int attempt = 0; attempt < std::max(1, options.attempts) && !out.ok; ++attempt) {
      if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto link = radio_.Connect(address, options.timeout, &error);
      out.ok = link && link->Write(kControlService, kIdentifyCharacteristic, {0x01}, &error);
    }
    if (!out.ok) out.error = error.empty() ? "failed" : error;
  });
}

Outcome Manager::SetChannel(const std::string& serial, int channel, const Options& options) {
  if (!ValidChannel(channel)) {
    Outcome out;
    out.serial = serial;
    out.error = "channels are 1 to 16";
    return out;
  }
  // The whole scan, not only until this base is heard: who else is on that channel?
  Scan(options.scan);
  const auto heard = Heard();
  const std::string radioError = RadioError();
  return ForEach({serial}, [&](const std::string&, Outcome& out) {
    if (!radioError.empty()) { out.error = radioError; return; }
    if (const auto self = heard.find(serial); self != heard.end()) out.channel = self->second.channel;
    if (!options.force)
      for (const auto& [other, station] : heard)
        if (other != serial && station.channel == channel) {
          out.error = other + " is on channel " + std::to_string(channel) + " already (two bases on one channel can't be told apart)";
          return;
        }
    const uint64_t address = AddressOf(serial);
    if (!address) {
      out.error = "not found: out of Bluetooth range, unplugged, or not a 2.0 base station";
      return;
    }
    std::string error;
    for (int attempt = 0; attempt < std::max(1, options.attempts) && !out.ok; ++attempt) {
      if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto link = radio_.Connect(address, options.timeout, &error);
      if (!link || !link->Write(kControlService, kChannelCharacteristic, {uint8_t(channel)}, &error)) continue;
      std::vector<uint8_t> value;
      if (link->Read(kControlService, kChannelCharacteristic, &value, &error) && !value.empty()) out.channel = value[0];
      out.ok = out.channel == channel;
      if (!out.ok && error.empty()) error = "the base reports channel " + std::to_string(out.channel);
    }
    if (!out.ok) out.error = error.empty() ? "failed" : error;
  }).front();
}

std::vector<std::pair<Outcome, Details>> Manager::ReadDetails(const std::vector<std::string>& serials,
                                                              const Options& options) {
  Scan(options.scan, std::set<std::string>(serials.begin(), serials.end()));
  std::vector<Details> details(serials.size());
  std::map<std::string, size_t> index;
  for (size_t i = 0; i < serials.size(); ++i) index[serials[i]] = i;
  std::mutex m;
  const std::string radioError = RadioError();
  auto outcomes = ForEach(serials, [&](const std::string& serial, Outcome& out) {
    if (!radioError.empty()) { out.error = radioError; return; }
    const uint64_t address = AddressOf(serial);
    if (!address) {
      out.error = "not found: out of Bluetooth range, unplugged, or not a 2.0 base station";
      return;
    }
    std::string error;
    Details d;
    for (int attempt = 0; attempt < std::max(1, options.attempts) && !out.ok; ++attempt) {
      if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto link = radio_.Connect(address, options.timeout, &error);
      if (!link) continue;
      std::vector<uint8_t> value;
      if (!link->Read(kControlService, kPowerCharacteristic, &value, &error) || value.empty()) continue;
      d.powerRaw = value[0];
      d.power = DecodePower(value[0]);
      if (link->Read(kControlService, kChannelCharacteristic, &value, &error) && !value.empty()) d.channel = value[0];
      auto text = [&](const Uuid& characteristic) {
        std::vector<uint8_t> bytes;
        std::string ignored;
        if (!link->Read(kDeviceInformationService, characteristic, &bytes, &ignored)) return std::string();
        std::string s(bytes.begin(), bytes.end());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0' || s.back() == '\n')) s.pop_back();
        return s;
      };
      d.manufacturer = text(kManufacturerName);
      d.model = text(kModelNumber);
      d.hardwareSerial = text(kHardwareSerial);
      d.hardwareRevision = text(kHardwareRevision);
      d.firmware = ParseFirmwareRevision(text(kFirmwareRevision));
      out.ok = true;
      out.power = d.power;
      out.powerRaw = d.powerRaw;
      out.channel = d.channel;
    }
    if (!out.ok) out.error = error.empty() ? "failed" : error;
    std::lock_guard lock(m);
    details[index[serial]] = d;
  });
  std::vector<std::pair<Outcome, Details>> out;
  for (size_t i = 0; i < serials.size(); ++i) out.emplace_back(outcomes[i], details[i]);
  return out;
}

void Manager::LoadCache() {
  if (cachePath_.empty()) return;
  try {
    std::ifstream in(cachePath_);
    if (!in) return;
    const auto j = nlohmann::json::parse(in);
    for (const auto& s : j.at("stations")) {
      const auto serial = NormalizeSerial(s.at("serial").get<std::string>());
      const auto address = ParseAddress(s.at("address").get<std::string>());
      if (serial && address) addresses_[*serial] = *address;
    }
  } catch (const std::exception&) {
    // A damaged cache only costs a longer scan.
  }
}

void Manager::SaveCache() const {  // under mutex_
  if (cachePath_.empty()) return;
  try {
    nlohmann::json stations = nlohmann::json::array();
    for (const auto& [serial, address] : addresses_)
      stations.push_back({{"serial", serial}, {"address", FormatAddress(address)}});
    const nlohmann::json j = {{"version", 1}, {"stations", stations}};
    const std::filesystem::path target(cachePath_);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
    // Two lhbase runs can save at once (status doesn't take the command lock).
    const auto temporary = target.string() + "." + std::to_string(platform::ProcessId()) + ".tmp";
    {
      std::ofstream out(temporary, std::ios::trunc);
      out << j.dump(2) << "\n";
      if (!out) return;
    }
    platform::ReplaceFile(temporary, target);
  } catch (const std::exception&) {
  }
}

}  // namespace lh::basestation
