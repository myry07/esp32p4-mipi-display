#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "avi_player.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>   // for tolower()

// ===== 播放列表状态 =====
static char   **s_avi_list      = NULL;
static int      s_avi_count     = 0;
static bool     s_loop_playlist = true;
static volatile bool s_playing  = false;   // 由 avi_end_cb() 置 false

// ===== 兼容常量（LVGL v8 没有 LV_COLOR_FORMAT_RGB565）=====
#ifndef LV_COLOR_FORMAT_RGB565
#define LV_COLOR_FORMAT_RGB565 LV_IMG_CF_TRUE_COLOR
#endif

// ===== 根据你的片源/屏幕调整画布大小 =====
#define CANVAS_W   240
#define CANVAS_H   320

// ===== 内部状态 =====
static lv_obj_t    *s_canvas = NULL;
static lv_color_t  *s_buf[2] = {NULL, NULL};
static volatile int s_front = 0;   // 正在显示的下标
static volatile int s_back  = 1;   // 填充用下标
static volatile bool s_frame_ready = false;

static jpeg_dec_handle_t s_jpeg = NULL;
static avi_player_handle_t s_avi = NULL;

// 尺寸不匹配时的临时解码缓冲（只分配一次）
static uint8_t *s_decode_buf = NULL;
static int      s_decode_cap = 0;

// === 小工具：安全分配/释放（优先对齐）===
static void *safe_calloc_align(size_t n, size_t align)
{
    void *p = jpeg_calloc_align(n, align);
    if(!p) p = calloc(1, n); // 兜底
    return p;
}
static void safe_free_align(void *p){
    if(p) jpeg_free_align(p);
}

// === UI 线程里执行：交换前后缓冲并请求重绘（不做整帧 memcpy）===
static void ui_swap_and_invalidate(void *unused)
{
    (void)unused;
    if(!s_canvas || !s_buf[0] || !s_buf[1]) return;
    if(!s_frame_ready) return;

    // 交换前后缓冲索引
    int old_front = s_front;
    s_front = s_back;
    s_back  = old_front;

    // 让 canvas 指向新的前台缓冲（尺寸不变，开销极小）
    lv_canvas_set_buffer(s_canvas, s_buf[s_front], CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);

    lv_obj_invalidate(s_canvas);
    s_frame_ready = false;
}

// === 初始化：创建 canvas + 分配双缓冲 ===
static bool avi_canvas_init_once(void)
{
    if(s_canvas) return true;
    if(lv_disp_get_default() == NULL) { printf("No LVGL display\n"); return false; }

    s_buf[0] = (lv_color_t *)safe_calloc_align((size_t)CANVAS_W * CANVAS_H * sizeof(lv_color_t), 16);
    s_buf[1] = (lv_color_t *)safe_calloc_align((size_t)CANVAS_W * CANVAS_H * sizeof(lv_color_t), 16);
    if(!s_buf[0] || !s_buf[1]) { printf("no mem canvas buffers\n"); return false; }

    s_canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(s_canvas, s_buf[s_front], CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    return true;
}

// === JPEG 解码器一次性初始化 ===
static bool jpeg_init_once(void)
{
    if(s_jpeg) return true;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    return (jpeg_dec_open(&cfg, &s_jpeg) == JPEG_ERR_OK);
}

// === 把一帧 RGB565 居中贴到 s_buf[s_back]（大图裁剪，小图 letterbox）===
static void blit_center_rgb565(const uint8_t *rgb565, int img_w, int img_h)
{
    // 清背景黑
    memset(s_buf[s_back], 0, (size_t)CANVAS_W * CANVAS_H * sizeof(lv_color_t));

    int copy_w = img_w;
    int copy_h = img_h;
    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = 0, dst_y0 = 0;

    if(copy_w > CANVAS_W) { src_x0 = (copy_w - CANVAS_W)/2; copy_w = CANVAS_W; }
    else { dst_x0 = (CANVAS_W - copy_w)/2; }

    if(copy_h > CANVAS_H) { src_y0 = (copy_h - CANVAS_H)/2; copy_h = CANVAS_H; }
    else { dst_y0 = (CANVAS_H - copy_h)/2; }

    for(int y=0; y<copy_h; y++){
        const uint8_t *src = rgb565 + ((size_t)(src_y0 + y) * img_w + src_x0) * 2;
        lv_color_t *dst = s_buf[s_back] + ((size_t)(dst_y0 + y) * CANVAS_W + dst_x0);
        memcpy(dst, src, (size_t)copy_w * 2);
    }
}

// 简单工具：判断扩展名（大小写不敏感）
static bool has_ext(const char *name, const char *ext) {
    const char *p = strrchr(name, '.');
    if(!p) return false;
    while(*ext && *p) {
        char a = (char)tolower((unsigned char)*p++);
        char b = (char)tolower((unsigned char)*ext++);
        if(a != b) return false;
    }
    return *p=='\0' && *ext=='\0';
}

// 扫描目录，构建 AVI 文件列表（避免用 d_type）
static esp_err_t build_avi_list(const char *dir_path)
{
    // 清理旧列表
    if(s_avi_list){
        for(int i=0;i<s_avi_count;i++) free(s_avi_list[i]);
        free(s_avi_list);
        s_avi_list=NULL;
    }
    s_avi_count = 0;

    DIR *dir = opendir(dir_path);
    if(!dir){
        printf("opendir(%s) failed\n", dir_path);
        return ESP_FAIL;
    }

    // 先统计数量
    struct dirent *ent;
    while((ent = readdir(dir)) != NULL){
        if(ent->d_name[0]=='.') continue;
        if(has_ext(ent->d_name, ".avi")) s_avi_count++;
    }
    if(s_avi_count==0){
        closedir(dir);
        printf("no avi in %s\n", dir_path);
        return ESP_FAIL;
    }

    s_avi_list = (char**)calloc((size_t)s_avi_count, sizeof(char*));
    if(!s_avi_list){ closedir(dir); return ESP_ERR_NO_MEM; }

    // 再次遍历填充
    rewinddir(dir);
    int idx=0;
    while((ent = readdir(dir)) != NULL){
        if(ent->d_name[0]=='.') continue;
        if(!has_ext(ent->d_name, ".avi")) continue;

        size_t dlen = strlen(dir_path);
        size_t flen = strlen(ent->d_name);
        bool has_sep = (dlen>0 && (dir_path[dlen-1]=='/' || dir_path[dlen-1]=='\\'));
        size_t need = dlen + (has_sep?0:1) + flen + 1;

        char *full = (char*)malloc(need);
        if(!full){ closedir(dir); return ESP_ERR_NO_MEM; }
        if(has_sep) snprintf(full, need, "%s%s", dir_path, ent->d_name);
        else        snprintf(full, need, "%s/%s", dir_path, ent->d_name);

        s_avi_list[idx++] = full;
        if(idx>=s_avi_count) break;
    }
    closedir(dir);

    // （可选）打印列表
    for(int i=0;i<s_avi_count;i++){
        printf("AVI[%d/%d]: %s\n", i+1, s_avi_count, s_avi_list[i]);
    }
    return ESP_OK;
}

// === avi_player 的视频回调：MJPEG -> RGB565 -> back 缓冲 ===
static void video_cb(frame_data_t *frame, void *arg)
{
    (void)arg;
    if(!frame || frame->type != FRAME_TYPE_VIDEO || !frame->data || frame->data_bytes == 0) return;
    if(!avi_canvas_init_once()) return;
    if(!jpeg_init_once()) return;

    jpeg_dec_io_t io = {
        .inbuf = frame->data,
        .inbuf_len = (int)frame->data_bytes,
        .outbuf = NULL,
    };
    jpeg_dec_header_info_t hi;
    if(jpeg_dec_parse_header(s_jpeg, &io, &hi) != JPEG_ERR_OK) return;

    int out_len = 0;
    if(jpeg_dec_get_outbuf_len(s_jpeg, &out_len) != JPEG_ERR_OK || out_len <= 0) return;

    // 情况 A：尺寸完全匹配 → 直接解码到 back（零拷贝、零临时内存）
    if ((int)hi.width == CANVAS_W && (int)hi.height == CANVAS_H) {
        io.outbuf = (uint8_t*)s_buf[s_back];
        if (jpeg_dec_process(s_jpeg, &io) == JPEG_ERR_OK) {
            s_frame_ready = true;
            lv_async_call(ui_swap_and_invalidate, NULL);
        }
        return;
    }

    // 情况 B：尺寸不匹配 → 仅在首次/尺寸变化时按需分配一次解码缓冲
    if (out_len > s_decode_cap) {
        if (s_decode_buf) { safe_free_align(s_decode_buf); s_decode_buf = NULL; }
        s_decode_buf = (uint8_t*)safe_calloc_align((size_t)out_len, 16);
        if (!s_decode_buf) { printf("no mem decode_buf %d\n", out_len); return; }
        s_decode_cap = out_len;
    }

    io.outbuf = s_decode_buf;
    if (jpeg_dec_process(s_jpeg, &io) == JPEG_ERR_OK) {
        blit_center_rgb565(s_decode_buf, (int)hi.width, (int)hi.height);
        s_frame_ready = true;
        lv_async_call(ui_swap_and_invalidate, NULL);
    }
}

// === 音频：按你的 I2S/Codec 来，这里给占位 ===
static void audio_cb(frame_data_t *data, void *arg)
{
    (void)arg;
    if(!data || data->type != FRAME_TYPE_AUDIO || !data->data || data->data_bytes == 0) return;
    // 建议：写 I2S 时用小超时（例如 40~50ms），缓冲满则丢，避免阻塞视频回调
    // size_t written = 0;
    // i2s_write(..., data->data, data->data_bytes, &written, pdMS_TO_TICKS(50));
}

static void my_audio_set_clock_cb(uint32_t rate, uint32_t bits, uint32_t ch, void *arg)
{
    (void)arg;
    if(rate == 0) rate = 44100;
    if(bits == 0) bits = 16;
    // TODO: 配置你的 I2S/Codec
}

// === 播放结束回调：用于单文件或播放列表（统一一个版本，避免重定义）===
static void avi_end_cb(void *arg)
{
    (void)arg;
    s_playing = false; // 播放列表模式会用到；单文件模式也无害
}

// === 对外接口：开始播放一个 AVI 文件 ===
bool avi_play_start(const char *avi_path)
{
    if(!avi_canvas_init_once()) return false;

    avi_player_config_t cfg = {
        .buffer_size = 256 * 1024,     // 可酌情提到 384K/512K（看内存）
        .video_cb = video_cb,
        .audio_cb = audio_cb,
        .audio_set_clock_cb = my_audio_set_clock_cb,
        .avi_play_end_cb = avi_end_cb,
        .priority = 7,
        .coreID = 0,                   // 若 LVGL 在 0 核，可把 avi 放到 1 核
        .user_data = NULL,
        .stack_size = 12 * 1024,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .stack_in_psram = false,
#endif
    };

    if(avi_player_init(cfg, &s_avi) != ESP_OK) {
        printf("avi_player_init failed\n");
        return false;
    }
    if(avi_player_play_from_file(s_avi, avi_path) != ESP_OK) {
        printf("avi play failed: %s\n", avi_path);
        avi_player_deinit(s_avi);
        s_avi = NULL;
        return false;
    }
    return true;
}

// === 对外接口：停止并清理 ===
void avi_play_stop_and_deinit(void)
{
    if(s_avi){
        avi_player_play_stop(s_avi);
        avi_player_deinit(s_avi);
        s_avi = NULL;
    }
    if(s_jpeg){
        jpeg_dec_close(s_jpeg);
        s_jpeg = NULL;
    }
    if(s_canvas){
        lv_obj_del(s_canvas);
        s_canvas = NULL;
    }
    if(s_buf[0]) { safe_free_align(s_buf[0]); s_buf[0]=NULL; }
    if(s_buf[1]) { safe_free_align(s_buf[1]); s_buf[1]=NULL; }
    if(s_decode_buf){ safe_free_align(s_decode_buf); s_decode_buf=NULL; s_decode_cap=0; }
}

// ====== 播放列表 ======

// 循环播放任务
static void avi_playlist_task(void *param)
{
    const char *dir_path = (const char*)param;

    if(build_avi_list(dir_path) != ESP_OK){
        printf("no playable avi in %s\n", dir_path);
        vTaskDelete(NULL);
        return;
    }

    // 确保画布/JPEG 已初始化
    if(!avi_canvas_init_once() || !jpeg_init_once()){
        vTaskDelete(NULL);
        return;
    }

    // 初始化 avi 播放器
    avi_player_config_t cfg = {
        .buffer_size = 384 * 1024,                // 可根据内存调整 256K~512K
        .video_cb = video_cb,
        .audio_cb = audio_cb,
        .audio_set_clock_cb = my_audio_set_clock_cb,
        .avi_play_end_cb = avi_end_cb,            // 统一回调
        .priority = 7,
        .coreID = 1,                              // 若 LVGL 在 0 核，放 1 核更稳
        .user_data = NULL,
        .stack_size = 12 * 1024,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .stack_in_psram = false,
#endif
    };
    if(avi_player_init(cfg, &s_avi) != ESP_OK){
        printf("avi_player_init failed\n");
        vTaskDelete(NULL);
        return;
    }

    for(;;){
        for(int i=0;i<s_avi_count;i++){
            const char *path = s_avi_list[i];
            printf("\n=== play: %s (%d/%d) ===\n", path, i+1, s_avi_count);

            s_playing = true;
            if(avi_player_play_from_file(s_avi, path) != ESP_OK){
                printf("play failed: %s\n", path);
                s_playing = false;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            // 等该文件结束（avi_end_cb 会置 s_playing=false）
            while(s_playing){
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // 小间隔
        }

        if(!s_loop_playlist) break; // 不循环则退出
        vTaskDelay(pdMS_TO_TICKS(200)); // 循环之间的缓冲
    }

    // 清理
    avi_player_play_stop(s_avi);
    avi_player_deinit(s_avi);
    s_avi = NULL;

    // 列表释放
    if(s_avi_list){
        for(int i=0;i<s_avi_count;i++) free(s_avi_list[i]);
        free(s_avi_list);
        s_avi_list=NULL;
    }
    s_avi_count=0;

    vTaskDelete(NULL);
}

// 对外：开始播放某文件夹（loop=true 循环）
bool avi_playlist_start(const char *dir_path, bool loop)
{
    s_loop_playlist = loop;
    BaseType_t ok = xTaskCreatePinnedToCore(
        avi_playlist_task, "avi_playlist_task",
        12*1024, (void*)dir_path, 7, NULL, 1
    );
    return (ok == pdPASS);
}

// 对外：停止播放并释放（若需要在别处调用）
void avi_playlist_stop(void)
{
    s_loop_playlist = false;   // 让任务跑完一轮退出
    s_playing = false;         // 若正在播当前文件，提前跳出等待
}