set(srcs "main.c" "app_video.c" "camera_display.c")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .)
