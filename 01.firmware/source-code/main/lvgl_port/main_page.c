#include "lvgl.h"
#include "esp_log.h"
#include "ui.h"
#include "bsp.h"

#include <stdio.h>

#define btn_w 150
#define btn_h 40
#define btn_step 30

static const char *TAG = "main_page";

static lv_obj_t *s_main_page = NULL;

static void video_page_delete_cb(lv_event_t *e);
static void video_back_btn_cb(lv_event_t *e);
static void video_gesture_cb(lv_event_t *e);

typedef struct
{
    char path[256];
    bool is_dir;
    bool loop;
    lv_obj_t *back_btn;
    lv_obj_t *title;
} video_page_ctx_t;

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
        // video_page_create("/sdcard/nr/gc4.avi", false, false);
        video_page_create("/sdcard/nr", true, true);
    }
}

lv_obj_t *page_main_create(void)
{
    s_main_page = lv_obj_create(NULL);
    lv_obj_set_size(s_main_page, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_main_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_main_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img_canvas = show_jpg_on_canvas(s_main_page, "/sdcard/bg/4k2.JPG", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    if (img_canvas)
    {
        lv_obj_align(img_canvas, LV_ALIGN_CENTER, 0, 0);
    }

    /* === 一行容器：水平等间距，垂直居中 === */
    lv_obj_t *row = lv_obj_create(s_main_page);
    lv_obj_set_size(row, LV_PCT(100), 140);     // 行高给到能容纳 90x90+阴影
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 60); // 整行的纵向位置，按需改 y 偏移
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_EVENLY, // 主轴（水平）等间距
                          LV_FLEX_ALIGN_CENTER,       // 交叉轴（垂直）居中
                          LV_FLEX_ALIGN_CENTER);

    /* === 公共样式 === */
    const int ICON_W = 90, ICON_H = 90, R = 20;

    /* === Picture 按钮 === */
    lv_obj_t *s_pic_button = lv_btn_create(row);
    lv_obj_set_size(s_pic_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_pic_button, R, 0);
    lv_obj_set_style_bg_color(s_pic_button, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_pic_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_pic_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_pic_button, 9, 0);
    lv_obj_set_style_clip_corner(s_pic_button, true, 0);

    lv_obj_t *img_pic = show_jpg_on_canvas(s_pic_button, "/sdcard/btns/photo.JPG", ICON_W, ICON_H);
    if (img_pic)
    {
        lv_obj_center(img_pic);
        lv_obj_set_style_radius(img_pic, R, 0);
        lv_obj_set_style_clip_corner(img_pic, true, 0);
    }
    else
    {
        ESP_LOGW(TAG, "photo.JPG load failed, fallback color only");
    }

    /* === Video 按钮 === */
    lv_obj_t *s_video_button = lv_btn_create(row);
    lv_obj_set_size(s_video_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_video_button, R, 0);
    lv_obj_set_style_bg_color(s_video_button, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_video_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_video_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_video_button, 9, 0);
    lv_obj_set_style_clip_corner(s_video_button, true, 0);

    lv_obj_t *img_video = show_jpg_on_canvas(s_video_button, "/sdcard/btns/video.JPG", ICON_W, ICON_H);
    if (img_video)
    {
        lv_obj_center(img_video);
        lv_obj_set_style_radius(img_video, R, 0);
        lv_obj_set_style_clip_corner(img_video, true, 0);
    }
    else
    {
        ESP_LOGW(TAG, "video.JPG load failed, fallback color only");
    }

    /* === Music 按钮 === */
    lv_obj_t *s_music_button = lv_btn_create(row);
    lv_obj_set_size(s_music_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_music_button, R, 0);
    lv_obj_set_style_bg_color(s_music_button, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(s_music_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_music_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_music_button, 9, 0);
    lv_obj_set_style_clip_corner(s_music_button, true, 0);

    lv_obj_t *img_music = show_jpg_on_canvas(s_music_button, "/sdcard/btns/music.JPG", ICON_W, ICON_H);
    if (img_music)
    {
        lv_obj_center(img_music);
        lv_obj_set_style_radius(img_music, R, 0);
        lv_obj_set_style_clip_corner(img_music, true, 0);
    }
    else
    {
        ESP_LOGW(TAG, "music.JPG load failed, fallback color icon");
    }

    /* 事件回调 */
    lv_obj_add_event_cb(s_pic_button, pic_btn_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(s_video_button, video_btn_cb, LV_EVENT_CLICKED, NULL);
    // lv_obj_add_event_cb(s_music_button, music_btn_cb, LV_EVENT_CLICKED, NULL); // 有的话加上

    lv_obj_add_event_cb(s_main_page, load_page_cb, LV_EVENT_ALL, NULL);
    return s_main_page;
}

lv_obj_t *video_page_create(const char *path, bool is_dir, bool loop)
{
    // 1) 新建一个 screen（黑底，禁滚动）
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // 2) 顶部工具条（返回 + 标题）
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *btn = lv_btn_create(bar);
    lv_obj_set_size(btn, 64, 36);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(btn, video_back_btn_cb, LV_EVENT_CLICKED, scr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_center(lbl);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Video");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // 3) 保存上下文
    video_page_ctx_t *ctx = (video_page_ctx_t *)lv_mem_alloc(sizeof(video_page_ctx_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->is_dir = is_dir;
    ctx->loop = loop;
    ctx->back_btn = btn;
    ctx->title = title;
    snprintf(ctx->path, sizeof(ctx->path), "%s", path ? path : "");

    lv_obj_add_event_cb(scr, video_page_delete_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(scr, video_gesture_cb, LV_EVENT_ALL, ctx); // 支持下滑返回

    // 4) 立即加载为当前 screen，然后启动播放（此时 lv_scr_act() 就是这个 scr）
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, true);

    if (is_dir)
    {
        // 播放列表
        if (!avi_playlist_start(ctx->path, loop))
        {
            ESP_LOGE(TAG, "avi_playlist_start(%s) failed", ctx->path);
        }
    }
    else
    {
        // 单文件
        if (!avi_play_start(ctx->path))
        {
            ESP_LOGE(TAG, "avi_play_start(%s) failed", ctx->path);
        }
    }

    return scr;
}

// 返回按钮：停止播放 → 回到上一个 screen（这里简单回到默认主屏，按你项目可替换）
static void video_back_btn_cb(lv_event_t *e)
{
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_user_data(e);
    LV_UNUSED(scr);

    // 停止播放并清理（内部已在 UI 锁里删 canvas）
    avi_playlist_stop();        // 如果是列表，先让任务退出
    avi_play_stop_and_deinit(); // 通用停止/清理

    // 切回主界面（按你项目实际改，比如 ui_main_create()）
    // 这里示例：创建一个空白 screen
    lv_obj_t *home = page_main_create();
    lv_scr_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 120, 0, true);
}

// 下滑返回（与相册一致）
static void video_gesture_cb(lv_event_t *e)
{
    static lv_point_t p0;
    static bool pressed = false;
    lv_event_code_t code = lv_event_get_code(e);

    switch (code)
    {
    case LV_EVENT_PRESSED:
    {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev)
            lv_indev_get_point(indev, &p0);
        pressed = true;
        break;
    }
    case LV_EVENT_RELEASED:
    {
        if (!pressed)
            break;
        pressed = false;
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev)
            break;
        lv_point_t p1;
        lv_indev_get_point(indev, &p1);

        int dx = p1.x - p0.x;
        int dy = p1.y - p0.y;
        int thr = 20;
        lv_obj_t *target = lv_event_get_target(e);
        if (target)
        {
            int w = lv_obj_get_width(target);
            if (w / 20 > thr)
                thr = w / 20;
        }
        if (abs(dy) > abs(dx) && dy > thr)
        {
            // 等效“返回”
            video_back_btn_cb(e);
        }
        break;
    }
    default:
        break;
    }
}

// 安全兜底：如果视频页被删除，确保播放器清理
static void video_page_delete_cb(lv_event_t *e)
{
    video_page_ctx_t *ctx = (video_page_ctx_t *)lv_event_get_user_data(e);
    avi_playlist_stop();
    avi_play_stop_and_deinit();
    if (ctx)
        lv_mem_free(ctx);
}