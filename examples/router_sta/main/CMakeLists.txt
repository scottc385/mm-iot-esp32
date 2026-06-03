# Copyright 2026
# SPDX-License-Identifier: Apache-2.0

set(src
    "src/mm_app_loadconfig.c"
    "src/mm_app_common.c"
    "src/mm_app_regdb.c"
    "wbacnet/tbx.c"
    "wbacnet/router/router_core.c"
    "wbacnet/router/router_policy.c"
    "wbacnet/router/router_service.c"
    "wbacnet/router/router_mgmt_command.c"
    "wbacnet/bacnet/npdu.c"
    "wbacnet/bacnet/bacaddr.c"
    "wbacnet/bacnet/bacapp.c"
    "wbacnet/bacnet/bacdcode.c"
    "wbacnet/bacnet/bacint.c"
    "wbacnet/bacnet/bactext.c"
    "wbacnet/bacnet/bacstr.c"
    "wbacnet/bacnet/bactimevalue.c"
    "wbacnet/bacnet/bacreal.c"
    "wbacnet/bacnet/indtext.c"
    "wbacnet/bacnet/hostnport.c"
    "wbacnet/bacnet/iam.c"
    "wbacnet/bacnet/whois.c"
    "wbacnet/bacnet/rp.c"
    "wbacnet/bacnet/wp.c"
    "wbacnet/bacnet/reject.c"
    "wbacnet/bacnet/abort.c"
    "wbacnet/bacnet/bacerror.c"
    "ports/udp_tbx_port.c"
    "src/router_sta.c")

if(CONFIG_ROUTER_STA_LOCAL_APP_ENABLE)
    list(APPEND src "ports/local_app_port.c")
endif()

set(priv_requires
    driver
    morselib
    mm_shims
    mmipal
    lwip
    esp_eth
    esp_netif
    esp_event
    esp_driver_gpio
    esp_driver_spi
    esp_driver_uart
    esp_timer
    w5500)

if(CONFIG_ROUTER_STA_W5500_PROBE_ENABLE)
    list(APPEND src "ports/w5500_probe.c")
endif()

if(CONFIG_ROUTER_STA_W5500_BIP_ENABLE)
    list(APPEND src "ports/bip_port.c")
endif()

if(CONFIG_ROUTER_STA_RS485_PROBE_ENABLE)
    list(APPEND src "ports/rs485_probe.c")
endif()

set(inc
    "src"
    "ports"
    "wbacnet"
    "wbacnet/router")

idf_component_register(SRCS ${src}
                       INCLUDE_DIRS ${inc}
                       PRIV_REQUIRES ${priv_requires})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BACDL_BIP=1
    BBMD_ENABLED=0
    BACNET_STACK_DEPRECATED_DISABLE=1
    ROUTER_CORE_ENABLE_LEGACY=0)
