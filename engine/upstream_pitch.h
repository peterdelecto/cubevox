#pragma once

#include "engine/pitch.h"

// What the chain hands a card that reads the pitch of its input from Autotune's tracker. The chain
// builds it block by block, walking the slots in order. With no Autotune ahead of the card `result` is
// null and the card runs a tracker of its own.

namespace cv {

struct UpstreamPitch {
  // Autotune's tracker result. It describes the audio that entered Autotune's slot.
  const PitchResult* result = nullptr;

  // The semitones the audio reaching the card sits above that pitch. The card divides the tracker's
  // period by 2^(shiftSemis / 12) to get the period it hears.
  float shiftSemis = 0.0f;

  // The delay, in samples, the slots between Autotune's input and the card put on the audio.
  float latency = 0.0f;
};

}  // namespace cv
