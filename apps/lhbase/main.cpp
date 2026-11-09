// lhbase: Lighthouse 2.0 base stations over this PC's Bluetooth.
//
//   lhbase                         list the 2.0 bases in range (channel, power, signal)
//   lhbase on|standby|sleep        power the room's bases (or the named ones)
//   lhbase identify SERIAL         blink a base's LED
//   lhbase channel SERIAL N        set a base's channel
//   lhbase info                    firmware versions and serials
//   lhbase watch                   wake the bases with SteamVR, sleep them after it
//
// The driver starts it on SteamVR start and exit (src/openvr/basestation_power.*).
#include "basestation/manager.hpp"
#include "basestation/targets.hpp"
#include "lh/version.hpp"
#include "platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace lh::basestation;
using namespace std::chrono_literals;

struct Args {
  std::string command = "status";
  std::vector<std::string> serials, except;
  bool room = false, all = false, json = false, quiet = false, force = false;
  int channel = 0;
  double scanSeconds = -1, waitSeconds = 0;
  std::string exitMode = "sleep";
  std::string logPath;
};

int Usage(int code) {
  std::cout <<
      "usage: lhbase [COMMAND] [BASE STATIONS] [OPTIONS]\n"
      "Lighthouse 2.0 base stations over this PC's Bluetooth (openhouse " << lh::kVersion << ").\n\n"
      "commands:\n"
      "  status            the 2.0 base stations in range: channel, power, signal (default)\n"
      "  on | wake         spin up and turn the lasers on\n"
      "  standby           lasers off, the motor keeps spinning (wakes at once)\n"
      "  sleep | off       lasers and motor off (about 12 s to spin back up)\n"
      "  identify          blink the LED\n"
      "  channel N         set a base station's channel (1-16)\n"
      "  info              firmware versions and serial numbers (connects to each)\n"
      "  watch             wake the base stations when SteamVR starts, sleep them when it exits\n\n"
      "base stations: serials (LHB-1A2B3C4D or 1A2B3C4D), and/or\n"
      "  --room            the room's: openhouse's room map, else SteamVR's base station\n"
      "                    power list (the default for every command but status)\n"
      "  --all             every 2.0 base station in range, a neighbour's too\n"
      "  --except SERIAL   not this one (repeatable)\n\n"
      "options:\n"
      "  --scan S          how long to listen for base stations (default 4)\n"
      "  --wait S          power: wait up to S seconds for the new state; a base that\n"
      "                    isn't there by then gets the command again, then counts as failed\n"
      "  --force           power: send it even when the advert says it's already there;\n"
      "                    channel: even when another base station in range is on it\n"
      "  --exit MODE       watch: sleep (default), standby or none when SteamVR exits\n"
      "  --json            machine-readable output\n"
      "  --log FILE        also append what happened to FILE\n"
      "  --quiet           print errors only\n";
  return code;
}

std::string Now() {
  const std::time_t t = std::time(nullptr);
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  return text;
}

struct Output {
  const Args& args;
  std::ofstream log;
  explicit Output(const Args& a) : args(a) {
    if (!a.logPath.empty()) {
      std::error_code ec;
      const std::filesystem::path p(a.logPath);
      if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
      // The driver logs here every session: keep one older megabyte at most.
      if (std::filesystem::file_size(p, ec) > (1u << 20) && !ec) {
        const std::filesystem::path older(a.logPath + ".old");
        std::filesystem::remove(older, ec);
        std::filesystem::rename(p, older, ec);
      }
      log.open(a.logPath, std::ios::app);
    }
  }
  void Line(const std::string& text, bool error = false) {
    if (error) std::cerr << text << "\n";
    else if (!args.quiet && !args.json) std::cout << text << "\n";
    if (log) log << Now() << " " << text << "\n" << std::flush;
  }
};

std::string PowerText(Power power, uint8_t raw) {
  char text[48];
  if (power == Power::Unknown) std::snprintf(text, sizeof text, "unknown (0x%02X)", unsigned(raw));
  else std::snprintf(text, sizeof text, "%s", ToString(power));
  return text;
}

// The serials a command works on: the named ones, the room's (--room, or when none is
// named), or all in range, minus --except and the ones switched off in SteamVR's list.
// `optional`: serials taken from SteamVR's power list, which keeps every 2.0 base it
// has ever seen, so those out of range are expected (not errors).
// false: an error was printed. True with no serials: nothing to do (said so).
bool Targets(const Args& args, Manager& manager, Output& out, std::vector<std::string>* serials,
             std::set<std::string>* optional) {
  std::vector<std::string> named, except = args.except;
  for (const auto& s : args.serials) {
    const auto n = NormalizeSerial(s);
    if (!n) {
      out.Line("not a base station serial: " + s, true);
      return false;
    }
    named.push_back(*n);
  }
  const auto room = ResolveRoom(DefaultRoomMapPath(), DefaultVrSettingsPath());
  if (args.all) {
    for (const auto& s : manager.Scan(std::chrono::milliseconds(int(1000 * (args.scanSeconds > 0 ? args.scanSeconds : 3)))))
      serials->push_back(s.serial);
    except.insert(except.end(), room.excluded.begin(), room.excluded.end());
    *serials = WithoutExcluded(*serials, except);
    if (!manager.RadioError().empty()) out.Line(manager.RadioError(), true);
    else if (serials->empty()) out.Line("no 2.0 base stations in Bluetooth range", true);
    return !serials->empty();
  }
  *serials = named;
  if (args.room || named.empty()) {
    if (room.serials.empty() && named.empty()) {
      if (args.room) {  // asked for the room as it is (the driver does): nothing to do
        out.Line(room.source.empty() ? "no room yet, and SteamVR lists no 2.0 base stations: nothing to do"
                                     : "no 2.0 base stations in " + room.source + ": nothing to do");
        return true;
      }
      out.Line("no room to take base stations from yet: name them (lhbase status lists them) or use --all", true);
      return false;
    }
    serials->insert(serials->end(), room.serials.begin(), room.serials.end());
    if (room.fromSteamVr) {
      for (const auto& s : room.serials)
        if (std::find(named.begin(), named.end(), s) == named.end()) optional->insert(s);
      out.Line("base stations from " + room.source + " (" + std::to_string(room.serials.size()) +
               "), the ones in Bluetooth range");
    } else if (!room.serials.empty()) {
      std::string list;
      for (const auto& serial : room.serials) list += (list.empty() ? "" : " ") + serial;
      out.Line("base stations from " + room.source + ": " + list);
    }
  }
  *serials = WithoutExcluded(*serials, except);  // also drops repeats
  return true;
}

Options MakeOptions(const Args& args) {
  Options o;
  if (args.scanSeconds > 0) o.scan = std::chrono::milliseconds(int(args.scanSeconds * 1000));
  o.wait = std::chrono::milliseconds(int(args.waitSeconds * 1000));
  o.force = args.force;
  return o;
}

int Status(const Args& args, Manager& manager, Output& out) {
  std::string error;
  std::set<std::string> wanted;
  for (const auto& s : args.serials)
    if (const auto n = NormalizeSerial(s)) wanted.insert(*n);
  const auto stations =
      manager.Scan(std::chrono::milliseconds(int(1000 * (args.scanSeconds > 0 ? args.scanSeconds : 3))), wanted, &error);
  if (!error.empty()) out.Line(error, true);
  nlohmann::json list = nlohmann::json::array();
  int shown = 0;
  for (const auto& s : stations) {
    if (!wanted.empty() && !wanted.count(s.serial)) continue;
    ++shown;
    char line[160];
    std::snprintf(line, sizeof line, "%s  channel %2d  %-14s %4d dBm  %s", s.serial.c_str(), s.channel,
                  PowerText(s.power, s.powerRaw).c_str(), s.rssi, FormatAddress(s.address).c_str());
    out.Line(line);
    list.push_back({{"serial", s.serial}, {"channel", s.channel}, {"power", ToString(s.power)},
                    {"powerRaw", s.powerRaw}, {"rssi", s.rssi}, {"address", FormatAddress(s.address)}});
  }
  for (const auto& w : wanted)
    if (std::none_of(stations.begin(), stations.end(), [&](const Station& s) { return s.serial == w; }))
      out.Line(w + "  not heard: out of Bluetooth range, unplugged, or not a 2.0 base station", true);
  if (args.json) std::cout << list.dump(2) << "\n";
  else out.Line(std::to_string(shown) + " base station" + (shown == 1 ? "" : "s") + " in range");
  return error.empty() && (wanted.empty() || size_t(shown) == wanted.size()) ? 0 : 1;
}

bool NotFound(const Outcome& o) { return !o.ok && o.error.rfind("not found", 0) == 0; }

// Commands that change bases run one at a time across processes: SteamVR can exit and
// start again within seconds (a restart), and the driver's sleep must not land after
// the next session's wake.
class OneAtATime {
 public:
  OneAtATime() {
#ifdef _WIN32
    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\openhouse-lhbase");
    if (mutex_) {
      const DWORD r = WaitForSingleObject(mutex_, 120000);
      held_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    }
#endif
  }
  ~OneAtATime() {
#ifdef _WIN32
    if (held_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
#endif
  }
  OneAtATime(const OneAtATime&) = delete;
  OneAtATime& operator=(const OneAtATime&) = delete;

 private:
#ifdef _WIN32
  HANDLE mutex_ = nullptr;
#endif
  [[maybe_unused]] bool held_ = false;
};

// Bluetooth off or absent: said once, not per base.
bool RadioFailed(const Manager& manager, Output& out) {
  const std::string error = manager.RadioError();
  if (!error.empty()) out.Line(error, true);
  return !error.empty();
}

int Report(const Args& args, Output& out, const Manager& manager, const std::vector<Outcome>& outcomes, const char* verb,
           const std::set<std::string>& optional) {
  if (RadioFailed(manager, out)) return 1;
  nlohmann::json list = nlohmann::json::array();
  int failed = 0, away = 0;
  for (const auto& o : outcomes) {
    char line[256];
    if (optional.count(o.serial) && NotFound(o)) {
      ++away;
      continue;
    }
    if (!o.ok) {
      ++failed;
      std::snprintf(line, sizeof line, "%s  %s failed: %s", o.serial.c_str(), verb, o.error.c_str());
      out.Line(line, true);
    } else if (o.skipped) {
      std::snprintf(line, sizeof line, "%s  already %s", o.serial.c_str(), ToString(o.power));
      out.Line(line);
    } else {
      std::snprintf(line, sizeof line, "%s  %s: now %s (%.1f s)", o.serial.c_str(), verb,
                    PowerText(o.power, o.powerRaw).c_str(), o.seconds);
      out.Line(line);
    }
    list.push_back({{"serial", o.serial}, {"ok", o.ok}, {"skipped", o.skipped}, {"error", o.error},
                    {"power", ToString(o.power)}, {"powerRaw", o.powerRaw}, {"channel", o.channel},
                    {"seconds", o.seconds}});
  }
  if (away) out.Line(std::to_string(away) + " more in that list aren't in Bluetooth range");
  if (args.json) std::cout << list.dump(2) << "\n";
  return failed ? 1 : 0;
}

// One power command for the targets, under the cross-process lock.
int SetPowerCommand(const Args& args, Manager& manager, Output& out, PowerCommand command) {
  OneAtATime lock;
  std::vector<std::string> serials;
  std::set<std::string> optional;
  if (!Targets(args, manager, out, &serials, &optional)) return 1;
  if (serials.empty()) return 0;
  return Report(args, out, manager, manager.SetPower(serials, command, MakeOptions(args)), ToString(command), optional);
}

int Identify(const Args& args, Manager& manager, Output& out) {
  OneAtATime lock;
  std::vector<std::string> serials;
  std::set<std::string> optional;
  if (!Targets(args, manager, out, &serials, &optional)) return 1;
  if (serials.empty()) return 0;
  const auto outcomes = manager.Identify(serials, MakeOptions(args));
  if (RadioFailed(manager, out)) return 1;
  int failed = 0;
  for (const auto& o : outcomes) {
    if (optional.count(o.serial) && NotFound(o)) continue;
    if (o.ok) out.Line(o.serial + "  blinking");
    else { ++failed; out.Line(o.serial + "  identify failed: " + o.error, true); }
  }
  return failed ? 1 : 0;
}

int Channel(const Args& args, Manager& manager, Output& out) {
  if (args.serials.size() != 1 || !ValidChannel(args.channel)) {
    out.Line("usage: lhbase channel SERIAL N   (N from 1 to 16, one base station at a time)", true);
    return 2;
  }
  const auto serial = NormalizeSerial(args.serials.front());
  if (!serial) { out.Line("not a base station serial: " + args.serials.front(), true); return 2; }
  OneAtATime lock;
  const Outcome o = manager.SetChannel(*serial, args.channel, MakeOptions(args));
  if (!o.ok) { out.Line(o.serial + "  channel change failed: " + o.error, true); return 1; }
  out.Line(o.serial + "  now on channel " + std::to_string(o.channel));
  return 0;
}

int Info(const Args& args, Manager& manager, Output& out) {
  std::vector<std::string> serials;
  std::set<std::string> optional;
  if (!Targets(args, manager, out, &serials, &optional)) return 1;
  if (serials.empty()) return 0;
  const auto details = manager.ReadDetails(serials, MakeOptions(args));
  if (RadioFailed(manager, out)) return 1;
  nlohmann::json list = nlohmann::json::array();
  int failed = 0;
  for (const auto& [o, d] : details) {
    if (optional.count(o.serial) && NotFound(o)) continue;
    if (!o.ok) { ++failed; out.Line(o.serial + "  failed: " + o.error, true); continue; }
    out.Line(o.serial + "  channel " + std::to_string(d.channel) + ", " + PowerText(d.power, d.powerRaw));
    out.Line("    model " + d.model + ", hardware serial " + d.hardwareSerial + ", revision " + d.hardwareRevision);
    out.Line("    firmware: radio " + d.firmware.radio + ", MCU " + d.firmware.mcu + ", bootloader " + d.firmware.bootloader);
    list.push_back({{"serial", o.serial}, {"channel", d.channel}, {"power", ToString(d.power)}, {"powerRaw", d.powerRaw},
                    {"manufacturer", d.manufacturer}, {"model", d.model}, {"hardwareSerial", d.hardwareSerial},
                    {"hardwareRevision", d.hardwareRevision}, {"radio", d.firmware.radio}, {"mcu", d.firmware.mcu},
                    {"bootloader", d.firmware.bootloader}});
  }
  if (args.json) std::cout << list.dump(2) << "\n";
  return failed ? 1 : 0;
}

// For SteamVR's own driver too: follow vrserver, wake the room's bases when it starts,
// sleep (or standby) them a little after it exits (not between a quick restart).
int Watch(const Args& args, Manager& manager, Output& out) {
  const auto exitCommand = args.exitMode == "none" ? std::optional<PowerCommand>() : ParsePowerCommand(args.exitMode);
  if (args.exitMode != "none" && (!exitCommand || *exitCommand == PowerCommand::On)) {
    out.Line("--exit is sleep, standby or none", true);
    return 2;
  }
  out.Line("watching for SteamVR (vrserver)");
  bool running = lh::platform::VrserverRunning();
  auto stoppedAt = std::chrono::steady_clock::time_point::min();
  bool pendingExit = false;
  if (running) SetPowerCommand(args, manager, out, PowerCommand::On);
  for (;;) {
    std::this_thread::sleep_for(2s);
    const bool now = lh::platform::VrserverRunning();
    if (now && !running) {
      pendingExit = false;
      out.Line("SteamVR started: waking the base stations");
      SetPowerCommand(args, manager, out, PowerCommand::On);
    } else if (!now && running) {
      stoppedAt = std::chrono::steady_clock::now();
      pendingExit = exitCommand.has_value();
    }
    running = now;
    if (pendingExit && !running && std::chrono::steady_clock::now() - stoppedAt > 10s) {
      pendingExit = false;
      out.Line(std::string("SteamVR exited: ") + ToString(*exitCommand));
      SetPowerCommand(args, manager, out, *exitCommand);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  bool haveCommand = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto number = [&](double* value) {
      if (i + 1 >= argc) return false;
      try { *value = std::stod(argv[++i]); } catch (...) { return false; }
      return true;
    };
    if (a == "--help" || a == "-h") return Usage(0);
    else if (a == "--room") args.room = true;
    else if (a == "--all") args.all = true;
    else if (a == "--json") args.json = true;
    else if (a == "--quiet" || a == "-q") args.quiet = true;
    else if (a == "--force") args.force = true;
    else if (a == "--scan") { if (!number(&args.scanSeconds)) return Usage(2); }
    else if (a == "--wait") { if (!number(&args.waitSeconds)) return Usage(2); }
    else if (a == "--exit" && i + 1 < argc) args.exitMode = argv[++i];
    else if (a == "--log" && i + 1 < argc) args.logPath = argv[++i];
    else if (a == "--except" && i + 1 < argc) args.except.push_back(argv[++i]);
    else if (a.size() > 1 && a[0] == '-') return Usage(2);
    else if (!haveCommand && (a == "status" || a == "on" || a == "wake" || a == "standby" || a == "sleep" || a == "off" ||
                              a == "identify" || a == "channel" || a == "info" || a == "watch")) {
      args.command = a;
      haveCommand = true;
    } else if (args.command == "channel" && args.serials.size() == 1 && args.channel == 0) {
      try { args.channel = std::stoi(a); } catch (...) { return Usage(2); }
    } else {
      args.serials.push_back(a);
    }
  }
  if (args.all && (args.room || !args.serials.empty())) return Usage(2);

  Output out(args);
  std::string error;
  const auto radio = OpenPlatformRadio(&error);
  if (!radio) {
    out.Line(error, true);
    return 1;
  }
  Manager manager(*radio, Manager::DefaultCachePath());
  if (args.command == "status") return Status(args, manager, out);
  if (const auto command = ParsePowerCommand(args.command)) return SetPowerCommand(args, manager, out, *command);
  if (args.command == "identify") return Identify(args, manager, out);
  if (args.command == "channel") return Channel(args, manager, out);
  if (args.command == "info") return Info(args, manager, out);
  if (args.command == "watch") return Watch(args, manager, out);
  return Usage(2);
}
