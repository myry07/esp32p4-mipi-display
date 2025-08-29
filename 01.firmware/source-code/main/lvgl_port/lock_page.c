#include "lvgl.h"
#include "ui.h"
#include "bsp.h"
#include "esp_log.h"


static const char* TAG = "lock_page";

static lv_obj_t *s_lock_page = NULL;
static lv_obj_t *s_time_label = NULL;

static lv_point_t touch_start_point;  // 记录起始坐标
static bool gesture_detected = false; // 标志位

static void load_page_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    lv_obj_t *new_scr = NULL;

    switch (code)
    {
    case LV_EVENT_PRESSED:
        gesture_detected = false;
        lv_indev_get_point(indev, &touch_start_point);
        break;

    case LV_EVENT_RELEASED:
    {
        lv_point_t touch_end_point;
        lv_indev_get_point(indev, &touch_end_point);

        int dx = touch_end_point.x - touch_start_point.x;
        int dy = touch_end_point.y - touch_start_point.y;

        const int threshold = 15;

        if (abs(dx) < threshold && abs(dy) < threshold)
        {
            // 移动太小，不是滑动
            break;
        }

        gesture_detected = true;

        if (abs(dx) > abs(dy))
        {
            if (dx > threshold)
                ESP_LOGI("gesture", "右滑");
            else
                ESP_LOGI("gesture", "左滑");
        }
        else
        {
            if (dy > threshold)
            {
                ESP_LOGI("gesture", "下滑");
            }
            else
            {
                ESP_LOGI("gesture", "上滑");
                {
               
                    new_scr = page_main_create();
                    lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 50, 0, true);
                }
            }
        }

        break;
    }

    case LV_EVENT_CLICKED:
        if (gesture_detected)
        {
            break;
        }
        ESP_LOGI("btn", "点击事件");
        break;

    default:
        break;
    }
}


lv_obj_t *page_lock_create(void)
{
    s_lock_page = lv_obj_create(NULL);
    lv_obj_set_size(s_lock_page, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_lock_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_lock_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img_canvas = show_jpg_on_canvas(s_lock_page, "/sdcard/bg/4k1.JPG", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    if (img_canvas) {
        lv_obj_align(img_canvas, LV_ALIGN_CENTER, 0, 0);
    }

    s_time_label = lv_label_create(s_lock_page);
    lv_obj_set_style_text_color(s_time_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_26, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_text(s_time_label, "09:00");

    lv_obj_t *bat_label = lv_label_create(s_lock_page);
    lv_label_set_text(bat_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(bat_label, LV_ALIGN_TOP_RIGHT, -5, 0);

    lv_obj_add_event_cb(s_lock_page, load_page_cb, LV_EVENT_ALL, NULL);

    return s_lock_page;
}
