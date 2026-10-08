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
| 1 | Input gate | THRESHOLD · DECAY (release), as Gate (owner 2026-10-02) | Range, Attack, Hold (raw) |
| 2 | Gate | THRESHOLD · DECAY (release) | Range, Attack, Hold (raw) |
| 3 | Autotune | KEY (linked to Harmony) · RESPONSE | Pull range (0.5–6 st, `maxCorrectSemis`) · Correct to every note (`chromatic`, default off) |
| 4 | Octave | SEMITONES · FORMANT (engine B) · MIX | Slide |
| 5 | Harmony | KEY · MIX | Voices (High, Higher) · Tracking speed · Follow my bends |
| 6 | Unison | DEPTH · RATE (chorus speed, ~0.2–6 Hz; past today's rate the sweep narrows so the pitch swing caps near 2x today's) | Chorus ↔ Double |
| 7 | Slapback | MIX (level + repeats) · TIME (30–150 ms) | Repeats · Low pass (raw) |
| 8 | Distortion | DRIVE · TONE | Body · Bite · Grit |
| 9 | Reverb | DECAY · DWELL · MIX (engine radio SPRING B / CHASM = menu) | per engine, below |
| 9b | CHASM | | Wobble · Brightness · Bass |
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
| Unison RATE | lfoHz both | ×0.33 | ×1 (0.60 / 0.90) | ×7 (swing × 2/m above ×2) |
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
| CHASM Wobble | wobble (param) | 0 | 0.15 | 1.0 |
| CHASM Brightness | trebleLossHz | 1500 | 3000 | 7000 |
| | inputTrebleCut | 0.85 | 0.95 | 1.0 |
| CHASM Bass | bassCutHz | 400 | 200 | 80 |
| | bassCutHzTop | 200 | 100 | 40 |

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
3. Reverb face knobs: knob 1 DECAY → `springB.decay` / `chasm.decay`;
   knob 2 DWELL → each engine's `dwell`; MIX unchanged. Render CLI: `--decay` and
   `--dwell` apply to whichever engine is selected (keep `--tension`/`--wobble` as aliases).

## Done when

ctest passes; `--layout` passes in macro and Advanced views; the face shows only the
table above unless Advanced is on; Print tuning prints Macros, Choices, Knobs, then the raw initialisers.

## Hand-off polish (owner 2026-10-02)

1. **Saved state.** The face is saved to `~/Library/Application Support/cubevox-proto/state.txt`
   once a second when something changed, and on close; it is read on launch, loop included,
   with playback stopped. Format is `key value` lines; unknown keys are skipped and missing
   keys keep defaults, so a new build reads an old file. Saved are knobs, choices, voice menu,
   macro positions, the gate / slapback / EQ / autotune sliders, and the dev settings. Raw
   Advanced edits are not saved; macros re-apply on load. The `--layout` probe round-trips
   every field through the file.
2. **Hint line** under every tuning control: what it does, technical term last.
3. **Plain gate names** (owner choice): Opens at (threshold), Mute amount (range, shown
   positive, right = more), Fade in (attack), Stay open (hold), Fade out (release).
   Slapback keeps the owner's name Low pass.
4. **Output EQ face:** Low cut, Mud cut (shown positive), Presence, Air. Band centres move
   under Advanced.
5. **Test controls hidden.** Cmd+Shift+D toggles Advanced and the input gate's Tuning.
   Hidden means off. STAGE FEEDBACK with Amount and Movement stays on the face (owner). The probe runs with
   them shown, the superset of Adam's face.
6. **Headline transport row** (owner 2026-10-02): Load, file name, Play, meter, STAGE
   FEEDBACK, Reset and Copy settings to clipboard at 1.4x type with taller controls. The
   meter is 100 px and the file name has a fixed 110 px slot with an ellipsis. Print tuning
   is renamed Copy settings to clipboard; the text box beside it is gone and the button
   reads Copied for 1.5 s instead. The probe fails if the row runs past the window width,
   checked with a long real file name.
7. **Pedal mode** (owner 2026-10-02): headline-size bottom-right button flips the face to a row of eight
   stompboxes in signal order, panel knobs only: AUTOTUNE (RESPONSE, KEY encoder), OCTAVE
   (MIX, SEMITONES encoder), HARMONY (MIX, KEY encoder), UNISON (DEPTH, RATE), SLAPBACK
   (MIX, TIME), DISTORTION (DRIVE, TONE), GATE (THRESHOLD, DECAY), REVERB (DECAY,
   DWELL, MIX on the selected engine). Output EQ has no pedal; it lives in the menu. Knobs drag vertically or
   scroll; encoders step one detent per 14 px or scroll notch. The foot strip toggles the
   effect and lights its LED. The input gate is not shown (not exposed on the box).
8. **Two harmony voices** (owner 2026-10-02): Adam has no harmony of his own to match, so
   the face offers High (default Louder) and Higher (default Loud); the engine's third slot
   stays off. Drop out on breaths is removed from the face and stays off.
9. **MUTE** (Adam 2026-10-07; was BYPASS, owner 2026-10-02): above Pedal mode, same size,
   red while on. The output is silent and the chain keeps running underneath, as the box's
   top-left MUTE toggle does. The last column leaves room for it. Input gate shows the same
   controls as Gate, in both views. The eight pedals are the box's eight slots
   (`2026-10-07-slots-design.md`): two knobs and a toggle each, left to right is the chain.
