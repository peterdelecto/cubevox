# cubevox — six small firmware additions: slot crossfade, DC blockers, ceiling order, late blocks, brownout, power-on

Owner list 2026-10-30, after board 1's first week on the bench. Six changes to how the box
treats the edges of its signal path: a toggle flip, a block that runs late, the rails going
down, the rails coming up. Each is small and gets its own host test where the host can reach it.

## 1. Slot toggle crossfade
Today a slot's bypass toggle switches the slot's output hard between the engine and the dry
copy, which clicks on a loud note. Now the flip crossfades over 8 ms (6 blocks of 64 samples),
equal power (`sin` / `cos` over the fade, the same law as the BYPASS mix in `core/chain_run`).
During the fade the engine runs and both paths mix; at the end of a fade out the engine stops
running (no CPU for a bypassed slot, as today); on a fade in the engine is cleared first, so a
re-engaged reverb or delay starts from silence and not from a tail it held while bypassed. A
flip during a fade turns the fade round from where it is. The per-slot fade level lives in the
audio interrupt's state, advanced per block; the main loop only publishes the toggle. Host test:
`core_chain_run` drives a slot flip on a steady tone and checks the output has no step larger
than the fade allows, that the mix sums to unity power at the midpoint, that the engine runs
for exactly the fade blocks after a flip to off, and that a flip back on starts from a cleared
engine.

## 2. DC blockers after Octave and the reverb
Octave's granular shifter and the reverb's loops can carry a DC offset into the compressor,
which then pumps on it. Each gets a one-pole high-pass at 5 Hz on its output, inside the engine
header (`engine/octave.h`, `engine/reverb.h`) so the emulator and `cubevox-render` carry it too.
Distortion keeps its own at 10 Hz. Host tests: the engine's test feeds a 0.5 DC step and
checks the output settles under -60 dBFS within 200 ms, and a 100 Hz tone passes within 0.05 dB;
the level rule rows of both cards stay within their bands.

## 3. Soft ceiling ahead of the output trim
Today the output stage applies the trim and then the ceiling, `softCeiling(a[i] * gain)`. With
MIC's -30 dB trim the ceiling never touches the signal, so a clipped chain reaches the DAC at
-30 dB with its clip intact. Now the ceiling shapes the full-scale signal first and the trim
scales the result, `softCeiling(a[i]) * gain`; the clamp stays after both. LINE (0 dB) is
unchanged bit for bit. Host test in the output stage's test: a 0 dBFS square through MIC lands at
the ceiling's shoulder times the trim, and LINE output equals today's.

## 4. A late block passes the dry input
Today a block past its 640 000-cycle budget is counted and everything keeps running, so the
next block starts late too and the DMA can overrun. Now a late block sets a flag; the next block
skips the chain and passes the dry input (through the output stage as BYPASS does, with the
gain ramp and the ceiling), clears the flag, and counts `dry_blocks`. The engines' state goes
one block stale, which is inaudible against the overrun it prevents. `dry_blocks` joins the
status reply beside `late`, and the boot report's audio line prints it. Host test in `core_load`
or `core_chain_run`: with the late flag raised the next block's output equals the dry input
through the output stage, the counter reads 1, and the block after runs the chain.

## 5. Brownout drops the mute relay at once
Today the mute on power loss waits for the main loop to read 5VA on VA_SENSE and trip under
4.75 V. The H7's programmable voltage detector (PVD) on VDD, level 7 (2.9 V, the highest step),
raises EXTI line 16; its interrupt pulls MUTE_N low in the first instruction, writes the mute
holder flag the main loop already honours (`power_trips`), and returns. The main loop then
runs the ordinary power-fail path (XSMT low, hold, release after 5VA recovers) as today. The
PVD interrupt sits at a priority above the audio block (priority 1) so it runs inside a block.
No host test; the firmware builds through the warn gate, and bench 1 below shows it on the
board.

## 6. Power-on mute order
Today the mute release is MUTE_N high, 20 ms for the relay, then XSMT high, so the DAC's
unmute step reaches the output after the relay has closed. Now the release is XSMT high first,
then at least 100 ms of audio blocks (75 blocks) with the DAC running into an open relay, then
MUTE_N high. The mute order stays XSMT low, 5 ms, MUTE_N low, so a wanted mute has no relay
click. The 500 ms hold after the SAI clocks start stays; the 100 ms sits inside it when both
apply. Host test in `core_mute_sequence` (or wherever `mute_sequence.h` is tested): the release
steps come in the new order with the 75-block wait, and the mute steps are unchanged.

## Costs and budget
Items 1 and 2 add work: six blocks of double running per flipped slot (bounded, not steady), and
two one-pole filters per block. No `costs.h` card row changes by more than a point; the agent
measures with the host bench method in `costs.h` and says what moved.

## Bench (owner)
1. Brownout: 9 V pulled with the box singing; the relay opens before any thump reaches the PA.
2. Power-on: no pop at the PA on the slide switch.
3. A slot toggle flipped on a held note: no click.
4. MIC output with Distortion at full DRIVE: no square edge at the DAC.
