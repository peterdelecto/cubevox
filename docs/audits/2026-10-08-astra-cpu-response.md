# Astra response: engine CPU cost on the H743 (2026-10-08)

Packet: `2026-10-08-astra-cpu-packet.md`. Four runs (the first run wrote only answer 4; the
other three were re-asked one question each). Verbatim below; the lead's reading follows.

## 1. Does the diagnosis fit the data?

1. **The data strongly supports expensive per-sample math plus periodic YIN work.** Distortion nearly halves with fast-math, and unison improves substantially; both contain repeated transcendental calls. Large "off" costs suggest bypass still executes substantial DSP. The every-fourth-block overrun matches the tracker's cadence unusually well. YIN evaluates 52,992 squared differences per analysis, with a serial accumulation dependency. That makes it a credible burst source, although timing alone cannot establish that dependency as the main bottleneck.
2. **Memory and IRQ effects remain possible contributors, but fit the pattern less directly.** The two stack buffers total only 512 bytes; their size alone is no red flag. Stack location, ring-buffer access, cache conflicts, and higher-priority interrupts can affect timing. Enabled caches do not establish good locality, and CYCCNT intervals include preemption. None of these alternatives explains the four-block cadence as directly as scheduled YIN.
3. **Measure YIN separately first:** coarse, refinement, snapshot copying, and non-analysis blocks; correlate overruns with hop phase and higher-priority ISR activity. Then inspect target disassembly and time representative libm calls with runtime inputs. This distinguishes arithmetic cost, library implementation, memory stalls, and preemption.
4. **The proposed fixes are sensible, but average admission costs cannot guarantee deadlines.** Spreading coarse lags leaves refinement and snapshot costs to schedule; four accumulators need benchmarking. Linear gain smoothing changes gate behavior and needs validation. Require a measured worst-case execution bound with interrupt margin; "no bursts" alone does not establish it.

## 2. Review of the seven fix items

1. **Control-rate math: useful, but too broad.** Hoist true constants first. Smoothed state may affect feedback or sample-dependent dynamics; computing it once per block changes behavior. Interpolating linear gain also differs from the existing exponential trajectory. Preserve smoother timing, endpoints, and toggle behavior; check level and transitions after each stage conversion.
2. **Gate: changes the algorithm.** Linear envelope detection and converted thresholds can remove the logarithm, provided they preserve the floor and threshold semantics. A linear-gain one-pole is not equivalent to smoothing dB gain: attack/release curves change. Preserve hold, hysteresis, off-state behavior, and time constants; test trajectories before accepting that change.
3. **LFOs: reasonable with safeguards.** Recurrence needs drift control; tables need correct periodic interpolation. Check phase continuity, chorus-window complementarity, and equal-power mix accuracy. Hoist fixed increments first; preserve phase advancement while bypassed.
4. **Approximation and wrap: separate changes.** Compare-and-wrap is straightforward if the increment and range assumptions hold. Specify the rational approximation's input domain, saturation behavior, and maximum error. Level tests alone cannot establish distortion equivalence; check symmetry, spectrum, and oversampled behavior.
5. **YIN: scheduling is incomplete.** Unrolling may help, but changes summation order and needs measurement. Budget snapshot copying, coarse CMND/selection, dependent refinement, and publication, not just coarse lags. Snapshot every buffer refinement needs into preallocated storage. One hop is **5.33 ms**, not 4 ms; four processing slots span 4 ms only under a specific start/finish convention. Define timestamps and test pitch/confidence equivalence.
6. **Tracker skipping: correct for absent cards.** Define reset/warm-up behavior when a pitch card enters the layout. Keeping tracking active while toggled off preserves readiness. Also investigate the substantial other off-state costs; bypass shortcuts must respect tails and state continuity.
7. **Costs: averages alone cannot guarantee deadlines.** Account for shared tracking once, fixed overhead, transitions, refinement, and interrupt interference. Validate complete layouts, not just summed stage averages. Measure with controlled signals and noise; the floating input is insufficient coverage. Keep 85% admission only alongside a defensible maximum-block bound.

**Order:** establish timing/audio baselines → constant hoists and wrap → absent-tracker skip → schedule all YIN work → stagewise control interpolation → LFOs → tanh approximation → gate redesign → recalibrate costs. Run the 14 host tests after each change, add targeted behavior checks, and measure board timing before claiming savings.

## 3. Spread YIN or shrink it?

I'd spread YIN across the hop first, preserving the lag range and windows. An extra 4–5.33 ms is a reasonable tradeoff to evaluate for live vocals when the alternative already produces regular deadline overruns. I wouldn't shorten the analysis solely to avoid that delay.

1. **Check the latency accounting.** A 256-sample hop at 48 kHz is **5.33 ms**. Completing quarters in the snapshot block and the next three blocks adds 4 ms; publishing "one hop later" adds 5.33 ms. Specify which schedule you mean.
2. **Preserve pitch coverage and stability first.** Reducing the maximum lag sacrifices low-note coverage; shortening the window reduces the evidence available for estimating periodicity. Both can hurt autotune and octave PSOLA. Acceptability depends on total monitoring latency and how stale estimates affect note transitions, not just this scheduling increment. Compare sustained low notes, slides, and abrupt transitions with voiced input; floating-pin noise cannot establish musical acceptability.
3. **Budget the whole analysis.** Spreading only the coarse search leaves the nine-lag refinement concentrated in one block. Schedule or reserve capacity for refinement, snapshot copying, and publication too. Average card costs below 85% do not establish a worst-case guarantee.

My preference: preserve the estimator, distribute its work, then audition the added delay. Reduce the window or range only if measured musical behavior justifies that tradeoff.

## 4. Expected load and the budget rule

**I would use 50–65% average load, 320,000–416,000 cycles/block, as a provisional planning target, not a predicted benchmark.** The packet cannot support a tighter estimate.

Reaching that range requires removing roughly 200,000–300,000 cycles from the current average. Gates, unison, distortion, and PitchFx provide plausible opportunities, but spring reverb, delay/filter work, and tracker ingestion remain. The inferred YIN burst alone contributes approximately 125,000 cycles/block, or 20%, averaged across the hop before optimization. Spreading it changes peak load, not average work.

**An 85% summed-average admission rule is insufficient.** At 85%, only 96,000 cycles remain for analysis-phase imbalance, parameter transitions, memory stalls, interrupt preemption, and unaccounted overhead. Removing the main burst does not bound those costs.

Keep average percentages for the UI, but admit layouts using: max over schedule phases of (bounded chain cycles) + interrupt allowance + margin < 640000. Include shared work once and account for the combined layout: isolated card timings need not add exactly because memory/cache behavior can change.

Until that model is validated, I would provisionally cap summed averages at **70–75%**, alongside a separate peak/deadline check. That cap supplies engineering headroom; it is not itself a guarantee. Reconsider 85% only after the complete scheduled chain demonstrates adequate worst-case margin on the board across representative signals and transitions.

## Lead's reading (2026-10-08)

Agreed on every point. Adopted: Astra's order; each step gated by the 14 host tests, a
per-stage behaviour check where the algorithm changes (gate trajectories, distortion
spectrum, pitch equivalence), and a board measurement before the saving is claimed. The
budget rule becomes 75 % summed average plus a worst-block check on the full layout; the
registry spec and `costs.h` change together. The YIN schedule is defined in the engine
change itself. A voiced test signal (bench tone or the Mac loop through the codec) replaces
the floating pin before any musical judgement.
