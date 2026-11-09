#include "basestation/protocol.hpp"

#include <cctype>
#include <cstdio>

namespace lh::basestation {

std::string ToString(const Uuid& u) {
  char text[40];
  std::snprintf(text, sizeof text, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", unsigned(u.data1),
                unsigned(u.data2), unsigned(u.data3), u.data4[0], u.data4[1], u.data4[2], u.data4[3], u.data4[4],
                u.data4[5], u.data4[6], u.data4[7]);
  return text;
}

const char* ToString(Power power) {
  switch (power) {
    case Power::Sleeping: return "sleeping";
    case Power::Standby: return "standby";
    case Power::Waking: return "waking";
    case Power::On: return "on";
    case Power::Unknown: break;
  }
  return "unknown";
}

Power DecodePower(uint8_t raw) {
  switch (raw) {
    case 0x00: return Power::Sleeping;
    case 0x02: return Power::Standby;
    case 0x0b: return Power::On;
    case 0x01: case 0x08: case 0x09: return Power::Waking;
    default: return Power::Unknown;
  }
}

const char* ToString(PowerCommand command) {
  switch (command) {
    case PowerCommand::On: return "on";
    case PowerCommand::Standby: return "standby";
    case PowerCommand::Sleep: return "sleep";
  }
  return "?";
}

std::optional<PowerCommand> ParsePowerCommand(std::string_view text) {
  if (text == "on" || text == "wake") return PowerCommand::On;
  if (text == "standby") return PowerCommand::Standby;
  if (text == "sleep" || text == "off") return PowerCommand::Sleep;
  return std::nullopt;
}

uint8_t CommandByte(PowerCommand command) {
  switch (command) {
    case PowerCommand::On: return 0x01;
    case PowerCommand::Standby: return 0x02;
    case PowerCommand::Sleep: return 0x00;
  }
  return 0x01;
}

Power Target(PowerCommand command) {
  switch (command) {
    case PowerCommand::On: return Power::On;
    case PowerCommand::Standby: return Power::Standby;
    case PowerCommand::Sleep: return Power::Sleeping;
  }
  return Power::Unknown;
}

bool Settled(PowerCommand command, Power power) { return power == Target(command); }

bool InProgress(PowerCommand command, Power power) {
  return command == PowerCommand::On && power == Power::Waking;
}

std::optional<AdvertStatus> ParseAdvert(const uint8_t* data, size_t size) {
  // 00 02 <channel> 01 <power> 06 00: byte 1 is 2 on every 2.0 base seen.
  if (!data || size < 5 || data[1] != 0x02 || !ValidChannel(data[2])) return std::nullopt;
  AdvertStatus status;
  status.channel = data[2];
  status.powerRaw = data[4];
  status.power = DecodePower(data[4]);
  return status;
}

namespace {
std::string Trim(std::string_view text) {
  size_t begin = 0, end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && (std::isspace(static_cast<unsigned char>(text[end - 1])) || text[end - 1] == '\0')) --end;
  return std::string(text.substr(begin, end - begin));
}
}  // namespace

Firmware ParseFirmwareRevision(std::string_view text) {
  Firmware out;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    const std::string line = Trim(text.substr(at, end - at));
    at = end + 1;
    if (line.size() < 2 || line[1] != ':') continue;
    const std::string value = Trim(std::string_view(line).substr(2));
    switch (line[0]) {
      case 'R': out.radio = value; break;
      case 'M': case 'A': out.mcu = value; break;
      case 'B': out.bootloader = value; break;
      default: break;
    }
  }
  return out;
}

std::string SerialFromId(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof text, "LHB-%08X", unsigned(id));
  return text;
}

std::optional<uint32_t> IdFromSerial(std::string_view serial) {
  if (serial.size() == 12 && (serial.substr(0, 4) == "LHB-" || serial.substr(0, 4) == "lhb-")) serial.remove_prefix(4);
  if (serial.size() != 8) return std::nullopt;
  uint32_t id = 0;
  for (char c : serial) {
    int digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return std::nullopt;
    id = (id << 4) | uint32_t(digit);
  }
  return id;
}

std::optional<std::string> NormalizeSerial(std::string_view text) {
  const auto id = IdFromSerial(text);
  if (!id) return std::nullopt;
  return SerialFromId(*id);
}

std::optional<std::string> SerialFromName(std::string_view name) {
  while (!name.empty() && (name.back() == '\0' || name.back() == ' ')) name.remove_suffix(1);
  if (name.size() != 12 || name.substr(0, 4) != "LHB-") return std::nullopt;
  return NormalizeSerial(name);
}

std::string FormatAddress(uint64_t address) {
  char text[24];
  std::snprintf(text, sizeof text, "%02X:%02X:%02X:%02X:%02X:%02X", unsigned(address >> 40) & 0xff,
                unsigned(address >> 32) & 0xff, unsigned(address >> 24) & 0xff, unsigned(address >> 16) & 0xff,
                unsigned(address >> 8) & 0xff, unsigned(address) & 0xff);
  return text;
}

std::optional<uint64_t> ParseAddress(std::string_view text) {
  uint64_t address = 0;
  int digits = 0;
  for (char c : text) {
    if (c == ':' || c == '-') continue;
    int digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return std::nullopt;
    address = (address << 4) | uint64_t(digit);
    if (++digits > 12) return std::nullopt;
  }
  if (digits != 12) return std::nullopt;
  return address;
}

std::vector<ManagedEntry> ParseManagedList(std::string_view text) {
  std::vector<ManagedEntry> out;
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find(',', at);
    if (end == std::string_view::npos) end = text.size();
    std::string_view item = text.substr(at, end - at);
    at = end + 1;
    // LHB-XXXXXXXX:BS2:1
    const size_t first = item.find(':');
    const size_t second = first == std::string_view::npos ? first : item.find(':', first + 1);
    if (second == std::string_view::npos) continue;
    const auto serial = NormalizeSerial(item.substr(0, first));
    const std::string_view kind = item.substr(first + 1, second - first - 1);
    const std::string_view flag = item.substr(second + 1);
    if (!serial || kind.size() != 3 || kind.substr(0, 2) != "BS" || kind[2] < '0' || kind[2] > '9') continue;
    out.push_back({*serial, kind[2] - '0', flag == "1"});
  }
  return out;
}

}  // namespace lh::basestation
