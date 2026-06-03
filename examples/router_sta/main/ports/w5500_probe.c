#include "w5500_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "lwip/ip4_addr.h"

#define W5500_PROBE_GOT_IP_BIT BIT0
#define W5500_PROBE_SPI_QUEUE_SIZE 12

static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static esp_eth_netif_glue_handle_t s_glue;
static EventGroupHandle_t s_event_group;
static bool s_started;
static bool s_event_handlers_registered;

static bool w5500_probe_parse_ipv4(const char *text, esp_ip4_addr_t *addr)
{
    unsigned int b0;
    unsigned int b1;
    unsigned int b2;
    unsigned int b3;

    if (!text || !addr) {
        return false;
    }
    if (sscanf(text, "%u.%u.%u.%u", &b0, &b1, &b2, &b3) != 4) {
        return false;
    }
    if (b0 > 255 || b1 > 255 || b2 > 255 || b3 > 255) {
        return false;
    }
    IP4_ADDR(addr, b0, b1, b2, b3);
    return true;
}

static esp_err_t w5500_probe_configure_ipv4(esp_netif_t *netif)
{
#if CONFIG_ROUTER_STA_W5500_STATIC_IP_ENABLE
    esp_netif_ip_info_t ip_info = {0};

    if (!w5500_probe_parse_ipv4(CONFIG_ROUTER_STA_W5500_STATIC_IP_ADDR, &ip_info.ip) ||
        !w5500_probe_parse_ipv4(CONFIG_ROUTER_STA_W5500_STATIC_NETMASK, &ip_info.netmask) ||
        !w5500_probe_parse_ipv4(CONFIG_ROUTER_STA_W5500_STATIC_GATEWAY, &ip_info.gw)) {
        printf("W5500_STATIC_IP invalid ip=%s netmask=%s gw=%s\n",
               CONFIG_ROUTER_STA_W5500_STATIC_IP_ADDR,
               CONFIG_ROUTER_STA_W5500_STATIC_NETMASK,
               CONFIG_ROUTER_STA_W5500_STATIC_GATEWAY);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        printf("W5500_STATIC_IP dhcp stop failed err=0x%x\n", (unsigned)err);
        return err;
    }
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        printf("W5500_STATIC_IP set failed err=0x%x\n", (unsigned)err);
        return err;
    }
    printf("W5500_STATIC_IP ip=" IPSTR " netmask=" IPSTR " gw=" IPSTR "\n",
           IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
    return ESP_OK;
#else
    esp_err_t err = esp_netif_dhcpc_start(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        printf("W5500_PROBE dhcp start failed err=0x%x\n", (unsigned)err);
        return err;
    }
    return ESP_OK;
#endif
}

static void w5500_probe_eth_event_handler(void *arg, esp_event_base_t event_base,
                                           int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        printf("W5500_EVENT start\n");
        break;
    case ETHERNET_EVENT_STOP:
        printf("W5500_EVENT stop\n");
        break;
    case ETHERNET_EVENT_CONNECTED:
        printf("W5500_EVENT connected\n");
        if (s_netif) {
            (void)esp_netif_create_ip6_linklocal(s_netif);
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        printf("W5500_EVENT disconnected\n");
        break;
    default:
        printf("W5500_EVENT id=%ld\n", (long)event_id);
        break;
    }
}

static void w5500_probe_got_ip_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id != IP_EVENT_ETH_GOT_IP || !event_data) {
        return;
    }

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    if (event->esp_netif != s_netif) {
        return;
    }

    printf("W5500_GOT_IP ip=" IPSTR " netmask=" IPSTR " gw=" IPSTR "\n",
           IP2STR(&event->ip_info.ip),
           IP2STR(&event->ip_info.netmask),
           IP2STR(&event->ip_info.gw));
    if (s_event_group) {
        xEventGroupSetBits(s_event_group, W5500_PROBE_GOT_IP_BIT);
    }
}

static esp_err_t w5500_probe_configure_spi_bus(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_ROUTER_STA_W5500_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_ROUTER_STA_W5500_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_ROUTER_STA_W5500_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };

    esp_err_t err = spi_bus_initialize((spi_host_device_t)CONFIG_ROUTER_STA_W5500_SPI_HOST,
                                       &buscfg, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("W5500_PROBE spi_bus already initialized host=%d\n", CONFIG_ROUTER_STA_W5500_SPI_HOST);
        return ESP_OK;
    }
    return err;
}

static void w5500_probe_cleanup(esp_eth_mac_t *mac, esp_eth_phy_t *phy,
                                bool driver_installed, bool mac_owned, bool phy_owned)
{
    if (s_started && s_eth) {
        (void)esp_eth_stop(s_eth);
        s_started = false;
    }
    if (s_event_handlers_registered) {
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &w5500_probe_got_ip_handler);
        (void)esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &w5500_probe_eth_event_handler);
        s_event_handlers_registered = false;
    }
    if (s_glue) {
        (void)esp_eth_del_netif_glue(s_glue);
        s_glue = NULL;
    }
    if (driver_installed && s_eth) {
        (void)esp_eth_driver_uninstall(s_eth);
        s_eth = NULL;
    }
    if (phy_owned && phy) {
        phy->del(phy);
    }
    if (mac_owned && mac) {
        mac->del(mac);
    }
    if (s_netif) {
        (void)esp_netif_dhcpc_stop(s_netif);
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
}

bool w5500_probe_start(void)
{
    if (s_started) {
        return true;
    }

    printf("W5500_PROBE start host=%d sclk=%d mosi=%d miso=%d cs=%d int=%d rst=%d clock=%dMHz\n",
           CONFIG_ROUTER_STA_W5500_SPI_HOST,
           CONFIG_ROUTER_STA_W5500_SPI_SCLK_GPIO,
           CONFIG_ROUTER_STA_W5500_SPI_MOSI_GPIO,
           CONFIG_ROUTER_STA_W5500_SPI_MISO_GPIO,
           CONFIG_ROUTER_STA_W5500_SPI_CS_GPIO,
           CONFIG_ROUTER_STA_W5500_INT_GPIO,
           CONFIG_ROUTER_STA_W5500_RST_GPIO,
           CONFIG_ROUTER_STA_W5500_SPI_CLOCK_MHZ);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("W5500_PROBE esp_netif_init failed err=0x%x\n", (unsigned)err);
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("W5500_PROBE event_loop_create failed err=0x%x\n", (unsigned)err);
        return false;
    }

    if (!s_event_group) {
        s_event_group = xEventGroupCreate();
        if (!s_event_group) {
            printf("W5500_PROBE event_group allocation failed\n");
            return false;
        }
    }

    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    bool driver_installed = false;
    bool mac_owned = false;
    bool phy_owned = false;

    err = w5500_probe_configure_spi_bus();
    if (err != ESP_OK) {
        printf("W5500_PROBE spi_bus_initialize failed err=0x%x\n", (unsigned)err);
        goto fail;
    }

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_desc = "w5500_probe";
    base_cfg.route_prio = 10;
    esp_netif_config_t netif_cfg = {
        .base = &base_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&netif_cfg);
    if (!s_netif) {
        printf("W5500_PROBE esp_netif_new failed\n");
        goto fail;
    }

    err = w5500_probe_configure_ipv4(s_netif);
    if (err != ESP_OK) {
        goto fail;
    }

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_ROUTER_STA_W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_ROUTER_STA_W5500_SPI_CS_GPIO,
        .queue_size = W5500_PROBE_SPI_QUEUE_SIZE,
        .flags = 0,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(
        (spi_host_device_t)CONFIG_ROUTER_STA_W5500_SPI_HOST,
        &devcfg);
    w5500_config.int_gpio_num = CONFIG_ROUTER_STA_W5500_INT_GPIO;
#if CONFIG_ROUTER_STA_W5500_INT_GPIO < 0
    w5500_config.poll_period_ms = 10;
#endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = CONFIG_ROUTER_STA_W5500_MAC_TASK_STACK_SIZE;
    mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        printf("W5500_PROBE esp_eth_mac_new_w5500 failed\n");
        goto fail;
    }
    mac_owned = true;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_ROUTER_STA_W5500_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ROUTER_STA_W5500_RST_GPIO;
    phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        printf("W5500_PROBE esp_eth_phy_new_w5500 failed\n");
        goto fail;
    }
    phy_owned = true;

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK) {
        printf("W5500_PROBE driver_install failed err=0x%x\n", (unsigned)err);
        goto fail;
    }
    driver_installed = true;
    mac_owned = false;
    phy_owned = false;

    uint8_t base_mac[ETH_ADDR_LEN];
    uint8_t local_mac[ETH_ADDR_LEN];
    err = esp_efuse_mac_get_default(base_mac);
    if (err != ESP_OK) {
        printf("W5500_PROBE efuse mac failed err=0x%x\n", (unsigned)err);
        goto fail;
    }
    err = esp_derive_local_mac(local_mac, base_mac);
    if (err != ESP_OK) {
        printf("W5500_PROBE derive mac failed err=0x%x\n", (unsigned)err);
        goto fail;
    }
    err = esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, local_mac);
    if (err != ESP_OK) {
        printf("W5500_PROBE set mac failed err=0x%x\n", (unsigned)err);
        goto fail;
    }
    printf("W5500_MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           local_mac[0], local_mac[1], local_mac[2], local_mac[3], local_mac[4], local_mac[5]);

    s_glue = esp_eth_new_netif_glue(s_eth);
    if (!s_glue) {
        printf("W5500_PROBE netif_glue allocation failed\n");
        goto fail;
    }
    err = esp_netif_attach(s_netif, s_glue);
    if (err != ESP_OK) {
        printf("W5500_PROBE netif attach failed err=0x%x\n", (unsigned)err);
        goto fail;
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                     &w5500_probe_eth_event_handler, NULL);
    if (err != ESP_OK) {
        printf("W5500_PROBE eth handler register failed err=0x%x\n", (unsigned)err);
        goto fail;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                     &w5500_probe_got_ip_handler, NULL);
    if (err != ESP_OK) {
        (void)esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &w5500_probe_eth_event_handler);
        printf("W5500_PROBE ip handler register failed err=0x%x\n", (unsigned)err);
        goto fail;
    }
    s_event_handlers_registered = true;

    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        printf("W5500_PROBE eth_start failed err=0x%x\n", (unsigned)err);
        goto fail;
    }
    s_started = true;

#if CONFIG_ROUTER_STA_W5500_STATIC_IP_ENABLE
    printf("W5500_PROBE ready\n");
#else
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           W5500_PROBE_GOT_IP_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_ROUTER_STA_W5500_WAIT_IP_TIMEOUT_MS));
    if ((bits & W5500_PROBE_GOT_IP_BIT) == 0) {
        printf("W5500_PROBE started; no DHCP address within %d ms\n",
               CONFIG_ROUTER_STA_W5500_WAIT_IP_TIMEOUT_MS);
    } else {
        printf("W5500_PROBE ready\n");
    }
#endif
    return true;

fail:
    w5500_probe_cleanup(mac, phy, driver_installed, mac_owned, phy_owned);
    printf("W5500_PROBE failed; HaLow router continues\n");
    return false;
}
