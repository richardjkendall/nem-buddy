#ifndef UI_THEME_H
#define UI_THEME_H
#include "lvgl.h"

/* Palette (approved mockups). */
#define NEM_C_BG        lv_color_hex(0x000000)
#define NEM_C_BLUE      lv_color_hex(0x4a9eff)   /* normal */
#define NEM_C_GREEN     lv_color_hex(0x37d67a)   /* cheap / negative */
#define NEM_C_AMBER     lv_color_hex(0xe0a23b)   /* high */
#define NEM_C_RED       lv_color_hex(0xff3b2f)   /* spike */
#define NEM_C_WHITE     lv_color_hex(0xe8e8ee)
#define NEM_C_MUTED     lv_color_hex(0x8a8a92)

/* Generation-mix fuel colours. */
#define NEM_C_COAL      lv_color_hex(0x5a5a5a)
#define NEM_C_GAS       lv_color_hex(0xe0a23b)
#define NEM_C_WIND      lv_color_hex(0x37d67a)
#define NEM_C_SOLAR     lv_color_hex(0xffd23f)
#define NEM_C_HYDRO     lv_color_hex(0x4a9eff)
#define NEM_C_BATTERY   lv_color_hex(0xb06bff)

#endif
