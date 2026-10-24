#include "board/input_jack.h"

#include <Arduino.h>

#include "board/pins.h"

void inputJackInit() {
  digitalWrite(pins::kPadEn, LOW);
  pinMode(pins::kPadEn, OUTPUT);
}

void inputJackApply(InputJack jack) {
  digitalWrite(pins::kPadEn, jack == InputJack::Line ? HIGH : LOW);
}
