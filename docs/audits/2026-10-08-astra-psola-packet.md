# Astra packet: PSOLA voice cost on Cortex-M7

Answer item by item; write each answer as you finish it. Reason from this packet
only, no file reads needed.

## Facts

Target STM32H743, Cortex-M7 at 480 MHz, I/D cache on, FPU on. Toolchain
xpack arm-none-eabi-gcc 14.2.1, `-O2`, `-mfpu=fpv5-d16 -mfloat-abi=hard`, newlib.
Audio block 64 samples at 48 kHz, budget 640,000 cycles per block. Cycle counts by
DWT around each stage, averaged over many blocks on a real vocal clip.

Measured for the pitch stage, all voices resident in AXI SRAM (D-cached):

| State | Stage cycles per block |
|---|---|
| everything off (copy-through, rings written) | 5,500 |
| Octave granular voice only (2 grains, cubic reads, rotor window) | 53,000 |
| Autotune PSOLA voice + tracker only | 122,000 |
| both | 147,000 |

Tracker analysis (YIN, 53k multiply-adds per 256-sample hop, four-accumulator loop)
costs about 140,000 per hop, so 35,000 per block averaged. That leaves the PSOLA
voice at roughly 59,000 per block, about 920 cycles per sample. At ratio 1 the
voice has about 2 active grains per sample (grain length 2 periods, one launched
per output period). My count of the work per sample is about 230 cycles. I want
to know where the other 700 go.

The ring `VoiceRing` is `std::array<float, 4800>`. `writeCount` is already wrapped
below 4800 by the owner. `clampf` is a plain ternary.

## The code (current)

```cpp
struct HannRotor {
  float c = 1.0f, s = 0.0f, dc = 1.0f, ds = 0.0f;
  void init(float phase, float step) { c = cosf(phase); s = sinf(phase); dc = cosf(step); ds = sinf(step); }
  float window() const { return 0.5f * (1.0f - c); }
  void advance() { const float c2 = c * dc - s * ds; s = s * dc + c * ds; c = c2; }
};

class PsolaVoice {
 public:
  static constexpr int kGrainDelay = 1040;
  static constexpr int kMaxGrains = 8;

  float tick(const VoiceRing& ring, long writeCount, float period, float ratio) {
    if (period <= 0.0f) return 0.0f;
    const float p = clampf(period, kMinPeriod, kMaxPeriod);
    const float r = clampf(ratio, kMinRatio, kMaxRatio);
    const float outPeriod = p / r;
    const int head = static_cast<int>(writeCount);

    advanceMarks(p);
    if (countdown_ > outPeriod) countdown_ = outPeriod;
    countdown_ -= 1.0f;
    if (countdown_ <= 0.0f) {
      launch(p, -countdown_);
      countdown_ += outPeriod;
    }

    float sum = 0.0f;
    for (Grain& g : grains_) sum += play(ring, head, g);
    return sum / r;
  }

 private:
  struct Grain {
    float delay = 0.0f;
    float age = 0.0f;
    float len = 0.0f;
    HannRotor window;
    bool active = false;
  };

  void advanceMarks(float p) {
    markDelay_ += 1.0f;
    while (markDelay_ - p >= grainDelay_) markDelay_ -= p;
  }

  void launch(float p, float lead) {
    float centre = markDelay_;
    if (centre - grainDelay_ > 0.5f * p) centre -= p;
    Grain* slot = &grains_[0];
    for (Grain& g : grains_) {
      if (!g.active) { slot = &g; break; }
      if (g.age > slot->age) slot = &g;
    }
    slot->delay = clampf(centre + p - lead, kMinDelay, kMaxDelay);
    slot->age = lead;
    slot->len = 2.0f * p;
    slot->window.init(kTwoPi * lead / slot->len, kTwoPi / slot->len);
    slot->active = true;
  }

  float play(const VoiceRing& ring, int head, Grain& g) const {
    if (!g.active) return 0.0f;
    if (g.age >= g.len) { g.active = false; return 0.0f; }
    const float w = g.window.window();
    g.window.advance();
    g.age += 1.0f;
    return w * readCubic(ring, head, g.delay);
  }

  static int wrap(int i) {
    if (i < 0) i += kVoiceRingLen;
    else if (i >= kVoiceRingLen) i -= kVoiceRingLen;
    return static_cast<unsigned>(i) < static_cast<unsigned>(kVoiceRingLen) ? i : 0;
  }

  static float readCubic(const VoiceRing& ring, int head, float delay) {
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    const float f = pos - fl;
    const int i0 = wrap(static_cast<int>(fl));
    const float xm = ring[wrap(i0 - 1)];
    const float x0 = ring[i0];
    const float x1 = ring[wrap(i0 + 1)];
    const float x2 = ring[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + f * (c1 + f * (c2 + f * c3));
  }

  std::array<Grain, kMaxGrains> grains_{};
  float grainDelay_ = 1040.0f;
  float markDelay_ = 1040.0f;
  float countdown_ = 0.0f;
};
```

The caller per sample, with Autotune on: `corr_ = smooth::step(corr_, target_, a_)`
(one-pole with a snap compare), `ratio = exp2Fast(corr_ / 12)` (polynomial, no
libm), then `gain_ * psola_.tick(...)`. `floorf` compiles to `vrintm`, confirmed by
objdump (no `bl floorf`).

## Questions

1. Given this code and this compiler, what most plausibly accounts for about 900
   cycles per sample? Rank the suspects (the 8-grain loop with `Grain` 24 bytes and
   a bool, the float-to-int and `vrintm` chain, the `sum / r` divide, the
   `while` in advanceMarks, denormals, something else) with a rough cycle
   estimate each. Say which you are unsure of.
2. The cheapest change that keeps the output within about -60 dB of the current
   render. Concrete code, not a list of ideas.
3. Is there anything in this code that would make the real active-grain count much
   larger than 2 at ratio 1, or make grains fail to deactivate?
