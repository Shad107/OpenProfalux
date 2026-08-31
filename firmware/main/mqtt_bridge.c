#include "mqtt_bridge.h"
#include "hardware_config.h"   /* TARGET_NAME, utilise ligne 130 */
#include <string.h>
#include <stdio.h>
#include <mqtt_client.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cJSON.h>

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_mqtt = NULL;
static mqtt_handlers_t          s_hdl = {0};
static char s_client_id[32];
static char s_avail_topic[64];   /* topic de disponibilite HA (LWT) : openprofalux/<device>/status */

/* Topic base — set at start */
#define TOPIC_BASE "openprofalux"

static void publish_log(const char *level, const char *msg) {
    if (!s_mqtt) return;
    char topic[64], payload[512];
    snprintf(topic, sizeof(topic), TOPIC_BASE "/log");
    snprintf(payload, sizeof(payload),
             "{\"lvl\":\"%s\",\"msg\":\"%s\",\"client\":\"%s\"}",
             level, msg, s_client_id);
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 0, 0);
}

/* Route incoming message to handlers based on topic */
static void handle_incoming(const char *topic, int tlen, const char *data, int dlen) {
    char t[128]; int n = tlen < 127 ? tlen : 127;
    memcpy(t, topic, n); t[n] = 0;
    ESP_LOGI(TAG, "RX topic=%s len=%d", t, dlen);
    publish_log("info", t);

    /* Cover HA + switch "Ecoute RF permanente" + repeuplement frames/log -> handler shutters */
    if (strncmp(t, TOPIC_BASE "/cover/", strlen(TOPIC_BASE "/cover/")) == 0
        || strncmp(t, TOPIC_BASE "/frames/log/", strlen(TOPIC_BASE "/frames/log/")) == 0
        || strncmp(t, TOPIC_BASE "/listen/", strlen(TOPIC_BASE "/listen/")) == 0   /* set + state (restauration au boot) */
        || strncmp(t, TOPIC_BASE "/update/", strlen(TOPIC_BASE "/update/")) == 0) {  /* install MAJ via HA */
        if (s_hdl.on_message) s_hdl.on_message(t, data, dlen);
        return;
    }
    /* OTA pull : payload = URL du .bin */
    if (strcmp(t, TOPIC_BASE "/ota/pull") == 0 && s_hdl.on_ota_pull) {
        char url[256]; int un = dlen < 255 ? dlen : 255;
        memcpy(url, data, un); url[un] = 0;
        s_hdl.on_ota_pull(url);
        return;
    }
}

static void mqtt_event_cb(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t evt = event_data;
    switch (evt->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to broker");
            esp_mqtt_client_publish(s_mqtt, s_avail_topic, "online", 0, 1, 1);   /* disponibilite HA */
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_BASE "/listen/#", 1);
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_BASE "/cover/+/set", 1);
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_BASE "/cover/+/set_position", 1);
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_BASE "/ota/pull", 1);
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_BASE "/update/install", 1);   /* entite update HA */
            /* PAS d'abonnement a frames/log/# : le ring est deja persiste en SPIFFS (load_ring au boot).
             * S'y abonner = l'ESP s'auto-inonde de ses propres trames retained (jeu qui grossit sans fin)
             * au boot -> la tache MQTT traite ce flot en prenant le LOCK pendant que la discovery publie
             * sous le LOCK -> DEADLOCK au demarrage. Le repeuplement MQTT etait redondant avec SPIFFS. */
            mqtt_pub_system_status();
            publish_log("info", "MQTT connected");
            if (s_hdl.on_connected) s_hdl.on_connected();   /* -> publie la decouverte HA (vraie connexion) */
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            if (s_hdl.on_disconnected) s_hdl.on_disconnected();
            break;
        case MQTT_EVENT_DATA:
            handle_incoming(evt->topic, evt->topic_len, evt->data, evt->data_len);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
        default: break;
    }
}

int mqtt_bridge_start(const char *broker_uri, const char *client_id, const char *user, const char *pass) {
    strncpy(s_client_id, client_id, sizeof(s_client_id) - 1);
    snprintf(s_avail_topic, sizeof(s_avail_topic), TOPIC_BASE "/%s/status", client_id);
    /* Le client MQTT exige une URI avec schema (mqtt://host[:port]). Si l'utilisateur a saisi
     * une IP nue, on prefixe mqtt:// automatiquement (sinon "Error parse uri" -> jamais connecte). */
    static char s_uri[128];
    if (broker_uri && *broker_uri && !strstr(broker_uri, "://"))
        snprintf(s_uri, sizeof(s_uri), "mqtt://%s", broker_uri);
    else
        snprintf(s_uri, sizeof(s_uri), "%s", broker_uri ? broker_uri : "");
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.client_id = client_id,
        .credentials.username = user,
        .credentials.authentication.password = pass,
        /* Last Will : le broker publie "offline" (retain) si le boitier tombe sans se
         * deconnecter proprement -> HA grise les entites. On republie "online" a la connexion. */
        .session.last_will.topic = s_avail_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_cb, NULL);
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "MQTT started, broker=%s", broker_uri);
    return 0;
}

void mqtt_bridge_stop(void) {
    if (s_mqtt) { esp_mqtt_client_stop(s_mqtt); esp_mqtt_client_destroy(s_mqtt); s_mqtt = NULL; }
}

int mqtt_bridge_set_handlers(const mqtt_handlers_t *h) { s_hdl = *h; return 0; }

int mqtt_pub_state(const char *device, const char *json) {
    if (!s_mqtt) return -1;
    char topic[64]; snprintf(topic, sizeof(topic), TOPIC_BASE "/%s/state", device);
    return esp_mqtt_client_publish(s_mqtt, topic, json, 0, 1, 1);
}

int mqtt_pub_raw(const char *topic, const char *payload, int qos, int retain) {
    if (!s_mqtt) return -1;
    return esp_mqtt_client_publish(s_mqtt, topic, payload, 0, qos, retain);
}

int mqtt_pub_system_status(void) {
    if (!s_mqtt) return -1;
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"fw\":\"%s\",\"target\":\"" TARGET_NAME "\",\"free_heap\":%u}",
             esp_app_get_description()->version, (unsigned)esp_get_free_heap_size());
    return esp_mqtt_client_publish(s_mqtt, TOPIC_BASE "/system/status", payload, 0, 1, 1);
}

