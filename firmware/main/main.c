/*
 * OpenProfalux main task orchestration
 * Boot flow:
 *   1. Init NVS
 *   2. Init CC1101
 *   3. Init Wi-Fi (STA from NVS config, else SoftAP)
 *   4. Init MQTT (broker from NVS config)
 *   5. Publish HA discovery
 *   6. Listen for MQTT commands
 */
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>

#include "hardware_config.h"
#include "profalux.h"
#include "cc1101.h"
#include "driver/gpio.h"
#include "wifi_bridge.h"
#include "mqtt_bridge.h"

static const char *TAG = "main";

/* Device name / config from NVS */
static char s_device_name[32] = "volet_test";
static char s_wifi_ssid[32]   = "";
static char s_wifi_pass[64]   = "";
static char s_mqtt_uri[128]   = "mqtt://ha.local:1883";
static char s_mqtt_user[32]   = "";
static char s_mqtt_pass[64]   = "";

static pfx_tx_state_t g_state;

/* Load user config from NVS "cfg" namespace */
static void load_config(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz;
    sz = sizeof(s_device_name); nvs_get_str(h, "device", s_device_name, &sz);
    sz = sizeof(s_wifi_ssid);   nvs_get_str(h, "wifi_ssid", s_wifi_ssid, &sz);
    sz = sizeof(s_wifi_pass);   nvs_get_str(h, "wifi_pass", s_wifi_pass, &sz);
    sz = sizeof(s_mqtt_uri);    nvs_get_str(h, "mqtt_uri", s_mqtt_uri, &sz);
    sz = sizeof(s_mqtt_user);   nvs_get_str(h, "mqtt_user", s_mqtt_user, &sz);
    sz = sizeof(s_mqtt_pass);   nvs_get_str(h, "mqtt_pass", s_mqtt_pass, &sz);
    nvs_close(h);
}

/* Publish state JSON */
static void publish_state(const char *last_cmd) {
    char json[256];
    snprintf(json, sizeof(json),
        "{\"serial\":\"0x%08X\",\"counter\":%u,\"last_cmd\":\"%s\","
         "\"rssi\":%d,\"free_heap\":%u}",
        (unsigned)g_state.serial, (unsigned)g_state.counter, last_cmd,
        wifi_bridge_rssi(), (unsigned)esp_get_free_heap_size());
    mqtt_pub_state(s_device_name, json);
    ESP_LOGI(TAG, "State published: counter=%u last=%s", (unsigned)g_state.counter, last_cmd);
}

/* ────── MQTT handlers ────── */

static void on_pair(const char *device) {
    (void)device;
    /* NATIF Profalux, notice platine radio etape 4.1 : le nouvel emetteur fait
     * Stop+P = bouton 0x8 (trame que la VRAIE telecommande emet pour enroler,
     * prouvee par la capture du 10/08). Emis 5 s, compteur INCREMENTE (anti-replay). */
    ESP_LOGI(TAG, "▶ ENROLEMENT NATIF 4.1 : Stop+P = bouton 0x8, maintenu 5 s.");
    ESP_LOGI(TAG, "  PUIS (vraie telecommande) : montee+Stop, descente+Stop, montee+Stop (SANS butees).");
    pfx_emit_hold(&g_state, PFX_BTN_PROG, 5000);
    mqtt_pub_pair_result(s_device_name, true, 1);
    publish_state("PAIR");
}

static void on_reset(const char *device) {
    (void)device;
    ESP_LOGW(TAG, "⚠ RESET: generating new random state. LOSES PAIRING!");
    pfx_state_reset(&g_state);
    publish_state("RESET");
}

static void on_cmd(const char *device, const char *btn) {
    (void)device;
    uint8_t b = 0;
    if      (strcmp(btn, "UP")   == 0) b = PFX_BTN_UP;
    else if (strcmp(btn, "STOP") == 0) b = PFX_BTN_STOP;
    else if (strcmp(btn, "DOWN") == 0) b = PFX_BTN_DOWN;
    else { ESP_LOGW(TAG, "Unknown button: %s", btn); return; }
    ESP_LOGI(TAG, "▶ CMD %s", btn);
    pfx_emit_command(&g_state, b);
    publish_state(btn);
}

static void rx_frame_cb(const uint8_t *frame, size_t bits, int8_t rssi) {
    (void)bits;
    pfx_rx_frame_t rxf; pfx_frame_parse(frame, &rxf);
    char json[256];
    snprintf(json, sizeof(json),
        "{\"serial\":\"0x%08X\",\"button\":%u,\"encrypted_hop\":\"0x%08X\","
         "\"status\":%u,\"rssi\":%d}",
        (unsigned)rxf.serial, rxf.button, (unsigned)rxf.encrypted_hop,
        rxf.status_flags, rssi);
    mqtt_pub_rx_frame(json);
    ESP_LOGI(TAG, "◀ RX serial=0x%08X btn=%u rssi=%d",
             (unsigned)rxf.serial, rxf.button, rssi);
}

static void on_listen_start(void) {
    ESP_LOGI(TAG, "◀ LISTEN start");
    cc1101_rx_start(rx_frame_cb);
}

static void on_listen_stop(void) {
    ESP_LOGI(TAG, "◀ LISTEN stop");
    cc1101_rx_stop();
}

/* ────── App entry ────── */

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);   /* coupe le flot verbeux SPI/RMT: log lisible */
    ESP_LOGI(TAG, "╔══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║ OpenProfalux firmware — target=" TARGET_NAME);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════╝");

    /* 1. NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    /* 2. Config */
    load_config();
    ESP_LOGI(TAG, "device=%s wifi_ssid=%s mqtt=%s",
             s_device_name, s_wifi_ssid, s_mqtt_uri);

    /* 3. Profalux state */
    pfx_state_init(&g_state);

    /* 4. CC1101 */
    if (cc1101_init() != 0) {
        ESP_LOGE(TAG, "CC1101 init FAILED. Check wiring per hardware_config.h");
    }
    /* 4b. Auto-test emission au boot (prouve que la puce passe en TX). */
    if (cc1101_tx_selftest() == 0)
        ESP_LOGI(TAG, "TX SELFTEST OK : la puce emet (MARCSTATE=TX).");
    else
        ESP_LOGW(TAG, "TX SELFTEST ECHEC : puce NE passe PAS en TX (config/SPI).");
    /* 4c. Auto-verif trame : re-capture GDO0 de notre propre emission (slot 54, 0x8). */
    pfx_selfverify(&g_state, PFX_BTN_PROG);

    /* 5. Wi-Fi */
    wifi_bridge_init();
    if (strlen(s_wifi_ssid) > 0) {
        wifi_bridge_start_sta(s_wifi_ssid, s_wifi_pass);
        /* Wait for connection or timeout */
        int retry = 0;
        while (!wifi_bridge_is_connected() && retry++ < 60) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!wifi_bridge_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi not connected. Starting SoftAP for config.");
        wifi_bridge_start_softap("OpenProfalux-Setup", "openprofalux");
    }

    /* 6. MQTT */
    if (wifi_bridge_is_connected() && strlen(s_mqtt_uri) > 0) {
        mqtt_bridge_start(s_mqtt_uri, s_device_name,
                          strlen(s_mqtt_user) ? s_mqtt_user : NULL,
                          strlen(s_mqtt_pass) ? s_mqtt_pass : NULL);
        mqtt_handlers_t h = {
            .on_pair = on_pair, .on_reset = on_reset, .on_cmd = on_cmd,
            .on_listen_start = on_listen_start, .on_listen_stop = on_listen_stop,
        };
        mqtt_bridge_set_handlers(&h);
        vTaskDelay(pdMS_TO_TICKS(2000));  /* let MQTT connect */
        mqtt_ha_publish_discovery(s_device_name);
        publish_state("BOOT");
    }

    /* Trigger LOCAL pour le test d'enrolement (pas besoin de MQTT/WiFi) :
     * appui sur le bouton integre de l'ATOM Lite (GPIO39) -> burst d'appairage. */
    gpio_config_t btn = { .pin_bit_mask = 1ULL << 39, .mode = GPIO_MODE_INPUT };
    gpio_config(&btn);
    ESP_LOGI(TAG, "PRET. Bouton ATOM (G39) = burst appairage. Identite serial=0x%08X counter=%u.",
             (unsigned)g_state.serial, (unsigned)g_state.counter);

    /* Bouton ATOM (G39) :
     *   - appui LONG (>1,5 s) = ENROLEMENT (burst bouton PROG 0x8)
     *   - appui court        = PILOTAGE, cycle MONTEE -> STOP -> DESCENTE
     * Une seule image pour enroler puis piloter sans reflasher. */
    /* MODE CAPTURE + REJEU (famille RollJam) :
     *   - appui court = CAPTURE une vraie trame (RX ~6s), la stocke + logge
     *   - appui LONG  = REJOUE la trame captee (TX x10)
     * Rappel : le rejeu ne marche que si le moteur n'a PAS entendu la trame (rolling). */
    char cap[80]; int cap_n = 0;
    int prev = 1; uint32_t hb = 0;
    while (1) {
        int now = gpio_get_level(39);
        if (prev == 1 && now == 0) {          /* front descendant = debut appui */
            uint32_t held = 0;
            while (gpio_get_level(39) == 0) { vTaskDelay(pdMS_TO_TICKS(20)); held += 20; }
            if (held >= 1500) {
                if (cap_n >= 64) {
                    ESP_LOGI(TAG, ">>> Appui LONG : REJEU trame captee (%d bits) x10 <<<", cap_n);
                    for (int k = 0; k < 10; k++) { cc1101_tx_raw_bits(cap, cap_n); vTaskDelay(pdMS_TO_TICKS(30)); }
                    ESP_LOGI(TAG, "  rejeu fini. Le volet a bouge ?");
                } else {
                    ESP_LOGW(TAG, ">>> Appui LONG : rien a rejouer (fais un appui court pour capturer d'abord) <<<");
                }
            } else {
                ESP_LOGI(TAG, ">>> Appui court : CAPTURE - presse ta 0x813 dans les 6 s... <<<");
                cap_n = cc1101_rx_listen_bits(6000, cap, (int)sizeof(cap) - 1);
                if (cap_n >= 64) {
                    uint32_t hop = 0;    for (int i = 0;  i < 32; i++) hop    = (hop << 1)    | (cap[i]-'0');
                    uint32_t serial = 0; for (int i = 59; i >= 32; i--) serial = (serial << 1) | (cap[i]-'0');
                    int btn = 0;         for (int i = 60; i < 64; i++) btn    = (btn << 1)    | (cap[i]-'0');
                    int rpt = (cap_n >= 66) ? cap[65]-'0' : -1;
                    ESP_LOGI(TAG, "  CAPTUREE: serial=0x%05X fam=0x%03X btn=0x%X RPT=%d hop=0x%08X (%d bits) -> appui LONG pour rejouer",
                             (unsigned)serial, (unsigned)(serial & 0x3FFu), btn, rpt, (unsigned)hop, cap_n);
                } else {
                    ESP_LOGW(TAG, "  rien capte (nb=%d). Approche la telecommande du module et reessaie.", cap_n);
                }
            }
            now = 1;                            /* relache */
        }
        prev = now;
        if (++hb >= 1200) { hb = 0; publish_state("HEARTBEAT"); }  /* ~60s */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
