/*
 * radio.c — arbitrage RX/TX de l'unique CC1101 (voir radio.h).
 */
#include "radio.h"
#include "cc1101.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "radio";

#define RX_WINDOW_MS 250   /* fenetre d'ecoute : borne la latence max d'un TX a ~250 ms */

static SemaphoreHandle_t s_mtx = NULL;   /* proprietaire exclusif du radio (GDO0 + RMT) */
static radio_frame_cb_t  s_cb  = NULL;
static volatile bool     s_run     = false;
static volatile bool     s_paused  = false;  /* pause temporaire (pendant nos TX stream) */
static volatile bool     s_listen  = false;  /* interrupteur maitre (case UI "capture toutes les trames") */

/* Lecture LSB-first d'un champ de la trame (ordre du fil). */
static uint32_t lsb(const char *b, int from, int len) {
    uint32_t v = 0;
    for (int i = 0; i < len; i++) if (b[from + i] == '1') v |= (1u << i);
    return v;
}

void radio_init(void) {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    /* Coupe le spam "RX(GDO0): MARCSTATE=..." emis a chaque fenetre d'ecoute. */
    esp_log_level_set("cc1101", ESP_LOG_WARN);
    /* Coupe le spam "hw buffer too small" : le bruit OOK sature normalement le buffer
     * RMT de l'ecoute permanente ; c'est attendu et sans consequence (on se re-arme). */
    esp_log_level_set("rmt", ESP_LOG_NONE);
}

void radio_tx(const char *bits, int repeats) {
    if (!bits || !bits[0] || !s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cc1101_tx_raw_bits(bits, (int)strlen(bits), repeats);   /* rafale en UNE session TX */
    xSemaphoreGive(s_mtx);
}

int radio_listen_once(uint32_t timeout_ms, char *buf, int max) {
    if (!s_mtx) return -1;
    bool was = s_paused; s_paused = true;   /* met la tache permanente en pause pendant l'ecoute dediee */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = cc1101_rx_listen_bits(timeout_ms, buf, max);
    xSemaphoreGive(s_mtx);
    s_paused = was;
    return n;
}

void radio_pause_rx(bool pause) { s_paused = pause; }

void radio_set_listening(bool on) {
    s_listen = on;
    ESP_LOGI(TAG, "ecoute permanente %s (case UI 'capture toutes les trames')", on ? "ON" : "OFF");
}

static void rx_task(void *arg) {
    (void)arg;
    char bits[80];
    while (s_run) {
        if (!s_listen || s_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }  /* veille tant que capture OFF ou TX en cours */
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        int n = (!s_listen || s_paused) ? -1 : cc1101_rx_listen_bits(RX_WINDOW_MS, bits, 79);
        int8_t rssi = (n >= 64) ? cc1101_get_rssi() : 0;
        xSemaphoreGive(s_mtx);
        if (n >= 64 && s_cb) {
            bits[n] = 0;
            uint32_t serial = lsb(bits, 32, 28);
            uint32_t hop    = lsb(bits, 0, 32);
            uint8_t  button = (uint8_t)lsb(bits, 60, 4);
            s_cb(bits, serial, button, hop, rssi);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));   /* cede la main entre 2 fenetres */
        }
    }
    vTaskDelete(NULL);
}

void radio_start(radio_frame_cb_t cb) {
    if (!s_mtx) radio_init();
    s_cb = cb; s_run = true;
    xTaskCreate(rx_task, "radio_rx", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "RX permanent demarre (arbitrage RX/TX par mutex, fenetre %d ms)", RX_WINDOW_MS);
}
