// Not stock: a 2.0 rotor's timing wobble, measured by the devices in LH2 mode for those in LH1 mode
// (MixedLightOptions::rotorReference, a test; ai/driver/mixed_stations.md, "The rotor's wobble").
//
// In LH1 mode a sorted 2.0 hit's angle is its time since a beam word predicted from the rotor's line, and
// the rotor doesn't keep to a line: its real beam words sit 0.25-0.30 us (0.08-0.09 mrad) off it, wandering
// at 1-8 Hz over 0.12-0.15 s. That is the station's own: two devices in LH2 mode on one rotor at once saw it
// alike (r 1.00, 0.005 mrad apart, replays of 2026-11-11). Devices lying still in LH1 mode, raw angles: a
// frame's sorted hits had 0.07-0.09 mrad in common, and the headset's beam words for that rotor at that
// moment explained it (r 0.91-0.97, slope 1), leaving 0.017-0.032 mrad, as LH2 words' 0.018-0.021. It can't
// be told from its own past (80 % of it 20 ms ahead, 25 % at 60 ms), so it's measured as it happens.
//
// So each device in LH2 mode measures every rotor it sees, word by word, against that rotor's recent line
// (kMemory), and a device in LH1 mode takes that off its sorted hits of the rotor. What is slower (its own
// line against the rotor's, between visits to LH2 mode) is left to the scout and the wobble follower.
#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>

namespace lh::runtime {

class RotorReference {
 public:
  static constexpr double kMemory = 5.0;     // s: how fast the recent line forgets a word
  static constexpr int kWords = 50;          // words in a line before its offsets count
  static constexpr double kOutlier = 5e-6;   // s off the line: a bad word (good ones are 0.3 us)
  static constexpr int kOutliers = 10;       // that many in a row: the line starts over
  static constexpr double kGap = 1.0;        // s without a word: the line starts over
  static constexpr double kNear = 0.03;      // s: how far from a word its offset still holds
  static constexpr double kKeep = 1.0;       // s of offsets kept

  // A real beam word from `source` (a device in LH2 mode) for `base`, at `deviceTime` on the source's
  // clock and `hostTime` on the session's; period: the channel's nominal (s).
  void OnWord(const std::string& source, uint32_t base, double deviceTime, double hostTime, double period) {
    double offset = 0;
    if (!lines_[{source, base}].Add(deviceTime, period, &offset)) return;
    auto& samples = samples_[base][source];
    if (!samples.empty() && hostTime <= samples.back().time) samples.clear();  // a clock that went back
    samples.push_back({hostTime, offset});
    while (!samples.empty() && samples.front().time < hostTime - kKeep) samples.pop_front();
  }

  // The rotor's offset at hostTime (rad, of its sweeps' angle: a sorted hit's angle less this is where it
  // would be with the rotor on its line), from the source with a word nearest that moment. False: no
  // device in LH2 mode measured that rotor within kNear.
  bool Offset(uint32_t base, double hostTime, double* radians) const {
    const auto b = samples_.find(base);
    if (b == samples_.end()) return false;
    double best = kNear, value = 0;
    bool found = false;
    for (const auto& [source, samples] : b->second) {
      if (samples.empty()) continue;
      size_t i = 0;
      while (i < samples.size() && samples[i].time < hostTime) ++i;
      if (i > 0 && i < samples.size()) {  // between two words: their line
        const Sample& p = samples[i - 1];
        const Sample& n = samples[i];
        const double apart = std::min(hostTime - p.time, n.time - hostTime);
        if (n.time - p.time <= 2 * kNear && apart <= best) {
          best = apart;
          value = p.radians + (n.radians - p.radians) * (hostTime - p.time) / (n.time - p.time);
          found = true;
          continue;
        }
      }
      for (const size_t k : {i - 1, i}) {  // past either end (or a gap): the nearest word's
        if (k >= samples.size()) continue;
        const double apart = std::fabs(samples[k].time - hostTime);
        if (apart <= best) { best = apart; value = samples[k].radians; found = true; }
      }
    }
    if (found) *radians = value;
    return found;
  }

  void Forget(const std::string& source) {
    for (auto it = lines_.begin(); it != lines_.end();) it = it->first.first == source ? lines_.erase(it) : std::next(it);
    for (auto& [base, sources] : samples_) sources.erase(source);
  }

 private:
  // Word n at anchor + a + n * b, by least squares fading over kMemory.
  struct Line {
    double anchor = 0, a = 0, b = 0, last = 0;
    double s0 = 0, sn = 0, sy = 0, snn = 0, sny = 0;
    long words = 0;
    int outliers = 0;

    void Start(double t, double period) {
      *this = Line{};
      anchor = last = t;
      b = period;
    }
    // Takes a word; true and its offset (rad) once the line has kWords.
    bool Add(double t, double period, double* radians) {
      if (words == 0 || !(t - last < kGap) || t < last) Start(t, period);
      double n = std::round((t - anchor - a) / b);
      if (n > 2000) {  // move the origin to word n, so the sums stay small
        const double y0 = a + n * b;
        sny = sny - n * sy - y0 * sn + n * y0 * s0;
        snn = snn - 2 * n * sn + n * n * s0;
        sy -= y0 * s0;
        sn -= n * s0;
        anchor += y0;
        a = 0;
        n = 0;
      }
      const double y = t - anchor;
      bool measured = false;
      if (words >= kWords) {
        const double off = y - (a + b * n);
        if (std::fabs(off) > kOutlier) {
          if (++outliers > kOutliers) Start(t, period);
          return false;
        }
        outliers = 0;
        *radians = off * 6.283185307179586 / b;
        measured = true;
      }
      const double fade = std::exp(-(t - last) / kMemory);
      s0 = s0 * fade + 1;
      sn = sn * fade + n;
      sy = sy * fade + y;
      snn = snn * fade + n * n;
      sny = sny * fade + n * y;
      last = t;
      ++words;
      const double det = s0 * snn - sn * sn;
      if (words >= 3 && det > 0) {
        b = (s0 * sny - sn * sy) / det;
        a = (sy - b * sn) / s0;
      }
      return measured;
    }
  };
  struct Sample {
    double time, radians;
  };
  std::map<std::pair<std::string, uint32_t>, Line> lines_;
  std::map<uint32_t, std::map<std::string, std::deque<Sample>>> samples_;
};

}  // namespace lh::runtime
