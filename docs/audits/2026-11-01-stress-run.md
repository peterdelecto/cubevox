## 02:36:27 stress run, fw v2.00.81, 2 run(s), pass 1 read 20 s, layouts default, crowd, chasm, crowd-chasm

### default

Cards, slot 1 to 8: input_gate, autotune, octave, unison, slapback, distortion, gate, reverb_spring.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 21.9k | 3.4 % | 5 % | ok |
| 2 | autotune* | 75.8k | 11.8 % | 14 % | 107.5k | 16.8 % | 16 % | FAIL worst |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.3k | 0.0 % | 5 % | ok |
| 4 | unison | 21.1k | 3.3 % | 8 % | 26.6k | 4.2 % | 8 % | ok |
| 5 | slapback | 13.1k | 2.1 % | 5 % | 13.7k | 2.1 % | 5 % | ok |
| 6 | distortion | 45.5k | 7.1 % | 8 % | 64.7k | 10.1 % | 10 % | FAIL worst |
| 7 | gate | 18.6k | 2.9 % | 4 % | 22.2k | 3.5 % | 5 % | ok |
| 8 | reverb_spring | 82.9k | 13.0 % | 15 % | 88.6k | 13.8 % | 15 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 311.7k (48.7 %), worst 356.3k (55.7 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 312.1k (48.8 %), worst 365.4k (57.1 %), late 0, overruns 0, dry 0
- run 2: block mean 311.3k (48.6 %), worst 356.3k (55.7 %), late 0, overruns 0, dry 0

### crowd

Cards, slot 1 to 8: input_gate, autotune, octave, crowd, slapback, distortion, gate, reverb_spring.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.9k | 2.8 % | 4 % | 22.2k | 3.5 % | 5 % | ok |
| 2 | autotune* | 75.9k | 11.9 % | 14 % | 105.9k | 16.5 % | 16 % | FAIL worst |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.3k | 0.0 % | 5 % | ok |
| 4 | crowd | 79.0k | 12.3 % | 13 % | 99.6k | 15.6 % | 15 % | FAIL worst |
| 5 | slapback | 13.4k | 2.1 % | 5 % | 13.9k | 2.2 % | 5 % | ok |
| 6 | distortion | 46.7k | 7.3 % | 8 % | 64.5k | 10.1 % | 10 % | FAIL worst |
| 7 | gate | 18.9k | 3.0 % | 4 % | 23.5k | 3.7 % | 5 % | ok |
| 8 | reverb_spring | 83.3k | 13.0 % | 15 % | 88.7k | 13.9 % | 15 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 371.8k (58.1 %), worst 419.9k (65.6 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 371.5k (58.0 %), worst 424.9k (66.4 %), late 0, overruns 0, dry 0
- run 2: block mean 372.1k (58.1 %), worst 419.9k (65.6 %), late 0, overruns 0, dry 0

### chasm

Cards, slot 1 to 8: input_gate, autotune, octave, unison, slapback, distortion, gate, reverb_spring; the reverb slot runs Chasm.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.7k | 2.8 % | 4 % | 21.7k | 3.4 % | 5 % | ok |
| 2 | autotune* | 75.7k | 11.8 % | 14 % | 106.9k | 16.7 % | 16 % | FAIL worst |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.3k | 0.0 % | 5 % | ok |
| 4 | unison | 21.2k | 3.3 % | 8 % | 21.9k | 3.4 % | 8 % | ok |
| 5 | slapback | 13.1k | 2.0 % | 5 % | 13.7k | 2.1 % | 5 % | ok |
| 6 | distortion | 45.7k | 7.1 % | 8 % | 67.7k | 10.6 % | 10 % | FAIL worst |
| 7 | gate | 18.5k | 2.9 % | 4 % | 22.8k | 3.6 % | 5 % | ok |
| 8 | reverb_spring | 102.2k | 16.0 % | 16 % | 105.5k | 16.5 % | 16 % | FAIL worst |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 330.7k (51.7 %), worst 374.8k (58.6 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 331.0k (51.7 %), worst 375.0k (58.6 %), late 0, overruns 0, dry 0
- run 2: block mean 330.4k (51.6 %), worst 374.8k (58.6 %), late 0, overruns 0, dry 0

### crowd-chasm

Cards, slot 1 to 8: input_gate, autotune, octave, crowd, slapback, distortion, gate, reverb_spring; the reverb slot runs Chasm.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 23.8k | 3.7 % | 5 % | ok |
| 2 | autotune* | 75.9k | 11.9 % | 14 % | 107.6k | 16.8 % | 16 % | FAIL worst |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.3k | 0.0 % | 5 % | ok |
| 4 | crowd | 79.0k | 12.3 % | 13 % | 101.6k | 15.9 % | 15 % | FAIL worst |
| 5 | slapback | 13.4k | 2.1 % | 5 % | 15.9k | 2.5 % | 5 % | ok |
| 6 | distortion | 46.8k | 7.3 % | 8 % | 65.5k | 10.2 % | 10 % | FAIL worst |
| 7 | gate | 18.8k | 2.9 % | 4 % | 25.9k | 4.0 % | 5 % | ok |
| 8 | reverb_spring | 102.5k | 16.0 % | 16 % | 108.2k | 16.9 % | 16 % | FAIL mean and worst |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 390.9k (61.1 %), worst 439.0k (68.6 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 390.3k (61.0 %), worst 439.0k (68.6 %), late 0, overruns 0, dry 0
- run 2: block mean 391.4k (61.2 %), worst 439.6k (68.7 %), late 0, overruns 0, dry 0

gates: 20 of 33 passed
  FAIL default: slot 2 autotune worst within its row (16.8 % over 16 %)
  FAIL default: slot 6 distortion worst within its row (10.1 % over 10 %)
  FAIL crowd: slot 2 autotune worst within its row (16.5 % over 16 %)
  FAIL crowd: slot 4 crowd worst within its row (15.6 % over 15 %)
  FAIL crowd: slot 6 distortion worst within its row (10.1 % over 10 %)
  FAIL chasm: slot 2 autotune worst within its row (16.7 % over 16 %)
  FAIL chasm: slot 6 distortion worst within its row (10.6 % over 10 %)
  FAIL chasm: slot 8 reverb_spring worst within its row (16.5 % over 16 %)
  FAIL crowd-chasm: slot 2 autotune worst within its row (16.8 % over 16 %)
  FAIL crowd-chasm: slot 4 crowd worst within its row (15.9 % over 15 %)
  FAIL crowd-chasm: slot 6 distortion worst within its row (10.2 % over 10 %)
  FAIL crowd-chasm: slot 8 reverb_spring mean within its row (16.0 % over 16 %)
  FAIL crowd-chasm: slot 8 reverb_spring worst within its row (16.9 % over 16 %)

## 02:38:46 stress run, fw v2.00.81, 1 run(s), pass 1 read 10 s, layouts crowd-chasm

### crowd-chasm

Cards, slot 1 to 8: input_gate, autotune, octave, crowd, slapback, distortion, gate, reverb_spring; the reverb slot runs Chasm.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.9k | 2.8 % | 4 % | 25.1k | 3.9 % | 5 % | ok |
| 2 | autotune* | 75.9k | 11.9 % | 14 % | 107.4k | 16.8 % | 16 % | FAIL worst |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.2k | 0.0 % | 5 % | ok |
| 4 | crowd | 78.8k | 12.3 % | 13 % | 103.6k | 16.2 % | 15 % | FAIL worst |
| 5 | slapback | 13.4k | 2.1 % | 5 % | 13.9k | 2.2 % | 5 % | ok |
| 6 | distortion | 46.6k | 7.3 % | 8 % | 65.1k | 10.2 % | 10 % | FAIL worst |
| 7 | gate | 18.8k | 2.9 % | 4 % | 23.0k | 3.6 % | 5 % | ok |
| 8 | reverb_spring | 102.5k | 16.0 % | 16 % | 108.0k | 16.9 % | 16 % | FAIL mean and worst |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 390.4k (61.0 %), worst 439.4k (68.7 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 390.4k (61.0 %), worst 439.4k (68.7 %), late 0, overruns 0, dry 0

gates: 5 of 10 passed
  FAIL crowd-chasm: slot 2 autotune worst within its row (16.79 % over 16 %)
  FAIL crowd-chasm: slot 4 crowd worst within its row (16.19 % over 15 %)
  FAIL crowd-chasm: slot 6 distortion worst within its row (10.17 % over 10 %)
  FAIL crowd-chasm: slot 8 reverb_spring mean within its row (16.01 % over 16 %)
  FAIL crowd-chasm: slot 8 reverb_spring worst within its row (16.88 % over 16 %)

## 02:51:48 stress run, fw v2.00.82, 2 run(s), pass 1 read 20 s, layouts default, crowd, chasm, crowd-chasm

### default

Cards, slot 1 to 8: input_gate, autotune, octave, unison, slapback, distortion, gate, reverb_spring.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 21.6k | 3.4 % | 5 % | ok |
| 2 | autotune* | 75.9k | 11.9 % | 14 % | 107.5k | 16.8 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.2k | 0.0 % | 5 % | ok |
| 4 | unison | 21.1k | 3.3 % | 8 % | 21.7k | 3.4 % | 8 % | ok |
| 5 | slapback | 13.2k | 2.1 % | 5 % | 13.8k | 2.1 % | 5 % | ok |
| 6 | distortion | 45.6k | 7.1 % | 8 % | 65.2k | 10.2 % | 11 % | ok |
| 7 | gate | 18.7k | 2.9 % | 4 % | 21.3k | 3.3 % | 5 % | ok |
| 8 | reverb_spring | 82.9k | 13.0 % | 15 % | 88.6k | 13.8 % | 15 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 311.8k (48.7 %), worst 353.6k (55.3 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 312.1k (48.8 %), worst 353.6k (55.3 %), late 0, overruns 0, dry 0
- run 2: block mean 311.4k (48.7 %), worst 354.6k (55.4 %), late 0, overruns 0, dry 0

### crowd

Cards, slot 1 to 8: input_gate, autotune, octave, crowd, slapback, distortion, gate, reverb_spring.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.9k | 2.8 % | 4 % | 23.8k | 3.7 % | 5 % | ok |
| 2 | autotune* | 76.0k | 11.9 % | 14 % | 106.1k | 16.6 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.2k | 0.0 % | 5 % | ok |
| 4 | crowd | 78.9k | 12.3 % | 13 % | 98.3k | 15.4 % | 16 % | ok |
| 5 | slapback | 13.4k | 2.1 % | 5 % | 14.1k | 2.2 % | 5 % | ok |
| 6 | distortion | 46.8k | 7.3 % | 8 % | 65.6k | 10.3 % | 11 % | ok |
| 7 | gate | 18.9k | 3.0 % | 4 % | 21.4k | 3.4 % | 5 % | ok |
| 8 | reverb_spring | 83.3k | 13.0 % | 15 % | 88.8k | 13.9 % | 15 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 371.8k (58.1 %), worst 420.4k (65.7 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 371.5k (58.0 %), worst 427.6k (66.8 %), late 0, overruns 0, dry 0
- run 2: block mean 372.1k (58.1 %), worst 420.4k (65.7 %), late 0, overruns 0, dry 0

### chasm

Cards, slot 1 to 8: input_gate, autotune, octave, unison, slapback, distortion, gate, reverb_spring; the reverb slot runs Chasm.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 22.4k | 3.5 % | 5 % | ok |
| 2 | autotune* | 75.8k | 11.8 % | 14 % | 106.6k | 16.7 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.2k | 0.0 % | 5 % | ok |
| 4 | unison | 21.2k | 3.3 % | 8 % | 24.1k | 3.8 % | 8 % | ok |
| 5 | slapback | 13.1k | 2.1 % | 5 % | 16.3k | 2.5 % | 5 % | ok |
| 6 | distortion | 45.8k | 7.2 % | 8 % | 66.1k | 10.3 % | 11 % | ok |
| 7 | gate | 18.5k | 2.9 % | 4 % | 21.9k | 3.4 % | 5 % | ok |
| 8 | reverb_spring | 102.2k | 16.0 % | 17 % | 107.9k | 16.9 % | 17 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 330.8k (51.7 %), worst 372.6k (58.2 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 331.2k (51.7 %), worst 372.6k (58.2 %), late 0, overruns 0, dry 0
- run 2: block mean 330.5k (51.6 %), worst 373.8k (58.4 %), late 0, overruns 0, dry 0

### crowd-chasm

Cards, slot 1 to 8: input_gate, autotune, octave, crowd, slapback, distortion, gate, reverb_spring; the reverb slot runs Chasm.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.9k | 2.8 % | 4 % | 24.1k | 3.8 % | 5 % | ok |
| 2 | autotune* | 76.0k | 11.9 % | 14 % | 107.2k | 16.8 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.2k | 0.0 % | 5 % | ok |
| 4 | crowd | 78.9k | 12.3 % | 13 % | 99.7k | 15.6 % | 16 % | ok |
| 5 | slapback | 13.4k | 2.1 % | 5 % | 16.7k | 2.6 % | 5 % | ok |
| 6 | distortion | 47.0k | 7.3 % | 8 % | 66.2k | 10.3 % | 11 % | ok |
| 7 | gate | 18.8k | 2.9 % | 4 % | 22.4k | 3.5 % | 5 % | ok |
| 8 | reverb_spring | 102.5k | 16.0 % | 17 % | 107.6k | 16.8 % | 17 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 391.0k (61.1 %), worst 440.0k (68.7 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 390.4k (61.0 %), worst 442.5k (69.1 %), late 0, overruns 0, dry 0
- run 2: block mean 391.6k (61.2 %), worst 440.0k (68.7 %), late 0, overruns 0, dry 0

gates: 20 of 20 passed

## 03:02:45 stress run, fw v2.00.82, 2 run(s), pass 1 read 20 s, layouts default, crowd, chasm, crowd-chasm

### default

Cards, slot 1 to 8: input_gate, autotune, octave, unison, slapback, distortion, gate, reverb_spring.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 22.1k | 3.5 % | 5 % | ok |
| 2 | autotune* | 75.9k | 11.9 % | 14 % | 107.3k | 16.8 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.2k | 0.0 % | 5 % | ok |
| 4 | unison | 21.1k | 3.3 % | 8 % | 23.7k | 3.7 % | 8 % | ok |
| 5 | slapback | 13.2k | 2.1 % | 5 % | 13.7k | 2.1 % | 5 % | ok |
| 6 | distortion | 45.6k | 7.1 % | 8 % | 67.7k | 10.6 % | 11 % | ok |
| 7 | gate | 18.7k | 2.9 % | 4 % | 21.5k | 3.4 % | 5 % | ok |
| 8 | reverb_spring | 82.9k | 13.0 % | 15 % | 88.6k | 13.8 % | 15 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 311.8k (48.7 %), worst 355.4k (55.5 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 312.1k (48.8 %), worst 357.5k (55.9 %), late 0, overruns 0, dry 0
- run 2: block mean 311.4k (48.7 %), worst 355.4k (55.5 %), late 0, overruns 0, dry 0

### crowd

Cards, slot 1 to 8: input_gate, autotune, octave, crowd, slapback, distortion, gate, reverb_spring.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 22.0k | 3.4 % | 5 % | ok |
| 2 | autotune* | 76.0k | 11.9 % | 14 % | 106.9k | 16.7 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.3k | 0.0 % | 5 % | ok |
| 4 | crowd | 79.0k | 12.3 % | 13 % | 101.3k | 15.8 % | 16 % | ok |
| 5 | slapback | 13.4k | 2.1 % | 5 % | 14.0k | 2.2 % | 5 % | ok |
| 6 | distortion | 46.8k | 7.3 % | 8 % | 64.5k | 10.1 % | 11 % | ok |
| 7 | gate | 18.9k | 2.9 % | 4 % | 23.6k | 3.7 % | 5 % | ok |
| 8 | reverb_spring | 83.3k | 13.0 % | 15 % | 88.9k | 13.9 % | 15 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 371.9k (58.1 %), worst 421.4k (65.8 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 371.2k (58.0 %), worst 421.4k (65.8 %), late 0, overruns 0, dry 0
- run 2: block mean 372.6k (58.2 %), worst 423.0k (66.1 %), late 0, overruns 0, dry 0

### chasm

Cards, slot 1 to 8: input_gate, autotune, octave, unison, slapback, distortion, gate, reverb_spring; the reverb slot runs Chasm.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 23.3k | 3.6 % | 5 % | ok |
| 2 | autotune* | 75.8k | 11.8 % | 14 % | 107.2k | 16.8 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.3k | 0.0 % | 5 % | ok |
| 4 | unison | 21.2k | 3.3 % | 8 % | 26.6k | 4.2 % | 8 % | ok |
| 5 | slapback | 13.2k | 2.1 % | 5 % | 13.7k | 2.1 % | 5 % | ok |
| 6 | distortion | 46.0k | 7.2 % | 8 % | 67.3k | 10.5 % | 11 % | ok |
| 7 | gate | 18.6k | 2.9 % | 4 % | 21.3k | 3.3 % | 5 % | ok |
| 8 | reverb_spring | 102.3k | 16.0 % | 17 % | 107.8k | 16.8 % | 17 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 331.3k (51.8 %), worst 375.7k (58.7 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 331.5k (51.8 %), worst 379.3k (59.3 %), late 0, overruns 0, dry 0
- run 2: block mean 331.0k (51.7 %), worst 375.7k (58.7 %), late 0, overruns 0, dry 0

### crowd-chasm

Cards, slot 1 to 8: input_gate, autotune, octave, crowd, slapback, distortion, gate, reverb_spring; the reverb slot runs Chasm.

| slot | card | mean cycles | mean % | row mean % | worst cycles | worst % | row worst % | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | input_gate | 17.8k | 2.8 % | 4 % | 23.7k | 3.7 % | 5 % | ok |
| 2 | autotune* | 76.0k | 11.9 % | 14 % | 106.8k | 16.7 % | 17 % | ok |
| 3 | octave | 0.1k | 0.0 % | 5 % | 0.3k | 0.0 % | 5 % | ok |
| 4 | crowd | 79.0k | 12.3 % | 13 % | 99.7k | 15.6 % | 16 % | ok |
| 5 | slapback | 13.4k | 2.1 % | 5 % | 14.0k | 2.2 % | 5 % | ok |
| 6 | distortion | 46.9k | 7.3 % | 8 % | 65.2k | 10.2 % | 11 % | ok |
| 7 | gate | 18.8k | 2.9 % | 4 % | 22.9k | 3.6 % | 5 % | ok |
| 8 | reverb_spring | 102.6k | 16.0 % | 17 % | 108.2k | 16.9 % | 17 % | ok |

`*` Slot 2 runs the shared pitch stage, so its row is the sum of the Autotune and Octave rows placed, plus the tracker row when Autotune or Crowd is placed.

block total: mean 391.1k (61.1 %), worst 440.5k (68.8 %) of 640.0k, late 0, overruns 0, dry 0

- run 1: block mean 390.6k (61.0 %), worst 441.3k (69.0 %), late 0, overruns 0, dry 0
- run 2: block mean 391.6k (61.2 %), worst 440.5k (68.8 %), late 0, overruns 0, dry 0

gates: 84 of 84 passed
