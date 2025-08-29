// video_player.c  — ESP32P4 + LVGL v8 线程安全双核播放器（MJPEG / AVI）
// 解码：Core 1     LVGL渲染：Core 0
// 依赖：avi_player.h (你的已有库) / esp_jpeg_dec / esp_lvgl_port

#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "avi_player.h"

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

#include "esp_lvgl_port.h"

#define LVGL_LOCK(ms) lvgl_port_lock((ms))
#define LVGL_UNLOCK() lvgl_port_unlock()

// 退出/停止标志：返回按钮按下后立刻置位
static volatile bool s_stop_requested = false;

// -------------------- 播放列表状态 --------------------
static char **s_avi_list = NULL;
static int s_avi_count = 0;
static bool s_loop_playlist = true;
static volatile bool s_playing = false; // avi_end_cb() 置 false

// -------------------- 播放器/解码器 -------------------
static jpeg_dec_handle_t s_jpeg = NULL;
static avi_player_handle_t s_avi = NULL;

// -------------------- 画面与缓冲 ----------------------
static lv_obj_t *s_canvas = NULL;           // 仅 UI 线程访问
static lv_color_t *s_buf[2] = {NULL, NULL}; // 帧双缓冲（内部优先，不足回落 PSRAM）

static int s_w = 0, s_h = 0;                                  // 当前帧尺寸（首帧/切片确定）
static volatile int s_front = 0;                              // 显示中的缓冲下标（仅 UI 修改）
static volatile int s_back = 1;                               // 解码写入的缓冲下标（解码线程写）
static volatile bool s_frame_ready = false;                   // 解码线程置位，UI线程消费
static portMUX_TYPE s_buf_mux = portMUX_INITIALIZER_UNLOCKED; // 轻量临界区保护

// 临时解码缓冲（尺寸不一致时才用，放 PSRAM/对齐分配）
static uint8_t *s_decode_buf = NULL;
static int s_decode_cap = 0;

// -------------------- 前向声明（UI线程回调）------------
// static void ui_swap_and_invalidate(void *unused);
// static void ui_ensure_canvas_and_bind(void *unused);
// static void ui_destroy_canvas(void *unused);
/* Forward declarations for stop APIs used before their definitions */
void avi_playlist_stop(void);
void avi_play_stop_and_deinit(void);


// =====================================================
// 小工具：安全对齐分配（给 JPEG 临时缓冲用）
// =====================================================
static void *safe_calloc_align(size_t n, size_t align)
{
    void *p = jpeg_calloc_align(n, align);
    if (!p)
        p = calloc(1, n);
    return p;
}
static void safe_free_align(void *p)
{
    if (p)
        jpeg_free_align(p);
}

// =====================================================
// 帧缓冲分配：内部RAM+DMA优先，不足回落PSRAM
// =====================================================
static lv_color_t *alloc_framebuf(size_t pixels, bool *in_internal_out)
{
    lv_color_t *p = heap_caps_aligned_calloc(16, pixels, sizeof(lv_color_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (p)
    {
        if (in_internal_out)
            *in_internal_out = true;
        return p;
    }

    p = heap_caps_aligned_calloc(16, pixels, sizeof(lv_color_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (in_internal_out)
        *in_internal_out = false;
    return p;
}

static bool avi_canvas_alloc_buffers(size_t pixels)
{
    bool b0 = false, b1 = false;

    if (s_buf[0])
    {
        heap_caps_free(s_buf[0]);
        s_buf[0] = NULL;
    }
    if (s_buf[1])
    {
        heap_caps_free(s_buf[1]);
        s_buf[1] = NULL;
    }

    s_buf[0] = alloc_framebuf(pixels, &b0);
    s_buf[1] = alloc_framebuf(pixels, &b1);

    if (!s_buf[0] || !s_buf[1])
    {
        if (s_buf[0])
        {
            heap_caps_free(s_buf[0]);
            s_buf[0] = NULL;
        }
        if (s_buf[1])
        {
            heap_caps_free(s_buf[1]);
            s_buf[1] = NULL;
        }
        printf("no INTERNAL/PSRAM frame buffers, need %u bytes each\n",
               (unsigned)(pixels * sizeof(lv_color_t)));
        return false;
    }

    s_front = 0;
    s_back = 1;
    s_frame_ready = false;

    printf("framebuf0 in %s, framebuf1 in %s\n",
           b0 ? "INTERNAL" : "PSRAM",
           b1 ? "INTERNAL" : "PSRAM");
    return true;
}

// =====================================================
// 目录工具：大小写不敏感扩展名 & 列表释放
// =====================================================
static bool has_ext(const char *name, const char *ext)
{
    const char *p = strrchr(name, '.');
    if (!p)
        return false;
    while (*ext && *p)
    {
        char a = (char)tolower((unsigned char)*p++);
        char b = (char)tolower((unsigned char)*ext++);
        if (a != b)
            return false;
    }
    return *p == '\0' && *ext == '\0';
}

static void free_list(char **list, int n)
{
    if (!list)
        return;
    for (int i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

static esp_err_t build_avi_list(const char *dir_path)
{
    if (s_avi_list)
    {
        free_list(s_avi_list, s_avi_count);
        s_avi_list = NULL;
    }
    s_avi_count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        printf("opendir(%s) failed\n", dir_path);
        return ESP_FAIL;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_name[0] == '.')
            continue;
        if (has_ext(ent->d_name, ".avi"))
            s_avi_count++;
    }
    if (s_avi_count == 0)
    {
        closedir(dir);
        printf("no avi in %s\n", dir_path);
        return ESP_FAIL;
    }

    s_avi_list = (char **)calloc((size_t)s_avi_count, sizeof(char *));
    if (!s_avi_list)
    {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    rewinddir(dir);
    int idx = 0;
    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_name[0] == '.')
            continue;
        if (!has_ext(ent->d_name, ".avi"))
            continue;

        size_t dlen = strlen(dir_path), flen = strlen(ent->d_name);
        bool has_sep = (dlen > 0 && (dir_path[dlen - 1] == '/' || dir_path[dlen - 1] == '\\'));
        size_t need = dlen + (has_sep ? 0 : 1) + flen + 1;
        char *full = (char *)malloc(need);
        if (!full)
        {
            closedir(dir);
            free_list(s_avi_list, idx);
            s_avi_list = NULL;
            s_avi_count = 0;
            return ESP_ERR_NO_MEM;
        }

        if (has_sep)
            snprintf(full, need, "%s%s", dir_path, ent->d_name);
        else
            snprintf(full, need, "%s/%s", dir_path, ent->d_name);

        s_avi_list[idx++] = full;
        if (idx >= s_avi_count)
            break;
    }
    closedir(dir);

    for (int i = 0; i < s_avi_count; i++)
        printf("AVI[%d/%d]: %s\n", i + 1, s_avi_count, s_avi_list[i]);
    return ESP_OK;
}

// =====================================================
// JPEG 解码器一次性初始化
// =====================================================
static bool jpeg_init_once(void)
{
    if (s_jpeg)
        return true;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    return (jpeg_dec_open(&cfg, &s_jpeg) == JPEG_ERR_OK);
}

// =====================================================
// 居中贴图到 back（尽量避免，只有尺寸不匹配才走）
// 使用 s_w/s_h，而不是固定宏
// =====================================================
static void blit_center_rgb565(const uint8_t *rgb565, int img_w, int img_h)
{
    if (!s_buf[s_back] || s_w <= 0 || s_h <= 0)
        return;

    memset(s_buf[s_back], 0, (size_t)s_w * s_h * sizeof(lv_color_t));

    int copy_w = img_w, copy_h = img_h;
    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = 0, dst_y0 = 0;

    if (copy_w > s_w)
    {
        src_x0 = (copy_w - s_w) / 2;
        copy_w = s_w;
    }
    else
    {
        dst_x0 = (s_w - copy_w) / 2;
    }

    if (copy_h > s_h)
    {
        src_y0 = (copy_h - s_h) / 2;
        copy_h = s_h;
    }
    else
    {
        dst_y0 = (s_h - copy_h) / 2;
    }

    for (int y = 0; y < copy_h; y++)
    {
        const uint8_t *src = rgb565 + ((size_t)(src_y0 + y) * img_w + src_x0) * 2;
        lv_color_t *dst = s_buf[s_back] + ((size_t)(dst_y0 + y) * s_w + dst_x0);
        memcpy(dst, src, (size_t)copy_w * 2);
    }
}

// =====================================================
// UI 线程回调：创建/绑定画布；交换前后缓冲；销毁画布
// =====================================================
// static void ui_ensure_canvas_and_bind(void *unused)
// {
//     (void)unused;
//     if (!s_canvas) {
//         s_canvas = lv_canvas_create(lv_scr_act());
//         lv_obj_center(s_canvas);
//     }
//     lv_canvas_set_buffer(s_canvas, s_buf[s_front], s_w, s_h, LV_IMG_CF_TRUE_COLOR);
//     lv_obj_invalidate(s_canvas);
// }

// static void ui_swap_and_invalidate(void *unused)
// {
//     (void)unused;
//     if (!s_canvas || !s_buf[0] || !s_buf[1]) return;

//     portENTER_CRITICAL(&s_buf_mux);
//     if (!s_frame_ready) { portEXIT_CRITICAL(&s_buf_mux); return; }
//     int old_front = s_front;
//     s_front = s_back;
//     s_back  = old_front;
//     s_frame_ready = false;
//     portEXIT_CRITICAL(&s_buf_mux);

//     lv_canvas_set_buffer(s_canvas, s_buf[s_front], s_w, s_h, LV_IMG_CF_TRUE_COLOR);
//     lv_obj_invalidate(s_canvas);
// }

// static void ui_destroy_canvas(void *unused)
// {
//     (void)unused;
//     if (s_canvas) { lv_obj_del(s_canvas); s_canvas=NULL; }
// }

// =====================================================
// 视频回调（解码线程 / Core 1）— 不调用任何 lv_*
// =====================================================
static void video_cb(frame_data_t *frame, void *arg)
{
    (void)arg;

    /* —— 1) 立刻可退出：返回/停止时不再做任何工作 —— */
    if (s_stop_requested) return;

    /* —— 2) 基本校验 —— */
    if (!frame || frame->type != FRAME_TYPE_VIDEO || !frame->data || frame->data_bytes == 0) return;
    if (!jpeg_init_once()) return;

    /* —— 3) 读取 JPEG 头 —— */
    jpeg_dec_io_t io = {
        .inbuf     = frame->data,
        .inbuf_len = (int)frame->data_bytes,
        .outbuf    = NULL,
    };
    jpeg_dec_header_info_t hi;
    if (jpeg_dec_parse_header(s_jpeg, &io, &hi) != JPEG_ERR_OK) return;

    /* —— 4) 首帧 / 分辨率变化：重配双缓冲，并在 UI 锁内创建/绑定 canvas —— */
    if (s_w != (int)hi.width || s_h != (int)hi.height || !s_buf[0] || !s_buf[1]) {
        s_w = (int)hi.width;
        s_h = (int)hi.height;
        size_t pixels = (size_t)s_w * s_h;

        /* 你已有的缓冲分配函数；若没有请换成你现有的分配逻辑 */
        if (!avi_canvas_alloc_buffers(pixels)) return;

        /* 在 UI 锁内：如无则创建 canvas，并绑定当前 front 缓冲 */
        if (!lvgl_port_lock(pdMS_TO_TICKS(5))) return;     /* try-lock，拿不到就丢帧 */
        if (!s_stop_requested) {
            if (!s_canvas) {
                s_canvas = lv_canvas_create(lv_scr_act());
                lv_obj_center(s_canvas);
            }
            lv_canvas_set_buffer(s_canvas, s_buf[s_front], s_w, s_h, LV_IMG_CF_TRUE_COLOR);
            lv_obj_invalidate(s_canvas);
        }
        lvgl_port_unlock();

        s_decode_cap = 0;   /* 触发临时解码缓冲按需重配 */
    }

    /* —— 5) 计算输出缓冲需求 —— */
    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(s_jpeg, &out_len) != JPEG_ERR_OK || out_len <= 0) return;

    const size_t back_bytes = (size_t)s_w * s_h * sizeof(lv_color_t);

    /* —— 6A) 零拷贝路径：直接解码到 back 缓冲 —— */
    if (out_len == (int)back_bytes) {
        io.outbuf = (uint8_t *)s_buf[s_back];
        if (jpeg_dec_process(s_jpeg, &io) != JPEG_ERR_OK) return;

        /* 在 UI 锁内：交换 front/back、重绑 canvas、请求重绘 */
        if (!lvgl_port_lock(pdMS_TO_TICKS(5))) return;     /* 拿不到锁=丢这一帧 */
        if (!s_stop_requested && s_canvas) {
            int old_front = s_front;
            s_front = s_back;
            s_back  = old_front;

            lv_canvas_set_buffer(s_canvas, s_buf[s_front], s_w, s_h, LV_IMG_CF_TRUE_COLOR);
            lv_obj_invalidate(s_canvas);
        }
        lvgl_port_unlock();
        return;
    }

    /* —— 6B) 尺寸/采样不一致：解码到临时缓冲 → 贴到 back → UI 锁内换前台 —— */
    if (out_len > s_decode_cap) {
        if (s_decode_buf) { safe_free_align(s_decode_buf); s_decode_buf = NULL; }
        s_decode_buf = (uint8_t *)safe_calloc_align((size_t)out_len, 16);
        if (!s_decode_buf) { printf("no mem decode_buf %d\n", out_len); return; }
        s_decode_cap = out_len;
    }

    io.outbuf = s_decode_buf;
    if (jpeg_dec_process(s_jpeg, &io) != JPEG_ERR_OK) return;

    /* 把临时解码帧居中贴到 back（不涉及 LVGL） */
    blit_center_rgb565(s_decode_buf, (int)hi.width, (int)hi.height);

    /* 在 UI 锁内：交换 front/back、重绑 canvas、请求重绘 */
    if (!lvgl_port_lock(pdMS_TO_TICKS(5))) return;
    if (!s_stop_requested && s_canvas) {
        int old_front = s_front;
        s_front = s_back;
        s_back  = old_front;

        lv_canvas_set_buffer(s_canvas, s_buf[s_front], s_w, s_h, LV_IMG_CF_TRUE_COLOR);
        lv_obj_invalidate(s_canvas);
    }
    lvgl_port_unlock();
}

// =====================================================
// 音频回调/时钟（按你实际 I2S 实现）
// =====================================================
static void audio_cb(frame_data_t *data, void *arg)
{
    (void)arg;
    if (!data || data->type != FRAME_TYPE_AUDIO || !data->data || data->data_bytes == 0)
        return;
    // 建议：i2s_write(..., 50ms超时)，满了就丢，避免阻塞视频回调
}

static void my_audio_set_clock_cb(uint32_t rate, uint32_t bits, uint32_t ch, void *arg)
{
    (void)arg;
    if (rate == 0)
        rate = 44100;
    if (bits == 0)
        bits = 16;
    // TODO: 配置你的 I2S/Codec
}

// =====================================================
// 播放结束回调
// =====================================================
static void avi_end_cb(void *arg)
{
    (void)arg;
    s_playing = false;
}

// =====================================================
// 对外：播放单文件
// =====================================================
bool avi_play_start(const char *avi_path)
{

    s_stop_requested = false;

    avi_player_config_t cfg = {
        .buffer_size = 384 * 1024, // 256K~512K 视内存而定
        .video_cb = video_cb,
        .audio_cb = audio_cb,
        .audio_set_clock_cb = my_audio_set_clock_cb,
        .avi_play_end_cb = avi_end_cb,
        .priority = 7,
        .coreID = 1, // 解码在 Core 1
        .user_data = NULL,
        .stack_size = 12 * 1024,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .stack_in_psram = false,
#endif
    };

    if (avi_player_init(cfg, &s_avi) != ESP_OK)
    {
        printf("avi_player_init failed\n");
        return false;
    }
    if (avi_player_play_from_file(s_avi, avi_path) != ESP_OK)
    {
        printf("avi play failed: %s\n", avi_path);
        avi_player_deinit(s_avi);
        s_avi = NULL;
        return false;
    }
    return true;
}

// =====================================================
// 对外：停止并清理
// =====================================================
static void video_back_btn_cb(lv_event_t *e)
{
    // 先让播放线程退出（这一步内部会置 s_stop_requested、停解复用、删画布）
    avi_playlist_stop();            // 若用了目录播放
    avi_play_stop_and_deinit();     // 通用停止/清理

    // 再切屏（这时已经没有任何 video_cb 在触碰 LVGL 了）
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_scr_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 120, 0, true);
}

// =====================================================
// 播放列表任务（Core 1）
// =====================================================
static void avi_playlist_task(void *param)
{
    const char *dir_path = (const char *)param;

    if (build_avi_list(dir_path) != ESP_OK)
    {
        printf("no playable avi in %s\n", dir_path);
        vTaskDelete(NULL);
        return;
    }

    avi_player_config_t cfg = {
        .buffer_size = 384 * 1024,
        .video_cb = video_cb,
        .audio_cb = audio_cb,
        .audio_set_clock_cb = my_audio_set_clock_cb,
        .avi_play_end_cb = avi_end_cb,
        .priority = 7,
        .coreID = 1, // 解码在 Core 1
        .user_data = NULL,
        .stack_size = 12 * 1024,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .stack_in_psram = false,
#endif
    };
    if (avi_player_init(cfg, &s_avi) != ESP_OK)
    {
        printf("avi_player_init failed\n");
        vTaskDelete(NULL);
        return;
    }

    for (;;)
    {
        for (int i = 0; i < s_avi_count; i++)
        {
            const char *path = s_avi_list[i];
            printf("\n=== play: %s (%d/%d) ===\n", path, i + 1, s_avi_count);

            s_playing = true;
            if (avi_player_play_from_file(s_avi, path) != ESP_OK)
            {
                printf("play failed: %s\n", path);
                s_playing = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            while (s_playing)
                vTaskDelay(pdMS_TO_TICKS(10));
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        if (!s_loop_playlist)
            break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    avi_player_play_stop(s_avi);
    avi_player_deinit(s_avi);
    s_avi = NULL;

    if (s_avi_list)
    {
        free_list(s_avi_list, s_avi_count);
        s_avi_list = NULL;
    }
    s_avi_count = 0;

    vTaskDelete(NULL);
}

// =====================================================
// 对外：开始播放文件夹（loop=true 循环）
// =====================================================
bool avi_playlist_start(const char *dir_path, bool loop)
{
    s_stop_requested = false;

    s_loop_playlist = loop;
    BaseType_t ok = xTaskCreatePinnedToCore(
        avi_playlist_task, "avi_playlist_task",
        12 * 1024, (void *)dir_path, 7, NULL, 1 // Core 1
    );
    return (ok == pdPASS);
}

/* Stop current playback (single file or playlist item) and free resources safely */
void avi_play_stop_and_deinit(void)
{
    /* Signal all callbacks to early-exit */
    s_stop_requested = true;

    /* Stop and deinit AVI player if running */
    if (s_avi) {
        avi_player_play_stop(s_avi);
        avi_player_deinit(s_avi);
        s_avi = NULL;
    }

    /* Destroy LVGL canvas under UI lock */
    if (LVGL_LOCK(pdMS_TO_TICKS(50))) {
        if (s_canvas) {
            lv_obj_t *tmp = s_canvas;
            s_canvas = NULL;          /* clear pointer first to avoid late callbacks touching it */
            lv_obj_del(tmp);
        }
        LVGL_UNLOCK();
    }

    /* Free frame buffers */
    if (s_buf[0]) { heap_caps_free(s_buf[0]); s_buf[0] = NULL; }
    if (s_buf[1]) { heap_caps_free(s_buf[1]); s_buf[1] = NULL; }

    /* Free temporary decode buffer */
    if (s_decode_buf) { safe_free_align(s_decode_buf); s_decode_buf = NULL; s_decode_cap = 0; }

    /* Reset state */
    s_w = 0; s_h = 0;
    s_front = 0; s_back = 1;
    s_frame_ready = false;
}


// =====================================================
// 对外：停止播放列表
// =====================================================
void avi_playlist_stop(void)
{
    s_loop_playlist = false; // 让任务跑完一轮退出
    s_playing = false;       // 若正在播当前文件，提前跳出等待
}