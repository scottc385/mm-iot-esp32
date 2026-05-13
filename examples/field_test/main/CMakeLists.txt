# Copyright 2023 Morse Micro
# SPDX-License-Identifier: Apache-2.0
set(src
    "src/mm_app_loadconfig.c"
    "src/mm_app_common.c"
    "src/mm_app_regdb.c"
    "src/led_strip_encoder.c"
    "src/field_test.c")

set(inc
    "src")

idf_component_register(SRCS ${src}
                       INCLUDE_DIRS ${inc}
                       PRIV_REQUIRES driver esp_driver_gpio esp_driver_ledc esp_driver_rmt morselib mm_shims mmipal mmiperf lwip)
