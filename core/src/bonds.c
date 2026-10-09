// Paired controllers: the list in settings_t (see bond_t). Kept apart from
// settings.c (storage) so the host tests can use it.

#include <string.h>

#include "settings.h"

void settings_bond_sync(settings_t *s) {
    const bond_t *b = &s->bonds[0];
    s->bonded = b->used ? 1 : 0;
    memcpy(s->ctrl_addr, b->used ? b->addr : (const uint8_t[6]){0}, 6);
    s->ctrl_addr_type = b->used ? b->addr_type : 0;
    s->ctrl_pid = b->used ? b->pid : 0;
}

int settings_bond_find(const settings_t *s, const uint8_t addr[6]) {
    for (int i = 0; i < BOND_MAX; i++) {
        if (s->bonds[i].used && memcmp(s->bonds[i].addr, addr, 6) == 0) return i;
    }
    return -1;
}

int settings_bond_count(const settings_t *s) {
    int n = 0;
    for (int i = 0; i < BOND_MAX; i++) n += s->bonds[i].used ? 1 : 0;
    return n;
}

bool settings_bond_remove(settings_t *s, const uint8_t addr[6]) {
    int i = settings_bond_find(s, addr);
    if (i < 0) return false;
    memmove(&s->bonds[i], &s->bonds[i + 1], (size_t)(BOND_MAX - 1 - i) * sizeof s->bonds[0]);
    memset(&s->bonds[BOND_MAX - 1], 0, sizeof s->bonds[0]);
    settings_bond_sync(s);
    return true;
}

bool settings_bond_add(settings_t *s, const uint8_t addr[6], uint8_t addr_type, uint16_t pid, bond_t *dropped) {
    settings_bond_remove(s, addr);
    bool full = s->bonds[BOND_MAX - 1].used;
    if (full && dropped) *dropped = s->bonds[BOND_MAX - 1];
    memmove(&s->bonds[1], &s->bonds[0], (BOND_MAX - 1) * sizeof s->bonds[0]);
    bond_t *b = &s->bonds[0];
    memset(b, 0, sizeof *b);
    memcpy(b->addr, addr, 6);
    b->addr_type = addr_type;
    b->pid = pid;
    b->used = 1;
    settings_bond_sync(s);
    return full;
}

void settings_bond_clear(settings_t *s) {
    memset(s->bonds, 0, sizeof s->bonds);
    settings_bond_sync(s);
}
