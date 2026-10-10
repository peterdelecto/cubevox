# Astra packet: switched input pad, relay vs analog switch (2026-10-09)

Answer in ONE sentence. Do not read any files; every fact you need is here.

## Facts
- Live vocal effects box. Mic: Shure SM58 (1.9 mV rms/Pa), almost always on an
  XLR-to-1/4" TS cable into the 1/4" contacts of a Neutrik NCJ6FA-H combo jack.
- Analog on a single 5 V rail, signal nodes biased at VREF = 2.5 V. Op-amps OPA2197.
- 1/4" input network per leg: 10 uF electrolytic, 22 kOhm series, into node IN_x
  which has 1.2 kOhm to VREF. This forms a 25.7 dB pad for pedals/line.
  XLR legs skip the 22 kOhm and land on the same IN_x nodes.
- Preamp: fixed 20 dB instrumentation stage, then 0 to 30 dB on a pot. ADC PCM1808,
  full scale 3 V p-p.
- Problem: the SM58 on the TS cable goes through the pad, costing ~25 dB of
  signal-to-noise against the XLR path. Fix: a MIC/PEDAL menu item switches the pad
  out by shorting both 22 kOhm resistors (tip and ring legs).
- Candidate A: DPDT signal relay Omron G6K-2F-Y DC5, already on the board as the mute
  relay with an MMBT3904 driver, 1N4148W flyback. Coil ~28 mA when energized.
  Proposed rest state = pad shorted (MIC), energized = PEDAL.
- Candidate B: a dual SPST analog switch IC (e.g. TS5A23157 class) in the signal path,
  nodes at 2.5 V inside the 5 V rail. New part family, ~1 Ohm on-resistance.
- Switching happens only when the user changes a menu item, never during performance.

## Question
Relay or analog switch for shorting the 22 kOhm pad resistors on a 2 mV mic signal,
and why, in one sentence.
