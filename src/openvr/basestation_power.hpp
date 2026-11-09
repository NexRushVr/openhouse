// 2.0 base stations' power with SteamVR, over the PC's Bluetooth: the driver wakes the
// room's bases when it starts and, when asked to in the settings, puts them to sleep (or
// standby) when SteamVR exits.
// SteamVR's own driver does this only through a headset's Bluetooth radio (Index, Vive
// Pro), so with any other headset (Quest + Virtual Desktop, Bigscreen Beyond, ...) the
// bases otherwise spin until they're unplugged.
//
// The Bluetooth work runs in lhbase (apps/lhbase), out of the host's process: it takes
// a few seconds and has to finish after the host has gone at shutdown.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace lh::openvr {

struct BaseStationPowerSettings {
  bool enabled = true;          // driver_openhouse/baseStationPower
  // driver_openhouse/baseStationPowerOff: none, sleep, standby. None by default, like
  // SteamVR's own base station power management: bases can be shared with another PC.
  std::string onExit = "none";
};

class BaseStationPower {
 public:
  using LogFn = std::function<void(const std::string&)>;

  // lhbase: LH_BASESTATION_TOOL, else where the installer puts it (the data directory,
  // next to openhouse-check). "" when it isn't there.
  static std::string Tool();
  static std::string LogPath();  // <data>/basestations.log

  // lhbase's arguments: waking the room's bases, and the exit command for the room's
  // plus `seen` (the 2.0 bases seen this session), both without `excluded` (switched
  // off in SteamVR's base station power list). Empty: nothing to do.
  static std::vector<std::string> WakeArguments(const std::vector<std::string>& excluded, const std::string& logPath);
  static std::vector<std::string> ExitArguments(const BaseStationPowerSettings& settings,
                                                const std::vector<std::string>& seen,
                                                const std::vector<std::string>& excluded, const std::string& logPath);

  void OnStart(const BaseStationPowerSettings& settings, const std::vector<std::string>& excluded, const LogFn& log);
  // Starts lhbase with `args`. atExit: where the host's job would end it with the host,
  // waits the few seconds it takes.
  bool Run(const std::vector<std::string>& args, bool atExit, const LogFn& log);
};

}  // namespace lh::openvr
