#include "lvgl.h"
#include "ui.h"
#include "bsp.h"

#include "esp_log.h"

static lv_obj_t *s_lock_page = NULL;
static lv_obj_t *s_time_label = NULL;

lv_obj_t *ui_lock_page_create(void)
{
    s_lock_page = lv_obj_create(NULL);
    lv_obj_set_size(s_lock_page, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_lock_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_lock_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img_canvas = show_jpg_on_canvas(s_lock_page, "/sdcard/nr/S.JPG", 540, 720);
    if (img_canvas)
    {
        lv_obj_align(img_canvas, LV_ALIGN_CENTER, 0, 0);
    }

    s_time_label = lv_label_create(s_lock_page);
    lv_obj_set_style_text_color(s_time_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_text(s_time_label, "09:00");

    lv_obj_t *bat_label = lv_label_create(s_lock_page);
    lv_label_set_text(bat_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(bat_label, LV_ALIGN_TOP_RIGHT, -5, 0);

    lv_obj_add_event_cb(s_lock_page, lv_touch_cb, LV_EVENT_ALL, NULL);

    lv_scr_load(s_lock_page);
    return s_lock_page;
}
