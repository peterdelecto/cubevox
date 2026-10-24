# WeAct H743 test board: the cubevox board with the OLED off, the LED on PE3, double tap on and the
# WeAct UF2 family, so a bench image for it never lands on a cubevox box (cubevox/board.h).
CV_BOARD = weact
include $(TOP)/$(PORT_DIR)/boards/cubevox/board.mk
