/*
 * shutters.c — modele cover OpenProfalux (clone/replay + position time-based).
 * Config persistee en NVS sous forme de blob JSON (cle "cfg" du namespace "shutters").
 */
#include "shutters.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "cc1101.h"
#include "radio.h"
#include "hardware_config.h"
#include "mqtt_bridge.h"
#include "esp_wifi.h"
#include "esp_netif.h"

static const char *TAG = "shutters";
#define TICK_MS 150
#define SH_MAX_HOPS 1024   /* set de hops distincts memorises par serial (4 Ko/serial max) */

typedef struct {
    char id[SH_ID_LEN];
    char serials[SH_MAX_SERIALS][SH_SERIAL_LEN];
    int  n_serials;
    char up[SH_BITS_LEN], down[SH_BITS_LEN], stop[SH_BITS_LEN];
    uint8_t up_btn, down_btn, stop_btn;   /* code bouton appris (pour la sync RX) */
    uint32_t travel_up_ms, travel_down_ms;
    float position;          /* 0..100 */
    int   dir;               /* -1 down, 0 stop, +1 up */
    int   target;            /* -1 = aucun, sinon 0..100 */
    bool  own_move;          /* true = notre commande (on stream) ; false = externe (on suit sans emettre) */
    int64_t last_tick_us;
    int   pub_pos;           /* derniere position publiee MQTT (evite le spam) */
    int   pub_dir;           /* derniere direction publiee MQTT */
} volet_t;

typedef struct {
    char serial[SH_SERIAL_LEN]; char name[SH_ID_LEN];
    uint32_t  last_hop;      /* fast-path : dernier hop (evite le scan sur les maintiens) */
    uint32_t *hops;          /* SET des hopping codes deja enregistres (dedup complet) */
    uint16_t  nhops;         /* = nb de trames DISTINCTES loggees */
    uint16_t  caphops;       /* capacite allouee du set */
} remote_t;

typedef struct { uint32_t t; char serial[SH_SERIAL_LEN]; uint8_t button; uint32_t hop; int8_t rssi; } rfrec_t;

static volet_t  s_volets[SH_MAX_VOLETS];
static int      s_nvolets = 0;
static remote_t s_remotes[16];
static int      s_nremotes = 0;
static rfrec_t  s_rf[12];
static int      s_rfhead = 0;
static SemaphoreHandle_t s_lock;
static char     s_device[32] = "op";   /* nom appareil (prefixe topics HA) */
static bool     s_mqtt_ready = false;   /* true apres shutters_mqtt_announce() */
static bool     s_log_frames = false;   /* publie toutes les trames captees en MQTT */

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void update_listening(void);   /* (defini plus bas) : allume le RX si volet appris OU option capture */

static volet_t *find_volet(const char *id) {
    for (int i = 0; i < s_nvolets; i++) if (!strcmp(s_volets[i].id, id)) return &s_volets[i];
    return NULL;
}
static volet_t *get_or_create(const char *id) {
    volet_t *v = find_volet(id);
    if (v) return v;
    if (s_nvolets >= SH_MAX_VOLETS) return NULL;
    v = &s_volets[s_nvolets++];
    memset(v, 0, sizeof(*v));
    strlcpy(v->id, id, SH_ID_LEN);
    v->position = 50; v->target = -1; v->pub_pos = -1; v->pub_dir = -9;
    return v;
}

/* ── Persistance JSON <-> NVS ── */
/* Serialise toute la config (telecommandes + noms + trames de reference + calibration). */
static char *cfg_to_json(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *rem = cJSON_AddObjectToObject(root, "remotes");
    for (int i = 0; i < s_nremotes; i++) cJSON_AddStringToObject(rem, s_remotes[i].serial, s_remotes[i].name);
    cJSON *vols = cJSON_AddArrayToObject(root, "volets");
    for (int i = 0; i < s_nvolets; i++) {
        volet_t *v = &s_volets[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", v->id);
        cJSON *sr = cJSON_AddArrayToObject(o, "serials");
        for (int j = 0; j < v->n_serials; j++) cJSON_AddItemToArray(sr, cJSON_CreateString(v->serials[j]));
        cJSON *cmd = cJSON_AddObjectToObject(o, "cmd");
        cJSON_AddStringToObject(cmd, "up", v->up);
        cJSON_AddStringToObject(cmd, "down", v->down);
        cJSON_AddStringToObject(cmd, "stop", v->stop);
        cJSON_AddNumberToObject(o, "up_btn", v->up_btn);
        cJSON_AddNumberToObject(o, "down_btn", v->down_btn);
        cJSON_AddNumberToObject(o, "stop_btn", v->stop_btn);
        cJSON_AddNumberToObject(o, "travel_up_ms", v->travel_up_ms);
        cJSON_AddNumberToObject(o, "travel_down_ms", v->travel_down_ms);
        cJSON_AddNumberToObject(o, "position", (int)(v->position + 0.5f));
        cJSON_AddItemToArray(vols, o);
    }
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}
static void save_cfg(void) {
    char *js = cfg_to_json();
    if (js) {
        nvs_handle_t h;
        if (nvs_open("shutters", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "cfg", js); nvs_commit(h); nvs_close(h);
        }
        free(js);
    }
}
/* Remet la config a zero (libere les sets de hops avant reload/import). */
static void reset_state(void) {
    for (int i = 0; i < s_nremotes; i++) { free(s_remotes[i].hops); s_remotes[i].hops = NULL; s_remotes[i].nhops = s_remotes[i].caphops = 0; }
    s_nremotes = 0; s_nvolets = 0;
}
/* Peuple s_remotes/s_volets depuis un JSON (l'etat doit etre remis a zero avant). */
static void parse_cfg_json(cJSON *root) {
    cJSON *rem = cJSON_GetObjectItem(root, "remotes");
    for (cJSON *it = rem ? rem->child : NULL; it && s_nremotes < 16; it = it->next) {
        strlcpy(s_remotes[s_nremotes].serial, it->string, SH_SERIAL_LEN);
        strlcpy(s_remotes[s_nremotes].name, cJSON_IsString(it) ? it->valuestring : "", SH_ID_LEN);
        s_remotes[s_nremotes].hops = NULL; s_remotes[s_nremotes].nhops = 0; s_remotes[s_nremotes].caphops = 0;
        s_nremotes++;
    }
    cJSON *vols = cJSON_GetObjectItem(root, "volets");
    for (cJSON *o = vols ? vols->child : NULL; o && s_nvolets < SH_MAX_VOLETS; o = o->next) {
        volet_t *v = &s_volets[s_nvolets++];
        memset(v, 0, sizeof(*v)); v->target = -1; v->pub_pos = -1; v->pub_dir = -9;
        strlcpy(v->id, cJSON_GetStringValue(cJSON_GetObjectItem(o, "id")) ?: "", SH_ID_LEN);
        cJSON *sr = cJSON_GetObjectItem(o, "serials");
        for (cJSON *s = sr ? sr->child : NULL; s && v->n_serials < SH_MAX_SERIALS; s = s->next)
            strlcpy(v->serials[v->n_serials++], cJSON_GetStringValue(s) ?: "", SH_SERIAL_LEN);
        cJSON *cmd = cJSON_GetObjectItem(o, "cmd");
        if (cmd) {
            strlcpy(v->up,   cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "up"))   ?: "", SH_BITS_LEN);
            strlcpy(v->down, cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "down")) ?: "", SH_BITS_LEN);
            strlcpy(v->stop, cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "stop")) ?: "", SH_BITS_LEN);
        }
        v->up_btn   = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "up_btn"));
        v->down_btn = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "down_btn"));
        v->stop_btn = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "stop_btn"));
        v->travel_up_ms   = cJSON_GetNumberValue(cJSON_GetObjectItem(o, "travel_up_ms"));
        v->travel_down_ms = cJSON_GetNumberValue(cJSON_GetObjectItem(o, "travel_down_ms"));
        v->position       = cJSON_GetNumberValue(cJSON_GetObjectItem(o, "position"));
    }
}
static void load_cfg(void) {
    nvs_handle_t h;
    if (nvs_open("shutters", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = 0;
    if (nvs_get_str(h, "cfg", NULL, &sz) != ESP_OK || sz == 0) { nvs_close(h); return; }
    char *js = malloc(sz);
    if (!js) { nvs_close(h); return; }
    nvs_get_str(h, "cfg", js, &sz); nvs_close(h);
    cJSON *root = cJSON_Parse(js); free(js);
    if (!root) return;
    parse_cfg_json(root);
    cJSON_Delete(root);
}

/* ── RF ── */
#define PRESS_REPEATS 4   /* nb de trames par "appui" (comme la rafale d'une vraie telecommande) */
/* Emet 1 trame via l'arbitre radio (serialise avec l'ecoute permanente). */
static void emit(const char *bits) {
    if (bits && bits[0]) radio_tx(bits);
}
/* Emet une RAFALE = 1 appui de telecommande (le moteur part ensuite tout seul jusqu'au STOP/butee). */
static void emit_press(const char *bits) {
    for (int i = 0; i < PRESS_REPEATS; i++) emit(bits);
}
/* bouton (4 bits LSB) d'une trame captee (bits[60..63]). */
static uint8_t bits_button(const char *b) {
    uint8_t v = 0;
    if ((int)strlen(b) >= 64) for (int i = 0; i < 4; i++) if (b[60 + i] == '1') v |= (1u << i);
    return v;
}

/* Demarre un mouvement (dir=+1 up / -1 down) : UNE rafale (appui), puis le moteur part seul.
 * On integre juste la position ensuite ; l'arret se fait par do_stop (rafale STOP) au bon moment. */
static void start_move(volet_t *v, int dir) {
    v->dir = dir; v->own_move = true;
    v->last_tick_us = esp_timer_get_time();
    radio_pause_rx(true);   /* le temps de nos rafales, l'ecoute permanente ne doit pas bloquer le TX */
    emit_press(dir > 0 ? v->up : v->down);
}
static void do_stop(volet_t *v) {
    v->dir = 0; v->target = -1;
    emit_press(v->stop);    /* rafale STOP = fige le moteur a la position voulue */
    radio_pause_rx(false);  /* fin de notre commande : on reprend l'ecoute (sync + frame-log) */
}
/* Sync depuis une commande EXTERNE (vraie telecommande) : suit sans emettre.
 * Le moteur part vers la butee (target) ; l'arret intermediaire vient du STOP (freeze). */
static void track_move(volet_t *v, int dir) {
    v->dir = dir; v->own_move = false; v->target = (dir > 0) ? 100 : 0;
    v->last_tick_us = esp_timer_get_time();
}
static void freeze(volet_t *v) { v->dir = 0; v->target = -1; }

/* ── Home Assistant : discovery + etat (MQTT) ── */
static void ha_state_str(const volet_t *v, char *out) {
    if      (v->dir > 0)          strcpy(out, "opening");
    else if (v->dir < 0)          strcpy(out, "closing");
    else if (v->position >= 99)   strcpy(out, "open");
    else if (v->position <= 1)    strcpy(out, "closed");
    else                          strcpy(out, "stopped");
}
/* Publie position + etat du volet (retained). Appele sous LOCK. */
static void publish_volet_state(volet_t *v) {
    if (!s_mqtt_ready) return;
    char topic[96], pl[16], st[10];
    snprintf(topic, sizeof(topic), "openprofalux/cover/%s/position", v->id);
    snprintf(pl, sizeof(pl), "%d", (int)(v->position + 0.5f));
    mqtt_pub_raw(topic, pl, 0, 1);
    ha_state_str(v, st);
    snprintf(topic, sizeof(topic), "openprofalux/cover/%s/state", v->id);
    mqtt_pub_raw(topic, st, 0, 1);
    v->pub_pos = (int)(v->position + 0.5f); v->pub_dir = v->dir;
}
/* Publie la config HA discovery d'un cover (retained). Appele sous LOCK. */
static void announce_one(volet_t *v) {
    if (!s_mqtt_ready) return;
    char topic[128], *pl = malloc(768);
    if (!pl) return;
    snprintf(topic, sizeof(topic), "homeassistant/cover/openprofalux_%s_%s/config", s_device, v->id);
    snprintf(pl, 768,
        "{\"name\":\"%s\",\"uniq_id\":\"opfx_%s_%s\",\"dev_cla\":\"shutter\","
        "\"cmd_t\":\"openprofalux/cover/%s/set\","
        "\"pl_open\":\"OPEN\",\"pl_cls\":\"CLOSE\",\"pl_stop\":\"STOP\","
        "\"pos_t\":\"openprofalux/cover/%s/position\","
        "\"set_pos_t\":\"openprofalux/cover/%s/set_position\","
        "\"pos_open\":100,\"pos_clsd\":0,"
        "\"stat_t\":\"openprofalux/cover/%s/state\","
        "\"dev\":{\"ids\":[\"openprofalux_%s\"],\"name\":\"OpenProfalux %s\","
        "\"mf\":\"isno.fr\",\"mdl\":\"ESP32+CC1101\","
        "\"cu\":\"https://www.isno.fr/projets/openprofalux\"}}",
        v->id, s_device, v->id, v->id, v->id, v->id, v->id, s_device, s_device);
    mqtt_pub_raw(topic, pl, 1, 1);
    free(pl);
    publish_volet_state(v);
}

/* ── API commande ── */
int shutters_delete_volet(const char *id) {
    if (!id || !*id) return -1;
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_nvolets; i++) if (!strcmp(s_volets[i].id, id)) { idx = i; break; }
    if (idx < 0) { UNLOCK(); return -1; }
    for (int i = idx; i < s_nvolets - 1; i++) s_volets[i] = s_volets[i + 1];
    s_nvolets--;
    save_cfg();
    update_listening();   /* plus aucun volet -> coupe l'ecoute permanente */
    UNLOCK();
    ESP_LOGW(TAG, "volet '%s' supprime", id);
    return 0;
}

int shutters_cmd(const char *id, const char *cmd, int value) {
    LOCK();
    volet_t *v = find_volet(id);
    if (!v) { ESP_LOGW(TAG, "CMD '%s' : volet '%s' introuvable", cmd, id ? id : "(null)"); UNLOCK(); return -1; }
    ESP_LOGW(TAG, "CMD %s '%s' (up=%dc stop=%dc down=%dc)", cmd, id,
             (int)strlen(v->up), (int)strlen(v->stop), (int)strlen(v->down));
    if (!strcmp(cmd, "up"))        { v->target = 100; start_move(v, +1); }
    else if (!strcmp(cmd, "down")) { v->target = 0;   start_move(v, -1); }
    else if (!strcmp(cmd, "stop")) { do_stop(v); }
    else if (!strcmp(cmd, "pos"))  {
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        v->target = value;
        if (value > v->position + 1)      start_move(v, +1);
        else if (value < v->position - 1) start_move(v, -1);
        else v->dir = 0;
    }
    publish_volet_state(v);
    UNLOCK();
    return 0;
}

/* ── Tache : suivi position + streaming du maintien ── */
static void tick_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        LOCK();
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < s_nvolets; i++) {
            volet_t *v = &s_volets[i];
            if (v->dir == 0) continue;
            uint32_t travel = v->dir > 0 ? v->travel_up_ms : v->travel_down_ms;
            if (travel < 500) travel = 18000;   /* defaut si non calibre */
            float dms = (now - v->last_tick_us) / 1000.0f;
            v->last_tick_us = now;
            v->position += v->dir * (dms / travel) * 100.0f;
            if (v->position < 0) v->position = 0;
            if (v->position > 100) v->position = 100;
            /* cible atteinte ou butee -> stop */
            int reached = (v->dir > 0 && (v->position >= 100 || (v->target >= 0 && v->position >= v->target)))
                       || (v->dir < 0 && (v->position <= 0   || (v->target >= 0 && v->position <= v->target)));
            if (reached) { if (v->own_move) do_stop(v); else freeze(v); }
            /* sinon mouvement en cours : le moteur (le notre, lance par une rafale, ou l'externe)
             * tourne tout seul jusqu'au STOP / butee ; on integre juste la position sans re-emettre */
            /* publie l'etat HA quand la position (entiere) ou la direction change */
            if (reached || (int)(v->position + 0.5f) != v->pub_pos || v->dir != v->pub_dir)
                publish_volet_state(v);
        }
        UNLOCK();
    }
}

/* ── Apprentissage / calibration / nommage ── */
int shutters_learn_assign(const char *id, const char *action, const char *bits) {
    if (!id || !*id || !bits) return -1;
    LOCK();
    volet_t *v = get_or_create(id);
    if (!v) { UNLOCK(); return -1; }
    uint8_t btn = bits_button(bits);
    if      (!strcmp(action, "up"))   { strlcpy(v->up, bits, SH_BITS_LEN);   v->up_btn = btn; }
    else if (!strcmp(action, "down")) { strlcpy(v->down, bits, SH_BITS_LEN); v->down_btn = btn; }
    else if (!strcmp(action, "stop")) { strlcpy(v->stop, bits, SH_BITS_LEN); v->stop_btn = btn; }
    else { UNLOCK(); return -1; }
    /* memorise le serial de cette telecommande pour ce volet (sync position) */
    uint32_t ser = 0; if ((int)strlen(bits) >= 60) for (int i = 0; i < 28; i++) if (bits[32 + i] == '1') ser |= (1u << i);
    char shex[SH_SERIAL_LEN]; snprintf(shex, sizeof(shex), "0x%07X", (unsigned)ser);
    bool has = false;
    for (int i = 0; i < v->n_serials; i++) if (!strcmp(v->serials[i], shex)) { has = true; break; }
    if (!has && v->n_serials < SH_MAX_SERIALS) strlcpy(v->serials[v->n_serials++], shex, SH_SERIAL_LEN);
    save_cfg();
    announce_one(v);       /* (re)publie le cover HA des qu'il est appris */
    update_listening();    /* 1er volet appris -> allume l'ecoute pour le suivi de position */
    UNLOCK();
    return 0;
}
int shutters_calibrate(const char *id, uint32_t up_ms, uint32_t down_ms) {
    LOCK();
    volet_t *v = find_volet(id);
    if (!v) { UNLOCK(); return -1; }
    v->travel_up_ms = up_ms; v->travel_down_ms = down_ms;
    save_cfg();
    UNLOCK();
    return 0;
}
int shutters_remote_name(const char *serial, const char *name) {
    LOCK();
    for (int i = 0; i < s_nremotes; i++) if (!strcmp(s_remotes[i].serial, serial)) {
        strlcpy(s_remotes[i].name, name, SH_ID_LEN); save_cfg(); UNLOCK(); return 0;
    }
    if (s_nremotes < 16) {
        strlcpy(s_remotes[s_nremotes].serial, serial, SH_SERIAL_LEN);
        strlcpy(s_remotes[s_nremotes].name, name, SH_ID_LEN);
        s_nremotes++; save_cfg();
    }
    UNLOCK();
    return 0;
}

/* ══ FONCTION 1 — Dataset slide (option MQTT "capture toutes les trames") ══
 * Publie un hop UNIQUEMENT s'il n'est pas deja dans le set enregistre de ce serial.
 * Sert a collecter les 65536 hops distincts pour l'attaque slide-MITM. Appele sous LOCK. */
static void dataset_log_frame(const char *shex, uint8_t button, uint32_t hop, int8_t rssi) {
    remote_t *rm = NULL;
    for (int i = 0; i < s_nremotes; i++) if (!strcmp(s_remotes[i].serial, shex)) { rm = &s_remotes[i]; break; }
    if (!rm) return;
    bool is_new = true;
    if (rm->nhops && rm->last_hop == hop) is_new = false;                      /* fast-path maintien */
    else for (int i = 0; i < rm->nhops; i++) if (rm->hops[i] == hop) { is_new = false; break; }
    rm->last_hop = hop;
    if (!is_new) return;                                                       /* deja enregistre -> rien */
    /* ajoute au set (croissance bornee a SH_MAX_HOPS) */
    if (rm->nhops >= rm->caphops && rm->caphops < SH_MAX_HOPS) {
        uint16_t nc = rm->caphops ? (uint16_t)(rm->caphops * 2) : 32;
        if (nc > SH_MAX_HOPS) nc = SH_MAX_HOPS;
        uint32_t *np = realloc(rm->hops, (size_t)nc * sizeof(uint32_t));
        if (np) { rm->hops = np; rm->caphops = nc; }
    }
    if (rm->nhops < rm->caphops) rm->hops[rm->nhops++] = hop;
    char topic[64], pl[160];
    snprintf(topic, sizeof(topic), "openprofalux/frames/%s", shex);
    snprintf(pl, sizeof(pl), "{\"button\":%u,\"hop\":\"0x%08X\",\"rssi\":%d,\"distinct\":%u}",
             button, (unsigned)hop, rssi, (unsigned)rm->nhops);
    mqtt_pub_raw(topic, pl, 0, 0);
    snprintf(topic, sizeof(topic), "openprofalux/frames/%s/count", shex);
    snprintf(pl, sizeof(pl), "%u", (unsigned)rm->nhops);
    mqtt_pub_raw(topic, pl, 0, 1);   /* le broker/HA accumule le total reel (dataset slide) */
}

/* Rattrapage : rejoue le ring RF recent (12 dernieres trames) vers le dataset MQTT,
 * du plus ancien au plus recent. Appele sous LOCK, no-op si option off ou MQTT absent. */
static void flush_frame_ring_locked(void) {
    if (!s_log_frames || !s_mqtt_ready) return;
    for (int k = 0; k < 12; k++) {
        rfrec_t *r = &s_rf[(s_rfhead + k) % 12];
        if (r->serial[0]) dataset_log_frame(r->serial, r->button, r->hop, r->rssi);
    }
}

/* ══ FONCTION 2 — Suivi de position via telecommandes ENREGISTREES ══
 * Independante de l'option MQTT : des qu'un volet a appris ce serial+bouton, on suit
 * sa position quand la VRAIE telecommande l'actionne. Appele sous LOCK. */
static void track_shutter_position(const char *shex, uint8_t button) {
    for (int i = 0; i < s_nvolets; i++) {
        volet_t *v = &s_volets[i];
        bool mine = false;
        for (int j = 0; j < v->n_serials; j++) if (!strcmp(v->serials[j], shex)) { mine = true; break; }
        if (!mine) continue;
        /* Profalux = appui + stop : ▲/▼ lance le moteur vers la butee, STOP le fige.
         * On demarre le suivi au 1er appui (repetitions du burst ignorees), on fige au STOP. */
        if (v->stop_btn && button == v->stop_btn) {
            freeze(v);
        } else if (v->up_btn && button == v->up_btn) {
            if (!v->own_move && v->dir != +1) track_move(v, +1);
        } else if (v->down_btn && button == v->down_btn) {
            if (!v->own_move && v->dir != -1) track_move(v, -1);
        }
        /* v->own_move : notre propre echo (meme serial rejoue) -> ignore */
        break;
    }
}

/* ── RX : chaque trame recue -> journal + les 2 fonctions ci-dessus ── */
void shutters_on_rx(uint32_t serial, uint8_t button, int8_t rssi, uint32_t hop) {
    ESP_LOGI(TAG, "RX serial=0x%07X bouton=0x%X hop=0x%08X rssi=%d dBm",
             (unsigned)serial, button, (unsigned)hop, rssi);
    LOCK();
    /* journal RF pour le monitor UI */
    rfrec_t *r = &s_rf[s_rfhead];
    r->t = (uint32_t)(esp_timer_get_time() / 1000000); r->button = button; r->hop = hop; r->rssi = rssi;
    snprintf(r->serial, SH_SERIAL_LEN, "0x%07X", (unsigned)serial);
    s_rfhead = (s_rfhead + 1) % 12;
    /* auto-enregistre le serial vu (nom vide) */
    char shex[SH_SERIAL_LEN]; snprintf(shex, sizeof(shex), "0x%07X", (unsigned)serial);
    bool known = false;
    for (int i = 0; i < s_nremotes; i++) if (!strcmp(s_remotes[i].serial, shex)) { known = true; break; }
    if (!known && s_nremotes < 16) { strlcpy(s_remotes[s_nremotes].serial, shex, SH_SERIAL_LEN); s_remotes[s_nremotes].name[0] = 0; s_nremotes++; }

    if (s_log_frames && s_mqtt_ready) dataset_log_frame(shex, button, hop, rssi);  /* fonction 1 (option MQTT) */
    track_shutter_position(shex, button);                                          /* fonction 2 (toujours) */
    UNLOCK();
}

/* Ajoute au JSON une commande apprise : { "b": <bouton>, "s": "0x<serial>" }.
 * Le serial est extrait de la trame stockee (bits[32..59], LSB), pour verifier
 * dans l'UI que les 3 commandes viennent bien de la meme telecommande. */
static void add_cmd_json(cJSON *cmd, const char *key, const char *bits, uint8_t btn) {
    if (!bits[0]) return;
    uint32_t ser = 0;
    if ((int)strlen(bits) >= 60) for (int i = 0; i < 28; i++) if (bits[32 + i] == '1') ser |= (1u << i);
    char sx[12]; snprintf(sx, sizeof(sx), "0x%07X", (unsigned)ser);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddNumberToObject(c, "b", btn);
    cJSON_AddStringToObject(c, "s", sx);
    cJSON_AddItemToObject(cmd, key, c);
}

/* ── Status JSON ── */
int shutters_status_json(char *buf, int cap) {
    LOCK();
    cJSON *root = cJSON_CreateObject();
    cJSON *rem = cJSON_AddObjectToObject(root, "remotes");
    for (int i = 0; i < s_nremotes; i++) cJSON_AddStringToObject(rem, s_remotes[i].serial, s_remotes[i].name);
    cJSON *vols = cJSON_AddArrayToObject(root, "volets");
    for (int i = 0; i < s_nvolets; i++) {
        volet_t *v = &s_volets[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", v->id);
        cJSON_AddNumberToObject(o, "position", (int)(v->position + 0.5f));
        cJSON *sr = cJSON_AddArrayToObject(o, "serials");
        for (int j = 0; j < v->n_serials; j++) cJSON_AddItemToArray(sr, cJSON_CreateString(v->serials[j]));
        /* etat des 3 commandes : bouton appris (nibble) si le slot est rempli, absent sinon.
         * Permet a l'UI d'apprentissage centree-volet d'afficher appris / a capturer. */
        cJSON *cmd = cJSON_AddObjectToObject(o, "cmd");
        add_cmd_json(cmd, "up",   v->up,   v->up_btn);
        add_cmd_json(cmd, "stop", v->stop, v->stop_btn);
        add_cmd_json(cmd, "down", v->down, v->down_btn);
        cJSON_AddItemToArray(vols, o);
    }
    cJSON *rf = cJSON_AddArrayToObject(root, "rf");
    for (int k = 0; k < 12; k++) {
        int idx = (s_rfhead - 1 - k + 24) % 12;
        rfrec_t *r = &s_rf[idx];
        if (!r->serial[0]) continue;
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "t", r->t);
        cJSON_AddStringToObject(f, "serial", r->serial);
        char hx[10]; snprintf(hx, sizeof(hx), "%X", r->button); cJSON_AddStringToObject(f, "button", hx);
        snprintf(hx, sizeof(hx), "%08X", (unsigned)r->hop); cJSON_AddStringToObject(f, "hop", hx);
        cJSON_AddNumberToObject(f, "rssi", r->rssi);
        cJSON_AddItemToArray(rf, f);
    }
    UNLOCK();
    /* Statut Wi-Fi + MQTT (pour l'UI : pastilles + force du signal) */
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddBoolToObject(wifi, "connected", true);
        cJSON_AddStringToObject(wifi, "ssid", (char *)ap.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
            char ips[16]; snprintf(ips, sizeof(ips), IPSTR, IP2STR(&ip.ip));
            cJSON_AddStringToObject(wifi, "ip", ips);
        }
    } else {
        cJSON_AddBoolToObject(wifi, "connected", false);
    }
    cJSON_AddBoolToObject(root, "mqtt", s_mqtt_ready);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    int n = 0;
    if (js) { n = snprintf(buf, cap, "%s", js); free(js); }
    if (n >= cap) n = cap - 1;   /* snprintf renvoie la longueur NON tronquee : borner pour ne pas sur-lire buf */
    return n;
}

/* ── Sauvegarde / restauration (telecommandes + noms + trames de reference + calibration) ── */
int shutters_export_json(char *buf, int cap) {
    LOCK();
    char *js = cfg_to_json();
    UNLOCK();
    int n = 0;
    if (js) { n = snprintf(buf, cap, "%s", js); free(js); }
    return n;
}
int shutters_import_json(const char *js) {
    if (!js) return -1;
    cJSON *root = cJSON_Parse(js);
    if (!root) return -1;
    LOCK();
    reset_state();
    parse_cfg_json(root);
    save_cfg();
    for (int i = 0; i < s_nvolets; i++) announce_one(&s_volets[i]);   /* re-publie les covers HA restaures */
    update_listening();
    UNLOCK();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "config restauree : %d volets, %d telecommandes", s_nvolets, s_nremotes);
    return 0;
}

/* ── Integration Home Assistant (MQTT) ── */
void shutters_mqtt_announce(const char *device) {
    LOCK();
    strlcpy(s_device, (device && *device) ? device : "op", sizeof(s_device));
    s_mqtt_ready = true;
    for (int i = 0; i < s_nvolets; i++) announce_one(&s_volets[i]);
    flush_frame_ring_locked();   /* rattrapage : envoie les dernieres trames du ring des la connexion */
    UNLOCK();
    ESP_LOGI(TAG, "HA discovery publiee pour %d cover(s), device=%s", s_nvolets, s_device);
}

void shutters_mqtt_on_message(const char *topic, const char *data, int len) {
    static const char P[] = "openprofalux/cover/";
    if (strncmp(topic, P, sizeof(P) - 1)) return;
    const char *after = topic + sizeof(P) - 1;
    const char *slash = strchr(after, '/');
    if (!slash) return;
    char id[SH_ID_LEN]; int n = slash - after; if (n >= SH_ID_LEN) n = SH_ID_LEN - 1;
    memcpy(id, after, n); id[n] = 0;
    const char *sub = slash + 1;
    char payload[16]; int m = len < 15 ? len : 15; memcpy(payload, data, m); payload[m] = 0;
    if (!strcmp(sub, "set")) {
        if      (!strcasecmp(payload, "OPEN"))  shutters_cmd(id, "up", 0);
        else if (!strcasecmp(payload, "CLOSE")) shutters_cmd(id, "down", 0);
        else if (!strcasecmp(payload, "STOP"))  shutters_cmd(id, "stop", 0);
    } else if (!strcmp(sub, "set_position")) {
        shutters_cmd(id, "pos", atoi(payload));
    }
}

/* L'ecoute radio est necessaire pour DEUX raisons independantes :
 *  - suivre la position via une vraie telecommande  -> des qu'au moins un volet est appris,
 *  - collecter le dataset slide                      -> si l'option "capture" est ON.
 * On ecoute donc si l'une OU l'autre est vraie. */
static void update_listening(void) {
    /* Ecoute permanente = uniquement si l'option est activee. Evite la saturation
     * (boucle RMT sur le bruit OOK) en usage normal. L'apprentissage et le rejeu
     * n'en dependent pas ; seuls le RF debug live, le suivi de position via la vraie
     * telecommande et le dataset MQTT necessitent cette ecoute. */
    radio_set_listening(s_log_frames);
}

void shutters_set_log_frames(bool on) {
    s_log_frames = on;
    update_listening();
    if (on) { LOCK(); flush_frame_ring_locked(); UNLOCK(); }   /* si MQTT deja connecte : rattrape le ring tout de suite */
    ESP_LOGI(TAG, "log_frames=%d", on);
}

/* Dispatch d'une trame recue par la tache radio -> sync position + frame-log. */
static void on_air(const char *bits, uint32_t serial, uint8_t button, uint32_t hop, int8_t rssi) {
    (void)bits;
    shutters_on_rx(serial, button, rssi, hop);
}

void shutters_init(void) {
    s_lock = xSemaphoreCreateMutex();
    load_cfg();
    xTaskCreate(tick_task, "sh_tick", 4096, NULL, 5, NULL);
    radio_init();
    radio_start(on_air);   /* tache radio prete (arbitre TX) */
    update_listening();    /* ecoute si des volets sont deja appris (suivi position) ou si capture ON */
    ESP_LOGI(TAG, "init : %d volets, %d telecommandes", s_nvolets, s_nremotes);
}
