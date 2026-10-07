#ifndef S2P_STATUS_LED_H
#define S2P_STATUS_LED_H

// The Pico W's on-board LED (driven through the CYW43 chip).
//   fast blink ........ looking for a controller (none paired yet)
//   slow blink ........ waiting for the paired controller to wake up
//   flicker ........... connecting / initialising
//   solid ............. controller connected
//   double blink ...... configuration Wi-Fi access point is on (overrides)
void status_led_task(void);

#endif
