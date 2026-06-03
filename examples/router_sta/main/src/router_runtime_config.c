#include "router_runtime_config.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_ROUTER_STA_NODE_ID
#define CONFIG_ROUTER_STA_NODE_ID 1
#endif
#ifndef CONFIG_ROUTER_STA_W5500_STATIC_IP_ENABLE
#define CONFIG_ROUTER_STA_W5500_STATIC_IP_ENABLE 0
#endif
#ifndef CONFIG_ROUTER_STA_W5500_STATIC_IP_ADDR
#define CONFIG_ROUTER_STA_W5500_STATIC_IP_ADDR "192.168.92.253"
#endif
#ifndef CONFIG_ROUTER_STA_W5500_STATIC_NETMASK
#define CONFIG_ROUTER_STA_W5500_STATIC_NETMASK "255.255.255.0"
#endif
#ifndef CONFIG_ROUTER_STA_W5500_STATIC_GATEWAY
#define CONFIG_ROUTER_STA_W5500_STATIC_GATEWAY "192.168.92.1"
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_ENABLE
#define CONFIG_ROUTER_STA_MSTP_ENABLE 0
#endif
#ifndef CONFIG_ROUTER_STA_NET_BLOCK
#define CONFIG_ROUTER_STA_NET_BLOCK 1
#endif

#define ROUTER_NVS_NS "router"

static router_runtime_config s_config;
static bool s_loaded;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    snprintf(dst, dst_size, "%s", src);
}

void router_runtime_config_init_defaults(router_runtime_config *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->node_id = CONFIG_ROUTER_STA_NODE_ID;
#if CONFIG_ROUTER_STA_HALOW_CHANNEL_1MHZ_LOW || CONFIG_ROUTER_STA_HALOW_CHANNEL_1MHZ_MID || CONFIG_ROUTER_STA_HALOW_CHANNEL_1MHZ_HIGH
    cfg->speed = ROUTER_STA_SPEED_SLOW;
#elif CONFIG_ROUTER_STA_HALOW_CHANNEL_8MHZ_HIGH
    cfg->speed = ROUTER_STA_SPEED_FAST;
#else
    cfg->speed = ROUTER_STA_SPEED_MEDIUM;
#endif
    cfg->w5500_dhcp = !CONFIG_ROUTER_STA_W5500_STATIC_IP_ENABLE;
    copy_str(cfg->w5500_ip, sizeof(cfg->w5500_ip), CONFIG_ROUTER_STA_W5500_STATIC_IP_ADDR);
    copy_str(cfg->w5500_netmask, sizeof(cfg->w5500_netmask), CONFIG_ROUTER_STA_W5500_STATIC_NETMASK);
    copy_str(cfg->w5500_gateway, sizeof(cfg->w5500_gateway), CONFIG_ROUTER_STA_W5500_STATIC_GATEWAY);
    cfg->mstp_enable = CONFIG_ROUTER_STA_MSTP_ENABLE ? true : false;
}

static esp_err_t get_str(nvs_handle_t nvs, const char *key, char *dst, size_t dst_size)
{
    size_t len = dst_size;
    esp_err_t err = nvs_get_str(nvs, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

esp_err_t router_runtime_config_load(void)
{
    router_runtime_config_init_defaults(&s_config);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ROUTER_NVS_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_loaded = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        printf("ROUTER_CFG nvs_open failed err=0x%x; using defaults\n", (unsigned)err);
        s_loaded = true;
        return err;
    }

    uint16_t node_id = s_config.node_id;
    if (nvs_get_u16(nvs, "node_id", &node_id) == ESP_OK && node_id >= 1 && node_id <= 999) {
        s_config.node_id = node_id;
    }

    uint8_t speed = (uint8_t)s_config.speed;
    if (nvs_get_u8(nvs, "speed", &speed) == ESP_OK && speed <= ROUTER_STA_SPEED_FAST) {
        s_config.speed = (router_sta_speed)speed;
    }

    uint8_t dhcp = s_config.w5500_dhcp ? 1U : 0U;
    if (nvs_get_u8(nvs, "w5500_dhcp", &dhcp) == ESP_OK) {
        s_config.w5500_dhcp = dhcp != 0;
    }

    (void)get_str(nvs, "w5500_ip", s_config.w5500_ip, sizeof(s_config.w5500_ip));
    (void)get_str(nvs, "w5500_mask", s_config.w5500_netmask, sizeof(s_config.w5500_netmask));
    (void)get_str(nvs, "w5500_gw", s_config.w5500_gateway, sizeof(s_config.w5500_gateway));

    uint8_t mstp = s_config.mstp_enable ? 1U : 0U;
    if (nvs_get_u8(nvs, "mstp", &mstp) == ESP_OK) {
        s_config.mstp_enable = mstp != 0;
    }

    nvs_close(nvs);
    s_loaded = true;
    return ESP_OK;
}

esp_err_t router_runtime_config_save(const router_runtime_config *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ROUTER_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u16(nvs, "node_id", cfg->node_id);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "speed", (uint8_t)cfg->speed);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "w5500_dhcp", cfg->w5500_dhcp ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "w5500_ip", cfg->w5500_ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "w5500_mask", cfg->w5500_netmask);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "w5500_gw", cfg->w5500_gateway);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "mstp", cfg->mstp_enable ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    if (err == ESP_OK) {
        s_config = *cfg;
        s_loaded = true;
    }
    return err;
}

const router_runtime_config *router_runtime_config_get(void)
{
    if (!s_loaded) {
        router_runtime_config_init_defaults(&s_config);
        s_loaded = true;
    }
    return &s_config;
}

router_runtime_config *router_runtime_config_get_mutable(void)
{
    (void)router_runtime_config_get();
    return &s_config;
}

const char *router_runtime_config_speed_name(router_sta_speed speed)
{
    switch (speed) {
    case ROUTER_STA_SPEED_SLOW:
        return "slow";
    case ROUTER_STA_SPEED_MEDIUM:
        return "medium";
    case ROUTER_STA_SPEED_FAST:
        return "fast";
    default:
        return "unknown";
    }
}

bool router_runtime_config_parse_speed(const char *text, router_sta_speed *speed)
{
    if (!text || !speed) {
        return false;
    }
    if (strcmp(text, "slow") == 0) {
        *speed = ROUTER_STA_SPEED_SLOW;
        return true;
    }
    if (strcmp(text, "medium") == 0) {
        *speed = ROUTER_STA_SPEED_MEDIUM;
        return true;
    }
    if (strcmp(text, "fast") == 0) {
        *speed = ROUTER_STA_SPEED_FAST;
        return true;
    }
    return false;
}

uint16_t router_runtime_config_derive_net(uint16_t transport_slot)
{
    const router_runtime_config *cfg = router_runtime_config_get();
    uint32_t net = (uint32_t)CONFIG_ROUTER_STA_NET_BLOCK * 10000U +
                   (uint32_t)cfg->node_id * 10U +
                   (uint32_t)transport_slot;
    if (net == 0 || net >= 65535U) {
        return 0;
    }
    return (uint16_t)net;
}
