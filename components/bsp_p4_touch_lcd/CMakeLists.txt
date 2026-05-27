
idf_component_register(
    SRCS "bsp_p4_touch_lcd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver
    PRIV_REQUIRES esp_lcd
)
