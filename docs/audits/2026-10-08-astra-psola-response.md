**3. At constant period and ratio 1, nothing here produces a sustained active-grain count much larger than two.**

A grain lasts `2*p` samples and launches occur approximately every `p` samples. One wrinkle: **launch happens before expired grains are deactivated**. An expiring grain can still have `active == true` during slot selection, so you can briefly see three flagged grains. `play()` retires the expired one before reading audio. Floating-point timing can also produce occasional boundary overlap.

More grains are possible when:

- **Period falls quickly:** existing grains retain their old, longer `len`, while new launches use shorter spacing. Occupancy can reach eight, after which launches replace a slot.
- **Ratio exceeds 1:** steady occupancy is approximately `2*r`, capped at eight.

For ordinary finite audio periods, `age += 1.0f` and `age >= len` reliably terminate grains. Exceptions worth distinguishing:

- `period <= 0` returns immediately, **freezing existing grains** until valid processing resumes.
- A NaN `len`, if invalid input reaches it, defeats the termination comparison.
- Unrealistically large lengths can encounter float integer precision limits: an age increment can stop advancing at `2^24`.

Neither `markDelay_` nor the fixed playback `delay` controls retirement. With finite, constant audio-range `p` and `r == 1`, persistent occupancy near eight would indicate behavior outside the shown normal path.