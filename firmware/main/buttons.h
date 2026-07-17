/* firmware/main/buttons.h */
#ifndef BUTTONS_H
#define BUTTONS_H

typedef void (*button_cb_t)(void);

/* Start a task polling the IO18 user button. `on_press` fires once per press,
 * on the falling edge, from the button task's context — so it MUST take
 * bsp_display_lock() before touching LVGL. */
void buttons_start(button_cb_t on_press);

#endif
