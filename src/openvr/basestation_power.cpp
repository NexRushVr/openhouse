#include "basestation_power.hpp"

#include "basestation/targets.hpp"
#include "platform/platform.hpp"

#include <filesystem>

namespace lh::openvr {

std::string BaseStationPower::Tool() {
  if (auto tool = platform::Env("LH_BASESTATION_TOOL"); !tool.empty()) return tool;
#ifdef _WIN32
  const auto installed = std::filesystem::path(platform::DataDirectory()) / "lhbase.exe";
  std::error_code ec;
  if (std::filesystem::exists(installed, ec)) return installed.string();
#endif
  return {};
}

std::string BaseStationPower::LogPath() { return platform::DataDirectory() + "/basestations.log"; }

namespace {
void Finish(std::vector<std::string>* args, const std::vector<std::string>& excluded, const std::string& logPath) {
  for (const auto& serial : basestation::WithoutExcluded(excluded, {})) args->insert(args->end(), {"--except", serial});
  args->insert(args->end(), {"--quiet", "--log", logPath});
}
}  // namespace

std::vector<std::string> BaseStationPower::WakeArguments(const std::vector<std::string>& excluded,
                                                         const std::string& logPath) {
  std::vector<std::string> args{"on", "--room"};
  Finish(&args, excluded, logPath);
  return args;
}

std::vector<std::string> BaseStationPower::ExitArguments(const BaseStationPowerSettings& settings,
                                                         const std::vector<std::string>& seen,
                                                         const std::vector<std::string>& excluded,
                                                         const std::string& logPath) {
  if (!settings.enabled || (settings.onExit != "sleep" && settings.onExit != "standby")) return {};
  // The room's as at the start (a base no device looked at this session was woken too),
  // and any other base seen.
  std::vector<std::string> args{settings.onExit, "--room"};
  const auto serials = basestation::WithoutExcluded(seen, excluded);
  args.insert(args.end(), serials.begin(), serials.end());
  Finish(&args, excluded, logPath);
  return args;
}

bool BaseStationPower::Run(const std::vector<std::string>& args, bool atExit, const LogFn& log) {
  const std::string tool = Tool();
  if (tool.empty()) {
#ifdef _WIN32
    log("base station power: lhbase isn't installed (run the installer again)");
#endif
    return false;
  }
  std::string line;
  for (const auto& a : args) line += " " + a;
  // At exit, a host job that won't let it go could end it with the host: give it the
  // seconds a sleep takes (a scan, then every base at once).
  if (!platform::RunInBackground(tool, args, std::chrono::milliseconds(atExit ? 8000 : 0))) {
    log("base station power: could not start " + tool);
    return false;
  }
  log("base station power: lhbase" + line);
  return true;
}

void BaseStationPower::OnStart(const BaseStationPowerSettings& settings, const std::vector<std::string>& excluded,
                               const LogFn& log) {
  if (!settings.enabled) return;
  Run(WakeArguments(excluded, LogPath()), false, log);
}

}  // namespace lh::openvr
