// Source: /Users/j/Documents/Claude/Projects/Teensy-H7-Port/test/support/h7_dfu_reboot.h (copied verbatim below; do not edit here)
/* h7_dfu_reboot.h — reboot an STM32H7 into its ROM DFU bootloader on command,
 * so flashing never needs the BOOT/RESET button dance.
 *
 * Part of Teensy-H7-Port. Test-bench support only: no audio code depends on
 * this, and nothing here ships in a product build.
 *
 * STATUS: working, confirmed on a Daisy Seed rev4 on 2026-09-07 — two flashes
 * in a row entered DFU with no button pressed.
 *
 * Getting there ruled out two suspects by measurement:
 *   - the bootloader address is right. test/h7_board_id scanned system memory
 *     and 0x1FF09800 is the only valid vector table there (SP=0x24003AF8,
 *     PC=0x1FF09ABD).
 *   - de-initialising the clock tree before the jump changed nothing.
 * The actual cause was interrupts: the jump ran with __disable_irq() still in
 * force, and the ROM bootloader drives USB from interrupts without ever
 * clearing PRIMASK, because after a real reset it is already clear. The board
 * entered the bootloader and then enumerated nothing.
 *
 * Use it from a sketch:
 *
 *     #include "h7_dfu_reboot.h"
 *     void loop() { dfu_reboot_poll(); ... }
 *
 * Then `test/flash_h7_board_id.sh` writes 'b' to the USB serial port and the
 * board is in DFU mode a second later.
 *
 * HOW IT WORKS, and why not the obvious way
 * -----------------------------------------
 * Jumping to the ROM bootloader straight from a running sketch does not work
 * on USB: the host still sees the sketch's CDC device, so the bootloader never
 * enumerates as DFU. So this goes the long way round.
 *
 *   1. dfu_reboot_request() writes a magic word to RTC backup register 31 and
 *      calls NVIC_SystemReset(). The backup registers survive a reset; RAM
 *      contents are not relied on. The reset drops USB, so the host sees a
 *      clean disconnect.
 *   2. On the way back up, initVariant() runs before setup() and therefore
 *      before Serial.begin() brings USB back. It sees the magic, clears it,
 *      and jumps to the ROM bootloader, which enumerates as 0483:df11.
 *
 * Clearing the magic BEFORE the jump matters: if the jump is wrong, the next
 * reset comes up as a normal sketch instead of looping into a dead branch.
 * Worst case is still recoverable with the button dance.
 *
 * This header defines initVariant(), so include it in exactly one translation
 * unit. A sketch is one translation unit, so that is the normal case. The
 * DAISY_SEED variant does not define initVariant(), so overriding the core's
 * weak one costs nothing.
 */

#ifndef H7_DFU_REBOOT_H
#define H7_DFU_REBOOT_H

#include <Arduino.h>

/* System memory (ROM bootloader) on STM32H74x/H75x, per ST AN2606. The word at
 * this address is the bootloader's initial stack pointer, the next its entry
 * point — the same layout as any Cortex-M vector table. */
static const uint32_t kH7SystemMemory = 0x1FF09800UL;

/* Arbitrary, just has to be a value that will not appear by accident. */
static const uint32_t kDfuRebootMagic = 0xB00710ADUL;

/* How far the last DFU attempt got, left in a second backup register. Backup
 * registers survive a reset, so if the jump hangs the board, tapping RESET
 * brings the sketch back and it can say where it died — instead of every
 * attempt costing a blind retry. */
static const uint32_t kDfuStageNone      = 0;
static const uint32_t kDfuStageRequested = 1; /* magic seen after the reset */
static const uint32_t kDfuStageValidated = 2; /* bootloader vector table is sane */
static const uint32_t kDfuStageJumping   = 3; /* about to enter the bootloader */

/* The whole system-memory window, for the diagnostic dump below. */
static const uint32_t kH7SystemMemoryBase = 0x1FF00000UL;
static const uint32_t kH7SystemMemoryEnd  = 0x1FF20000UL;

/* Does this address hold something that looks like a Cortex-M vector table for
 * code living in system memory? Word 0 must be a stack pointer somewhere in
 * SRAM, word 1 an entry point inside system memory with the Thumb bit set.
 *
 * The jump is guarded by this because an address that is merely plausible is
 * not good enough: jumping to garbage hard-faults with USB down, and the board
 * then looks dead until someone reaches for the BOOT button. Refusing to jump
 * leaves the sketch running and re-flashable. */
static inline bool dfu_vector_table_looks_valid(uint32_t addr)
{
	const uint32_t *vt = (const uint32_t *)addr;
	uint32_t sp = vt[0];
	uint32_t pc = vt[1];

	/* DTCM at 0x20000000, AXI SRAM and the D2/D3 SRAMs from 0x24000000 up. */
	bool sp_ok = (sp >= 0x20000000UL && sp <= 0x20020000UL) ||
	             (sp >= 0x24000000UL && sp <= 0x38900000UL);
	bool pc_ok = (pc >= kH7SystemMemoryBase) && (pc < kH7SystemMemoryEnd) &&
	             ((pc & 1UL) == 1UL);
	return sp_ok && pc_ok;
}

/* Print every plausible vector table in system memory. Called from a test
 * sketch to find the ROM bootloader's real entry rather than trusting a
 * hard-coded address. */
static inline void dfu_dump_system_memory(void)
{
	Serial.println("[dfu] scanning system memory for the ROM bootloader:");
	bool found = false;
	/* Bootloader vector tables are aligned; 0x200 is the coarsest step that
	 * still finds every documented H7 entry point. */
	for (uint32_t a = kH7SystemMemoryBase; a < kH7SystemMemoryEnd; a += 0x200UL) {
		if (!dfu_vector_table_looks_valid(a)) continue;
		const uint32_t *vt = (const uint32_t *)a;
		Serial.print("  0x");
		Serial.print(a, HEX);
		Serial.print("  SP=0x");
		Serial.print(vt[0], HEX);
		Serial.print("  PC=0x");
		Serial.print(vt[1], HEX);
		Serial.println(a == kH7SystemMemory ? "   <- the address we jump to" : "");
		found = true;
	}
	if (!found) Serial.println("  none — nothing in 0x1FF00000..0x1FF20000 looks like one");
}

/* Backup registers need two things: the RTC's APB clock (register access) and
 * the backup domain unlocked (write access). Neither needs an RTC clock
 * source, so no LSE/LSI setup is involved. PWR itself takes no clock enable on
 * the H7 — unlike the F4, it has no RCC gate, so there is no
 * __HAL_RCC_PWR_CLK_ENABLE() to call here. */
static inline void dfu_backup_regs_unlock(void)
{
	HAL_PWR_EnableBkUpAccess();
	__HAL_RCC_RTC_CLK_ENABLE();
}

static inline void dfu_breadcrumb(uint32_t stage)
{
	RTC->BKP30R = stage;
}

/* Print, and then clear, how far the previous DFU attempt got. Clearing it
 * means the next report describes the next attempt, not this one forever. */
static inline void dfu_report_last_attempt(void)
{
	dfu_backup_regs_unlock();
	uint32_t stage = RTC->BKP30R;
	const char *what;
	switch (stage) {
	case kDfuStageNone:      what = "no DFU attempt since power-up"; break;
	case kDfuStageRequested: what = "reset into DFU, but the vector table check refused the jump"; break;
	case kDfuStageValidated: what = "vector table passed, but never reached the jump"; break;
	/* Stage 3 is the last thing recorded before control leaves this image, so
	 * it cannot say whether the bootloader then came up. If a flash succeeded,
	 * it did. If the board had to be rescued with the BOOT button, it did not. */
	case kDfuStageJumping:   what = "jumped into the ROM bootloader"; break;
	default:                 what = "unknown breadcrumb"; break;
	}
	Serial.print("[dfu] last attempt: ");
	Serial.println(what);
	RTC->BKP30R = kDfuStageNone;
}

/* Ask for DFU. Does not return — the board resets. */
static inline void dfu_reboot_request(void)
{
	dfu_backup_regs_unlock();
	RTC->BKP31R = kDfuRebootMagic;
	/* Let any in-flight USB serial output drain before the port disappears. */
	delay(50);
	NVIC_SystemReset();
}

/* Arm a dead man's switch: come up in DFU if the sketch never reaches loop().
 *
 * WHY THIS EXISTS. The serial 'b' command only works if loop() runs. A sketch
 * that wedges the CPU before then — an audio graph heavy enough that the update
 * interrupt never finishes, which is exactly what test/h7_census did on its
 * first run — cannot answer, and the only way back is the three-handed
 * BOOT-hold, RESET-tap, BOOT-release dance. That cost a full round trip.
 *
 * Armed, the magic is already in the backup register, so a plain tap of RESET
 * lands in the bootloader. One button instead of three, and no watchdog to get
 * wrong.
 *
 * Call it at the top of setup() in any sketch that might overload the graph.
 * dfu_reboot_poll() disarms it on the first pass through loop(), so a healthy
 * sketch reboots normally and only a wedged one is caught. */
static inline void dfu_reboot_arm_deadman(void)
{
	dfu_backup_regs_unlock();
	RTC->BKP31R = kDfuRebootMagic;
}

/* Call from loop(). 'b' or 'B' on the USB serial port reboots to DFU.
 *
 * Also disarms the dead man's switch on the first call: reaching loop() is the
 * definition of "this sketch is alive". */
static inline void dfu_reboot_poll(void)
{
	static bool disarmed = false;
	if (!disarmed) {
		dfu_backup_regs_unlock();
		if (RTC->BKP31R == kDfuRebootMagic) RTC->BKP31R = 0;
		disarmed = true;
	}
	while (Serial.available()) {
		int c = Serial.read();
		if (c == 'b' || c == 'B') {
			Serial.println("[dfu] rebooting to bootloader");
			Serial.flush();
			dfu_reboot_request();
		}
	}
}

/* Runs from main() before setup(), so before USB comes up. */
extern "C" void initVariant(void)
{
	dfu_backup_regs_unlock();
	if (RTC->BKP31R != kDfuRebootMagic) return;
	RTC->BKP31R = 0;
	dfu_breadcrumb(kDfuStageRequested);

	/* Refuse rather than hard-fault into a dead board. setup() reports it. */
	if (!dfu_vector_table_looks_valid(kH7SystemMemory)) return;
	dfu_breadcrumb(kDfuStageValidated);

	/* Hand the bootloader a machine that looks freshly reset. The caches are
	 * on because the core enables them in premain(); clean and kill them, or
	 * the bootloader reads stale memory. */
	__disable_irq();
	SCB_DisableDCache();
	SCB_DisableICache();

	/* And put the clock tree back to its reset state. This is not optional:
	 * premain() -> init() -> SystemClock_Config() has already run by now, so
	 * the chip is on the PLL with HSE live. The ROM bootloader derives its USB
	 * clock assuming the reset defaults (HSI), and with the PLL still running
	 * it comes up but never enumerates — the board simply disappears from USB.
	 * HAL_DeInit() first, so peripherals are not clocked while their source
	 * changes underneath them. */
	HAL_DeInit();
	HAL_RCC_DeInit();

	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;
	for (uint32_t i = 0; i < 8; i++) {
		NVIC->ICER[i] = 0xFFFFFFFFUL;
		NVIC->ICPR[i] = 0xFFFFFFFFUL;
	}

	/* Read both words out to registers BEFORE the stack pointer moves. After
	 * __set_MSP the old stack is gone, so anything still living there is not
	 * safe to read. */
	const uint32_t *vt = (const uint32_t *)kH7SystemMemory;
	uint32_t boot_sp = vt[0];
	uint32_t boot_pc = vt[1];

	SCB->VTOR = kH7SystemMemory;
	__DSB();
	__ISB();

	/* Un-mask interrupts. At a real reset PRIMASK, FAULTMASK and BASEPRI are
	 * all clear, and the ROM bootloader is written for that: it drives USB
	 * from interrupts and never clears the masks itself. Jumping with
	 * __disable_irq() still in force leaves it enumerating nothing, which is
	 * exactly the "board vanishes from USB" symptom. */
	dfu_breadcrumb(kDfuStageJumping);
	__set_BASEPRI(0);
	__set_FAULTMASK(0);
	__set_PRIMASK(0);
	__set_MSP(boot_sp);
	((void (*)(void))boot_pc)();

	while (1) { /* not reached */ }
}

#endif /* H7_DFU_REBOOT_H */
