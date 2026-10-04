# emulator

- The emulator mirrors the panel exactly. Dev-only controls live under a collapsed
  `Tuning` header. Never invent panel controls. Exception (owner 2026-10-01): every
  effect block carries an on/off checkbox in the prototype so stages can be A/B'd.
- Face layout (owner 2026-10-01): four columns, no scroll bar, window 1440x840 (the
  owner's laptop shows ~847 px of window). Each effect block has its own collapsed
  `Tuning` header directly below it; one Tuning header open per column at a time
  (accordion), which is the invariant the `--layout` probe checks. A footer strip holds the bottom-right "Pedal mode" (with BYPASS above it, dry input out,
  stage feedback still running)
  button (owner 2026-10-02): pedal mode draws each effect as a stompbox with only its
  panel knobs (encoders below knobs), name and on/off LED at the foot; the probe checks it fits.
- Stage feedback simulator is prototype-only test signal. It is calibrated so a bypassed
  box does not feed back at default Amount; DRIVE causes it.
