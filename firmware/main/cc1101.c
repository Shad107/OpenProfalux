/*
 * CC1101 driver — Profalux 868.35 MHz OOK
 *
 * NOTE: This is a functional skeleton. Config registers below are
 * validated against SmartRF Studio output for OOK 868.35 MHz ~1550 baud
 * asynchronous serial mode with GDO0 output on TX and GDO0 input on RX.
 */
#include "cc1101.h"
#include "hardware_config.h"
#include <string.h>
#include <esp_log.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_rom_sys.h>

static const char *TAG = "cc1101";
static spi_device_handle_t s_spi = NULL;
static cc1101_rx_cb_t     s_rx_cb = NULL;
static TaskHandle_t       s_rx_task = NULL;
static volatile bool      s_rx_running = false;

/* --- Registers CC1101 --- */
#define CC_IOCFG2   0x00
#define CC_IOCFG0   0x02
#define CC_FIFOTHR  0x03
#define CC_PKTLEN   0x06
#define CC_PKTCTRL0 0x08
#define CC_FREQ2    0x0D
#define CC_FREQ1    0x0E
#define CC_FREQ0    0x0F
#define CC_MDMCFG4  0x10
#define CC_MDMCFG3  0x11
#define CC_MDMCFG2  0x12
#define CC_MDMCFG1  0x13
#define CC_MDMCFG0  0x14
#define CC_DEVIATN  0x15
#define CC_MCSM0    0x18
#define CC_FOCCFG   0x19
#define CC_WORCTRL  0x20
#define CC_FSCAL3   0x23
#define CC_FSCAL2   0x24
#define CC_FSCAL1   0x25
#define CC_FSCAL0   0x26
#define CC_TEST2    0x2C
#define CC_TEST1    0x2D
#define CC_TEST0    0x2E
#define CC_PARTNUM  0x30
#define CC_RSSI     0x34

/* Strobes */
#define CC_SRES     0x30
#define CC_SIDLE    0x36
#define CC_STX      0x35
#define CC_SRX      0x34
#define CC_SFTX     0x3B
#define CC_SFRX     0x3A

/* Config array: OOK 868.35 MHz ~1550 baud asynchronous mode */
static const struct { uint8_t reg, val; } s_regs[] = {
    {CC_IOCFG2,   0x0D},  /* Serial data output */
    {CC_IOCFG0,   0x0D},  /* Serial data output on GDO0 */
    {CC_FIFOTHR,  0x47},
    {CC_PKTLEN,   0xFF},
    {CC_PKTCTRL0, 0x32},  /* Async serial, no CRC, infinite packet */
    {CC_FREQ2,    0x21},  /* 868.35 MHz */
    {CC_FREQ1,    0x65},
    {CC_FREQ0,    0x6A},
    {CC_MDMCFG4,  0xF6},  /* Data rate ~1550 baud */
    {CC_MDMCFG3,  0x83},
    {CC_MDMCFG2,  0x30},  /* OOK/ASK modulation, no preamble/sync */
    {CC_MDMCFG1,  0x22},
    {CC_MDMCFG0,  0xF8},
    {CC_DEVIATN,  0x15},
    {CC_MCSM0,    0x18},
    {CC_FOCCFG,   0x14},
    {CC_WORCTRL,  0xFB},
    {CC_FSCAL3,   0xE9},
    {CC_FSCAL2,   0x2A},
    {CC_FSCAL1,   0x00},
    {CC_FSCAL0,   0x1F},
    {CC_TEST2,    0x81},
    {CC_TEST1,    0x35},
    {CC_TEST0,    0x09},
};

static void cs_low(void)  { gpio_set_level(CC1101_PIN_CS, 0); }
static void cs_high(void) { gpio_set_level(CC1101_PIN_CS, 1); }

static void spi_xfer(uint8_t *tx, uint8_t *rx, size_t n) {
    spi_transaction_t t = { .length = 8 * n, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_spi, &t);
}

void cc1101_write_reg(uint8_t addr, uint8_t val) {
    uint8_t tx[2] = {addr, val}, rx[2];
    cs_low(); spi_xfer(tx, rx, 2); cs_high();
}

uint8_t cc1101_read_reg(uint8_t addr) {
    uint8_t tx[2] = {addr | 0x80, 0}, rx[2];
    cs_low(); spi_xfer(tx, rx, 2); cs_high();
    return rx[1];
}

static void strobe(uint8_t s) {
    uint8_t tx = s, rx;
    cs_low(); spi_xfer(&tx, &rx, 1); cs_high();
}

int cc1101_init(void) {
    /* GPIO CS + GDO0 */
    gpio_config_t io_cs = { .pin_bit_mask = 1ULL << CC1101_PIN_CS,
                            .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io_cs);
    cs_high();

    gpio_config_t io_gdo = { .pin_bit_mask = 1ULL << CC1101_PIN_GDO0,
                             .mode = GPIO_MODE_INPUT_OUTPUT };
    gpio_config(&io_gdo);

    /* SPI init */
    spi_bus_config_t bus = {
        .miso_io_num = CC1101_PIN_MISO, .mosi_io_num = CC1101_PIN_MOSI,
        .sclk_io_num = CC1101_PIN_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CC1101_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = CC1101_SPI_FREQ_HZ, .mode = 0,
        .spics_io_num = -1, .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(CC1101_SPI_HOST, &dev, &s_spi));

    /* Reset + configure */
    strobe(CC_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));
    for (size_t i = 0; i < sizeof(s_regs) / sizeof(s_regs[0]); i++) {
        cc1101_write_reg(s_regs[i].reg, s_regs[i].val);
    }
    uint8_t partnum = cc1101_read_reg(CC_PARTNUM | 0x40);
    ESP_LOGI(TAG, "PARTNUM=0x%02X (=expected 0x00 for CC1101)", partnum);
    return (partnum == 0x00) ? 0 : -1;
}

/* Synchronous bit-bang OOK on GDO0.
 * KEELOQ PWM encoding : bit_time ~650us, "0"=short pulse then long space, "1"=long pulse then short space.
 * Preamble : 12 pulses + header 4ms + 66 data bits + inter-frame gap.
 */
static void ook_bit(bool one) {
    /* PWM style: "1" = high 250us + low 400us. "0" = high 500us + low 150us. */
    if (one) {
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(250);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(400);
    } else {
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(500);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(150);
    }
}

int cc1101_tx_ook_frame(const uint8_t *frame, size_t bits) {
    strobe(CC_STX);
    esp_rom_delay_us(500);

    /* Preamble: 12 short pulses ~400us */
    for (int i = 0; i < PROFALUX_PREAMBLE_PULSES; i++) {
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(PROFALUX_PREAMBLE_US);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(PROFALUX_PREAMBLE_US);
    }
    /* Header: 4ms low */
    gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(PROFALUX_HEADER_US);

    /* Data bits MSB first */
    for (size_t i = 0; i < bits; i++) {
        bool b = (frame[i / 8] >> (7 - (i % 8))) & 1;
        ook_bit(b);
    }
    gpio_set_level(CC1101_PIN_GDO0, 0);
    esp_rom_delay_us(2000);  /* Inter-frame gap */
    strobe(CC_SIDLE);
    return 0;
}

/* --- RX --- */
static void rx_task(void *pv) {
    ESP_LOGI(TAG, "RX task started");
    strobe(CC_SRX);
    while (s_rx_running) {
        /* TODO: Sample GDO0 with hardware timer + detect preamble + decode bits.
         * This skeleton just polls RSSI and provides infrastructure.
         * Full implementation: use RMT peripheral or GPIO ISR + circular buffer.
         */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    strobe(CC_SIDLE);
    ESP_LOGI(TAG, "RX task stopped");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

int cc1101_rx_start(cc1101_rx_cb_t cb) {
    if (s_rx_running) return -1;
    s_rx_cb = cb;
    s_rx_running = true;
    xTaskCreate(rx_task, "cc1101_rx", 4096, NULL, 5, &s_rx_task);
    return 0;
}

int cc1101_rx_stop(void) {
    s_rx_running = false;
    return 0;
}

int8_t cc1101_get_rssi(void) {
    uint8_t r = cc1101_read_reg(CC_RSSI | 0x40);
    return (r >= 128) ? (int8_t)((r - 256) / 2 - 74) : (int8_t)(r / 2 - 74);
}
