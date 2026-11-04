// Everything the audio block needs from the panel and menu. Built by the main loop,
// published whole to the audio interrupt (see audioPublish).

#pragma once

#include "engine/bloom.h"
#include "engine/crowd.h"
#include "engine/distortion.h"
#include "engine/gate.h"
#include "engine/growl.h"
#include "engine/lofi.h"
#include "engine/pitch_fx.h"
#include "engine/polish.h"
#include "engine/polish_comp.h"
#include "engine/reverb.h"
#include "engine/slapback.h"
#include "engine/unison.h"

struct ChainParams {
  cv::GateParams inputGate;
  cv::PitchFxParams pitchFx;  // Autotune, Octave and the Formant card (pitchFx.formant)
  cv::CrowdParams crowd;      // its toggle also sets pitchFx.crowdOn
  cv::UnisonParams unison;
  cv::SlapbackParams slapback;
  cv::DistortionParams distortion;
  cv::GrowlParams growl;  // its toggle also sets pitchFx.growlOn
  cv::GateParams gate;
  cv::LofiParams lofi;
  cv::BloomParams bloom;  // keyFromGate is set by placing the Input gate; runBloom passes the gate's state
  cv::ReverbParams reverb;
  cv::PolishCompParams comp;  // the chain tail: comp, then the back half of eq
  cv::PolishParams eq;        // its front half runs ahead of slot 1

  ChainParams();
};
