# Astra packet — USB-C data path review (2026-10-08)

Independent review requested: will USB full-speed device data (CDC serial for a config tool, and ST ROM DFU for firmware updates) work on this board? Point out anything wrong or missing. Answer in numbered points, concise, no praise.

## Board (STM32H743VIT6 LQFP-100, 4-layer F.Cu / In1 GND / In2 GND / B.Cu, JLC)
- USB-C receptacle J1 (16-pin, C393939). D+ A6+B6 -> ESD D2 USBLC6-2SC6 pins 1/6 -> PA12. D- A7+B7 -> D2 pins 3/4 -> PA11. D2 pin 2 GND, pin 5 VBUS. D2 sits beside J1.
- CC1 (A5) -> R4 5.1k -> GND. CC2 (B5) -> R5 5.1k -> GND. Separate resistors.
- VBUS (A4/B9, B4/A9) -> C18 1 uF to GND, D2 pin 5, divider R6 33k / R7 82k -> PA9 (VBUS_SENSE, GPIO input; native OTG VBUS sensing disabled per AN4879). VBUS powers nothing. Box runs from 9 V barrel. SBU A8/B8 unconnected. Shield/GND pads to GND.
- D+/D- traces 0.2 mm wide, 0.4 mm pitch, F.Cu only, no vias, ~70 mm, over a solid GND plane (In1). Each receptacle side pair joined at the J1 pads (short T).
- HSE: 25 MHz crystal Y1 (12 pF spec) on PH0/PH1 with 15 pF C15/C16 to GND, 0 R series R1 on OSC_OUT. BOOT0 has a pull-down and an inside button. NRST has a cap and a button. SWD on Tag-Connect.
- DFU entry: firmware writes 0xB00710AD to RTC BKP31R and jumps to 0x1FF09800 after reset; menu item "Update firmware". BOOT0 button is recovery.

## Firmware
- Arduino STM32duino core 3.0.0, FQBN GenH7:pnum=GENERIC_H743VITX,usb=CDCgen.
- The sketch has no SystemClock_Config override. The generic variant's clock: HSI 64 MHz -> PLL1 M4 N60 P2 (480 MHz), USB clock = HSI48, no CRS. audio.cpp sets PLL3 M25 N196 FRACN 4981 P4 for SAI1 assuming the PLL source is HSE 25 MHz.
- Planned fix: override SystemClock_Config: HSE on, PLL1 source HSE, M5 N192 P2 Q20 (480 MHz core, 48 MHz PLL1Q), USB clock PLL1Q, HSI48 off. PLL3 keeps M25.
- Planned use: USB CDC at runtime for a JSON config protocol with a Mac app; ROM DFU (dfu-util) for firmware. Device is self-powered.

## Questions
1. Anything in the hardware list that would stop enumeration or make it unreliable?
2. Is the planned clock fix correct and sufficient for USB FS and for the ROM DFU bootloader with a 25 MHz crystal? Any H743 errata or bootloader-revision caveats?
3. Self-powered device with VBUS sensing off: should firmware gate the D+ pull-up (USB_OTG soft disconnect) on PA9 reading VBUS present? Any host-side problems if not?
4. Anything else you would check or add before fab (test points, series resistors, ESD placement, pull-ups, clock capacitors)?
