#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include <stdio.h>
#include <string.h>

// 兼容 LVGL v8 / v9 的像素格式常量
#ifndef LV_COLOR_FORMAT_RGB565
#define LV_COLOR_FORMAT_RGB565 LV_IMG_CF_TRUE_COLOR
#endif

// 简单的“安全分配”宏：优先用 jpeg_calloc_align，退化用 calloc
static void *safe_calloc_align(size_t n, size_t align)
{
    void *p = NULL;
    // 有些头文件声明了 jpeg_calloc_align
    p = jpeg_calloc_align(n, align);
    if (!p) p = calloc(1, n); // 退化（对齐不保证，但大多数平台可用）
    return p;
}

static void safe_free_align(void *p)
{
    if (!p) return;
    // 若用 jpeg_calloc_align 分配，则用 jpeg_free_align 释放
    jpeg_free_align(p);
}

// 在当前屏上创建一个固定大小 canvas，显示 JPG（过大则居中裁剪；过小则居中贴图）
lv_obj_t* show_jpg_on_canvas(lv_obj_t *parent, const char *jpg_path, int canvas_w, int canvas_h)
{
    if (!parent) return NULL;

    // 1) 读文件进内存
    FILE *fp = fopen(jpg_path, "rb");
    if (!fp) { printf("open %s failed\n", jpg_path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) { fclose(fp); printf("bad file size\n"); return NULL; }

    uint8_t *jpg_bytes = (uint8_t *)safe_calloc_align((size_t)fsize, 16);
    if (!jpg_bytes) { fclose(fp); printf("no mem jpg\n"); return NULL; }
    size_t rd = fread(jpg_bytes, 1, (size_t)fsize, fp);
    fclose(fp);
    if (rd != (size_t)fsize) { safe_free_align(jpg_bytes); printf("read fail\n"); return NULL; }

    // 2) 打开 JPEG 解码器并解析头
    jpeg_dec_handle_t j = NULL;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE; // 输出 RGB565
    if (jpeg_dec_open(&cfg, &j) != JPEG_ERR_OK) {
        safe_free_align(jpg_bytes); printf("jpeg open fail\n"); return NULL;
    }

    jpeg_dec_io_t io = {
        .inbuf = jpg_bytes,
        .inbuf_len = (int)fsize,
        .outbuf = NULL,
    };
    jpeg_dec_header_info_t hi;
    if (jpeg_dec_parse_header(j, &io, &hi) != JPEG_ERR_OK) {
        jpeg_dec_close(j); safe_free_align(jpg_bytes); printf("parse header fail\n"); return NULL;
    }

    // 3) 申请输出缓冲并解码（整幅解到 RGB565）
    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(j, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        jpeg_dec_close(j); safe_free_align(jpg_bytes); printf("get out len fail\n"); return NULL;
    }
    uint8_t *rgb565 = (uint8_t *)safe_calloc_align((size_t)out_len, 16);
    if (!rgb565) {
        jpeg_dec_close(j); safe_free_align(jpg_bytes); printf("no mem out\n"); return NULL;
    }
    io.outbuf = rgb565;
    if (jpeg_dec_process(j, &io) != JPEG_ERR_OK) {
        jpeg_dec_close(j); safe_free_align(jpg_bytes); safe_free_align(rgb565); printf("decode fail\n"); return NULL;
    }

    int img_w = (int)hi.width;
    int img_h = (int)hi.height;

    // 4) 创建 canvas（挂在 parent）+ 分配像素缓冲
    lv_obj_t *canvas = lv_canvas_create(parent);     // ★ 挂到传入的父对象
    if (!canvas) {
        jpeg_dec_close(j); safe_free_align(jpg_bytes); safe_free_align(rgb565);
        printf("lv_canvas_create failed\n"); return NULL;
    }

    lv_color_t *canvas_buf = (lv_color_t *)safe_calloc_align((size_t)canvas_w * canvas_h * sizeof(lv_color_t), 16);
    if (!canvas_buf) {
        lv_obj_del(canvas);
        jpeg_dec_close(j); safe_free_align(jpg_bytes); safe_free_align(rgb565);
        printf("no mem canvas\n"); return NULL;
    }

    // 先把缓冲绑定到 canvas（之后对 canvas_buf 的写就会显示）
    lv_canvas_set_buffer(canvas, canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    // 5) 贴图：小图居中，大图居中裁剪（RGB565：2 字节/像素）
    memset(canvas_buf, 0, (size_t)canvas_w * canvas_h * sizeof(lv_color_t)); // 背景清黑

    int copy_w = img_w;
    int copy_h = img_h;
    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = 0, dst_y0 = 0;

    if (copy_w > canvas_w) { src_x0 = (copy_w - canvas_w) / 2; copy_w = canvas_w; }
    else { dst_x0 = (canvas_w - copy_w) / 2; }

    if (copy_h > canvas_h) { src_y0 = (copy_h - canvas_h) / 2; copy_h = canvas_h; }
    else { dst_y0 = (canvas_h - copy_h) / 2; }

    for (int y = 0; y < copy_h; y++) {
        const uint8_t *src = rgb565 + ((size_t)(src_y0 + y) * img_w + src_x0) * 2;
        lv_color_t *dst = canvas_buf + ((size_t)(dst_y0 + y) * canvas_w + dst_x0);
        memcpy(dst, src, (size_t)copy_w * 2);
    }
    lv_obj_invalidate(canvas);

    // 6) 清理临时资源（canvas_buf 归 canvas 持有，别释放）
    jpeg_dec_close(j);
    safe_free_align(jpg_bytes);
    safe_free_align(rgb565);

    return canvas;
}