#include <gtest/gtest.h>

#include "basestation/manager.hpp"
#include "basestation/protocol.hpp"
#include "basestation/targets.hpp"
#include "platform/platform.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace {
using namespace lh::basestation;

std::vector<uint8_t> Hex(const std::string& text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(uint8_t(std::stoi(text.substr(i, 2), nullptr, 16)));
  return out;
}

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          ("lh-test-basestation-" + std::to_string(lh::platform::ProcessId()) + "-" + name))
      .string();
}

TEST(BaseStationProtocol, PowerStates) {
  EXPECT_EQ(DecodePower(0x00), Power::Sleeping);
  EXPECT_EQ(DecodePower(0x02), Power::Standby);
  EXPECT_EQ(DecodePower(0x0b), Power::On);
  EXPECT_EQ(DecodePower(0x01), Power::Waking);
  EXPECT_EQ(DecodePower(0x09), Power::Waking);
  EXPECT_EQ(DecodePower(0x42), Power::Unknown);
  EXPECT_EQ(CommandByte(PowerCommand::On), 0x01);
  EXPECT_EQ(CommandByte(PowerCommand::Standby), 0x02);
  EXPECT_EQ(CommandByte(PowerCommand::Sleep), 0x00);
  EXPECT_TRUE(Settled(PowerCommand::Sleep, Power::Sleeping));
  EXPECT_FALSE(Settled(PowerCommand::On, Power::Waking));
  EXPECT_TRUE(InProgress(PowerCommand::On, Power::Waking));
  EXPECT_EQ(ParsePowerCommand("wake"), PowerCommand::On);
  EXPECT_EQ(ParsePowerCommand("off"), PowerCommand::Sleep);
  EXPECT_FALSE(ParsePowerCommand("reboot"));
}

TEST(BaseStationProtocol, Adverts) {
  // Manufacturer data (company 0x055D) of bases on channels 1, 7, 11 and 16, all on.
  const std::pair<const char*, int> adverts[] = {
      {"000201010b0600", 1}, {"000207010b0600", 7}, {"00020b010b0600", 11}, {"000210010b0600", 16}};
  for (const auto& [hex, channel] : adverts) {
    const auto bytes = Hex(hex);
    const auto status = ParseAdvert(bytes.data(), bytes.size());
    ASSERT_TRUE(status) << hex;
    EXPECT_EQ(status->channel, channel);
    EXPECT_EQ(status->power, Power::On);
  }
  const auto sleeping = Hex("000205010006");
  EXPECT_EQ(ParseAdvert(sleeping.data(), sleeping.size())->power, Power::Sleeping);
  for (const char* bad : {"000300010b0600", "000200010b0600", "000211010b0600", "000205"}) {
    const auto bytes = Hex(bad);
    EXPECT_FALSE(ParseAdvert(bytes.data(), bytes.size())) << bad;
  }
}

TEST(BaseStationProtocol, FirmwareSerialsAddresses) {
  const auto fw = ParseFirmwareRevision("R: 9.8.7000001    \nM: 6.5.4000002    \nB: 3.2.1000003    ");
  EXPECT_EQ(fw.radio, "9.8.7000001");
  EXPECT_EQ(fw.mcu, "6.5.4000002");
  EXPECT_EQ(fw.bootloader, "3.2.1000003");
  EXPECT_EQ(SerialFromId(0x1a2b3c4d), "LHB-1A2B3C4D");
  EXPECT_EQ(IdFromSerial("1a2b3c4d"), 0x1a2b3c4du);
  EXPECT_EQ(NormalizeSerial("lhb-1a2b3c4d"), "LHB-1A2B3C4D");
  EXPECT_FALSE(NormalizeSerial("LHR-708192A3"));
  EXPECT_FALSE(NormalizeSerial("LHB-7A31B93"));
  EXPECT_EQ(SerialFromName("LHB-1A2B3C4D"), "LHB-1A2B3C4D");
  EXPECT_FALSE(SerialFromName("HTC BS 123456"));
  EXPECT_EQ(FormatAddress(0xC1A2B3C4D5E6ull), "C1:A2:B3:C4:D5:E6");
  EXPECT_EQ(ParseAddress("C1:A2:B3:C4:D5:E6"), 0xC1A2B3C4D5E6ull);
  EXPECT_FALSE(ParseAddress("C1:A2:B3:C4:D5"));
  EXPECT_EQ(ToString(kPowerCharacteristic), "00001525-1212-efde-1523-785feabcd124");
  EXPECT_EQ(ToString(kFirmwareRevision), "00002a26-0000-1000-8000-00805f9b34fb");
}

TEST(BaseStationProtocol, SteamVrPowerList) {
  const auto list = ParseManagedList("LHB-5E6F7081:BS2:1,LHB-6F708192:BS1:1,LHB-2B3C4D5E:BS2:0,junk,LHB-12:BS2:1,");
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list[0].serial, "LHB-5E6F7081");
  EXPECT_EQ(list[0].generation, 2);
  EXPECT_TRUE(list[0].managed);
  EXPECT_EQ(list[1].generation, 1);
  EXPECT_FALSE(list[2].managed);
  EXPECT_TRUE(ParseManagedList("").empty());
}

TEST(BaseStationTargets, RoomMapFirstThenSteamVr) {
  const auto room = TempPath("roommap.json");
  const auto settings = TempPath("steamvr.vrsettings");
  {
    std::ofstream(room) << R"({"version":1,"rooms":[
      {"id":"old","updated":100,"bases":[{"serial":1,"pose":[0,0,0,1,0,0,0]}]},
      {"id":"new","updated":200,"bases":[{"serial":439041101,"pose":[0,0,0,1,0,0,0]},
                                         {"serial":725372254,"pose":[0,0,0,1,0,0,0]}]}]})";
    std::ofstream(settings) << R"({"driver_lighthouse":{"PowerManagedBaseStations2":
      "LHB-1A2B3C4D:BS2:1,LHB-2B3C4D5E:BS2:0,LHB-4D5E6F70:BS2:1,LHB-6F708192:BS1:1"}})";
  }
  EXPECT_EQ(RoomMapStations(room), (std::vector<std::string>{"LHB-1A2B3C4D", "LHB-2B3C4D5E"}));
  auto targets = ResolveRoom(room, settings);
  EXPECT_EQ(targets.serials, std::vector<std::string>{"LHB-1A2B3C4D"});  // the other is switched off in SteamVR
  EXPECT_EQ(targets.excluded, std::vector<std::string>{"LHB-2B3C4D5E"});
  EXPECT_FALSE(targets.fromSteamVr);
  targets = ResolveRoom(TempPath("missing.json"), settings);  // no room map: SteamVR's 2.0 bases
  EXPECT_EQ(targets.serials, (std::vector<std::string>{"LHB-1A2B3C4D", "LHB-4D5E6F70"}));
  EXPECT_TRUE(targets.fromSteamVr);
  std::filesystem::remove(room);
  std::filesystem::remove(settings);
}

// 1.0 bases have another Bluetooth protocol: a room's are never targets.
TEST(BaseStationTargets, LeavesOutLighthouse1Bases) {
  const auto room = TempPath("roommap-mixed.json");
  const auto settings = TempPath("steamvr-mixed.vrsettings");
  {
    // 439041101 is a 2.0 base by its config (hardware 15), 725372254 a 1.0 (hardware 9),
    // 1869644178 (6F708192) has no config yet but SteamVR lists it as a 1.0, 7 is unknown.
    std::ofstream(room) << R"({"version":1,"rooms":[{"id":"r","updated":1,"bases":[
        {"serial":439041101,"pose":[0,0,0,1,0,0,0]},{"serial":725372254,"pose":[0,0,0,1,0,0,0]},
        {"serial":1869644178,"pose":[0,0,0,1,0,0,0]},{"serial":7,"pose":[0,0,0,1,0,0,0]}]}],
      "baseStations":[{"serial":439041101,"hardwareVersion":15},{"serial":725372254,"hardwareVersion":9}]})";
    std::ofstream(settings) << R"({"driver_lighthouse":{"PowerManagedBaseStations2":"LHB-6F708192:BS1:1,LHB-4D5E6F70:BS2:1"}})";
  }
  bool hasRoom = false;
  EXPECT_EQ(RoomMapStations(room, nullptr, &hasRoom), (std::vector<std::string>{"LHB-1A2B3C4D", "LHB-6F708192", "LHB-00000007"}));
  EXPECT_TRUE(hasRoom);
  EXPECT_EQ(ResolveRoom(room, settings).serials, (std::vector<std::string>{"LHB-1A2B3C4D", "LHB-00000007"}));
  // A room of 1.0 bases only: no targets, and no falling back to SteamVR's list.
  std::ofstream(room) << R"({"version":1,"rooms":[{"id":"r","updated":1,"bases":[{"serial":725372254,"pose":[0,0,0,1,0,0,0]}]}],
      "baseStations":[{"serial":725372254,"hardwareVersion":9}]})";
  const auto targets = ResolveRoom(room, settings);
  EXPECT_TRUE(targets.serials.empty());
  EXPECT_FALSE(targets.fromSteamVr);
  std::filesystem::remove(room);
  std::filesystem::remove(settings);
}

// A room of base stations behind a fake radio.
struct FakeBase {
  std::string name;
  uint8_t power = 0x0b, channel = 1;
  bool advertises = true, splitAdvert = false, stuck = false;  // stuck: never finishes waking
  int connections = 0, identifies = 0, wakeReads = 0;
};

class FakeLink : public GattLink {
 public:
  FakeLink(std::mutex& m, FakeBase& base) : m_(m), base_(base) {}
  bool Read(const Uuid& service, const Uuid& c, std::vector<uint8_t>* value, std::string* error) override {
    std::lock_guard lock(m_);
    if (service == kControlService && c == kPowerCharacteristic) {
      // Waking: two reads of 0x09, then on.
      if (base_.power == 0x09 && !base_.stuck && ++base_.wakeReads > 2) base_.power = 0x0b;
      *value = {base_.power};
    } else if (service == kControlService && c == kChannelCharacteristic) {
      *value = {base_.channel};
    } else if (service == kDeviceInformationService && c == kFirmwareRevision) {
      const std::string fw = "R: 2.9.1\nM: 1.8.2\nB: 3.4.3";
      value->assign(fw.begin(), fw.end());
    } else {
      *error = "not found";
      return false;
    }
    return true;
  }
  bool Write(const Uuid& service, const Uuid& c, const std::vector<uint8_t>& value, std::string* error) override {
    std::lock_guard lock(m_);
    if (service != kControlService || value.size() != 1) { *error = "bad write"; return false; }
    if (c == kPowerCharacteristic) base_.power = value[0] == 0x01 ? 0x09 : value[0];
    else if (c == kChannelCharacteristic) base_.channel = value[0];
    else if (c == kIdentifyCharacteristic) ++base_.identifies;
    else { *error = "bad write"; return false; }
    return true;
  }

 private:
  std::mutex& m_;
  FakeBase& base_;
};

class FakeRadio : public Radio {
 public:
  std::map<uint64_t, FakeBase> bases;
  std::mutex m;
  std::string off;  // set: Bluetooth is off, scans fail with this
  int connects = 0;
  bool Scan(std::chrono::milliseconds, const std::function<bool(const Advert&)>& onAdvert, std::string* error) override {
    if (!off.empty()) { *error = off; return false; }
    std::vector<Advert> adverts;
    {
      std::lock_guard lock(m);
      for (const auto& [address, b] : bases) {
        if (!b.advertises) continue;
        Advert a;
        a.address = address;
        a.rssi = -55;
        a.name = b.name;
        a.hasValveData = true;
        a.valveData = {0x00, 0x02, b.channel, 0x01, b.power, 0x06, 0x00};
        if (b.splitAdvert) {  // name in the scan response, data in the advert
          Advert data = a;
          data.name.clear();
          a.hasValveData = false;
          a.valveData.clear();
          adverts.push_back(data);
        }
        adverts.push_back(a);
      }
    }
    for (const auto& a : adverts)
      if (!onAdvert(a)) break;
    return true;
  }
  std::unique_ptr<GattLink> Connect(uint64_t address, std::chrono::milliseconds, std::string* error) override {
    std::lock_guard lock(m);
    ++connects;
    const auto found = bases.find(address);
    if (found == bases.end()) { *error = "unreachable"; return nullptr; }
    ++found->second.connections;
    return std::make_unique<FakeLink>(m, found->second);
  }
};

TEST(BaseStationManager, PowerSkipsBasesAlreadyThere) {
  FakeRadio radio;
  radio.bases[0xC1A2B3C4D5E6] = {"LHB-1A2B3C4D", 0x0b, 5};
  radio.bases[0xD1D2D3D4D5D6] = {"LHB-2B3C4D5E", 0x00, 2};
  radio.bases[0xD1D2D3D4D5D6].splitAdvert = true;
  Manager manager(radio, "");
  const auto outcomes = manager.SetPower({"LHB-1A2B3C4D", "LHB-2B3C4D5E", "LHB-00000001"}, PowerCommand::Sleep);
  ASSERT_EQ(outcomes.size(), 3u);
  EXPECT_TRUE(outcomes[0].ok);
  EXPECT_FALSE(outcomes[0].skipped);
  EXPECT_EQ(outcomes[0].power, Power::Sleeping);
  EXPECT_TRUE(outcomes[1].ok);
  EXPECT_TRUE(outcomes[1].skipped);  // its advert said it was asleep: no connection
  EXPECT_EQ(radio.bases[0xD1D2D3D4D5D6].connections, 0);
  EXPECT_FALSE(outcomes[2].ok);  // never heard, never cached
  EXPECT_NE(outcomes[2].error.find("not found"), std::string::npos);
}

TEST(BaseStationManager, WakeWaitsForOn) {
  FakeRadio radio;
  radio.bases[0xE1E2E3E4E5E6] = {"LHB-4D5E6F70", 0x00, 12};
  Manager manager(radio, "");
  Options options;
  options.wait = std::chrono::milliseconds(5000);
  auto outcomes = manager.SetPower({"LHB-4D5E6F70"}, PowerCommand::On, options);
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_TRUE(outcomes[0].ok);
  EXPECT_EQ(outcomes[0].power, Power::On);
  EXPECT_EQ(outcomes[0].powerRaw, 0x0b);
  // Without --wait: done after the first read, still waking.
  radio.bases[0xE1E2E3E4E5E6].power = 0x00;
  radio.bases[0xE1E2E3E4E5E6].wakeReads = 0;
  outcomes = manager.SetPower({"LHB-4D5E6F70"}, PowerCommand::On);
  EXPECT_TRUE(outcomes[0].ok);
  EXPECT_EQ(outcomes[0].power, Power::Waking);
}

TEST(BaseStationManager, WaitFailsWhenABaseNeverGetsThere) {
  FakeRadio radio;
  radio.bases[0xE1E2E3E4E5E6] = {"LHB-4D5E6F70", 0x00, 12};
  radio.bases[0xE1E2E3E4E5E6].stuck = true;
  Manager manager(radio, "");
  Options options;
  options.wait = std::chrono::milliseconds(300);
  const auto outcomes = manager.SetPower({"LHB-4D5E6F70"}, PowerCommand::On, options);
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_FALSE(outcomes[0].ok);
  EXPECT_EQ(outcomes[0].power, Power::Waking);
  EXPECT_EQ(outcomes[0].channel, 12);
  EXPECT_NE(outcomes[0].error.find("still waking"), std::string::npos);
  EXPECT_EQ(radio.bases[0xE1E2E3E4E5E6].connections, 2);  // the command went out twice
}

TEST(BaseStationManager, BluetoothOffIsSaidNotGuessed) {
  const auto cache = TempPath("off.json");
  std::filesystem::remove(cache);
  FakeRadio radio;
  radio.bases[0xC1A2B3C4D5E6] = {"LHB-1A2B3C4D", 0x0b, 5};
  { Manager manager(radio, cache); manager.Scan(std::chrono::milliseconds(10)); }  // its address is cached now
  radio.off = "the Bluetooth scan was aborted: is Bluetooth on?";
  Manager manager(radio, cache);
  const int before = radio.connects;
  for (const auto& o : manager.SetPower({"LHB-1A2B3C4D", "LHB-00000001"}, PowerCommand::Sleep)) {
    EXPECT_FALSE(o.ok);
    EXPECT_EQ(o.error, radio.off);  // not "not found: unplugged..."
  }
  EXPECT_EQ(manager.RadioError(), radio.off);
  EXPECT_FALSE(manager.Identify({"LHB-1A2B3C4D"})[0].ok);
  EXPECT_FALSE(manager.SetChannel("LHB-1A2B3C4D", 9).ok);
  EXPECT_FALSE(manager.ReadDetails({"LHB-1A2B3C4D"})[0].first.ok);
  EXPECT_EQ(radio.connects, before);  // nothing was tried
  radio.off.clear();
  EXPECT_TRUE(manager.SetPower({"LHB-1A2B3C4D"}, PowerCommand::Sleep)[0].ok);
  EXPECT_TRUE(manager.RadioError().empty());
  std::filesystem::remove(cache);
}

TEST(BaseStationManager, ChannelRefusesOneAnotherBaseIsOn) {
  FakeRadio radio;
  radio.bases[0xC1A2B3C4D5E6] = {"LHB-1A2B3C4D", 0x0b, 5};
  radio.bases[0xD1D2D3D4D5D6] = {"LHB-2B3C4D5E", 0x0b, 2};
  Manager manager(radio, "");
  const auto refused = manager.SetChannel("LHB-1A2B3C4D", 2);
  EXPECT_FALSE(refused.ok);
  EXPECT_NE(refused.error.find("LHB-2B3C4D5E is on channel 2"), std::string::npos);
  EXPECT_EQ(radio.bases[0xC1A2B3C4D5E6].channel, 5);
  Options force;
  force.force = true;
  EXPECT_TRUE(manager.SetChannel("LHB-1A2B3C4D", 2, force).ok);
  EXPECT_EQ(radio.bases[0xC1A2B3C4D5E6].channel, 2);
  EXPECT_TRUE(manager.SetChannel("LHB-1A2B3C4D", 5).ok);  // its own channel again: nobody else on it
}

TEST(BaseStationManager, CachedAddressReachesQuietBase) {
  const auto cache = TempPath("basestations.json");
  std::filesystem::remove(cache);
  FakeRadio radio;
  radio.bases[0xF1F2F3F4F5F6] = {"LHB-3C4D5E6F", 0x02, 16};
  {
    Manager manager(radio, cache);
    EXPECT_EQ(manager.Scan(std::chrono::milliseconds(10)).size(), 1u);
  }
  ASSERT_TRUE(std::filesystem::exists(cache));
  radio.bases[0xF1F2F3F4F5F6].advertises = false;  // not heard, but still connectable
  Manager manager(radio, cache);
  const auto outcomes = manager.SetPower({"LHB-3C4D5E6F"}, PowerCommand::On);
  EXPECT_TRUE(outcomes[0].ok);
  EXPECT_EQ(radio.bases[0xF1F2F3F4F5F6].connections, 1);
  std::filesystem::remove(cache);
}

TEST(BaseStationManager, ChannelIdentifyDetails) {
  FakeRadio radio;
  radio.bases[0xC1A2B3C4D5E6] = {"LHB-1A2B3C4D", 0x0b, 5};
  Manager manager(radio, "");
  EXPECT_FALSE(manager.SetChannel("LHB-1A2B3C4D", 17).ok);
  const auto changed = manager.SetChannel("LHB-1A2B3C4D", 9);
  EXPECT_TRUE(changed.ok);
  EXPECT_EQ(changed.channel, 9);
  EXPECT_EQ(radio.bases[0xC1A2B3C4D5E6].channel, 9);
  EXPECT_TRUE(manager.Identify({"LHB-1A2B3C4D"})[0].ok);
  EXPECT_EQ(radio.bases[0xC1A2B3C4D5E6].identifies, 1);
  const auto details = manager.ReadDetails({"LHB-1A2B3C4D"});
  ASSERT_EQ(details.size(), 1u);
  EXPECT_TRUE(details[0].first.ok);
  EXPECT_EQ(details[0].second.channel, 9);
  EXPECT_EQ(details[0].second.firmware.radio, "2.9.1");
  EXPECT_EQ(details[0].second.power, Power::On);
}

}  // namespace
