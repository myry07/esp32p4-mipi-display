#ifndef _UI_LED_H_
#define _UI_LED_H_

#include "lvgl.h"


void lv_touch_cb(lv_event_t *e);


lv_obj_t* show_jpg_on_canvas(lv_obj_t *parent, const char *jpg_path, int canvas_w, int canvas_h);
bool avi_play_start(const char *avi_path);
bool avi_playlist_start(const char *dir_path, bool loop);

lv_obj_t *photo_album_create(const char *dir, int canvas_w, int canvas_h, bool loop);

lv_obj_t *ui_lock_page_create(void);


#endif