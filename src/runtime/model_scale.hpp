// Per-model sensor geometry scale, as SteamVR's own lighthouse driver applies it. A driver's
// resources may hold "lighthouse_scale.json": reference objects, each a CAD model's sensor
// points and the scale its units were measured to need:
//   "FlipVR Controller VC1B Right": {"scale": 1.048, "modelPoints": [[x, y, z], ...]}
// The stock driver reads {lighthouse}/lighthouse_scale.json and the one of each device's
// resource_root, and scales a device's modelPoints (about the object origin; normals, IMU
// and head unchanged) by the first reference whose points match its own: the same count,
// RMS distance under 1 mm and summed point radii within 0.2 % ("Similar CAD"). Without it a
// FlipVR right controller was solved 4.8 % too small, its pose off with the distance.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace lh::runtime {

struct ScaleReference {
  std::string name;
  double scale = 1;
  std::vector<std::array<double, 3>> points;
};

// The reference objects of a lighthouse_scale.json, in file order.
bool ParseScaleReferences(const std::string& text, std::vector<ScaleReference>* out, std::string* error);

struct ScaleMatch {
  const ScaleReference* reference = nullptr;  // applied, or nullptr
  std::vector<std::string> mismatched;        // similar CAD but a scale mismatch (as the stock logs)
};

// The stock driver's rule: the first reference that matches `points` scales them in place.
ScaleMatch ApplyScaleReference(const std::vector<ScaleReference>& references, std::vector<std::array<double, 3>>* points);

// Where a driver's resources are: SteamVR's drivers/<name>/resources, or an external driver
// (openvrpaths.vrpath "external_drivers") whose driver.vrdrivermanifest has that "name".
std::string DriverResourceDirectory(const std::string& driverName);

}  // namespace lh::runtime
