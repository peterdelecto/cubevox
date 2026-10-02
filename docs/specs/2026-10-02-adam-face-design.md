# cubevox — Adam's face: hardware knobs + musician tuning sliders

Date 2026-10-02. Owner approved the numbered table in chat ("build it"). Goal: Adam
(musician, not technical) can choose knobs and season each effect with a few named
sliders, then report numbers we bake in.

## Rules

1. Each module shows its **hardware knobs** first, then a collapsed `Tuning` header
   with the **musician sliders** below. Nothing else by default.
2. A global `Advanced` checkbox on the transport row (default off) shows today's raw
   tuning nodes under each module instead, for us. Raw values and macros stay in sync:
   moving a macro writes the raw fields; the macro position is stored in the emulator.
3. Macro sliders are 0–100 % and open at **50 % = today's sound**. Mapping is
   piecewise between three anchor values (0 / 50 / 100) per field: log interpolation
   for Hz and ms, linear otherwise, integers stepped. Anchors are in the table below
   and live in one file, `emulator/macros.h`, as a constexpr table.
4. `Print tuning` is a complete plain-text snapshot Adam can paste into a message and
   we bake into firmware. In order: `Macros:` every macro with its %, `Choices:` every
   module on/off, engine radios, Harmony voice levels, Follow my bends, Drop out on
   breaths, Autotune key link and chromatic, `Knobs:` every hardware knob position,
   then the raw initialisers.
5. Trims are never shown outside Advanced. `level_test` keeps owning them.
6. Accordion and `--layout` probe rules unchanged; probe scenarios cover each module's
   Tuning header (macro view) and the Advanced view.

## Modules

| # | Module | Hardware knobs | Tuning sliders |
|---|---|---|---|
| 1 | Input gate | none (no checkbox either; always on in the face) | Threshold, Range, Attack, Hold, Release (raw) |
| 2 | Gate | THRESHOLD | Range, Attack, Hold, Release (raw) |
| 3 | Autotune | KEY (linked to Harmony) · RESPONSE | Pull range (0.5–6 st, `maxCorrectSemis`) · Correct to every note (`chromatic`, default off) |
| 4 | Octave | SEMITONES · FORMANT (engine B) · MIX | Slide |
| 5 | Harmony | KEY · MIX | Voices · Tracking speed · Follow my bends · Drop out on breaths |
| 6 | Unison | DEPTH | Chorus ↔ Double · Motion speed |
| 7 | Slapback | INTENSITY | Time · Repeats · Low pass (raw) |
| 8 | Distortion | DRIVE · TONE | Body · Bite · Grit |
| 9 | Reverb | DECAY · DWELL · MIX (engine radio SPRING / CHASM / PARKER SPRING = menu) | per engine, below |
| 9a | SPRING | | Splash · Flutter · Low end |
| 9b | CHASM | | Wobble · Brightness · Bass |
| 9c | PARKER SPRING | | Splash · Drip · Flutter · Brightness |
| 10 | Output EQ | menu | Low cut · Low-mid dip (freq, amount) · Presence (freq, amount) · Air (raw) |

Engine radios (Octave A/B/C, Harmony A/B/C, Autotune A/B, reverb engine) stay visible:
they are what Adam is choosing between.

## Macro anchors (0 % / 50 % today / 100 %)

| Macro | Field | 0 | 50 | 100 |
|---|---|---|---|---|
| Octave Slide | glideMs | 2 | 15 | 120 |
| Harmony Tracking speed | glideMs | 120 | 30 | 5 |
| Unison Chorus ↔ Double | detuneCents (±) | 0 | 2 | 6 |
| | swingMinMs / swingMaxMs | ×1 | ×1 (0.5 / 3.0) | ×0.2 |
| Unison Motion speed | lfoHz both | ×0.5 | ×1 (0.60 / 0.90) | ×2 |
| Distortion Body | inputHpHz | 160 | 90 | 40 |
| | s1BassDb | −18 | −12 | −6 |
| | stackBassDb | 6 | 10 | 14 |
| | bassPeakDb | −2 | 2 | 6 |
| Distortion Bite | s1LpHz | 3000 | 6000 | 12000 |
| | stackTrebleDb | −10 | −5 | 0 |
| | s2LpHz | 4000 | 6000 | 10000 |
| | trebleCutDb | −9 | −6 | −3 |
| Distortion Grit | railAsym | 0 | 0.05 | 0.2 |
| | railSoft | 0.3 | 0.1 | 0.02 |
| SPRING Splash | hfMixDbLo | −40 | −22 | −10 |
| | hfMixDbHi | −30 | −14 | −4 |
| | rippleGain | 0.03 | 0.10 | 0.30 |
| | splashDiffuse | 0 | 0 | 0.7 |
| | hfSections | 0 | 0 | 120 |
| SPRING Flutter | modDepth | 2 | 8 | 24 |
| | modRateHz | 1.5 | 3 | 5 |
| | springs | 2 | 2 | 3 (≥ 75 %) |
| SPRING Low end | hpHz | 600 | 300 | 120 |
| | boingDb | 0 | 0 | 6 |
| CHASM Wobble | wobble (param) | 0 | 0.15 | 0.6 |
| CHASM Brightness | trebleLossHz | 1500 | 3000 | 7000 |
| | inputTrebleCut | 0.85 | 0.95 | 1.0 |
| CHASM Bass | bassCutHz | 400 | 200 | 80 |
| | bassCutHzTop | 200 | 100 | 40 |
| PARKER Splash | hfMixDb | −40 | −22 | −8 |
| | echoGain | 0.05 | 0.2 | 0.3 |
| | rippleGain | 0.05 | 0.2 | 0.3 |
| | presenceDb | 1 | 5 | 8 |
| PARKER Drip | aLf | 0.55 | 0.70 | 0.82 |
| | mLow | 60 | 100 | 100 |
| PARKER Flutter | spread k (tdFactor = 1, 1+0.15k, 1−0.12k) | 0.3 | 1 | 1.6 |
| | modDepth | 2 | 8 | 16 |
| PARKER Brightness | lpHz | 5000 | 9000 | 14000 |
| | presenceHz | 2500 | 3000 | 4000 |

Harmony checkboxes: **Follow my bends** = `!snapToScale` (default on). **Drop out on
breaths** = `muteUnvoiced` (default off). **Voices**: three rows Low / High / Higher,
each Off / Low / Med / High (no cap on how many are on), with a Formant slider per row (engine B, voice on); Lower and Fixed are off the face.

## Engine changes

1. **Harmony, three voice slots.** `HarmonyParams::slots` becomes `std::array<HarmonySlot, 3>`;
   `HarmonyVoices` holds three of each shifter. A slot with level 0 costs nothing.
   Existing tests unchanged (third slot off). Harmony test gains one check: Low, High,
   Higher all on (A3 in C) → three distinct output partials at the diatonic targets
   (±10 cents each, via band-summed Goertzel).
2. **CHASM DWELL.** `ChasmParams` gains `dwell` (default 0.2) and `ChasmTuning` gains
   `dwellDrive = 8`, `dwellComp = 0.8`; front end `x·dwellDrive^dwell → softClip (Padé,
   as Spring) → ·drive^-comp` before the existing input stage. dwell 0 = drive 1, so the
   soft clip is the only change at 0 (below −6 dBFS it is within 0.1 dB of linear).
   Chasm test gains: dwell 1 adds ≥ 6 dB more 3rd-harmonic on a 220 Hz sine than dwell 0.
   `level_test` CHASM row re-trimmed if it leaves 0..+0.5 dB.
3. Reverb face knobs: knob 1 DECAY → `spring.tension` / `chasm.decay` / `parker.tension`;
   knob 2 DWELL → each engine's `dwell`; MIX unchanged. Render CLI: `--decay` and
   `--dwell` apply to whichever engine is selected (keep `--tension`/`--wobble` as aliases).

## Done when

ctest passes; `--layout` passes in macro and Advanced views; the face shows only the
table above unless Advanced is on; Print tuning prints Macros, Choices, Knobs, then the raw initialisers.
