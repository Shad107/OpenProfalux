/*
 * shutters.c — modele cover OpenProfalux (clone/replay + position time-based).
 * Config persistee en NVS sous forme de blob JSON (cle "cfg" du namespace "shutters").
 */
#include "shutters.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
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

/* dframe_t (trame distincte du dataset slide : hop brut + bouton + t) est defini dans shutters.h. */
typedef struct {
    char serial[SH_SERIAL_LEN]; char name[SH_ID_LEN];
    uint32_t  last_hop;      /* fast-path : dernier hop (evite le scan sur les maintiens) */
    dframe_t *hops;          /* SET des trames distinctes (dedup par hop) : hop + bouton + t */
    uint16_t  nhops;         /* = nb de trames DISTINCTES loggees */
    uint16_t  caphops;       /* capacite allouee du set */
} remote_t;

typedef struct { uint32_t t; char serial[SH_SERIAL_LEN]; uint8_t button; uint32_t hop; int8_t rssi; } rfrec_t;
/* LEGER (pas de bits stockes) : les 66 bits sont RECONSTRUITS a la demande depuis serial+bouton+hop
 * (build_frame_bits) pour le rejeu et l'adoption -> permet un ring de 1000 sans exploser la RAM. */

static volet_t  s_volets[SH_MAX_VOLETS];
static int      s_nvolets = 0;
static remote_t s_remotes[16];
static int      s_nremotes = 0;
#define RF_RING 1000          /* trames a l'affichage (paginees) ; l'export /api/frames = l'ensemble du dataset */
#define STATUS_RF_SHOW 20     /* nb de trames recentes mises dans /api/status (leger, poll 3 s) ; l'onglet RF pagine via /api/rf */
static rfrec_t  s_rf[RF_RING];
static int      s_rfhead = 0;
static SemaphoreHandle_t s_lock;
static char     s_device[32] = "op";   /* nom appareil (prefixe topics HA) */
static bool     s_mqtt_ready = false;   /* true apres shutters_mqtt_announce() */
static bool     s_log_frames = false;   /* publie toutes les trames captees en MQTT */
static bool     s_frames_dirty = false; /* dataset modifie -> a resauvegarder en NVS */
static bool     s_ring_dirty = false;    /* ring RF modifie -> a resauvegarder en NVS */
#define FRAMES_NVS_MAX 256              /* hops distincts persistes NVS (~2 Ko) ; NVS=16Ko partagee avec le ring 300 + cfg. Le gros dataset slide s'accumule cote MQTT/HA. */

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

/* nom volet -> slug HA-safe ([a-zA-Z0-9_-]) : espaces/accents interdits dans les topics
 * (decouverte HA ET command/state), sinon commandes/decouverte cassees. */
static void slugify(char *dst, int cap, const char *src) {
    int p = 0;
    for (int k = 0; src[k] && p < cap - 1; k++) {
        char c = src[k];
        dst[p++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    dst[p] = 0;
    if (!p && cap > 6) strlcpy(dst, "volet", cap);
}
static volet_t *find_volet_by_slug(const char *slug) {
    char s[SH_ID_LEN];
    for (int i = 0; i < s_nvolets; i++) { slugify(s, sizeof(s), s_volets[i].id); if (!strcmp(s, slug)) return &s_volets[i]; }
    return NULL;
}

/* ── Accesseurs dataset (export des trames captees : hops distincts par telecommande) ── */
int shutters_remote_count(void) { LOCK(); int n = s_nremotes; UNLOCK(); return n; }
int shutters_remote_dump(int i, char *serial, int sser, char *name, int sname, dframe_t *frames, int maxframes) {
    LOCK();
    if (i < 0 || i >= s_nremotes) { UNLOCK(); return -1; }
    strlcpy(serial, s_remotes[i].serial, sser);
    strlcpy(name, s_remotes[i].name, sname);
    int nh = s_remotes[i].nhops; if (nh > maxframes) nh = maxframes;
    if (nh > 0 && s_remotes[i].hops) memcpy(frames, s_remotes[i].hops, (size_t)nh * sizeof(dframe_t));
    UNLOCK();
    return nh < 0 ? 0 : nh;
}

/* ── Persistance JSON <-> NVS ── */
/* Serialise toute la config (telecommandes + noms + trames de reference + calibration). */
static char *cfg_to_json(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "log_frames", s_log_frames);   /* ecoute permanente : restauree au boot */
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
/* Persistance BORNEE du dataset de trames (hops distincts) en NVS : survit au reboot,
 * mais plafonnee (FRAMES_NVS_MAX) pour ne pas saturer la flash. Le GROS dataset (65536)
 * doit vivre cote HA (recorder). Sauvegarde periodique (pas a chaque trame -> menage la flash). */
static void save_frames(void) {
    static uint32_t buf[FRAMES_NVS_MAX * 4];   /* records {serial, hop, t, bouton} */
    int n = 0;
    LOCK();
    for (int i = 0; i < s_nremotes && n < FRAMES_NVS_MAX; i++) {
        uint32_t ser = (uint32_t)strtoul(s_remotes[i].serial, NULL, 16);
        for (int k = 0; k < s_remotes[i].nhops && n < FRAMES_NVS_MAX; k++) {
            dframe_t *d = &s_remotes[i].hops[k];
            buf[n * 4] = ser; buf[n * 4 + 1] = d->hop; buf[n * 4 + 2] = d->t; buf[n * 4 + 3] = d->button; n++;
        }
    }
    s_frames_dirty = false;
    UNLOCK();
    nvs_handle_t h;
    if (nvs_open("shutters", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "framesv2", buf, (size_t)n * 4 * sizeof(uint32_t));
        nvs_commit(h); nvs_close(h);
    }
}
static void load_frames(void) {   /* appele au boot (sous LOCK via shutters_init) apres load_cfg */
    nvs_handle_t h;
    if (nvs_open("shutters", NVS_READONLY, &h) != ESP_OK) return;
    static uint32_t buf[FRAMES_NVS_MAX * 4];
    size_t sz = sizeof(buf);
    esp_err_t e = nvs_get_blob(h, "framesv2", buf, &sz);
    nvs_close(h);
    if (e != ESP_OK || sz < 16) return;
    int n = sz / (4 * sizeof(uint32_t));
    for (int i = 0; i < n; i++) {
        char shex[SH_SERIAL_LEN]; snprintf(shex, sizeof(shex), "0x%07X", (unsigned)buf[i * 4]);
        remote_t *rm = NULL;
        for (int j = 0; j < s_nremotes; j++) if (!strcmp(s_remotes[j].serial, shex)) { rm = &s_remotes[j]; break; }
        if (!rm && s_nremotes < 16) { rm = &s_remotes[s_nremotes++]; memset(rm, 0, sizeof(*rm)); strlcpy(rm->serial, shex, SH_SERIAL_LEN); }
        if (!rm) continue;
        if (rm->nhops >= rm->caphops && rm->caphops < SH_MAX_HOPS) {
            uint16_t nc = rm->caphops ? (uint16_t)(rm->caphops * 2) : 32; if (nc > SH_MAX_HOPS) nc = SH_MAX_HOPS;
            dframe_t *np = realloc(rm->hops, (size_t)nc * sizeof(dframe_t));
            if (np) { rm->hops = np; rm->caphops = nc; }
        }
        if (rm->nhops < rm->caphops) rm->hops[rm->nhops++] = (dframe_t){ .hop = buf[i * 4 + 1], .t = buf[i * 4 + 2], .button = (uint8_t)buf[i * 4 + 3] };
    }
}

/* Reconstruit la trame 66 bits a partir de serial(28)+bouton(4)+hop(32) : format deterministe
 * (radio.c : hop=bits[0..31], serial=bits[32..59], bouton=bits[60..63], LSB-first). Les 2 bits
 * de statut (VLOW/RPT, bits[64..65]) ne sont pas transmis -> 0 (sans effet sur le rejeu). */
static void build_frame_bits(char *out, uint32_t serial, uint8_t button, uint32_t hop) {
    for (int i = 0; i < 32; i++) out[i]      = ((hop    >> i) & 1u) ? '1' : '0';
    for (int i = 0; i < 28; i++) out[32 + i] = ((serial >> i) & 1u) ? '1' : '0';
    for (int i = 0; i < 4;  i++) out[60 + i] = ((button >> i) & 1u) ? '1' : '0';
    out[64] = '0'; out[65] = '0'; out[66] = 0;
}

/* Persistance BORNEE du ring RF en NVS : on ne stocke que les METADONNEES {serial,bouton,hop,t,rssi}
 * (16 o/trame) et on RECONSTRUIT les bits au boot -> les trames restent REJOUABLES apres reboot/flash. */
typedef struct { uint32_t serial, hop, t; uint8_t button; int8_t rssi; uint16_t _pad; } ringrec_t;
#define RING_FILE "/spiffs/rfring.bin"
static ringrec_t s_ringbuf[RF_RING];   /* buffer partage save/load (hors pile, 1 seule copie) */
static void spiffs_mount(void) {   /* la NVS (16 Ko) ne tient pas le ring 300 -> SPIFFS (partition storage, 960 Ko) */
    esp_vfs_spiffs_conf_t c = { .base_path = "/spiffs", .partition_label = "storage", .max_files = 4, .format_if_mount_failed = true };
    esp_err_t e = esp_vfs_spiffs_register(&c);
    ESP_LOGI(TAG, "spiffs mount: %s", esp_err_to_name(e));
}
/* Le (serial, hop) est-il deja dans le ring ? (dedup : une pression = ~10 repetitions du meme hop).
 * Le hop encode le bouton -> (serial, hop) identique => meme bouton. Caller detient le LOCK (ou boot). */
static bool ring_contains(const char *shex, uint32_t hop) {
    for (int k = 0; k < RF_RING; k++)
        if (s_rf[k].serial[0] && s_rf[k].hop == hop && !strcmp(s_rf[k].serial, shex)) return true;
    return false;
}
static void save_ring(void) {
    int head;
    LOCK();
    head = s_rfhead;
    for (int i = 0; i < RF_RING; i++) {
        rfrec_t *r = &s_rf[i];
        s_ringbuf[i].serial = r->serial[0] ? (uint32_t)strtoul(r->serial, NULL, 16) : 0;
        s_ringbuf[i].hop = r->hop; s_ringbuf[i].t = r->t; s_ringbuf[i].button = r->button; s_ringbuf[i].rssi = r->rssi; s_ringbuf[i]._pad = 0;
    }
    s_ring_dirty = false;
    UNLOCK();
    FILE *f = fopen(RING_FILE, "wb");
    if (!f) { ESP_LOGW(TAG, "save_ring: fopen KO"); return; }
    fwrite(&head, sizeof(head), 1, f);
    size_t w = fwrite(s_ringbuf, 1, sizeof(s_ringbuf), f);
    fclose(f);
    ESP_LOGI(TAG, "save_ring: %d o ecrits (head=%d)", (int)w, head);
}
static void load_ring(void) {   /* boot : restaure le ring + reconstruit les bits (trames rejouables) */
    FILE *f = fopen(RING_FILE, "rb");
    if (!f) { ESP_LOGW(TAG, "load_ring: pas de fichier (rien a charger)"); return; }
    int head = 0;
    if (fread(&head, sizeof(head), 1, f) != 1) { fclose(f); return; }
    size_t r = fread(s_ringbuf, 1, sizeof(s_ringbuf), f);
    fclose(f);
    (void)head;
    int n = r / sizeof(ringrec_t); if (n > RF_RING) n = RF_RING;
    int loaded = 0;   /* on compacte + on deduplique (nettoie d'eventuels doublons deja en SPIFFS) */
    for (int i = 0; i < n && loaded < RF_RING; i++) {
        if (!s_ringbuf[i].serial && !s_ringbuf[i].hop) continue;
        char shex[SH_SERIAL_LEN]; snprintf(shex, sizeof(shex), "0x%07X", (unsigned)s_ringbuf[i].serial);
        if (ring_contains(shex, s_ringbuf[i].hop)) continue;   /* dedup (serial, hop) */
        rfrec_t *rr = &s_rf[loaded];
        strlcpy(rr->serial, shex, SH_SERIAL_LEN);
        rr->hop = s_ringbuf[i].hop; rr->t = s_ringbuf[i].t; rr->button = s_ringbuf[i].button; rr->rssi = s_ringbuf[i].rssi;
        loaded++;
    }
    s_rfhead = loaded % RF_RING;   /* prochaine place libre apres compactage */
    ESP_LOGI(TAG, "load_ring: %d trames rechargees/dedupliquees (%d o)", loaded, (int)r);
}

/* Repeuplement depuis MQTT (frames/log/<slot>) : remplit le slot UNIQUEMENT s'il est vide (NVS vide)
 * -> restaure affichage RF + rejeu + dataset slide. Ne republie rien. Reconcilie aussi la NVS. */
static void ring_fill_from_mqtt(int slot, const char *ser, uint8_t button, uint32_t hop, uint32_t t, int8_t rssi) {
    if (slot < 0 || slot >= RF_RING || !ser || !ser[0]) return;
    LOCK();
    rfrec_t *r = &s_rf[slot];
    if (!r->serial[0] && !ring_contains(ser, hop)) {   /* slot vide ET pas deja present ailleurs (dedup) */
        strlcpy(r->serial, ser, SH_SERIAL_LEN);
        r->button = button; r->hop = hop; r->t = t; r->rssi = rssi;
        s_ring_dirty = true;
        remote_t *rm = NULL;
        for (int i = 0; i < s_nremotes; i++) if (!strcmp(s_remotes[i].serial, ser)) { rm = &s_remotes[i]; break; }
        if (!rm && s_nremotes < 16) { rm = &s_remotes[s_nremotes++]; memset(rm, 0, sizeof(*rm)); strlcpy(rm->serial, ser, SH_SERIAL_LEN); }
        if (rm) {
            bool isnew = true;
            for (int i = 0; i < rm->nhops; i++) if (rm->hops[i].hop == hop) { isnew = false; break; }
            if (isnew) {
                if (rm->nhops >= rm->caphops && rm->caphops < SH_MAX_HOPS) {
                    uint16_t nc = rm->caphops ? (uint16_t)(rm->caphops * 2) : 32; if (nc > SH_MAX_HOPS) nc = SH_MAX_HOPS;
                    dframe_t *np = realloc(rm->hops, (size_t)nc * sizeof(dframe_t)); if (np) { rm->hops = np; rm->caphops = nc; }
                }
                if (rm->nhops < rm->caphops) { rm->hops[rm->nhops++] = (dframe_t){ .hop = hop, .t = t, .button = button }; s_frames_dirty = true; }
            }
        }
    }
    UNLOCK();
}

/* Remet la config a zero (libere les sets de hops avant reload/import). */
static void reset_state(void) {
    for (int i = 0; i < s_nremotes; i++) { free(s_remotes[i].hops); s_remotes[i].hops = NULL; s_remotes[i].nhops = s_remotes[i].caphops = 0; }
    s_nremotes = 0; s_nvolets = 0;
}
/* Peuple s_remotes/s_volets depuis un JSON (l'etat doit etre remis a zero avant). */
static void parse_cfg_json(cJSON *root) {
    cJSON *lf = cJSON_GetObjectItem(root, "log_frames");
    if (cJSON_IsBool(lf)) s_log_frames = cJSON_IsTrue(lf);   /* restaure l'ecoute permanente au boot */
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
#define PRESS_REPEATS 10   /* nb de trames par "appui" : le bit-bang peut etre preempte par le WiFi
                              (trame corrompue) ; plus de repetitions = plus de chances qu'une passe propre */
/* Emet une RAFALE = 1 appui de telecommande, en UNE session TX (trames dos-a-dos, une seule
 * calibration) via l'arbitre radio. Le moteur part ensuite tout seul jusqu'au STOP/butee. */
static void emit_press(const char *bits) {
    if (bits && bits[0]) radio_tx(bits, PRESS_REPEATS);
}
/* Rejoue TELLE QUELLE une trame captee (identifiee par serial+hop dans l'anneau RF).
 * Sert au bouton "rejouer" du debug RF : on renvoie la trame brute, sans passer par un volet. */
int shutters_replay_frame(const char *serial, uint32_t hop) {
    if (!serial || !serial[0]) return -1;
    char bits[SH_BITS_LEN] = {0};
    LOCK();
    for (int k = 0; k < RF_RING; k++) {
        rfrec_t *r = &s_rf[k];
        if (r->serial[0] && !strcmp(r->serial, serial) && r->hop == hop) {   /* reconstruit les 66 bits a la demande */
            build_frame_bits(bits, (uint32_t)strtoul(serial, NULL, 16), r->button, hop); break;
        }
    }
    UNLOCK();
    if (!bits[0]) return -2;   /* plus dans l'anneau (ecrasee) */
    emit_press(bits);
    return 0;
}
/* Dump du ring RF pour /api/rf : k=0 = plus recente. Renvoie 0 si trame presente, -1 sinon. */
int shutters_rf_get(int k, char *serial, int sser, uint8_t *button, uint32_t *hop, uint32_t *t, int8_t *rssi) {
    if (k < 0 || k >= RF_RING) return -1;
    LOCK();
    int idx = (s_rfhead - 1 - k + 2 * RF_RING) % RF_RING;
    rfrec_t *r = &s_rf[idx];
    if (!r->serial[0]) { UNLOCK(); return -1; }
    strlcpy(serial, r->serial, sser);
    *button = r->button; *hop = r->hop; *t = r->t; *rssi = r->rssi;
    UNLOCK();
    return 0;
}
int shutters_rf_capacity(void) { return RF_RING; }
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
    char topic[96], pl[16], st[10], slug[SH_ID_LEN];
    slugify(slug, sizeof(slug), v->id);   /* topics slugifies (coherents avec la decouverte) */
    if (s_log_frames) {   /* position publiee seulement si suivi actif (coherent avec la decouverte + l'UI) */
        snprintf(topic, sizeof(topic), "openprofalux/cover/%s/position", slug);
        snprintf(pl, sizeof(pl), "%d", (int)(v->position + 0.5f));
        mqtt_pub_raw(topic, pl, 0, 1);
    }
    ha_state_str(v, st);
    snprintf(topic, sizeof(topic), "openprofalux/cover/%s/state", slug);
    mqtt_pub_raw(topic, st, 0, 1);
    v->pub_pos = (int)(v->position + 0.5f); v->pub_dir = v->dir;
}
/* Publie la config HA discovery d'un cover (retained). Appele sous LOCK. */
static void announce_one(volet_t *v) {
    if (!s_mqtt_ready) return;
    char topic[160], *pl = malloc(1536);
    if (!pl) return;
    /* configuration_url = UI web du boitier (via son IP) pour "Visiter l'appareil" dans HA ;
     * repli sur la page projet si l'IP n'est pas connue. */
    char cu[52];
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr)
        snprintf(cu, sizeof(cu), "http://" IPSTR, IP2STR(&ip.ip));
    else
        strlcpy(cu, "https://www.isno.fr/projets/openprofalux", sizeof(cu));
    /* Position exposee a HA UNIQUEMENT si l'ecoute permanente est active (comme dans l'UI web) :
     * sinon un coup de vraie telecommande desynchronise l'estimation, un % faux est pire qu'aucun.
     * Sans l'option, le cover reste pilotable (ouvrir/fermer/stop) mais sans position. */
    /* slug HA-safe : le nom du volet peut avoir espaces/accents (ex "Chambre Parent"),
     * INTERDITS dans les topics (decouverte ET command/state). On slugifie PARTOUT et on
     * resout le volet par slug a la reception (find_volet_by_slug). */
    char slug[SH_ID_LEN]; slugify(slug, sizeof(slug), v->id);
    const char *devname = (!s_device[0] || !strcmp(s_device, "openprofalux")) ? "OpenProfalux" : s_device;
    /* Position seulement si ecoute permanente (sinon % non fiable) */
    char pos[240] = "";
    if (s_log_frames)
        snprintf(pos, sizeof(pos),
            "\"position_topic\":\"openprofalux/cover/%s/position\","
            "\"set_position_topic\":\"openprofalux/cover/%s/set_position\","
            "\"position_open\":100,\"position_closed\":0,", slug, slug);
    snprintf(topic, sizeof(topic), "homeassistant/cover/openprofalux_%s/config", slug);
    /* Cles en TOUTES LETTRES (alignees sur OpenXtraflame) ; device PARTAGE (un seul appareil
     * "OpenProfalux" regroupe tous les volets) ; topics command/state SLUGIFIES. */
    snprintf(pl, 1536,
        "{\"name\":\"%s\",\"unique_id\":\"openprofalux_%s\",\"object_id\":\"openprofalux_%s\",\"device_class\":\"shutter\","
        "\"command_topic\":\"openprofalux/cover/%s/set\","
        "\"payload_open\":\"OPEN\",\"payload_close\":\"CLOSE\",\"payload_stop\":\"STOP\","
        "%s"
        "\"state_topic\":\"openprofalux/cover/%s/state\","
        "\"availability_topic\":\"openprofalux/%s/status\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"device\":{\"identifiers\":[\"openprofalux\"],\"name\":\"%s\","
        "\"manufacturer\":\"isno.fr\",\"model\":\"OpenProfalux\",\"configuration_url\":\"%s\"}}",
        v->id, slug, slug, slug, pos, slug, s_device, devname, cu);
    mqtt_pub_raw(topic, pl, 1, 1);
    free(pl);
    publish_volet_state(v);
}

/* Entites HA supplementaires (une seule fois, sous le meme appareil) :
 * - un SWITCH "Ecoute RF permanente" (on/off depuis HA),
 * - un CAPTEUR "Derniere trame RF" (voir les trames passer dans HA). */
static void announce_extras(void) {
    if (!s_mqtt_ready) return;
    const char *devname = (!s_device[0] || !strcmp(s_device, "openprofalux")) ? "OpenProfalux" : s_device;
    char dev[320];
    snprintf(dev, sizeof(dev),
        "\"availability_topic\":\"openprofalux/%s/status\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"device\":{\"identifiers\":[\"openprofalux\"],\"name\":\"%s\",\"manufacturer\":\"isno.fr\",\"model\":\"OpenProfalux\"}",
        s_device, devname);
    char *pl = malloc(700); if (!pl) return;
    snprintf(pl, 700,
        "{\"name\":\"Ecoute RF permanente\",\"unique_id\":\"openprofalux_listen\",\"object_id\":\"openprofalux_listen\","
        "\"command_topic\":\"openprofalux/listen/set\",\"state_topic\":\"openprofalux/listen/state\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"icon\":\"mdi:radio-tower\",%s}", dev);
    mqtt_pub_raw("homeassistant/switch/openprofalux_listen/config", pl, 1, 1);
    snprintf(pl, 700,
        "{\"name\":\"Derniere trame RF\",\"unique_id\":\"openprofalux_last_frame\",\"object_id\":\"openprofalux_last_frame\","
        "\"state_topic\":\"openprofalux/frames/last\",\"value_template\":\"{{ value_json.hop }}\","
        "\"json_attributes_topic\":\"openprofalux/frames/last\",\"icon\":\"mdi:remote\",%s}", dev);
    mqtt_pub_raw("homeassistant/sensor/openprofalux_last_frame/config", pl, 1, 1);
    free(pl);
    mqtt_pub_raw("openprofalux/listen/state", s_log_frames ? "ON" : "OFF", 0, 1);
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
    int save_ticks = 0;
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
        /* sauvegarde periodique du dataset en NVS (hors LOCK ; menage la flash : ~60 s si modifie) */
        if (++save_ticks * TICK_MS >= 60000) {
            save_ticks = 0;
            if (s_frames_dirty) save_frames();
            if (s_ring_dirty)   save_ring();   /* trames recentes rejouables persistees */
        }
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

/* Champs (bits + bouton) d'une action pour un volet. NULL si action inconnue. */
static char *action_bits(volet_t *v, const char *a, uint8_t **btn) {
    if (!strcmp(a, "up"))   { *btn = &v->up_btn;   return v->up; }
    if (!strcmp(a, "stop")) { *btn = &v->stop_btn; return v->stop; }
    if (!strcmp(a, "down")) { *btn = &v->down_btn; return v->down; }
    return NULL;
}

/* Permute deux commandes apprises (corrige une trame rangee dans le mauvais slot,
 * sans avoir a re-presser la telecommande). Si la cible est vide, revient a un deplacement. */
int shutters_reassign(const char *id, const char *from, const char *to) {
    if (!id || !from || !to) return -1;
    if (!strcmp(from, to)) return 0;
    LOCK();
    volet_t *v = find_volet(id);
    if (!v) { UNLOCK(); return -1; }
    uint8_t *fb, *tb; char *fbits = action_bits(v, from, &fb), *tbits = action_bits(v, to, &tb);
    if (!fbits || !tbits) { UNLOCK(); return -1; }
    char tmp[SH_BITS_LEN]; uint8_t tmpb;
    strlcpy(tmp, tbits, SH_BITS_LEN); tmpb = *tb;
    strlcpy(tbits, fbits, SH_BITS_LEN); *tb = *fb;
    strlcpy(fbits, tmp, SH_BITS_LEN); *fb = tmpb;
    save_cfg();
    announce_one(v);
    UNLOCK();
    ESP_LOGW(TAG, "reaffectation '%s' %s <-> %s", id, from, to);
    return 0;
}

int shutters_adopt(const char *id, const char *action, const char *serial, uint32_t hop) {
    if (!id || !action || !serial) return -1;
    char bits[SH_BITS_LEN] = {0};
    LOCK();
    for (int k = 0; k < RF_RING; k++) {
        rfrec_t *r = &s_rf[k];
        if (r->serial[0] && !strcmp(r->serial, serial) && r->hop == hop) {   /* reconstruit les 66 bits */
            build_frame_bits(bits, (uint32_t)strtoul(serial, NULL, 16), r->button, hop); break;
        }
    }
    UNLOCK();
    if (!bits[0]) return -1;
    return shutters_learn_assign(id, action, bits);   /* prend son propre LOCK */
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
static void dataset_log_frame(const char *shex, uint8_t button, uint32_t hop, int8_t rssi, uint32_t t) {
    remote_t *rm = NULL;
    for (int i = 0; i < s_nremotes; i++) if (!strcmp(s_remotes[i].serial, shex)) { rm = &s_remotes[i]; break; }
    if (!rm) return;
    bool is_new = true;
    if (rm->nhops && rm->last_hop == hop) is_new = false;                      /* fast-path maintien */
    else for (int i = 0; i < rm->nhops; i++) if (rm->hops[i].hop == hop) { is_new = false; break; }
    rm->last_hop = hop;
    if (!is_new) return;                                                       /* deja enregistre -> rien */
    /* ajoute au set (croissance bornee a SH_MAX_HOPS) */
    if (rm->nhops >= rm->caphops && rm->caphops < SH_MAX_HOPS) {
        uint16_t nc = rm->caphops ? (uint16_t)(rm->caphops * 2) : 32;
        if (nc > SH_MAX_HOPS) nc = SH_MAX_HOPS;
        dframe_t *np = realloc(rm->hops, (size_t)nc * sizeof(dframe_t));
        if (np) { rm->hops = np; rm->caphops = nc; }
    }
    if (rm->nhops < rm->caphops) { rm->hops[rm->nhops++] = (dframe_t){ .hop = hop, .t = t, .button = button }; s_frames_dirty = true; }
    /* Plus de publication frames/<serial> ni /count : une seule structure MQTT = l'anneau frames/log/<n>.
     * Le dataset slide (hops distincts) reste construit ici en RAM/NVS pour l'export /api/frames. */
}

/* Rattrapage a la connexion : pousse TOUT le ring (trames captees hors-ligne) vers MQTT,
 * du plus ancien au plus recent -> dataset (dedup) + anneau retained frames/log/<slot>.
 * Realise le modele tampon : capte toujours, buffer si MQTT absent, flush a l'activation.
 * Appele sous LOCK, no-op si option off ou MQTT absent. */
static void flush_frame_ring_locked(void) {
    if (!s_log_frames || !s_mqtt_ready) return;
    char lt[48], lf[128];
    for (int k = 0; k < RF_RING; k++) {
        int slot = (s_rfhead + k) % RF_RING;
        rfrec_t *r = &s_rf[slot];
        if (!r->serial[0]) continue;
        dataset_log_frame(r->serial, r->button, r->hop, r->rssi, r->t);   /* dataset slide (dedup) */
        snprintf(lf, sizeof(lf), "{\"serial\":\"%s\",\"button\":\"0x%X\",\"hop\":\"0x%08X\",\"rssi\":%d,\"t\":%u}",
                 r->serial, r->button, (unsigned)r->hop, r->rssi, (unsigned)r->t);
        snprintf(lt, sizeof(lt), "openprofalux/frames/log/%d", slot);
        mqtt_pub_raw(lt, lf, 1, 1);   /* QoS 1 (confirme) + retained : recuperable via frames/log/# */
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
void shutters_on_rx(const char *bits, uint32_t serial, uint8_t button, int8_t rssi, uint32_t hop) {
    ESP_LOGI(TAG, "RX serial=0x%07X bouton=0x%X hop=0x%08X rssi=%d dBm",
             (unsigned)serial, button, (unsigned)hop, rssi);
    LOCK();
    char shex[SH_SERIAL_LEN]; snprintf(shex, sizeof(shex), "0x%07X", (unsigned)serial);
    (void)bits;   /* plus stocke : les 66 bits sont reconstruits a la demande (rejeu/adoption) */
    /* DEDUP : meme (serial, hop) deja vu -> repetition d'un meme appui, on n'ajoute rien (ni ring ni MQTT). */
    if (!ring_contains(shex, hop)) {
        int slot = s_rfhead;   /* emplacement -> topic MQTT recuperable frames/log/<slot> */
        rfrec_t *r = &s_rf[s_rfhead];
        time_t now = time(NULL);
        uint32_t t = (now > 1600000000) ? (uint32_t)now : 0;   /* epoch si SNTP synchro, sinon 0 */
        r->t = t; r->button = button; r->hop = hop; r->rssi = rssi;
        strlcpy(r->serial, shex, SH_SERIAL_LEN);
        s_rfhead = (s_rfhead + 1) % RF_RING;
        s_ring_dirty = true;
        /* auto-enregistre le serial vu (nom vide) */
        bool known = false;
        for (int i = 0; i < s_nremotes; i++) if (!strcmp(s_remotes[i].serial, shex)) { known = true; break; }
        if (!known && s_nremotes < 16) { strlcpy(s_remotes[s_nremotes].serial, shex, SH_SERIAL_LEN); s_remotes[s_nremotes].name[0] = 0; s_nremotes++; }
        if (s_log_frames && s_mqtt_ready) {
            dataset_log_frame(shex, button, hop, rssi, t);  /* dataset slide (dedup par hop, bouton+t) */
            char lf[128];
            snprintf(lf, sizeof(lf), "{\"serial\":\"%s\",\"button\":\"0x%X\",\"hop\":\"0x%08X\",\"rssi\":%d,\"t\":%u}",
                     shex, button, (unsigned)hop, rssi, (unsigned)t);
            mqtt_pub_raw("openprofalux/frames/last", lf, 0, 1);   /* capteur HA "Derniere trame" */
            char lt[48]; snprintf(lt, sizeof(lt), "openprofalux/frames/log/%d", slot);
            mqtt_pub_raw(lt, lf, 1, 1);   /* QoS 1 + retained : recuperable via frames/log/# */
        }
    }
    track_shutter_position(shex, button);   /* toujours (idempotent sur les repetitions) */
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
    cJSON *rf = cJSON_AddArrayToObject(root, "rf");   /* recentes seulement ; les 300 via /api/rf */
    for (int k = 0; k < STATUS_RF_SHOW; k++) {
        int idx = (s_rfhead - 1 - k + 2 * RF_RING) % RF_RING;
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
    cJSON_AddBoolToObject(root, "listening", s_log_frames);   /* ecoute permanente = position fiable */
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));   /* s depuis boot : date les trames RF cote UI */
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
    announce_extras();           /* switch "Ecoute RF permanente" + capteur "Derniere trame" */
    flush_frame_ring_locked();   /* rattrapage : envoie les dernieres trames du ring des la connexion */
    UNLOCK();
    ESP_LOGI(TAG, "HA discovery publiee pour %d cover(s), device=%s", s_nvolets, s_device);
}

void shutters_mqtt_lost(void) {
    LOCK();
    s_mqtt_ready = false;   /* broker perdu : le statut UI repasse a deconnecte */
    UNLOCK();
    ESP_LOGW(TAG, "MQTT perdu : statut hors ligne");
}

void shutters_mqtt_on_message(const char *topic, const char *data, int len) {
    /* Switch HA "Ecoute RF permanente" (commande) */
    if (!strcmp(topic, "openprofalux/listen/set")) {
        shutters_set_log_frames(len >= 2 && !strncasecmp(data, "ON", 2));   /* publie l'etat + re-annonce */
        return;
    }
    /* Etat retained (publie par nous) : restaure l'ecoute au boot si differente. Garde anti-boucle. */
    if (!strcmp(topic, "openprofalux/listen/state")) {
        bool on = (len >= 2 && !strncasecmp(data, "ON", 2));
        if (on != s_log_frames) shutters_set_log_frames(on);   /* re-echo ignore (meme valeur) */
        return;
    }
    /* Repeuplement du ring depuis MQTT (retained frames/log/<slot>) : rempli si slot vide (NVS vide). */
    if (!strncmp(topic, "openprofalux/frames/log/", 24)) {
        int slot = atoi(topic + 24);
        cJSON *j = cJSON_ParseWithLength(data, len);
        if (j) {
            const char *ser = cJSON_GetStringValue(cJSON_GetObjectItem(j, "serial"));
            const char *bs  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "button"));
            const char *hs  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "hop"));
            cJSON *tj = cJSON_GetObjectItem(j, "t"), *rj = cJSON_GetObjectItem(j, "rssi");
            if (ser && hs)
                ring_fill_from_mqtt(slot, ser,
                    bs ? (uint8_t)strtoul(bs, NULL, 16) : 0, (uint32_t)strtoul(hs, NULL, 16),
                    tj ? (uint32_t)cJSON_GetNumberValue(tj) : 0, rj ? (int8_t)cJSON_GetNumberValue(rj) : 0);
            cJSON_Delete(j);
        }
        return;
    }
    static const char P[] = "openprofalux/cover/";
    if (strncmp(topic, P, sizeof(P) - 1)) return;
    const char *after = topic + sizeof(P) - 1;
    const char *slash = strchr(after, '/');
    if (!slash) return;
    char slug[SH_ID_LEN]; int n = slash - after; if (n >= SH_ID_LEN) n = SH_ID_LEN - 1;
    memcpy(slug, after, n); slug[n] = 0;
    const char *sub = slash + 1;
    /* le topic porte le SLUG -> on retrouve le vrai volet, puis on commande avec son id reel */
    volet_t *v = find_volet_by_slug(slug);
    if (!v) { ESP_LOGW(TAG, "MQTT cmd : volet slug '%s' inconnu", slug); return; }
    char id[SH_ID_LEN]; strlcpy(id, v->id, sizeof(id));
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
    LOCK();
    save_cfg();                                        /* persiste l'ecoute permanente -> restauree au boot */
    if (on) flush_frame_ring_locked();                 /* si MQTT deja connecte : rattrape le ring tout de suite */
    for (int i = 0; i < s_nvolets; i++) announce_one(&s_volets[i]);   /* re-publie la decouverte : position apparait/disparait selon l'option */
    if (s_mqtt_ready) mqtt_pub_raw("openprofalux/listen/state", on ? "ON" : "OFF", 0, 1);   /* etat du switch HA */
    UNLOCK();
    ESP_LOGI(TAG, "log_frames=%d", on);
}

/* Dispatch d'une trame recue par la tache radio -> sync position + frame-log. */
static void on_air(const char *bits, uint32_t serial, uint8_t button, uint32_t hop, int8_t rssi) {
    shutters_on_rx(bits, serial, button, rssi, hop);
}

void shutters_init(void) {
    s_lock = xSemaphoreCreateMutex();
    load_cfg();
    load_frames();   /* restaure le dataset de trames borne persiste en NVS (survit au reboot) */
    spiffs_mount();  /* stockage du ring RF (trop gros pour la NVS 16 Ko) */
    load_ring();     /* restaure les trames recentes rejouables (bits reconstruits) apres reboot */
    xTaskCreate(tick_task, "sh_tick", 4096, NULL, 5, NULL);
    radio_init();
    radio_start(on_air);   /* tache radio prete (arbitre TX) */
    update_listening();    /* ecoute si des volets sont deja appris (suivi position) ou si capture ON */
    ESP_LOGI(TAG, "init : %d volets, %d telecommandes, ecoute permanente=%s", s_nvolets, s_nremotes, s_log_frames ? "ON" : "OFF");
}
