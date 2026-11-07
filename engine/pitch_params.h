#pragma once

#include "engine/autotune.h"
#include "engine/formant_params.h"
#include "engine/octave.h"

// The three pitch cards' settings. Each card's engine (AutotuneFx, OctaveFx, FormantFx) takes its own
// member, and the chain carries the three together because the registry and the panel mapping set them
// by card.

namespace cv {

struct PitchCardParams {
  AutotuneParams autotune;
  OctaveParams octave;
  FormantParams formant;
};

}  // namespace cv
