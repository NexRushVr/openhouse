// "Mapped input" controllers: firmware that sends its input record as raw bytes
// (controller sub-protocol 1) and leaves the layout to the device's own input profile,
// as the stock driver reads it (config firmware_config.mapped_input). Shiftall's FlipVR
// is one: {shiftall}/input/flipvr_controller_vc1b_profile.json names each component's
// bits under "input_components":
//   "/input/joystick/x": {"bit_offset": 0, "bit_width": 8, "min": 0, "max": 255,
//                         "type": "scalar", "unit": "twos_complement"},
//   "/input/a/click":    {"bit_offset": 36, "type": "bool"},
//   "/output/haptic":    {"type": "vibration"}
// Bits count from the least significant bit of the first byte.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lh/session.hpp"

namespace lh::runtime {

struct MappedComponent {
  enum class Kind { Bool, Scalar, Haptic };
  std::string path;  // "/input/a/click"
  Kind kind = Kind::Bool;
  int bitOffset = 0, bitWidth = 1;
  bool twoSided = false;  // unit "twos_complement": min..max onto -1..1; else onto 0..1
  double min = 0, max = 0;
};

struct MappedProfile {
  std::string controllerType;  // the profile's "controller_type", e.g. "flipvr_controller_vc1b"
  std::vector<MappedComponent> components;
};

// Parses an input profile. `hand` ("left_hand"/"right_hand", or "" for both) leaves out
// the components of an input_source meant for the other side ("side": "left"/"right").
bool ParseMappedProfile(const std::string& profileJson, const std::string& hand, MappedProfile* out,
                        std::string* error);

// A component's value in a record: bool 0/1, scalar normalized (0..1, or -1..1 when
// two-sided). nullopt for a haptic, or when the record is too short for its bits.
std::optional<double> MappedValue(const std::vector<uint8_t>& record, const MappedComponent& c);

// The record's components by name onto the fields the hand skeleton and the
// controller-in-use check read (trigger, grip, thumbstick, A/B or X/Y, system).
void MappedToControllerInput(const MappedProfile& profile, const std::vector<uint8_t>& record, ControllerInput* out);

}  // namespace lh::runtime
