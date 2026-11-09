#include "basestation/targets.hpp"

#include "lh/optical.hpp"
#include "platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace lh::basestation {

std::string DefaultRoomMapPath() {
  if (auto path = platform::Env("LH_ROOM_MAP"); !path.empty()) return path;
  return platform::ConfigDirectory() + "/roommap.json";
}

std::string DefaultVrSettingsPath() {
  // SteamConfigDirectory() is <Steam>/config/lighthouse (or LH_STEAM_CONFIG_DIR's).
  return (std::filesystem::path(platform::SteamConfigDirectory()).parent_path() / "steamvr.vrsettings").string();
}

std::vector<std::string> RoomMapStations(const std::string& roomMapPath, std::string* error, bool* hasRoom) {
  std::vector<std::string> out;
  if (hasRoom) *hasRoom = false;
  try {
    std::ifstream in(roomMapPath);
    if (!in) return out;
    const auto j = nlohmann::json::parse(in);
    const nlohmann::json* newest = nullptr;
    double newestTime = -1;
    for (const auto& room : j.at("rooms")) {
      const double updated = room.value("updated", 0.0);
      if (!newest || updated > newestTime) {
        newest = &room;
        newestTime = updated;
      }
    }
    if (!newest) return out;
    if (hasRoom) *hasRoom = true;
    // The room map's base configs say which are 1.0 bases (their Bluetooth is another
    // protocol: not ours to wake). A base without a config yet is kept.
    std::set<uint32_t> lh1;
    if (const auto configs = j.find("baseStations"); configs != j.end() && configs->is_array())
      for (const auto& c : *configs)
        if (c.contains("serial") && c.contains("hardwareVersion") && ModelForHardware(c.at("hardwareVersion").get<int>()) != 5)
          lh1.insert(c.at("serial").get<uint32_t>());
    for (const auto& base : newest->at("bases")) {
      const auto id = base.at("serial").get<uint32_t>();
      if (!lh1.count(id)) out.push_back(SerialFromId(id));
    }
  } catch (const std::exception& e) {
    if (error) *error = "room map " + roomMapPath + ": " + e.what();
    if (hasRoom) *hasRoom = false;
    out.clear();
  }
  return out;
}

std::vector<ManagedEntry> SteamVrManagedStations(const std::string& vrsettingsPath) {
  try {
    std::ifstream in(vrsettingsPath);
    if (!in) return {};
    const auto j = nlohmann::json::parse(in, nullptr, true, true);  // SteamVR writes plain JSON; be lenient
    const auto section = j.find("driver_lighthouse");
    if (section == j.end() || !section->is_object()) return {};
    const auto list = section->find("PowerManagedBaseStations2");
    if (list == section->end() || !list->is_string()) return {};
    return ParseManagedList(list->get<std::string>());
  } catch (const std::exception&) {
    return {};
  }
}

std::vector<std::string> WithoutExcluded(const std::vector<std::string>& serials,
                                         const std::vector<std::string>& excluded) {
  std::set<std::string> drop;
  for (const auto& s : excluded)
    if (const auto n = NormalizeSerial(s)) drop.insert(*n);
  std::vector<std::string> out;
  for (const auto& s : serials) {
    const auto n = NormalizeSerial(s);
    if (n && !drop.count(*n) && std::find(out.begin(), out.end(), *n) == out.end()) out.push_back(*n);
  }
  return out;
}

RoomTargets ResolveRoom(const std::string& roomMapPath, const std::string& vrsettingsPath) {
  RoomTargets out;
  std::vector<std::string> managed, notOurs;  // notOurs: switched off, or 1.0 bases by SteamVR's list
  for (const auto& entry : SteamVrManagedStations(vrsettingsPath)) {
    if (!entry.managed) out.excluded.push_back(entry.serial);
    else if (entry.generation == 2) managed.push_back(entry.serial);
    if (!entry.managed || entry.generation != 2) notOurs.push_back(entry.serial);
  }
  bool hasRoom = false;
  if (auto room = RoomMapStations(roomMapPath, nullptr, &hasRoom); hasRoom) {  // even when it has no 2.0 base
    out.serials = WithoutExcluded(room, notOurs);
    out.source = "openhouse's room map";
  } else if (!managed.empty()) {
    out.serials = WithoutExcluded(managed, out.excluded);
    out.source = "SteamVR's base station power list";
    out.fromSteamVr = true;
  }
  return out;
}

}  // namespace lh::basestation
