# Yawn: the throat drops on every syllable (2026-11-04)

Owner 2026-11-04: "for now, just call it yawn and write the spec. we will change the name later."

The card is shown as Apraxia since 2026-11-05; this file keeps the name Yawn it was written under. Since the
same day it drives `FormantFx` on the block the slot ahead hands it, not the shared stage
(`2026-11-05-pitch-split-design.md`).

## Purpose

A card that moves Adam's throat with his singing, the way an envelope filter pedal moves a bass
player's filter with the pluck. Each syllable drops the formants into a bigger, darker throat and
lets them come back while the note still holds. The pitch never changes. The owner heard three
renders of the third audition build and picked them as the sound: clip 2 at SENSITIVITY 50 % and
RANGE −7 st with a 250 ms decay, the same with a 500 ms decay, and SENSITIVITY 25 % with −7 st at
250 ms. The three picks all go down. The owner heard the shipping build and asked for RANGE to
spread both ways, −12 st at the pot's bottom and +6 st at its top, zero at noon (owner 2026-11-04,
"an asymmetrical spread in both directions ... maximum −12 st to +6 st but that would be at the
far extreme", then "A" for zero at noon). The top then went to +12 st, the same reach each way
(owner 2026-11-05, "apraxia should go to +12st").

## Where it sits

Slot 3, the third choice beside Octave and Formant (the SLOT 3 menu row and the emulator's slot 3
switch cycle OCTAVE, FORMANT, YAWN). It is a pitch-group card: it drives the Formant voice
(`FormantPsolaVoice` in `engine/formant_voice.h`) inside the pitch stage, on Autotune's note and
delay, so a layout holds Formant or Yawn, never both (they are one voice). Shown as Yawn in the
registry and YAWN on glass. The code id stays `formant_follow` until a rename pass, as Mob keeps
`crowd` and Chatter keeps `bloom`.

## Panel

| Pot | Name | Range | Default | What it does |
|---|---|---|---|---|
| upper | SENSITIVITY | 0 to 100 % | 40 % | the level that gives a full drop: −6 dBFS at 0 %, −36 dBFS at 100 %, a 12 dB window under it |
| lower | RANGE | −12 st at the bottom, 0 at noon, +12 st at the top (2026-11-05; +6 st before) | −5 st (29 % of the travel) | how far the throat moves on a full sweep, down below noon and up above it; noon is no effect; the amount knob of this slot |

RANGE is a straight line on each side of noon, and both sides move the same semitones per degree.
The readout shows RANGE in semitones with its sign, up to three cells (`+12`). DECAY, the time a sweep takes to come back, is
a tuning field at 300 ms, between the two decays the owner liked. The owner's call if Adam wants
to play the decay: DECAY takes the upper pot (100 ms to 1 s on a log curve) and SENSITIVITY becomes
a menu row; that is a one-line registry change and the engine does not move.

The wet path is the whole output. There is no MIX: the card replaces the voice's throat, and a
dry blend was heard in the first build as two voices, which is a different effect.

## The law

Per block of 64 samples (1.33 ms), on the card's input:

1. Level. The block's peak in dBFS.
2. Follower. A one-pole on the level with 3 ms attack and 70 ms release, so it rides syllables.
3. Drive. `full = −6 − 0.30 × SENSITIVITY%` dBFS. `drive = clamp((level − (full − 12)) / 12, 0, 1)`.
   No peak tracking and no auto-range: the first build ranged itself over 30 dB under the recent
   peak and left every sung note near full depth.
4. Onset. The drive rising through 0.5 after it has been under 0.2, or the level rising more than
   6 dB in 10 ms against a copy of itself lagged by a 10 ms one-pole. The lagged copy is held at
   the drive's zero point, so a reset does not fire onsets.
5. Sweep. The envelope is the follower's drive times a ramp that falls from 1 to 0 over DECAY while
   the note holds. A note's first block, the first with a drive above 0, starts the ramp, and each
   onset restarts it at 1. A held note then stays at 0 shift until the next onset. A soft note whose
   drive never reaches an onset (under 0.5, a note peaking 6 to 12 dB under the full point) plucks the
   same way, smaller.
6. Shift. `shiftSt = RANGE × envelope`, handed to the Formant voice as its shift, which glides it
   over 20 ms inside the pitch stage. That glide is the only smoothing on the shift. The voice
   takes −12 to +12 st from this card; the Formant card's own SHIFT knob keeps its ±7 st.
7. Silence. The drive reads 0, no onset fires, the shift rests within 0.007 st of 0.

Nothing above runs per sample: the follower, the detector and the envelope step once a block, and
the log work is per block. The Formant voice, `psola.h` and `pitch_fx.h` are unchanged by
the card.

## Level rule

Wet only and at unity: the Formant card's trim and its wet-path law, so the card at its default
reads 0 to +0.5 dB RMS over its input on Adam's clips. The third build measured +0.13 to +0.42 dB
across every mode and range at SENSITIVITY 50 %. The level row judges the default on the clips.

## Tests

1. `formant_follower` (to be renamed with the card): the drive at SENSITIVITY 50 % reads about 1 on
   a −21 dBFS note and 0 on a −33 dBFS note; the follower covers 1 − 1/e of a step in 3.00 ms up
   and 70.00 ms down; a sweep at RANGE −7 st opens at 1.000, reads 0.50 at half the decay after
   its last onset (the detector refires through the first 15 ms of a note) and 0 from the decay's
   end while the note holds; the shift rests within 0.01 st of 0 after it; a −25 dBFS note at the
   default (drive about 0.42, no onset) dips and rests within 0.01 st of 0 from DECAY on, and
   `growl_test` holds the same for Deepthroat's throat;
   off is a bit-exact copy with `==`; output finite at every knob corner, −12 st included; RANGE
   maps −12 st at the pot's bottom, 0 at noon and +12 st at the top (2026-11-05), and the default
   position reads −5 st.
2. The level row on the clips at the default.
3. The layout tests: Yawn is slot 3's third choice, one voice with Formant, in the pitch group.
4. Formant and Octave hashed identical on the three clips before and after any change to the card.

## Cost and memory

`kFormantFollower` {5, 8, false}, estimated as the Formant card's {4, 7} plus one point for the
follower (1.1 ns a sample at most on the host at −12 st, 3.7k board cycles a block). The sweep and the detector cost
nothing per sample. DTCM unchanged at 64,064 B; the card adds 200 B of D1 SRAM. The row covers
the pot's worst setting: at −12 st the grains play at half speed and more of them overlap, so the
row is set from the host bench at −12 st, not at the default. The row stays `false` until a
stress run sweeps SENSITIVITY and RANGE.

## What ships and what does not

1. Ships: the pluck law above, SENSITIVITY and RANGE (−12 st to +12 st through noon, 2026-11-05), DECAY as
   tuning, wet only.
2. Out, after the audition: the plain follower mode (the throat opens and stays open on a held
   note; the owner heard it as static), the free-running LFO and the envelope-gated LFO (heard,
   not picked), MIX. The code for the three unpicked modes came out in the shipping build; the
   render tool keeps keys only for what ships. The audition WAVs that carried them are listed in
   the log. The upward range was out after the third build (+7 st read as an alien voice) and came
   back at the owner's ask after the shipping build, capped at +6 st at the pot's top, then at +12 st
   from 2026-11-05.

## Done when

`ctest` passes with the card's rows; the emulator's slot 3 switch reads OCTAVE / FORMANT / YAWN and
the readout shows YAWN with SENSITIVITY and RANGE; the three picked renders hold their level within
0.1 dB of the shipping build's, and their shift moves only where log entry 8 says; the owner listens on
the box.

## Log

1. 2026-11-04, first build (v2.01.18, `build/audit/report-follower-audition.md`): the Formant
   voice's shift driven by a follower auto-ranged over 30 dB under the recent peak, DEPTH −7 to +7
   st and MIX. Owner: it is meant to adjust the formant in response to the input like an envelope
   follower bass pedal. The envelope sat at 0.6 to 1.0 the whole clip.
2. 2026-11-04, second build (v2.01.20, `report-follower-2-audition.md`): SENSITIVITY in place of
   the auto-range, 3 ms / 70 ms follower, RANGE −7 to +7 st, wet only, a pluck sweep behind a tuning
   switch. Owner: the pluck files and clip 3 at sensitivity 25 % almost work; the rest is an alien
   voice; try again with LFO modulation and the formant going lower, +3 st at most.
3. 2026-11-04, third build (v2.01.21, `report-follower-3-audition.md`, WAVs in
   `build/audition/follower3/`): RANGE −7 to +3 st through noon, modes follow / pluck / lfo /
   lfoEnv, pluck the default. Owner picked `clip2-pluck-sens50-range-minus7-decay250`,
   `clip2-pluck-sens50-range-minus7-decay500` and `clip2-pluck-sens25-range-minus7-decay250`, named
   the card Yawn for now, and asked for this spec.
4. 2026-11-04, shipping build (v2.01.22, `report-yawn-ship.md`, WAVs in `build/audition/yawn/`):
   the pluck law alone, DECAY a tuning field at 300 ms, RANGE 0 to −7 st on the lower pot through
   a reversed linear curve (`linear_reversed` on the wire; the zero-at-noon curve is gone),
   SENSITIVITY 40 % and RANGE −5 st as the defaults, YAWN on glass. The three picks render
   bit-identical to the third build's files. At the default on the three clips the shift reaches
   −4.34 st, the level reads +0.07 to +0.45 dB, and the host bench puts the card 1.1 to 1.6 ns a
   sample over Formant, so `kFormantFollower` stays {4, 5, false}.
5. 2026-11-04, RANGE both ways (v2.01.23, `report-yawn-range.md`, WAVs in `build/audition/yawn2/`):
   −12 st at the bottom, 0 at noon, +6 st at the top through the zero-at-noon curve; the voice's
   limit `kFormantVoiceMaxShiftSt` 12 beside the Formant card's ±7 knob. At −12 st four grains
   overlap, the window sum reads 0.000 dB at 220 Hz and at the slowest period, the ring holds
   with 65.9 ms of read depth at most, and the level reads −0.21 to +0.55 dB (the trim table ends
   at ±7 st and holds its end entry past it). The default renders are sample-identical to the
   shipping build's. Bench: 1.10 ns a sample over Formant at worst (−12 st), so the row stays
   {4, 5, false}.
6. 2026-11-05, RANGE to +12 st (owner, "apraxia should go to +12st"): `kFollowerMaxRangeSt` 12, so the pot
   runs −12 st, 0 at noon, +12 st, the same reach each way and the voice's own limit. At +12 st the grains
   play at double speed and a grain lasts two output periods (the floor), so two overlap. The window sum
   reads 0.000 dB at periods 48, 218 and 686 samples, and no read reaches a sample the ring has not been
   given. The trim table grew from 15 to 25 entries, −12 to +12 st (the end entries were held past ±7 st
   before). Held-end readings on Adam's cleaned clips, voice alone at MIX 100 %, mean of the three, before
   the new entries: +8 st +0.29, +9 +0.10, +10 +0.41, +11 +0.42, +12 +1.48 dB; −8 +0.15, −9 +0.35, −10
   +0.25, −11 +0.20, −12 +0.65 dB. After the fit every entry from ±8 to ±11 st reads +0.24 to +0.27 dB, +12 st
   reads +0.27 dB and −12 st +0.40 dB (set 0.15 dB high so the card behind Autotune holds the rule there).
   The card directly behind Autotune read −0.32 dB at +12 st and −0.02 dB at −12 st on those entries, so
   `kFormantGroupTopTrimDb` lifts the corrected voice's share by up to 0.5 dB from +11 to +12 st; behind
   Autotune the means then read +0.06 to +0.33 dB from ±8 to ±12 st. `level_test` holds every SHIFT from
   −12 to +12 st, alone and behind Autotune. The default and every setting inside ±7 st render as before.
7. 2026-11-07, a388e76: `kFormantFollower` raised to {5, 8, false}, the Formant row {4, 7} plus one point.
8. 2026-11-10, fix pass: a soft note plucks. A note whose drive stayed under the 0.5 onset (peaking 6 to
   12 dB under the full point, −30 to −24 dBFS at the default) fired no onset, so the sweep followed the
   drive and the throat held about 2 st down at RANGE −5 st (up to 6 st at −12 st) for the whole note, the
   plain follower the owner dropped. A sweep now starts on a note's first block with a drive above 0, and
   each onset restarts it. Deepthroat shares the law, so its throat no longer holds on soft notes either.
   The three picks against the shipping build (v2.01.74), on the cleaned clip 2: level within 0.004 dB;
   the mean shift within 0.01 st. The shift moves past 0.05 st on 2.0 % (decay 250 ms), 3.0 % (500 ms)
   and 4.6 % (SENSITIVITY 25 %) of the samples. On the clip's first note the quiet lead-in before the 0.5
   crossing now starts the ramp, so for 52 to 84 ms the throat opens up to 1 st less deep, then meets the
   old path within about 50 ms of the crossing. Elsewhere the moves are single blocks of 0.05 to 0.11 st,
   where the whole-semitone hand-off rounds one block the other way. The shipping build already differed
   from the third build's files before this pass (−0.18 to −0.25 dB RMS, every sample, also with POLISH COMP
   and VOX EQ off), so log entry 4's bit-identical match no longer holds; the voice path moved in later
   passes, not bisected. Levels at the default: +0.27, +0.08, +0.22 dB.
