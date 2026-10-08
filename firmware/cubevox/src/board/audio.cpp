#include "board/audio.h"

#include <Arduino.h>
#include <math.h>

#include <atomic>

#include "engine/common.h"
#include "h7_block_mem.h"
#include "core/registry.h"

// Clocks (hardware spec item 10 and note F4): HSE 25 MHz / 25 * 196.608 = 196.608 MHz VCO,
// / 4 = 49.152 MHz SAI kernel clock, MCKDIV 4 gives MCLK = 256 fs = 12.288 MHz.
constexpr uint32_t kPll3M = 25;
constexpr uint32_t kPll3N = 196;
constexpr uint32_t kPll3FracN = 4981;
constexpr uint32_t kPll3P = 4;
constexpr uint32_t kSaiKernelHz = 49152000;
constexpr uint32_t kSaiKernelToleranceHz = 5000;

constexpr int kFramesPerHalf = cv::kBlock;
constexpr int kWordsPerHalf = kFramesPerHalf * 2;  // L and R per frame
constexpr int kWordsPerRing = kWordsPerHalf * 2;

constexpr float kInt32Scale = 2147483648.0f;
constexpr float kInt32Max = 2147483520.0f;  // largest float below 2^31
constexpr uint32_t kDmaIrqPriority = 2;
constexpr uint32_t kBlockIrqPriority = 15;  // lowest, so the main loop and DMA both preempt it

// Rings live in RAM_D2 and are marked non-cacheable by h7_block_mem_mpu_init().
H7_DMA_MEM static uint32_t gTxRing[kWordsPerRing];
H7_DMA_MEM static uint32_t gRxRing[kWordsPerRing];

static SAI_HandleTypeDef gSaiTx;
static SAI_HandleTypeDef gSaiRx;
static DMA_HandleTypeDef gDmaTx;
static DMA_HandleTypeDef gDmaRx;

namespace {

// Engine objects, constructed in reset state.
struct Chain {
  cv::Gate inputGate;
  cv::PitchFx pitchFx;
  cv::Unison unison;
  cv::Slapback slapback;
  cv::Distortion distortion;
  cv::Gate gate;
  cv::Reverb reverb;
  cv::Polish polish;
};

Chain gChain;
ChainParams gParams[2];
std::atomic<int> gPublished{0};
int gWriteIndex = 1;

const char* gError = "audioInit() has not run";
volatile uint8_t gReadyMask = 0;  // bit h set when RX half h is waiting
uint8_t gNextHalf = 0;
volatile AudioStats gStats;
volatile uint64_t gSumCycles = 0;   // since the last peaks reset
volatile uint32_t gSumBlocks = 0;
volatile uint64_t gSlotCycles[kSlotCount + 1] = {};

// Runs one card. Returns false when it wrote nothing, so the caller keeps its buffers.
// Pitch cards share one PitchFx call, run at the first pitch card's slot.
bool runStage(Card c, const float* in, float* out, int n, const ChainParams& p, bool& pitchDone) {
  switch (c) {
    case Card::Empty: return false;
    case Card::InputGate: gChain.inputGate.process(in, out, n, p.inputGate); return true;
    case Card::Autotune:
    case Card::Octave:
      if (pitchDone) return false;
      pitchDone = true;
      gChain.pitchFx.process(in, out, n, p.pitchFx);
      return true;
    case Card::Unison: gChain.unison.process(in, out, n, p.unison); return true;
    case Card::Slapback: gChain.slapback.process(in, out, n, p.slapback); return true;
    case Card::Distortion: gChain.distortion.process(in, out, n, p.distortion); return true;
    case Card::Gate: gChain.gate.process(in, out, n, p.gate); return true;
    case Card::ReverbSpring:
    case Card::ReverbChasm: gChain.reverb.process(in, out, n, p.reverb); return true;
  }
  return false;
}

// Slot order, then the fixed EQ. Result lands in `a`.
void runChain(float* a, float* b, int n, const ChainParams& p) {
  bool pitchDone = false;
  for (int s = 0; s < kSlotCount; ++s) {
    const uint32_t t0 = DWT->CYCCNT;
    const bool wrote = runStage(slotCard(s), a, b, n, p, pitchDone);
    gSlotCycles[s] = gSlotCycles[s] + (DWT->CYCCNT - t0);
    if (wrote) {
      float* t = a;
      a = b;
      b = t;
    }
  }
  const uint32_t t0 = DWT->CYCCNT;
  gChain.polish.process(a, b, n, p.eq);
  for (int i = 0; i < n; ++i) a[i] = b[i];
  gSlotCycles[kSlotCount] = gSlotCycles[kSlotCount] + (DWT->CYCCNT - t0);
}

void processHalf(int half) {
  const uint32_t* rx = gRxRing + half * kWordsPerHalf;
  uint32_t* tx = gTxRing + half * kWordsPerHalf;
  const ChainParams& p = gParams[gPublished.load(std::memory_order_acquire)];

  const float inGain = powf(10.0f, p.inputGainDb / 20.0f) / kInt32Scale;
  float a[cv::kBlock];
  float b[cv::kBlock];
  for (int i = 0; i < kFramesPerHalf; ++i) {
    a[i] = static_cast<float>(static_cast<int32_t>(rx[2 * i])) * inGain;  // left only
  }
  runChain(a, b, kFramesPerHalf, p);

  const float gain = powf(10.0f, p.outputGainDb / 20.0f);
  for (int i = 0; i < kFramesPerHalf; ++i) {
    float v = a[i] * gain;
    v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    const uint32_t word = static_cast<uint32_t>(static_cast<int32_t>(v * kInt32Max));
    tx[2 * i] = word;
    tx[2 * i + 1] = word;
  }
}

void enableCycleCounter() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

bool initClock() {
  RCC_PeriphCLKInitTypeDef c = {};
  c.PeriphClockSelection = RCC_PERIPHCLK_SAI1;
  c.Sai1ClockSelection = RCC_SAI1CLKSOURCE_PLL3;
  c.PLL3.PLL3M = kPll3M;
  c.PLL3.PLL3N = kPll3N;
  c.PLL3.PLL3FRACN = kPll3FracN;
  c.PLL3.PLL3P = kPll3P;
  c.PLL3.PLL3Q = 4;
  c.PLL3.PLL3R = 4;
  c.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_0;  // 1..2 MHz reference
  c.PLL3.PLL3VCOSEL = RCC_PLL3VCOMEDIUM;
  if (HAL_RCCEx_PeriphCLKConfig(&c) != HAL_OK) return false;

  const uint32_t hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SAI1);
  return hz + kSaiKernelToleranceHz >= kSaiKernelHz && hz <= kSaiKernelHz + kSaiKernelToleranceHz;
}

void initGpio() {
  __HAL_RCC_GPIOE_CLK_ENABLE();
  GPIO_InitTypeDef g = {};
  g.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;  // MCLK 12.288 MHz, BCLK 3.072 MHz
  g.Alternate = GPIO_AF6_SAI1;
  HAL_GPIO_Init(GPIOE, &g);
}

bool initDma(DMA_HandleTypeDef& dma, DMA_Stream_TypeDef* stream, uint32_t request, uint32_t direction,
             IRQn_Type irq) {
  dma.Instance = stream;
  dma.Init.Request = request;
  dma.Init.Direction = direction;
  dma.Init.PeriphInc = DMA_PINC_DISABLE;
  dma.Init.MemInc = DMA_MINC_ENABLE;
  dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  dma.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
  dma.Init.Mode = DMA_CIRCULAR;
  dma.Init.Priority = DMA_PRIORITY_HIGH;
  dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&dma) != HAL_OK) return false;
  HAL_NVIC_SetPriority(irq, kDmaIrqPriority, 0);
  HAL_NVIC_EnableIRQ(irq);
  return true;
}

// Both codecs use I2S standard framing (PCM5102A FMT low, PCM1808 strapped I2S 24-bit).
bool initSai() {
  __HAL_RCC_SAI1_CLK_ENABLE();

  gSaiTx.Instance = SAI1_Block_A;
  gSaiTx.Init.AudioMode = SAI_MODEMASTER_TX;
  gSaiTx.Init.Synchro = SAI_ASYNCHRONOUS;
  gSaiTx.Init.SynchroExt = SAI_SYNCEXT_DISABLE;
  gSaiTx.Init.MckOutput = SAI_MCK_OUTPUT_ENABLE;
  gSaiTx.Init.OutputDrive = SAI_OUTPUTDRIVE_ENABLE;
  gSaiTx.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE;
  gSaiTx.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_HF;
  gSaiTx.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_48K;
  gSaiTx.Init.MckOverSampling = SAI_MCK_OVERSAMPLING_DISABLE;
  gSaiTx.Init.MonoStereoMode = SAI_STEREOMODE;
  gSaiTx.Init.CompandingMode = SAI_NOCOMPANDING;
  gSaiTx.Init.TriState = SAI_OUTPUT_NOTRELEASED;
  if (HAL_SAI_InitProtocol(&gSaiTx, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_32BIT, 2) != HAL_OK) return false;

  // Block B takes BCLK and LRCLK from block A inside the peripheral.
  gSaiRx = gSaiTx;
  gSaiRx.Instance = SAI1_Block_B;
  gSaiRx.Init.AudioMode = SAI_MODESLAVE_RX;
  gSaiRx.Init.Synchro = SAI_SYNCHRONOUS;
  gSaiRx.Init.MckOutput = SAI_MCK_OUTPUT_DISABLE;
  return HAL_SAI_InitProtocol(&gSaiRx, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_32BIT, 2) == HAL_OK;
}

// A spare peripheral vector serves as the software interrupt that runs the audio block.
void initBlockInterrupt() {
  NVIC_SetPriority(SWPMI1_IRQn, kBlockIrqPriority);
  NVIC_ClearPendingIRQ(SWPMI1_IRQn);
  NVIC_EnableIRQ(SWPMI1_IRQn);
}

void onRxHalf(int half) {
  const uint8_t bit = static_cast<uint8_t>(1u << half);
  if (gReadyMask & bit) gStats.overruns = gStats.overruns + 1;
  gReadyMask = gReadyMask | bit;
  NVIC_SetPendingIRQ(SWPMI1_IRQn);
}

}  // namespace

extern "C" {

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef* hsai) {
  if (hsai == &gSaiRx) onRxHalf(0);
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef* hsai) {
  if (hsai == &gSaiRx) onRxHalf(1);
}

// TX needs no work: block processing is paced by RX, which is synchronous with TX, and each
// pass rewrites the TX half the DMA just finished.
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef*) {}
void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef*) {}

void DMA1_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&gDmaTx); }
void DMA1_Stream1_IRQHandler(void) { HAL_DMA_IRQHandler(&gDmaRx); }

void SWPMI1_IRQHandler(void) {
  while (gReadyMask & (1u << gNextHalf)) {
    const uint32_t start = DWT->CYCCNT;
    processHalf(gNextHalf);
    const uint32_t cycles = DWT->CYCCNT - start;

    __disable_irq();
    gReadyMask = gReadyMask & static_cast<uint8_t>(~(1u << gNextHalf));
    __enable_irq();
    gNextHalf ^= 1u;
    gStats.blocks = gStats.blocks + 1;
    if (cycles > gStats.maxCycles) gStats.maxCycles = cycles;
    if (cycles > gStats.budgetCycles) gStats.late = gStats.late + 1;
    gSumCycles = gSumCycles + cycles;
    gSumBlocks = gSumBlocks + 1;
  }
}

}  // extern "C"

bool audioInit() {
  if (!h7_block_mem_mpu_init()) {
    gError = "MPU: could not mark RAM_D2 non-cached";
  } else if (h7_block_mem_region_of(gTxRing) != H7_MEM_RAM_D2) {
    gError = "DMA rings are not in RAM_D2, check the linker script";
  } else if (!initClock()) {
    gError = "PLL3 did not give a 49.152 MHz SAI kernel clock";
  } else {
    gError = nullptr;
  }
  if (gError) return false;

  gParams[0] = ChainParams();
  enableCycleCounter();
  gStats.budgetCycles = static_cast<uint32_t>(
      static_cast<uint64_t>(SystemCoreClock) * kFramesPerHalf / cv::kSampleRate);

  initGpio();
  __HAL_RCC_DMA1_CLK_ENABLE();
  if (!initSai()) {
    gError = "SAI1 init failed";
  } else if (!initDma(gDmaTx, DMA1_Stream0, DMA_REQUEST_SAI1_A, DMA_MEMORY_TO_PERIPH, DMA1_Stream0_IRQn) ||
             !initDma(gDmaRx, DMA1_Stream1, DMA_REQUEST_SAI1_B, DMA_PERIPH_TO_MEMORY, DMA1_Stream1_IRQn)) {
    gError = "DMA1 init failed";
  }
  if (gError) return false;
  __HAL_LINKDMA(&gSaiTx, hdmatx, gDmaTx);
  __HAL_LINKDMA(&gSaiRx, hdmarx, gDmaRx);
  initBlockInterrupt();

  // The slave receiver must be armed before the master starts the clocks.
  if (HAL_SAI_Receive_DMA(&gSaiRx, reinterpret_cast<uint8_t*>(gRxRing), kWordsPerRing) != HAL_OK ||
      HAL_SAI_Transmit_DMA(&gSaiTx, reinterpret_cast<uint8_t*>(gTxRing), kWordsPerRing) != HAL_OK) {
    gError = "SAI DMA start refused";
    return false;
  }
  return true;
}

const char* audioError() { return gError; }

void audioPublish(const ChainParams& params) {
  gParams[gWriteIndex] = params;
  gPublished.store(gWriteIndex, std::memory_order_release);
  gWriteIndex ^= 1;
}

void audioStopForBench() { HAL_SAI_DMAStop(&gSaiRx); }

void audioResetPeaks() {
  gStats.late = 0;
  gStats.overruns = 0;
  gStats.maxCycles = 0;
  gSumCycles = 0;
  gSumBlocks = 0;
  for (int s = 0; s <= kSlotCount; ++s) gSlotCycles[s] = 0;
}

AudioStats audioStats() {
  AudioStats s;
  s.blocks = gStats.blocks;
  s.late = gStats.late;
  s.overruns = gStats.overruns;
  s.maxCycles = gStats.maxCycles;
  const uint32_t n = gSumBlocks;
  s.avgCycles = n ? static_cast<uint32_t>(gSumCycles / n) : 0;
  for (int i = 0; i <= kSlotCount; ++i) s.slotAvg[i] = n ? static_cast<uint32_t>(gSlotCycles[i] / n) : 0;
  s.budgetCycles = gStats.budgetCycles;
  return s;
}
