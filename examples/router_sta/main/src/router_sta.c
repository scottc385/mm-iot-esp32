/*
 * ESP32 HaLow router station scaffold.
 *
 * Current milestone: Heltec HT-HC01P joins the Pi HaLow AP, opens a UDP/TBX
 * transport, configures the copied portable router service, and periodically
 * sends a real BACnet network-layer Who-Is-Router-To-Network NPDU over TBX.
 * Inbound TBX frames are unwrapped and submitted to the router service. A
 * routed application Who-Is probe can be enabled at build time for diagnostics,
 * but is disabled in the normal Heltec profile.
 */

#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "mmhal_wlan.h"
#include "mm_app_common.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "bacnet/bacenum.h"
#include "bacnet/npdu.h"
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
#include "bip_port.h"
#endif
#if CONFIG_ROUTER_STA_LOCAL_APP_ENABLE
#include "local_app_port.h"
#endif
#if CONFIG_ROUTER_STA_MSTP_ENABLE
#include "mstp_port.h"
#endif
#if CONFIG_ROUTER_STA_RS485_PROBE_ENABLE
#include "rs485_probe.h"
#endif
#include "router_service.h"
#include "router_log_control.h"
#include "router_sta.h"
#include "router_transport.h"
#include "router_cli.h"
#include "router_runtime_config.h"
#include "udp_tbx_port.h"
#if CONFIG_ROUTER_STA_W5500_PROBE_ENABLE
#include "w5500_probe.h"
#endif

#ifndef CONFIG_ROUTER_STA_UDP_LOCAL_PORT
#define CONFIG_ROUTER_STA_UDP_LOCAL_PORT 5000
#endif

#ifndef CONFIG_ROUTER_STA_UDP_PEER_IP
#define CONFIG_ROUTER_STA_UDP_PEER_IP "192.168.50.1"
#endif

#ifndef CONFIG_ROUTER_STA_UDP_PEER_PORT
#define CONFIG_ROUTER_STA_UDP_PEER_PORT 5000
#endif

#ifndef CONFIG_ROUTER_STA_STATUS_INTERVAL_MS
#define CONFIG_ROUTER_STA_STATUS_INTERVAL_MS 5000
#endif

#ifndef CONFIG_ROUTER_STA_LOOP_DELAY_MS
#define CONFIG_ROUTER_STA_LOOP_DELAY_MS 50
#endif

#ifndef CONFIG_ROUTER_STA_TBX_ORIGIN_ID
#define CONFIG_ROUTER_STA_TBX_ORIGIN_ID "ESP1"
#endif

#ifndef CONFIG_ROUTER_STA_UDP_TBX_NET
#define CONFIG_ROUTER_STA_UDP_TBX_NET 65000
#endif

#ifndef CONFIG_ROUTER_STA_UDP_TBX_PORT_ID
#define CONFIG_ROUTER_STA_UDP_TBX_PORT_ID 1
#endif

#ifndef CONFIG_ROUTER_STA_NET_BLOCK
#define CONFIG_ROUTER_STA_NET_BLOCK 1
#endif

#ifndef CONFIG_ROUTER_STA_NODE_ID
#define CONFIG_ROUTER_STA_NODE_ID 1
#endif

#ifndef CONFIG_ROUTER_STA_LOCAL_APP_ENABLE
#define CONFIG_ROUTER_STA_LOCAL_APP_ENABLE 0
#endif

#ifndef CONFIG_ROUTER_STA_LOCAL_APP_PORT_ID
#define CONFIG_ROUTER_STA_LOCAL_APP_PORT_ID 2
#endif

#ifndef CONFIG_ROUTER_STA_LOCAL_APP_NET
#define CONFIG_ROUTER_STA_LOCAL_APP_NET 5001
#endif

#ifndef CONFIG_ROUTER_STA_LOCAL_APP_DEVICE_ID
#define CONFIG_ROUTER_STA_LOCAL_APP_DEVICE_ID 500100
#endif

#ifndef CONFIG_ROUTER_STA_LOCAL_APP_OBJECT_NAME
#define CONFIG_ROUTER_STA_LOCAL_APP_OBJECT_NAME "esp32-local-app"
#endif

#ifndef CONFIG_ROUTER_STA_DNET_TTL_MS
#define CONFIG_ROUTER_STA_DNET_TTL_MS 60000
#endif

#ifndef CONFIG_ROUTER_STA_LOG_LEVEL
#define CONFIG_ROUTER_STA_LOG_LEVEL 1
#endif

#ifndef CONFIG_ROUTER_STA_DEBUG_ROUTE_PROBE_DNET
#define CONFIG_ROUTER_STA_DEBUG_ROUTE_PROBE_DNET 0
#endif

#ifndef CONFIG_ROUTER_STA_DEBUG_APP_PROBE_DNET
#define CONFIG_ROUTER_STA_DEBUG_APP_PROBE_DNET CONFIG_ROUTER_STA_DEBUG_ROUTE_PROBE_DNET
#endif

#ifndef CONFIG_ROUTER_STA_W5500_PROBE_ENABLE
#define CONFIG_ROUTER_STA_W5500_PROBE_ENABLE 0
#endif

#ifndef CONFIG_ROUTER_STA_W5500_BIP_ENABLE
#define CONFIG_ROUTER_STA_W5500_BIP_ENABLE 0
#endif

#ifndef CONFIG_ROUTER_STA_W5500_BIP_PORT_ID
#define CONFIG_ROUTER_STA_W5500_BIP_PORT_ID 3
#endif

#ifndef CONFIG_ROUTER_STA_W5500_BIP_NET
#define CONFIG_ROUTER_STA_W5500_BIP_NET 10010
#endif

#ifndef CONFIG_ROUTER_STA_W5500_BIP_NET_AUTO
#define CONFIG_ROUTER_STA_W5500_BIP_NET_AUTO 1
#endif

#ifndef CONFIG_ROUTER_STA_W5500_BIP_UDP_PORT
#define CONFIG_ROUTER_STA_W5500_BIP_UDP_PORT 47808
#endif

#ifndef CONFIG_ROUTER_STA_RS485_PROBE_ENABLE
#define CONFIG_ROUTER_STA_RS485_PROBE_ENABLE 0
#endif

#ifndef CONFIG_ROUTER_STA_MSTP_ENABLE
#define CONFIG_ROUTER_STA_MSTP_ENABLE 0
#endif

#ifndef CONFIG_ROUTER_STA_MSTP_PORT_ID
#define CONFIG_ROUTER_STA_MSTP_PORT_ID 4
#endif

#ifndef CONFIG_ROUTER_STA_MSTP_NET
#define CONFIG_ROUTER_STA_MSTP_NET 10011
#endif

#ifndef CONFIG_ROUTER_STA_MSTP_NET_AUTO
#define CONFIG_ROUTER_STA_MSTP_NET_AUTO 1
#endif

static tb_router_service router_service;
static udp_tbx_port udp_tbx;
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
static bip_port w5500_bip;
#endif
#if CONFIG_ROUTER_STA_MSTP_ENABLE
static mstp_port mstp;
#endif
#if CONFIG_ROUTER_STA_LOCAL_APP_ENABLE
static local_app_port local_app;
static uint32_t router_advertise_seq;
#endif
static uint32_t router_probe_seq;
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
static uint32_t w5500_wir_seq;
#endif
#if CONFIG_ROUTER_STA_DEBUG_APP_PROBE_DNET > 0
static uint32_t debug_app_probe_seq;
#endif
static uint32_t last_status_ms;
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE && CONFIG_ROUTER_STA_W5500_WIR_INTERVAL_MS > 0
static uint32_t last_w5500_wir_ms;
static bool w5500_ip_seen;
#endif
static uint32_t route_snapshot_seq;
static volatile bool reboot_requested;
static bool wlan_initialized;

enum {
    ROUTER_STA_TRANSPORT_SLOT_BIP0 = 0,
    ROUTER_STA_TRANSPORT_SLOT_MSTP0 = 1,
};

static uint16_t router_sta_derive_local_net(uint16_t transport_slot)
{
    return router_runtime_config_derive_net(transport_slot);
}

static uint16_t router_sta_w5500_bip_net(void)
{
#if CONFIG_ROUTER_STA_W5500_BIP_NET_AUTO
    uint16_t net = router_sta_derive_local_net(ROUTER_STA_TRANSPORT_SLOT_BIP0);
    return net != 0 ? net : (uint16_t)CONFIG_ROUTER_STA_W5500_BIP_NET;
#else
    return (uint16_t)CONFIG_ROUTER_STA_W5500_BIP_NET;
#endif
}

static uint16_t router_sta_mstp_net(void)
{
#if CONFIG_ROUTER_STA_MSTP_NET_AUTO
    uint16_t net = router_sta_derive_local_net(ROUTER_STA_TRANSPORT_SLOT_MSTP0);
    return net != 0 ? net : (uint16_t)CONFIG_ROUTER_STA_MSTP_NET;
#else
    return (uint16_t)CONFIG_ROUTER_STA_MSTP_NET;
#endif
}

static void router_sta_print_memory(const char *tag)
{
    printf("MEM[%s] free=%lu min_free=%lu largest=%lu stack_hwm=%lu\n",
           tag,
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned long)uxTaskGetStackHighWaterMark(NULL));
}

static int32_t get_link_rssi(void)
{
    return mmwlan_get_rssi();
}

static const char *router_sta_tbx_origin_id(void)
{
    static char origin[12];
    snprintf(origin, sizeof(origin), "ESP%u",
             (unsigned)router_runtime_config_get()->node_id);
    return origin;
}

static void router_sta_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        printf("NVS init requires erase err=0x%x\n", (unsigned)err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        printf("NVS init failed err=0x%x; using defaults\n", (unsigned)err);
        return;
    }
    (void)router_runtime_config_load();
}

static bool router_sta_configure_service(void)
{
    tb_router_port_config ports[4];
    size_t port_count = 0;

    ports[port_count++] = (tb_router_port_config){
        .port_id = CONFIG_ROUTER_STA_UDP_TBX_PORT_ID,
        .net = CONFIG_ROUTER_STA_UDP_TBX_NET,
        .kind = TB_ROUTER_TRANSPORT_TBX_UDP,
        .caps = TB_ROUTER_PORT_CAP_UNICAST |
                TB_ROUTER_PORT_CAP_BROADCAST |
                TB_ROUTER_PORT_CAP_STATIC_PEERS |
                TB_ROUTER_PORT_CAP_ROUTE_META,
        .transport_state = &udp_tbx,
        .ops = &k_udp_tbx_router_ops,
        .static_dnets = NULL,
        .static_dnet_count = 0,
    };

#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
    if (w5500_bip.sock >= 0) {
        ports[port_count++] = (tb_router_port_config){
            .port_id = CONFIG_ROUTER_STA_W5500_BIP_PORT_ID,
            .net = router_sta_w5500_bip_net(),
            .kind = TB_ROUTER_TRANSPORT_BIP,
            .caps = TB_ROUTER_PORT_CAP_UNICAST |
                    TB_ROUTER_PORT_CAP_BROADCAST |
                    TB_ROUTER_PORT_CAP_LEARNED_PEERS,
            .transport_state = &w5500_bip,
            .ops = &k_bip_router_ops,
            .static_dnets = NULL,
            .static_dnet_count = 0,
        };
    }
#endif

#if CONFIG_ROUTER_STA_LOCAL_APP_ENABLE
    local_app_port_init(&local_app,
                        &router_service,
                        CONFIG_ROUTER_STA_LOCAL_APP_PORT_ID,
                        CONFIG_ROUTER_STA_LOCAL_APP_NET,
                        CONFIG_ROUTER_STA_LOCAL_APP_DEVICE_ID,
                        CONFIG_ROUTER_STA_LOCAL_APP_OBJECT_NAME);
    if (local_app.active) {
        ports[port_count++] = (tb_router_port_config){
            .port_id = CONFIG_ROUTER_STA_LOCAL_APP_PORT_ID,
            .net = CONFIG_ROUTER_STA_LOCAL_APP_NET,
            .kind = TB_ROUTER_TRANSPORT_LOCAL_APP,
            .caps = TB_ROUTER_PORT_CAP_UNICAST | TB_ROUTER_PORT_CAP_BROADCAST,
            .transport_state = &local_app,
            .ops = &k_local_app_router_ops,
            .static_dnets = NULL,
            .static_dnet_count = 0,
        };
    }
#endif

#if CONFIG_ROUTER_STA_MSTP_ENABLE
    if (mstp.active) {
        ports[port_count++] = (tb_router_port_config){
            .port_id = CONFIG_ROUTER_STA_MSTP_PORT_ID,
            .net = mstp.net,
            .kind = TB_ROUTER_TRANSPORT_MSTP,
            .caps = TB_ROUTER_PORT_CAP_UNICAST | TB_ROUTER_PORT_CAP_BROADCAST,
            .transport_state = &mstp,
            .ops = &k_mstp_router_ops,
            .static_dnets = NULL,
            .static_dnet_count = 0,
        };
    }
#endif

    bool ok = tb_router_service_configure(&router_service,
                                          NULL,
                                          ports,
                                          port_count,
                                          CONFIG_ROUTER_STA_DNET_TTL_MS,
                                          CONFIG_ROUTER_STA_LOG_LEVEL);
    printf("ROUTER_CONFIG %s ports=%u tbx_net=%u w5500_bip=%u w5500_net=%u mstp=%u mstp_net=%u local_app=%u local_net=%u\n",
           ok ? "ok" : "failed",
           (unsigned)port_count,
           (unsigned)CONFIG_ROUTER_STA_UDP_TBX_NET,
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
           w5500_bip.sock >= 0 ? 1U : 0U,
           w5500_bip.sock >= 0 ? (unsigned)w5500_bip.net : 0U,
#else
           0U,
           0U,
#endif
#if CONFIG_ROUTER_STA_MSTP_ENABLE
           mstp.active ? 1U : 0U,
           mstp.active ? (unsigned)mstp.net : 0U,
#else
           0U,
           0U,
#endif
#if CONFIG_ROUTER_STA_LOCAL_APP_ENABLE
           local_app.active ? 1U : 0U,
           local_app.active ? (unsigned)local_app.net : 0U);
#else
           0U,
           0U);
#endif
    return ok;
}

static void router_sta_send_whois_router(void)
{
    uint8_t npdu[64];
    size_t len = tb_router_npdu_build_whois_router(npdu, sizeof(npdu));
    if (len == 0) {
        printf("TX_ROUTER_WIR build failed\n");
        return;
    }

    router_probe_seq++;
    if (router_log_get(ROUTER_LOG_STATUS)) {
        printf("TX_ROUTER_WIR seq=%lu npdu_len=%u\n",
               (unsigned long)router_probe_seq, (unsigned)len);
    }
    udp_tbx_port_send_npdu_direct(&udp_tbx, npdu, len);
}

#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
static void router_sta_send_w5500_whois_router(void)
{
#if CONFIG_ROUTER_STA_W5500_WIR_INTERVAL_MS > 0
    if (w5500_bip.sock < 0) {
        return;
    }

    uint8_t npdu[64];
    size_t len = tb_router_npdu_build_whois_router(npdu, sizeof(npdu));
    if (len == 0) {
        printf("TX_W5500_ROUTER_WIR build failed\n");
        return;
    }

    w5500_wir_seq++;
    if (router_log_get(ROUTER_LOG_STATUS)) {
        printf("TX_W5500_ROUTER_WIR seq=%lu npdu_len=%u\n",
               (unsigned long)w5500_wir_seq, (unsigned)len);
    }
    bip_port_send_npdu_broadcast(&w5500_bip, npdu, len);
#endif
}
#endif

static void router_sta_send_iam_router(void)
{
#if CONFIG_ROUTER_STA_LOCAL_APP_ENABLE
    if (!local_app.active) {
        return;
    }

    uint16_t nets[] = { local_app.net };
    uint8_t npdu[64];
    size_t len = tb_router_npdu_build_iar(npdu, sizeof(npdu), nets, sizeof(nets) / sizeof(nets[0]));
    if (len == 0) {
        printf("TX_ROUTER_IAR build failed\n");
        return;
    }

    router_advertise_seq++;
    if (router_log_get(ROUTER_LOG_STATUS)) {
        printf("TX_ROUTER_IAR seq=%lu nets=%u npdu_len=%u\n",
               (unsigned long)router_advertise_seq,
               (unsigned)local_app.net,
               (unsigned)len);
    }
    udp_tbx_port_send_npdu_direct(&udp_tbx, npdu, len);
#endif
}

#if CONFIG_ROUTER_STA_DEBUG_APP_PROBE_DNET > 0
static size_t router_sta_build_debug_whois_npdu(uint8_t *out, size_t out_cap, uint16_t dnet)
{
    if (!out || out_cap < 16 || dnet == 0 || dnet == 0xffff) {
        return 0;
    }

    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu = {0};

    daddr.net = dnet;
    daddr.len = 0; /* Remote broadcast on the destination network. */
    saddr.net = CONFIG_ROUTER_STA_UDP_TBX_NET;
    saddr.len = 0;
    npdu_encode_npdu_data(&npdu, false, MESSAGE_PRIORITY_NORMAL);
    npdu.hop_count = 0xff;

    int header_len = npdu_encode_pdu(out, &daddr, &saddr, &npdu);
    if (header_len <= 0 || (size_t)header_len + 2 > out_cap) {
        return 0;
    }

    out[header_len++] = PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST;
    out[header_len++] = SERVICE_UNCONFIRMED_WHO_IS;
    return (size_t)header_len;
}
#endif

static void router_sta_send_debug_app_probe(void)
{
#if CONFIG_ROUTER_STA_DEBUG_APP_PROBE_DNET > 0
    const uint16_t dnet = CONFIG_ROUTER_STA_DEBUG_APP_PROBE_DNET;
    if (!udp_tbx_port_has_route(&udp_tbx, dnet)) {
        return;
    }

    uint8_t npdu[64];
    size_t len = router_sta_build_debug_whois_npdu(npdu, sizeof(npdu), dnet);
    if (len == 0) {
        printf("TX_DEBUG_APP_PROBE build failed dnet=%u\n", (unsigned)dnet);
        return;
    }

    BACNET_ADDRESS daddr = {0};
    daddr.net = dnet;
    daddr.len = 0;

    debug_app_probe_seq++;
    printf("TX_DEBUG_APP_PROBE seq=%lu dnet=%u kind=who-is npdu_len=%u\n",
           (unsigned long)debug_app_probe_seq, (unsigned)dnet, (unsigned)len);
    udp_tbx_port_send_npdu(&udp_tbx, npdu, len, &daddr);
#endif
}

static void router_sta_print_router_log(void)
{
    size_t log_len = 0;
    const char *log = tb_router_service_get_log(&router_service, &log_len);
    if (log && log_len > 0) {
        printf("ROUTER_LOG %.*s\n", (int)log_len, log);
        tb_router_service_clear_log(&router_service);
    }
}

static void router_sta_print_event(void *arg, const tb_router_event *event)
{
    (void)arg;
    if (!event) {
        return;
    }

    const char *type = "unknown";
    switch (event->type) {
    case TB_ROUTER_EVENT_DNET_CHANGE:
        type = "dnet_change";
        break;
    case TB_ROUTER_EVENT_IAR_RX:
        type = "iar_rx";
        break;
    default:
        break;
    }

    if (router_log_get(ROUTER_LOG_STATUS)) {
        printf("ROUTER_EVENT type=%s port=%u net=%u\n",
               type,
               (unsigned)event->port_id,
               (unsigned)event->net);
    }
}

static void router_sta_print_route(void *arg, const tb_router_route_snapshot *route)
{
    (void)arg;
    if (!route) {
        return;
    }

    printf("ROUTER_ROUTE port=%u port_net=%u dnet=%u age_ms=%lu static=%u next_hop=%u peer=%u\n",
           (unsigned)route->port_id,
           (unsigned)route->port_net,
           (unsigned)route->dnet,
           (unsigned long)route->age_ms,
           route->configured_static ? 1U : 0U,
           route->have_next_hop ? 1U : 0U,
           route->have_peer ? 1U : 0U);
}

static void router_sta_print_route_snapshot(uint32_t now_ms)
{
    route_snapshot_seq++;
    printf("ROUTER_ROUTE_SNAPSHOT seq=%lu\n", (unsigned long)route_snapshot_seq);
    tb_router_service_for_each_route(&router_service, now_ms, router_sta_print_route, NULL);
}

void router_sta_request_reboot(void)
{
    reboot_requested = true;
}

bool router_sta_reboot_is_requested(void)
{
    return reboot_requested;
}

static void router_sta_safe_reboot(void)
{
    printf("ROUTER_REBOOT begin\n");
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
    bip_port_close(&w5500_bip);
#endif
    udp_tbx_port_close(&udp_tbx);
#if CONFIG_ROUTER_STA_W5500_PROBE_ENABLE
    w5500_probe_stop_reset();
#endif
    if (wlan_initialized) {
        printf("ROUTER_REBOOT wlan_stop\n");
        app_wlan_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        printf("ROUTER_REBOOT wlan_hard_reset\n");
        mmhal_wlan_hard_reset();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    printf("ROUTER_REBOOT esp_restart\n");
    fflush(stdout);
    esp_restart();
}

void app_main(void)
{
    printf("\n\nESP32 HaLow Router STA (Built " __DATE__ " " __TIME__ ")\n\n");
    router_sta_init_nvs();

    const router_runtime_config boot_runtime_cfg = *router_runtime_config_get();
    const router_runtime_config *runtime_cfg = &boot_runtime_cfg;

    printf("MVP milestone: HaLow STA + UDP/TBX router-service scaffold. peer=%s:%d local_port=%d\n",
           CONFIG_ROUTER_STA_UDP_PEER_IP,
           CONFIG_ROUTER_STA_UDP_PEER_PORT,
           CONFIG_ROUTER_STA_UDP_LOCAL_PORT);
    printf("ROUTER_BUILD local_app=%u local_net=%u local_device=%lu\n",
           CONFIG_ROUTER_STA_LOCAL_APP_ENABLE ? 1U : 0U,
           CONFIG_ROUTER_STA_LOCAL_APP_ENABLE ? (unsigned)CONFIG_ROUTER_STA_LOCAL_APP_NET : 0U,
           CONFIG_ROUTER_STA_LOCAL_APP_ENABLE ? (unsigned long)CONFIG_ROUTER_STA_LOCAL_APP_DEVICE_ID : 0UL);
    printf("ROUTER_NUMBERING net_block=%u node_id=%u w5500_bip_net=%u w5500_bip_auto=%u mstp_net=%u mstp_auto=%u udp_tbx_net=%u\n",
           (unsigned)CONFIG_ROUTER_STA_NET_BLOCK,
           (unsigned)runtime_cfg->node_id,
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
           (unsigned)router_sta_w5500_bip_net(),
           CONFIG_ROUTER_STA_W5500_BIP_NET_AUTO ? 1U : 0U,
#else
           0U,
           0U,
#endif
#if CONFIG_ROUTER_STA_MSTP_ENABLE
           (unsigned)router_sta_mstp_net(),
           CONFIG_ROUTER_STA_MSTP_NET_AUTO ? 1U : 0U,
#else
           0U,
           0U,
#endif
           (unsigned)CONFIG_ROUTER_STA_UDP_TBX_NET);
    printf("ROUTER_RUNTIME speed=%s w5500_dhcp=%u w5500_ip=%s w5500_netmask=%s w5500_gw=%s mstp=%u\n",
           router_runtime_config_speed_name(runtime_cfg->speed),
           runtime_cfg->w5500_dhcp ? 1U : 0U,
           runtime_cfg->w5500_ip,
           runtime_cfg->w5500_netmask,
           runtime_cfg->w5500_gateway,
           runtime_cfg->mstp_enable ? 1U : 0U);

    router_cli_start();

    router_sta_print_memory("boot");
    if (reboot_requested) {
        router_sta_safe_reboot();
    }
    app_wlan_init();
    wlan_initialized = true;
    router_sta_print_memory("after_wlan_init");
    if (!app_wlan_start_until(router_sta_reboot_is_requested)) {
        router_sta_safe_reboot();
    }
    router_sta_print_memory("after_wlan_start");

    if (!udp_tbx_port_open(&udp_tbx,
                           CONFIG_ROUTER_STA_UDP_LOCAL_PORT,
                           CONFIG_ROUTER_STA_UDP_PEER_IP,
                           CONFIG_ROUTER_STA_UDP_PEER_PORT,
                           router_sta_tbx_origin_id())) {
        printf("UDP_TBX unavailable; router scaffold will not run\n");
    }

#if CONFIG_ROUTER_STA_W5500_PROBE_ENABLE
    (void)w5500_probe_start();
    router_sta_print_memory("after_w5500_probe");
#endif

#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
    if (!bip_port_open(&w5500_bip,
                       router_sta_w5500_bip_net(),
                       CONFIG_ROUTER_STA_W5500_BIP_UDP_PORT)) {
        printf("W5500_BIP unavailable; continuing without field-side BACnet/IP\n");
    }
    router_sta_print_memory("after_w5500_bip");
#endif

#if CONFIG_ROUTER_STA_RS485_PROBE_ENABLE
    (void)rs485_probe_start();
    router_sta_print_memory("after_rs485_probe");
#endif

#if CONFIG_ROUTER_STA_MSTP_ENABLE
    if (runtime_cfg->mstp_enable && !mstp_port_open(&mstp, router_sta_mstp_net())) {
        printf("MSTP unavailable; continuing without field-side MS/TP\n");
    } else if (!runtime_cfg->mstp_enable) {
        printf("MSTP disabled by runtime config\n");
    }
    router_sta_print_memory("after_mstp");
#endif

    (void)router_sta_configure_service();
    router_sta_print_memory("after_router_config");

    while (true) {
        if (reboot_requested) {
            router_sta_safe_reboot();
        }

        uint32_t now = mmosal_get_time_ms();
        udp_tbx_port_poll(&udp_tbx, &router_service, CONFIG_ROUTER_STA_UDP_TBX_PORT_ID, now);
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
        bip_port_poll(&w5500_bip, &router_service, CONFIG_ROUTER_STA_W5500_BIP_PORT_ID, now);
#endif
#if CONFIG_ROUTER_STA_RS485_PROBE_ENABLE
        rs485_probe_poll(now);
#endif
#if CONFIG_ROUTER_STA_MSTP_ENABLE
        if (runtime_cfg->mstp_enable) {
            mstp_port_poll(&mstp, &router_service, CONFIG_ROUTER_STA_MSTP_PORT_ID, now);
        }
#endif
        tb_router_service_tick(&router_service, now);
        tb_router_service_drain_events(&router_service, router_sta_print_event, NULL);
        router_sta_print_router_log();

        if (now - last_status_ms >= CONFIG_ROUTER_STA_STATUS_INTERVAL_MS) {
            last_status_ms = now;
            if (router_log_get(ROUTER_LOG_STATUS)) {
                printf("ROUTER_STA status link=%s rssi=%ld wir_seq=%lu\n",
                       app_link_is_up() ? "up" : "down",
                       (long)get_link_rssi(),
                       (unsigned long)router_probe_seq);
                udp_tbx_port_print_status(&udp_tbx);
#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE
                bip_port_print_status(&w5500_bip);
#endif
#if CONFIG_ROUTER_STA_MSTP_ENABLE
                if (runtime_cfg->mstp_enable) {
                    mstp_port_print_status(&mstp);
                }
#endif
                router_sta_print_route_snapshot(now);
            }
            router_sta_send_whois_router();
            router_sta_send_iam_router();
            router_sta_send_debug_app_probe();
        }

#if CONFIG_ROUTER_STA_W5500_BIP_ENABLE && CONFIG_ROUTER_STA_W5500_WIR_INTERVAL_MS > 0
        bool w5500_ready = w5500_bip.sock >= 0;
#if CONFIG_ROUTER_STA_W5500_PROBE_ENABLE
        w5500_ready = w5500_ready && w5500_probe_has_ip();
#endif
        if (w5500_ready &&
            (!w5500_ip_seen ||
             now - last_w5500_wir_ms >= CONFIG_ROUTER_STA_W5500_WIR_INTERVAL_MS)) {
            w5500_ip_seen = true;
            last_w5500_wir_ms = now;
            router_sta_send_w5500_whois_router();
        }
#endif

        mmosal_task_sleep(CONFIG_ROUTER_STA_LOOP_DELAY_MS);
    }
}
