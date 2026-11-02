# cubevox — Echo stage (card id `slapback`)

Date 2026-10-01, rewritten to the code after the 2026-11-02 audit fix pass. Owner 2026-10-01:
"slapback should be 30ms - 120ms. it should have a variable lowpass filter". The card was
Slapback until owner 2026-11-01 named it Echo; code and protocol keep the id `slapback`.

## Panel

1. Upper pot TIME, 60 to 250 ms, default 100 ms (Adam never slaps under 70 ms, owner
   2026-10-07). `Slapback::kTimeMinMs` and `kTimeMaxMs` hold the range; the registry and the
   tests read it from there.
2. Lower pot AMOUNT, default 25 % (owner 2026-10-31 named it). It sets the echo level and the
   repeat gain together. Feedback = min(0.5, tuned feedback × sqrt(AMOUNT / 0.25)): the
   owner's 0.35 (about 2.5 repeats) lands at 25 %, grows with the knob and caps at 0.5 from
   about 51 %.
3. A TIME move glides the delay with a 50 ms time constant, so a repeat bends in pitch while
   the knob moves, as most delays do (owner 2026-11-01: "dont change the bending behavior").
   The glide lands exactly on the knob's value.

## Engine (`engine/slapback.h`)

1. `SlapbackTuning`: `timeMs` 100, `lowpassHz` 4000, `feedback` 0.35, `wetMaxDb` +4.9 dB (echo
   level at AMOUNT 1, before the trim), `trimDb` −5.3 dB. `SlapbackParams`: `on`, `amount`
   (0..1), `tuning`. The engine clamps the cutoff to 500..12000 Hz, feedback to 0..0.5 and both
   levels to ±24 dB; `cubevox-render` refuses a key outside those ranges.
2. Signal: the delay read → two-pole RBJ low-pass (Q 0.7071, direct form I, two samples a pass)
   → back into the line × feedback, plus the input. Output = input + echo × wet gain × trim. The
   dry passes at unity at every AMOUNT (owner 2026-11-01: the dry never drops); the trim is one
   constant on the echo alone.
3. The line holds 250 ms plus one block plus four read taps (12,068 floats) and three guard
   samples, so a block reads in whole segments. While TIME sits still the read is a whole
   sample; while it glides it is a Catmull-Rom read. Taps behind the first recording read zero,
   so an enable needs no clear.
4. AMOUNT 0 copies the input and keeps recording, so the echo is there when AMOUNT comes back.
   Wet gain, feedback and trim follow AMOUNT at the block edges and are interpolated between,
   so no libm runs per sample.
5. No heap, no I/O, float only, `-Werror` clean.

## Level

`level_test` judges the row at AMOUNT 25 % on the mean of Adam's three clips, power-summed over
TIME 60..250 ms in 10 ms steps (a single TIME is a coin flip, because a tone adds or cancels with
its own echo): +0.25 dB. The dry stays at unity, so the echo adds on top and the output rises with
AMOUNT: +1.10 dB at 50 % and +3.38 dB at 100 %. Those rows print as notes; the level rule binds
the default (`2026-10-01-level-rule.md`).

## Cost

Host bench 2026-11-02 on Adam's clips, ns a sample, clang / gcc, before → after the fix pass:
settled 7.98 / 8.84 → 2.03 / 1.93; AMOUNT moving 9.51 / 12.32 → 3.13 / 3.11; TIME moving
8.31 / 8.89 → 3.87 / 3.55; AMOUNT 0 8.11 / 8.78 → 0.24 / 0.23. `costs.h` carries
`kSlapback {1, 2, false}` from Echo's own host ratio (25.9k board cycles a host µs) and the M7
inner loops; the board measured the earlier engine at 13.1k to 13.4k mean in the 2026-11-01
stress run. A stress run of this engine replaces the row; `firmware/stress.py` sweeps both knobs
and flips the toggle.

## Render CLI

`--slap <0..1>` enables the stage at that AMOUNT. Tuning keys: `slapTimeMs`, `slapLowpassHz`,
`slapFeedback`, `slapWetMaxDb`, `slapTrimDb`.

## Tests (`test/slapback_test.cpp`, ctest `slapback`)

Settled off is a bit-exact copy; TIME peaks at 60 / 100 / 180 / 250 ms and its clamps; the TIME
glide lands on the knob; the low-pass slope; feedback at 6 / 25 / 100 % against the law and its
cap; level clamps; the dry at exact unity at every AMOUNT and the echo trim; chunk sizes; reset;
no allocation. `knob_step_test` holds the TIME bend to a glide (a click fails it), and
`level_test` carries the row above.
