# Sympath (was Strings), tuning decisions

Status: decided with the owner 2026-11-04; build in progress.

## Decided
1. Name: Sympath everywhere, glass SYMPATH. Code id stays `strings`.
2. The metallic clang stays; the owner hears it as authentic.
3. The fault to fix is clashing notes.
4. Sympath does not rely on the Autotune card or its key (owner 2026-11-04). It works the same with
   or without Autotune in the chain.
5. Its own pitch tracking can be simple; it matters less here than in the pitch cards.

6. The strings follow the held note (owner's option A). A note counts as held when the card's own
   tracker stays within 0.5 st of one semitone for 150 ms. The strings then retune to that note's
   root and fifth over four octaves, from the root at or above 110 Hz: 8 strings. No 3rd, so no
   major or minor guess.
7. Decided by the lead:
   1. Retune: the new bank builds as today, takes the voice, and the old bank loses its send and
      fades over 200 ms, so a ringing string dies away instead of cutting off.
   2. Before the first held note the strings are silent; unvoiced frames keep the last tuning.
   3. 8 strings, down from 12; the 4 dropped strings cut the card's cost by a third.
   4. The tracker is the card's own and cheap: under 1 % of the processor on the box.
