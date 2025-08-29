#include "lvgl.h"
#include "esp_log.h"
#include "ui.h"
#include "bsp.h"

#define btn_w 150
#define btn_h 40
#define btn_step 30

static const char *TAG = "main_page";

static lv_obj_t *s_main_page = NULL;

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

                new_scr = page_lock_create();
                lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 50, 0, true);
            }
            else
            {
                ESP_LOGI("gesture", "上滑");
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

// Pic 按钮的回调
void pic_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "Picture 被点击");
        lv_obj_t *new_scr = photo_album_create("/sdcard/nr",
                                               EXAMPLE_LCD_H_RES,
                                               EXAMPLE_LCD_V_RES,
                                               true);
        if (new_scr)
        {
            lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_FADE_IN, 50, 0, true);
        }
        else
        {
            ESP_LOGE(TAG, "创建相册页面失败（目录不存在或无图片）");
            // 可选：弹个提示
            lv_obj_t *mb = lv_msgbox_create(NULL, "相册",
                                            "未找到 /sdcard/nr 或没有 JPG 图片。",
                                            NULL, true);
            lv_obj_center(mb);
        }
    }
}

// video btn
void video_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "Video 被点击");
    }
}

lv_obj_t *page_main_create(void)
{
    s_main_page = lv_obj_create(NULL);
    lv_obj_set_size(s_main_page, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_main_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_main_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img_canvas = show_jpg_on_canvas(s_main_page, "/sdcard/bg/bk2.JPG", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    if (img_canvas)
    {
        lv_obj_align(img_canvas, LV_ALIGN_CENTER, 0, 0);
    }

    /* <--------------picture--------------> */
    lv_obj_t *s_pic_button = lv_btn_create(s_main_page);
    lv_obj_set_pos(s_pic_button, 10, btn_step);
    lv_obj_set_style_bg_color(s_pic_button, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_size(s_pic_button, btn_w, btn_h);
    lv_obj_set_style_bg_opa(s_pic_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pic_button, 0, 0);
    lv_obj_set_style_radius(s_pic_button, 0, 0);
    lv_obj_set_style_shadow_width(s_pic_button, 0, 0);

    lv_obj_t *s_pic_label = lv_label_create(s_pic_button);
    lv_obj_align(s_pic_label, LV_ALIGN_LEFT_MID, 40, 0);
    lv_label_set_text(s_pic_label, "Pictures");
    lv_obj_set_style_text_font(s_pic_label, &lv_font_montserrat_14, LV_STATE_DEFAULT);

    /* <--------------video--------------> */
    lv_obj_t *s_video_button = lv_btn_create(s_main_page);
    lv_obj_set_pos(s_video_button, 10, btn_step * 5);
    lv_obj_set_style_bg_color(s_video_button, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_size(s_video_button, btn_w, btn_h);
    lv_obj_set_style_bg_opa(s_video_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_video_button, 0, 0);
    lv_obj_set_style_radius(s_video_button, 0, 0);
    lv_obj_set_style_shadow_width(s_video_button, 0, 0);

    lv_obj_t *s_video_label = lv_label_create(s_video_button);
    lv_obj_align(s_video_label, LV_ALIGN_LEFT_MID, 40, 0);
    lv_label_set_text(s_video_label, "Videos");
    lv_obj_set_style_text_font(s_video_label, &lv_font_montserrat_14, LV_STATE_DEFAULT);

    lv_obj_add_event_cb(s_main_page, load_page_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(s_pic_button, pic_btn_cb, LV_EVENT_ALL, NULL);

    return s_main_page;
}
