// RotorWobble: the offset a 2.0 rotor's sorted hits share, followed apart from the pose.
#include <gtest/gtest.h>

#include <cmath>

#include "tracking/rotor_wobble.hpp"

using lh::direct::RotorWobble;

namespace {
constexpr double kSpread = 0.3e-3, kMemory = 0.2;  // rad, s
constexpr double kNoise = 0.15e-3 * 0.15e-3;       // a frame of five hits at 0.33 mrad each
}  // namespace

TEST(RotorWobble, StartsAtZeroAsUnsureAsItsSpread) {
  RotorWobble w;
  w.Predict(10.0, kSpread, kMemory);
  EXPECT_EQ(w.value, 0);
  EXPECT_DOUBLE_EQ(w.variance, kSpread * kSpread);
}

TEST(RotorWobble, TakesWhatAWellKnownPoseCannotExplain) {
  RotorWobble w;
  w.Predict(10.0, kSpread, kMemory);
  // The pose is known to 0.02 mrad: nearly all of a 0.2 mrad offset is the rotor's.
  ASSERT_TRUE(w.Update(0.2e-3, 0.02e-3 * 0.02e-3, kNoise));
  EXPECT_GT(w.value, 0.15e-3);
  EXPECT_LT(w.value, 0.2e-3);
  EXPECT_LT(w.variance, kSpread * kSpread);
}

TEST(RotorWobble, LeavesItToThePoseWhenThePoseIsUnsure) {
  RotorWobble w;
  w.Predict(10.0, kSpread, kMemory);
  // Just reacquired, or swinging: the pose could be 5 mrad off, so the light should correct it.
  ASSERT_TRUE(w.Update(0.2e-3, 5e-3 * 5e-3, kNoise));
  EXPECT_LT(w.value, 0.002e-3);
}

TEST(RotorWobble, FollowsAWanderingRotorFrameByFrame) {
  RotorWobble w;
  double worst = 0;
  for (int i = 0; i < 500; ++i) {
    const double t = 10.0 + 0.01 * i;
    const double truth = 0.15e-3 * std::sin(2 * 3.141592653589793 * 2.5 * t);  // 2.5 Hz, as the rotors do
    w.Predict(t, kSpread, kMemory);
    ASSERT_TRUE(w.Update(truth, 0.03e-3 * 0.03e-3, kNoise));
    if (i > 50) worst = std::max(worst, std::fabs(truth - w.value));
  }
  EXPECT_LT(worst, 0.05e-3);  // a third of it left at most
}

TEST(RotorWobble, FadesWithoutFramesAndStartsOverAfterAGap) {
  RotorWobble w;
  w.Predict(10.0, kSpread, kMemory);
  ASSERT_TRUE(w.Update(0.2e-3, 0.02e-3 * 0.02e-3, kNoise));
  const double learned = w.value, sure = w.variance;
  w.Predict(10.2, kSpread, kMemory);  // one memory on
  EXPECT_NEAR(w.value, learned * std::exp(-1.0), 1e-9);
  EXPECT_GT(w.variance, sure);
  EXPECT_LT(w.variance, kSpread * kSpread);
  w.Predict(12.0, kSpread, kMemory);
  EXPECT_EQ(w.value, 0);
  EXPECT_DOUBLE_EQ(w.variance, kSpread * kSpread);
}

TEST(RotorWobble, AFrameFarOffIsNotAWobble) {
  RotorWobble w;
  w.Predict(10.0, kSpread, kMemory);
  // 3 mrad off with the pose known to 0.02 mrad: another station's sweep, not this rotor wandering.
  EXPECT_FALSE(w.Update(3e-3, 0.02e-3 * 0.02e-3, kNoise));
  EXPECT_EQ(w.value, 0);
  EXPECT_DOUBLE_EQ(w.variance, kSpread * kSpread);
}
