#include "lvgl.h"
#include "esp_log.h"

static lv_point_t touch_start_point;  // 记录起始坐标
static bool gesture_detected = false; // 标志位

static const char* TAG = "main_page";

void lv_touch_cb(lv_event_t *e)
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
            break;
        }

        gesture_detected = true;

        if (abs(dx) > abs(dy))
        {
            if (dx > threshold)
                ESP_LOGI(TAG, "right");
            else
                ESP_LOGI(TAG, "left");
        }
        else
        {
            if (dy > threshold)
            {
                ESP_LOGI(TAG, "down");
            }
            else
            {
                ESP_LOGI(TAG, "up");
                {
                    // new_scr = ui_main_page_create();
                    // lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, anim_speed, 0, true);
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
        ESP_LOGI(TAG, "klick event");
        break;

    default:
        break;
    }
}