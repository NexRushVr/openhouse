#include <gtest/gtest.h>

#include <string>

#include "runtime/mixed_light.hpp"

using lh::runtime::ChooseMixedLight;
using lh::runtime::MixedLightStep;
using lh::runtime::MixedLightView;

namespace {

constexpr double kVisits = 45;

// A device in LH2 mode, tracking, knowing its rotors.
MixedLightView InLh2(double inMode) {
  MixedLightView v;
  v.mode = 1;
  v.inMode = inMode;
  v.sinceSent = inMode;
  v.readyRotors = 3;
  v.tracking = true;
  v.calm = true;
  return v;
}

// A device in LH1 mode, tracking, with light.
MixedLightView InLh1(double inMode) {
  MixedLightView v = InLh2(inMode);
  v.mode = 0;
  return v;
}

int Mode(const MixedLightView& v) { return ChooseMixedLight(v, kVisits).mode; }

}  // namespace

TEST(MixedLight, StartsInLh2) {
  MixedLightView v;
  EXPECT_EQ(Mode(v), 1);
  v.stayLh2 = true;
  EXPECT_EQ(Mode(v), 1);
}

TEST(MixedLight, LeavesLh2OnceItKnowsItsRotors) {
  EXPECT_EQ(Mode(InLh2(1.0)), -1);  // however ready, a visit lasts long enough to hear a new rotor
  EXPECT_EQ(Mode(InLh2(2.5)), 0);
}

TEST(MixedLight, WaitsForARotorStillBeingLearned) {
  MixedLightView v = InLh2(5.0);
  v.learningRotors = 1;
  EXPECT_EQ(Mode(v), -1);
  v.inMode = 8.0;
  EXPECT_EQ(Mode(v), 0);  // but not for ever
}

TEST(MixedLight, GivesAStationTimeToBeIdentified) {
  MixedLightView v = InLh2(20.0);
  v.unnamedRotors = 1;
  EXPECT_EQ(Mode(v), -1);
  v.inMode = 25.0;
  EXPECT_EQ(Mode(v), 0);
}

TEST(MixedLight, WithNoPoseStaysLongerToIdentifyAStation) {
  MixedLightView v = InLh2(59.0);
  v.unnamedRotors = 1;
  v.tracking = false;
  EXPECT_EQ(Mode(v), -1);
  v.inMode = 60.0;
  EXPECT_EQ(Mode(v), 0);
}

TEST(MixedLight, WaitsForTrackingToComeBack) {
  MixedLightView v = InLh2(5.0);
  v.tracking = false;
  EXPECT_EQ(Mode(v), -1);
  v.inMode = 8.0;
  EXPECT_EQ(Mode(v), 0);
}

TEST(MixedLight, StationsWakingAreWaitedFor) {
  MixedLightView v = InLh2(29.0);
  v.readyRotors = 0;
  v.tracking = false;
  v.dark = 29.0;
  EXPECT_NE(Mode(v), 0);  // LH1 mode now would see the 1.0 station first
  v.dark = v.inMode = 30.0;
  EXPECT_EQ(Mode(v), 0);
}

TEST(MixedLight, AVisitInDarknessEndsEarly) {
  MixedLightView v = InLh2(4.0);
  v.tracking = false;
  v.dark = 4.0;
  EXPECT_EQ(Mode(v), 0);
  v.inMode = 2.0;  // the switch itself takes half a second
  v.dark = 2.0;
  EXPECT_EQ(Mode(v), -1);
}

TEST(MixedLight, VisitsWhenCalm) {
  EXPECT_EQ(Mode(InLh1(44.0)), -1);
  EXPECT_EQ(Mode(InLh1(45.0)), 1);
}

TEST(MixedLight, AVisitWaitsOutMovementButNotForEver) {
  MixedLightView v = InLh1(60.0);
  v.calm = false;
  EXPECT_EQ(Mode(v), -1);
  v.inMode = 90.0;
  EXPECT_EQ(Mode(v), 1);
}

TEST(MixedLight, VisitsCanBeTurnedOff) {
  EXPECT_EQ(ChooseMixedLight(InLh1(1000.0), 0).mode, -1);
  MixedLightView lost = InLh1(1000.0);
  lost.lost = 2.0;
  EXPECT_EQ(ChooseMixedLight(lost, 0).mode, 1);  // going for help still happens
}

TEST(MixedLight, LostInLh1GoesToLh2) {
  MixedLightView v = InLh1(10.0);
  v.tracking = false;
  v.lost = 0.5;
  EXPECT_EQ(Mode(v), -1);  // long enough to recover where it is
  v.lost = 1.0;
  EXPECT_EQ(Mode(v), 1);
  v.inMode = 2.0;
  EXPECT_EQ(Mode(v), -1);  // not straight back after arriving lost
}

TEST(MixedLight, NoLightInLh1GoesToLh2) {
  MixedLightView v = InLh1(10.0);
  v.dark = 5.0;
  EXPECT_EQ(Mode(v), 1);
}

TEST(MixedLight, LooksForRotorsAgainWhenItKnowsNone) {
  MixedLightView v = InLh1(14.0);
  v.readyRotors = 0;
  EXPECT_EQ(Mode(v), -1);
  v.inMode = 15.0;
  EXPECT_EQ(Mode(v), 1);
}

TEST(MixedLight, ResendsItsModeWhileDark) {
  MixedLightView v = InLh2(10.0);
  v.readyRotors = 0;
  v.tracking = false;
  v.dark = 10.0;
  v.sinceSent = 4.0;
  EXPECT_EQ(Mode(v), -1);
  v.sinceSent = 5.0;
  const MixedLightStep step = ChooseMixedLight(v, kVisits);
  EXPECT_EQ(step.mode, 1);
  EXPECT_EQ(std::string(step.why), "re-send");
}

TEST(MixedLight, AHeadsetStaysInLh2) {
  MixedLightView v = InLh2(1000.0);
  v.stayLh2 = true;
  EXPECT_EQ(Mode(v), -1);
  v.dark = 5.0;
  v.sinceSent = 5.0;
  EXPECT_EQ(Mode(v), 1);  // re-sent, the same mode
  v.mode = 0;
  v.dark = 0;
  EXPECT_EQ(Mode(v), 1);
  v.mode = 1;
  v.tracking = false;
  v.lost = 100;
  EXPECT_EQ(Mode(v), -1);
}

TEST(MixedLight, EachKindOfDeviceHasItsOwnSwitch) {
  using lh::DeviceRole;
  using lh::runtime::MixesLight;
  lh::MixedLightOptions options;  // the defaults: controllers and trackers, never the headset
  EXPECT_FALSE(options.enabled);
  EXPECT_TRUE(MixesLight(DeviceRole::Controller, options));
  EXPECT_FALSE(MixesLight(DeviceRole::Hmd, options));
  EXPECT_TRUE(MixesLight(DeviceRole::Tracker, options));
  EXPECT_FALSE(MixesLight(DeviceRole::BaseStation, options));
  options.controllers = false;
  EXPECT_FALSE(MixesLight(DeviceRole::Controller, options));
  EXPECT_TRUE(MixesLight(DeviceRole::Tracker, options));
  options.trackers = false;
  EXPECT_FALSE(MixesLight(DeviceRole::Tracker, options));
}

TEST(MixedLight, SwitchedOffInLh1GoesToLh2AtOnce) {
  MixedLightView v = InLh1(10.0);
  v.calm = false;  // even mid-swing: it was asked for
  v.stayLh2 = true;
  const MixedLightStep step = ChooseMixedLight(v, kVisits);
  EXPECT_EQ(step.mode, 1);
  EXPECT_EQ(std::string(step.why), "stays in LH2");
}
