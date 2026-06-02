/*
 * ESP32 HaLow router station scaffold.
 *
 * Current milestone: Heltec HT-HC01P joins the Pi HaLow AP, opens a UDP/TBX
 * transport, configures the copied portable router service, and periodically
 * sends a real BACnet network-layer Who-Is-Router-To-Network NPDU over TBX.
 * Inbound TBX frames are unwrapped and submitted to the router service.
 */

#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"

#include "mm_app_common.h"
#include "mmosal.h"
#include "mmwlan.h"
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

#ifndef CONFIG_ROUTER_STA_DNET_TTL_MS
#define CONFIG_ROUTER_STA_DNET_TTL_MS 60000
#endif

#ifndef CONFIG_ROUTER_STA_LOG_LEVEL
#define CONFIG_ROUTER_STA_LOG_LEVEL 1
#endif

static tb_router_service router_service;
static udp_tbx_port udp_tbx;
static uint32_t router_probe_seq;
static uint32_t last_status_ms;

static int32_t get_link_rssi(void)
{
    return mmwlan_get_rssi();
}

static bool router_sta_configure_service(void)
{
    tb_router_port_config ports[] = {
        {
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
        },
    };

    bool ok = tb_router_service_configure(&router_service,
                                          NULL,
                                          ports,
                                          sizeof(ports) / sizeof(ports[0]),
                                          CONFIG_ROUTER_STA_DNET_TTL_MS,
                                          CONFIG_ROUTER_STA_LOG_LEVEL);
    printf("ROUTER_CONFIG %s ports=%u tbx_net=%u\n",
           ok ? "ok" : "failed",
           (unsigned)(sizeof(ports) / sizeof(ports[0])),
           (unsigned)CONFIG_ROUTER_STA_UDP_TBX_NET);
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

static void router_sta_print_router_log(void)
{
    size_t log_len = 0;
    const char *log = tb_router_service_get_log(&router_service, &log_len);
    if (log && log_len > 0) {
        printf("ROUTER_LOG %.*s\n", (int)log_len, log);
        tb_router_service_clear_log(&router_service);
    }
}

void app_main(void)
{
    printf("\n\nESP32 HaLow Router STA (Built " __DATE__ " " __TIME__ ")\n\n");
    printf("MVP milestone: HaLow STA + UDP/TBX router-service scaffold. peer=%s:%d local_port=%d\n",
           CONFIG_ROUTER_STA_UDP_PEER_IP,
           CONFIG_ROUTER_STA_UDP_PEER_PORT,
           CONFIG_ROUTER_STA_UDP_LOCAL_PORT);

    app_wlan_init();
    app_wlan_start();

    if (!udp_tbx_port_open(&udp_tbx,
                           CONFIG_ROUTER_STA_UDP_LOCAL_PORT,
                           CONFIG_ROUTER_STA_UDP_PEER_IP,
                           CONFIG_ROUTER_STA_UDP_PEER_PORT,
                           CONFIG_ROUTER_STA_TBX_ORIGIN_ID)) {
        printf("UDP_TBX unavailable; router scaffold will not run\n");
    }

    (void)router_sta_configure_service();

    while (true) {
        uint32_t now = mmosal_get_time_ms();
        udp_tbx_port_poll(&udp_tbx, &router_service, CONFIG_ROUTER_STA_UDP_TBX_PORT_ID, now);
        tb_router_service_tick(&router_service, now);
        router_sta_print_router_log();

        if (now - last_status_ms >= CONFIG_ROUTER_STA_STATUS_INTERVAL_MS) {
            last_status_ms = now;
            printf("ROUTER_STA status link=%s rssi=%ld wir_seq=%lu\n",
                   app_link_is_up() ? "up" : "down",
                   (long)get_link_rssi(),
                   (unsigned long)router_probe_seq);
            udp_tbx_port_print_status(&udp_tbx);
            router_sta_send_whois_router();
        }

        mmosal_task_sleep(50);
    }
}
