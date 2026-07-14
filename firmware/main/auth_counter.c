#include "auth_counter.h"
#include "nem/proxy_auth.h"
#include "nvs.h"

#define NS  "nem"
#define KEY "actr"          /* u64: smallest counter not yet reserved */
#define GAP 1024ULL

static bool s_init = false;
static unsigned long long s_next;       /* next value to hand out */
static unsigned long long s_block_end;  /* persisted floor; reserve more when reached */

static void persist(unsigned long long floor) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u64(h, KEY, floor);
        nvs_commit(h);
        nvs_close(h);
    }
}

unsigned long long auth_counter_next(void) {
    if (!s_init) {
        unsigned long long stored = 0;
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u64(h, KEY, &stored);   /* leaves 0 if absent */
            nvs_close(h);
        }
        s_next = nem_auth_reserve(stored, GAP, &s_block_end);
        persist(s_block_end);
        s_init = true;
    }
    if (s_next >= s_block_end) {             /* exhausted the reserved block */
        (void)nem_auth_reserve(s_block_end, GAP, &s_block_end);
        persist(s_block_end);
    }
    return s_next++;
}
