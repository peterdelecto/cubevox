// TinyUF2 board for cubevox: STM32H743VIT6, 25 MHz crystal, TinyUF2 in bank 1 sector 0 and the
// application from CV_APP_START. Addresses, magics and ids come from boot_block.h.

#ifndef BOARD_H_
#define BOARD_H_

#include "boot_block.h"
#include "clock.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shown in INFO_UF2.TXT and DIAG.TXT. Bumped with every bootloader change; independent of
// VERSION.txt because the two programs ship on different paths.
#define CV_BOOT_VERSION "0.05"

//--------------------------------------------------------------------+
// Board identity
//--------------------------------------------------------------------+

// USER_LED (pins.h kUserLed). The sealed box has no reset button Adam can reach, so no double
// tap: the 500 ms wait would cost every boot for nothing.
#define LED_PORT             GPIOB
#define LED_PIN              GPIO_PIN_7
#define CV_OLED              1
#define TINYUF2_DBL_TAP_DFU  0
#define CV_BOARD_FAMILY      CV_UF2_FAMILY_CUBEVOX
#define UF2_BOARD_ID         "cubevox-h743-v4"
#define UF2_VOLUME_LABEL     "CUBEVOX"
#define LED_STATE_ON         1

//--------------------------------------------------------------------+
// MENU switch
//--------------------------------------------------------------------+

// ENC_MENU_SW (pins.h kEncMenuSw), low while pressed. Held at reset, it forces update mode.
#define CV_MENU_PORT         GPIOE
#define CV_MENU_PIN          GPIO_PIN_13
#define CV_MENU_SETTLE_MS    20u

//--------------------------------------------------------------------+
// Flash
//--------------------------------------------------------------------+

// The port writes [BOARD_FLASH_APP_START, FLASH_BANK1_BASE + BOARD_FLASH_SIZE). Ending that at
// CV_APP_END keeps every UF2 write in bank 1; bank 2 holds the layout record and Adam's data.
#define BOARD_FLASH_APP_START  CV_APP_START
#define BOARD_FLASH_SIZE       (CV_APP_END - FLASH_BANK1_BASE)

//--------------------------------------------------------------------+
// USB UF2
//--------------------------------------------------------------------+

#define USB_VID              CV_USB_VID_BOOT
#define USB_PID              CV_USB_PID_BOOT
#define USB_MANUFACTURER     "cubevox"
#define USB_PRODUCT          "cubevox update"
#define UF2_PRODUCT_NAME     "cubevox"

// OTG_FS on PA11/PA12. The box is self-powered and PA9 is not OTG VBUS, so the port's B-session
// override keeps the core attached and board_hooks.c senses the cable on VBUS_SENSE (pins.h
// kVbusSense, the R6/R7 divider) as a plain input, as the application's usb_link.cpp does.
#define USB_NO_VBUS_PIN      1
#define BOARD_TUD_RHPORT     0
#define CV_VBUS_PORT         GPIOA
#define CV_VBUS_PIN          GPIO_PIN_9
#define CV_VBUS_DEBOUNCE_MS  50u

// The drive serves INFO_UF2.TXT and DIAG.TXT only (diag.c). Every UF2 block passes
// board_uf2_block_accept() and reports its write to board_uf2_block_written(), and every root
// directory sector the host writes reaches board_uf2_dir_written() (board_hooks.c). Both files
// have a fixed size, so the directory never changes while their text does.
#define TINYUF2_INDEX_HTM           0
#define TINYUF2_CURRENT_UF2         0
#define TINYUF2_BOARD_INFO_UF2      1
#define TINYUF2_BOARD_BLOCK_FILTER  1
#define TINYUF2_BOARD_DIR_HOOK      1
#define CV_INFO_TEXT_BYTES          512u
#define CV_DIAG_TEXT_BYTES          4096u
extern char cvDiagText[CV_DIAG_TEXT_BYTES];
#define TINYUF2_BOARD_FILES {.name = "DIAG    TXT", .content = cvDiagText, .size = CV_DIAG_TEXT_BYTES},

#ifdef __cplusplus
}
#endif

#endif
