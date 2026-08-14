/*
 * web_ui.c — serveur HTTP OpenProfalux : sert l'UI embarquee + endpoints sous /api.
 */
#include "web_ui.h"
#include "shutters.h"
#include "radio.h"
#include "ota.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "cc1101.h"

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "web_ui";

/* Fichiers UI embarques (voir EMBED_FILES du CMakeLists). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");

/* ── Etat apprentissage ── */
static char           s_lbits[SH_BITS_LEN];
static uint32_t       s_lserial;
static uint8_t        s_lbtn;
static int8_t         s_lrssi;
static volatile bool  s_lready;
static volatile bool  s_lactive;

static uint32_t bits_lsb(const char *b, int from, int len) {
    uint32_t v = 0;
    for (int i = 0; i < len; i++) if (b[from + i] == '1') v |= (1u << i);
    return v;
}

static void learn_task(void *arg) {
    (void)arg;
    char bits[SH_BITS_LEN];
    /* ecoute dediee via l'arbitre : suspend le RX permanent, prend la radio, ecoute 15 s */
    int n = radio_listen_once(15000, bits, SH_BITS_LEN - 1);
    if (n >= 60) {
        strlcpy(s_lbits, bits, SH_BITS_LEN);
        s_lserial = bits_lsb(bits, 32, 28);
        s_lbtn    = (uint8_t)bits_lsb(bits, 60, 4);
        s_lrssi   = cc1101_get_rssi();
        s_lready  = true;
    }
    s_lactive = false;
    vTaskDelete(NULL);
}

/* ── Helpers HTTP ── */
static esp_err_t send_asset(httpd_req_t *r, const char *type, const uint8_t *s, const uint8_t *e) {
    httpd_resp_set_type(r, type);
    return httpd_resp_send(r, (const char *)s, e - s);
}
static char *read_body(httpd_req_t *r) {
    int len = r->content_len;
    if (len <= 0 || len > 8192) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int k = httpd_req_recv(r, buf + got, len - got);
        if (k <= 0) { free(buf); return NULL; }
        got += k;
    }
    buf[len] = 0;
    return buf;
}
static const char *jstr(cJSON *o, const char *k) {
    cJSON *i = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

/* ── Handlers statiques ── */
static esp_err_t h_index(httpd_req_t *r) { return send_asset(r, "text/html",       index_html_start, index_html_end); }
static esp_err_t h_css  (httpd_req_t *r) { return send_asset(r, "text/css",        style_css_start,  style_css_end);  }
static esp_err_t h_js   (httpd_req_t *r) { return send_asset(r, "application/javascript", app_js_start, app_js_end);  }

/* ── /api/status ── */
static esp_err_t h_status(httpd_req_t *r) {
    static char buf[3072];
    int n = shutters_status_json(buf, sizeof(buf));
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n > 0 ? n : 0);
}

/* ── /api/shutter ── */
static esp_err_t h_shutter(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    const char *id = jstr(j, "id"), *cmd = jstr(j, "cmd");
    cJSON *val = cJSON_GetObjectItem(j, "value");
    int rc = (id && cmd) ? shutters_cmd(id, cmd, val ? (int)val->valuedouble : 0) : -1;
    cJSON_Delete(j);
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

/* ── /api/learn/start + /poll ── */
static esp_err_t h_learn_start(httpd_req_t *r) {
    if (!s_lactive) { s_lready = false; s_lactive = true; xTaskCreate(learn_task, "learn", 4096, NULL, 6, NULL); }
    httpd_resp_sendstr(r, "{\"ok\":1}");
    return ESP_OK;
}
static esp_err_t h_learn_poll(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    if (!s_lready) { httpd_resp_sendstr(r, "{}"); return ESP_OK; }
    char out[SH_BITS_LEN + 96];
    snprintf(out, sizeof(out), "{\"bits\":\"%s\",\"serial\":\"0x%07X\",\"button\":\"%X\",\"rssi\":%d}",
             s_lbits, (unsigned)s_lserial, s_lbtn, s_lrssi);
    httpd_resp_sendstr(r, out);
    return ESP_OK;
}
static esp_err_t h_learn_assign(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    int rc = shutters_learn_assign(jstr(j, "id"), jstr(j, "action"), jstr(j, "bits"));
    cJSON_Delete(j); s_lready = false;
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

/* ── /api/calibrate + /api/remote ── */
static esp_err_t h_calibrate(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    int rc = shutters_calibrate(jstr(j, "id"),
        (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "travel_up_ms")),
        (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "travel_down_ms")));
    cJSON_Delete(j);
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}
static esp_err_t h_remote(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    int rc = shutters_remote_name(jstr(j, "serial"), jstr(j, "name") ?: "");
    cJSON_Delete(j);
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

/* ── /api/config : Wi-Fi + MQTT + nom (namespace NVS "cfg", lu par main.c) ── */
static void cfg_get(nvs_handle_t h, const char *k, char *out, size_t cap) {
    size_t sz = cap; out[0] = 0; nvs_get_str(h, k, out, &sz);
}
static esp_err_t h_config_get(httpd_req_t *r) {
    char dev[32] = "", ssid[32] = "", uri[96] = "", user[48] = "";
    uint8_t logf = 0;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        cfg_get(h, "device", dev, sizeof(dev)); cfg_get(h, "wifi_ssid", ssid, sizeof(ssid));
        cfg_get(h, "mqtt_uri", uri, sizeof(uri)); cfg_get(h, "mqtt_user", user, sizeof(user));
        nvs_get_u8(h, "log_frames", &logf);
        nvs_close(h);
    }
    char out[352];
    snprintf(out, sizeof(out),
             "{\"device\":\"%s\",\"wifi_ssid\":\"%s\",\"mqtt_uri\":\"%s\",\"mqtt_user\":\"%s\",\"log_frames\":%d}",
             dev, ssid, uri, user, logf ? 1 : 0);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, out);
    return ESP_OK;
}
static void cfg_set_if(nvs_handle_t h, cJSON *j, const char *field, const char *key) {
    const char *v = jstr(j, field);
    if (v) nvs_set_str(h, key, v);
}
static esp_err_t h_config_post(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        cfg_set_if(h, j, "device", "device");
        cfg_set_if(h, j, "wifi_ssid", "wifi_ssid"); cfg_set_if(h, j, "wifi_pass", "wifi_pass");
        cfg_set_if(h, j, "mqtt_uri", "mqtt_uri"); cfg_set_if(h, j, "mqtt_user", "mqtt_user");
        cfg_set_if(h, j, "mqtt_pass", "mqtt_pass");
        cJSON *lf = cJSON_GetObjectItem(j, "log_frames");
        if (cJSON_IsBool(lf) || cJSON_IsNumber(lf)) {
            uint8_t on = cJSON_IsTrue(lf) || (cJSON_IsNumber(lf) && lf->valuedouble != 0);
            nvs_set_u8(h, "log_frames", on);
            shutters_set_log_frames(on);   /* prise en compte immediate (le RX permanent s'active au reboot) */
        }
        nvs_commit(h); nvs_close(h);
    }
    bool reboot = cJSON_IsTrue(cJSON_GetObjectItem(j, "reboot"));
    cJSON_Delete(j);
    httpd_resp_sendstr(r, "{\"ok\":1}");
    if (reboot) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
    return ESP_OK;
}

/* ── /api/backup (export) + /api/restore (import) : telecommandes + trames de reference ── */
static esp_err_t h_backup(httpd_req_t *r) {
    static char buf[8192];
    int n = shutters_export_json(buf, sizeof(buf));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=openprofalux-backup.json");
    return httpd_resp_send(r, buf, n > 0 ? n : 0);
}
static esp_err_t h_restore(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    int rc = shutters_import_json(body);
    free(body);
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

/* ── /api/ota/status + /api/ota/upload ── */
static esp_err_t h_ota_status(httpd_req_t *r) {
    ota_status_t st; ota_get_status(&st);
    char out[256];
    snprintf(out, sizeof(out), "{\"state\":%d,\"total\":%u,\"written\":%u,\"msg\":\"%s\",\"version\":\"%s\"}",
             st.state, (unsigned)st.total_bytes, (unsigned)st.written_bytes, st.message, st.active_version);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, out);
    return ESP_OK;
}
static esp_err_t h_ota_upload(httpd_req_t *r) {
    int total = r->content_len;
    if (total <= 0) return httpd_resp_send_err(r, 400, "empty");
    if (ota_upload_begin(total) != ESP_OK) return httpd_resp_send_err(r, 500, "ota begin");
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM; esp_wifi_get_ps(&ps); esp_wifi_set_ps(WIFI_PS_NONE);
    char *buf = malloc(1460);
    if (!buf) { esp_wifi_set_ps(ps); ota_upload_abort(); return httpd_resp_send_err(r, 500, "malloc"); }
    int rem = total, cc = 0; esp_err_t res = ESP_OK;
    while (rem > 0) {
        int k = httpd_req_recv(r, buf, MIN(rem, 1460));
        if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (k <= 0 || ota_upload_data(buf, k) != ESP_OK) { res = ESP_FAIL; break; }
        rem -= k;
        if ((++cc & 0x0F) == 0) vTaskDelay(1);
    }
    esp_wifi_set_ps(ps); free(buf);
    if (res != ESP_OK) { ota_upload_abort(); return httpd_resp_send_err(r, 500, "upload"); }
    httpd_resp_sendstr(r, "{\"ok\":1}");
    ota_upload_end();   /* vérifie + reboot */
    return ESP_OK;
}
/* ── /api/ota/rollback : rebascule sur la partition precedente (comme OpenXtraflame) ── */
static esp_err_t h_ota_rollback(httpd_req_t *r) {
    /* On repond AVANT : ota_rollback() reboote immediatement si l'etat le permet. */
    httpd_resp_sendstr(r, "{\"ok\":1}");
    vTaskDelay(pdMS_TO_TICKS(300));
    ota_rollback();   /* esp_ota_mark_app_invalid_rollback_and_reboot : reboote, ou echoue si pas d'image precedente valide */
    return ESP_OK;
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t *)) {
    httpd_uri_t u = { .uri = uri, .method = m, .handler = h };
    httpd_register_uri_handler(s, &u);
}

void web_ui_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 18;
    cfg.stack_size = 6144;   /* esp_ota_end() consomme la pile en fin d'upload */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start KO"); return; }
    reg(s, "/",            HTTP_GET,  h_index);
    reg(s, "/style.css",   HTTP_GET,  h_css);
    reg(s, "/app.js",      HTTP_GET,  h_js);
    reg(s, "/api/status",  HTTP_GET,  h_status);
    reg(s, "/api/shutter", HTTP_POST, h_shutter);
    reg(s, "/api/learn/start",  HTTP_POST, h_learn_start);
    reg(s, "/api/learn/poll",   HTTP_GET,  h_learn_poll);
    reg(s, "/api/learn/assign", HTTP_POST, h_learn_assign);
    reg(s, "/api/calibrate", HTTP_POST, h_calibrate);
    reg(s, "/api/remote",    HTTP_POST, h_remote);
    reg(s, "/api/config",    HTTP_GET,  h_config_get);
    reg(s, "/api/config",    HTTP_POST, h_config_post);
    reg(s, "/api/ota/status",   HTTP_GET,  h_ota_status);
    reg(s, "/api/ota/upload",   HTTP_POST, h_ota_upload);
    reg(s, "/api/ota/rollback", HTTP_POST, h_ota_rollback);
    reg(s, "/api/backup",       HTTP_GET,  h_backup);
    reg(s, "/api/restore",      HTTP_POST, h_restore);
    ESP_LOGI(TAG, "HTTP UI demarree");
}
