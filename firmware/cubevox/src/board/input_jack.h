// Drives PAD_EN, which switches relay K103 between the MIC and LINE input networks.

#pragma once

#include "core/settings.h"

// Drives PAD_EN low (MIC, coil off). Call in setup() right after muteInit().
void inputJackInit();

// Line drives PAD_EN high (pad in circuit); Mic drives it low.
void inputJackApply(InputJack jack);
