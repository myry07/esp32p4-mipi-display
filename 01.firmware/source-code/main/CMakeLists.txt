idf_component_register(SRCS "main.c"

                            "lvgl_port/main_page.c" "lvgl_port/lock_page.c"
                            "lvgl_port/show_jpg.c" "lvgl_port/video_player.c" 
                            "lvgl_port/photo_album.c" "lvgl_port/audio_player.c"
                            "lvgl_port/page_manager.c"


                    INCLUDE_DIRS "."  "lvgl_port/include"
                    )