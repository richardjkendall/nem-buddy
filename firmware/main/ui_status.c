/* firmware/main/ui_status.c */
#include "ui_status.h"
#include "ui_theme.h"
#include "axp2101.h"
#include "net_creds.h"
#include "wifi_ctrl.h"
#include "data_task.h"
#include "nem/battery.h"
#include "nem/timefmt.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_mac.h"

/* Safe area: this panel clips ~20px more on the right than the left. */
#define SX 20
#define SY 20
#define HDR_H 24
#define GAP 9
#define BOX_W 205
#define BOX_H 199
#define GY (SY + HDR_H + GAP)

static struct {
    lv_obj_t *root;
    lv_obj_t *batt_pct, *batt_bar, *batt_chg, *batt_mv;
    lv_obj_t *net_ssid, *net_rssi, *net_ip;
    lv_obj_t *h_last, *h_err, *h_up;
    lv_timer_t *timer;
    bool open;
} s;

static lv_obj_t *make_box(lv_obj_t *parent, int col, int row, const char *title)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BOX_W, BOX_H);
    lv_obj_set_pos(b, SX + col * (BOX_W + GAP), GY + row * (BOX_H + GAP));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0e0e12), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_pad_all(b, 13, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(t, 0, 0);
    return b;
}

/* A left-aligned "key   value" row at a given y inside a box. */
static lv_obj_t *make_row(lv_obj_t *box, int y, const char *key)
{
    lv_obj_t *k = lv_label_create(box);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(k, 0, y);

    lv_obj_t *v = lv_label_create(box);
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_color(v, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(v, 110);
    lv_obj_set_pos(v, BOX_W - 26 - 110, y);
    return v;
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    if (!s.open) return;

    /* ---- battery ---- */
    axp2101_state_t b;
    char buf[32];
    if (axp2101_read(&b) == ESP_OK && b.present) {
        uint8_t pct = b.percent;
        /* Fuel gauge can read 0 on a cell it hasn't characterised; fall back to
         * the voltage curve rather than claim a flat battery. */
        if (pct == 0 && b.millivolts > 0) pct = nem_batt_pct_from_mv(b.millivolts);

        snprintf(buf, sizeof buf, "%u", pct);
        lv_label_set_text(s.batt_pct, buf);
        lv_obj_set_style_text_color(s.batt_pct, NEM_C_WHITE, 0);
        lv_obj_set_width(s.batt_bar, (int32_t)LV_PCT(pct > 100 ? 100 : pct));
        lv_obj_set_style_bg_color(s.batt_bar,
            pct <= 15 ? NEM_C_RED : (pct <= 35 ? NEM_C_AMBER : NEM_C_GREEN), 0);

        lv_label_set_text(s.batt_chg, b.charging ? "Charging" : "On battery");
        lv_obj_set_style_text_color(s.batt_chg, b.charging ? NEM_C_GREEN : NEM_C_MUTED, 0);
        snprintf(buf, sizeof buf, "%u.%02u V", b.millivolts / 1000, (b.millivolts % 1000) / 10);
        lv_label_set_text(s.batt_mv, buf);
    } else {
        lv_label_set_text(s.batt_pct, "n/a");
        lv_obj_set_style_text_color(s.batt_pct, NEM_C_MUTED, 0);
        lv_obj_set_width(s.batt_bar, 0);
        lv_label_set_text(s.batt_chg, "No battery");
        lv_obj_set_style_text_color(s.batt_chg, NEM_C_MUTED, 0);
        lv_label_set_text(s.batt_mv, "USB power");
    }

    /* ---- network ---- */
    wifi_ctrl_sta_info_t w;
    wifi_ctrl_sta_info(&w);
    if (w.connected) {
        lv_label_set_text(s.net_ssid, w.ssid);
        lv_obj_set_style_text_color(s.net_ssid, NEM_C_WHITE, 0);
        snprintf(buf, sizeof buf, "%d dBm", w.rssi);
        lv_label_set_text(s.net_rssi, buf);
        lv_obj_set_style_text_color(s.net_rssi, w.rssi > -67 ? NEM_C_GREEN : NEM_C_AMBER, 0);
        lv_label_set_text(s.net_ip, w.ip[0] ? w.ip : "-");
    } else {
        lv_label_set_text(s.net_ssid, "disconnected");
        lv_obj_set_style_text_color(s.net_ssid, NEM_C_RED, 0);
        lv_label_set_text(s.net_rssi, "-");
        lv_obj_set_style_text_color(s.net_rssi, NEM_C_MUTED, 0);
        lv_label_set_text(s.net_ip, "-");
    }

    /* ---- data health ---- */
    data_health_t h;
    data_task_health(&h);
    nem_fmt_ago(h.last_ok_s < 0 ? -1 : h.uptime_s - h.last_ok_s, buf, sizeof buf);
    lv_label_set_text(s.h_last, buf);
    lv_obj_set_style_text_color(s.h_last, h.last_ok_s < 0 ? NEM_C_MUTED : NEM_C_GREEN, 0);

    snprintf(buf, sizeof buf, "%lu", (unsigned long)h.consec_errors);
    lv_label_set_text(s.h_err, buf);
    lv_obj_set_style_text_color(s.h_err, h.consec_errors ? NEM_C_RED : NEM_C_WHITE, 0);

    long long up = h.uptime_s;
    if (up < 3600) snprintf(buf, sizeof buf, "%llom %llos", up / 60, up % 60);
    else           snprintf(buf, sizeof buf, "%lloh %llom", up / 3600, (up % 3600) / 60);
    lv_label_set_text(s.h_up, buf);
}

static void build(void)
{
    /* layer_top so status sits above the drill-in overlay without touching it */
    s.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.root, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s.root, 0, 0);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s.root);
    lv_label_set_text(hdr, "DEVICE STATUS");
    lv_obj_set_style_text_color(hdr, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(hdr, SX, SY);

    /* ---- battery box ---- */
    lv_obj_t *bb = make_box(s.root, 0, 0, "BATTERY");
    s.batt_pct = lv_label_create(bb);
    lv_obj_set_style_text_font(s.batt_pct, &lv_font_montserrat_38, 0);
    lv_obj_set_style_text_color(s.batt_pct, NEM_C_WHITE, 0);
    lv_obj_set_pos(s.batt_pct, 0, 24);

    lv_obj_t *pc = lv_label_create(bb);
    lv_label_set_text(pc, "%");
    lv_obj_set_style_text_color(pc, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(pc, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(pc, 84, 44);

    lv_obj_t *track = lv_obj_create(bb);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, BOX_W - 26, 11);
    lv_obj_set_pos(track, 0, 80);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x1c1c20), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 6, 0);

    /* Explicit LV_PCT width from the data — never flex-grow on this board. */
    s.batt_bar = lv_obj_create(track);
    lv_obj_remove_style_all(s.batt_bar);
    lv_obj_set_size(s.batt_bar, 0, 11);
    lv_obj_set_pos(s.batt_bar, 0, 0);
    lv_obj_set_style_bg_color(s.batt_bar, NEM_C_GREEN, 0);
    lv_obj_set_style_bg_opa(s.batt_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s.batt_bar, 6, 0);

    s.batt_chg = lv_label_create(bb);
    lv_obj_set_style_text_font(s.batt_chg, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s.batt_chg, 0, 100);
    s.batt_mv = lv_label_create(bb);
    lv_obj_set_style_text_font(s.batt_mv, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s.batt_mv, NEM_C_MUTED, 0);
    lv_obj_set_pos(s.batt_mv, 0, 122);

    /* ---- network box ---- */
    lv_obj_t *nb = make_box(s.root, 1, 0, "NETWORK");
    s.net_ssid = lv_label_create(nb);
    lv_obj_set_style_text_font(s.net_ssid, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(s.net_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s.net_ssid, BOX_W - 26);
    lv_obj_set_pos(s.net_ssid, 0, 26);
    s.net_rssi = make_row(nb, 66, "Signal");
    s.net_ip   = make_row(nb, 92, "IP");

    /* ---- data health box ---- */
    lv_obj_t *hb = make_box(s.root, 0, 1, "DATA HEALTH");
    s.h_last = make_row(hb, 30, "Last fetch");
    s.h_err  = make_row(hb, 56, "Errors");
    s.h_up   = make_row(hb, 82, "Uptime");

    /* ---- device box (static values) ---- */
    lv_obj_t *db = make_box(s.root, 1, 1, "DEVICE");
    net_creds_t c;
    net_creds_load(&c);

    lv_obj_t *v_id = make_row(db, 30, "ID");
    lv_label_set_text(v_id, c.device_id[0] ? c.device_id : "-");

    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *v_fw = make_row(db, 56, "F/W");
    lv_label_set_text(v_fw, app ? app->version : "-");

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mbuf[16];
    snprintf(mbuf, sizeof mbuf, "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    lv_obj_t *v_mac = make_row(db, 82, "MAC");
    lv_label_set_text(v_mac, mbuf);

    /* Proxy host only — the full URL will not fit legibly in a quarter panel. */
    lv_obj_t *host = lv_label_create(db);
    const char *u = c.proxy_url;
    if (strncmp(u, "http://", 7) == 0) u += 7;
    else if (strncmp(u, "https://", 8) == 0) u += 8;
    char hbuf[48];
    strlcpy(hbuf, u[0] ? u : "-", sizeof hbuf);
    char *slash = strchr(hbuf, '/');
    if (slash) *slash = '\0';
    lv_label_set_text(host, hbuf);
    lv_obj_set_style_text_color(host, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(host, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(host, LV_LABEL_LONG_DOT);
    lv_obj_set_width(host, BOX_W - 26);
    lv_obj_set_pos(host, 0, 112);
}

void ui_status_toggle(void)
{
    if (s.open) {
        if (s.timer) { lv_timer_del(s.timer); s.timer = NULL; }
        if (s.root)  { lv_obj_del(s.root);    s.root  = NULL; }
        s.open = false;
        return;
    }
    build();
    s.open = true;
    refresh(NULL);                       /* paint immediately, don't wait 2s */
    s.timer = lv_timer_create(refresh, 2000, NULL);
}

bool ui_status_is_open(void) { return s.open; }
