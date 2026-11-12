// Not stock: how far a 2.0 rotor's hits, picked out of LH1 pulses, sit off the pose all alike
// (ai/driver/mixed_stations.md, "The rotor's wobble").
//
// A sorted hit's angle is its time since a beam word predicted from the rotor's line, but the rotor
// doesn't keep to a line: its real beam words wander about it by 0.25-0.3 us rms (0.09 mrad), over
// 0.1-0.5 s (most of it at 1-8 Hz), and a little more comes from elsewhere. Every sensor of both
// planes is off by the same angle then: on a tracker lying still, frames' means wandered by
// 0.09-0.17 mrad while the sensors within a frame kept to 0.02-0.07 mrad of one another. An LH2 word
// carries its own angle and has none of this. Weighted as noise of its own per hit, the wander went
// into the pose, a 2-3 Hz sway of 0.2-0.3 mm.
//
// So each rotor's offset is followed, as a value fading to zero, from what a frame's hits have in
// common against the pose (three or more of them), as far as the pose is known better than the
// offset, and taken out of the hits before the filter has them. Their weights stay as they were
// (LighthouseDirect::FollowRotorWobble).
//
// It follows more than the wobble: gaining about 0.28 a frame against a fade of 5-10 % a frame, it
// takes about nine tenths of an offset that stays, and such offsets are there (a rotor's line running
// off between visits to LH2 mode: its sorted hits sat 0.2-0.5 mrad off the pose typically, and -1.7
// mrad for seconds on end). The pose's variance here is the filter's own, much of which it has from
// the same rotor's hits.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace lh::direct {

// The hits of a frame that say what the frame has in common: those within kWobbleOutlier of the median of
// how far each sits off the pose (`off`, rad); the rest is another station's hit or a reflection. Empty
// when fewer than kWobbleMinHits are left. It was two, but two sensors agree by chance, and where a device
// has only 2-3 sensors a frame of a rotor the light is poor altogether (hits 1-2 mrad apart, frames' means
// swinging +-1 mrad from one to the next): followed there, a 2018 tracker lying still slid 1 m and was
// lost for 2.6 s. With three it didn't, with 1.0 hits at 0.49, 0.5 or 0.501 mrad; without the follower it
// does at 0.49 and 0.52 mrad, not at 0.5.
constexpr double kWobbleOutlier = 0.5e-3;  // rad
constexpr int kWobbleMinHits = 3;
inline std::vector<size_t> HitsInCommon(const std::vector<double>& off) {
  std::vector<size_t> hits;
  if (off.empty()) return hits;
  std::vector<double> ordered = off;
  std::nth_element(ordered.begin(), ordered.begin() + std::ptrdiff_t(ordered.size() / 2), ordered.end());
  const double median = ordered[ordered.size() / 2];
  for (size_t i = 0; i < off.size(); ++i)
    if (!(std::fabs(off[i] - median) > kWobbleOutlier)) hits.push_back(i);
  if (hits.size() < size_t(kWobbleMinHits)) hits.clear();
  return hits;
}

struct RotorWobble {
  double value = 0;     // rad, added to every sorted hit's angle of this rotor now
  double variance = 0;  // rad^2
  double time = -1e300;

  // Carried to `t`: fading towards zero +- spread (rad) over `memory` s; starting over after a second
  // without a frame.
  void Predict(double t, double spread, double memory) {
    const double gap = t - time;
    if (!(gap < 1.0) || gap < -0.1) {
      value = 0;
      variance = spread * spread;
    } else if (gap > 0) {
      const double keep = std::exp(-gap / memory);
      value *= keep;
      variance = keep * keep * variance + (1 - keep * keep) * spread * spread;
    }
    time = t;
  }

  // A frame's hits sit `offset` rad off the pose on average; poseVariance: how well the pose knows
  // where they should be, noiseVariance: what their own scatter leaves of that average. False, and
  // nothing learned, when that is more than 4 sigma from what was expected: not a wobble.
  bool Update(double offset, double poseVariance, double noiseVariance) {
    const double total = variance + poseVariance + noiseVariance;
    const double innovation = offset - value;
    if (!(total > 0) || innovation * innovation > 16 * total) return false;
    const double gain = variance / total;
    value += gain * innovation;
    variance *= 1 - gain;
    return true;
  }
};

}  // namespace lh::direct
