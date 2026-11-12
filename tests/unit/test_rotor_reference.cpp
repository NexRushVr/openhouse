#include <gtest/gtest.h>

#include <cmath>

#include "runtime/rotor_reference.hpp"

using lh::runtime::RotorReference;

namespace {

constexpr double kPeriod = 959000 / 48e6;  // channel 0's
constexpr double kPi = 3.141592653589793;
constexpr uint32_t kBase = 0x1B49A56F;

// A rotor 0.3 us off its line at 3 Hz, as real ones wander; a word every revolution, on a device clock
// `skew` s from the session's.
double Wobble(double t) { return 0.3e-6 * std::sin(2 * kPi * 3 * t); }

void Feed(RotorReference* ref, const char* source, double from, int words, double skew = 0, double phase = 0) {
  for (int n = 0; n < words; ++n) {
    const double host = from + n * kPeriod;
    const double w = Wobble(host + phase);
    ref->OnWord(source, kBase, host + skew + w, host + w, kPeriod);
  }
}

}  // namespace

TEST(RotorReference, MeasuresTheRotorsOffsetFromItsLine) {
  RotorReference ref;
  Feed(&ref, "LHR-HMD", 10.0, 600, 123.4);
  // A sorted hit 5 ms after the last word: its angle is off by the rotor's offset there, so much as a
  // share of the revolution.
  const double last = 10.0 + 599 * kPeriod;
  for (const double at : {last - 0.101, last - 0.05, last + 0.005}) {
    double radians = 0;
    ASSERT_TRUE(ref.Offset(kBase, at, &radians));
    const double expected = 2 * kPi * Wobble(at) / kPeriod;
    EXPECT_NEAR(radians, expected, 0.2 * 2 * kPi * 0.3e-6 / kPeriod) << at;
  }
}

TEST(RotorReference, SaysNothingWithoutAWordNearby) {
  RotorReference ref;
  double radians = 0;
  EXPECT_FALSE(ref.Offset(kBase, 10.0, &radians));
  Feed(&ref, "LHR-HMD", 10.0, RotorReference::kWords);  // the line isn't ready yet
  EXPECT_FALSE(ref.Offset(kBase, 10.0 + (RotorReference::kWords - 1) * kPeriod, &radians));
  Feed(&ref, "LHR-HMD", 10.0 + RotorReference::kWords * kPeriod, 100);
  const double last = 10.0 + (RotorReference::kWords + 99) * kPeriod;
  EXPECT_TRUE(ref.Offset(kBase, last + 0.02, &radians));
  EXPECT_FALSE(ref.Offset(kBase, last + RotorReference::kNear + 0.01, &radians));  // the rotor out of view
  EXPECT_FALSE(ref.Offset(0x614DFA50, last, &radians));                            // another rotor
}

TEST(RotorReference, LeavesOutABadWord) {
  RotorReference ref;
  Feed(&ref, "LHR-HMD", 10.0, 300);
  const double t = 10.0 + 300 * kPeriod;
  ref.OnWord("LHR-HMD", kBase, t + 50e-6, t + 50e-6, kPeriod);  // a reflection, 50 us off
  double radians = 0;
  ASSERT_TRUE(ref.Offset(kBase, t, &radians));
  EXPECT_NEAR(radians, 2 * kPi * Wobble(t - kPeriod) / kPeriod, 0.2 * 2 * kPi * 0.3e-6 / kPeriod);
  // The line goes on as before: the next good word is measured.
  Feed(&ref, "LHR-HMD", t + kPeriod, 1);
  ASSERT_TRUE(ref.Offset(kBase, t + kPeriod, &radians));
  EXPECT_NEAR(radians, 2 * kPi * Wobble(t + kPeriod) / kPeriod, 0.2 * 2 * kPi * 0.3e-6 / kPeriod);
}

TEST(RotorReference, TakesTheSourceWithAWordNearest) {
  RotorReference ref;
  Feed(&ref, "LHR-HMD", 10.0, 300);
  // A controller in LH2 mode sees the same rotor until 0.1 s later, so its words are the nearer ones then.
  Feed(&ref, "LHR-KNUCKLES", 10.0, 305);
  const double t = 10.0 + 304 * kPeriod;
  double radians = 0;
  ASSERT_TRUE(ref.Offset(kBase, t, &radians));
  EXPECT_NEAR(radians, 2 * kPi * Wobble(t) / kPeriod, 0.2 * 2 * kPi * 0.3e-6 / kPeriod);
  ref.Forget("LHR-KNUCKLES");
  EXPECT_FALSE(ref.Offset(kBase, t, &radians));  // the headset's last word is 0.1 s before
}
