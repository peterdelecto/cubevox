# cubevox — hand-off packet for Codex: tuning defaults review

You are low on tokens. This packet is self-contained; do not open the repo. Answer in
the format at the end, under 600 words.

## Project in five lines

cubevox is a tabletop live vocal effects box for one singer, Adam Keith (SM58, loud
rock stage, not technical). Final target STM32H743 at 48 kHz; today it runs as a Mac
prototype he will use to pick which effects and knobs he wants. Each effect has a few
panel knobs (what he touches live) and a "Tuning" drawer of constants (what we set
once). Every stage is level-matched: at its default setting it outputs 0 to +0.5 dB
over its input, and the trim that achieves that lives in tuning (fields named `trimDb`
/ `wetDb`). Do not propose changes to trims; a test re-derives them.

Signal chain: Input gate → [Autotune → Octave → Harmony, one shared pitch tracker]
→ Unison → Slapback → Distortion (Boss BD-2 model) → Gate → Reverb (three engines)
→ Output EQ.

Panel knobs (not your concern unless a default knob position seems wrong):
Autotune KEY + RESPONSE · Octave SEMITONES + MIX (+FORMANT on engine B) · Harmony
KEY + MIX (voices Lower/Low/High/Higher in a menu) · Unison DEPTH · Slapback
INTENSITY · Distortion DRIVE + TONE · Gate THRESHOLD · Reverb TENSION + DWELL +
MIX · EQ in menu.

## Tuning parameters per module (current default → meaning)

**Input gate** (soft, always on): attack 1 ms, hold 100 ms, release 25 ms, range −12 dB,
knee 12 dB, hysteresis 6 dB, detector high-pass 120 Hz. Threshold knob default −40 dBFS.

**Gate** (after distortion): attack 1, hold 150, release 250 ms, range −40 dB, knee 12,
hysteresis 6, detector HP 120 Hz. Threshold default −40 dBFS.

**Pitch tracker** (shared): voiced threshold 0.15 (YIN normalised difference; lower =
stricter). Shifter (PSOLA, engines A grid / B epoch-aligned with formant / C granular):
grainPeriods 2 (1.5–3), epochSearch 0.25 period, epochLpHz 1000, grainWindowMs 40
(C, 20–80), grainCount 2 (C, 2 or 4).

**Autotune**: maxCorrectSemis 6 (never pull further than this), RESPONSE knob default
60 ms (5 = robotic snap, 200+ = gentle), engine B default.

**Octave**: levelDb 0, glideMs 15 (interval change glide), muteUnvoiced off. Knob
defaults 0 st, mix 50 %, engine A.

**Harmony**: voice levels low/med/high −12/−6/0 dB, glideMs 15 ("tracking speed"),
muteUnvoiced off, snapToScale off (harmony follows the singer's flatness), diatonic
interval tables per scale degree (Higher 7 7 7 7 7 7 6; High 4 3 3 4 4 3 3; Low −3 −3 −4
−3 −3 −4 −4; Lower −7 −7 −7 −6 −7 −7 −7), chromatic intervals −7/−4/+4/+7. Knob
defaults mix 50 %, menu High at medium, engine A.

**Unison** (two voices, chorus + fixed detune blended): base delay 20/15 ms, LFO
0.60/0.90 Hz, swing 0.5 ms at depth 0 → 3.0 ms at depth 1, detune +2/−2 cents, window
20 ms. DEPTH default 80 %.

**Slapback**: time 70 ms (30–120), lowpass 4 kHz, feedback 0.2 (0–0.5). INTENSITY 50 %.

**Distortion** (BD-2 model, two rail-saturating stages): input HP 20 Hz; stage 1 bass
shelf −12 dB below 700 Hz, LP 6 kHz, gain max 100×; tone stack bass +10 dB @400 Hz,
treble −5 dB @2 kHz, loss −20 dB; stage 2 HP 100 / LP 6 kHz, gain max 90×; rail
asymmetry 0.05, rail soft edge 0.1; fixed treble cut −6 dB above 1 kHz; TONE range
−12..+6 dB (noon flat); bass bump +6 dB @120 Hz Q 1; fade-in over first 5 % of DRIVE;
2× oversampling on. DRIVE default 30 %, TONE 50 %.

**Reverb SPRING** (Välimäki/Parker parametric, DrumSynthV3 port): input gain 0.5, HP
300 Hz, tension → |g| 0.60–0.97, dwell drive 32× with comp 0.80, high-band mix −22
(dwell 0) → −14 dB (dwell 1), ripple 0.10, splash diffuse 0 (0–0.9), hf sections 0
(0–200), springs 2 (or 3), wander 8 samples @3 Hz, boing 0 dB (95 Hz resonator).
Knobs: tension 55 %, dwell 35 %, mix 30 %.

**Reverb CHASM** (hexefx tank): time 0.35–0.86, treble loss 3 kHz, loop treble cut off,
input treble cut 0.95, bass cut 200 → 100 Hz with decay, wobble depth 192 samples,
rate 0.5–7 Hz, wobble level lift +2 dB. Knobs: decay 45 %, wobble 25 %, mix 30 %.

**Reverb PARKER SPRING** (DAFx-11 parametric, 3 springs): T_D 56 ms, F_c 4275 Hz,
100 sections a = 0.70, |g| 0.35–0.82, hf ratio 1.3, 189 hf sections a = −0.34, hf mix
−22 dB, cross 0.1, chirp EQ off, echo/ripple taps 0.2/0.2, wander 8 samples pole
0.93, spring detune T_D ×1.0/1.15/0.88 and F_c ×1.0/0.98/1.02, drive HP 150 / LP
9000 Hz, dwell drive 32× comp 0.80, presence +5 dB @3 kHz Q 1. Knobs: tension 50 %,
dwell 30 %, mix 30 %.

**Output EQ**: HP 90 Hz 12 dB/oct; low-mid dip 300 Hz −2.5 dB Q 1.0; presence 3.5 kHz
+1.5 dB Q 0.7; air shelf +1.5 dB above 10 kHz.

## The ask

For a live SM58 vocal on a loud stage, used by a non-technical singer who likes
"small amounts of several effects", recommend default **tuning** values (and knob
start positions where ours look wrong). Goals: no gate chatter, intelligible
distortion that does not fizz, reverb that reads as a spring without washing out,
harmonies that sound like other people, nothing he has to re-tune at a gig.

Answer format, nothing else:

```
MODULE
  field = value   // reason in ≤ 8 words
```

List only fields you would change. Skip trims. If you would leave a module alone,
write `MODULE: keep`. End with three one-line warnings about settings he should
never touch live.
