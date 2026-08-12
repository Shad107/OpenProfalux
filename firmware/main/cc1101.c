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
#define CC_MARCSTATE 0x35   /* etat machine radio (0x0D=RX, 0x13=TX, 0x01=IDLE) */
#define CC_MCSM1    0x17
#define CC_FREND1   0x21
#define CC_FREND0   0x22    /* PA_POWER : bit0..2 = index PATABLE utilise */
#define CC_PATABLE  0x3E

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
    {CC_FREQ2,    0x21},  /* 868.425 MHz (mesure 2026-08-10, etait 868.35 = hors bande) */
    {CC_FREQ1,    0x66},
    {CC_FREQ0,    0xA5},
    {CC_MDMCFG4,  0xC6},  /* RX BW 102 kHz (mesure) */
    {CC_MDMCFG3,  0x83},
    {CC_MDMCFG2,  0x30},  /* OOK/ASK modulation, no preamble/sync */
    {CC_MDMCFG1,  0x22},
    {CC_MDMCFG0,  0xF8},
    {CC_DEVIATN,  0x15},
    {CC_MCSM1,    0x3C},   /* reste en RX apres reception (comme le sniffer) */
    {CC_MCSM0,    0x18},   /* FS_AUTOCAL: calibre le PLL a IDLE->TX/RX */
    {CC_FREND1,   0xB6},   /* front analogique (ref sniffer qui capte) */
    {CC_FREND0,   0x11},   /* PA_POWER=1 => OOK utilise PATABLE[0] et [1] */
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
    /* PATABLE (burst) pour OOK : index0=eteint (bit 0), index1=puissance (bit 1). */
    {
        uint8_t tx[3] = { CC_PATABLE | 0x40, 0x00, 0xC0 }, rx[3];
        cs_low(); spi_xfer(tx, rx, 3); cs_high();
    }
    uint8_t partnum = cc1101_read_reg(CC_PARTNUM | 0x40);
    uint8_t marc    = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
    ESP_LOGI(TAG, "PARTNUM=0x%02X (=expected 0x00) MARCSTATE=0x%02X (0x01=IDLE)", partnum, marc);
    return (partnum == 0x00) ? 0 : -1;
}

/* Synchronous bit-bang OOK on GDO0. Codage PWM HCS30x mesure le 2026-08-10 :
 *   Te=455us, periode de bit = 3*Te = 1365us.
 *   symbole H455 L910 (H<L)  et  symbole H910 L455 (H>L).
 * POLARITE (quel symbole = logique 1) NON etablie avec une seule telecommande :
 * on applique la convention "bit=1 si H<L" du decode, A CONFIRMER empiriquement
 * (si l'appairage echoue, inverser PFX_TX_INVERT). */
#ifndef PFX_TX_INVERT
#define PFX_TX_INVERT 0
#endif
static void ook_bit(bool one) {
    if (PFX_TX_INVERT) one = !one;
    if (one) {  /* symbole H455 L910 */
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(455);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(910);
    } else {    /* symbole H910 L455 */
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(910);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(455);
    }
}

/* Auto-test d'emission : passe en TX, lit MARCSTATE (0x13 = porteuse ON),
 * module ~300 ms puis revient a IDLE. Prouve que la puce emet, sans recepteur. */
int cc1101_tx_selftest(void) {
    strobe(CC_STX);
    esp_rom_delay_us(1000);
    uint8_t m1 = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
    for (int i = 0; i < 300; i++) {   /* porteuse modulee 1kHz pendant ~300ms */
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(500);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(500);
    }
    uint8_t m2 = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
    strobe(CC_SIDLE);
    ESP_LOGI(TAG, "TX SELFTEST: MARCSTATE post-STX=0x%02X, pendant modulation=0x%02X "
                  "(0x13=TX porteuse ON, 0x01=IDLE=rien)", m1, m2);
    return (m1 == 0x13 || m2 == 0x13) ? 0 : -1;
}

int cc1101_tx_ook_frame(const uint8_t *frame, size_t bits) {
    strobe(CC_STX);
    esp_rom_delay_us(800);   /* laisse le PLL se caler (FS_AUTOCAL) avant de moduler */

    /* Preuve d'emission : lit MARCSTATE une fois par boot. 0x13=TX (porteuse ON). */
    static bool s_marc_logged = false;
    if (!s_marc_logged) {
        s_marc_logged = true;
        uint8_t m = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
        ESP_LOGI(TAG, "TX MARCSTATE=0x%02X (0x13=TX porteuse ON, 0x01=IDLE=rien emis)", m);
    }

    /* Preambule: 23 alternances de Te (mesure) */
    for (int i = 0; i < PROFALUX_PREAMBLE_ELEMENTS; i++) {
        gpio_set_level(CC1101_PIN_GDO0, (i & 1) == 0);  /* H,L,H,L... */
        esp_rom_delay_us(PROFALUX_TE_US);
    }
    /* Header: silence ~4450us low */
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
