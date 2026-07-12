#include "portal_http.h"
#include "net_creds.h"
#include "nem/provision.h"
#include "ui_setup.h"
#include "bsp/esp-bsp.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "portal";
static const wifi_ctrl_ap_t *s_aps;
static int s_apn;

/* Minimal HTML-escaper: writes a NUL-terminated escaped copy of src into
 * dst, never exceeding cap bytes. Escapes & < > " ' so untrusted text
 * (nearby SSIDs) and prior values can't break out of the markup. */
static void html_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        const char *rep = NULL;
        switch (src[i]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl >= cap) break;
            memcpy(dst + o, rep, rl);
            o += rl;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static esp_err_t send_form(httpd_req_t *req, const char *err) {
    net_creds_t cur;
    net_creds_load(&cur);   /* for prefill; ignore return */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>NEM Buddy setup</title><style>"
        "body{font-family:sans-serif;max-width:24rem;margin:2rem auto;padding:0 1rem}"
        "label{display:block;margin:.75rem 0 .2rem;font-weight:600}"
        "input,select{width:100%;padding:.5rem;font-size:1rem;box-sizing:border-box}"
        "button{margin-top:1.2rem;width:100%;padding:.7rem;font-size:1rem}"
        ".err{color:#b00;font-weight:600}</style><h2>NEM Buddy Wi-Fi setup</h2>");
    if (err) {
        httpd_resp_sendstr_chunk(req, "<p class=err>");
        httpd_resp_sendstr_chunk(req, err);
        httpd_resp_sendstr_chunk(req, "</p>");
    }
    httpd_resp_sendstr_chunk(req,
        "<form method=POST action=/save "
        "onsubmit=\"s.value=sel.value=='__other__'?oth.value:sel.value\">"
        "<input type=hidden name=ssid id=s>"
        "<label>Wi-Fi network</label>"
        "<select id=sel onchange=\"oth.style.display=this.value=='__other__'?'block':'none'\">");
    char row[256];
    for (int i = 0; i < s_apn; i++) {
        char esc[200];
        html_escape(s_aps[i].ssid, esc, sizeof esc);
        snprintf(row, sizeof row, "<option>%s</option>", esc);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req,
        "<option value=__other__>Other / hidden\xE2\x80\xA6</option></select>"
        "<input id=oth placeholder='hidden SSID' style='display:none'>"
        "<label>Password</label><input name=password type=password>");
    /* Emit each prefilled field with its value embedded in one buffer. Sending
     * an (escaped) value as its own chunk would send a zero-length chunk when
     * the value is empty, which in HTTP chunked encoding ends the response and
     * truncates the form. */
    char ebuf[800];
    char field[900];
    html_escape(cur.proxy_url, ebuf, sizeof ebuf);
    snprintf(field, sizeof field,
             "<label>Proxy URL</label><input name=proxy_url value='%s'>", ebuf);
    httpd_resp_sendstr_chunk(req, field);
    html_escape(cur.proxy_token, ebuf, sizeof ebuf);
    snprintf(field, sizeof field,
             "<label>Proxy token (optional)</label>"
             "<input name=proxy_token value='%s'>", ebuf);
    httpd_resp_sendstr_chunk(req, field);
    httpd_resp_sendstr_chunk(req, "<button>Save &amp; connect</button></form>");
    httpd_resp_sendstr_chunk(req, NULL);   /* end chunks */
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req) {
    return send_form(req, NULL);
}

static esp_err_t save_post(httpd_req_t *req) {
    static char body[1024];
    int total = 0, r;
    while ((r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(body) - 1) break;
    }
    if (r < 0) return ESP_FAIL;
    body[total] = '\0';

    net_creds_t f;   /* net_creds_t == nem_prov_form_t */
    if (!nem_provision_parse_form(body, (size_t)total, &f))
        return send_form(req, "Please pick or enter a Wi-Fi network.");
    if (!net_creds_save(&f))
        return send_form(req, "Could not save — please try again.");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;text-align:center;margin-top:3rem'>"
        "<h2>Saved \xE2\x80\x94 connecting\xE2\x80\xA6</h2>"
        "<p>NEM Buddy will restart and join your network.</p>");
    bsp_display_lock(-1);
    ui_setup_status("Saved - connecting...");
    bsp_display_unlock();
    ESP_LOGI(TAG, "creds saved; rebooting");
    vTaskDelay(pdMS_TO_TICKS(1200));   /* let the response flush */
    esp_restart();
    return ESP_OK;
}

/* Any unknown URL (OS captive-probe endpoints) -> redirect to the form. */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void portal_http_start(const wifi_ctrl_ap_t *aps, int ap_n) {
    s_aps = aps;
    s_apn = ap_n;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, redirect_404);
    ESP_LOGI(TAG, "portal HTTP server started");
}
