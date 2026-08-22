#include "wifi_bridge.h"
#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <time.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* Watchdog WiFi : si la STA reste coupee, relance la pile toute seule (plus besoin de
 * debrancher). Demarre APRES la 1re connexion (pas en mode SoftAP-config). */
static TaskHandle_t s_wdt = NULL;
static void wifi_watchdog_task(void *arg) {
    (void)arg; int down_s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (wifi_bridge_is_connected()) { down_s = 0; continue; }
        down_s += 10;
        if (down_s == 120) {   /* 2 min coupe -> relance propre de la pile (re-declenche STA_START->connect) */
            ESP_LOGW(TAG, "WiFi coupe depuis 2 min -> restart pile WiFi");
            esp_wifi_stop(); vTaskDelay(pdMS_TO_TICKS(500)); esp_wifi_start();
        } else if (down_s >= 300) {   /* 5 min -> reboot en DERNIER recours (recupere tout) */
            ESP_LOGE(TAG, "WiFi coupe depuis 5 min -> reboot de recuperation");
            esp_restart();
        }
    }
}

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, retry");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (!s_wdt) xTaskCreate(wifi_watchdog_task, "wifi_wdt", 3072, NULL, 4, &s_wdt);   /* arme le watchdog a la 1re connexion */
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* Horloge reelle (SNTP) : date correctement les trames RF, meme apres reboot. Une seule init. */
        static bool s_sntp = false;
        if (!s_sntp) {
            s_sntp = true;
            setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();   /* Europe/Paris (heure d'ete auto) */
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
    }
}

int wifi_bridge_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_events = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL, NULL));
    return 0;
}

int wifi_bridge_start_sta(const char *ssid, const char *pass) {
    esp_netif_create_default_wifi_sta();
    wifi_config_t cfg = {0};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* ROOT CAUSE instabilite : le power-save par defaut (WIFI_PS_MIN_MODEM) endort le modem
     * -> l'AP casse l'agregation (block-ack "delba code:39" = timeout) et le lien meurt
     * SILENCIEUSEMENT (pas de STA_DISCONNECTED) -> pas de reconnexion -> MQTT/HTTP morts jusqu'au
     * reboot. On desactive le modem-sleep : lien stable. (Deja fait ponctuellement pendant l'OTA.) */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "STA started, SSID=%s (power-save OFF)", ssid);
    return 0;
}

int wifi_bridge_start_softap(const char *ssid, const char *pass) {
    esp_netif_create_default_wifi_ap();
    wifi_config_t cfg = {0};
    strncpy((char*)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    strncpy((char*)cfg.ap.password, pass, sizeof(cfg.ap.password));
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = strlen(pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP started, SSID=%s", ssid);
    return 0;
}

bool wifi_bridge_is_connected(void) {
    EventBits_t bits = xEventGroupGetBits(s_wifi_events);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

int wifi_bridge_rssi(void) {
    wifi_ap_record_t ap; return esp_wifi_sta_get_ap_info(&ap) == 0 ? ap.rssi : 0;
}

void wifi_bridge_get_ip(char *buf, int len) {
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == 0) {
        snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    } else strncpy(buf, "0.0.0.0", len);
}
