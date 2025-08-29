#include "page_manager.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "lvgl.h"

#include "ui.h"

static const char *TAG = "page_mgr";

// static lv_point_t touch_start_point;  // 记录起始坐标
// static bool gesture_detected = false; // 标志位

// void load_page_cb(lv_event_t *e)
// {
//     lv_event_code_t code = lv_event_get_code(e);
//     lv_indev_t *indev = lv_indev_get_act();

//     lv_obj_t *new_scr = NULL;

//     switch (code)
//     {
//     case LV_EVENT_PRESSED:
//         gesture_detected = false;
//         lv_indev_get_point(indev, &touch_start_point);
//         break;

//     case LV_EVENT_RELEASED:
//     {
//         lv_point_t touch_end_point;
//         lv_indev_get_point(indev, &touch_end_point);

//         int dx = touch_end_point.x - touch_start_point.x;
//         int dy = touch_end_point.y - touch_start_point.y;

//         const int threshold = 15;

//         if (abs(dx) < threshold && abs(dy) < threshold)
//         {
//             // 移动太小，不是滑动
//             break;
//         }

//         gesture_detected = true;

//         if (abs(dx) > abs(dy))
//         {
//             if (dx > threshold)
//                 ESP_LOGI("gesture", "右滑");
//             else
//                 ESP_LOGI("gesture", "左滑");
//         }
//         else
//         {
//             if (dy > threshold)
//             {
//                 ESP_LOGI("gesture", "下滑");
//             }
//             else
//             {
//                 ESP_LOGI("gesture", "上滑");
//                 {
               
//                     new_scr = page_main_create();
//                     lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 50, 0, true);
//                 }
//             }
//         }

//         break;
//     }

//     case LV_EVENT_CLICKED:
//         if (gesture_detected)
//         {
//             break;
//         }
//         ESP_LOGI("btn", "点击事件");
//         break;

//     default:
//         break;
//     }
// }
