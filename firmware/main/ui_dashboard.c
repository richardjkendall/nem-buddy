#include "ui_dashboard.h"
#include "ui_theme.h"
#include "ui_drill.h"
#include "nem/config.h"
#include <stdio.h>

#define RIBBON_MAX (NEM_REGION_COUNT - 1)

/* fuel colours in nem_fuel_t order: COAL,GAS,HYDRO,WIND,SOLAR,BATTERY,OTHER */
static const uint32_t k_fuel_hex[NEM_FUEL_COUNT] = {
    0x5a5a5a, 0xe0a23b, 0x4a9eff, 0x37d67a, 0xffd23f, 0xb06bff, 0x8a8a92
};
static const char *const k_fuel_name[NEM_FUEL_COUNT] = {
    "Coal", "Gas", "Hydro", "Wind", "Solar", "Battery", "Other"
};

static struct {
    lv_obj_t *region, *price, *unit, *demand_val, *renew_val;
    lv_obj_t *seg[NEM_FUEL_COUNT];
    lv_obj_t *tf_sw[3], *tf_lbl[3];    /* top-fuels: swatch + "Name NN%" */
    lv_obj_t *netflow;                 /* net interconnector flow summary */
    lv_obj_t *chip[RIBBON_MAX];        /* chip container per ribbon slot */
    lv_obj_t *chip_name[RIBBON_MAX];
    lv_obj_t *chip_price[RIBBON_MAX];
    int chip_n;
    nem_region_t hero;
    nem_snapshot_t snap;
    nem_region_mix_t mix;
    bool have_data;
} d;

static void render(void);

static lv_color_t price_color(double p)
{
    if (p < 0)    return NEM_C_GREEN;
    if (p > 1000) return NEM_C_RED;
    if (p > 300)  return NEM_C_AMBER;
    return NEM_C_WHITE;
}

static lv_obj_t *mk(lv_obj_t *p, const lv_font_t *f, lv_color_t col, lv_align_t a, int x, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, a, x, y);
    return l;
}

static int round_dollar(double p) { return (int)(p + (p < 0 ? -0.5 : 0.5)); }

/* Which region does ribbon slot `ci` show? The ribbon lists every region
 * except the hero, in enum order. */
static nem_region_t ribbon_region(int ci)
{
    int seen = 0;
    for (int r = 0; r < NEM_REGION_COUNT; r++) {
        if (r == d.hero) continue;
        if (seen == ci) return (nem_region_t)r;
        seen++;
    }
    return d.hero;
}

static void chip_clicked_cb(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_target(e);
    for (int i = 0; i < d.chip_n; i++) {
        if (d.chip[i] == chip) {
            d.hero = ribbon_region(i);
            render();
            break;
        }
    }
}

static void hero_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_drill_show(ui_dashboard_hero_region());
}

void ui_dashboard_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 20, 0);

    nem_config_t cfg; nem_config_defaults(&cfg);
    d.hero = cfg.home_region;
    d.have_data = false;

    lv_obj_t *status = mk(parent, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(status, "NEM   LIVE");

    d.region = mk(parent, &lv_font_montserrat_26, NEM_C_BLUE, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_text(d.region, "—");
    d.price  = mk(parent, &lv_font_montserrat_48, NEM_C_WHITE, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_label_set_text(d.price, "—");
    d.unit   = mk(parent, &lv_font_montserrat_16, NEM_C_MUTED, LV_ALIGN_TOP_LEFT, 0, 122);
    lv_label_set_text(d.unit, "$/MWh");

    lv_obj_t *dl = mk(parent, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_TOP_RIGHT, 0, 34);
    lv_label_set_text(dl, "DEMAND");
    d.demand_val = mk(parent, &lv_font_montserrat_20, NEM_C_WHITE, LV_ALIGN_TOP_RIGHT, 0, 52);
    lv_label_set_text(d.demand_val, "—");
    lv_obj_t *rl = mk(parent, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_TOP_RIGHT, 0, 82);
    lv_label_set_text(rl, "RENEWABLES");
    d.renew_val = mk(parent, &lv_font_montserrat_20, NEM_C_GREEN, LV_ALIGN_TOP_RIGHT, 0, 100);
    lv_label_set_text(d.renew_val, "—");

    /* Generation-mix bar: one flex segment per fuel; widths set live from mix. */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 440, 12);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x141416), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < NEM_FUEL_COUNT; i++) {
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, LV_PCT(100));
        lv_obj_set_flex_grow(seg, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(k_fuel_hex[i]), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        d.seg[i] = seg;
    }

    /* Legend under the mix bar: colour swatch + fuel name (static reference) */
    lv_obj_t *legend = lv_obj_create(parent);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, 440, 44);
    lv_obj_align(legend, LV_ALIGN_TOP_MID, 0, 186);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, 12, 0);
    lv_obj_set_style_pad_row(legend, 4, 0);
    lv_obj_clear_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < NEM_FUEL_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(legend);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, 5, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sw = lv_obj_create(item);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 10, 10);
        lv_obj_set_style_radius(sw, 3, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(k_fuel_hex[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);

        lv_obj_t *nm = lv_label_create(item);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xc9c9d2), 0);
        lv_label_set_text(nm, k_fuel_name[i]);
    }

    /* Top fuels: the biggest 3 with their share (filled live in render) */
    lv_obj_t *tf = lv_obj_create(parent);
    lv_obj_remove_style_all(tf);
    lv_obj_set_size(tf, 440, 30);
    lv_obj_align(tf, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_set_flex_flow(tf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tf, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tf, LV_OBJ_FLAG_SCROLLABLE);
    for (int k = 0; k < 3; k++) {
        lv_obj_t *item = lv_obj_create(tf);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, 6, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sw = lv_obj_create(item);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 12, 12);
        lv_obj_set_style_radius(sw, 3, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        d.tf_sw[k] = sw;

        lv_obj_t *lb = lv_label_create(item);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(lb, NEM_C_WHITE, 0);
        lv_label_set_text(lb, "");
        d.tf_lbl[k] = lb;
    }

    /* Net interconnector flow (same figure as the interconnector drill tile) */
    d.netflow = mk(parent, &lv_font_montserrat_18, NEM_C_MUTED, LV_ALIGN_TOP_MID, 0, 300);
    lv_label_set_text(d.netflow, "");

    lv_obj_t *hero_hit = lv_obj_create(parent);
    lv_obj_remove_style_all(hero_hit);
    lv_obj_set_size(hero_hit, 440, 190);
    lv_obj_align(hero_hit, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(hero_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hero_hit, hero_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* ribbon */
    lv_obj_t *ribbon = lv_obj_create(parent);
    lv_obj_remove_style_all(ribbon);
    lv_obj_set_size(ribbon, 440, 68);
    lv_obj_align(ribbon, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(ribbon, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ribbon, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ribbon, LV_OBJ_FLAG_SCROLLABLE);

    d.chip_n = 0;
    for (int r = 0; r < NEM_REGION_COUNT && d.chip_n < RIBBON_MAX; r++) {
        lv_obj_t *chip = lv_obj_create(ribbon);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, 102, 64);
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *nm = mk(chip, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(nm, "—");
        lv_obj_t *pr = mk(chip, &lv_font_montserrat_20, NEM_C_WHITE, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(pr, "—");
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, chip_clicked_cb, LV_EVENT_CLICKED, NULL);
        d.chip[d.chip_n] = chip;
        d.chip_name[d.chip_n] = nm;
        d.chip_price[d.chip_n] = pr;
        d.chip_n++;
    }
}

void ui_dashboard_update(const nem_snapshot_t *snap, const nem_region_mix_t *mix)
{
    d.snap = *snap;
    if (mix) d.mix = *mix;
    d.have_data = true;
    render();
}

static void render(void)
{
    if (!d.have_data) return;
    const nem_region_snapshot_t *h = &d.snap.regions[d.hero];
    const nem_fuel_mix_t *hm = &d.mix.regions[d.hero];

    lv_label_set_text(d.region, nem_region_name(d.hero));
    if (h->valid) {
        lv_label_set_text_fmt(d.price, "$%d", round_dollar(h->price));
        lv_obj_set_style_text_color(d.price, price_color(h->price), 0);
        lv_label_set_text_fmt(d.demand_val, "%d MW", (int)(h->demand_mw + 0.5));
    }
    if (hm->valid) {
        double tot = 0;
        for (int i = 0; i < NEM_FUEL_COUNT; i++) tot += hm->mw[i];
        if (tot < 1) tot = 1;
        for (int i = 0; i < NEM_FUEL_COUNT; i++)
            lv_obj_set_width(d.seg[i], LV_PCT((int)(hm->mw[i] * 100.0 / tot + 0.5)));
        lv_label_set_text_fmt(d.renew_val, "%d%%", (int)(hm->renewable_fraction * 100 + 0.5));

        /* top-fuels: biggest 3 (skip any that round to <1%) */
        int ord[NEM_FUEL_COUNT];
        for (int i = 0; i < NEM_FUEL_COUNT; i++) ord[i] = i;
        for (int a = 0; a < NEM_FUEL_COUNT; a++)
            for (int b = a + 1; b < NEM_FUEL_COUNT; b++)
                if (hm->mw[ord[b]] > hm->mw[ord[a]]) { int t = ord[a]; ord[a] = ord[b]; ord[b] = t; }
        for (int k = 0; k < 3; k++) {
            int f = ord[k];
            int pct = (int)(hm->mw[f] * 100.0 / tot + 0.5);
            if (pct >= 1) {
                lv_obj_clear_flag(d.tf_sw[k], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(d.tf_sw[k], lv_color_hex(k_fuel_hex[f]), 0);
                lv_label_set_text_fmt(d.tf_lbl[k], "%s %d%%", k_fuel_name[f], pct);
            } else {
                lv_obj_add_flag(d.tf_sw[k], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(d.tf_lbl[k], "");
            }
        }
    }

    /* net interconnector flow (reconciles with the interconnector drill tile) */
    double net = ui_drill_net_export(&d.snap, d.hero);
    lv_label_set_text_fmt(d.netflow, "%s Net %s %d MW",
        net >= 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
        net >= 0 ? "exporting" : "importing", (int)(net < 0 ? -net + 0.5 : net + 0.5));
    lv_obj_set_style_text_color(d.netflow, net >= 0 ? NEM_C_AMBER : NEM_C_BLUE, 0);

    int ci = 0;
    for (int r = 0; r < NEM_REGION_COUNT && ci < d.chip_n; r++) {
        if (r == d.hero) continue;
        const nem_region_snapshot_t *rs = &d.snap.regions[r];
        lv_label_set_text(d.chip_name[ci], nem_region_name((nem_region_t)r));
        if (rs->valid) {
            lv_label_set_text_fmt(d.chip_price[ci], "$%d", round_dollar(rs->price));
            lv_obj_set_style_text_color(d.chip_price[ci], price_color(rs->price), 0);
        }
        ci++;
    }
}

nem_region_t ui_dashboard_hero_region(void) { return d.hero; }
const nem_snapshot_t   *ui_dashboard_snapshot(void) { return d.have_data ? &d.snap : NULL; }
const nem_region_mix_t *ui_dashboard_mix(void)      { return d.have_data ? &d.mix  : NULL; }
