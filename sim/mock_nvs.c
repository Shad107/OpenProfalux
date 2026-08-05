/*
 * Mock NVS + random - PC simulation
 * In-memory state, deterministic seed for reproducible tests
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* In-memory NVS blob */
static uint8_t nvs_blob[128];
static int    nvs_stored = 0;
static size_t nvs_size = 0;

int mock_nvs_load(void *out, size_t max_sz, size_t *actual_sz) {
    if (!nvs_stored) return -1;
    size_t sz = nvs_size > max_sz ? max_sz : nvs_size;
    memcpy(out, nvs_blob, sz);
    *actual_sz = sz;
    return 0;
}

int mock_nvs_save(const void *data, size_t sz) {
    if (sz > sizeof(nvs_blob)) return -1;
    memcpy(nvs_blob, data, sz);
    nvs_size = sz;
    nvs_stored = 1;
    printf("[MOCK NVS] Saved %zu bytes\n", sz);
    return 0;
}

int mock_nvs_reset(void) {
    nvs_stored = 0;
    nvs_size = 0;
    return 0;
}

/* Deterministic random for tests. Use SIM_SEED env var to change. */
uint32_t mock_random(void) {
    static uint32_t state = 0;
    if (state == 0) {
        const char *seed = getenv("SIM_SEED");
        state = seed ? (uint32_t)atoi(seed) : 0xDEADBEEF;
    }
    /* xorshift32 */
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
