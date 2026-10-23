// Boards without an embedded page direct users to the hosted configuration.
// These weak defaults must not share a translation unit with the HTTP router:
// otherwise optimization can fold web_ui_size to zero even on Pico builds.
#include <stddef.h>
#include <stdint.h>
__attribute__((weak)) const uint8_t web_ui_data[1] = {0};
__attribute__((weak)) const size_t web_ui_size = 0;
