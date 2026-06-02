# Copyright 2026
# SPDX-License-Identifier: Apache-2.0

set(src
    "src/mm_app_loadconfig.c"
    "src/mm_app_common.c"
    "src/mm_app_regdb.c"
    "wbacnet/tbx.c"
    "src/router_sta.c")

set(inc
    "src"
    "wbacnet")

idf_component_register(SRCS ${src}
                       INCLUDE_DIRS ${inc}
                       PRIV_REQUIRES driver morselib mm_shims mmipal lwip)
