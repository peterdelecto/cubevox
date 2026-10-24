// One ADC brought up for the firmware's own use. The core's analogRead is never called: each call
// re-initialises its ADC and then resets ADC1 and ADC2 together, which would silence the
// power-fail watch on ADC1.

#pragma once

#include <Arduino.h>

// The resolution adcSetup sets (ADC_RESOLUTION_12B).
constexpr int kAdcSetupBits = 12;
constexpr uint32_t kAdcSetupFullScale = (1u << kAdcSetupBits) - 1u;

// Sets up `instance` with one regular channel on software start, 12-bit, the newest result in DR,
// and runs the offset and linearity calibration. The clock is the 40 MHz PLL2 kernel clock
// (board/sysclock) divided by 2, which rev V silicon halves again inside the ADC. `continuous`
// converts without pause once started. Returns null on success, else the step that failed.
const char* adcSetup(ADC_HandleTypeDef& adc, ADC_TypeDef* instance, uint32_t channel, uint32_t sampleTime,
                     bool continuous);
