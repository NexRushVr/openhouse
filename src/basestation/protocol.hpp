// Lighthouse 2.0 base stations over Bluetooth LE: what the bytes mean.
//
// No radio code here (that's radio.hpp and its platform backends), so the tests can
// check every rule. Layout as read from 2.0 units (radio firmware 2.9):
//
//   service 00001523-1212-efde-1523-785feabcd124
//     00008421-1212-efde-1523-785feabcd124  identify  write (blinks the LED)
//     00001525-1212-efde-1523-785feabcd124  power     read, write, notify
//     00001524-1212-efde-1523-785feabcd124  channel   read, write, notify (1..16)
//   service 0x180A (Device Information): 0x2A29 manufacturer, 0x2A24 model, 0x2A25 the
//     sticker serial, 0x2A27 hardware revision, 0x2A26 firmware ("R: radio\nM: mcu\nB: boot")
//
// Adverts carry the name "LHB-XXXXXXXX" (the OOTX id in hex, the same serial the
// tracking uses) and manufacturer data under Valve's company id 0x055D:
// 00 02 <channel> 01 <power> 06 00. The channel byte matches the channel characteristic
// and the power byte matched the power characteristic on every unit read.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lh::basestation {

// A 128-bit UUID laid out like a Windows GUID (the backends convert it as needed).
struct Uuid {
  uint32_t data1 = 0;
  uint16_t data2 = 0, data3 = 0;
  std::array<uint8_t, 8> data4{};
  friend bool operator==(const Uuid&, const Uuid&) = default;
};
// 0000xxxx-0000-1000-8000-00805f9b34fb, a 16-bit Bluetooth SIG UUID.
constexpr Uuid Sig16(uint16_t id) { return {id, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}}; }
// 0000xxxx-1212-efde-1523-785feabcd124, the base station's own UUIDs.
constexpr Uuid Lh2(uint16_t id) { return {id, 0x1212, 0xefde, {0x15, 0x23, 0x78, 0x5f, 0xea, 0xbc, 0xd1, 0x24}}; }
std::string ToString(const Uuid& uuid);  // "00001525-1212-efde-1523-785feabcd124"

inline constexpr Uuid kControlService = Lh2(0x1523);
inline constexpr Uuid kChannelCharacteristic = Lh2(0x1524);
inline constexpr Uuid kPowerCharacteristic = Lh2(0x1525);
inline constexpr Uuid kIdentifyCharacteristic = Lh2(0x8421);
inline constexpr Uuid kDeviceInformationService = Sig16(0x180a);
inline constexpr Uuid kManufacturerName = Sig16(0x2a29);
inline constexpr Uuid kModelNumber = Sig16(0x2a24);
inline constexpr Uuid kHardwareSerial = Sig16(0x2a25);
inline constexpr Uuid kHardwareRevision = Sig16(0x2a27);
inline constexpr Uuid kFirmwareRevision = Sig16(0x2a26);
inline constexpr uint16_t kValveCompanyId = 0x055d;

// What the power characteristic (or the advert's power byte) says.
enum class Power { Unknown, Sleeping, Standby, Waking, On };
const char* ToString(Power power);
// 0x00 sleeping, 0x02 standby, 0x0b on; 0x01, 0x08 and 0x09 show up while it spins up.
Power DecodePower(uint8_t raw);

// What to write to the power characteristic.
enum class PowerCommand { On, Standby, Sleep };
const char* ToString(PowerCommand command);
std::optional<PowerCommand> ParsePowerCommand(std::string_view text);  // on/wake, standby, sleep/off
uint8_t CommandByte(PowerCommand command);  // on 0x01, standby 0x02, sleep 0x00
// The state a command ends in, and whether `power` is on the way there.
Power Target(PowerCommand command);
bool Settled(PowerCommand command, Power power);
bool InProgress(PowerCommand command, Power power);

inline constexpr int kMinChannel = 1, kMaxChannel = 16;
inline bool ValidChannel(int channel) { return channel >= kMinChannel && channel <= kMaxChannel; }

// The advert's manufacturer data under kValveCompanyId (after the company id).
struct AdvertStatus {
  int channel = 0;
  uint8_t powerRaw = 0;
  Power power = Power::Unknown;
};
std::optional<AdvertStatus> ParseAdvert(const uint8_t* data, size_t size);

// "R: <radio>    \nM: <mcu>    \nB: <bootloader>    " (space padded)
struct Firmware {
  std::string radio, mcu, bootloader;
};
Firmware ParseFirmwareRevision(std::string_view text);

// Serials: "LHB-" and the base's 32-bit id in upper-case hex.
std::string SerialFromId(uint32_t id);
std::optional<uint32_t> IdFromSerial(std::string_view serial);  // "LHB-1A2B3C4D", "1a2b3c4d"
std::optional<std::string> NormalizeSerial(std::string_view text);
std::optional<std::string> SerialFromName(std::string_view advertisedName);  // only "LHB-" names

// Bluetooth addresses as the radios report them (48 bits in a uint64).
std::string FormatAddress(uint64_t address);              // "C1:A2:B3:C4:D5:E6"
std::optional<uint64_t> ParseAddress(std::string_view text);

// SteamVR's list of power-managed base stations (steamvr.vrsettings, driver_lighthouse,
// "PowerManagedBaseStations2"): "LHB-5E6F7081:BS2:1,LHB-6F708192:BS1:1". The last field
// is whether SteamVR manages that base's power (the per-base switch in its base station
// settings).
struct ManagedEntry {
  std::string serial;
  int generation = 0;  // 2 for "BS2"
  bool managed = false;
};
std::vector<ManagedEntry> ParseManagedList(std::string_view text);

}  // namespace lh::basestation
