/*
 * Mock CC1101 - PC simulation
 * Logs frames to stdout instead of transmitting
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void (*cc1101_rx_cb_t)(const uint8_t *frame, size_t bits, int8_t rssi_dbm);

int cc1101_init(void) {
    printf("[MOCK CC1101] Init OK\n");
    return 0;
}

int cc1101_tx_ook_frame(const uint8_t *frame, size_t bits) {
    printf("[MOCK CC1101 TX] bits=%zu | ", bits);
    for (size_t i = 0; i < 9; i++) printf("%02X ", frame[i]);
    printf("\n");
    return 0;
}

int cc1101_rx_start(cc1101_rx_cb_t cb) {
    (void)cb;
    printf("[MOCK CC1101 RX] started (=no real capture in sim)\n");
    return 0;
}

int cc1101_rx_stop(void) {
    printf("[MOCK CC1101 RX] stopped\n");
    return 0;
}

int8_t cc1101_get_rssi(void) { return -55; }
uint8_t cc1101_read_reg(uint8_t addr) { (void)addr; return 0; }
void cc1101_write_reg(uint8_t addr, uint8_t val) { (void)addr; (void)val; }
