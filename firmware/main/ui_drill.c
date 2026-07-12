#include "ui_drill.h"
#include "ui_theme.h"
#include "ui_dashboard.h"
#include "data_task.h"
#include "nem/snapshot.h"
#include "nem/fuel.h"
#include "nem/history.h"
#include "nem/regions.h"
#include "lvgl.h"
#include <stdio.h>

#define N_TILES 3

/* Plot geometry shared by the history charts (tile-relative, tile pad = 0). */
#define PX 20              /* left margin */
#define PW 400             /* plot width  */
#define PC_Y 108           /* price chart top */
#define PC_H 140           /* price chart height */
#define DM_Y 300           /* demand chart top */
#define DM_H 60            /* demand chart height */

static struct {
    lv_obj_t *root, *tv, *tile[N_TILES], *dot[N_TILES];
    nem_region_t region;
    bool open;
    /* history tile */
    lv_obj_t *h_price, *h_arrow, *h_sub;
    lv_obj_t *hist_chart; lv_chart_series_t *hist_ser;
    lv_obj_t *peak_dot, *peak_lbl;
    lv_obj_t *dem_chart; lv_chart_series_t *dem_ser;
    /* mix tile */
    lv_obj_t *mix_sub_mw, *mix_sub_ren;
    lv_obj_t *mix_name[NEM_FUEL_COUNT], *mix_fill[NEM_FUEL_COUNT], *mix_pct[NEM_FUEL_COUNT];
    /* interconnector tile */
    lv_obj_t *ic_head;
    lv_obj_t *ic_name[NEM_MAX_INTERCONNECTORS], *ic_val[NEM_MAX_INTERCONNECTORS];
} s;

static const char *const FUEL_NAME[NEM_FUEL_COUNT] = {
    "Coal", "Gas", "Hydro", "Wind", "Solar", "Battery", "Other"
};
static const uint32_t FUEL_HEX[NEM_FUEL_COUNT] = {
    0x5a5a5a, 0xe0a23b, 0x4a9eff, 0x37d67a, 0xffd23f, 0xb06bff, 0x8a8a92
};

static lv_color_t price_band(double p)
{
    if (p < 0)    return NEM_C_GREEN;
    if (p > 1000) return NEM_C_RED;
    if (p > 300)  return NEM_C_AMBER;
    return NEM_C_BLUE;
}

static int active_tile(void)
{
    lv_obj_t *act = lv_tileview_get_tile_active(s.tv);
    for (int i = 0; i < N_TILES; i++) if (s.tile[i] == act) return i;
    return 0;
}

static void update_dots(void)
{
    int a = active_tile();
    for (int i = 0; i < N_TILES; i++)
        lv_obj_set_style_bg_color(s.dot[i], i == a ? NEM_C_BLUE : lv_color_hex(0x3a3a42), 0);
}

/* --- shared chrome: "< back" left, "VIC now/today" context right --- */
static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static void tile_head(lv_obj_t *t, const char *ctx)
{
    lv_obj_t *back = mk_label(t, &lv_font_montserrat_14, NEM_C_BLUE);
    lv_label_set_text(back, LV_SYMBOL_LEFT " back");
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *c = mk_label(t, &lv_font_montserrat_14, NEM_C_MUTED);
    lv_label_set_text(c, ctx);
    lv_obj_align(c, LV_ALIGN_TOP_RIGHT, 0, 0);
}

/* ===================== Tile 1: intraday history ===================== */

static void build_history_tile(lv_obj_t *t)
{
    char ctx[24]; snprintf(ctx, sizeof ctx, "%s  today", nem_region_name(s.region));
    tile_head(t, ctx);

    lv_obj_t *now = mk_label(t, &lv_font_montserrat_18, NEM_C_MUTED);
    lv_label_set_text(now, "now");
    lv_obj_align(now, LV_ALIGN_TOP_LEFT, PX, 26);

    s.h_price = mk_label(t, &lv_font_montserrat_48, NEM_C_WHITE);
    lv_label_set_text(s.h_price, "$--");
    lv_obj_align(s.h_price, LV_ALIGN_TOP_LEFT, PX + 54, 22);

    s.h_arrow = mk_label(t, &lv_font_montserrat_24, NEM_C_GREEN);
    lv_label_set_text(s.h_arrow, "");
    lv_obj_align(s.h_arrow, LV_ALIGN_TOP_LEFT, PX + 150, 40);

    s.h_sub = mk_label(t, &lv_font_montserrat_14, NEM_C_MUTED);
    lv_label_set_text(s.h_sub, "Price $/MWh  midnight to now");
    lv_obj_align(s.h_sub, LV_ALIGN_TOP_LEFT, PX, 86);

    lv_obj_t *c = lv_chart_create(t);
    lv_obj_set_pos(c, PX, PC_Y);
    lv_obj_set_size(c, PW, PC_H);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(c, 3, 0);
    lv_obj_set_style_line_width(c, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);   /* hide point markers */
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    s.hist_ser = lv_chart_add_series(c, NEM_C_BLUE, LV_CHART_AXIS_PRIMARY_Y);
    s.hist_chart = c;

    /* peak marker (positioned in render) */
    s.peak_dot = lv_obj_create(t);
    lv_obj_remove_style_all(s.peak_dot);
    lv_obj_set_size(s.peak_dot, 8, 8);
    lv_obj_set_style_radius(s.peak_dot, 4, 0);
    lv_obj_set_style_bg_color(s.peak_dot, NEM_C_RED, 0);
    lv_obj_set_style_bg_opa(s.peak_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s.peak_dot, LV_OBJ_FLAG_HIDDEN);
    s.peak_lbl = mk_label(t, &lv_font_montserrat_14, NEM_C_RED);
    lv_label_set_text(s.peak_lbl, "");
    lv_obj_add_flag(s.peak_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dl = mk_label(t, &lv_font_montserrat_14, NEM_C_MUTED);
    lv_label_set_text(dl, "Demand MW");
    lv_obj_align(dl, LV_ALIGN_TOP_LEFT, PX, DM_Y - 22);

    lv_obj_t *d = lv_chart_create(t);
    lv_obj_set_pos(d, PX, DM_Y);
    lv_obj_set_size(d, PW, DM_H);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_chart_set_type(d, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(d, 0, 0);
    lv_obj_set_style_line_width(d, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(d, 0, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    s.dem_ser = lv_chart_add_series(d, NEM_C_MUTED, LV_CHART_AXIS_PRIMARY_Y);
    s.dem_chart = d;
}

static void render_history(void)
{
    const nem_region_history_t *h = nem_history_of(s.region);
    const nem_snapshot_t *snap = ui_dashboard_snapshot();

    /* headline current price from the live snapshot */
    if (snap && snap->regions[s.region].valid) {
        double p = snap->regions[s.region].price;
        lv_label_set_text_fmt(s.h_price, "$%d", (int)(p + (p < 0 ? -0.5 : 0.5)));
        lv_obj_set_style_text_color(s.h_price, price_band(p), 0);
    }
    if (!h) return;

    /* gather filled price + demand, track peak */
    double plo = 1e12, phi = -1e12, dlo = 1e12, dhi = -1e12;
    int n = 0, peak_idx = -1; double peak_val = -1e12;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
        if (!h->filled[i]) continue;
        double p = h->price[i], dm = h->demand[i];
        if (p < plo) plo = p;
        if (p > phi) { phi = p; peak_val = p; peak_idx = n; }
        if (dm < dlo) dlo = dm;
        if (dm > dhi) dhi = dm;
        n++;
    }

    /* trend arrow: compare last two filled price samples (down=green good) */
    if (n >= 2) {
        double a = 0, b = 0; int k = 0;
        for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
            if (!h->filled[i]) continue;
            a = b; b = h->price[i]; k++;
        }
        (void)k;
        if (b < a)      { lv_label_set_text(s.h_arrow, LV_SYMBOL_DOWN); lv_obj_set_style_text_color(s.h_arrow, NEM_C_GREEN, 0); }
        else if (b > a) { lv_label_set_text(s.h_arrow, LV_SYMBOL_UP);   lv_obj_set_style_text_color(s.h_arrow, NEM_C_RED, 0); }
        else            { lv_label_set_text(s.h_arrow, ""); }
    } else {
        lv_label_set_text(s.h_arrow, "");
    }

    if (n == 0) { lv_chart_set_point_count(s.hist_chart, 1); lv_obj_add_flag(s.peak_dot, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s.peak_lbl, LV_OBJ_FLAG_HIDDEN); return; }
    if (phi <= plo) phi = plo + 1;
    if (dhi <= dlo) dhi = dlo + 1;

    lv_chart_set_axis_range(s.hist_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)plo, (int32_t)phi);
    lv_chart_set_point_count(s.hist_chart, (uint32_t)n);
    lv_chart_set_axis_range(s.dem_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)dlo, (int32_t)dhi);
    lv_chart_set_point_count(s.dem_chart, (uint32_t)n);
    int idx = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
        if (!h->filled[i]) continue;
        lv_chart_set_series_value_by_id(s.hist_chart, s.hist_ser, idx, (int32_t)(h->price[i] + 0.5));
        lv_chart_set_series_value_by_id(s.dem_chart, s.dem_ser, idx, (int32_t)(h->demand[i] + 0.5));
        idx++;
    }
    lv_obj_set_style_line_color(s.hist_chart, price_band(phi), LV_PART_ITEMS);

    /* peak marker: position over the price plot (tile-relative coords) */
    if (n >= 2 && peak_idx >= 0) {
        int px = PX + (int)((long)PW * peak_idx / (n - 1));
        int py = PC_Y + (int)(PC_H * (1.0 - (peak_val - plo) / (phi - plo)));
        lv_obj_set_pos(s.peak_dot, px - 4, py - 4);
        lv_obj_clear_flag(s.peak_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s.peak_lbl, "$%d peak", (int)(peak_val + 0.5));
        int lx = px - 24; if (lx < PX) lx = PX; if (lx > PX + PW - 60) lx = PX + PW - 60;
        lv_obj_set_pos(s.peak_lbl, lx, py - 22 < PC_Y ? PC_Y : py - 22);
        lv_obj_clear_flag(s.peak_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s.peak_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.peak_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== Tile 2: generation mix ===================== */

static void build_mix_tile(lv_obj_t *t)
{
    char ctx[24]; snprintf(ctx, sizeof ctx, "%s  now", nem_region_name(s.region));
    tile_head(t, ctx);

    lv_obj_t *title = mk_label(t, &lv_font_montserrat_22, NEM_C_WHITE);
    lv_label_set_text(title, "Generation mix");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PX, 26);

    s.mix_sub_mw = mk_label(t, &lv_font_montserrat_14, NEM_C_MUTED);
    lv_label_set_text(s.mix_sub_mw, "-- MW total");
    lv_obj_align(s.mix_sub_mw, LV_ALIGN_TOP_LEFT, PX, 58);
    s.mix_sub_ren = mk_label(t, &lv_font_montserrat_14, NEM_C_GREEN);
    lv_label_set_text(s.mix_sub_ren, "");
    lv_obj_align(s.mix_sub_ren, LV_ALIGN_TOP_LEFT, PX + 120, 58);

    for (int i = 0; i < NEM_FUEL_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(t);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, PW, 22);
        lv_obj_set_pos(row, PX, 88 + i * 30);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = mk_label(row, &lv_font_montserrat_16, lv_color_hex(0xc9c9d2));
        lv_obj_set_width(nm, 66);
        lv_label_set_text(nm, "");
        s.mix_name[i] = nm;

        lv_obj_t *track = lv_obj_create(row);
        lv_obj_remove_style_all(track);
        lv_obj_set_height(track, 14);
        lv_obj_set_flex_grow(track, 1);
        lv_obj_set_style_radius(track, 7, 0);
        lv_obj_set_style_clip_corner(track, true, 0);
        lv_obj_set_style_bg_color(track, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *fill = lv_obj_create(track);
        lv_obj_remove_style_all(fill);
        lv_obj_set_height(fill, LV_PCT(100));
        lv_obj_set_width(fill, LV_PCT(0));
        lv_obj_set_style_radius(fill, 7, 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        s.mix_fill[i] = fill;

        lv_obj_t *pct = mk_label(row, &lv_font_montserrat_16, NEM_C_WHITE);
        lv_obj_set_width(pct, 42);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(pct, "");
        s.mix_pct[i] = pct;
    }
}

static void render_mix(void)
{
    const nem_region_mix_t *m = ui_dashboard_mix();
    if (!m) return;
    const nem_fuel_mix_t *fm = &m->regions[s.region];
    if (!fm->valid || fm->total_mw <= 0) {
        lv_label_set_text(s.mix_sub_mw, "-- MW total");
        lv_label_set_text(s.mix_sub_ren, "");
        for (int i = 0; i < NEM_FUEL_COUNT; i++) {
            lv_label_set_text(s.mix_name[i], "");
            lv_label_set_text(s.mix_pct[i], "");
            lv_obj_set_width(s.mix_fill[i], LV_PCT(0));
        }
        return;
    }
    lv_label_set_text_fmt(s.mix_sub_mw, "%d MW total", (int)(fm->total_mw + 0.5));
    lv_label_set_text_fmt(s.mix_sub_ren, "%d%% renewable", (int)(fm->renewable_fraction * 100 + 0.5));

    int order[NEM_FUEL_COUNT];
    for (int i = 0; i < NEM_FUEL_COUNT; i++) order[i] = i;
    for (int i = 0; i < NEM_FUEL_COUNT; i++)
        for (int j = i + 1; j < NEM_FUEL_COUNT; j++)
            if (fm->mw[order[j]] > fm->mw[order[i]]) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }

    for (int rank = 0; rank < NEM_FUEL_COUNT; rank++) {
        int f = order[rank];
        int pct = (int)(100.0 * fm->mw[f] / fm->total_mw + 0.5);
        lv_label_set_text(s.mix_name[rank], FUEL_NAME[f]);
        lv_label_set_text_fmt(s.mix_pct[rank], "%d%%", pct);
        lv_obj_set_width(s.mix_fill[rank], LV_PCT(pct));
        lv_obj_set_style_bg_color(s.mix_fill[rank], lv_color_hex(FUEL_HEX[f]), 0);
    }
}

/* ===================== Tile 3: interconnectors ===================== */

static void build_ic_tile(lv_obj_t *t)
{
    char ctx[24]; snprintf(ctx, sizeof ctx, "%s  now", nem_region_name(s.region));
    tile_head(t, ctx);

    lv_obj_t *title = mk_label(t, &lv_font_montserrat_22, NEM_C_WHITE);
    lv_label_set_text(title, "Interconnectors");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PX, 26);

    s.ic_head = mk_label(t, &lv_font_montserrat_18, NEM_C_MUTED);
    lv_label_set_text(s.ic_head, "--");
    lv_obj_align(s.ic_head, LV_ALIGN_TOP_LEFT, PX, 58);

    for (int i = 0; i < NEM_MAX_INTERCONNECTORS; i++) {
        lv_obj_t *row = lv_obj_create(t);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, PW, 26);
        lv_obj_set_pos(row, PX, 96 + i * 34);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = mk_label(row, &lv_font_montserrat_18, lv_color_hex(0xc9c9d2));
        lv_label_set_text(nm, "");
        s.ic_name[i] = nm;

        lv_obj_t *vl = mk_label(row, &lv_font_montserrat_18, NEM_C_WHITE);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(vl, "");
        s.ic_val[i] = vl;
    }
}

static void render_ic(void)
{
    const nem_snapshot_t *snap = ui_dashboard_snapshot();
    if (!snap) return;
    const nem_region_snapshot_t *rs = &snap->regions[s.region];
    if (!rs->valid) {
        lv_label_set_text(s.ic_head, "--");
        lv_obj_set_style_text_color(s.ic_head, NEM_C_MUTED, 0);
        for (int i = 0; i < NEM_MAX_INTERCONNECTORS; i++) {
            lv_label_set_text(s.ic_name[i], "");
            lv_label_set_text(s.ic_val[i], "");
        }
        return;
    }
    double ni = rs->net_interchange;
    lv_label_set_text_fmt(s.ic_head, "Net %s %d MW",
                          ni >= 0 ? "exporting" : "importing", (int)(ni < 0 ? -ni : ni));
    lv_obj_set_style_text_color(s.ic_head, ni >= 0 ? NEM_C_AMBER : NEM_C_BLUE, 0);

    for (int i = 0; i < NEM_MAX_INTERCONNECTORS; i++) {
        if (i < rs->interconnector_count) {
            const nem_interconnector_flow_t *f = &rs->interconnectors[i];
            lv_label_set_text(s.ic_name[i], f->name);
            const char *arrow = f->value >= 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN;  /* export/import */
            lv_label_set_text_fmt(s.ic_val[i], "%s %d MW", arrow, (int)(f->value < 0 ? -f->value : f->value));
            lv_obj_set_style_text_color(s.ic_val[i], f->value >= 0 ? NEM_C_AMBER : NEM_C_BLUE, 0);
        } else {
            lv_label_set_text(s.ic_name[i], "");
            lv_label_set_text(s.ic_val[i], "");
        }
    }
}

/* ===================== scaffold ===================== */

static void close_drill(void)
{
    if (s.root) { lv_obj_del(s.root); s.root = NULL; }
    s.open = false;
}

static void root_clicked_cb(lv_event_t *e)
{
    (void)e;
    close_drill();
}

static void tv_changed_cb(lv_event_t *e)
{
    (void)e;
    update_dots();
    ui_drill_refresh();
}

/* Set the context label ("VIC  today"/"now") is baked at build; update region
 * name by rebuilding on show. */
void ui_drill_show(nem_region_t region)
{
    if (s.open) close_drill();
    s.region = region;

    s.root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s.root);
    lv_obj_set_style_bg_color(s.root, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s.root, 20, 0);
    lv_obj_add_flag(s.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s.root, root_clicked_cb, LV_EVENT_CLICKED, NULL);

    s.tv = lv_tileview_create(s.root);
    lv_obj_remove_style_all(s.tv);
    lv_obj_set_size(s.tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s.tv, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s.tv, tv_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Tile CLICKED must bubble to s.root; LVGL v9 re-checks the flag at each
     * hop, so the tileview needs it too. */
    lv_obj_add_flag(s.tv, LV_OBJ_FLAG_EVENT_BUBBLE);

    s.tile[0] = lv_tileview_add_tile(s.tv, 0, 0, LV_DIR_RIGHT);
    s.tile[1] = lv_tileview_add_tile(s.tv, 1, 0, LV_DIR_HOR);
    s.tile[2] = lv_tileview_add_tile(s.tv, 2, 0, LV_DIR_LEFT);
    for (int i = 0; i < N_TILES; i++) {
        lv_obj_set_style_pad_all(s.tile[i], 0, 0);
        lv_obj_clear_flag(s.tile[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s.tile[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    build_history_tile(s.tile[0]);
    build_mix_tile(s.tile[1]);
    build_ic_tile(s.tile[2]);

    /* page dots */
    lv_obj_t *dots = lv_obj_create(s.root);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, 80, 10);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dots, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int i = 0; i < N_TILES; i++) {
        lv_obj_t *dot = lv_obj_create(dots);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_margin_left(dot, 4, 0);
        lv_obj_set_style_margin_right(dot, 4, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        s.dot[i] = dot;
    }

    s.open = true;
    update_dots();
    render_history();
    render_mix();
    render_ic();
}

void ui_drill_refresh(void)
{
    if (!s.open) return;
    switch (active_tile()) {
        case 0: render_history(); break;
        case 1: render_mix();     break;
        case 2: render_ic();      break;
    }
}

bool ui_drill_is_open(void) { return s.open; }
