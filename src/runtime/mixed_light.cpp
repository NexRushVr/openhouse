#include "mixed_light.hpp"

namespace lh::runtime {

namespace {
constexpr double kResend = 5.0;       // s dark before sending the mode again: it may have been lost
constexpr double kMinVisit = 2.5;     // s in LH2 mode: 100 beam words take about 2 s
constexpr double kMaxVisit = 8.0;     // s, when a rotor is still learning or tracking hasn't come back
constexpr double kMaxNaming = 25.0;   // s: identifying a station takes about 9 s of steady light
constexpr double kMaxNamingLost = 60.0;  // s with no pose: leaving would start the 9 s over, for little
constexpr double kWakeDark = 30.0;    // s dark in LH2 mode knowing no rotors: stations may be waking
constexpr double kVisitDark = 4.0;    // s dark in LH2 mode knowing rotors: nothing more to learn
constexpr double kLh1Dark = 5.0;      // s with no pulses at all in LH1 mode
constexpr double kNoRotorsRetry = 15.0;  // s in LH1 mode knowing no rotors, before looking again
constexpr double kLostBeforeHelp = 1.0;  // s lost in LH1 mode: leaves time to recover where it is
constexpr double kMinLh1 = 3.0;       // s in LH1 mode before going back because lost
constexpr double kOverdueFactor = 2;  // a visit goes ahead after this many intervals, calm or not
}  // namespace

bool MixesLight(DeviceRole role, const MixedLightOptions& options) {
  switch (role) {
    case DeviceRole::Controller: return options.controllers;
    case DeviceRole::Tracker: return options.trackers;
    case DeviceRole::Hmd:          // the stock driver puts it back in LH2 mode (mixed_stations.md)
    case DeviceRole::BaseStation: return false;
  }
  return false;
}

MixedLightStep ChooseMixedLight(const MixedLightView& v, double visitEvery) {
  if (v.mode < 0) return {1, "start in LH2"};
  const bool resend = v.dark >= kResend && v.sinceSent >= kResend;
  if (v.stayLh2) {
    if (v.mode != 1) return {1, "stays in LH2"};
    return resend ? MixedLightStep{1, "re-send"} : MixedLightStep{};
  }
  if (v.mode == 1) {
    if (v.readyRotors == 0) {
      if (v.dark >= kWakeDark) return {0, "no 2.0 light"};
    } else {
      const bool done = v.learningRotors == 0 && v.unnamedRotors == 0 && v.tracking;
      if (done && v.inMode >= kMinVisit) return {0, "rotors ready"};
      if (v.inMode >= kMinVisit && v.dark >= kVisitDark) return {0, "no 2.0 light"};
      const double naming = v.tracking ? kMaxNaming : kMaxNamingLost;
      if (v.inMode >= (v.unnamedRotors > 0 ? naming : kMaxVisit)) return {0, "visit over"};
    }
    return resend ? MixedLightStep{1, "re-send"} : MixedLightStep{};
  }
  if (v.dark >= kLh1Dark) return {1, "no light"};
  if (v.readyRotors == 0 && v.inMode >= kNoRotorsRetry) return {1, "no rotors yet"};
  if (v.lost >= kLostBeforeHelp && v.inMode >= kMinLh1) return {1, "lost"};
  if (visitEvery > 0 && v.inMode >= visitEvery && (v.calm || v.inMode >= visitEvery * kOverdueFactor)) return {1, "visit"};
  return {};
}

}  // namespace lh::runtime
