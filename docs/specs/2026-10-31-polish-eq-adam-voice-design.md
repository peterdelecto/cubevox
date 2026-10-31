# cubevox — POLISH EQ voiced for Adam's voice: de-esser, front filters, one toggle

Owner direction 2026-10-31: polish his voice, reduce sibilance, add a mild low-pass tailored to his
voice against feedback on stage, EQ where it helps; one ON/OFF row in the menu; roll it into
POLISH EQ; fold the HPF and LPF rows into it if they are still needed. Target: the H7 firmware,
48 kHz, 64-sample blocks, float32. This is a spec; nothing is built.

## Scope
POLISH EQ becomes one stage in two places under its one menu row. The half ahead of slot 1 holds a
high-pass, a low-pass and a de-esser. The half after POLISH COMP holds the 300 Hz dip, the 3.5 kHz
presence and the trim. The input conditioning block and the HPF and LPF menu rows go; their filters
move into the front half at fixed cutoffs chosen for his voice. The air shelf and the back half's
own high-pass go.

## His voice, measured
Three clips in `samples/`, mono 16-bit 48 kHz, 14 s, all on one note. Frames of 20 ms split into
voiced, ess and silent; pitch by YIN checked against the harmonic peaks.

| Item | Measured |
|---|---|
| Fundamental | G2, 97 Hz median, 90 to 108 Hz; harmonics at 97/193/289 Hz and 103/205/309 Hz |
| Energy | 50 % under 270 Hz, 90 % under 700 Hz, 99 % under 2.3 kHz, 99.9 % under 4.3 kHz; centroid 377 Hz |
| Harmonics | about 8 dB per octave down from 1 to 4 kHz, then a cliff from 3.5 to 5 kHz (−27 dB to −54 dB re total); nothing but breath above 4.5 kHz |
| Ess | 4.5 % of sung time; peak 5.8 kHz; 90 % of ess energy under 6.1 kHz; −10 dB by 6.5 kHz, −17 dB by 9 kHz |
| Top | nothing of his above 9 kHz; the recording is clean to 11 kHz (ess 40 to 50 dB over the floor), so the cliff is his voice, not the file |
| Today's chain on his fundamental | input HPF 120 Hz −5.2 dB plus POLISH EQ high-pass 110 Hz −4.2 dB, together −9.3 dB at 97 Hz on the clips |

## Chain position
```
ADC → input tap → POLISH EQ front (HPF 70 Hz → LPF 6 kHz → de-esser) → slot 1 Input gate
(detector on the front half's output) → slots 2 to 8 → NaN guard → POLISH COMP →
POLISH EQ back (300 Hz dip → 3.5 kHz presence → trim) → output trim → soft ceiling → output tap → DAC
```
Both halves follow the one `PolishParams::on` through the usual 20 ms active smoother with snap, so
one click fades both together and settled off is a bit-exact copy in both places. The BYPASS dry
copy is taken before the front half, where it is taken before the conditioning today. The Input
gate's detector keeps reading the signal after the front half, so `floor_dbfs` follows the row's state.

## Front half, ahead of slot 1

| Stage | Topology | Response | Why |
|---|---|---|---|
| High-pass | 4th-order Butterworth, 70 Hz, two biquads, Q 0.5412 and 1.3066 | −3.0 dB at 70 Hz, −0.3 dB at 97 Hz, −6.5 dB at 60 Hz, −12 dB at 50 Hz | the one high-pass for the whole box; keeps his G2, takes rumble, handling noise and plosives |
| Low-pass | 4th-order Butterworth, 6 kHz, two biquads, same Qs | −3.0 dB at 6 kHz, −2.4 dB at 5.8 kHz, −11.8 dB at 8 kHz, −21.5 dB at 10 kHz, −30.6 dB at 12 kHz | his voice ends at 4.5 kHz and his ess at 7 kHz; above that is hiss and where a wedge rings |
| De-esser | below | | |

### De-esser
| Item | Value |
|---|---|
| Detector | 2nd-order Butterworth band-pass 4.5 to 7 kHz on the de-esser's input; per block (64 samples, 1.33 ms) the RMS of the band and of the full signal, both in dB |
| Trigger | band level minus full level, so his volume does not matter; threshold −12 dB; no cut while the band is under −60 dBFS |
| Cut | the overshoot over the threshold, capped at 6 dB |
| Attack, release | attack within the block; release a 40 ms one-pole on the cut in dB |
| Action | peaking EQ at 5.8 kHz, Q 1.5, gain 0 to −6 dB, RBJ coefficients recomputed per block |
| Separation on the clips | voiced frames band-minus-full p95 −30 dB; ess frames p5 −11.6 dB, median −5.1 dB; silent frames hold the band under −90 dBFS |
| Cost | three biquads plus two log2 and one exp2 per block; under 1 % of 480 MHz, measured on the bench |

## Back half, after POLISH COMP

| Band | Value | Change |
|---|---|---|
| Low-mid dip | 300 Hz, −2.5 dB, Q 1.0 | unchanged; his loudest bands are 450 to 560 Hz, a higher dip would hollow him |
| Presence | 3.5 kHz, +1.5 dB, Q 0.7 | unchanged |
| Trim | `PolishTuning::trimDb`, re-set by `level_test` | about 1.2 dB; today 1.6 dB, and the new voicing is 0.36 dB hotter on the level test's tone |
| High-pass 110 Hz | removed | the front half has the one high-pass |
| Air shelf +1.5 dB at 10 kHz | removed | the low-pass has 10 kHz at −21.5 dB; the shelf only added hiss and feedback gain |

## On his clips, new against today
Both chains without POLISH COMP, trim excluded, the raw clips as reference.

| Band, frames | Today | New |
|---|---|---|
| Fundamental 85 to 115 Hz, voiced | −9.3 dB | −0.7 dB |
| Second harmonic 180 to 220 Hz, voiced | −2.4 dB | −1.4 dB |
| Body 300 to 700 Hz, voiced | −1.4 dB | −1.3 dB |
| Presence 3 to 4 kHz, voiced | +1.5 dB | +1.4 dB |
| Ess band 4.5 to 7 kHz, ess frames | +1.2 dB | −6.5 dB |
| 8 to 12 kHz, all frames | +1.0 dB | −16.4 dB |
| Ess frames, full RMS | −1.9 dB | −3.5 dB |
| Voiced frames, full RMS | −2.6 dB | −1.1 dB |

The de-esser cuts more than 1 dB during 90 % of ess frames, 9.5 % of voiced frames (the release
tail after an ess and breathy consonants) and 2 % of silent frames; mean cut on an ess 5.2 dB.
On its own it moves the voiced frames −0.001 dB and the program −0.004 dB.

## Menu
The HPF and LPF rows leave. Nine rows in the polish spec's order: INPUT, REVERB, POLISH COMP,
POLISH EQ, OUTPUT, TOP SWITCH, DEBUG, UPDATE FIRMWARE, EXIT. POLISH EQ ON / OFF drives both halves.
Nothing else on the menu changes.

## Settings and the layout record
`Settings` loses `hpf` and `lpf`. Record bytes 6 and 7 are retired, written as 0 and ignored on
read, so a record from today's firmware loads here and a record from this firmware loads on
today's as the defaults (0 reads as the default there). Schema stays v2. The protocol `settings`
object is unchanged; HPF and LPF were never in it.

## Engine
1. `engine/polish.h`: `Polish` runs as two calls, `processFront` and `processBack`, each with its
   own 20 ms active smoother on the one `on`. `PolishParams` loses `hpHz` and `airDb`;
   `PolishTuning` loses `airHz` and gains the de-esser tuning (band edges, threshold, max cut,
   release, notch Hz and Q). The front cutoffs are `constexpr`.
2. `engine/deesser.h`: the detector and the notch as one class owned by `Polish`. Float only,
   `std::array`, no heap or I/O, `-Werror` clean. The log2 and exp2 are the engine's fast ones.
3. `engine/conditioning.h` goes, with `cv::kHpfCutoffHz`, `cv::kLpfCutoffHz`, the default
   indices and its tests. `chain_run.h` calls `processFront` where it called the conditioning and
   `processBack` where it called `polish.process`.

## Emulator and render
`cubevox-render` loses `--hpf` and `--lpf`; `--polish-eq on|off` switches both halves. The
emulator's EQ module loses the HPF and Air sliders and runs the front half ahead of the slots and the
back half after the comp as the box does; the de-esser threshold and max cut join the tuning node.
The parity test holds because both run the same engine.

## Tests
1. `test/polish_test.cpp`: both halves bit-exact passthrough after 100 ms when off; front response
   by sine RMS out/in over seconds 1 to 2: 50 Hz ≤ −11 dB, 70 Hz −3.0 ± 0.3 dB, 97 Hz 0 ± 0.5 dB,
   1 kHz 0 ± 0.1 dB, 6 kHz −3.0 ± 0.3 dB, 10 kHz ≤ −20 dB; back response: 300 Hz −2.5 ± 0.4 dB,
   1 kHz 0 ± 0.4 dB, 3.5 kHz +1.5 ± 0.4 dB; no allocation in either call.
2. `test/deesser_test.cpp`: a 1 kHz tone at −20 dBFS gets under 0.1 dB of cut; a 5.8 kHz tone at
   −20 dBFS alone gets 6.0 ± 0.3 dB; 5.8 kHz at −30 dBFS over 1 kHz at −20 dBFS gets 1.6 ± 0.5 dB;
   the cut falls under 1 dB within 100 ms of the ess ending; a band under −60 dBFS never cuts; no
   NaN or denormal on silence.
3. `level_test`: the `polish EQ defaults` row holds 0 to +0.5 dB with the new trim; the
   conditioning rows go.
4. `test/firmware/core_record.cpp`: bytes 6 and 7 ignored on read and written 0; a record from
   today's firmware loads.
5. `test/firmware/ui_menu.cpp`: the nine rows in order, each click, the saved settings, proofs
   regenerated for the owner.
6. `cubevox-render` on the three clips with `--polish-eq on` and `off`: the ess band 4.5 to 7 kHz
   down 5 to 7 dB on ess frames, voiced frames moved under 0.1 dB by the de-esser alone.
7. Bench: load % of the whole stage; the SM58 into a wedge, ON against OFF, gain before ring as a
   listen check; Adam's esses by ear.

## Open items for the owner
1. Octave at −12 puts his G2 at 48 Hz. Today's two high-passes cut that note 30 dB; the one 70 Hz
   high-pass cuts it 13 dB, so the octave-down fundamental comes through 17 dB stronger. By ear
   with the Octave card on.
2. The de-esser's 6 dB cap, by ear on the render of his clips.
