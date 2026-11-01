// While the host sleeps (paused), the dropped controller's own adverts are
// ignored until it goes quiet; the next advert is a button press.
#include <assert.h>
#include <stdio.h>
#include "../core/src/s2_link.c"
static uint32_t test_now;
uint32_t platform_millis(void) { return test_now; }
void log_printf(const char *fmt, ...) { (void)fmt; }

int main(void) {
    // Still advertising right after the drop: not a press.
    test_now = 1000;
    pause_quiet_start();
    for (int i = 0; i < 40; i++) {
        test_now += 100;
        assert(!pause_advert_is_press(test_now));
    }
    // Quiet for longer than the gap, then an advert: a press.
    test_now += PAUSE_GAP_MS + 1;
    assert(pause_advert_is_press(test_now));
    test_now += 100;
    assert(pause_advert_is_press(test_now));   // and the adverts after it

    // No adverts after the drop: the first one after the gap is a press,
    // well before PAUSE_QUIET_MS.
    pause_quiet_start();
    test_now += PAUSE_GAP_MS - 500;
    assert(!pause_advert_is_press(test_now));
    pause_quiet_start();
    test_now += PAUSE_GAP_MS + 500;
    assert(pause_advert_is_press(test_now));

    // Adverts that never stop count only after PAUSE_QUIET_MS.
    pause_quiet_start();
    uint32_t start = test_now;
    while (test_now - start < PAUSE_QUIET_MS - 500) {
        test_now += 500;
        assert(!pause_advert_is_press(test_now));
    }
    test_now = start + PAUSE_QUIET_MS;
    assert(pause_advert_is_press(test_now));
    puts("Paused wake-up: passed");
}
