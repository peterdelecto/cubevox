// TinyUF2 board for cubevox: STM32H743VIT6, 25 MHz crystal, TinyUF2 in bank 1 sector 0 and the
// application from CV_APP_START. Addresses, magics and ids come from boot_block.h. The WeAct H743
// test board builds this board with CV_BOARD_WEACT 1 (boards/weact).

#ifndef BOARD_H_
#define BOARD_H_

#include "boot_block.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shown in INFO_UF2.TXT and DIAG.TXT. Bumped with every bootloader change; independent of
// VERSION.txt because the two programs ship on different paths.
#define CV_BOOT_VERSION "0.02"

//--------------------------------------------------------------------+
// Board identity
//--------------------------------------------------------------------+

#if CV_BOARD_WEACT
// WeAct MiniSTM32H743: LED on PE3, a reset button for the double tap, no OLED.
#define LED_PORT             GPIOE
#define LED_PIN              GPIO_PIN_3
#define CV_OLED              0
#define TINYUF2_DBL_TAP_DFU  1
#define CV_BOARD_FAMILY      CV_UF2_FAMILY_WEACT
#define UF2_BOARD_ID         "cubevox-weact-h743"
#define UF2_VOLUME_LABEL     "WEACTBOOT"
#else
// USER_LED (pins.h kUserLed). The sealed box has no reset button Adam can reach, so no double
// tap: the 500 ms wait would cost every boot for nothing.
#define LED_PORT             GPIOB
#define LED_PIN              GPIO_PIN_7
#define CV_OLED              1
#define TINYUF2_DBL_TAP_DFU  0
#define CV_BOARD_FAMILY      CV_UF2_FAMILY_CUBEVOX
#define UF2_BOARD_ID         "cubevox-h743-v4"
#define UF2_VOLUME_LABEL     "CUBEVOX"
#endif
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
// override stands in for VBUS sensing.
#define USB_NO_VBUS_PIN      1
#define BOARD_TUD_RHPORT     0

// The drive serves INFO_UF2.TXT and DIAG.TXT only (diag.c), and every UF2 block passes
// board_uf2_block_accept() (board_hooks.c). DIAG.TXT has a fixed size, so the directory never
// changes while its text does. 2 KB holds the header lines and the 16 newest log lines.
#define TINYUF2_INDEX_HTM           0
#define TINYUF2_CURRENT_UF2         0
#define TINYUF2_BOARD_INFO_UF2      1
#define TINYUF2_BOARD_BLOCK_FILTER  1
#define CV_DIAG_TEXT_BYTES          2048u
extern char cvDiagText[CV_DIAG_TEXT_BYTES];
#define TINYUF2_BOARD_FILES {.name = "DIAG    TXT", .content = cvDiagText, .size = CV_DIAG_TEXT_BYTES},

// clock.c, mirroring firmware/cubevox/src/board/sysclock.cpp.
void clock_init(void);

#ifdef __cplusplus
}
#endif

#endif
