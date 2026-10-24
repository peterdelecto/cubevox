#include "board/adc_setup.h"

namespace {

void fillInit(ADC_InitTypeDef& init, bool continuous) {
  init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV2;
  init.Resolution = ADC_RESOLUTION_12B;  // kAdcSetupBits
  init.ScanConvMode = ADC_SCAN_DISABLE;
  init.EOCSelection = ADC_EOC_SINGLE_CONV;
  init.LowPowerAutoWait = DISABLE;
  init.ContinuousConvMode = continuous ? ENABLE : DISABLE;
  init.NbrOfConversion = 1;
  init.DiscontinuousConvMode = DISABLE;
  init.ExternalTrigConv = ADC_SOFTWARE_START;
  init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  init.OversamplingMode = DISABLE;
}

}  // namespace

const char* adcSetup(ADC_HandleTypeDef& adc, ADC_TypeDef* instance, uint32_t channel, uint32_t sampleTime,
                     bool continuous) {
  adc = ADC_HandleTypeDef{};
  adc.Instance = instance;
  fillInit(adc.Init, continuous);
  if (HAL_ADC_Init(&adc) != HAL_OK) return "init failed";
  if (HAL_ADCEx_Calibration_Start(&adc, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK) {
    return "calibration failed";
  }
  ADC_ChannelConfTypeDef ch = {};
  ch.Channel = channel;
  ch.Rank = ADC_REGULAR_RANK_1;
  ch.SamplingTime = sampleTime;
  ch.SingleDiff = ADC_SINGLE_ENDED;
  ch.OffsetNumber = ADC_OFFSET_NONE;
  if (HAL_ADC_ConfigChannel(&adc, &ch) != HAL_OK) return "channel setup failed";
  return nullptr;
}
