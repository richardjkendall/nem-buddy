#include "ui_setup.h"
#include "lvgl.h"

static lv_obj_t *s_status;

void ui_setup_show(const char *ap_ssid, const char *ap_pass, const char *portal_url) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 380, 380);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1b2027), 0);
    lv_obj_set_style_border_width(card, 0, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Wi-Fi setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text_fmt(body,
        "1. Join Wi-Fi:\n"
        "   %s\n"
        "   password: %s\n\n"
        "2. Open in a browser:\n"
        "   %s",
        ap_ssid, ap_pass, portal_url);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_18, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 4, 48);

    s_status = lv_label_create(card);
    lv_label_set_text(s_status, "Waiting for setup\xE2\x80\xA6");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void ui_setup_status(const char *msg) {
    if (s_status) lv_label_set_text(s_status, msg);
}
