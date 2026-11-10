# cubevox — the ways the box can fail on stage, and what it does about each

Owner direction 2026-10-31: a robust way to ensure the box cannot die during a performance.
Target: the H7 firmware on the v4 board, 48 kHz, 64-sample blocks, float32. This is a spec;
nothing is built.

Settled: a card the box turns off stays off until Adam flips its toggle off and on. The box never
turns a card back on by itself. Board changes are out of scope.

## How to read this
One entry per failure. Each gives how it arises, what Adam hears today, what he hears after this
spec, the bound the box must meet, and the drill that proves it (section 4). A bound without a
measured number in the run log is not met.

## 1. Failures

### F1. One block runs long
How. A worst-case block in Autotune, Octave or Reverb lands with an interrupt on top. Rare and
single.
Today. The block is counted late; the next block passes the dry input; the chain resumes. One tick.
After. Unchanged. The late block also counts into the load rule's window (F2).
Bound. One tick, no mute.
Drill. `burn 700000` for one block.

### F2. The load stays over the limit
How. A layout near the 95 % line with knobs at their costliest positions, a firmware regression, or
an input that drives a tracker hard.
Today. Every chain block is late, so blocks alternate dry and effected every 1.3 ms. A buzz. The
dry blocks hand back enough time that the overrun mute never trips, so nothing recovers until the
load falls.
After. The load rule. A rolling window of 48 blocks (64 ms) counts late blocks. Two late blocks
inside the window turn off the engaged card with the highest `worst` in the cost table, through the
slot's 8 ms equal-power fade. POLISH COMP, POLISH EQ and the Input gate are never turned off.
Steps are at least 50 ms apart. The bottom is every card off, his dry voice through the Input
gate, POLISH COMP and POLISH EQ. A card the rule turned off stays off for the rest of the power
cycle; Adam brings it back by flipping its toggle off and on, and if the load is still over the
rule steps it off again with one tick. A layout apply or a card swap clears the holds, because that
path restarts the chain anyway. A 5x7 row on the voice screen names the cards held, `HOLD REVERB
OCTAVE`, row chosen on the proof by number.
Bound. His voice at every moment, one tick per step, never a buzz, the mute relay never drops
for load.
Drill. `burn 200000` (about 103 %), `burn 400000` (about 134 %), a ramp 0 to 400 000 over 10 s.

### F3. An engine produces NaN or infinity
How. A numeric edge in one card.
Today. The block is silenced and every engine is cleared, so every tail and every tracker starts
cold. One silent block and a cold chain.
After. The NaN guard names the slot. The load rule turns that card off at once, with no window,
because an engine that produces NaN is broken, not busy. The other cards keep their state.
Bound. One silent block, then his voice through the other cards.
Drill. `nan <slot>`.

### F4. The audio interrupt stops
How. A stuck DMA stream, a SAI error the stream did not ride through, a lock in the block
interrupt.
Today. Eight overruns in a row drop the mute relay; the stream restarts, the mute holds, then opens.
At most three restarts fit in any minute. Three cases reset the box, all by one path
(`faultAudioWatch`), each with an error code (owner 2026-11-10): A1, a fourth fault inside a
minute of three restarts (owner 2026-11-10); A2, a restart that fails; A3, a stall, where no block
finishes for 100 ms. The box mutes at once, writes one `audio` entry whose fault text names the
code and the cause (`A1 repeated stream faults`, `A2 stream restart failed`, `A3 audio stalled`),
waits for the flash to be idle with no cap, and resets. Every reset then takes the power-on path,
so the relay closes 6 s after the app starts. Through that wait the splash explains the error and
shows its code (`ERROR A1` to `ERROR A3`), so Adam sees why the sound went (OLED GUI spec,
Priority 11). A start that does not run
audio for 5 s counts as a failed start, so a stream that faults past the budget inside 5 s of every
start ends on the third start in "Startup failed." (update mode, until the power is cycled). A stream
that runs longer than 5 s before it faults resets the box again and again, with the 6 s gap each
time.
After. Warm restart. The reset cause in the boot block (`resetFlags`) with 5VA good decides the
path. SFTRSTF, IWDG1RSTF or WWDG1RSTF is warm; PORRSTF, BORRSTF or PINRSTF is cold and keeps
today's 6 s. On a warm restart `setup()` runs audio first (clocks, the record read, the engines,
SAI and DMA, XSMT high at zero output), then MUTE_N high at once, 20 ms for the relay, the 50 ms
fade up, and only then the panel, USB and the menu. The stall path's flash wait is capped at
200 ms; past that the box resets without the DIAG.TXT entry. The boot report prints the time from
reset to the first block and to the relay.
Bound. Gap ≤ 1.5 s from the last good block to the first good block, same layout and settings,
nothing pressed. Budget: stall detect 100 ms, flash wait ≤ 200 ms, reset and bootloader ≤ 20 ms,
setup to the first block ≤ 400 ms (target, measured), relay 20 ms, fade 50 ms.
Drill. `stall`.

### F5. loop() hangs
How. The panel's SPI, USB or a flash write waits forever.
Today. The audio interrupt keeps running, so the sound continues while the menu and screen freeze.
IWDG1 fires at 2 s and the box resets onto the 6 s path.
After. Same until the watchdog, then the warm restart of F4. The RX DMA interrupt sits above the
block interrupt and the block interrupt above everything else, so a stuck loop() never touches
audio before the watchdog.
Bound. Sound for the 2 s, then a gap ≤ 1.5 s.
Drill. `hang`.

### F6. A hard fault or a runaway
How. A bad pointer, a stack overflow, a corrupt table.
Today. The fault handler mutes and resets; the reason reaches DIAG.TXT. Then the 6 s path.
After. The warm restart of F4.
Bound. Gap ≤ 1.5 s.
Drill. `crash`.

### F7. A card crashes the box every time it runs
How. A fault inside one engine that the layout and his input reach each time.
Today. Crash, reset, the same layout, crash. After three starts that never reach healthy the
bootloader stays in update mode, a silent box showing the update screen until a power cycle.
After. Safe start. When `bootFails` is 1 or more at start, the application starts with every card
held and the screen row reads `HOLD ALL  RESTART`. Adam brings cards back one at a time with their
toggles. The application keeps one word in DTCM, which survives a system reset, holding the slot
most recently switched on by a toggle flip, or none. On a safe start that card stays held even after
a flip and the row names it, so one card costs one restart and cannot start a loop. A layout apply
or a power cycle clears the word. The bootloader's three-strikes rule stays; with safe start ahead
of it, three in a row means the dry chain itself fails, which only a new `.uf2` fixes.
Bound. One restart, then his dry voice, stable.
Drill. `crash-in <slot>`.

### F8. Brownout or power loss
How. The 9 V supply sags or is pulled.
Today. The PVD interrupt drops the mute relay at once; the power-fail path holds XSMT low until
5VA recovers; power-on returns on the 6 s path.
After. Unchanged. A cold reset keeps the 6 s because the pop it prevents is real.
Bound. Mute before garbage. Return in the power-on time.
Drill. 9 V pulled to 7 V, then cut.

### F9. A flash write during a song
How. A menu click saves the layout record while the chain runs.
Today. The record store writes bank 2 while code runs from bank 1, chunked and polled from loop().
A reset mid-write lands in the store's ECC recovery and append log (layout-record spec).
After. Unchanged, now drilled under full load and with a reset during the write.
Bound. No late block from a save. A record cut mid-write loads or falls back to the default layout.
Drill. 20 menu saves during `burn 300000`; `crash` during a save.

### F10. USB plugged or unplugged mid-song
How. A host attaches or drops.
Today. Handled in loop(). The USB interrupt sits at level 1, above the block interrupt at 15,
so USB traffic counts toward block time. The serial reader handles one line per loop() pass, so a host that
keeps sending cannot hold loop() into the watchdog.
After. Unchanged, drilled.
Bound. No late block.
Drill. Plug and unplug the host under load.

### F11. The panel fails
How. The OLED ribbon or the SH1106 stops answering.
Today. The driver's retry rule (OLED GUI spec) retries without blocking the loop beyond its budget.
After. Unchanged, plus `setup()` starts audio before the panel on every start (F4), so a dead panel
never delays the first block.
Bound. Sound continues with a blank screen. No late block.
Drill. `panel-fail`, forcing the driver's error path.

### F12. A door into update mode during a song
How. A hand on MENU.
Today. UPDATE FIRMWARE opens a confirm screen first. MENU held at power-on is the only other way in.
After. Unchanged, drilled.
Bound. No single click or hold leaves audio.
Drill. Click UPDATE FIRMWARE, cancel; hold MENU with the box running.

### F13. A corrupt or over-budget layout at start
How. Power lost during a save, a record from a bad app build, a layout past 75 % mean or 95 %
worst.
Today. The record is refused and the default layout loads; the reason is in the boot report.
After. Unchanged.
Bound. Sound with the default layout.
Drill. `corrupt-record`, then power cycle.

### F14. A hot input or DC
How. A shouted note, a plugged jack, a DC step from Octave or the reverb.
Today. The soft ceiling never passes 1.0, the 5 Hz DC blockers after Octave and the reverb,
flush-to-zero, the Input gate.
After. Unchanged.
Bound. No sample past full scale, no NaN.
Drill. A +6 dBFS burst; a jack plugged under load.

### F15. The processor dies
How. A hardware fault, a rail.
Today and after. K102 mutes. Nothing on the board passes the mic around a dead processor, so the
output is silent until the restart, and silent for good if it never restarts.
Bound. None on this board. Board changes are out of scope.
Drill. None.

## 2. Counters and reports
`held` (a bitmask of held slots), `steps` and `window_worst` join the status reply. The boot
report's audio line prints steps since boot and, on a warm restart, the time to the first block and
to the relay. DIAG.TXT gets one line per step with the card and the window's worst percent, and one
line per safe start naming the held card.

## 3. Host tests
1. `core/load_rule.h`, pure: window counts, the two-late trip, step order by `worst`, 50 ms
   spacing, the polish stages and the Input gate never held, a flip off and on releases a hold, a
   layout apply clears holds, a NaN slot is held at once.
2. `core/restart_rule.h`, pure: warm against cold from the reset flags and 5VA.
3. `core/safe_start_rule.h`, pure: `bootFails` ≥ 1 holds all; the DTCM word keeps its card held
   through a flip; a layout apply clears it.
4. `core_chain_run`: a NaN block names its slot.
5. `test/firmware/ui_*`: the HOLD row, proof by number.
6. `core_protocol`: the three new status fields.

## 4. Fault drills, on the bench, every release
A serial command set that works only while DEBUG is ON and a host is attached, so nothing on the
panel can reach it. Each drill records its number in the run log beside the version. A release
`.uf2` ships with a drill table at its version.

| Failure | Drill | Expected | Measured |
|---|---|---|---|
| F1 | `burn 700000` once | one dry block, one tick | ticks |
| F2 | `burn 200000` | one step inside 120 ms, Reverb held, voice continuous; Reverb stays held after `burn 0` until its toggle is flipped off and on | time to step, ticks |
| F2 | `burn 400000` | steps to the bottom at 50 ms spacing, voice continuous, no mute | steps, time to bottom |
| F2 | ramp 0 to 400 000 over 10 s | one step at a time, no mute, no buzz | steps, late count |
| F3 | `nan <slot>` | that card held at once, other tails intact | silent blocks |
| F4 | `stall` | mute inside 100 ms, reset, back with the same layout | gap |
| F5 | `hang` | sound for 2 s, watchdog, back | gap after the watchdog |
| F6 | `crash` | mute, reset, back; DIAG.TXT entry | gap |
| F7 | `crash-in <slot>` | one restart, all cards held, the slot named, no second crash after its flip | restarts |
| F8 | 9 V to 7 V, then cut | mute before garbage; return on the power-on path | gap on return |
| F9 | 20 saves during `burn 300000`; `crash` during a save | no late block beyond the rule's steps; the record loads or the default layout does | late count |
| F10 | host plug and unplug under load | no late block | late count |
| F11 | `panel-fail` | sound continues, blank screen | late count |
| F12 | UPDATE FIRMWARE then cancel; MENU held while running | audio unbroken | late count |
| F13 | `corrupt-record`, power cycle | default layout, reason in the boot report | — |
| F14 | +6 dBFS burst; jack plugged under load | nothing past full scale, no NaN | peak |

## 5. Costs
The load rule is a few compares per block and a 48-byte window. The rest costs nothing at run time.

## Open item for the owner
1. The 1.5 s bound in F4, or tighter, which forces `setup()` to be measured and trimmed.
