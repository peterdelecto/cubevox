// cubevox's TinyUF2 board hooks: the start (option bytes, then the boot decision before any flash
// read), the fault handlers, the watchdog before the jump, and update mode: the cable, the timeout,
// the screens, the UF2 filters and the end of an update. The policy is boot_decide.c; this file
// reads the hardware and acts on its answers.

#include <string.h>

#include "board_api.h"
#include "device/dcd.h"
#include "tusb.h"
#include "uf2.h"

#include "boot_decide.h"
#include "diag.h"
#include "image_check.h"
#include "text.h"
#if CV_OLED
#include "oled.h"
#include "screens.h"
#endif

CV_STATIC_ASSERT(family_from_boot_block, BOARD_UF2_FAMILY_ID == CV_BOARD_FAMILY);
CV_STATIC_ASSERT(menu_settles_before_timeout, CV_MENU_SETTLE_MS < CV_UPDATE_TIMEOUT_MS);
CV_STATIC_ASSERT(dbeccerr_is_cmsis, CV_FLASH_SR_DBECCERR == FLASH_SR_DBECCERR);
CV_STATIC_ASSERT(ecc_address_is_cmsis, CV_FLASH_ECC_ADDR == FLASH_ECC_FA_FAIL_ECC_ADDR);
CV_STATIC_ASSERT(bank2_base_is_cmsis, CV_BANK2_BASE == FLASH_BANK2_BASE);
CV_STATIC_ASSERT(flash_word_is_hal, CV_FLASH_WORD_BYTES == 4u * FLASH_NB_32BITWORD_IN_FLASHWORD);
CV_STATIC_ASSERT(power_on_flags_are_cmsis, CV_RSR_PORRSTF == RCC_RSR_PORRSTF && CV_RSR_BORRSTF == RCC_RSR_BORRSTF);
CV_STATIC_ASSERT(restart_flags_are_cmsis, CV_RSR_SFTRSTF == RCC_RSR_SFTRSTF &&
                                              CV_RSR_IWDG1RSTF == RCC_RSR_IWDG1RSTF &&
                                              CV_RSR_WWDG1RSTF == RCC_RSR_WWDG1RSTF);
CV_STATIC_ASSERT(bor_field_is_cmsis, CV_OPTSR_BOR_LEV_MASK == FLASH_OPTSR_BOR_LEV &&
                                         (CV_BOR_LEVEL_WANTED << CV_OPTSR_BOR_LEV_POS) == OB_BOR_LEVEL3);
CV_STATIC_ASSERT(wrp_sector0_is_hal, CV_WPSN_SECTOR0 == OB_WRP_SECTOR_0);
// The decode of a double ECC error in bank 2 spans the whole bank, so every such fault names an
// address DIAG.TXT recognises.
CV_STATIC_ASSERT(ecc_decode_spans_bank2,
                 (CV_FLASH_ECC_ADDR + 1u) * CV_FLASH_WORD_BYTES == CV_BANK2_END - CV_BANK2_BASE);

#define CV_BLOCK ((struct CvBootBlock*)CV_BOOT_BLOCK_ADDR)
#define CV_FAULT_RECORD ((const struct CvFaultRecord*)CV_FAULT_RECORD_ADDR)

// The bootloader's RAM, where its stack and so any exception frame lie.
#define CV_RAM_START 0x20000000u
#define CV_RAM_END   0x20010000u

// IWDG1 (RM0433): the keys, and a prescaler of 256 on the LSI's 32 kHz, 8 ms a count.
#define CV_IWDG_KEY_START      0xCCCCu
#define CV_IWDG_KEY_ACCESS     0x5555u
#define CV_IWDG_KEY_RELOAD     0xAAAAu
#define CV_IWDG_PRESCALER_256  6u
#define CV_IWDG_TICK_MS        8u
#define CV_IWDG_RELOAD         (CV_WATCHDOG_BOOT_MS / CV_IWDG_TICK_MS - 1u)
CV_STATIC_ASSERT(boot_watchdog_fits_reload, CV_IWDG_RELOAD <= 0xFFFu);

// Bounds on the waits for IWDG1's register update and for the option byte change.
#define CV_IWDG_READY_MS       100u
#define CV_OPTION_TIMEOUT_MS   1000u
// DIAG.TXT reads the whole log, so a burst of refused blocks rewrites it at most this often.
#define CV_DIAG_REFRESH_MS     250u

// The port's temporary boot address, for boards that boot from QSPI or AXI SRAM. This board boots
// from internal flash only, so the words live in .bss, clear of the boot block's counter, which
// sits where common.ld would put them.
volatile uint32_t _board_tmp_boot_addr[1];
volatile uint32_t _board_tmp_boot_magic[1];

void cvBootFault(const uint32_t* frame) __attribute__((used, noreturn));

typedef struct {
  CvBootDecision decision;
  // The installed image, read once reading is safe and again at a mount after a write began.
  struct CvImageReport image;
  // Time since board_init2, from the cycle counter.
  uint32_t elapsedMs;
  uint32_t cycleMark;
  uint32_t cycleCarry;
  // What the fault handler needs to know about the moment it interrupts.
  bool adopted;
  bool imageReading;
  bool updateMode;
  // The decision named a sector and its erase failed, so the faulting word may remain.
  bool eraseFailed;
  // The cable, debounced, and whether update mode has acted on its first reading.
  CvDebounce vbus;
  bool cableTracked;
  // The cable came out after a host had connected, and is still out.
  bool cableOut;
  // A host has enumerated since the cable went in; the timeout waits for a pull.
  bool hostSeen;
  bool mounted;
  // When the timeout started counting: update mode's start, or the last pull with nothing written.
  uint32_t timeoutFromMs;
  // Update mode has accepted a UF2 block, so the installed image is partly overwritten.
  bool writeBegun;
  // Since the host last mounted the drive, as CvScreenInputs names them.
  bool sessionAccepted;
  bool sessionRefused;
  bool sessionFailed;
  // Application sectors the transfer in progress has written, one bit each from sector 1.
  uint32_t sessionSectors;
  CvBlockCounts blocks;
  char wrongFile[13];
  // DIAG.TXT lags the counts or the wrong file, and the time of its last rewrite.
  bool diagDirty;
  uint32_t diagMs;
  // The screen and host state on the panel, -1 before the first draw.
  int16_t shown;
} BoardState;

static BoardState gState = {.shown = -1};

//--------------------------------------------------------------------+
// Time
//--------------------------------------------------------------------+

static void cycleCounterStart(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55u;  // the Cortex-M7 DWT ignores writes until unlocked
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// Milliseconds since cycleCounterStart. The 32-bit count wraps every 11.9 s at 360 MHz. Calls come
// far more often: the DFU loop calls every pass, and its longest stall, a sector erase, takes about
// 2 s.
static uint32_t elapsedMs(void) {
  const uint32_t now = DWT->CYCCNT;
  const uint32_t cyclesPerMs = SystemCoreClock / 1000u;
  gState.cycleCarry += now - gState.cycleMark;
  gState.cycleMark = now;
  gState.elapsedMs += gState.cycleCarry / cyclesPerMs;
  gState.cycleCarry %= cyclesPerMs;
  return gState.elapsedMs;
}

//--------------------------------------------------------------------+
// Option bytes
//--------------------------------------------------------------------+

// True once `flags` clear in *reg within CV_OPTION_TIMEOUT_MS.
static bool waitClear(volatile uint32_t* reg, uint32_t flags) {
  const uint32_t startMs = elapsedMs();
  while ((*reg & flags) != 0u) {
    if (elapsedMs() - startMs >= CV_OPTION_TIMEOUT_MS) return false;
  }
  return true;
}

// One change of OPTSR_PRG and WPSN_PRG1 (RM0433, option byte modification). No flash operation
// may run meanwhile. Every other option bit is written back as it stands.
static uint32_t optionsProgram(void) {
  if (!waitClear(&FLASH->SR1, FLASH_SR_QW) || !waitClear(&FLASH->SR2, FLASH_SR_QW)) return CvOptionTimeout;
  if ((FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) != 0u) {
    FLASH->OPTKEYR = FLASH_OPT_KEY1;
    FLASH->OPTKEYR = FLASH_OPT_KEY2;
  }
  if ((FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) != 0u) return CvOptionLocked;
  FLASH->OPTCCR = FLASH_OPTCCR_CLR_OPTCHANGEERR;
  FLASH->OPTSR_PRG = cvOptionsOptsr(FLASH->OPTSR_PRG);
  FLASH->WPSN_PRG1 = cvOptionsWpsn(FLASH->WPSN_PRG1);
  FLASH->OPTCR |= FLASH_OPTCR_OPTSTART;
  uint32_t result = CvOptionProgrammed;
  if (!waitClear(&FLASH->OPTSR_CUR, FLASH_OPTSR_OPT_BUSY)) {
    result = CvOptionTimeout;
  } else if ((FLASH->OPTSR_CUR & FLASH_OPTSR_OPTCHANGEERR) != 0u) {
    result = CvOptionChangeError;
  } else if (cvOptionsWanted(FLASH->OPTSR_CUR, FLASH->WPSN_CUR1)) {
    result = CvOptionNotApplied;
  }
  FLASH->OPTCR |= FLASH_OPTCR_OPTLOCK;
  return result;
}

// BOR level 3 keeps the core out of the voltage range where a write can tear, and the protected
// sector 0 keeps any application bug from erasing the bootloader. One attempt per power-on: the
// block says Interrupted until the step returns, so a fault or a reset inside it is not retried.
static void optionBytesCheck(struct CvBootBlock* block) {
  const uint32_t optsr = FLASH->OPTSR_CUR;
  const uint32_t wpsn = FLASH->WPSN_CUR1;
  if (!cvOptionsWanted(optsr, wpsn)) {
    block->optionBytes = CvOptionAlreadySet;
  } else if (cvOptionsAttempt(block, optsr, wpsn)) {
    block->optionBytes = CvOptionInterrupted;
    __DMB();  // the store lands before the change starts, so a fault inside it finds the mark
    block->optionBytes = optionsProgram();
  }
}

//--------------------------------------------------------------------+
// Boot decision
//--------------------------------------------------------------------+

static void menuSwitchStart(void) {
  GPIO_InitTypeDef pin = {0};
  pin.Pin = CV_MENU_PIN;
  pin.Mode = GPIO_MODE_INPUT;
  pin.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(CV_MENU_PORT, &pin);
}

// Sampled once the pull-up has had CV_MENU_SETTLE_MS; the image check overlaps that wait.
static bool menuSwitchHeld(void) {
  while (elapsedMs() < CV_MENU_SETTLE_MS) {}
  return HAL_GPIO_ReadPin(CV_MENU_PORT, CV_MENU_PIN) == GPIO_PIN_RESET;
}

// Header and CRC, then the port's stack and reset vector check, so check_dfu_mode's own
// board_app_valid() never disagrees with the decision. A fault in here is an unreadable image.
static void imageRead(void) {
  gState.imageReading = true;
  cvImageInspect((const uint8_t*)CV_APP_START, CV_APP_END - CV_APP_START, &gState.image);
  if (gState.image.state == CvImageGood && !board_app_valid()) gState.image.state = CvImageBad;
  gState.imageReading = false;
}

static CvImageStatus imageStatus(void) {
  return gState.image.state == CvImageGood ? CvImageValid : CvImageInvalid;
}

// Erases the application sector at `address`, bank 1 only, never below CV_APP_START. True when the
// HAL reports the erase done. A failed erase is not read back: the word that faulted may remain.
static bool appSectorErase(uint32_t address) {
  if (address < CV_APP_START || address >= CV_APP_END || address % CV_FLASH_SECTOR_SIZE != 0u) return false;
  FLASH_EraseInitTypeDef erase = {0};
  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.Banks = FLASH_BANK_1;
  erase.Sector = (address - FLASH_BANK1_BASE) / CV_FLASH_SECTOR_SIZE;
  erase.NbSectors = 1u;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_4;
  uint32_t failedSector = 0u;
  bool ok = HAL_FLASH_Unlock() == HAL_OK;
  if (ok) {
    __HAL_FLASH_CLEAR_FLAG_BANK1(FLASH_FLAG_ALL_ERRORS_BANK1);
    ok = HAL_FLASHEx_Erase(&erase, &failedSector) == HAL_OK;
  }
  HAL_FLASH_Lock();
  SCB_InvalidateDCache_by_Addr((void*)address, (int32_t)CV_FLASH_SECTOR_SIZE);
  return ok;
}

// After a decision on a pending request: erase the sector a read fault named, then read the image
// unless the decision says reading could fault again.
static void imageAfterDecision(void) {
  const uint32_t erase = gState.decision.eraseAddress;
  gState.eraseFailed = erase != 0u && !appSectorErase(erase);
  if (gState.decision.imageReadable && !gState.eraseFailed) {
    imageRead();
  } else {
    gState.image.state = CvImageBad;
  }
}

static void updateModeStart(void);

// Runs before check_dfu_mode reads any flash. A pending request may name an unreadable word, so the
// image is read only after the decision has erased that word's sector.
void board_init2(void) {
  cycleCounterStart();
  struct CvBootBlock* block = CV_BLOCK;
  cvBootAdopt(block, RCC->RSR);
  gState.adopted = true;
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
  optionBytesCheck(block);
  menuSwitchStart();

  const bool pending = cvBootRequestPending(block);
  if (!pending) imageRead();
  const CvBootInputs in = {menuSwitchHeld(), pending ? CvImageInvalid : imageStatus(), cvClockFailedStep()};
  gState.decision = cvBootDecide(block, &in);
  if (pending) imageAfterDecision();
  if (gState.decision.action == CvActionUpdateMode) updateModeStart();
}

// Asked by check_dfu_mode after the port's vector check: the decision board_init2 made.
bool board_app_valid2(void) {
  return gState.decision.action == CvActionRunApp;
}

// Starts IWDG1 for CV_WATCHDOG_BOOT_MS (boot_block.h), the order RM0433 gives. Until the new values
// land the reset period of about 0.5 s applies, a few LSI cycles.
static void watchdogStart(void) {
  IWDG1->KR = CV_IWDG_KEY_START;
  IWDG1->KR = CV_IWDG_KEY_ACCESS;
  IWDG1->PR = CV_IWDG_PRESCALER_256;
  IWDG1->RLR = CV_IWDG_RELOAD;
  const uint32_t startMs = elapsedMs();
  while (IWDG1->SR != 0u && elapsedMs() - startMs < CV_IWDG_READY_MS) {}
  IWDG1->KR = CV_IWDG_KEY_RELOAD;
}

// Before the jump to the application: the MENU pin back to its reset state, and the watchdog that
// ends a hang in setup() with a counted start.
void board_teardown2(void) {
  HAL_GPIO_DeInit(CV_MENU_PORT, CV_MENU_PIN);
  watchdogStart();
}

//--------------------------------------------------------------------+
// Fault handlers
//--------------------------------------------------------------------+

// Every fault restarts the stack at the top of RAM, so a fault on a broken stack still records. r0
// carries the exception frame, from the stack in use at the fault.
#define CV_FAULT_ENTRY(handler)                  \
  __attribute__((naked)) void handler(void) {    \
    __asm volatile(                              \
        "tst lr, #4                \n"           \
        "ite eq                    \n"           \
        "mrseq r0, msp             \n"           \
        "mrsne r0, psp             \n"           \
        "movw r1, #:lower16:_estack \n"          \
        "movt r1, #:upper16:_estack \n"          \
        "msr msp, r1               \n"           \
        "b cvBootFault             \n");         \
  }

CV_FAULT_ENTRY(NMI_Handler)
CV_FAULT_ENTRY(HardFault_Handler)
CV_FAULT_ENTRY(MemManage_Handler)
CV_FAULT_ENTRY(BusFault_Handler)
CV_FAULT_ENTRY(UsageFault_Handler)

// The stacked pc, or 0 when the frame is not wholly inside the bootloader's RAM.
static uint32_t framePc(const uint32_t* frame) {
  const uint32_t at = (uint32_t)frame;
  if (at % 4u != 0u || at < CV_RAM_START || at > CV_RAM_END - 8u * sizeof(uint32_t)) return 0u;
  return frame[6];
}

// The address of a double ECC error: bank 1 for a word whose programming a power loss cut, bank 2
// for the diagnostic log. 0 for any other fault.
static uint32_t eccAddress(void) {
  if (FLASH->SR1 & FLASH_SR_DBECCERR) {
    return FLASH_BANK1_BASE + (FLASH->ECC_FA1 & FLASH_ECC_FA_FAIL_ECC_ADDR) * CV_FLASH_WORD_BYTES;
  }
  if (FLASH->SR2 & FLASH_SR_DBECCERR) {
    return FLASH_BANK2_BASE + (FLASH->ECC_FA2 & FLASH_ECC_FA_FAIL_ECC_ADDR) * CV_FLASH_WORD_BYTES;
  }
  return 0u;
}

// cvBootRecordFault decides what the next start does; this records and resets.
void cvBootFault(const uint32_t* frame) {
  CvFaultInfo fault;
  fault.resetFlags = RCC->RSR;
  fault.blockAdopted = gState.adopted;
  fault.eccAddress = eccAddress();
  fault.imageReading = gState.imageReading;
  fault.updateMode = gState.updateMode;
  fault.pc = framePc(frame);
  fault.cfsr = SCB->CFSR;
  fault.hfsr = SCB->HFSR;
  fault.bfar = SCB->BFAR;
  cvBootRecordFault(CV_BLOCK, &fault);
  __DSB();
  NVIC_SystemReset();
}

//--------------------------------------------------------------------+
// Update mode: the drive's texts
//--------------------------------------------------------------------+

// The USB serial as text: upstream's board_usb_get_serial() bytes, high nibble first, as TinyUF2's
// string descriptor spells them.
static const char* serialText(void) {
  static char text[2u * 16u + 1u];
  if (text[0] == '\0') {
    uint8_t id[16];
    const uint8_t length = board_usb_get_serial(id);
    for (uint8_t i = 0; i < length; ++i) {
      text[2u * i] = cvTextHexDigit(id[i] >> 4);
      text[2u * i + 1u] = cvTextHexDigit(id[i]);
    }
    text[2u * length] = '\0';
  }
  return text;
}

static CvDriveState driveState(void) {
  const CvDriveState state = {CV_BLOCK,           &gState.image,      CV_FAULT_RECORD,
                              serialText(),       TUSB_VERSION_STRING, gState.blocks,
                              gState.wrongFile,   cvClockFailedStep(), FLASH->OPTSR_CUR,
                              FLASH->WPSN_CUR1,   (const uint8_t*)CV_DIAG_LOG_ADDR};
  return state;
}

static void diagRefresh(void) {
  const CvDriveState state = driveState();
  cvDiagWrite(&state);
  gState.diagDirty = false;
  gState.diagMs = elapsedMs();
}

// Each mount gets both texts afresh. Once a write has begun the image is inspected again, so
// Installed and installed say what the flash holds now.
static void mountRefresh(void) {
  if (gState.writeBegun) imageRead();
  const CvDriveState state = driveState();
  cvInfoText(&state);
  diagRefresh();
}

char const* board_uf2_info(uint32_t* len) {
  const CvDriveState state = driveState();
  *len = CV_INFO_TEXT_BYTES;
  return cvInfoText(&state);
}

//--------------------------------------------------------------------+
// Update mode: screens, cable, host, timeout
//--------------------------------------------------------------------+

static bool timeoutArmed(void) {
  return cvBootTimeoutAllowed(CV_BLOCK, imageStatus(), gState.hostSeen, gState.writeBegun);
}

#if CV_OLED
static CvScreen screenNow(void) {
  const CvScreenInputs in = {gState.decision.reason, gState.image.state == CvImageGood, gState.writeBegun,
                             gState.sessionAccepted, gState.sessionRefused,             gState.sessionFailed,
                             gState.cableOut,        timeoutArmed()};
  return cvUpdateScreen(&in);
}

static void screenShow(CvScreen screen) {
  static uint8_t frame[CV_FRAME_BYTES];
  cvScreenDraw(frame, screen, cvInstalledText(&gState.image), gState.mounted);
  cvOledShow(frame);
}

static void screenRefresh(void) {
  const CvScreen screen = screenNow();
  const int16_t shown = (int16_t)((int16_t)screen * 2 + (gState.mounted ? 1 : 0));
  if (shown == gState.shown) return;
  gState.shown = shown;
  screenShow(screen);
}
#endif

// A cut power can leave a torn word anywhere in the region, and the image check reads only the
// image. One read of every flash word before USB starts brings such a word to the fault handler
// now, which erases its sector at the next start, instead of halfway through a copy. When the
// decision says the image must not be read, or the erase of the faulting sector just failed, the
// read is skipped, or the same word would fault at every start.
static void regionRead(void) {
  if (gState.eraseFailed || !gState.decision.imageReadable) return;
  gState.imageReading = true;
  uint32_t sink = 0u;
  for (uint32_t at = CV_APP_START; at < CV_APP_END; at += CV_FLASH_WORD_BYTES) {
    sink ^= *(volatile const uint32_t*)at;
  }
  gState.imageReading = false;
  (void)sink;
}

static bool vbusPresent(void) {
  return HAL_GPIO_ReadPin(CV_VBUS_PORT, CV_VBUS_PIN) == GPIO_PIN_SET;
}

static void vbusStart(void) {
  GPIO_InitTypeDef pin = {0};
  pin.Pin = CV_VBUS_PIN;
  pin.Mode = GPIO_MODE_INPUT;
  pin.Pull = GPIO_NOPULL;  // a pull would skew the R6/R7 divider
  HAL_GPIO_Init(CV_VBUS_PORT, &pin);
  cvDebounceReset(&gState.vbus, vbusPresent());
}

static void updateModeStart(void) {
  gState.updateMode = true;
  regionRead();
  vbusStart();
  diagRefresh();
#if CV_OLED
  cvOledStart();
  screenRefresh();
#endif
}

// A self-powered device holds its D+ pull-up only while the cable carries VBUS. A pull leaves
// TinyUSB mounted, because the core's own VBUS sensing is off, so the board posts the unplug event
// that sensing would. Without a write the box may return to the application again, 60 s from the
// pull.
static void cableTrack(uint32_t nowMs) {
  const bool first = !gState.cableTracked;
  const bool changed = cvDebounceUpdate(&gState.vbus, vbusPresent(), nowMs, CV_VBUS_DEBOUNCE_MS);
  gState.cableTracked = true;
  if (!first && !changed) return;
  if (gState.vbus.level) {
    gState.cableOut = false;
    tud_connect();
    return;
  }
  tud_disconnect();
  if (first) return;
  dcd_event_bus_signal(BOARD_TUD_RHPORT, DCD_EVENT_UNPLUGGED, false);
  gState.cableOut = gState.hostSeen;
  if (!gState.writeBegun) {
    gState.hostSeen = false;
    gState.timeoutFromMs = nowMs;
  }
}

// tud_mounted() is the state the port's tud_mount_cb reports. A mount or an unmount starts a new
// host session.
static void hostTrack(void) {
  const bool mounted = tud_mounted();
  if (mounted == gState.mounted) return;
  gState.mounted = mounted;
  gState.sessionAccepted = false;
  gState.sessionRefused = false;
  gState.sessionFailed = false;
  if (mounted) {
    gState.hostSeen = true;
    mountRefresh();
  }
}

// Each pass of the DFU loop. The timeout resets rather than jumps: the request is consumed, so the
// decision runs the application from a clean reset with USB and I2C untouched.
void board_dfu_task(void) {
  const uint32_t nowMs = elapsedMs();
  cableTrack(nowMs);
  hostTrack();
  if (timeoutArmed() && nowMs - gState.timeoutFromMs >= CV_UPDATE_TIMEOUT_MS) NVIC_SystemReset();
  if (gState.diagDirty && nowMs - gState.diagMs >= CV_DIAG_REFRESH_MS) diagRefresh();
#if CV_OLED
  screenRefresh();
#endif
}

//--------------------------------------------------------------------+
// Update mode: UF2 blocks and the root directory
//--------------------------------------------------------------------+

static void blockRefused(uint32_t familyId) {
  gState.blocks.refused += 1u;
  gState.blocks.lastRefusedFamily = familyId;
  if (gState.sessionAccepted) {
    gState.sessionFailed = true;
  } else {
    gState.sessionRefused = true;
  }
  gState.diagDirty = true;
}

// Flash this transfer erased that is neither blank nor the block's own payload.
static bool targetHoldsOtherData(UF2_Block const* block) {
  const uint8_t* target = (const uint8_t*)block->targetAddr;
  bool blank = true;
  for (uint32_t i = 0; i < block->payloadSize && blank; ++i) blank = target[i] == 0xFFu;
  return !blank && memcmp(target, block->data, block->payloadSize) != 0;
}

// Before ghostfat writes or counts a block. A refused block is counted for DIAG.TXT and the screens
// and never written. A second file in one host session, or the file dropped again after a failed
// transfer, restarts the transfer, so it is erased and counted afresh instead of meeting the words
// already written.
bool board_uf2_block_accept(UF2_Block const* block, WriteState const* state) {
  if (!cvUf2BlockAccepted(block->familyID, block->targetAddr, block->payloadSize)) {
    blockRefused(block->familyID);
    return false;
  }
  if (state->numBlocks == 0u) gState.sessionSectors = 0u;
  const uint32_t sector = 1u << ((block->targetAddr - CV_APP_START) / CV_FLASH_SECTOR_SIZE);
  const bool erased = (gState.sessionSectors & sector) != 0u;
  const CvUf2SessionInputs session = {state->numBlocks, block->numBlocks, block->blockNo, erased,
                                      erased && targetHoldsOtherData(block), gState.sessionFailed};
  if (cvUf2SessionRestart(&session)) {
    uf2_write_state_reset();
    gState.sessionSectors = 0u;
    gState.sessionRefused = false;
    gState.sessionFailed = false;
  }
  gState.sessionSectors |= sector;
  gState.writeBegun = true;
  gState.sessionAccepted = true;
  return true;
}

// After the flash write of an accepted block. A failed one does not count toward the end of the
// transfer, so the update cannot complete over it.
void board_uf2_block_written(UF2_Block const* block, bool written) {
  if (written) {
    gState.blocks.written += 1u;
    return;
  }
  if (gState.blocks.failed == 0u) gState.blocks.firstFailedAddress = block->targetAddr;
  gState.blocks.failed += 1u;
  gState.sessionFailed = true;
  gState.diagDirty = true;
}

// A root directory sector from the host. A file that cannot be firmware shows the wrong-file screen,
// as refused blocks do, and goes into DIAG.TXT by name.
void board_uf2_dir_written(uint8_t const* sector) {
  char name[sizeof gState.wrongFile];
  if (!cvDirNamesWrongFile(sector, name)) return;
  if (strcmp(name, gState.wrongFile) != 0) {
    memcpy(gState.wrongFile, name, sizeof name);
    gState.diagDirty = true;
  }
  if (!gState.sessionAccepted) gState.sessionRefused = true;
}

// After the last block: a fresh start count for the new image, then a reset into the decision.
void board_dfu_complete(void) {
#if CV_OLED
  screenShow(CvScreenDone);
#endif
  cvBootMarkUpdated(CV_BLOCK);
  SCB_CleanDCache();
  NVIC_SystemReset();
}
