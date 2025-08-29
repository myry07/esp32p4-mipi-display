#ifndef _UI_LED_H_
#define _UI_LED_H_

#include "lvgl.h"


lv_obj_t* show_jpg_on_canvas(lv_obj_t *parent, const char *jpg_path, int canvas_w, int canvas_h);

bool avi_play_start(const char *avi_path);
bool avi_playlist_start(const char *dir_path, bool loop);
void mp3_play_start(void);

lv_obj_t *photo_album_create(const char *dir, int canvas_w, int canvas_h, bool loop);


// void load_page_cb(lv_event_t *e);


lv_obj_t *page_lock_create(void);
lv_obj_t *page_main_create(void);


#endif