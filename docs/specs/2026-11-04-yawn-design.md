# Yawn: the throat drops on every syllable (2026-11-04)

Owner 2026-11-04: "for now, just call it yawn and write the spec. we will change the name later."

## Purpose

A card that moves Adam's throat with his singing, the way an envelope filter pedal moves a bass
player's filter with the pluck. Each syllable drops the formants into a bigger, darker throat and
lets them come back while the note still holds. The pitch never changes. The owner heard three
renders of the third audition build and picked them as the sound: clip 2 at SENSITIVITY 50 % and
RANGE −7 st with a 250 ms decay, the same with a 500 ms decay, and SENSITIVITY 25 % with −7 st at
250 ms. The formants going up read as an alien voice and are out; +3 st was the most the owner
wanted to hear, and the three picks all go down.

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
| lower | RANGE | 0 to −7 st | −5 st | how far the throat drops on a full sweep; 0 is no effect, the amount knob of this slot |

The readout shows RANGE in semitones with its sign. DECAY, the time a sweep takes to come back, is
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
5. Sweep. An onset starts an envelope at the follower's drive, which decays to 0 over DECAY while
   the note holds. A held note then stays at 0 shift until the next onset. A new onset restarts
   the envelope from the current drive.
6. Shift. `shiftSt = RANGE × envelope`, handed to the Formant voice as its shift, which glides it
   over 20 ms inside the pitch stage. That glide is the only smoothing on the shift.
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
   end while the note holds; the shift rests within 0.01 st of 0 after it;
   off is a bit-exact copy with `==`; output finite at every knob corner; RANGE maps 0 and −7 st
   at the pot's ends.
2. The level row on the clips at the default.
3. The layout tests: Yawn is slot 3's third choice, one voice with Formant, in the pitch group.
4. Formant and Octave hashed identical on the three clips before and after any change to the card.

## Cost and memory

`kFormantFollower` {4, 5}, estimated as the Formant card's {3, 4} plus one point for the follower
(1.5 ns a sample at most on the host, 4.8k board cycles a block). The sweep, the detector and the
LFO-free law cost nothing per sample. DTCM unchanged at 64,064 B; the card adds 200 B of D1 SRAM.
The row stays `false` until a stress run sweeps SENSITIVITY and RANGE.

## What ships and what does not

1. Ships: the pluck law above, SENSITIVITY and RANGE, DECAY as tuning, wet only.
2. Out, after the audition: the plain follower mode (the throat opens and stays open on a held
   note; the owner heard it as static), the free-running LFO and the envelope-gated LFO (heard,
   not picked), the upward range (+3 st at most was the owner's ceiling; up reads as an alien
   voice; the third build's +3 renders were not picked), MIX. The code for the three unpicked modes
   comes out in the build that makes this spec real; the render tool keeps keys only for what
   ships. The audition WAVs that carried them are listed in the log.

## Done when

`ctest` passes with the card's rows; the emulator's slot 3 switch reads OCTAVE / FORMANT / YAWN and
the readout shows YAWN with SENSITIVITY and RANGE; the three picked renders are reproduced by the
shipping build within 0.1 dB and 0.05 st of shift; the owner listens on the box.

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
