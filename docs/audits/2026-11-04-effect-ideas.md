# Effect ideas for the box (2026-11-04)

Owner's ask: a list of effects that would (1) make this a more well-rounded vocal processing
box, (2) make it an interesting exotic sound-design effector, (3) process live input into wild,
novel, never-before-heard musical effects. Words only; nothing here is built or planned.

Every card keeps the slot's shape: two pots and a bypass toggle, with the amount knob on the
lower pot. Costs are estimates from the M7 cost of the parts each idea reuses (a biquad is about
0.15 % of a block, a PSOLA voice about 1.5 %, a cubic delay read about 0.5 %, a 512-point FFT and
its inverse every block about 2.5 %). One small effect is 4 % of a block. Today's headroom per
layout is in `build/audit/2026-11-04-cpu-savings.md`. Memory: about 66 KB of DTCM and roughly
280 KB of D1 SRAM are free, so about 1.5 s of 16-bit audio fits today; the H743's D2 SRAM banks
(288 KB) would double that if the linker brings them in.

What the library already covers: Input gate, Autotune, Octave, Formant, Unison (a chorus), Mob
(eight copies, so a doubler at one or two), Echo (slapback to 250 ms), Distortion (a BD-2), Gate,
Lo-fi, Chatter (word stutter), Spring and Chasm reverbs, with POLISH COMP, POLISH EQ and its
de-esser fixed in the tail. Harmony was removed by the owner on 2026-10-08; it is listed below
once, for completeness, and only the owner reopens it.

## 1 A more well-rounded vocal box

| # | Card | What it does to the voice | Pots | Cost | Reuses |
|---|---|---|---|---|---|
| 1.1 | Plate reverb | The vocal standard: dense, bright, no spring boing, no cavern. A Dattorro plate. | Time, Mix | 1.5 | reverb_parts |
| 1.2 | Tape delay | A longer echo, up to 1 s, with wow and flutter and a soft-saturating feedback path. Repeats darken and smear like tape. | Time, Feedback | 0.5 | Echo's line, Unison's LFO |
| 1.3 | Compressor card | A per-slot squash for a pop vocal, ahead of the fixed FIRM comp. Threshold and ratio on the pots. | Threshold, Amount | 0.3 | polish_comp |
| 1.4 | Auto level | A slow gain rider that holds Adam's level when he moves off the mic. Range and speed. | Range, Speed | 0.1 | gate's detector |
| 1.5 | Exciter | A high-passed harmonic generator mixed under the voice for air and presence without EQ boost. | Tune, Amount | 0.3 | distortion's knee |
| 1.6 | Saturation | Tape or tube warmth at gentle drive with a tilt tone, where Distortion starts too hard. 2x oversampled. | Warmth, Drive | 0.8 | distortion |
| 1.7 | Vibrato | A pitch wobble with no dry, for a singer who wants it added or exaggerated. | Rate, Depth | 0.3 | unison |
| 1.8 | Flanger | A short modulated delay with feedback. The jet sweep on a voice. | Rate, Depth | 0.3 | unison |
| 1.9 | Phaser | Six allpass stages under an LFO. Softer than the flanger, sits well on held notes. | Rate, Depth | 0.2 | dsp |
| 1.10 | Harmony | A diatonic third or fifth above or below in the Autotune KEY. One PSOLA voice. Removed 2026-10-08; owner's call. | Interval, Mix | 1.5 | pitch stage, scale |
| 1.11 | Phrase looper | Hold the toggle to record a phrase, release to loop it under the live voice. Memory-bound: 1.5 s today, 3 s with the D2 banks. | Level, Decay | 0.3 | a ring |

## 2 An exotic sound-design effector

| # | Card | What it does | Pots | Cost | Reuses |
|---|---|---|---|---|---|
| 2.1 | Freeze | Holds the last 100 ms as a granular cloud for as long as the toggle is on. The note hangs like a pad. | Blur, Mix | 0.5 | psola grains |
| 2.2 | Shimmer reverb | Chasm with a +12 st shift in its feedback. Every repeat climbs an octave into a choir of light. | Time, Mix | 2.0 | chasm, one PSOLA voice |
| 2.3 | Pitch-tracked ring modulator | The carrier sits at a ratio of the sung pitch, so the clang stays harmonic and follows the melody. | Ratio, Mix | 0.2 | tracker |
| 2.4 | Frequency shifter | A Hilbert allpass pair shifts every partial by the same Hz. Metallic, inharmonic, barber-pole at small shifts. | Shift, Mix | 0.2 | dsp |
| 2.5 | Vocoder, internal carrier | Sixteen band pairs; the carrier is a saw or pulse at the tracked pitch, or a fixed chord. The robot voice. | Tone, Mix | 1.0 | tracker, biquads |
| 2.6 | Pitch-tracked resonator | A comb bank tuned to the sung pitch and its harmonics that rings on after each note. | Decay, Mix | 0.3 | tracker, delays |
| 2.7 | Vowel filter | A formant bank sweeping A, E, I, O, U under an LFO or the input envelope. Imposes a vowel on whatever is sung. | Vowel, Rate | 0.3 | biquads |
| 2.8 | Wavefolder | West-coast folding on a voice: new harmonics that fold back as the level rises. 2x oversampled. | Fold, Mix | 0.5 | distortion's oversampler |
| 2.9 | Reverse echo | Grains played backwards into the delay. The swell-before-the-note sound. | Time, Mix | 0.5 | Echo's line |
| 2.10 | Tape stop | On the toggle the pitch dives to zero over Time; off, it spins back up. | Time, Depth | 0.3 | a delay read |
| 2.11 | Envelope filter | A resonant low-pass driven by the input envelope. Auto-wah on a voice. | Sense, Resonance | 0.1 | gate's detector, biquad |
| 2.12 | Sub octave synth | An analogue-style square an octave down, gated by the voice envelope. Fatter than Octave's shifted copy. | Tone, Mix | 0.1 | tracker |
| 2.13 | Spectral blur | A 512-point FFT every block; phases randomised or magnitudes averaged across frames. The voice becomes a smear of itself. | Blur, Mix | 1.0 | a new FFT |
| 2.14 | Whisper | The same FFT with every phase randomised each frame. The voice turns to breath but keeps its words. | Air, Mix | 1.0 | the FFT |
| 2.15 | Howl machine | A controlled self-feedback path with a limiter. Mic-into-amp feedback with no amp, safe by the limiter. | Gain, Tone | 0.1 | polish_comp |

## 3 Wild, novel, live-input effects

These lean on what the box already knows about the voice as it arrives: the pitch, the key, the
word boundaries from the Input gate, and the gate's detector. Nothing on a stompbox treats a
voice this way today.

| # | Card | What it does | Pots | Cost | Reuses |
|---|---|---|---|---|---|
| 3.1 | Scale-climbing echo | Each repeat is re-pitched one scale degree up (or down) in the Autotune KEY, so a held note walks away up the scale as it fades. | Steps, Mix | 1.0 | PSOLA voice in the feedback, scale |
| 3.2 | Counterpoint echo | Repeats are shifted to the notes Adam sang two or three notes ago, from a short pitch history, so the delay answers his own melody. | Lag, Mix | 1.0 | tracker history, PSOLA voice |
| 3.3 | Held-note chord | Each flip of the toggle freezes the current note as a grain loop, up to four; the box holds a chord of Adam's own vowels under his live line. | Decay, Mix | 1.0 | four grain loops |
| 3.4 | Consonant and vowel split | A voiced / unvoiced detector routes consonants and vowels to different treatments: consonants dry and sharp with vowels in shimmer, or consonants exploding into noise bursts while vowels stay dry. | Split, Amount | 0.3 plus what it feeds | gate's detector, zero crossings |
| 3.5 | Synth shadow | The tracked pitch and the voice envelope drive an FM or sub synth under the voice. Adam plays a bass or a lead by singing. | Tone, Mix | 0.3 | tracker |
| 3.6 | Interval triggers | A leap larger than a fifth up fires a stutter or a dive; a leap down fires a reverse grain. The melody plays the effects. | Threshold, Amount | 0.5 | tracker, Chatter's slices |
| 3.7 | Slow motion | Real-time granular stretch at 1.5x to 3x that lags behind the live voice and catches up during the gaps between phrases. | Stretch, Mix | 0.7 | psola grains, a 2 s ring |
| 3.8 | Reverse word | Each word is played backwards the moment it ends, overlapping the next. The word detector exists in Chatter. | Length, Mix | 0.5 | Chatter's word key |
| 3.9 | Sympathetic strings | Twelve Karplus-Strong strings tuned to the key, excited by the voice, ringing on like a sitar's sympathetic set. | Decay, Mix | 0.5 | short delays, scale |
| 3.10 | Vocal arpeggiator | Slices at a set rate, each re-pitched through a pattern (root, third, fifth, octave) in the key. A sung note becomes a sequence. | Rate, Pattern | 1.0 | Chatter's slices, PSOLA voice, scale |
| 3.11 | Breath synth | Breath and unvoiced passages below the gate become a noise-driven wind instrument; sung notes stay dry. | Tone, Amount | 0.2 | gate's detector |
| 3.12 | Harmonic split | A comb pair at the sung pitch separates odd and even harmonics; each half goes to its own treatment, odd dry and even into reverb. | Balance, Mix | 0.3 | tracker, combs |
| 3.13 | Self canon | The voice against itself from 1 to 3 s ago, re-tuned to the current key, let through only when the earlier phrase was pitched. | Delay, Mix | 1.0 | a 16-bit ring, PSOLA voice, tracker confidence |
| 3.14 | Vowel swap | The live pitch wearing the formant envelope from a few seconds ago. Adam hears his earlier vowel shapes on his current note. | Delay, Mix | 1.5 | the FFT or an LPC envelope |

## Owner's picks, to do (owner 2026-11-04)

1. Pitch-tracked ring modulator (2.3).
2. Sub octave synth (2.12), reconceived as a growl voice like the vocal on SOPHIE's "Whole New
   World": a gross, gnarly monster. What makes that sound, and what the box has for each part:
   the voice an octave or more down with the formants dropped too (Octave and the Formant voice
   both exist); a subharmonic rasp, the period-doubled fry of a death-metal growl, made by
   amplitude or ring modulation at half or a third of the tracked pitch (a phase accumulator at
   f0/2 or f0/3, nearly free); then hard saturation or folding with the tone tilted to the low
   mids (Distortion's oversampler and knee); and a slow gate so the growl breathes. The card is
   the rasp, the fold and the tilt with Growl (subharmonic ratio and depth, half to a third to
   chaotic) and Drive on the pots; Octave and Formant ahead of it in the chain do the drop, or
   the card carries its own pitch-down voice at about 1.5 % more.
3. Sympathetic strings (3.9).
4. Formant modulation by the envelope (owner 2026-11-04): the formant offset of a voice follows
   the input envelope, so a louder note opens the throat and a soft one closes it. Applied to a
   Mob-like card (at four copies or fewer, each copy with its own depth and sign) or to the growl
   voice (the fold gets a throat that opens with the note). The Formant voice's playback rate is
   the lever; the Input gate's detector or a one-pole on the block's RMS is the envelope.

Item 4 was built three times on 2026-11-04 and became the Yawn card; its spec is
`docs/specs/2026-11-04-yawn-design.md`. Growl (item 2) was built once for audition
(`build/audit/report-growl-audition.md`, on main at v2.01.19) and is not yet judged.

## Mob 2, audition build (owner 2026-11-04)

Owner: build Mob 2 with per-copy vibrato (rate and depth), pitch drift and onset jitter, at
fewer copies; audition it in the emulator; drop or develop from there. No deep build. What Mob
already does: a slow random pitch walk per copy (`driftCents` 15 cents, new target every 2 s)
and a lag re-drawn at every gap. What Mob 2 adds: a vibrato per copy with its own rate, depth
and onset delay; a wider lag and level re-draw at every word onset; four tuned copies and no
raised copies. Brief: `build/audit/brief-mob2-audition.md`. The spec is written after the
audition if the card survives.

## What the three lists share
Ideas 2.3, 2.5, 2.6, 2.12, 3.1, 3.2, 3.5, 3.6, 3.10 and 3.13 all ride the pitch tracker that
Autotune, Octave, Formant and Mob already pay for, so each costs its own voice or filter bank
and nothing for the pitch. Ideas 2.13, 2.14 and 3.14 need one FFT the engine does not have
yet; the first of them pays for it and the others share it. Ideas 1.11, 3.7 and 3.13 are bound by
RAM before CPU.
