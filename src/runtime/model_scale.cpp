#include "model_scale.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "platform/platform.hpp"

namespace lh::runtime {
namespace {
using json = nlohmann::json;

// The stock driver's thresholds (driver_lighthouse.dll, next to "Applying scale %0.04f from
// reference object %s"): RMS point distance, and the ratio of summed point radii.
constexpr double kMaxRms = 0.001;  // m
constexpr double kMaxRatio = 1.002;

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Utf8(const std::filesystem::path& path) {
  const auto s = path.u8string();
  return std::string(s.begin(), s.end());
}
}  // namespace

bool ParseScaleReferences(const std::string& text, std::vector<ScaleReference>* out, std::string* error) {
  try {
    const json j = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    if (!j.is_object()) throw std::runtime_error("not an object");
    std::vector<ScaleReference> refs;
    for (const auto& [name, r] : j.items()) {
      if (!r.is_object() || !r.contains("scale") || !r.at("scale").is_number() || !r.contains("modelPoints") ||
          !r.at("modelPoints").is_array())
        continue;
      ScaleReference ref;
      ref.name = name;
      ref.scale = r.at("scale").get<double>();
      for (const auto& p : r.at("modelPoints")) {
        if (!p.is_array() || p.size() < 3) throw std::runtime_error("bad modelPoints entry in " + name);
        ref.points.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
      }
      if (ref.points.empty() || !std::isfinite(ref.scale) || ref.scale <= 0) continue;
      refs.push_back(std::move(ref));
    }
    *out = std::move(refs);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

ScaleMatch ApplyScaleReference(const std::vector<ScaleReference>& references, std::vector<std::array<double, 3>>* points) {
  ScaleMatch match;
  auto& own = *points;
  const auto norm = [](const std::array<double, 3>& p) { return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); };
  for (const auto& ref : references) {
    if (ref.points.size() != own.size() || own.empty()) continue;
    double squared = 0, refRadii = 0, ownRadii = 0;
    for (size_t i = 0; i < own.size(); ++i) {
      for (int k = 0; k < 3; ++k) squared += (ref.points[i][k] - own[i][k]) * (ref.points[i][k] - own[i][k]);
      refRadii += norm(ref.points[i]);
      ownRadii += norm(own[i]);
    }
    if (ownRadii <= 0) continue;
    double ratio = refRadii / ownRadii;
    if (ratio < 1) ratio = 1 / ratio;
    if (std::sqrt(squared / double(own.size())) >= kMaxRms) continue;  // another CAD
    if (ratio >= kMaxRatio) {
      match.mismatched.push_back(ref.name);
      continue;
    }
    for (auto& p : own)
      for (double& v : p) v *= ref.scale;
    match.reference = &ref;
    break;
  }
  return match;
}

std::string DriverResourceDirectory(const std::string& driverName) {
  namespace fs = std::filesystem;
  if (driverName.empty()) return {};
  std::error_code ec;
  const std::string steamvr = platform::SteamVRDirectory();
  if (!steamvr.empty()) {
    const fs::path dir = fs::u8path(steamvr) / "drivers" / fs::u8path(driverName) / "resources";
    if (fs::is_directory(dir, ec)) return Utf8(dir);
  }
  const std::string paths = ReadFile(fs::u8path(platform::OpenVRPathsFile()));
  for (const auto& external : platform::JsonStringList(paths, "external_drivers")) {
    const fs::path root = fs::u8path(external);
    try {
      const json manifest = json::parse(ReadFile(root / "driver.vrdrivermanifest"), nullptr, true, true);
      if (manifest.is_object() && manifest.value("name", "") == driverName) return Utf8(root / "resources");
    } catch (const std::exception&) {
    }
  }
  return {};
}

}  // namespace lh::runtime
