/*
 * CC1101 driver — TX + RX for Profalux 868.35 MHz OOK
 *
 * Provides:
 *   - Init SPI + register config for OOK 868.35 MHz ~1550 baud
 *   - TX single frame (=synthesize OOK signal via GDO0 bit-banging)
 *   - RX capture — invokes callback for each detected frame
 */
#ifndef CC1101_H
#define CC1101_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*cc1101_rx_cb_t)(const uint8_t *frame, size_t bits, int8_t rssi_dbm);

int  cc1101_init(void);

/* Transmit one frame using OOK modulation (=synthesize preamble + header + data) */
int  cc1101_tx_ook_frame(const uint8_t *frame, size_t bits);

/* Enter async RX mode. Callback invoked from ISR-friendly task */
int  cc1101_rx_start(cc1101_rx_cb_t cb);
int  cc1101_rx_stop(void);

/* Auto-test emission : renvoie 0 si la puce passe bien en TX (MARCSTATE 0x13). */
int  cc1101_tx_selftest(void);

/* Utils */
int8_t cc1101_get_rssi(void);
uint8_t cc1101_read_reg(uint8_t addr);
void   cc1101_write_reg(uint8_t addr, uint8_t val);

#endif
