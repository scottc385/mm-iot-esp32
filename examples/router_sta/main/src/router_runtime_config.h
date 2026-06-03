#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum router_sta_speed {
    ROUTER_STA_SPEED_SLOW = 0,
    ROUTER_STA_SPEED_MEDIUM = 1,
    ROUTER_STA_SPEED_FAST = 2,
} router_sta_speed;

typedef struct router_runtime_config {
    uint16_t node_id;
    router_sta_speed speed;
    bool w5500_dhcp;
    char w5500_ip[16];
    char w5500_netmask[16];
    char w5500_gateway[16];
    bool mstp_enable;
} router_runtime_config;

void router_runtime_config_init_defaults(router_runtime_config *cfg);
esp_err_t router_runtime_config_load(void);
esp_err_t router_runtime_config_save(const router_runtime_config *cfg);
const router_runtime_config *router_runtime_config_get(void);
router_runtime_config *router_runtime_config_get_mutable(void);
const char *router_runtime_config_speed_name(router_sta_speed speed);
bool router_runtime_config_parse_speed(const char *text, router_sta_speed *speed);
uint16_t router_runtime_config_derive_net(uint16_t transport_slot);

