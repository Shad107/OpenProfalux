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
#include "esp_netif.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
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
    /* Ecoute dediee 15 s : on garde la PREMIERE trame valide, quel que soit le bouton.
     * C'est l'action choisie dans l'UI + le bouton physique presse qui determinent la
     * commande. Filtrer par valeur de bouton etait une erreur (codes reels differents de
     * ce qu'on supposait) : ca empechait d'apprendre stop et descente. */
    int n = radio_listen_once(15000, bits, SH_BITS_LEN - 1);
    if (n >= 60) {
        s_lrssi   = cc1101_get_rssi();
        strlcpy(s_lbits, bits, SH_BITS_LEN);
        s_lserial = bits_lsb(bits, 32, 28);
        s_lbtn    = (uint8_t)bits_lsb(bits, 60, 4);
        s_lready  = true;
        /* fait aussi apparaitre la trame captee dans le journal RF debug */
        shutters_on_rx(bits, s_lserial, s_lbtn, s_lrssi, bits_lsb(bits, 0, 32));
    }
    s_lactive = false;
    vTaskDelete(NULL);
}

/* ── Helpers HTTP ── */
static esp_err_t send_asset(httpd_req_t *r, const char *type, const uint8_t *s, const uint8_t *e) {
    httpd_resp_set_type(r, type);
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");   /* toujours revalider : pas d'UI perimee apres MAJ */
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
    static char buf[4096];
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
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":%d,\"tx_marc\":%d}", rc == 0 ? 1 : 0, g_tx_marc);
    httpd_resp_sendstr(r, resp);
    return ESP_OK;
}

/* ── /api/volet/delete : supprime un volet (permet de reapprendre proprement) ── */
static esp_err_t h_volet_delete(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    int rc = shutters_delete_volet(jstr(j, "id"));
    cJSON_Delete(j);
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

/* ── /api/learn/start + /poll ── */
static esp_err_t h_learn_start(httpd_req_t *r) {
    char *body = read_body(r); if (body) free(body);   /* le corps {action} n'est plus utilise cote firmware */
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
static esp_err_t h_learn_reassign(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    int rc = shutters_reassign(jstr(j, "id"), jstr(j, "from"), jstr(j, "to"));
    cJSON_Delete(j);
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}
static esp_err_t h_learn_adopt(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    const char *hs = jstr(j, "hop");
    uint32_t hop = hs ? (uint32_t)strtoul(hs, NULL, 16) : 0;
    int rc = shutters_adopt(jstr(j, "id"), jstr(j, "action"), jstr(j, "serial"), hop);
    cJSON_Delete(j);
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

static esp_err_t h_rf_replay(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(r, 400, "json");
    const char *hs = jstr(j, "hop");
    uint32_t hop = hs ? (uint32_t)strtoul(hs, NULL, 16) : 0;
    int rc = shutters_replay_frame(jstr(j, "serial"), hop);
    cJSON_Delete(j);
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
    char dev[32] = "", ssid[33] = "", uri[160] = "", user[128] = "", pass[256] = "";
    uint8_t logf = 0;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        cfg_get(h, "device", dev, sizeof(dev)); cfg_get(h, "wifi_ssid", ssid, sizeof(ssid));
        cfg_get(h, "mqtt_uri", uri, sizeof(uri)); cfg_get(h, "mqtt_user", user, sizeof(user));
        cfg_get(h, "mqtt_pass", pass, sizeof(pass));   /* longueur seulement, JAMAIS renvoye en clair */
        nvs_get_u8(h, "log_frames", &logf);
        nvs_close(h);
    }
    char out[512];
    snprintf(out, sizeof(out),
             "{\"device\":\"%s\",\"wifi_ssid\":\"%s\",\"mqtt_uri\":\"%s\",\"mqtt_user\":\"%s\","
             "\"mqtt_user_len\":%d,\"mqtt_pass_len\":%d,\"log_frames\":%d}",
             dev, ssid, uri, user, (int)strlen(user), (int)strlen(pass), logf ? 1 : 0);
    memset(pass, 0, sizeof(pass));   /* on n'oublie pas d'effacer le mdp de la pile */
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

/* ── /api/frames : export du dataset de trames (hops distincts par telecommande), chunke ── */
#define SH_MAX_HOPS_EXPORT 1024   /* aligne sur SH_MAX_HOPS (cap par telecommande) */
static esp_err_t h_frames(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=openprofalux-trames.json");
    static uint32_t hbuf[SH_MAX_HOPS_EXPORT];
    char serial[SH_SERIAL_LEN], name[SH_ID_LEN], line[600];
    httpd_resp_sendstr_chunk(r, "{\"trames\":{");
    int nr = shutters_remote_count();
    for (int i = 0; i < nr; i++) {
        int nh = shutters_remote_dump(i, serial, sizeof(serial), name, sizeof(name), hbuf, SH_MAX_HOPS_EXPORT);
        if (nh < 0) continue;
        int p = snprintf(line, sizeof(line), "%s\"%s\":{\"name\":\"%s\",\"count\":%d,\"hops\":[",
                         i ? "," : "", serial, name, nh);
        httpd_resp_send_chunk(r, line, p);
        for (int k = 0; k < nh; ) {
            p = 0;
            while (k < nh && p < (int)sizeof(line) - 16) {
                p += snprintf(line + p, sizeof(line) - p, "%s\"0x%08X\"", k ? "," : "", (unsigned)hbuf[k]);
                k++;
            }
            httpd_resp_send_chunk(r, line, p);
        }
        httpd_resp_sendstr_chunk(r, "]}");
    }
    httpd_resp_sendstr_chunk(r, "}}");
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

/* ── /api/rf : les 300 dernieres trames du ring (plus recente d'abord), pour l'onglet RF ── */
static esp_err_t h_rf(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char serial[SH_SERIAL_LEN], line[160];
    uint8_t button; uint32_t hop, t; int8_t rssi;
    int cap = shutters_rf_capacity(), first = 1;
    for (int k = 0; k < cap; k++) {
        if (shutters_rf_get(k, serial, sizeof(serial), &button, &hop, &t, &rssi) != 0) continue;
        int p = snprintf(line, sizeof(line),
            "%s{\"serial\":\"%s\",\"button\":\"%X\",\"hop\":\"%08X\",\"rssi\":%d,\"t\":%u}",
            first ? "" : ",", serial, button, (unsigned)hop, rssi, (unsigned)t);
        httpd_resp_send_chunk(req, line, p);
        first = 0;
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_send_chunk(req, NULL, 0);
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

/* ── /api/mqtt/discover : cherche un broker MQTT en mDNS (_mqtt._tcp) ── */
static esp_err_t h_mqtt_discover(httpd_req_t *r) {
    mdns_result_t *res = NULL;
    char out[128] = "{}";
    bool found = false;
    if (mdns_query_ptr("_mqtt", "_tcp", 3000, 5, &res) == ESP_OK) {
        for (mdns_result_t *cur = res; cur && !found; cur = cur->next) {
            for (mdns_ip_addr_t *a = cur->addr; a && !found; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    char ip[16]; esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip, sizeof(ip));
                    snprintf(out, sizeof(out), "{\"uri\":\"mqtt://%s:%u\"}", ip, cur->port);
                    found = true;
                }
            }
        }
    }
    if (res) mdns_query_results_free(res);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, out);
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

/* ── /api/ota/pull : telecharge + flashe une image depuis une URL HTTPS (release GitHub) ── */
static void ota_pull_bg_task(void *arg) {
    char *url = (char *)arg;
    esp_err_t err = ota_pull_from_url(url);
    if (err != ESP_OK) ESP_LOGE(TAG, "ota_pull_from_url(%s) KO: %s", url, esp_err_to_name(err));
    free(url);
    vTaskDelete(NULL);
}
static esp_err_t h_ota_pull(httpd_req_t *r) {
    char *body = read_body(r); if (!body) return httpd_resp_send_err(r, 400, "body");
    cJSON *j = cJSON_Parse(body); free(body);
    const char *url = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "url")) : NULL;
    char *dup = (url && !strncmp(url, "https://", 8)) ? strdup(url) : NULL;   /* HTTPS uniquement */
    if (j) cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    if (dup && xTaskCreate(ota_pull_bg_task, "ota_pull", 8192, dup, 5, NULL) == pdPASS)
        return httpd_resp_sendstr(r, "{\"ok\":1}");
    free(dup);
    return httpd_resp_sendstr(r, "{\"ok\":0}");
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t *)) {
    httpd_uri_t u = { .uri = uri, .method = m, .handler = h };
    httpd_register_uri_handler(s, &u);
}

void web_ui_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 28;   /* > nb d'endpoints reels (sinon "no slots left" -> /api/... non enregistres) */
    cfg.stack_size = 6144;   /* esp_ota_end() consomme la pile en fin d'upload */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start KO"); return; }
    reg(s, "/",            HTTP_GET,  h_index);
    reg(s, "/style.css",   HTTP_GET,  h_css);
    reg(s, "/app.js",      HTTP_GET,  h_js);
    reg(s, "/api/status",  HTTP_GET,  h_status);
    reg(s, "/api/shutter", HTTP_POST, h_shutter);
    reg(s, "/api/volet/delete", HTTP_POST, h_volet_delete);
    reg(s, "/api/learn/start",  HTTP_POST, h_learn_start);
    reg(s, "/api/learn/poll",   HTTP_GET,  h_learn_poll);
    reg(s, "/api/learn/assign", HTTP_POST, h_learn_assign);
    reg(s, "/api/learn/reassign", HTTP_POST, h_learn_reassign);
    reg(s, "/api/learn/adopt", HTTP_POST, h_learn_adopt);
    reg(s, "/api/rf/replay", HTTP_POST, h_rf_replay);
    reg(s, "/api/calibrate", HTTP_POST, h_calibrate);
    reg(s, "/api/remote",    HTTP_POST, h_remote);
    reg(s, "/api/config",    HTTP_GET,  h_config_get);
    reg(s, "/api/config",    HTTP_POST, h_config_post);
    reg(s, "/api/ota/status",   HTTP_GET,  h_ota_status);
    reg(s, "/api/ota/upload",   HTTP_POST, h_ota_upload);
    reg(s, "/api/ota/rollback", HTTP_POST, h_ota_rollback);
    reg(s, "/api/ota/pull",     HTTP_POST, h_ota_pull);
    reg(s, "/api/frames",       HTTP_GET,  h_frames);
    reg(s, "/api/rf",           HTTP_GET,  h_rf);
    reg(s, "/api/backup",       HTTP_GET,  h_backup);
    reg(s, "/api/restore",      HTTP_POST, h_restore);
    reg(s, "/api/mqtt/discover", HTTP_GET, h_mqtt_discover);
    ESP_LOGI(TAG, "HTTP UI demarree");
}
