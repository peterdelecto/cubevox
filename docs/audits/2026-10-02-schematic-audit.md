# Cubevox schematic audit

Date: 2026-10-02. Status: **SCHEMATIC REVIEW COMPLETE — findings remain open**.

Scope: `cubevox.kicad_sch` and its `h7core_block.kicad_sch` child, including exported electrical connectivity, selected symbol/footprint pin maps, analog operating limits, power and interfaces. This is a schematic review, not PCB routing sign-off or bench validation. No design files have been changed.

**Disposition: fix A01–A04 before fabrication.** A clean ERC does not detect these analog and component-model problems. A05–A09 are medium-priority limitations/improvements; I01–I04 identify integration and documentation work. Confidence and conditional behavior are stated in each entry.

| Priority | Finding | Main consequence |
|---|---|---|
| High | A01: nonexistent input-jack detection switch | Audio tip connected to MCU GPIO and 3.3 V pull-up |
| High | A02: output-buffer input range too small | Intended signal swing leaves the specified operating range |
| High | A03: 500 nF directly on VREF follower | Reference instability/oscillation risk |
| High | A04: high DC gain without offset control | Lost headroom, possible saturation and bypass pops |
| Medium | A05: minimum gain and 200 Ω feedback load | Restricted pedal headroom and excessive reference loading |
| Medium | A06: analog undervoltage margin | USB-only/brownout audio operation not guaranteed |
| Medium | A07: X7R audio coupling | Distortion and microphonics risk |
| Medium | A08: unmatched difference-stage resistors | Limited common-mode/hum rejection |
| Medium | A09: omitted DAC external low-pass | Out-of-band noise/EMC performance needs validation |

## Checks completed

- KiCad 10.0.5 ERC, including errors, warnings and exclusions: **0 violations on both sheets**. The report lists four ignored check categories: single global labels, four-way junctions, SPICE models and footprint filters.
- Fresh XML netlist exported from the actual root schematic: 227 components, 214 nets, including no-connect nets.
- Compared the explicit pin/net assignments in both JSON specifications with exported connectivity: no mismatches among exported pins with explicit net assignments.
- Rendered and inspected both schematic sheets. The design is predominantly isolated symbols joined by net labels, which makes analog topology difficult to review visually.
- Read manufacturer documentation for OPA1678, PCM1808, PCM5102A, TPS2116, NCJ6FA-H and G6K.
- G6K relay pinout checked against Omron's diagram: coil 1 positive / 8 negative, commons 3/6, NC 2/7, NO 4/5. Schematic assignments and flyback-diode polarity agree.
- PCM5102A LDOO has the specified 100 nF capacitor; its grounded SCK is valid for BCK-derived PLL operation. PCM1808 mode pins select slave I2S; DAC format is also I2S.

## High-priority findings

### A01 — J101 has no plug-detection switch; the GPIO is connected to an audio terminal

**Confirmed component-model error.** J101 is NCJ6FA-H. The schematic labels pad 7 `T_SW`, connects it directly to MCU PB11 / `JACK_TRS_N`, and pulls it to 3.3 V through R107 (10 kΩ). Neutrik's drawing labels both relevant terminals **T**; its electrical diagram shows a single tip circuit and no detection switch.

The second tip terminal cannot supply the independent, active-low insertion indication assumed by the design. With the documented terminal mapping, the physical connector joins `TRS_T` and `JACK_TRS_N`, putting the MCU input and its pull-up directly on the external audio tip, ahead of C121. This adds an unintended DC feed/load and exposes the GPIO to external bipolar audio and transients. A low-impedance, DC-coupled pedal might happen to pull the signal low, but that is source-dependent and is not plug detection.

**Action:** correct the symbol/pad model and remove the GPIO/pull-up from the tip. Use an actual independent detection contact or explicit input selection. Continuity-check a sample to document the duplicate tip terminals before ordering.

Locations: `cubevox.kicad_sch:6076` (J101), `:8176` (R107).

Sources: [Neutrik electrical diagram, page 1](https://www.neutrik.com/en/product/ncj6fa-h.pdf), [manufacturer PCB terminal drawing](https://www.neutrik.com/media/8443/download/Drawing%20NCJ6FA-H.pdf?v=4).

### A02 — OPA1678 input common-mode range is incompatible with the intended output swing

**Confirmed operating-range violation at intended signal levels.** U106B is a unity buffer on 5VA, with `BUF_IN` centred on VREF ≈ 2.5 V. The OPA1678 input range is V− + 0.5 V through V+ − 2 V: approximately 0.5–3.0 V on a 5 V supply.

That leaves only 0.5 V positive input headroom, or about **0.354 Vrms** for a centred sine, before leaving the specified input range. R122/R123 instead deliver 0.6 × the DAC output: about **1.26 Vrms**, reaching approximately **4.28 V** on positive peaks. The bypass input has the same limitation. Rail-to-rail output does not solve the input limitation. U105B also needs its PRE1 input swing checked at minimum gain.

**Action:** select an amplifier whose input range supports the complete signal envelope at the minimum supply, or redesign supply/bias/gain. Recalculate every stage rather than changing only the output buffer.

Locations: `cubevox.kicad_sch:10743` (U106), `:9215` (U105), `:12733` (R122), `:12844` (R123).

Source: [TI OPA1678 datasheet, electrical characteristics](https://www.ti.com/lit/ds/symlink/opa1678.pdf).

### A03 — VREF buffer directly drives 500 nF; stability is not established

**Confirmed excessive capacitive load; oscillation is a risk, not a measured result.** U106A output and feedback input share `VREF`. C122, C126, C131, C133 and C140 each connect 100 nF directly from that net to ground: **500 nF total**, with no output isolation resistor.

TI characterizes capacitive drive in the hundreds of picofarads and recommends isolating heavier loads. The present load can destabilize the unity follower and contaminate all analog stages through their shared reference.

**Action:** design a stable reference distribution circuit. An isolation resistor needs an accompanying load/regulation analysis because R116 returns signal current to VREF; simply inserting 50 Ω can modulate the reference. Consider a buffer intended for capacitive loads or a properly compensated output network. Verify startup and loaded transient response on the bench.

Locations: `cubevox.kicad_sch:10743` (U106), `:7146` (first VREF capacitor), `:10410` (R116).

Source: [TI OPA1678 datasheet, section 8.1.1](https://www.ti.com/lit/ds/symlink/opa1678.pdf).

### A04 — Full DC gain can consume the high-gain preamp's headroom

**Confirmed topology with a tolerance-dependent failure risk.** There is no DC blocking or servo between the instrumentation stage, difference stage and variable-gain stage. The gain resistors also operate down to DC. The first pair's differential offset is multiplied by 10.4, then by as much as 102.

An illustrative **1 mV differential offset becomes 1.0608 V at PRE_OUT**. With 2 mV maximum offset per OPA1678 amplifier, the aligned worst-case offsets of all four preamp amplifiers give approximately `102 × (10.4 × 4 mV + 2 × 2 mV + 2 mV) = 4.86 V` output error before clipping. That is a tolerance bound, not a prediction that every board will have that offset.

C137 blocks DC only after PRE_OUT; it cannot restore headroom lost in U105B. K101 also switches directly between PRE_OUT, including its amplified offset, and DAC_ATT, whose DC level returns to VREF. This creates a potentially large DC step and an audible pop.

**Action:** establish a worst-case offset budget, reduce DC gain with a suitable AC-coupled gain leg/interstage coupling, or add a servo/choose sufficiently low-offset devices. Check low-frequency response, settling, pot movement noise and relay transitions after the redesign.

Locations: `cubevox.kicad_sch:9215` (U105), `:10143` (RVGAIN1), `:10410` (R116), `:11893` (K101).

Source for offset limit: [TI OPA1678 datasheet](https://www.ti.com/lit/ds/symlink/opa1678.pdf). Gain/offset propagation is calculated from the exported netlist.

## Medium-priority findings and design limitations

### A05 — Minimum gain and feedback loading constrain line/pedal headroom

**Calculated design limitation.** At RVGAIN1 = 0 Ω, R115 and R116 form a 100 Ω / 100 Ω noninverting feedback network. Stage 2 therefore has gain 2, not 1, and presents a **200 Ω AC load** from PRE_OUT to VREF. A 1.5 V peak signal requires 7.5 mA through that network alone. The reference buffer must sink/source that current as well.

The exact ideal gains are 26.36–60.51 dB on XLR and 6.96–41.11 dB on TRS before source loading. The PCM1808's nominal 3 Vpp full-scale input corresponds to about **51 mVrms XLR / 476 mVrms TRS at minimum gain**, before allowing for supply drop, tolerances and earlier clipping. Firmware attenuation after conversion cannot recover clipped pedal input. The later design-spec text acknowledges a minimum stage gain of 2, but the schematic/spec descriptions of 0–40 dB are misleading.

**Action:** decide the maximum supported pedal/line level and redesign gain/pad ranges accordingly. Increase feedback impedance with an appropriate pot/network, and verify output drive and VREF current across the range. These are additional constraints after A02 is fixed.

Locations: `cubevox.kicad_sch:10143` (RVGAIN1), `:10410` (R116); U8 on the child sheet.

Source for ADC full scale: [TI PCM1808 datasheet](https://www.ti.com/lit/ds/symlink/pcm1808.pdf). Other figures are circuit calculations.

### A06 — Supply selection permits operation below the analog chain's minimum voltage

**Confirmed threshold; conditional failure during low supply / switchover.** U2 TPS2116 uses R8 = 33 kΩ and R9 = 10 kΩ. Its nominal 1 V PR1 threshold selects the 9 V-derived rail down to **4.3 V**, below the analog components' minimum supply. Including the stated 0.92–1.08 V threshold range and 1% divider resistors gives an approximate **3.90–4.72 V** selection threshold range; hysteresis and transient timing also need consideration.

The PCM1808 requires VCC ≥ 4.5 V. R20 (10 Ω) drops approximately 86 mV typically, 110 mV at the specified maximum 11 mA analog current. Thus **5VA needs at least approximately 4.61 V** at maximum current, before additional design margin. USB feeds the analog rail through U2 and FB101 without a 5 V boost/regulator. A loaded 4.5 V USB input, for example, cannot meet that requirement. The OPA1678 also requires at least 4.5 V.

USB is specified for firmware only, so this does not prove that the intended 9 V-powered use fails. It does mean USB-only audio and clean audio during brownout/source transfer are not guaranteed, while the hardware still powers the analog chain and lets the switch energize the relay.

**Action:** document/enforce firmware-only USB operation and mute during invalid analog supply, or provide a supply architecture that guarantees the required margin. Set and tolerance-check the source-transfer threshold against the actual analog minimum rather than only the MCU's needs.

Locations: `h7core_block.kicad_sch:11845` (U2), `:13175` (R8), `:13287` (R9), `:17534` (R20).

Sources: [TPS2116 thresholds and priority selection](https://www.ti.com/lit/ds/symlink/tps2116.pdf), [PCM1808 supply/current limits](https://www.ti.com/lit/ds/symlink/pcm1808.pdf).

### A07 — Audio coupling uses high-K ceramic capacitors

**Component-quality risk requiring measurement.** The audio coupling capacitors use C14860 / CL31B106KAHNNNE, 10 µF X7R 1206, including C120/C124 at the microphone input, C121/C125 on TRS, and C137/C139/C141 in the later signal path. High-K ceramic capacitance varies with voltage and can also be microphonic. This is particularly undesirable ahead of roughly 60 dB gain on the same board as mechanical controls.

This is not a claim that every coupling position causes audible distortion: the AC voltage actually appearing across each capacitor matters. The low-impedance microphone bias network and capacitor DC-bias derating also need inclusion in low-frequency/CMRR checks.

**Action:** prioritize low-distortion, low-microphony coupling parts at the mic input, then measure THD versus frequency/level and perform a mechanical tap test. Consider suitable film or properly selected electrolytic/bipolar parts where size allows; calculate polarity, leakage and effective capacitance.

Sources: [TI capacitor selection analysis](https://www.ti.com/lit/an/slyt796a/slyt796a.pdf), [TI microphonics discussion](https://e2e.ti.com/blogs_/archives/b/precisionhub/posts/stress-induced-outbursts-microphonics-in-ceramic-capacitors-part-1).

### A08 — Difference-amplifier resistor matching limits hum rejection

**Calculated quality limitation, not a disconnected circuit.** R111–R114 are separate 10 kΩ C25804 resistors, not a specified ratio-matched network. With independent 1% resistor tolerance, the worst corner gives approximately 34 dB common-mode rejection in the difference stage, or **54.3 dB referred to the complete 10.4× instrumentation front end**. This is far below the op-amp's own CMRR and cannot be inferred from the op-amp specification alone. Input bias-resistor and coupling-capacitor mismatches can further reduce rejection, especially at low frequency.

**Action:** define a system CMRR target and specify resistor ratio matching/tracking, preferably with a matched network. Verify 50/60 Hz rejection with realistic source impedances and component tolerances. Preserve symmetry in the input protection and PCB layout.

Location: `cubevox.kicad_sch:9588` (R111 and the following difference-stage network). The 54.3 dB result is a corner calculation from the actual topology, assuming the listed 1% resistor grade.

### A09 — DAC output lacks the external low-pass network shown in TI's application circuit

**Quality/EMC improvement, not a proven audible defect.** U6 OUTL reaches C139, R122/R123, K101 and U106B without an intentional shunt capacitor forming a DAC reconstruction/RF filter. R124 and C141 at the final output are a series isolation resistor and an AC coupling capacitor; they do not form that low-pass filter.

TI's PCM5102A reference circuit uses 470 Ω followed by 2.2 nF, approximately 154 kHz. The DAC has internal filtering, so omission alone does not establish failure, but residual high-frequency output reaches the following buffer and cable without the recommended external network.

**Action:** include an appropriately calculated RC filter before the output buffer, accounting for the existing attenuator and capacitor loading. Measure out-of-band noise and audio-band response. Do not blindly put 2.2 nF across the 10 kΩ / 15 kΩ divider: its 6 kΩ source impedance would put the pole near 12 kHz.

Locations: `h7core_block.kicad_sch` U6; `cubevox.kicad_sch:12622` (C139), `:12733` (R122), `:13066` (R124).

Source: [TI PCM5102A datasheet, Figure 33](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf).

## Integration and documentation checks

### I01 — Prove the ADC master-clock plan before freezing the design

PD14 / U1 pin 61 supplies ADC SCKI through R27. The project pin map describes TIM4_CH3 generating 12.288 MHz, while the design brief refers to recalculating PLL3. TIM4 and SAI1 must actually produce coherent clocks; changing an audio PLL setting alone is not evidence that the timer output follows it. PCM1808 slave mode requires SCKI synchronized with LRCK and a supported ratio (256/384/512 × fs), plus the supported BCK frame length.

This is **not a claim that PD14 cannot generate the needed clock**: ST also documents SAI3_MCLK_B on PD14. No target-MCU clock initialization was present in this repository to verify a concrete solution. Either prove a common-clock configuration on the existing pin, or consider moving the clock to an appropriate SAI master-clock output. PE2 already carries QSPI_IO2, so reclaiming it for SAI1_MCLK_A would require a corresponding flash pin-assignment change.

Sources: [ST H743 datasheet, PE2 and PD14 alternate functions](https://www.st.com/resource/en/datasheet/stm32h743vi.pdf), [PCM1808 clock requirements, sections 7.3.2–7.3.5](https://www.ti.com/lit/ds/symlink/pcm1808.pdf).

### I02 — Allow panel-mux settling in firmware

R126 = 1 kΩ and C142 = 10 nF precede R30 = 100 Ω and C55 = 1 nF. A 10 kΩ pot contributes as much as 2.5 kΩ source resistance at mid-travel, plus the mux on-resistance. A rough dominant time constant is approximately 40 µs, so a full-scale channel change can require roughly **0.3–0.5 ms** settling for high-resolution reads. Exact settling depends on tolerances, ADC acquisition time and the required effective resolution. Do not sample immediately after switching the mux address; discard/settle/filter as appropriate. No hardware error is established here.

### I03 — Relay switching is neither automatically pop-free nor power-off pass-through

Both relay sources nominally sit at VREF, but A04 creates a DC mismatch, the live audio waveforms differ, and the direct panel switch cannot coordinate a fade around contact movement. Expect switching transients unless a mute/fade strategy is designed. Paralleled poles also do not guarantee simultaneous transfer; they can briefly connect the two signal sources during transfer. The DAC branch's resistor network limits this current, so this is mainly a switching-quality concern.

The NC route still uses the powered preamp and output buffer. It can bypass stopped/crashed digital processing while analog power is healthy, but cannot carry audio with all power removed. This agrees with a powered preamp bypass; document that distinction in the product behavior.

### I04 — Schematic readability and assembly records need cleanup

- Replace the isolated, label-connected analog rectangles with conventional op-amp units and visible feedback/signal wiring. Put the mic preamp, VREF, gain stage and relay/output on readable functional sheets. Present topology and calculated gains directly on the schematic.
- Correct the `T_SW` / active-low detection notes and the stage-2 `0–40 dB` label. The actual stage-2 range is 6.02–40.17 dB.
- `hardware/README.md` still states that no schematic or PCB exists. `PARTS.md` mixes superseded import warnings and unfinished-scaffold notes with current parts; its SWD-header description is stale because the actual schematic uses a Tag-Connect footprint.
- J101, J106 and J108 remain included in the schematic BOM, although PARTS.md instructs that these hand-soldered jacks be excluded from JLC BOM/CPL. J117 also carries an owner-supplied text string in its LCSC field. Ensure the assembly export explicitly filters these references; a separate filtered JLC BOM can be valid, but no final production BOM was audited here.

## Additional checks completed

- NMJ6HCD2 J108: imported footprint pad ordering agrees with the manufacturer component-side drawing: 1/2/3 = T/R/S, 4/5/6 = TN/RN/SN. Tip and sleeve are connected correctly for TS use; ring and switched contacts are intentionally unused. [Neutrik drawing](https://www.neutrik.com/media/8582/download/st-nmj6hcd2.PDF?v=3).
- AP63205 / AP63203 buck connections match the manufacturer's pin functions: direct fixed-output FB sense, EN to input, bootstrap capacitor to SW. The 5 V buck's 4.7 µH / 2×22 µF values match its typical circuit. Actual inductor saturation, capacitor derating and PCB switching loops still require layout/load verification. [Diodes datasheet](https://www.diodes.com/datasheet/download/AP63200-AP63201-AP63203-AP63205.pdf).
- MCU VDD/VSS connections, two VCAP capacitors, analog supply filtering, BOOT0 pulldown and SWD/reset connections are present. USB CC resistors are separate 5.1 kΩ pulldowns. No missing connection was identified in those checks.
- Mux spare inputs and enable are grounded; pot supply and mux supply are both 3V3A. Encoder GPIO interrupt line numbers do not conflict in the listed assignments.

## Recommended order of work

1. Correct the connector model/detection circuit (A01).
2. Redesign and calculate the analog chain together: amplifier input range, VREF stability, DC offset, gain range and loading (A02–A05). Changing only the op-amp part number will not resolve all of these.
3. Resolve analog power validity and audio-clock generation, then address CMRR, capacitor selection, DAC filtering and switch transitions.
4. Regenerate schematic/netlist and rerun ERC; verify the resulting PCB against the corrected schematic before fabrication.
5. On prototypes, measure PRE_OUT DC versus gain, VREF oscillation/startup, clipping/THD, CMRR, USB/9 V transitions and relay pops. Confirm connector continuity and validate clocks with a scope/logic analyzer.

## Verification and limits

Final KiCad ERC again reports **0 violations across 2 sheets**. A second netlist export matches the first electrically: **227 components and 214 nets**. Ten automated evidence checks passed, including the key cited connections and comparison with explicit JSON pin/net assignments.

No schematic, JSON design specification, symbol library or PCB was edited. Findings are from schematic/netlist inspection, manufacturer documentation and analytical calculations; no circuit simulation or bench measurements were performed. This review does not certify PCB return paths, thermal design, switcher layout, crystal startup margin, EMC/ESD immunity, full BOM correctness, connector mechanical fit or every supplier-specific footprint. Those need the corresponding layout, part and prototype checks. Existing design documents contain unverified barrel-jack and other imported-part notes; physical sample checks remain appropriate.

## Evidence

- [Final ERC report](audit/2026-10-02/erc.json).
- [Connectivity, component values, calculations, verification checks and source SHA-256 hashes](audit/2026-10-02/evidence.json).
- Scratch material in `/tmp/cubevox-audit/` includes full XML exports, readable connection listings, fresh schematic PDF/SVG renders and manufacturer datasheets. Scratch files are not required to read the findings or inspect the durable electrical evidence.
