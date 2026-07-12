#include "ui_drill.h"
#include "ui_theme.h"
#include "ui_dashboard.h"
#include "data_task.h"
#include "nem/snapshot.h"
#include "nem/fuel.h"
#include "nem/history.h"
#include "lvgl.h"
#include <stdio.h>

#define N_TILES 3

static struct {
    lv_obj_t *root;      /* full-screen overlay; NULL when closed */
    lv_obj_t *tv;        /* tileview */
    lv_obj_t *tile[N_TILES];
    lv_obj_t *dot[N_TILES];
    lv_obj_t *hist_chart;
    lv_chart_series_t *hist_ser;
    nem_region_t region;
    bool open;
    lv_obj_t *mix_rows[NEM_FUEL_COUNT];   /* "Wind   2978 MW  44%" labels */
    lv_obj_t *mix_head;
    lv_obj_t *ic_head;
    lv_obj_t *ic_rows[NEM_MAX_INTERCONNECTORS];
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
        lv_obj_set_style_bg_color(s.dot[i], i == a ? NEM_C_WHITE : NEM_C_MUTED, 0);
}

static void render_history(void)
{
    const nem_region_history_t *h = nem_history_of(s.region);
    if (!h) return;
    double lo = 1e12, hi = -1e12;
    int n = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
        if (!h->filled[i]) continue;
        double p = h->price[i];
        if (p < lo) lo = p;
        if (p > hi) hi = p;
        n++;
    }
    if (n == 0) { lv_chart_set_point_count(s.hist_chart, 1); return; }
    if (hi <= lo) hi = lo + 1;
    lv_chart_set_axis_range(s.hist_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lo, (int32_t)hi);
    lv_chart_set_point_count(s.hist_chart, (uint32_t)n);
    int idx = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
        if (!h->filled[i]) continue;
        lv_chart_set_series_value_by_id(s.hist_chart, s.hist_ser, idx++, (int32_t)(h->price[i] + 0.5));
    }
    lv_obj_set_style_line_color(s.hist_chart, price_band(hi), LV_PART_ITEMS);
}

static lv_obj_t *tile_header(lv_obj_t *t, const char *title)
{
    lv_obj_t *lbl = lv_label_create(t);
    lv_obj_set_style_text_color(lbl, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(lbl, title);
    return lbl;
}

static void build_history_tile(lv_obj_t *t)
{
    tile_header(t, "TODAY  PRICE");
    lv_obj_t *c = lv_chart_create(t);
    lv_obj_set_size(c, 400, 300);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 10);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(c, 4, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);   /* hide point markers */
    s.hist_ser = lv_chart_add_series(c, NEM_C_BLUE, LV_CHART_AXIS_PRIMARY_Y);
    s.hist_chart = c;
}

static void build_mix_tile(lv_obj_t *t)
{
    tile_header(t, "GENERATION MIX");
    s.mix_head = lv_label_create(t);
    lv_obj_set_style_text_color(s.mix_head, NEM_C_GREEN, 0);
    lv_obj_set_style_text_font(s.mix_head, &lv_font_montserrat_22, 0);
    lv_obj_align(s.mix_head, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s.mix_head, "--");
    for (int i = 0; i < NEM_FUEL_COUNT; i++) {
        lv_obj_t *l = lv_label_create(t);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(FUEL_HEX[i]), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, 70 + i * 30);
        lv_label_set_text(l, "");
        s.mix_rows[i] = l;
    }
}

static void render_mix(void)
{
    const nem_region_mix_t *m = ui_dashboard_mix();
    if (!m) return;
    const nem_fuel_mix_t *fm = &m->regions[s.region];
    if (!fm->valid || fm->total_mw <= 0) {
        lv_label_set_text(s.mix_head, "--");
        for (int i = 0; i < NEM_FUEL_COUNT; i++) lv_label_set_text(s.mix_rows[i], "");
        return;
    }
    lv_label_set_text_fmt(s.mix_head, "%d%% renewable   %d MW",
                          (int)(fm->renewable_fraction * 100 + 0.5), (int)(fm->total_mw + 0.5));
    /* rank by MW: simple selection over the fixed small array */
    int order[NEM_FUEL_COUNT];
    for (int i = 0; i < NEM_FUEL_COUNT; i++) order[i] = i;
    for (int i = 0; i < NEM_FUEL_COUNT; i++)
        for (int j = i + 1; j < NEM_FUEL_COUNT; j++)
            if (fm->mw[order[j]] > fm->mw[order[i]]) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
    for (int rank = 0; rank < NEM_FUEL_COUNT; rank++) {
        int f = order[rank];
        lv_obj_set_style_text_color(s.mix_rows[rank], lv_color_hex(FUEL_HEX[f]), 0);
        int pct = (int)(100.0 * fm->mw[f] / fm->total_mw + 0.5);
        lv_label_set_text_fmt(s.mix_rows[rank], "%-8s %5d MW  %2d%%", FUEL_NAME[f], (int)(fm->mw[f] + 0.5), pct);
    }
}

static void build_ic_tile(lv_obj_t *t)
{
    tile_header(t, "INTERCONNECTORS");
    s.ic_head = lv_label_create(t);
    lv_obj_set_style_text_color(s.ic_head, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(s.ic_head, &lv_font_montserrat_22, 0);
    lv_obj_align(s.ic_head, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s.ic_head, "--");
    for (int i = 0; i < NEM_MAX_INTERCONNECTORS; i++) {
        lv_obj_t *l = lv_label_create(t);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(l, NEM_C_WHITE, 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, 70 + i * 30);
        lv_label_set_text(l, "");
        s.ic_rows[i] = l;
    }
}

static void render_ic(void)
{
    const nem_snapshot_t *snap = ui_dashboard_snapshot();
    if (!snap) return;
    const nem_region_snapshot_t *rs = &snap->regions[s.region];
    double ni = rs->net_interchange;
    lv_label_set_text_fmt(s.ic_head, "Net: %s %d MW",
                          ni >= 0 ? "exporting" : "importing", (int)(ni < 0 ? -ni : ni) );
    lv_obj_set_style_text_color(s.ic_head, ni >= 0 ? NEM_C_AMBER : NEM_C_BLUE, 0);
    for (int i = 0; i < NEM_MAX_INTERCONNECTORS; i++) {
        if (i < rs->interconnector_count) {
            const nem_interconnector_flow_t *f = &rs->interconnectors[i];
            const char *arrow = f->value >= 0 ? "->" : "<-";
            lv_label_set_text_fmt(s.ic_rows[i], "%-10s %s %d MW",
                                  f->name, arrow, (int)(f->value < 0 ? -f->value : f->value));
            lv_obj_set_style_text_color(s.ic_rows[i], f->value >= 0 ? NEM_C_AMBER : NEM_C_BLUE, 0);
        } else {
            lv_label_set_text(s.ic_rows[i], "");
        }
    }
}

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

    s.tile[0] = lv_tileview_add_tile(s.tv, 0, 0, LV_DIR_RIGHT);
    s.tile[1] = lv_tileview_add_tile(s.tv, 1, 0, LV_DIR_HOR);
    s.tile[2] = lv_tileview_add_tile(s.tv, 2, 0, LV_DIR_LEFT);
    build_history_tile(s.tile[0]);
    build_mix_tile(s.tile[1]);
    build_ic_tile(s.tile[2]);

    /* page dots */
    lv_obj_t *dots = lv_obj_create(s.root);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, 80, 10);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < N_TILES; i++) {
        lv_obj_t *dot = lv_obj_create(dots);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
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
