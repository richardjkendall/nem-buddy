# BSP: Waveshare ESP32-S3-Touch-AMOLED-2.16

[![Component Registry](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_16/badge.svg)](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_16)

The ESP32-S3-Touch-AMOLED-2.16 is a 2.16-inch 480×480 capacitive touch development board designed by waveshare electronics.

> **Local correction (not upstream):** upstream's README says `410×502`. That is the
> **2.06** board's panel. This board is **480×480** with a large corner radius — see
> `BSP_LCD_H_RES` / `BSP_LCD_V_RES` in `include/bsp/display.h`, which are and always were
> correct. Trust `display.h`, not prose.

|                            HW version                            | BSP Version |
|:----------------------------------------------------------------:| :---------: |
| [V1.0](http://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.16) |      ^1     |