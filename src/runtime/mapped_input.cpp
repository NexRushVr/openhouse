#include "mapped_input.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace lh::runtime {
namespace {
using json = nlohmann::json;

// "/input/a/click" -> "/input/a"
std::string SourceOf(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos || slash == 0 ? path : path.substr(0, slash);
}

// "/input/a/click" -> {"a", "click"}
std::pair<std::string, std::string> NameAndField(const std::string& path) {
  const auto last = path.rfind('/');
  if (last == std::string::npos || last == 0) return {};
  const auto prev = path.rfind('/', last - 1);
  if (prev == std::string::npos) return {};
  return {path.substr(prev + 1, last - prev - 1), path.substr(last + 1)};
}

int IntOr(const json& j, const char* key, int fallback) {
  if (!j.contains(key)) return fallback;
  const auto& v = j.at(key);
  return v.is_number() ? v.get<int>() : fallback;
}

double DoubleOr(const json& j, const char* key, double fallback) {
  if (!j.contains(key)) return fallback;
  const auto& v = j.at(key);
  return v.is_number() ? v.get<double>() : fallback;
}
}  // namespace

bool ParseMappedProfile(const std::string& profileJson, const std::string& hand, MappedProfile* out,
                        std::string* error) {
  try {
    const json j = json::parse(profileJson, nullptr, true, /*ignore_comments=*/true);
    if (!j.contains("input_components") || !j.at("input_components").is_object()) {
      if (error) *error = "no input_components";
      return false;
    }
    MappedProfile p;
    p.controllerType = j.value("controller_type", "");
    const json sources = j.contains("input_source") && j.at("input_source").is_object() ? j.at("input_source") : json::object();
    const std::string side = hand == "left_hand" ? "left" : hand == "right_hand" ? "right" : "";
    for (const auto& [path, c] : j.at("input_components").items()) {
      if (!c.is_object()) continue;
      const std::string type = c.value("type", "");
      MappedComponent m;
      m.path = path;
      if (type == "vibration") {
        m.kind = MappedComponent::Kind::Haptic;
        p.components.push_back(m);
        continue;
      }
      if (!side.empty()) {
        const auto source = sources.find(SourceOf(path));
        if (source != sources.end() && source->is_object() && source->contains("side") &&
            source->at("side").is_string() && source->at("side").get<std::string>() != side)
          continue;
      }
      if (type == "bool") {
        m.kind = MappedComponent::Kind::Bool;
        m.bitWidth = 1;
      } else if (type == "scalar") {
        m.kind = MappedComponent::Kind::Scalar;
        m.bitWidth = IntOr(c, "bit_width", 8);
        m.twoSided = c.value("unit", "") == "twos_complement";
        m.min = DoubleOr(c, "min", 0);
        m.max = DoubleOr(c, "max", double((uint64_t(1) << std::clamp(m.bitWidth, 1, 32)) - 1));
      } else {
        continue;  // a type this driver doesn't know: left out, not guessed
      }
      m.bitOffset = IntOr(c, "bit_offset", -1);
      if (m.bitOffset < 0 || m.bitWidth < 1 || m.bitWidth > 32) continue;
      p.components.push_back(m);
    }
    *out = std::move(p);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

std::optional<double> MappedValue(const std::vector<uint8_t>& record, const MappedComponent& c) {
  if (c.kind == MappedComponent::Kind::Haptic) return std::nullopt;
  const int end = c.bitOffset + c.bitWidth;
  if (c.bitOffset < 0 || end > int(record.size()) * 8) return std::nullopt;
  uint64_t v = 0;
  for (int i = 0; i < c.bitWidth; ++i) {
    const int bit = c.bitOffset + i;
    if (record[size_t(bit / 8)] >> (bit % 8) & 1) v |= uint64_t(1) << i;
  }
  if (c.kind == MappedComponent::Kind::Bool) return v ? 1.0 : 0.0;
  // min..max onto 0..1, or onto -1..1 for a two-sided one. Despite its name the
  // "twos_complement" unit is not signed on the wire: a FlipVR stick at rest sends
  // 0x7f with min 0, max 255 (recorded), the middle of the range.
  const double t = c.max > c.min ? (double(v) - c.min) / (c.max - c.min) : double(v);
  return c.twoSided ? std::clamp(t * 2 - 1, -1.0, 1.0) : std::clamp(t, 0.0, 1.0);
}

void MappedToControllerInput(const MappedProfile& profile, const std::vector<uint8_t>& record, ControllerInput* out) {
  auto& s = *out;
  for (const auto& c : profile.components) {
    const auto value = MappedValue(record, c);
    if (!value) continue;
    const double v = *value;
    const bool on = v != 0;
    const auto [name, field] = NameAndField(c.path);
    if (name == "trigger") {
      if (field == "value") s.trigger = v;
      else if (field == "click") s.triggerClick = on;
      else if (field == "touch") s.triggerTouch = on;
    } else if (name == "grip") {
      if (field == "value" || field == "force") {
        s.gripForce = std::max(s.gripForce, v);
        s.fingerMiddle = s.fingerRing = s.fingerPinky = std::max(s.fingerMiddle, v);
      } else if (field == "click") {
        s.gripClick = on;
      } else if (field == "touch" && on) {
        s.fingerMiddle = s.fingerRing = s.fingerPinky = std::max(s.fingerMiddle, 0.5);
      }
    } else if (name == "joystick" || name == "thumbstick") {
      if (field == "x") s.thumbstickX = v;
      else if (field == "y") s.thumbstickY = v;
      else if (field == "click") s.thumbstickClick = on;
      else if (field == "touch") s.thumbstickTouch = on;
    } else if (name == "a" || name == "x") {
      if (field == "click") s.aClick = on;
      else if (field == "touch") s.aTouch = on;
    } else if (name == "b" || name == "y") {
      if (field == "click") s.bClick = on;
      else if (field == "touch") s.bTouch = on;
    } else if (name == "system") {
      if (field == "click") s.systemClick = on;
      else if (field == "touch") s.systemTouch = on;
    }
  }
  if (s.triggerTouch || s.trigger > 0) s.fingerIndex = std::max(s.fingerIndex, s.triggerTouch ? std::max(0.5, s.trigger) : s.trigger);
}

}  // namespace lh::runtime
