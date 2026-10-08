# Astra packet: cubevox engine CPU cost on the STM32H743 (2026-10-08)

Answer item by item. Write each answer as you finish it, not one block at the end.
Everything you need is in this packet; do not read the repository.

## Facts

Target: STM32H743VIT6 rev V, 480 MHz, I-cache and D-cache on (SCB->CCR read on the board),
FPU on, hard float ABI, `-mfpu=fpv5-d16`, GCC 14.2.1, `-O2 -ffp-contract=fast`. Audio: 48 kHz
mono float, 64-sample blocks, so the budget is 640000 cycles per block. The chain runs in a
software-pended lowest-priority IRQ; DWT CYCCNT measures each block and each stage.
Engine state is a single `Chain` struct in `.bss` (AXI SRAM at 0x24000000, cacheable).
Block buffers are two `float[64]` on the IRQ stack. Input on the bench board is a floating
SAI pin (noise), no codec.

Measured on a WeAct H743 (same die), average cycles per block over about 3000 blocks:

| stage | off | on |
|---|---|---|
| input gate | 25065 | 25805 |
| autotune + pitch tracker (one PitchFx stage) | 173105 | 215425 |
| unison (2-voice chorus) | 102297 | 134369 |
| slapback | 14352 | 26175 |
| distortion | 5849 | 101196 |
| gate | 25017 | 25449 |
| spring reverb | 5654 | 75240 |
| fixed EQ (5 biquads) | 9111 | 9225 |
| total | 364158 (57 %) | 616647 (96 %) |

Worst block: 988819 (155 % of budget), one block in four is over budget in every
configuration including all-off. The pitch tracker analyses every 256 samples (every fourth
block). With `-O3 -ffast-math`: total on 512334 (80 %), distortion 53605, unison 102559,
gates and tracker unchanged, worst block 895500.

Hot loops as they are today (emulator-first code, shared with a Mac build):

```cpp
// gate.h, per sample
hpState_ += aHp * (in[i] - hpState_);
const float mag = fabsf(in[i] - hpState_);
env_ = mag > env_ ? mag : env_ + aDet * (mag - env_);
const float levelDb = 20.0f * log10f(env_ + kFloor);
... hold / attack / release on gainDb_ ...
out[i] = in[i] * expf(gainDb_ * onMix_ * kDbToNeper);
```

```cpp
// unison.h, per sample, two voices
stepDepth(target, smooth);                       // one-pole toward p.on ? depth : 0
line_[writePos_] = in[i];
for (v = 0; v < 2; ++v) {
  centre = baseDelayMs[v]*kSmpPerMs + swing*kSmpPerMs*sinf(phase_[v]);
  s = readShifted(v, centre, windowSmp);         // two interpolated taps, 0.5*(1-cosf(2*pi*p)) window
  phase_[v] += inc[v]; wrap;
  ratio = powf(2.0f, detuneCents[v]*depth_/1200.0f);
  shiftPhase_[v] = frac(shiftPhase_[v] + (1.0f - ratio)/windowSmp);
}
out[i] = (in[i] + wetGain*wet) * powf(10.0f, trimDb*depth_/20.0f);
writePos_ = (writePos_ + 1) % kLen;
```

```cpp
// pitch.h, every 256 samples: two-stage YIN
// coarse: 12 kHz copy, W = 256, lags 1..171
for (t = 1; t <= 171; ++t) { d = 0; for (j = 0; j < 256; ++j) { e = x[j]-x[j+t]; d += e*e; }
  running += d; cmnd[t] = d*t/running; }
// refine: 48 kHz, W = 1024, 9 lags around 4*tau, same inner loop
```

Distortion: `expf` twice per sample for the two gain stages, `tanhf` per oversampled sample.
Slapback: `powf(10, trimDb*intensity/20)` and `sqrtf` per sample. PitchFx: `sinf`/`cosf` per
sample for two equal-power mixes; the tracker `push` runs a biquad and two ring writes per
sample and always runs, even with autotune and octave both off.

Project rules that bind the fix: engine headers are float-only, `std::array` state, no heap
and no I/O in `process()`, shared unchanged between the Mac emulator and the box; 14 host
tests cover level (every stage near unity at defaults), gate, slapback, reverb etc.; the box
rejects a layout whose summed card costs exceed 85 %.

## Proposed fix (for your review)

1. Control-rate parameter math: every dB-to-linear, `powf`, `expf`, `sqrtf` that depends only
   on knob values or on slowly smoothed state moves out of the sample loop, computed once per
   block (1.33 ms) with per-sample linear interpolation of the resulting linear gain.
2. Gate detector: keep the envelope in the linear domain; compare against linear thresholds;
   compute the gain ramp in linear gain with one-pole attack/release; no `log10f`/`expf`
   per sample.
3. LFOs by phase accumulator plus a quadrature recurrence or a 256-entry table with linear
   interpolation; the chorus window from the same table.
4. `tanhf` replaced by a rational approximation with a bounded error; the delay modulo by a
   compare-and-wrap.
5. YIN: four independent accumulators in the inner loop, and the coarse search spread across
   the four blocks of the hop so no single block carries the burst (snapshot the window at the
   hop, compute a quarter of the lags per block, publish one hop later; pitch latency +4 ms).
6. Skip the tracker `push` when neither pitch card is in the layout (not when toggled off,
   so engaging stays instant).
7. Costs table: card cost = measured average cycles per block with the card on, as a percent
   of 640000, after the pass; the 85 % rule stays on averages; the worst block must stay under
   100 % by construction (no bursts).

## Questions

1. Does the libm-per-sample diagnosis plus the YIN chain account for the numbers above, or
   does something else fit the data better (memory placement, the stack arrays, cache
   line effects, the IRQ priority scheme)? Name what you would measure first to be sure.
2. Review the seven fix items. Which are wrong, which are missing, and in what order would you
   do them so the host tests keep passing after each step?
3. Spreading YIN across the hop costs 4 ms of pitch latency. Is that acceptable for live vocal
   autotune and octave PSOLA, or would you rather cut the lag range or the window and keep the
   result at the hop?
4. For a 480 MHz M7 doing this chain, what total average load should we expect after the
   pass, and is 85 % summed-average a sound budget rule or should it be lower to absorb the
   remaining per-block variance?
