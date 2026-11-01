// Exercise real settings loading, CRCs and migrations with in-memory storage.
#include <assert.h>
#include "../core/src/settings.c"
static uint8_t flash_image[4096];
static size_t flash_size;
size_t platform_settings_read(void *dst, size_t cap) {
    assert(flash_size <= cap); memcpy(dst, flash_image, flash_size); return flash_size;
}
bool platform_settings_write(const void *src, size_t len) {
    assert(len <= sizeof flash_image); memcpy(flash_image, src, len); flash_size = len; return true;
}
void platform_unique_id(uint8_t out[PLATFORM_UNIQUE_ID_LEN]) { memset(out, 1, PLATFORM_UNIQUE_ID_LEN); }
bool platform_has_sync_button(void) { return true; }
uint32_t platform_millis(void) { return 0; }
void log_printf(const char *fmt, ...) { (void)fmt; }
static void checksum(void) {
    uint16_t size = (uint16_t)flash_size;
    memcpy(flash_image + offsetof(settings_t, size), &size, sizeof size);
    uint32_t crc = crc32(flash_image, flash_size - 4);
    memcpy(flash_image + flash_size - 4, &crc, 4);
}
static void legacy_image(uint8_t rev) {
    settings_t s; settings_defaults(&s);
    s.ext_rev = rev;
    s.joycon_single = s.decky_options = s.headset_low_latency = 1;
    s.bonds[0].used = 1; s.bonds[0].addr[5] = 42;
    const size_t at = offsetof(settings_t, prof);
    memcpy(flash_image, &s, at);
    for (int t = 0; t < CTRL_TYPE_COUNT; t++) {
        uint8_t *c = flash_image + at + t * CTRL_PROFILES_V16_SIZE;
        memset(c, 0, CTRL_PROFILES_V16_SIZE);
        for (int i = 0; i < PROFILE_MAX; i++) {
            profile_t p; settings_default_profile_for((ctrl_type_t)t, USB_MODE_SINPUT, &p);
            p.stick_deadzone_pct = (uint8_t)(t * 8 + i);
            p.rumble_style = 1; p.impulse_strength = 120;
            memcpy(c + 59 * i, &p, 59);
        }
        c[59 * PROFILE_MAX + MODE_SLOT_COUNT] = 2;
    }
    const uint8_t tail[4] = {1, 1, 1, 0};
    size_t tail_at = at + CTRL_TYPE_COUNT * CTRL_PROFILES_V16_SIZE;
    memcpy(flash_image + tail_at, tail, sizeof tail);
    flash_size = tail_at + sizeof tail + 4;
    checksum();
}
int main(void) {
    for (uint8_t rev = 14; rev <= 16; rev++) {
        legacy_image(rev); settings_init();
        assert(g_settings.ext_rev == SETTINGS_EXT_REV && g_settings.size == sizeof(settings_t));
        assert(g_settings.joycon_single && g_settings.decky_options && g_settings.headset_low_latency);
        assert(g_settings.bonded && g_settings.ctrl_addr[5] == 42);
        for (int t = 0; t < CTRL_TYPE_COUNT; t++) {
            assert(g_settings.prof[t].active == 2);
            for (int i = 0; i < PROFILE_MAX; i++) {
                profile_t *p = &g_settings.prof[t].p[i];
                assert(p->stick_deadzone_pct == t * 8 + i);
                assert(p->stick_deadzone_right_pct == p->stick_deadzone_pct);
                assert(p->joycon_gyro_side == JOYCON_GYRO_RIGHT);
                profile_t expected; settings_default_profile_for((ctrl_type_t)t, USB_MODE_SINPUT, &expected);
                assert(!memcmp(p->map, expected.map, IN_COUNT) && p->usb_mode == USB_MODE_SINPUT);
                assert(p->rumble_style == (rev == 16 ? 1 : 0));
                assert(p->impulse_strength == (rev == 16 ? 120 : 100));
            }
        }
    }
    profile_t *p = &g_settings.prof[CTRL_JOYCON_PAIR].p[2];
    p->stick_deadzone_pct = 0; p->stick_deadzone_right_pct = 27; p->joycon_gyro_side = JOYCON_GYRO_LEFT;
    settings_save_now(); memset(&g_settings, 0, sizeof g_settings); settings_init();
    p = &g_settings.prof[CTRL_JOYCON_PAIR].p[2];
    assert(!p->stick_deadzone_pct && p->stick_deadzone_right_pct == 27 && p->joycon_gyro_side == JOYCON_GYRO_LEFT);
    flash_image[flash_size - 1] ^= 1; settings_init(); // corrupt image falls back to defaults
    p = settings_active(&g_settings, CTRL_JOYCON_PAIR);
    assert(p->stick_deadzone_pct == 6 && p->stick_deadzone_right_pct == 6 && !p->joycon_gyro_side);
    puts("Settings storage: v14..16 migration, paired controllers, tail fields, new options and CRC passed");
}
