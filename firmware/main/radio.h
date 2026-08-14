/*
 * radio.h — arbitrage RX/TX sur l'unique CC1101 (GDO0 + canal RMT partages).
 *
 * Le driver cc1101 ne fait PAS de RX asynchrone (cc1101_rx_start est un squelette) :
 * le seul RX reel est cc1101_rx_listen_bits() (bloquant). RX et TX se disputent GDO0
 * et le canal RMT unique. Ce module serialise tout via un mutex + une tache RX unique :
 *   - une tache ecoute en fenetres courtes et dispatche chaque trame recue au callback,
 *   - radio_tx() prend le mutex et rejoue une trame (replay UI/MQTT/HA),
 *   - radio_pause_rx() suspend l'ecoute pendant qu'on STREAM notre propre commande
 *     (maintien), pour que le TX ne soit jamais bloque par une fenetre RX en cours,
 *   - radio_listen_once() fait une ecoute dediee (apprentissage) sans collision.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Callback appele (hors mutex) pour chaque trame recue. bits = chaine '0'/'1' ordre du fil. */
typedef void (*radio_frame_cb_t)(const char *bits, uint32_t serial, uint8_t button,
                                 uint32_t hop, int8_t rssi);

void radio_init(void);                 /* cree le mutex (idempotent) */
void radio_start(radio_frame_cb_t cb); /* demarre la tache RX (en veille tant que listening OFF) */
void radio_tx(const char *bits);       /* rejoue une trame (mutex-garde) — marche listening ON ou OFF */
int  radio_listen_once(uint32_t timeout_ms, char *buf, int max); /* ecoute dediee (apprentissage) — marche tjrs */
void radio_pause_rx(bool pause);       /* pause TEMPORAIRE pendant nos TX (stream maintien) */
void radio_set_listening(bool on);     /* interrupteur MAITRE : l'ecoute permanente (case UI "capture") */
