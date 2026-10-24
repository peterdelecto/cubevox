// cubevox's TinyUF2 board hooks: the boot decision before any flash read, the fault handlers, the
// update-mode timeout and screens, the UF2 block filter and the end of an update. The policy is
// boot_decide.c; this file reads the hardware and acts on its answers.

#include "board_api.h"
#include "tusb.h"

#include "boot_decide.h"
#include "diag.h"
#include "image_check.h"
#if CV_OLED
#include "oled.h"
#include "screens.h"
#endif

CV_STATIC_ASSERT(family_from_boot_block, BOARD_UF2_FAMILY_ID == CV_BOARD_FAMILY);
CV_STATIC_ASSERT(menu_settles_before_timeout, CV_MENU_SETTLE_MS < CV_UPDATE_TIMEOUT_MS);
CV_STATIC_ASSERT(dbeccerr_is_cmsis, CV_FLASH_SR_DBECCERR == FLASH_SR_DBECCERR);
CV_STATIC_ASSERT(ecc_address_is_cmsis, CV_FLASH_ECC_ADDR == FLASH_ECC_FA_FAIL_ECC_ADDR);
CV_STATIC_ASSERT(bank2_base_is_cmsis, CV_FLASH_BANK2_BASE == FLASH_BANK2_BASE);
CV_STATIC_ASSERT(flash_word_is_hal, CV_FLASH_WORD_BYTES == 4u * FLASH_NB_32BITWORD_IN_FLASHWORD);
// A double ECC error while DIAG.TXT reads the log decodes to an address inside the log sector, so no
// separate marker is needed around the read.
CV_STATIC_ASSERT(ecc_decode_covers_log,
                 CV_FLASH_BANK2_BASE + (CV_FLASH_ECC_ADDR + 1u) * CV_FLASH_WORD_BYTES >=
                         CV_DIAG_LOG_ADDR + CV_DIAG_LOG_SIZE);

#define CV_BLOCK ((struct CvBootBlock*)CV_BOOT_BLOCK_ADDR)
#define CV_FAULT_RECORD ((const struct CvFaultRecord*)CV_FAULT_RECORD_ADDR)

// The port's temporary boot address, for boards that boot from QSPI or AXI SRAM. This board boots
// from internal flash only, so the words live in .bss, clear of the boot block's counter, which
// sits where common.ld would put them.
volatile uint32_t _board_tmp_boot_addr[1];
volatile uint32_t _board_tmp_boot_magic[1];

void cvBootFault(void) __attribute__((used, noreturn));

typedef struct {
  CvBootDecision decision;
  // The installed image, read once reading is safe.
  struct CvImageReport image;
  // Time since board_init2, from the cycle counter.
  uint32_t elapsedMs;
  uint32_t cycleMark;
  uint32_t cycleCarry;
  // A host has enumerated since update mode began; the timeout is off for good.
  bool hostSeen;
  bool mounted;
  // A UF2 block was accepted since update mode began; the installed image is no longer intact.
  bool writeBegun;
  // Blocks accepted or refused since the host last mounted the drive.
  bool sessionAccepted;
  bool sessionRefused;
  uint32_t refusedBlocks;
  uint32_t lastRefusedFamily;
  uint32_t diagRefusedBlocks;
  // The screen and host state on the panel, -1 before the first draw.
  int shown;
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
// far more often while the timeout can still fire: no block is written then, so the DFU loop never
// waits on a sector erase. Erases during a write may stretch a gap, and the timeout is off by then.
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
// board_app_valid() never disagrees with the decision. A read fault here ends in cvBootFault.
static void imageRead(void) {
  cvImageInspect((const uint8_t*)CV_APP_START, CV_APP_END - CV_APP_START, &gState.image);
  if (gState.image.state == CvImageGood && !board_app_valid()) gState.image.state = CvImageBad;
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

static void updateModeStart(void);

// Runs before check_dfu_mode reads any flash. A pending request may name an unreadable word, so the
// image is read only after the decision has erased that word's sector.
void board_init2(void) {
  cycleCounterStart();
  menuSwitchStart();
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;

  struct CvBootBlock* block = CV_BLOCK;
  const bool requested = block->request == CV_REQUEST_UPDATE;
  if (!requested) imageRead();
  const bool menuHeld = menuSwitchHeld();
  gState.decision = cvBootDecide(block, menuHeld, requested ? CvImageInvalid : imageStatus());

  const uint32_t erase = gState.decision.eraseAddress;
  if (requested) {
    if (erase == 0u || appSectorErase(erase)) {
      imageRead();
    } else {
      gState.image.state = CvImageBad;
    }
  }
  if (gState.decision.action == CvActionUpdateMode) updateModeStart();
}

// Asked by check_dfu_mode after the port's vector check: the decision board_init2 made.
bool board_app_valid2(void) {
  return gState.decision.action == CvActionRunApp;
}

// Before the jump to the application: the MENU pin back to its reset state.
void board_teardown2(void) {
  HAL_GPIO_DeInit(CV_MENU_PORT, CV_MENU_PIN);
}

//--------------------------------------------------------------------+
// Fault handlers
//--------------------------------------------------------------------+

// Every fault restarts the stack at the top of RAM, so a fault on a broken stack still records.
#define CV_FAULT_ENTRY(handler)                  \
  __attribute__((naked)) void handler(void) {    \
    __asm volatile(                              \
        "movw r0, #:lower16:_estack \n"          \
        "movt r0, #:upper16:_estack \n"          \
        "msr msp, r0               \n"           \
        "b cvBootFault             \n");         \
  }

CV_FAULT_ENTRY(NMI_Handler)
CV_FAULT_ENTRY(HardFault_Handler)
CV_FAULT_ENTRY(MemManage_Handler)
CV_FAULT_ENTRY(BusFault_Handler)
CV_FAULT_ENTRY(UsageFault_Handler)

// A double ECC error in bank 1 is a word whose programming a power loss cut; the decision erases its
// sector on the next boot. One in bank 2 can only come from reading the diagnostic log for DIAG.TXT.
// It records the address and nothing else, so the image check still decides and no bank 2 sector is
// erased here. DIAG.TXT then skips the log, and the application erases the sector. Any other fault
// records address 0.
void cvBootFault(void) {
  uint32_t address = 0u;
  if (FLASH->SR1 & FLASH_SR_DBECCERR) {
    address = FLASH_BANK1_BASE + (FLASH->ECC_FA1 & FLASH_ECC_FA_FAIL_ECC_ADDR) * CV_FLASH_WORD_BYTES;
  } else if (FLASH->SR2 & FLASH_SR_DBECCERR) {
    address = FLASH_BANK2_BASE + (FLASH->ECC_FA2 & FLASH_ECC_FA_FAIL_ECC_ADDR) * CV_FLASH_WORD_BYTES;
  }
  cvBootRecordReadFault(CV_BLOCK, address);
  __DSB();
  NVIC_SystemReset();
}

//--------------------------------------------------------------------+
// Update mode
//--------------------------------------------------------------------+

// The USB serial as text: upstream's board_usb_get_serial() bytes, high nibble first, as TinyUF2's
// string descriptor spells them.
static const char* serialText(void) {
  static char text[2u * 16u + 1u];
  if (text[0] == '\0') {
    static const char kHex[] = "0123456789ABCDEF";
    uint8_t id[16];
    const uint8_t length = board_usb_get_serial(id);
    for (uint8_t i = 0; i < length; ++i) {
      text[2u * i] = kHex[id[i] >> 4];
      text[2u * i + 1u] = kHex[id[i] & 0xFu];
    }
    text[2u * length] = '\0';
  }
  return text;
}

static CvDriveState driveState(void) {
  const CvDriveState state = {CV_BLOCK,     &gState.image,      CV_FAULT_RECORD,         serialText(),
                              TUSB_VERSION_STRING, gState.refusedBlocks, gState.lastRefusedFamily,
                              (const uint8_t*)CV_DIAG_LOG_ADDR};
  return state;
}

static void diagRefresh(void) {
  const CvDriveState state = driveState();
  cvDiagWrite(&state);
  gState.diagRefusedBlocks = gState.refusedBlocks;
}

#if CV_OLED
// Writing and a wrong file belong to the current host session. Once a write has begun the image is
// no longer intact, so the waiting screen says the last update did not finish.
static CvScreen screenNow(void) {
  if (gState.sessionAccepted) return CvScreenWriting;
  if (gState.sessionRefused) return CvScreenWrongFile;
  if (gState.writeBegun) return CvScreenWaitNoImage;
  switch (gState.decision.reason) {
    case CvReasonMenuHeld:
    case CvReasonAppRequest: return gState.image.state == CvImageGood ? CvScreenWaitMenu : CvScreenWaitNoImage;
    case CvReasonBootFailed: return CvScreenWaitBootFailed;
    default: return CvScreenWaitNoImage;
  }
}

static void screenShow(CvScreen screen) {
  static uint8_t frame[CV_FRAME_BYTES];
  cvScreenDraw(frame, screen, cvInstalledText(&gState.image), gState.mounted);
  cvOledShow(frame);
}

static void screenRefresh(void) {
  const CvScreen screen = screenNow();
  const int shown = (int)screen * 2 + (gState.mounted ? 1 : 0);
  if (shown == gState.shown) return;
  gState.shown = shown;
  screenShow(screen);
}
#endif

static void updateModeStart(void) {
  diagRefresh();
#if CV_OLED
  cvOledStart();
  screenRefresh();
#endif
}

// tud_mounted() is the state the port's tud_mount_cb reports.
static void hostTrack(void) {
  const bool mounted = tud_mounted();
  if (mounted == gState.mounted) return;
  gState.mounted = mounted;
  gState.sessionAccepted = false;
  gState.sessionRefused = false;
  if (mounted) {
    gState.hostSeen = true;
    diagRefresh();
  }
}

// Each pass of the DFU loop. The timeout resets rather than jumps: the request is consumed, so the
// decision runs the application from a clean reset with USB and I2C untouched.
void board_dfu_task(void) {
  const uint32_t nowMs = elapsedMs();
  hostTrack();
  if (nowMs >= CV_UPDATE_TIMEOUT_MS &&
      cvBootTimeoutAllowed(CV_BLOCK, imageStatus(), gState.hostSeen, gState.writeBegun)) {
    NVIC_SystemReset();
  }
  if (gState.refusedBlocks != gState.diagRefusedBlocks) diagRefresh();
#if CV_OLED
  screenRefresh();
#endif
}

char const* board_uf2_info(uint32_t* len) {
  const CvDriveState state = driveState();
  return cvInfoText(&state, len);
}

// Only this board's family, inside the application region, whole flash words. A refused block is
// counted for DIAG.TXT and the WRONG FILE screen and never written.
bool board_uf2_block_accept(uint32_t family_id, uint32_t addr, uint32_t len) {
  const bool inRegion = addr >= CV_APP_START && addr < CV_APP_END && len <= CV_APP_END - addr;
  const bool wholeWords = len != 0u && addr % CV_FLASH_WORD_BYTES == 0u && len % CV_FLASH_WORD_BYTES == 0u;
  if (family_id == BOARD_UF2_FAMILY_ID && inRegion && wholeWords) {
    gState.writeBegun = true;
    gState.sessionAccepted = true;
    return true;
  }
  gState.refusedBlocks += 1u;
  gState.lastRefusedFamily = family_id;
  gState.sessionRefused = true;
  return false;
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
