1. FAIL — Corrected nominal ADC limits are approximately 62 mVrms open-circuit microphone input with 300 Ω source impedance and 0.50 Vrms TRS; the −54 dBV microphone reaches full scale at stage-2 gain ≈31. Switchable R101/R104=47 Ω/18 kΩ and R102/R105=33 kΩ provide attenuation, but D107/D108 also clamp sufficiently negative input excursions.
2. UNSURE — The 20/22/45 dB CMRR figures assume ±20% C120/C124 and 150 Ω common-mode source impedance per leg; differential microphone impedance alone does not establish those conditions, and 0.1% electrolytic matching is not a durable guarantee.
3. UNSURE — R116 draws 7.5 mA peak only at minimum gain and nominal ADC full scale; C159 is isolated by 200 Ω, while [OPA2197](https://www.ti.com/lit/ds/symlink/opa2197.pdf) linear-output margin still needs verification.
4. UNSURE — R146/C160 phase margin remains unverified; R112, R123 and unbalanced R103/R106 inject audio into VREF, whose noise receives up to 51× stage-2 amplification.
5. FAIL — C158 has approximately zero DC bias and alternating polarity: replace with 22 µF bipolar; other listed polarities are correct with grounded external sources. Add R148/R149, 100 kΩ from TRS_T/TRS_R to ground, to define unplugged C121/C125 bias.
6. UNSURE — Output is approximately 1.22 Vrms into 10 kΩ, with impedance approximately 100 Ω midband/800 Ω at 20 Hz; INSTRUMENT attenuation is unspecified and cannot affect analog bypass.
7. FAIL — [K101](https://omronfs.omron.com/en_US/ecb/products/pdf/en-g6k.pdf) handles line-level voltage, but parallel-pole timing can momentarily connect PRE_OUT to DAC_ATT, and coil sensing does not guarantee pop-free switching.
8. PASS — [PE2 AF6](https://www.st.com/resource/en/datasheet/stm32h743ii.pdf) is correct; PLL3 yields 49.152008 MHz, +0.164 ppm, using factors M/N/P=25/196/4 (raw DIVN3=195/DIVP3=3), PLL3RGE=0, PLL3VCOSEL=1, fractional enable, and [SAI MCKDIV=4, OSR=0, NOMCK=0](https://www.st.com/resource/en/reference_manual/rm0433-stm32h742-stm32h743-753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf).
9. PASS
10. PASS
11. UNSURE — FB101/C117 attenuation and resonance need impedance/ESR data; 120 Ω at 100 MHz does not establish suppression of U103's 1.1 MHz ripple or light-load bursts.
12a. FAIL — Removing all VBUS detection breaks self-powered attach/detach handling, although [ROM DFU](https://www.st.com/resource/en/application_note/an2606-.pdf) and forced-device enumeration can work without PA9. Retain R6=33 kΩ/R7=82 kΩ with PA9 GPIO detection and native sensing disabled per [AN4879](https://www.st.com/resource/en/application_note/DM00296349-.pdf).
12b. FAIL — K101 cannot suppress C159/VREF startup or C141 discharge transients; the proposed K102 is not a complete fix without release-time and hold-up design. [U6 XSMT](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf) needs ≥3.325 ms before power loss at 48 kHz.
12c. FAIL — Analog validity can fail around 5VA=4.61 V while 3V3 remains healthy; set U1 BOR level 3, approximately 2.61 V falling. The proposed 4.75 V supervisor threshold is provisional, requiring tolerance and shutdown-time budgeting.
12d. FAIL — Verified connectivity still includes R6/R7 and PA9, contradicting the addendum; U2/R8/R9/C17 are removed, and C18 is USB bypass capacitance, not mux residue.
13. PASS — Place the preamp beside J101, codecs along its inner boundary, MCU beyond them, and both buck switching loops away from the input corridor over uninterrupted ground.
14. UNSURE — [Cross-bank read-while-write](https://www.st.com/resource/en/reference_manual/rm0433-stm32h743-753-and-stm32h750-value-line-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf) is supported, but “reverb engine in bank 2” needs clarification because executing code or reading constants there during erase/program stalls access.
15. UNSURE — No QSPI references were found in project source, but no H743 linker map establishes internal-flash allocation or DSP RAM fit.
16. PASS

## Owner response, revision 3

1. Accepted. Stage 1 is 20.05 dB (R108, R110 4.53 k), stage 2 runs 0 to 29.9 dB (R116 330 R), and the TRS pad is 25.7 dB (R102, R105 22 k).
2. Accepted in part. C120 and C124 are 47 uF, which puts the corner at 2.8 Hz. CMRR with the electrolytic tolerance is a bench measurement.
3. Accepted. R116 at 330 R cuts the minimum-gain load to 4.2 mA rms. The OPA2197 linear-output margin is a bench check.
4. Declined as a schematic change. The 22 R follower isolation follows TI guidance. Phase margin and VREF noise are bench checks.
5. Accepted. C158 is a 22 uF 25 V bipolar electrolytic (C413679). R148 and R149 put 100 k from TRS_T and TRS_R to GND.
6. Documented. INSTRUMENT / LINE scaling is digital only, so BYPASS outputs the preamp at pot level (firmware note F6).
7. Accepted. K101 pole B is now a dry contact to FX_ON_SENSE, and the poles are no longer paralleled. K102 mutes the output.
8. Pass. No change. The clock values are recorded in firmware note F4.
9. Pass. No change.
10. Pass. No change.
11. Declined as a schematic change. FB101 and C117 ripple attenuation is a bench measurement.
12a. Accepted. R6 is 33 k and R7 is 82 k into PA9 as a GPIO input, native VBUS sensing disabled per AN4879.
12b. Accepted in part. K102, Q101 and MUTE_N mute the output, and XSMT is asserted at least 3.4 ms before shutdown (F3). Release time and hold-up are bench items.
12c. Accepted. VA_SENSE (10 k / 10 k, 100 nF) reads 5VA on PC4. BOR level 3 is set in firmware (F1). The 4.75 V threshold stays provisional (F2).
12d. Accepted. Schematic and docs now agree. U2, R8, R9 and C17 are removed, R6, R7 and PA9 stay, and C18 is the USB-C VBUS bypass.
13. Pass. No change.
14. Accepted. The reverb engine selection is a setting. Code and constants never live in bank 2 (spec item 17).
15. Firmware phase. No linker map exists yet. Flash allocation and DSP RAM fit are checked at firmware bring-up.
16. Pass. No change.
