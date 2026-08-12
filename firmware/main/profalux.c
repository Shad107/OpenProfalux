#include "profalux.h"
#include "keeloq.h"
#include "cc1101.h"
#include "pfx_keys.h"

/* --- Identite de test Phase 2 (reference DEVMEL) ---
 * Si PFX_USE_TEST_IDENTITY=1, pfx_state_reset() emet le serial DEVMEL du pool valide
 * (clef derivee via pfx_key_for_serial) au lieu d'un emetteur random.
 * serial 0x36067 = slot 54 (index 53) -> clef 0x41AA649E6ED6E5A7 (oracle valide).
 * Mettre a 0 pour revenir au self-learn random. */
#define PFX_USE_TEST_IDENTITY 1
#define PFX_TEST_SERIAL       0x00036067u
#include <string.h>
#include <esp_random.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "profalux";
static const char *NVS_NAMESPACE = "pfx_state";

int pfx_state_init(pfx_tx_state_t *st) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t sz = sizeof(*st);
        err = nvs_get_blob(h, "state", st, &sz);
        nvs_close(h);
        if (err == ESP_OK && sz == sizeof(*st)) {
            ESP_LOGI(TAG, "Loaded state: serial=0x%08X counter=%u",
                     (unsigned)st->serial, (unsigned)st->counter);
            return 0;
        }
    }
    ESP_LOGI(TAG, "No state in NVS, generating new random");
    return pfx_state_reset(st);
}

int pfx_state_save(const pfx_tx_state_t *st) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, "state", st, sizeof(*st)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    return 0;
}

int pfx_state_reset(pfx_tx_state_t *st) {
#if PFX_USE_TEST_IDENTITY
    /* Identite DEVMEL du pool valide (test Phase 2). Clef derivee du serial. */
    uint64_t k; uint8_t idx;
    if (pfx_key_for_serial(PFX_TEST_SERIAL, &k, &idx)) {
        st->serial = PFX_TEST_SERIAL;
        st->crypt_key = k;
        st->counter = 0;
        st->discrimination = st->serial & 0xFFF;   /* = 0x067 (famille PFX) */
        ESP_LOGI(TAG, "Identite DEVMEL de test: serial=0x%08X idx=%u key=0x%016llX",
                 (unsigned)st->serial, idx, (unsigned long long)st->crypt_key);
        return pfx_state_save(st);
    }
    ESP_LOGW(TAG, "PFX_TEST_SERIAL 0x%08X invalide (pas famille 0x067 ?), fallback random",
             (unsigned)PFX_TEST_SERIAL);
#endif
    st->serial = esp_random() & 0x0FFFFFFF;
    st->crypt_key = ((uint64_t)esp_random() << 32) | esp_random();
    st->counter = 0;
    st->discrimination = st->serial & 0xFFF;
    return pfx_state_save(st);
}

void pfx_frame_build(const pfx_tx_state_t *st, uint8_t button, uint8_t frame[9]) {
    /* State to encrypt: button(4) | discrim(12) | counter(16) = 32 bits */
    uint32_t plaintext = ((uint32_t)(button & 0xF) << 28)
                       | ((uint32_t)(st->discrimination & 0xFFF) << 16)
                       | st->counter;
    uint32_t enc = keeloq_encrypt(plaintext, st->crypt_key);

    /* Frame 66-bit layout (bit stream) :
     * [status:2][button:4][serial:28][encrypted:32]
     * Serialize MSB first for OOK/PWM transmission
     */
    memset(frame, 0, 9);
    /* Bits 64-33: encrypted (=high bits transmitted first) */
    frame[0] = (enc >> 24) & 0xFF;
    frame[1] = (enc >> 16) & 0xFF;
    frame[2] = (enc >>  8) & 0xFF;
    frame[3] = (enc >>  0) & 0xFF;
    /* Serial 28-bit serialise LSB-first (=comme les vraies trames HCS300/PFX 0x813,
     * verifie 25/25 contre captures reelles). On inverse les 28 bits du serial, puis
     * on empaquette MSB-first par octet (l'emission cc1101 est MSB-first) => le serial
     * ressort LSB-first sur l'air. crypt_key / counter / KeeLoq inchanges. */
    uint32_t sr = 0;
    for (int k = 0; k < 28; k++) sr |= ((st->serial >> k) & 1u) << (27 - k);
    frame[4] = (sr >> 20) & 0xFF;
    frame[5] = (sr >> 12) & 0xFF;
    frame[6] = (sr >>  4) & 0xFF;
    frame[7] = ((sr & 0xF) << 4) | (button & 0xF);
    /* Bits 4-3: button (already in frame[7]) */
    /* Bits 2-1: status flags = 0 (repeat=0, batt_low=0) */
    frame[8] = 0;
}

int pfx_frame_parse(const uint8_t frame[9], pfx_rx_frame_t *out) {
    if (!frame || !out) return -1;
    uint32_t enc = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16)
                 | ((uint32_t)frame[2] <<  8) | ((uint32_t)frame[3]);
    /* Serial LSB-first sur l'air (=meme convention que pfx_frame_build) : on lit le
     * champ empaquete (sr) puis on inverse les 28 bits pour retrouver le serial reel. */
    uint32_t sr = ((uint32_t)frame[4] << 20) | ((uint32_t)frame[5] << 12)
                | ((uint32_t)frame[6] <<  4) | ((uint32_t)(frame[7] >> 4) & 0x0F);
    uint32_t serial = 0;
    for (int k = 0; k < 28; k++) serial |= ((sr >> k) & 1u) << (27 - k);
    uint8_t button = frame[7] & 0x0F;
    uint8_t status = frame[8] & 0x03;

    out->serial = serial;
    out->encrypted_hop = enc;
    out->button = button;
    out->status_flags = status;
    out->counter = 0;
    out->decoded = false;
    return 0;
}

int pfx_frame_decrypt(pfx_rx_frame_t *frm, uint64_t crypt_key) {
    if (!frm) return -1;
    uint32_t plain = keeloq_decrypt(frm->encrypted_hop, crypt_key);
    /* Sanity check : discriminant should match 12 LSB of serial */
    uint16_t discrim = (plain >> 16) & 0xFFF;
    uint16_t expected = frm->serial & 0xFFF;
    if (discrim != expected) return -2; /* Wrong key */
    frm->counter = plain & 0xFFFF;
    frm->decoded = true;
    return 0;
}

void pfx_emit_burst(pfx_tx_state_t *st, uint8_t button, uint32_t n_frames, uint32_t duration_ms) {
    uint32_t delay_per_frame = duration_ms / (n_frames > 0 ? n_frames : 1);
    uint8_t frame[9];
    ESP_LOGI(TAG, "Burst start: %u frames over %u ms",
             (unsigned)n_frames, (unsigned)duration_ms);
    for (uint32_t i = 0; i < n_frames; i++) {
        pfx_frame_build(st, button, frame);
        cc1101_tx_ook_frame(frame, 66);
        st->counter++;
        if ((i & 0x0F) == 0) pfx_state_save(st);
        vTaskDelay(pdMS_TO_TICKS(delay_per_frame));
    }
    pfx_state_save(st);
    ESP_LOGI(TAG, "Burst done. Counter now = %u", (unsigned)st->counter);
}

void pfx_emit_command(pfx_tx_state_t *st, uint8_t button) {
    uint8_t frame[9];
    for (int i = 0; i < 3; i++) {  /* HCS301 typical = 3 repeats */
        pfx_frame_build(st, button, frame);
        cc1101_tx_ook_frame(frame, 66);
        st->counter++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    pfx_state_save(st);
}
