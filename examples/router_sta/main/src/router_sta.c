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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "mm_app_common.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "bacnet/bacenum.h"
#include "bacnet/npdu.h"
#include "local_app_port.h"
#include "router_service.h"
#include "router_transport.h"
#include "udp_tbx_port.h"

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

#ifndef CONFIG_ROUTER_STA_TBX_ORIGIN_ID
#define CONFIG_ROUTER_STA_TBX_ORIGIN_ID "ESP1"
#endif

#ifndef CONFIG_ROUTER_STA_UDP_TBX_NET
#define CONFIG_ROUTER_STA_UDP_TBX_NET 65000
#endif

#ifndef CONFIG_ROUTER_STA_UDP_TBX_PORT_ID
#define CONFIG_ROUTER_STA_UDP_TBX_PORT_ID 1
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

static tb_router_service router_service;
static udp_tbx_port udp_tbx;
static local_app_port local_app;
static uint32_t router_probe_seq;
static uint32_t router_advertise_seq;
static uint32_t debug_app_probe_seq;
static uint32_t last_status_ms;
static uint32_t route_snapshot_seq;

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

static bool router_sta_configure_service(void)
{
    tb_router_port_config ports[2];
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

    bool ok = tb_router_service_configure(&router_service,
                                          NULL,
                                          ports,
                                          port_count,
                                          CONFIG_ROUTER_STA_DNET_TTL_MS,
                                          CONFIG_ROUTER_STA_LOG_LEVEL);
    printf("ROUTER_CONFIG %s ports=%u tbx_net=%u local_app=%u local_net=%u\n",
           ok ? "ok" : "failed",
           (unsigned)port_count,
           (unsigned)CONFIG_ROUTER_STA_UDP_TBX_NET,
           local_app.active ? 1U : 0U,
           local_app.active ? (unsigned)local_app.net : 0U);
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
    printf("TX_ROUTER_WIR seq=%lu npdu_len=%u\n",
           (unsigned long)router_probe_seq, (unsigned)len);
    udp_tbx_port_send_npdu_direct(&udp_tbx, npdu, len);
}

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
    printf("TX_ROUTER_IAR seq=%lu nets=%u npdu_len=%u\n",
           (unsigned long)router_advertise_seq,
           (unsigned)local_app.net,
           (unsigned)len);
    udp_tbx_port_send_npdu_direct(&udp_tbx, npdu, len);
#endif
}

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

    printf("ROUTER_EVENT type=%s port=%u net=%u\n",
           type,
           (unsigned)event->port_id,
           (unsigned)event->net);
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

void app_main(void)
{
    printf("\n\nESP32 HaLow Router STA (Built " __DATE__ " " __TIME__ ")\n\n");
    printf("MVP milestone: HaLow STA + UDP/TBX router-service scaffold. peer=%s:%d local_port=%d\n",
           CONFIG_ROUTER_STA_UDP_PEER_IP,
           CONFIG_ROUTER_STA_UDP_PEER_PORT,
           CONFIG_ROUTER_STA_UDP_LOCAL_PORT);
    printf("ROUTER_BUILD local_app=%u local_net=%u local_device=%lu\n",
           CONFIG_ROUTER_STA_LOCAL_APP_ENABLE ? 1U : 0U,
           CONFIG_ROUTER_STA_LOCAL_APP_ENABLE ? (unsigned)CONFIG_ROUTER_STA_LOCAL_APP_NET : 0U,
           CONFIG_ROUTER_STA_LOCAL_APP_ENABLE ? (unsigned long)CONFIG_ROUTER_STA_LOCAL_APP_DEVICE_ID : 0UL);

    router_sta_print_memory("boot");
    app_wlan_init();
    router_sta_print_memory("after_wlan_init");
    app_wlan_start();
    router_sta_print_memory("after_wlan_start");

    if (!udp_tbx_port_open(&udp_tbx,
                           CONFIG_ROUTER_STA_UDP_LOCAL_PORT,
                           CONFIG_ROUTER_STA_UDP_PEER_IP,
                           CONFIG_ROUTER_STA_UDP_PEER_PORT,
                           CONFIG_ROUTER_STA_TBX_ORIGIN_ID)) {
        printf("UDP_TBX unavailable; router scaffold will not run\n");
    }

    (void)router_sta_configure_service();
    router_sta_print_memory("after_router_config");

    while (true) {
        uint32_t now = mmosal_get_time_ms();
        udp_tbx_port_poll(&udp_tbx, &router_service, CONFIG_ROUTER_STA_UDP_TBX_PORT_ID, now);
        tb_router_service_tick(&router_service, now);
        tb_router_service_drain_events(&router_service, router_sta_print_event, NULL);
        router_sta_print_router_log();

        if (now - last_status_ms >= CONFIG_ROUTER_STA_STATUS_INTERVAL_MS) {
            last_status_ms = now;
            printf("ROUTER_STA status link=%s rssi=%ld wir_seq=%lu\n",
                   app_link_is_up() ? "up" : "down",
                   (long)get_link_rssi(),
                   (unsigned long)router_probe_seq);
            udp_tbx_port_print_status(&udp_tbx);
            router_sta_print_route_snapshot(now);
            router_sta_send_whois_router();
            router_sta_send_iam_router();
            router_sta_send_debug_app_probe();
        }

        mmosal_task_sleep(50);
    }
}
