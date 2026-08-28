/*
 * pfx_enrol.c — enrôlement d'une identité virtuelle 0x067 (voir pfx_enrol.h).
 *
 * La génération de trame réplique EXACTEMENT le banc (firmware-capture-test : pfx_frame_build +
 * tx067_one) : plaintext = button<<28 | discrim<<16 | counter ; hop = KeeLoq(plaintext, clé du
 * slot) ; hop et serial sérialisés LSB-first ; statut '01' (RPT=1). L'émission passe par le même
 * chemin PWM que le rejeu (radio_tx -> cc1101_tx_raw_bits), au TE du modèle choisi.
 */
#include "pfx_enrol.h"
#include "keeloq.h"
#include "pfx_keys.h"
#include "radio.h"
#include "cc1101.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG    = "pfx_enrol";
static const char *NVS_NS = "pfx_enrol";

/* Rafale par appui = ~9 trames (mesuré sur trames réelles Lecanard). Compteur figé sur la rafale. */
#define PFX_BURST     9
/* Directions (montée/arrêt/descente) = PFX_BTN_* dans pfx_enrol.h (partagées avec shutters). */
#define BTN_LEARN   0x5   /* trame d'apprentissage radio */

/* Catalogue : tous routés canal PFX 0x636f, famille 0x067, même table de clés.
 * Seuls le TE et la chorégraphie (R6/R7/R8) diffèrent par modèle. */
static const pfx_model_t MODELS[] = {
    { "Profalux MAI-EMPX / MUR / NOE", 455, "R6" },
    { "FranciaFlex M4G",               415, "R6" },
    { "Profalux NeoSol",               455, "R8" },
    { "Profalux MAI-RDPX / RDTELPX",   455, "R7" },
};
#define N_MODELS ((int)(sizeof(MODELS) / sizeof(MODELS[0])))

static pfx_ident_t s_id;

int                pfx_enrol_model_count(void) { return N_MODELS; }
const pfx_model_t *pfx_enrol_model(int id)     { return (id >= 0 && id < N_MODELS) ? &MODELS[id] : NULL; }

static void save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "active", s_id.active ? 1 : 0);
    nvs_set_u8(h, "model", (uint8_t)s_id.model);
    nvs_set_u8(h, "idx", s_id.idx);
    nvs_set_u32(h, "serial", s_id.serial);
    nvs_set_u16(h, "counter", s_id.counter);
    nvs_commit(h); nvs_close(h);
}

void pfx_enrol_init(void) {
    memset(&s_id, 0, sizeof(s_id));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t a = 0, m = 0, ix = 0; uint32_t sr = 0; uint16_t c = 0;
        nvs_get_u8(h, "active", &a); nvs_get_u8(h, "model", &m); nvs_get_u8(h, "idx", &ix);
        nvs_get_u32(h, "serial", &sr); nvs_get_u16(h, "counter", &c);
        nvs_close(h);
        s_id.active = a; s_id.model = (m < N_MODELS) ? m : 0; s_id.idx = ix;
        s_id.serial = sr; s_id.counter = c;
    }
}

bool pfx_enrol_get(pfx_ident_t *out) { if (out) *out = s_id; return s_id.active; }

int pfx_enrol_new_identity(int model) {
    if (model < 0 || model >= N_MODELS) return -1;
    /* Tire un slot au hasard dans [1,63] : serial = slot<<12 | 0x067, idx = slot-1. */
    uint8_t slot = (uint8_t)(1 + (esp_random() % PFX_KEY_COUNT));
    uint32_t serial = ((uint32_t)slot << 12) | PFX_FAMILY_MARKER;
    uint64_t key; uint8_t idx;
    if (!pfx_key_for_serial(serial, &key, &idx)) return -1;   /* garde-fou */
    s_id.active = true; s_id.model = model; s_id.idx = idx;
    s_id.serial = serial; s_id.counter = 2;
    save();
    ESP_LOGW(TAG, "nouvelle identite 0x067 serial=0x%07X slot=%u model=%s",
             (unsigned)serial, slot, MODELS[model].name);
    return idx;
}

void pfx_enrol_forget(void) { memset(&s_id, 0, sizeof(s_id)); save(); }

/* rev4 : inverse les 4 bits du bouton (LSB-first sur l'air, fix anti-tamper). */
static inline uint8_t rev4(uint8_t b) {
    return (uint8_t)(((b & 1) << 3) | ((b & 2) << 1) | ((b & 4) >> 1) | ((b & 8) >> 3));
}

/* Construit la trame 66 bits on-air ('0'/'1', out[66]=NUL) pour une identité 0x067.
 * Retourne 0 si serial valide (famille 0x067), -1 sinon. Public (cf. pfx_enrol.h). */
int pfx_enrol_frame(uint32_t serial, uint8_t button, uint16_t counter, char out[68]) {
    uint64_t key = 0;
    if (!pfx_key_for_serial(serial, &key, NULL)) { out[0] = 0; return -1; }
    uint16_t discrim = (uint16_t)(serial & 0xFFF);
    uint32_t plain = ((uint32_t)(button & 0xF) << 28) | ((uint32_t)(discrim & 0xFFF) << 16) | counter;
    uint32_t enc = keeloq_encrypt(plain, key);
    uint32_t enc_tx = 0;                                   /* hop LSB-first */
    for (int k = 0; k < 32; k++) enc_tx |= ((enc >> k) & 1u) << (31 - k);
    uint8_t frame[9]; memset(frame, 0, 9);
    frame[0] = (enc_tx >> 24) & 0xFF; frame[1] = (enc_tx >> 16) & 0xFF;
    frame[2] = (enc_tx >>  8) & 0xFF; frame[3] =  enc_tx        & 0xFF;
    uint32_t sr = 0;                                       /* serial 28b LSB-first */
    for (int k = 0; k < 28; k++) sr |= ((serial >> k) & 1u) << (27 - k);
    frame[4] = (sr >> 20) & 0xFF; frame[5] = (sr >> 12) & 0xFF; frame[6] = (sr >> 4) & 0xFF;
    frame[7] = ((sr & 0xF) << 4) | rev4(button);
    frame[8] = 0x40;                                       /* statut '01' = RPT=1 */
    for (int i = 0; i < 66; i++) out[i] = ((frame[i / 8] >> (7 - (i % 8))) & 1) ? '1' : '0';
    out[66] = 0;
    return 0;
}

uint16_t pfx_enrol_model_te(int model) {
    return (model >= 0 && model < N_MODELS) ? MODELS[model].te_us : 455;
}

/* Lit le TE d'émission configuré (cfg/tx_te) pour le restaurer après notre émission. */
static uint32_t cfg_tx_te(void) {
    uint32_t te = 455; nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        uint32_t t; if (nvs_get_u32(h, "tx_te", &t) == ESP_OK && t) te = t;
        nvs_close(h);
    }
    return te;
}

static int emit(uint8_t button, uint16_t counter) {
    if (!s_id.active) return -1;
    const pfx_model_t *m = &MODELS[s_id.model];
    char bits[68]; pfx_enrol_frame(s_id.serial, button, counter, bits);
    uint32_t prev_te = cfg_tx_te();
    cc1101_set_tx_te(m->te_us);          /* TE du modèle le temps de la rafale */
    radio_tx(bits, PFX_BURST);           /* rafale, compteur figé sur toute la rafale */
    cc1101_set_tx_te(prev_te);           /* restaure le TE de rejeu configuré */
    ESP_LOGW(TAG, "TX 0x067 serial=0x%07X btn=0x%X cnt=%u te=%u",
             (unsigned)s_id.serial, button, counter, m->te_us);
    return 0;
}

int pfx_enrol_emit_learn(void) {
    if (!s_id.active) return -1;
    int rc = emit(BTN_LEARN, 3);         /* trame d'apprentissage : bouton 0x5, compteur figé = 3 */
    if (rc == 0) { s_id.counter = 4; save(); }   /* commandes de test à partir de 4 */
    return rc;
}

int pfx_enrol_cmd(const char *cmd) {
    if (!s_id.active || !cmd) return -1;
    uint8_t b;
    if      (!strcmp(cmd, "up"))   b = PFX_BTN_UP;
    else if (!strcmp(cmd, "stop")) b = PFX_BTN_STOP;
    else if (!strcmp(cmd, "down")) b = PFX_BTN_DOWN;
    else return -1;
    int rc = emit(b, s_id.counter);
    if (rc == 0) { s_id.counter++; save(); }
    return rc;
}
