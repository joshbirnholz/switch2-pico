#ifndef S2P_VERSION_H
#define S2P_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

// Firmware version. The Pico build passes it from the VERSION file; other
// builds use this default (keep it in sync with VERSION).
#ifndef S2P_VERSION
#define S2P_VERSION "0.7.1"
#endif

#ifdef __cplusplus
}
#endif

#endif
