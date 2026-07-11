#include "ui_dashboard.h"
#include "ui_theme.h"

/* TODO(plan3): all values below are placeholders until the core data layer is wired in.
 * Colours are stored as raw hex (uint32_t) because lv_color_hex() is not a
 * compile-time constant and cannot appear in a file-scope initializer. */
typedef struct { const char *name; int price; uint32_t color_hex; } region_chip_t;

static const region_chip_t k_ribbon[] = {
    { "NSW", 118, 0xe8e8ee },
    { "QLD",  76, 0xe8e8ee },
    { "SA",  -18, 0x37d67a },   /* negative -> green */
    { "TAS",  64, 0xe8e8ee },
};

/* One generation-mix segment: proportional weight + colour (hex). */
typedef struct { int pct; uint32_t color_hex; } mix_seg_t;
static const mix_seg_t k_mix[] = {
    { 32, 0x5a5a5a }, { 18, 0xe0a23b }, { 22, 0x37d67a },
    { 16, 0xffd23f }, { 12, 0x4a9eff },
};

static void make_metric(lv_obj_t *parent, const char *label, const char *value,
                        lv_color_t value_color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, value_color, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, y + 18);
}

void ui_dashboard_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    /* Safe-area inset: the AMOLED panel has rounded corners that clip content. */
    lv_obj_set_style_pad_all(parent, 20, 0);

    /* ---- Top status row (centered to dodge both top corners) ---- */
    lv_obj_t *status = lv_label_create(parent);
    lv_label_set_text(status, "NEM   LIVE");
    lv_obj_set_style_text_color(status, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_14, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);

    /* ---- Hero: region + big price ---- */
    lv_obj_t *region = lv_label_create(parent);
    lv_label_set_text(region, "VICTORIA");
    lv_obj_set_style_text_color(region, NEM_C_BLUE, 0);
    lv_obj_set_style_text_font(region, &lv_font_montserrat_26, 0);
    lv_obj_align(region, LV_ALIGN_TOP_LEFT, 0, 30);

    lv_obj_t *price = lv_label_create(parent);
    lv_label_set_text(price, "$92");   /* TODO(plan3) */
    lv_obj_set_style_text_color(price, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(price, &lv_font_montserrat_48, 0);
    lv_obj_align(price, LV_ALIGN_TOP_LEFT, 0, 62);

    lv_obj_t *unit = lv_label_create(parent);
    lv_label_set_text(unit, "$/MWh  " LV_SYMBOL_DOWN " 14%");
    lv_obj_set_style_text_color(unit, NEM_C_GREEN, 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_16, 0);
    lv_obj_align(unit, LV_ALIGN_TOP_LEFT, 0, 122);

    make_metric(parent, "DEMAND", "6,240 MW", NEM_C_WHITE, 34);
    make_metric(parent, "RENEWABLES", "41%", NEM_C_GREEN, 82);

    /* ---- Generation-mix stacked bar ---- */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 440, 12);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < sizeof(k_mix) / sizeof(k_mix[0]); i++) {
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, LV_PCT(100));
        lv_obj_set_flex_grow(seg, k_mix[i].pct); /* proportional width */
        lv_obj_set_style_bg_color(seg, lv_color_hex(k_mix[i].color_hex), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }

    /* ---- Bottom ribbon: 4 region chips (lifted off the bottom corners) ---- */
    lv_obj_t *ribbon = lv_obj_create(parent);
    lv_obj_remove_style_all(ribbon);
    lv_obj_set_size(ribbon, 440, 68);
    lv_obj_align(ribbon, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(ribbon, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ribbon, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ribbon, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(k_ribbon) / sizeof(k_ribbon[0]); i++) {
        lv_obj_t *chip = lv_obj_create(ribbon);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, 102, 64);
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *nm = lv_label_create(chip);
        lv_label_set_text(nm, k_ribbon[i].name);
        lv_obj_set_style_text_color(nm, NEM_C_MUTED, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);

        lv_obj_t *pr = lv_label_create(chip);
        lv_label_set_text_fmt(pr, "$%d", k_ribbon[i].price);
        lv_obj_set_style_text_color(pr, lv_color_hex(k_ribbon[i].color_hex), 0);
        lv_obj_set_style_text_font(pr, &lv_font_montserrat_20, 0);
    }
}
