# cubevox — the ways the box can fail on stage, and what it does about each

Owner direction 2026-10-31: a robust way to ensure the box cannot die during a performance.
Rewritten 2026-11-13 from the owner's rules of that day, after an audit of the earlier text
(`build/audit/stage-survival-audit.md`, 23 findings). Target: the H7 firmware on the v4 board,
48 kHz, 64-sample blocks, float32. Board changes are out of scope.

## The owner's rules
1. No more cards are being added. The design case is the costliest combination of today's cards,
   and the plan is "don't let the box fail".
2. After a fault the box comes back as it was. Adam never has to stop and set anything up again,
   and never has to act in the middle of a song.
3. The software always matches the panel. After any restart the toggles and pots are read again
   and what the panel shows is what the box plays.
4. A stage box comes back in the state the musician expects. It shows no fault-finding screen and
   has no reduced mode.
5. A box that fails to start before a gig is a different case from a box that restarts in the
   middle of a song. A restart mid-song never ends in update mode.
6. An error shows Adam a code or a plain word on the screen (owner 2026-11-10). `OVERLOAD` is the
   word for load.
7. Update mode is reached from the menu's `Update firmware` item. Holding MENU at a start does
   nothing (owner 2026-11-13).

## Where the box stands (measured 2026-11-13, v2.01.90, board 1)
The costliest combination the menu builds is Input gate, Autotune, Apraxia, Mob, Echo, Deepthroat,
Lo-fi and Spring Reverb. `firmware/stress.py --layouts costliest` ran it with every slot on,
CLEANUP EQ at STRONG, every knob swept and the toggles flipped, twice for 181 s: mean block
34.1 %, worst block 44.1 %, 0 late blocks, 0 overruns (`build/bench/2026-11-13-stress.md`). The
limits are 75 % and 95 %. The stress run gates that layout before every release.

## 1. What the box does

### R. Restart: every fault that stops the box mid-song
Causes. A hard fault, a hang that the watchdog ends, an audio stream that cannot be recovered
(A1 to A3), audio that does not start.
What Adam gets. The sound drops, and about a second later it is back with the same layout, the
same settings and the panel as it stands. The screen shows `ERROR` and a code for 6 s, then the
voice screen. He does nothing.

1. Warm or cold. A start is warm when the boot block's reset flags hold SFTRSTF, IWDG1RSTF or
   WWDG1RSTF, hold neither PORRSTF nor BORRSTF, and the power sense has a reading with no trip.
   Every other start is cold and keeps today's 6 s before the relay closes, because the pop it
   prevents is real. PINRSTF decides nothing; this chip sets it on every reset. The rule is
   `core/restart_rule.h`, pure. The bootloader's `cvBootPowerOn` uses the same flags but is not
   the same test: a bare NRST pulse is cold to the application and not a power-on to the
   bootloader. Nothing depends on the two agreeing.
2. Order. On every start `setup()` runs the mute pins, the power sense, the fault record, the
   mid-song mark (item 8), the record read and then audio. The panel, USB and the menu come
   after audio. `faultWatchdogKick()` stays between the long steps.
3. Knobs. On a cold start the mux selects wait 1100 ms for U107 to bring 3V3A up. On a warm start
   3V3A never went down, so the scan starts at once. The first snapshot needs all 16 pots and the
   toggles read. Until it is in, the blocks pass silence and run no card, so nothing processed
   with default settings is left in a delay line or a reverb tail.
4. Relay. On a warm start the mute sequence skips the 6 s. MUTE_N goes high once the clocks run
   and the first snapshot is in, then 20 ms for the relay and the 50 ms fade up.
5. Input relay. A reset leaves PAD_EN an input, so K103 drops to MIC through every restart.
   `setup()` drives K103 to the saved setting on every start, warm or cold, while the output is
   still muted, as today.
6. Flash. The reset before a restart keeps today's wait (`ResetWait`): it goes at once when both
   stores are idle, waits out a running erase, and gives up after 3 s with no erase running. An
   erase is never cut. A fault during an erase costs one longer gap, 1 to 2 s more, and no repair
   at the next start.
7. Audio that does not start. When `audioInit()` fails on a warm start the box does not sit silent.
   It resets again with its own code. On a cold start three such starts end in "Startup failed."
   as today.
8. Never update mode mid-song. The bootloader counts starts that do not run audio for 5 s, and
   three in a row end in update mode. That rule is for a box that cannot start after power-on or
   after an update. Once the box has run audio for 5 s, the application writes a mark into the
   boot block's application area: a magic word, the CRC of the running image from its header, and
   a check word. The bootloader zeroes that block at power-on. The first thing `setup()` does on a
   box-caused reset (SFTRSTF, IWDG1RSTF or WWDG1RSTF, no PORRSTF or BORRSTF) is read the mark; when
   it is whole and names the running image, `setup()` zeroes the failed-start counter. A new
   image has another CRC, so after an update the mark does not count and a bad update still ends
   in "Startup failed." and the drive.
9. MENU at a start does nothing. The bootloader no longer reads MENU. Update mode is entered by
   the menu's `Update firmware` item or the `enter_dfu` request, by an image that fails its
   check, or by three failed starts. A box whose application cannot reach its menu is recovered
   through the ROM DFU with BOOT0. Bootloader v0.09.
10. Codes. The splash shows a code for every box-caused restart, on the rows A1 to A3 use, with
    one plain line of what happened:

    | Code | Cause |
    |---|---|
    | `ERROR A1` | the audio stream faulted four times inside a minute |
    | `ERROR A2` | the audio stream could not be restarted |
    | `ERROR A3` | the audio stopped for 100 ms |
    | `ERROR A4` | the audio did not start |
    | `ERROR C1` | the processor faulted (a crash) |
    | `ERROR W1` | the watchdog ended a hang |

    On a warm start the splash stays 6 s and ends early when Adam touches a knob, a toggle or
    MENU. Sound is already back while it shows. DIAG.TXT gets the same code in its entry.
11. Report. The boot report prints warm or cold and, on a warm start, the ms from the start of
    `setup()` to the first block, to the first snapshot and to the relay.

Bound. Gap ≤ 1.5 s from the last good block to the first good block with no erase running,
measured by the drills. The path, with today's figures where they exist: stall detect 100 ms
(A3 only), one loop pass, the flash wait (ms when idle), serial flush ≤ 100 ms, mute for reset
25 ms, reset and bootloader (unmeasured, 25 to 40 ms expected), `setup()` to the first block
(target ≤ 400 ms), the 16 pots, XSMT lead 5 ms, relay 20 ms, fade 50 ms. For A1 the three stream
restarts before the reset each mute as well; that is today's behaviour and stays.

What a restart does not bring back.
1. A menu change made in the last 500 ms before the fault. The record is saved 500 ms after the
   last change, or at once when the menu closes.
2. An open menu. The box comes back on the splash and then the voice screen.

A hang. When `loop()` hangs, the block interrupt keeps running and the sound continues for the
2 s until the watchdog; then the restart. When an interrupt handler hangs (USB, the encoder),
the blocks stop too, so Adam gets up to 2 s of silence and then the restart. The block interrupt
is the lowest priority, 15; USB, the encoder and the DMA and SAI interrupts all preempt it.

A fault that repeats. The box restarts the same way each time, as it was. No card is switched off
and nothing is asked of Adam. The stress run exercises every card on every layout before a
release, and that is where a card that faults is caught.

### O. Overload: the load stays over the limit
This is a net for a catastrophic fault. With today's cards the worst block is 44.1 % and the box
does not reach it.
Today. Every chain block is late, so blocks alternate dry and effected every 1.3 ms. A buzz, and
nothing recovers until the load falls.
What Adam gets. He hears one tick and a card drop out, his voice never stops, and the top row
reads `OVERLOAD: MOB`. Two seconds later the card is back and the row is blank. He does nothing.

1. Window. The block interrupt keeps, for the last 48 blocks that ran the chain (64 ms), whether
   each was late and how many cycles each slot took. A dry block and a layout-fade block are not
   counted. The window is emptied after each step, on a stream restart and on a layout apply.
2. Trip. 12 late blocks inside the window raise a trip, which `loop()` takes on its next pass. The
   buzz makes every chain block late, so it trips inside 32 ms. One or two late blocks from a
   burst of interrupts never trip.
3. Which card. The rule takes out the running card whose slot took the most cycles in the window's
   worst block. The Input gate is never taken out. Autotune is passed over while a card behind it
   reads its tracker (Formant or Apraxia directly behind it, Mob, Deepthroat), because its engine
   keeps running for them. CLEANUP EQ, the soft ceiling and the BYPASS mix are not cards.
4. How. The hold is one more term where a slot is switched (`compose.cpp`, beside the bench's
   forced-off mask), and the card goes out through its slot's 8 ms fade. Steps are at least 50 ms
   apart.
5. Coming back. Each held card has its own wait, counted from when it was held: 2 s. When the wait
   is up and the window holds no late block, the card comes back. If it is taken out again its
   wait doubles, up to 16 s, and it keeps coming back at 16 s for as long as the fault lasts. A
   card that has stayed in for 60 s starts again at 2 s. No card is ever left out for good.
6. Parity. A hold ends when the card's toggle goes off, when the layout changes and at any
   restart. The only time a card is off with its toggle up is while the row says `OVERLOAD`.
7. The row. Text row 0 of the voice screen, 5x7, moving with the burn-in walk, blank unless a card
   is held. One card: `OVERLOAD: ` and its word, the word its SLOT row shows in the menu, or
   `AUTOTUNE` and `ECHO` for the two cards with no SLOT row. The longest, `OVERLOAD: DEEPTHROAT`,
   is 20 columns. More than one card: `OVERLOAD: 3 CARDS`. Proof by number.
8. The bottom. With every card it may take out held, the rule does nothing more. If the load is
   still over, the buzz stays until the fault ends; the cause is then outside the cards.
9. Reports. `held` (a bitmask of held slots) and `steps` join the status reply. DIAG.TXT gets one
   line per step, in the entry's text field under the audio kind, naming the card and the late
   count. The boot report's audio line prints the steps since boot.

### Unchanged
1. One block runs long. Counted late; the next block passes the dry input; one tick. It counts
   toward the overload window only if 11 more follow inside 64 ms.
2. An engine produces NaN or infinity, or a block past +60 dBFS. The block is silenced, every
   engine and CLEANUP EQ are cleared over about 13 silent blocks (17 ms), and the chain fades back
   in over 50 ms, as it was. No card is switched off.
3. Brownout or power loss. The PVD interrupt drops the mute relay at once; the return is a cold
   start with its 6 s.
4. A flash write during a song. Bank 2 is written while code runs from bank 1, chunked and polled
   from `loop()`; a write cut by a reset lands in the store's recovery (layout-record spec).
5. USB plugged or unplugged, a dead panel, a corrupt or over-budget layout at start, a hot input
   or DC: as today.
6. The processor dies. K102 mutes. Nothing on the board passes the mic around a dead processor.

## 2. Host tests
1. `core/restart_rule.h`: warm against cold from the reset flags and the power sense.
2. The mid-song mark, pure: whole and naming the running image zeroes the counter; a broken check
   word, another image's CRC or a power-on does not.
3. `core/mute_sequence`: the warm path skips the 6 s and still waits for the clocks, the first
   snapshot and the holds.
4. The splash: each code's text fits 21 columns; 6 s on a warm start with a code, ended by a touch.
5. `core/load_rule.h` and its window: 11 late blocks do not trip and 12 do; dry and fade blocks
   are not counted; the card with the most cycles goes; the Input gate never; Autotune passed over
   with a reader behind it; 50 ms spacing; the waits 2, 4, 8, 16, 16 s; back to 2 s after 60 s in;
   a toggle off, a layout apply and a stream restart end holds or empty the window.
6. `test/firmware/ui_*`: the `OVERLOAD` row for one card, the longest word and several cards.
7. `core_protocol`: the two new status fields.
8. Bootloader selftest: MENU down at a start changes no decision.

## 3. Drills, on the bench, every release
Bench ops, so they exist only in the bench image and only over the serial port. `stop_audio`,
`spin` and `fault` are built. The new ops are `burn` (cycles spun inside each block) and
`fail_audio_start` (the next `audioInit()` fails). `bringup.py --survival` runs the table from the
Mac with no hands on the box, on the costliest layout, and writes the measured column to the
board's log in `hardware/bringup/`. The boot report's `[boot] restart code` line carries the code the
splash shows. The bad update is the bench image under another header version, so another CRC,
installed through `enter_dfu` (the path the menu item takes) and faulted with `fault` at each start.

| Case | Drill | Expected | Measured |
|---|---|---|---|
| A3 | `stop_audio` | restart, `ERROR A3`, the same layout and panel | gap |
| W1 | `spin` | sound for 2 s, restart, `ERROR W1` | gap after the watchdog |
| C1 | `fault` | restart, `ERROR C1`, DIAG.TXT entry | gap |
| C1 repeated | `fault` five times, 2 s apart, after 10 s of running | the same layout and panel after each, never update mode | gap, five times |
| A4 | `fail_audio_start`, then `fault` | a second restart with `ERROR A4`, then sound | gap |
| Bad update | an image that faults 1 s after its start, installed from the menu | third start is "Startup failed." and the drive | starts |
| Overload | `burn` sized to make every chain block late on the costliest layout | one step inside 120 ms, the slowest card out, the row reads `OVERLOAD:` and its word, voice continuous | time to step |
| Overload ends | `burn 0` | the card back 2 s after it went, the row blank | time to return |
| Overload stays | the burn left on for 60 s | waits of 2, 4, 8, 16, 16 s, no restart, no mute | steps |
| One late block | `burn` for one block | one tick, no step | late count |
| Save under load | 20 menu saves during a burn at 80 % | no late block | late count |

## 4. Set aside
1. The safe start (every card off after a failed start, a remembered card). The owner's rule 4.
2. A card held until its toggle is flipped, and the word `HOLD`. The owner's rules 2 and 6.
3. The per-slot NaN check. It could leave the voice silent where today's guard costs one cut.
4. The 200 ms cap on the flash wait. It cut erases.
5. MENU held at a start as a way into update mode. The owner's rule 7.
