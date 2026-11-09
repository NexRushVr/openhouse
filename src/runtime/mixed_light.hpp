// Not stock: which light mode a device should be in, in a room with both kinds of base station
// (ai/driver/mixed_stations.md).
//
// In LH1 mode a device reports hits from both kinds, and PulseSorter tells them apart, but only
// for 2.0 rotors it has heard real beam words from. Those come only in LH2 mode, where a 1.0
// station gives nothing. So a device starts in LH2 mode, moves to LH1 mode once it knows the
// rotors it can see, and comes back now and then. Going to LH2 mode costs about half a second
// without light, so routine visits wait for a calm moment, and the same half second is no loss
// to a device that has lost tracking, which LH2 mode handles better on hard swings.
//
// A device can stay in LH2 mode throughout, by kind (MixedLightOptions, from the SteamVR
// settings page). The headset always does.
//
// Live only: a replay's modes are in its recording.
#pragma once

#include "lh/session.hpp"

namespace lh::runtime {

// s in LH1 mode between visits to LH2 mode. A rotor's line, learned on a visit, runs off by about
// 2.5 us (typical) to 9 us (worst tenth) over 45-60 s, against windows of 30 us and up.
constexpr double kVisitEvery = 45;

// Whether a device of this kind reads both kinds of base station.
bool MixesLight(DeviceRole role, const MixedLightOptions& options);

struct MixedLightView {
  int mode = -1;           // 0 = LH1, 1 = LH2, -1 = not sent yet
  double inMode = 0;       // s since this mode was chosen
  double dark = 0;         // s without this mode's kind of light
  double sinceSent = 0;    // s since the mode was last sent
  bool stayLh2 = false;    // never leave LH2 mode
  int readyRotors = 0;     // rotors the sorter can predict
  int learningRotors = 0;  // heard right now, short of words
  int unnamedRotors = 0;   // ready and heard right now, with no station identified for them
  bool tracking = false;   // has a pose from light right now
  double lost = 0;         // s since it last did
  bool calm = false;       // slow enough to go half a second without light
};

struct MixedLightStep {
  int mode = -1;  // the mode to send; -1: send nothing
  const char* why = nullptr;
};

// visitEvery: s in LH1 mode between visits to LH2 mode; 0: only when lost or dark.
MixedLightStep ChooseMixedLight(const MixedLightView& view, double visitEvery);

}  // namespace lh::runtime
