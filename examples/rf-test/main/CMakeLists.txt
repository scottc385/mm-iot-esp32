set(src
    "src/mm_app_regdb.c"
    "src/rf-test.c"
    "src/mmhal_uart.c")

set(inc
    "src")

idf_component_register(INCLUDE_DIRS ${inc}
                       SRCS ${src}
                       PRIV_REQUIRES driver esp_driver_gpio esp_driver_uart morselib mm_shims mmutils)
