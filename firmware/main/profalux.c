#include "profalux.h"
#include "keeloq.h"
#include "cc1101.h"
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
            ESP_LOGI(TAG, "Loaded state: serial=0x%08X counter=%u", st->serial, st->counter);
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
    /* Bits 32-5: serial 28-bit */
    frame[4] = (st->serial >> 20) & 0xFF;
    frame[5] = (st->serial >> 12) & 0xFF;
    frame[6] = (st->serial >>  4) & 0xFF;
    frame[7] = ((st->serial & 0xF) << 4) | (button & 0xF);
    /* Bits 4-3: button (already in frame[7]) */
    /* Bits 2-1: status flags = 0 (repeat=0, batt_low=0) */
    frame[8] = 0;
}

int pfx_frame_parse(const uint8_t frame[9], pfx_rx_frame_t *out) {
    if (!frame || !out) return -1;
    uint32_t enc = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16)
                 | ((uint32_t)frame[2] <<  8) | ((uint32_t)frame[3]);
    uint32_t serial = ((uint32_t)frame[4] << 20) | ((uint32_t)frame[5] << 12)
                    | ((uint32_t)frame[6] <<  4) | ((uint32_t)(frame[7] >> 4) & 0x0F);
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
    ESP_LOGI(TAG, "Burst start: %u frames over %u ms", n_frames, duration_ms);
    for (uint32_t i = 0; i < n_frames; i++) {
        pfx_frame_build(st, button, frame);
        cc1101_tx_ook_frame(frame, 66);
        st->counter++;
        if ((i & 0x0F) == 0) pfx_state_save(st);
        vTaskDelay(pdMS_TO_TICKS(delay_per_frame));
    }
    pfx_state_save(st);
    ESP_LOGI(TAG, "Burst done. Counter now = %u", st->counter);
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
