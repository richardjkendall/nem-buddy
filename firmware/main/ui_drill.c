#include "ui_drill.h"
#include "ui_theme.h"
#include "ui_dashboard.h"
#include "data_task.h"
#include "nem/snapshot.h"
#include "nem/fuel.h"
#include "nem/history.h"
#include "nem/regions.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <stdio.h>

#define N_TILES 3

/* Cap rendered points: the price/demand plots are ~400px wide, so drawing all
 * ~288 daily 5-min slots is wasted work that stalls the swipe animation (the
 * chart redraws every frame while a tile slides). Bucket down to this many. */
#define RENDER_MAX 80

/* Plot geometry shared by the history charts (tile-relative, tile pad = 0). */
#define PX 20              /* left margin */
#define PW 400             /* plot width  */
#define PC_Y 64            /* price chart top (gap below the header) */
#define PC_H 186           /* price chart height */
#define DM_Y 282           /* demand chart top */
#define DM_H 64            /* demand chart height */

static struct {
    lv_obj_t *root, *tv, *tile[N_TILES], *dot[N_TILES];
    nem_region_t region;
    bool open;
    /* history tile */
    lv_obj_t *h_sub;                 /* price chart title */
    lv_obj_t *price_plot;            /* lv_canvas: two-tone price chart, painted once */
    void *price_cbuf;                /* its PSRAM pixel buffer (freed on close) */
    lv_obj_t *dem_plot;              /* lv_canvas: demand strip w/ gradient fill */
    void *dem_cbuf;
    float pv[NEM_HISTORY_SLOTS];     /* filled price values, in order */
    int pn; double plo, phi;         /* point count + display range (incl. $0) */
    lv_obj_t *peak_dot, *peak_lbl, *min_dot, *min_lbl;
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

/* Custom price chart: two-tone gradient area fill split at $0 (blue above =
 * you pay, green below = you're paid), a dashed $0 baseline, and a two-tone line
 * that flips colour where it crosses zero. lv_chart can't do any of this, so we
 * draw it ourselves from s.pv[0..pn-1] over the display range [s.plo, s.phi].
 *
 * Painted ONCE into an lv_canvas per data update (not per frame) — the tileview
 * then just blits the cached bitmap during the swipe animation, so scroll cost
 * is independent of chart complexity. (x1,y1,w,h) is the canvas-local plot rect. */
static void paint_price(lv_layer_t *layer, int x1, int y1, int w, int h)
{
    int n = s.pn;
    if (n < 2 || !layer) return;

    const double lo = s.plo, hi = s.phi;
    const double span = (hi > lo) ? (hi - lo) : 1.0;

    #define PD_YOF(v) (y1 + (int)(h * (1.0 - ((v) - lo) / span) + 0.5))
    #define PD_XOF(fi) (x1 + (int)((double)w * (fi) / (n - 1) + 0.5))
    const int yzero = PD_YOF(0.0);
    const int ybot = y1 + h;

    /* ---- two-tone gradient area fill, one thin column at a time ---- */
    lv_draw_rect_dsc_t fd; lv_draw_rect_dsc_init(&fd);
    fd.bg_opa = LV_OPA_COVER;
    fd.border_opa = fd.outline_opa = fd.shadow_opa = LV_OPA_TRANSP;
    fd.bg_grad.dir = LV_GRAD_DIR_VER;
    fd.bg_grad.stops_count = 2;
    const int FILL_STEP = 4;   /* wider columns = fewer gradient draws; the crisp
                                * line on top hides the coarser fill top edge */
    const int TOPB = 0x62;   /* blue fill opacity at plot top   */
    const int BOTG = 0x5a;   /* green fill opacity at plot floor */
    for (int cx = 0; cx < w; cx += FILL_STEP) {
        double fi = (double)cx * (n - 1) / w;
        int i = (int)fi; if (i > n - 2) i = n - 2;
        double v = s.pv[i] + (fi - i) * (s.pv[i + 1] - s.pv[i]);
        int yv = PD_YOF(v);
        lv_area_t col = { x1 + cx, 0, x1 + cx + FILL_STEP - 1, 0 };
        if (v >= 0) {
            if (yv >= yzero) continue;
            col.y1 = yv; col.y2 = yzero;
            int oTop = (yzero > y1) ? TOPB * (yzero - yv) / (yzero - y1) : TOPB;
            fd.bg_grad.stops[0].color = NEM_C_BLUE; fd.bg_grad.stops[0].opa = (lv_opa_t)oTop; fd.bg_grad.stops[0].frac = 0;
            fd.bg_grad.stops[1].color = NEM_C_BLUE; fd.bg_grad.stops[1].opa = 0;              fd.bg_grad.stops[1].frac = 255;
        } else {
            if (yv <= yzero) continue;
            col.y1 = yzero; col.y2 = yv;
            int oBot = (ybot > yzero) ? BOTG * (yv - yzero) / (ybot - yzero) : BOTG;
            fd.bg_grad.stops[0].color = NEM_C_GREEN; fd.bg_grad.stops[0].opa = 0;              fd.bg_grad.stops[0].frac = 0;
            fd.bg_grad.stops[1].color = NEM_C_GREEN; fd.bg_grad.stops[1].opa = (lv_opa_t)oBot; fd.bg_grad.stops[1].frac = 255;
        }
        fd.bg_color = fd.bg_grad.stops[0].color;
        lv_draw_rect(layer, &fd, &col);
    }

    /* ---- dashed $0 baseline ---- */
    lv_draw_line_dsc_t zd; lv_draw_line_dsc_init(&zd);
    zd.color = lv_color_hex(0x4a4a52); zd.width = 1; zd.opa = LV_OPA_COVER;
    zd.dash_width = 3; zd.dash_gap = 3;
    zd.p1.x = x1; zd.p1.y = yzero; zd.p2.x = x1 + w; zd.p2.y = yzero;
    lv_draw_line(layer, &zd);

    /* ---- two-tone price line, split at each zero crossing ---- */
    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    ld.width = 3; ld.opa = LV_OPA_COVER; ld.round_start = ld.round_end = 0;
    for (int i = 0; i < n - 1; i++) {
        double v0 = s.pv[i], v1 = s.pv[i + 1];
        int x0 = PD_XOF(i), xa = PD_XOF(i + 1);
        int y0 = PD_YOF(v0), ya = PD_YOF(v1);
        if ((v0 >= 0) == (v1 >= 0)) {
            ld.color = (v0 >= 0) ? NEM_C_BLUE : NEM_C_GREEN;
            ld.p1.x = x0; ld.p1.y = y0; ld.p2.x = xa; ld.p2.y = ya;
            lv_draw_line(layer, &ld);
        } else {
            double f = v0 / (v0 - v1);   /* fraction along segment where v == 0 */
            int xc = x1 + (int)((double)w * (i + f) / (n - 1) + 0.5);
            ld.color = (v0 >= 0) ? NEM_C_BLUE : NEM_C_GREEN;
            ld.p1.x = x0; ld.p1.y = y0; ld.p2.x = xc; ld.p2.y = yzero;
            lv_draw_line(layer, &ld);
            ld.color = (v1 >= 0) ? NEM_C_BLUE : NEM_C_GREEN;
            ld.p1.x = xc; ld.p1.y = yzero; ld.p2.x = xa; ld.p2.y = ya;
            lv_draw_line(layer, &ld);
        }
    }
    #undef PD_YOF
    #undef PD_XOF
}

/* Demand strip: a grey line with a soft grey gradient fill fading down to the
 * plot floor. Painted into its own canvas from s.pv-parallel demand values. */
static void paint_demand(lv_layer_t *layer, int x1, int y1, int w, int h,
                         const float *dv, int n, double lo, double hi)
{
    if (n < 2 || !layer) return;
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    #define DD_YOF(v) (y1 + (int)(h * (1.0 - ((v) - lo) / span) + 0.5))
    #define DD_XOF(i) (x1 + (int)((double)w * (i) / (n - 1) + 0.5))
    const int ybot = y1 + h;

    lv_draw_rect_dsc_t fd; lv_draw_rect_dsc_init(&fd);
    fd.bg_opa = LV_OPA_COVER;
    fd.border_opa = fd.outline_opa = fd.shadow_opa = LV_OPA_TRANSP;
    fd.bg_grad.dir = LV_GRAD_DIR_VER;
    fd.bg_grad.stops_count = 2;
    const int FILL_STEP = 4, TOP = 0x50;   /* grey fill opacity near the line */
    for (int cx = 0; cx < w; cx += FILL_STEP) {
        double fi = (double)cx * (n - 1) / w;
        int i = (int)fi; if (i > n - 2) i = n - 2;
        double v = dv[i] + (fi - i) * (dv[i + 1] - dv[i]);
        int yv = DD_YOF(v);
        if (yv >= ybot) continue;
        int oTop = TOP * (ybot - yv) / (h > 0 ? h : 1);
        fd.bg_grad.stops[0].color = NEM_C_MUTED; fd.bg_grad.stops[0].opa = (lv_opa_t)oTop; fd.bg_grad.stops[0].frac = 0;
        fd.bg_grad.stops[1].color = NEM_C_MUTED; fd.bg_grad.stops[1].opa = 0;              fd.bg_grad.stops[1].frac = 255;
        fd.bg_color = NEM_C_MUTED;
        lv_area_t col = { x1 + cx, yv, x1 + cx + FILL_STEP - 1, ybot };
        lv_draw_rect(layer, &fd, &col);
    }

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    ld.color = NEM_C_MUTED; ld.width = 2; ld.opa = LV_OPA_COVER;
    for (int i = 0; i < n - 1; i++) {
        ld.p1.x = DD_XOF(i);     ld.p1.y = DD_YOF(dv[i]);
        ld.p2.x = DD_XOF(i + 1); ld.p2.y = DD_YOF(dv[i + 1]);
        lv_draw_line(layer, &ld);
    }
    #undef DD_YOF
    #undef DD_XOF
}

/* Place a marker dot at (px,py) with its label to the side — right if the dot
 * sits in the left half of the plot, else left — so it never sits on the line. */
static void place_marker(lv_obj_t *dot, lv_obj_t *lbl, int px, int py)
{
    lv_obj_set_pos(dot, px - 4, py - 4);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(lbl);
    int lw = lv_obj_get_width(lbl), lh = lv_obj_get_height(lbl);
    int lx = (px < PX + PW / 2) ? (px + 9) : (px - 9 - lw);
    if (lx < PX) lx = PX;
    if (lx > PX + PW - lw) lx = PX + PW - lw;
    int ly = py - lh / 2;
    if (ly < PC_Y) ly = PC_Y;
    if (ly > PC_Y + PC_H - lh) ly = PC_Y + PC_H - lh;
    lv_obj_set_pos(lbl, lx, ly);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

static void build_history_tile(lv_obj_t *t)
{
    char ctx[24]; snprintf(ctx, sizeof ctx, "%s  today", nem_region_name(s.region));
    tile_head(t, ctx);

    s.h_sub = mk_label(t, &lv_font_montserrat_14, NEM_C_WHITE);
    lv_label_set_text(s.h_sub, "Price $/MWh  midnight to now");
    lv_obj_align(s.h_sub, LV_ALIGN_TOP_LEFT, PX, 26);

    /* price chart is an lv_canvas (PSRAM-backed) painted on data updates only */
    lv_obj_t *pp = lv_canvas_create(t);
    lv_obj_set_pos(pp, PX, PC_Y);
    lv_obj_clear_flag(pp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pp, LV_OBJ_FLAG_EVENT_BUBBLE);
    s.price_cbuf = heap_caps_malloc(
        LV_CANVAS_BUF_SIZE(PW, PC_H, 16, LV_DRAW_BUF_STRIDE_ALIGN), MALLOC_CAP_SPIRAM);
    if (s.price_cbuf) {
        lv_canvas_set_buffer(pp, s.price_cbuf, PW, PC_H, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(pp, NEM_C_BG, LV_OPA_COVER);
    }
    s.price_plot = pp;

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

    /* min marker (green = cheapest / most-negative point) */
    s.min_dot = lv_obj_create(t);
    lv_obj_remove_style_all(s.min_dot);
    lv_obj_set_size(s.min_dot, 8, 8);
    lv_obj_set_style_radius(s.min_dot, 4, 0);
    lv_obj_set_style_bg_color(s.min_dot, NEM_C_GREEN, 0);
    lv_obj_set_style_bg_opa(s.min_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s.min_dot, LV_OBJ_FLAG_HIDDEN);
    s.min_lbl = mk_label(t, &lv_font_montserrat_14, NEM_C_GREEN);
    lv_label_set_text(s.min_lbl, "");
    lv_obj_add_flag(s.min_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dl = mk_label(t, &lv_font_montserrat_14, NEM_C_MUTED);
    lv_label_set_text(dl, "Demand MW");
    lv_obj_align(dl, LV_ALIGN_TOP_LEFT, PX, DM_Y - 22);

    /* demand strip is also a canvas (gradient fill + line, painted once) */
    lv_obj_t *d = lv_canvas_create(t);
    lv_obj_set_pos(d, PX, DM_Y);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d, LV_OBJ_FLAG_EVENT_BUBBLE);
    s.dem_cbuf = heap_caps_malloc(
        LV_CANVAS_BUF_SIZE(PW, DM_H, 16, LV_DRAW_BUF_STRIDE_ALIGN), MALLOC_CAP_SPIRAM);
    if (s.dem_cbuf) {
        lv_canvas_set_buffer(d, s.dem_cbuf, PW, DM_H, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(d, NEM_C_BG, LV_OPA_COVER);
    }
    s.dem_plot = d;
}

/* Downsample n raw (price,demand) samples into <=RENDER_MAX buckets. Per bucket
 * we keep the sample with the largest |price| so spikes and negative dips (the
 * whole story) survive — a plain average would flatten them out. Demand rides
 * along on the chosen index (it's a smooth secondary strip). Returns out count. */
static int decimate(const float *pin, const float *din, int n,
                    float *pout, float *dout)
{
    if (n <= RENDER_MAX) {
        for (int i = 0; i < n; i++) { pout[i] = pin[i]; dout[i] = din[i]; }
        return n;
    }
    for (int b = 0; b < RENDER_MAX; b++) {
        int s0 = (int)((long)b * n / RENDER_MAX);
        int s1 = (int)((long)(b + 1) * n / RENDER_MAX);
        if (s1 <= s0) s1 = s0 + 1;
        int best = s0; float bestmag = -1.0f;
        for (int i = s0; i < s1 && i < n; i++) {
            float mag = pin[i] < 0 ? -pin[i] : pin[i];
            if (mag > bestmag) { bestmag = mag; best = i; }
        }
        pout[b] = pin[best]; dout[b] = din[best];
    }
    return RENDER_MAX;
}

static void render_history(void)
{
    const nem_region_history_t *h = nem_history_of(s.region);
    if (!h) return;

    /* gather filled price + demand into raw buffers (static: keep off the
     * LVGL task stack) */
    static float rawp[NEM_HISTORY_SLOTS], rawd[NEM_HISTORY_SLOTS];
    int nn = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
        if (!h->filled[i]) continue;
        rawp[nn] = (float)h->price[i];
        rawd[nn] = (float)h->demand[i];
        nn++;
    }

    if (nn == 0) {
        s.pn = 0;
        if (s.price_cbuf) lv_canvas_fill_bg(s.price_plot, NEM_C_BG, LV_OPA_COVER);
        if (s.dem_cbuf)   lv_canvas_fill_bg(s.dem_plot,   NEM_C_BG, LV_OPA_COVER);
        lv_obj_add_flag(s.peak_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.peak_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.min_dot,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.min_lbl,  LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* decimate to a render-friendly point count (keeps spikes) */
    static float demd[RENDER_MAX];
    s.pn = decimate(rawp, rawd, nn, s.pv, demd);

    /* range from the decimated points, extended to always include $0 so the
     * baseline shows and negative prices visibly dip below it; track peak+min */
    double plo = 1e12, phi = -1e12, dlo = 1e12, dhi = -1e12;
    int peak_idx = -1, min_idx = -1; double peak_val = -1e12, min_val = 1e12;
    for (int i = 0; i < s.pn; i++) {
        double p = s.pv[i], dm = demd[i];
        if (p > phi) { phi = p; peak_val = p; peak_idx = i; }
        if (p < plo) { plo = p; min_val  = p; min_idx  = i; }
        if (dm < dlo) dlo = dm;
        if (dm > dhi) dhi = dm;
    }
    if (plo > 0) plo = 0;   /* display range always spans $0 */
    if (phi < 0) phi = 0;
    if (phi <= plo) phi = plo + 1;
    if (dhi <= dlo) dhi = dlo + 1;
    s.plo = plo; s.phi = phi;

    /* paint both charts into their canvases once (cheap blit thereafter) */
    if (s.price_cbuf) {
        lv_canvas_fill_bg(s.price_plot, NEM_C_BG, LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(s.price_plot, &layer);
        paint_price(&layer, 0, 0, PW, PC_H);
        lv_canvas_finish_layer(s.price_plot, &layer);
    }
    if (s.dem_cbuf) {
        lv_canvas_fill_bg(s.dem_plot, NEM_C_BG, LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(s.dem_plot, &layer);
        paint_demand(&layer, 0, 0, PW, DM_H, demd, s.pn, dlo, dhi);
        lv_canvas_finish_layer(s.dem_plot, &layer);
    }

    /* peak (red) + min (green) markers, labels placed to the side of the dot */
    if (s.pn >= 2) {
        int ppx = PX + (int)((long)PW * peak_idx / (s.pn - 1));
        int ppy = PC_Y + (int)(PC_H * (1.0 - (peak_val - plo) / (phi - plo)));
        lv_label_set_text_fmt(s.peak_lbl, "$%d peak", (int)(peak_val + 0.5));
        place_marker(s.peak_dot, s.peak_lbl, ppx, ppy);

        int mpx = PX + (int)((long)PW * min_idx / (s.pn - 1));
        int mpy = PC_Y + (int)(PC_H * (1.0 - (min_val - plo) / (phi - plo)));
        lv_label_set_text_fmt(s.min_lbl, "$%d min", (int)(min_val + (min_val < 0 ? -0.5 : 0.5)));
        place_marker(s.min_dot, s.min_lbl, mpx, mpy);
    } else {
        lv_obj_add_flag(s.peak_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.peak_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.min_dot,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.min_lbl,  LV_OBJ_FLAG_HIDDEN);
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
        lv_obj_set_style_pad_right(row, 28, 0);   /* keep the % clear of the edge */
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
    if (s.root) { lv_obj_del(s.root); s.root = NULL; }   /* deletes the canvas objs first */
    if (s.price_cbuf) { heap_caps_free(s.price_cbuf); s.price_cbuf = NULL; }
    if (s.dem_cbuf)   { heap_caps_free(s.dem_cbuf);   s.dem_cbuf = NULL; }
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
