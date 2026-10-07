#ifndef S2P_BOOTSEL_H
#define S2P_BOOTSEL_H

#include <stdbool.h>

// Reads the BOOTSEL button at runtime. It shares a pin with the flash chip
// select, so flash access is briefly suspended while sampling.
bool bootsel_pressed(void);

#endif
