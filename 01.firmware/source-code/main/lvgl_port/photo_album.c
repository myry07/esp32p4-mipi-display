#include "lvgl.h"
#include "esp_jpeg_dec.h"

#include "esp_err.h"
#include <ctype.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

#include "ui.h"
#include "esp_log.h"

// ============================= 配置项 =============================
#define ALBUM_LOG(fmt, ...) printf("[album] " fmt "\n", ##__VA_ARGS__)
#define SWIPE_THRESHOLD_PX 20 // 左右滑判定阈值
#define JPEG_ALIGN 16         // esp_jpeg 对齐

// 复用你的全局状态
static lv_point_t touch_start_point;  // 记录起始坐标
static bool gesture_detected = false; // 标志位

// ========================== 内部状态/资源 =========================
typedef struct
{
    lv_obj_t *page;         // 相册页面（容器）
    lv_obj_t *canvas;       // 用于显示的 canvas
    lv_color_t *canvas_buf; // canvas 像素缓冲 (RGB565)

    int cw, ch; // canvas 尺寸（通常等于屏幕）
    bool loop;  // 是否循环浏览

    // 解码缓冲（尺寸不匹配时临时用；按需扩容）
    uint8_t *decode_buf;
    int decode_cap;

    // 文件列表
    char **paths;
    int count;
    int index;

    // 手势
    bool pressed;
    lv_point_t p_down;

    // JPEG 解码器句柄（复用）
    jpeg_dec_handle_t j;
} album_ctx_t;

static album_ctx_t s_ctx = {0};

static void album_page_delete_cb(lv_event_t *e);

// ========================== 小工具函数 ============================
static void *safe_calloc_align(size_t n, size_t align)
{
    void *p = jpeg_calloc_align(n, align);
    if (!p)
    {
        ALBUM_LOG("jpeg_calloc_align(%zu) failed", n);
    }
    return p;
}
static void safe_free_align(void *p)
{
    if (p)
        jpeg_free_align(p);
}
static bool has_ext_icase(const char *name, const char *ext) // ext: ".jpg" / ".jpeg"
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    while (*ext && *dot)
    {
        char a = (char)tolower((unsigned char)*dot++);
        char b = (char)tolower((unsigned char)*ext++);
        if (a != b)
            return false;
    }
    return *dot == '\0' && *ext == '\0';
}

static void free_list(char **list, int n)
{
    if (!list)
        return;
    for (int i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

// 扫描目录，收集 .jpg/.jpeg
static esp_err_t build_jpg_list(const char *path, char ***out_list, int *out_n)
{
    *out_list = NULL;
    *out_n = 0;

    DIR *dir = opendir(path);
    if (!dir)
    {
        // 支持单文件路径
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (has_ext_icase(path, ".jpg") || has_ext_icase(path, ".jpeg")))
        {
            char **one = (char **)calloc(1, sizeof(char *));
            if (!one)
                return ESP_ERR_NO_MEM;
            one[0] = strdup(path);
            if (!one[0])
            {
                free(one);
                return ESP_ERR_NO_MEM;
            }
            *out_list = one;
            *out_n = 1;
            return ESP_OK;
        }
        ALBUM_LOG("opendir(%s) failed", path);
        return ESP_FAIL;
    }

    // 统计
    int cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_name[0] == '.')
            continue;
        if (has_ext_icase(ent->d_name, ".jpg") || has_ext_icase(ent->d_name, ".jpeg"))
            cnt++;
    }
    if (cnt == 0)
    {
        closedir(dir);
        ALBUM_LOG("no jpg in %s", path);
        return ESP_FAIL;
    }

    char **list = (char **)calloc((size_t)cnt, sizeof(char *));
    if (!list)
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
        if (!(has_ext_icase(ent->d_name, ".jpg") || has_ext_icase(ent->d_name, ".jpeg")))
            continue;

        size_t dlen = strlen(path), flen = strlen(ent->d_name);
        bool has_sep = (dlen > 0 && (path[dlen - 1] == '/' || path[dlen - 1] == '\\'));
        size_t need = dlen + (has_sep ? 0 : 1) + flen + 1;
        char *full = (char *)malloc(need);
        if (!full)
        {
            closedir(dir);
            free_list(list, idx);
            return ESP_ERR_NO_MEM;
        }
        if (has_sep)
            snprintf(full, need, "%s%s", path, ent->d_name);
        else
            snprintf(full, need, "%s/%s", path, ent->d_name);

        // 轻量 SOI 校验：不是 JPEG 就丢弃
        FILE *fp = fopen(full, "rb");
        bool ok = false;
        if (fp)
        {
            unsigned char soi[2] = {0};
            if (fread(soi, 1, 2, fp) == 2 && soi[0] == 0xFF && soi[1] == 0xD8)
                ok = true;
            fclose(fp);
        }
        if (!ok)
        {
            ESP_LOGW("album", "ignore non-jpeg: %s", full);
            free(full);
            continue;
        }

        list[idx++] = full;
    }
    closedir(dir);

    if (idx == 0)
    {
        free(list);
        ALBUM_LOG("no valid jpg in %s", path);
        return ESP_FAIL;
    }
    *out_list = list;
    *out_n = idx; // ← 用实际数量
    return ESP_OK;
}

// 创建/复用 canvas（只在第一次创建）
static bool ensure_canvas(album_ctx_t *c)
{
    if (c->canvas)
        return true;
    c->canvas_buf = (lv_color_t *)safe_calloc_align((size_t)c->cw * c->ch * sizeof(lv_color_t), JPEG_ALIGN);
    if (!c->canvas_buf)
    {
        ALBUM_LOG("no mem canvas_buf");
        return false;
    }

    c->canvas = lv_canvas_create(c->page);
    lv_canvas_set_buffer(c->canvas, c->canvas_buf, c->cw, c->ch, LV_IMG_CF_TRUE_COLOR /*RGB565 in v8*/);
    lv_obj_center(c->canvas);
    return true;
}

// 把一帧 RGB565 居中贴到 canvas（大图居中裁剪，小图黑边）
static void blit_center_rgb565(album_ctx_t *c, const uint8_t *rgb565, int img_w, int img_h)
{
    // 背景清黑
    memset(c->canvas_buf, 0, (size_t)c->cw * c->ch * sizeof(lv_color_t));

    int copy_w = img_w, copy_h = img_h;
    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = 0, dst_y0 = 0;

    if (copy_w > c->cw)
    {
        src_x0 = (copy_w - c->cw) / 2;
        copy_w = c->cw;
    }
    else
    {
        dst_x0 = (c->cw - copy_w) / 2;
    }

    if (copy_h > c->ch)
    {
        src_y0 = (copy_h - c->ch) / 2;
        copy_h = c->ch;
    }
    else
    {
        dst_y0 = (c->ch - copy_h) / 2;
    }

    for (int y = 0; y < copy_h; y++)
    {
        const uint8_t *src = rgb565 + ((size_t)(src_y0 + y) * img_w + src_x0) * 2;
        lv_color_t *dst = c->canvas_buf + ((size_t)(dst_y0 + y) * c->cw + dst_x0);
        memcpy(dst, src, (size_t)copy_w * 2);
    }
    lv_obj_invalidate(c->canvas);
}

// 加载指定路径的 jpg 到 canvas
static bool load_jpg(album_ctx_t *c, const char *path)
{
    if (!ensure_canvas(c))
        return false;

    // 读文件
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        ALBUM_LOG("open %s fail", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz <= 0)
    {
        fclose(fp);
        ALBUM_LOG("size 0");
        return false;
    }

    uint8_t *jpg = (uint8_t *)safe_calloc_align((size_t)fsz, JPEG_ALIGN);
    if (!jpg)
    {
        fclose(fp);
        ALBUM_LOG("no mem jpg");
        return false;
    }
    size_t rd = fread(jpg, 1, (size_t)fsz, fp);
    fclose(fp);
    if (rd != (size_t)fsz)
    {
        safe_free_align(jpg);
        ALBUM_LOG("read fail");
        return false;
    }

    // 打开/复用 JPEG 解码器
    if (!c->j)
    {
        jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
        cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        if (jpeg_dec_open(&cfg, &c->j) != JPEG_ERR_OK)
        {
            safe_free_align(jpg);
            ALBUM_LOG("jpeg open fail");
            return false;
        }
    }

    jpeg_dec_io_t io = {.inbuf = jpg, .inbuf_len = (int)fsz, .outbuf = NULL};
    jpeg_dec_header_info_t hi;
    if (jpeg_dec_parse_header(c->j, &io, &hi) != JPEG_ERR_OK)
    {
        safe_free_align(jpg);
        ALBUM_LOG("parse hdr fail");
        return false;
    }

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(c->j, &out_len) != JPEG_ERR_OK || out_len <= 0)
    {
        safe_free_align(jpg);
        ALBUM_LOG("get out len fail");
        return false;
    }

    // 动态保证解码缓冲容量
    if (out_len > c->decode_cap)
    {
        if (c->decode_buf)
        {
            safe_free_align(c->decode_buf);
            c->decode_buf = NULL;
        }
        c->decode_buf = (uint8_t *)safe_calloc_align((size_t)out_len, JPEG_ALIGN);
        if (!c->decode_buf)
        {
            safe_free_align(jpg);
            ALBUM_LOG("no mem decode buf %d", out_len);
            return false;
        }
        c->decode_cap = out_len;
    }

    io.outbuf = c->decode_buf;
    bool ok = false;
    if (jpeg_dec_process(c->j, &io) == JPEG_ERR_OK)
    {
        blit_center_rgb565(c, c->decode_buf, (int)hi.width, (int)hi.height);
        ok = true;
    }
    else
    {
        ALBUM_LOG("jpeg decode fail");
    }

    safe_free_align(jpg);
    return ok;
}

// =========================== 事件回调 ============================
static void album_event_cb(lv_event_t *e)
{
    album_ctx_t *c = &s_ctx; // 相册全局上下文
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    switch (code)
    {
    case LV_EVENT_PRESSED:
    {
        gesture_detected = false;
        if (indev)
        {
            lv_indev_get_point(indev, &touch_start_point);
        }
        else
        {
            touch_start_point.x = touch_start_point.y = 0;
        }
        break;
    }

    case LV_EVENT_RELEASED:
    {
        if (!indev)
            break;

        lv_point_t touch_end_point = {0};
        lv_indev_get_point(indev, &touch_end_point);

        int dx = touch_end_point.x - touch_start_point.x;
        int dy = touch_end_point.y - touch_start_point.y;

        // —— 阈值：取固定阈值与屏宽5%/DPI/8 的最大值，保证不同屏幕手感一致 ——
        int thr = 15; // 你的基础阈值
        lv_obj_t *target = lv_event_get_current_target(e);
        if (target)
        {
            int w = lv_obj_get_width(target);
            if (w / 20 > thr)
                thr = w / 20; // ~5% 宽度
        }
        uint32_t dpi = lv_disp_get_dpi(NULL);
        if ((int)(dpi / 8) > thr)
            thr = (int)(dpi / 8);
        if (thr < 12)
            thr = 12;
        if (thr > 60)
            thr = 60;

        // 位移太小 → 不是滑动，交给 CLICKED 处理
        if (abs(dx) < thr && abs(dy) < thr)
            break;

        // 判断为滑动
        gesture_detected = true;

        // —— 水平优先：显著水平才认作左右滑
        if (abs(dx) >= abs(dy))
        {
            if (!c || c->count <= 0 || !c->paths)
            {
                // 没有相册内容时忽略
                break;
            }

            int next = c->index;
            if (dx > 0)
            {
                ESP_LOGI("gesture", "右滑");
                next = c->index - 1;
                if (next < 0)
                    next = c->loop ? (c->count - 1) : 0;
            }
            else
            {
                ESP_LOGI("gesture", "左滑");
                next = c->index + 1;
                if (next >= c->count)
                    next = c->loop ? 0 : (c->count - 1);
            }

            if (next != c->index)
            {
                c->index = next;
                (void)load_jpg(c, c->paths[c->index]); // 失败也不致崩
            }
        }
        else
        {
            // —— 竖向滑动 —— 下滑进入锁屏页（判空再切屏）
            if (dy > thr)
            {
                ESP_LOGI("gesture", "下滑");
                lv_obj_t *new_scr = page_lock_create();
                if (new_scr)
                {
                    // 让 LVGL 在动画完成后自动删掉旧屏，并触发 LV_EVENT_DELETE → 我们释放资源
                    lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 50, 0, true /*auto_del*/);
                }
                else
                {
                    ESP_LOGE("gesture", "page_lock_create() 返回 NULL，未切屏");
                }
            }
            else
            {
                ESP_LOGI("gesture", "上滑");
                lv_obj_t *new_scr = page_main_create();
                if (new_scr)
                {
                    // 让 LVGL 在动画完成后自动删掉旧屏，并触发 LV_EVENT_DELETE → 我们释放资源
                    lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 50, 0, true /*auto_del*/);
                }
                else
                {
                    ESP_LOGE("gesture", "page_main_create() 返回 NULL，未切屏");
                }
            }
        }
        break;
    }

    case LV_EVENT_CLICKED:
        // 如果刚才识别成滑动了，就不再触发点击逻辑
        if (gesture_detected)
            break;
        ESP_LOGI("btn", "点击事件");
        // TODO: 这里可以加“单击显示工具栏/信息”的逻辑
        break;

    default:
        break;
    }
}

// =========================== 对外接口 ============================
// 创建相册页面（dir 可以是目录或单文件 .jpg/.jpeg）
lv_obj_t *photo_album_create(const char *dir, int canvas_w, int canvas_h, bool loop)
{
    album_ctx_t *c = &s_ctx;

    // ✨ 防御：如果上次已删，这里清掉任何残留引用（不再主动 del）
    if (c->page && !lv_obj_is_valid(c->page))
        c->page = NULL;
    if (c->canvas && !lv_obj_is_valid(c->canvas))
        c->canvas = NULL;

    // 清理旧实例（如果有的话）
    if (c->page)
    {
        if (c->canvas)
        {
            // canvas_buf 由我们分配，lvgl 不会释放
            if (c->canvas_buf)
            {
                safe_free_align(c->canvas_buf);
                c->canvas_buf = NULL;
            }
            lv_obj_del(c->canvas);
            c->canvas = NULL;
        }
        lv_obj_del(c->page);
        c->page = NULL;
    }
    if (c->decode_buf)
    {
        safe_free_align(c->decode_buf);
        c->decode_buf = NULL;
        c->decode_cap = 0;
    }
    if (c->paths)
    {
        free_list(c->paths, c->count);
        c->paths = NULL;
        c->count = 0;
    }
    if (c->j)
    {
        jpeg_dec_close(c->j);
        c->j = NULL;
    }

    c->cw = canvas_w;
    c->ch = canvas_h;
    c->loop = loop;

    if (build_jpg_list(dir, &c->paths, &c->count) != ESP_OK)
    {
        ALBUM_LOG("build list fail: %s", dir);
        return NULL;
    }

    c->index = 0;
    c->page = lv_obj_create(NULL);
    lv_obj_set_size(c->page, canvas_w, canvas_h);
    lv_obj_set_style_bg_opa(c->page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(c->page, LV_OBJ_FLAG_SCROLLABLE);

    // 事件：左右滑切换
    lv_obj_add_event_cb(c->page, album_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(c->page, album_page_delete_cb, LV_EVENT_DELETE, NULL);

    // 首张
    if (!load_jpg(c, c->paths[c->index]))
    {
        ALBUM_LOG("first image load failed");
    }

    return c->page;
}

// 销毁相册
void photo_album_destroy(void)
{
    album_ctx_t *c = &s_ctx;

    if (c->canvas)
    {
        if (c->canvas_buf)
        {
            safe_free_align(c->canvas_buf);
            c->canvas_buf = NULL;
        }
        lv_obj_del(c->canvas);
        c->canvas = NULL;
    }
    if (c->page)
    {
        lv_obj_del(c->page);
        c->page = NULL;
    }
    if (c->decode_buf)
    {
        safe_free_align(c->decode_buf);
        c->decode_buf = NULL;
        c->decode_cap = 0;
    }
    if (c->paths)
    {
        free_list(c->paths, c->count);
        c->paths = NULL;
        c->count = 0;
    }
    if (c->j)
    {
        jpeg_dec_close(c->j);
        c->j = NULL;
    }
}

// 只释放我们自己分配的资源，不去 lv_obj_del()
static void album_free_resources_only(void)
{
    album_ctx_t *c = &s_ctx;
    if (c->canvas_buf)
    {
        safe_free_align(c->canvas_buf);
        c->canvas_buf = NULL;
    }
    if (c->decode_buf)
    {
        safe_free_align(c->decode_buf);
        c->decode_buf = NULL;
        c->decode_cap = 0;
    }
    if (c->paths)
    {
        free_list(c->paths, c->count);
        c->paths = NULL;
        c->count = 0;
    }
    if (c->j)
    {
        jpeg_dec_close(c->j);
        c->j = NULL;
    }
    // 注意：c->canvas/c->page 不在这里置空，让 LVGL 自己删；DELETE 后它们自然无效了
}

static void album_page_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE)
        return;

    album_free_resources_only(); // 你已有：只 free 我们自己分配的 buffer/句柄

    // ✨ 同步清空我们手里的对象指针，避免“悬空二次删”
    s_ctx.canvas = NULL;
    s_ctx.page = NULL;
}