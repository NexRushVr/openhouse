// Which base stations "the room" means, for commands that don't name them.
//
// 1. openhouse's room map (roommap.json): the 2.0 bases of the room updated last;
// 2. without one, SteamVR's power-managed 2.0 bases (steamvr.vrsettings,
//    driver_lighthouse/PowerManagedBaseStations2), which SteamVR fills with every 2.0
//    base it has seen;
// and never a base switched off in SteamVR's own power management list.
#pragma once
#include "basestation/protocol.hpp"

#include <string>
#include <vector>

namespace lh::basestation {

struct RoomTargets {
  std::vector<std::string> serials;
  std::vector<std::string> excluded;  // switched off in SteamVR's list
  std::string source;                 // where `serials` came from, for messages
  bool fromSteamVr = false;           // SteamVR's list: every 2.0 base it ever saw, most out of range
};

// The 2.0 bases of the room updated last in an openhouse room map. hasRoom: whether it
// has a room at all (one of 1.0 bases only gives none).
std::vector<std::string> RoomMapStations(const std::string& roomMapPath, std::string* error = nullptr,
                                         bool* hasRoom = nullptr);
// SteamVR's power management list from a steamvr.vrsettings file (empty if absent).
std::vector<ManagedEntry> SteamVrManagedStations(const std::string& vrsettingsPath);

std::string DefaultRoomMapPath();     // <config>/roommap.json, or LH_ROOM_MAP
std::string DefaultVrSettingsPath();  // <Steam>/config/steamvr.vrsettings

RoomTargets ResolveRoom(const std::string& roomMapPath, const std::string& vrsettingsPath);
// Drops the excluded serials from `serials` (case-insensitive serial match).
std::vector<std::string> WithoutExcluded(const std::vector<std::string>& serials,
                                         const std::vector<std::string>& excluded);

}  // namespace lh::basestation
