/*
 * MQTT API - OpenProfalux
 *
 * Topics (le pilotage passe par le modele cover dans shutters.c) :
 *   openprofalux/cover/{slug}/set          [sub]  → "OPEN"|"CLOSE"|"STOP"
 *   openprofalux/cover/{slug}/set_position [sub]  → 0..100 (% d'ouverture)
 *   openprofalux/cover/{slug}/state|position [pub, retained]
 *   openprofalux/listen/set|state          [sub/pub] → switch "Ecoute RF permanente"
 *   openprofalux/frames/last|log/{slot}    [pub, retained] → trames captees
 *   openprofalux/ota/pull                  [sub]  → URL .bin (HTTPS)
 *   openprofalux/{device}/state            [pub]  → heartbeat {"last_cmd","rssi","free_heap"}
 *   openprofalux/{device}/status           [pub, retained] → LWT "online"/"offline"
 *   openprofalux/system/status             [pub, retained] → {"fw","target","free_heap"}
 *
 * HA MQTT Discovery : cover entities + switch/capteur publies par shutters.c
 */
#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

int  mqtt_bridge_start(const char *broker_uri, const char *client_id, const char *user, const char *pass);
void mqtt_bridge_stop(void);

/* Publish */
int  mqtt_pub_state(const char *device_name, const char *json);
int  mqtt_pub_system_status(void);
/* Publish générique (topic libre). qos/retain passés tels quels. */
int  mqtt_pub_raw(const char *topic, const char *payload, int qos, int retain);

/* Callback registered by main - dispatched based on topic */
typedef struct {
    void (*on_message)(const char *topic, const char *data, int len); /* cover HA : openprofalux/cover/# */
    void (*on_ota_pull)(const char *url);                             /* openprofalux/ota/pull */
    void (*on_connected)(void);                                       /* broker connecte : publier la decouverte */
    void (*on_disconnected)(void);                                    /* broker perdu : statut hors ligne */
} mqtt_handlers_t;

int  mqtt_bridge_set_handlers(const mqtt_handlers_t *h);

#endif
