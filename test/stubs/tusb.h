#ifndef TEST_TUSB_H
#define TEST_TUSB_H
#include <stdbool.h>
bool tud_connect(void);
bool tud_disconnect(void);
bool tud_mounted(void);
bool tud_suspended(void);
bool tud_remote_wakeup(void);
#endif
