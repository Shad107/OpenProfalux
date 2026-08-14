/*
 * MQTT API - OpenProfalux
 *
 * Topics :
 *   openprofalux/{device}/pair          [sub]  → trigger pairing burst (=60s)
 *   openprofalux/{device}/reset         [sub]  → regenerate random state (=lose current pairing!)
 *   openprofalux/{device}/cmd           [sub]  → {"btn":"UP"|"STOP"|"DOWN"}
 *   openprofalux/{device}/state         [pub]  → {"serial":"0x...","counter":N,"last_cmd":"UP","rssi":-52}
 *   openprofalux/{device}/pair_result   [pub]  → {"result":"success"|"timeout","frames_sent":60}
 *
 *   openprofalux/listen/start           [sub]  → start RX capture
 *   openprofalux/listen/stop            [sub]  → stop RX capture
 *   openprofalux/listen/frame           [pub]  → {"serial":"0x...","button":N,"enc":"0x...","rssi":-52}
 *
 *   openprofalux/system/status          [pub, retained] → {"fw":"v0.1","uptime":123,"free_heap":123456}
 *
 * HA MQTT Discovery : publie automatiquement les cover entities + button pair
 */
#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

int  mqtt_bridge_start(const char *broker_uri, const char *client_id, const char *user, const char *pass);
void mqtt_bridge_stop(void);

/* Publish */
int  mqtt_pub_state(const char *device_name, const char *json);
int  mqtt_pub_pair_result(const char *device_name, bool success, uint32_t frames_sent);
int  mqtt_pub_rx_frame(const char *json);
int  mqtt_pub_system_status(void);
/* Publish générique (topic libre). qos/retain passés tels quels. */
int  mqtt_pub_raw(const char *topic, const char *payload, int qos, int retain);

/* HA discovery: publish MQTT discovery configs (=cover entity + pair button) */
int  mqtt_ha_publish_discovery(const char *device_name);

/* Callback registered by main - dispatched based on topic */
typedef struct {
    void (*on_pair)(const char *device);
    void (*on_reset)(const char *device);
    void (*on_cmd)(const char *device, const char *button);
    void (*on_listen_start)(void);
    void (*on_listen_stop)(void);
    void (*on_message)(const char *topic, const char *data, int len); /* cover HA : openprofalux/cover/# */
    void (*on_ota_pull)(const char *url);                             /* openprofalux/ota/pull */
} mqtt_handlers_t;

int  mqtt_bridge_set_handlers(const mqtt_handlers_t *h);

#endif
