/*
 * ESP32 HaLow router station scaffold.
 *
 * This is the second MVP milestone: prove the Heltec HT-HC01P can connect as a
 * HaLow STA and exchange UDP/TBX-framed traffic with the Pi AP/router endpoint.
 * The BACnet router core is intentionally not copied yet; see
 * SYNC_FROM_WBACNET.md for the next milestone.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "mm_app_common.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "mmwlan_stats.h"
#include "tbx.h"

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

static int udp_sock = -1;
static uint32_t heartbeat_seq = 0;
static tbx_origin_t local_origin = {0};

static int32_t get_link_rssi(void)
{
    return mmwlan_get_rssi();
}

static void init_tbx_origin(void)
{
    local_origin.have_origin_id = true;

    const char *origin_id = CONFIG_ROUTER_STA_TBX_ORIGIN_ID;
    size_t origin_id_len = strlen(origin_id);
    if (origin_id_len > TBX_ORIGIN_ID_LEN)
    {
        origin_id_len = TBX_ORIGIN_ID_LEN;
    }
    memcpy(local_origin.origin_id, origin_id, origin_id_len);
}

static bool is_local_origin(const tbx_origin_t *origin)
{
    return origin && origin->have_origin_id &&
           memcmp(origin->origin_id, local_origin.origin_id, TBX_ORIGIN_ID_LEN) == 0;
}

static void print_origin_id(const tbx_origin_t *origin)
{
    if (!origin || !origin->have_origin_id)
    {
        printf("none");
        return;
    }

    for (size_t i = 0; i < TBX_ORIGIN_ID_LEN; i++)
    {
        uint8_t c = origin->origin_id[i];
        putchar((c >= 32 && c <= 126) ? (int)c : '.');
    }
}

static void udp_smoke_init(void)
{
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0)
    {
        printf("UDP/TBX smoke: socket failed errno=%d\n", errno);
        return;
    }

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(CONFIG_ROUTER_STA_UDP_LOCAL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udp_sock, (struct sockaddr *)&local, sizeof(local)) != 0)
    {
        printf("UDP/TBX smoke: bind port=%d failed errno=%d\n",
               CONFIG_ROUTER_STA_UDP_LOCAL_PORT, errno);
        close(udp_sock);
        udp_sock = -1;
        return;
    }

    int flags = fcntl(udp_sock, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(udp_sock, F_SETFL, flags | O_NONBLOCK);
    }
    printf("UDP/TBX smoke: local_port=%d peer=%s:%d origin_id=%s\n",
           CONFIG_ROUTER_STA_UDP_LOCAL_PORT,
           CONFIG_ROUTER_STA_UDP_PEER_IP,
           CONFIG_ROUTER_STA_UDP_PEER_PORT,
           CONFIG_ROUTER_STA_TBX_ORIGIN_ID);
}

static void udp_smoke_tick(void)
{
    if (udp_sock < 0)
    {
        return;
    }

    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(CONFIG_ROUTER_STA_UDP_PEER_PORT);
    peer.sin_addr.s_addr = inet_addr(CONFIG_ROUTER_STA_UDP_PEER_IP);

    char npdu[128];
    int npdu_len = snprintf(npdu, sizeof(npdu), "TBX_SMOKE seq=%lu rssi=%ld link=%s",
                            (unsigned long)heartbeat_seq++,
                            (long)get_link_rssi(),
                            app_link_is_up() ? "up" : "down");
    uint8_t frame[192];
    size_t frame_len = tbx_wrap_bacnet_npdu(frame, sizeof(frame),
                                            (const uint8_t *)npdu,
                                            (size_t)npdu_len,
                                            &local_origin);
    if (frame_len == 0)
    {
        printf("TX_TBX_SMOKE wrap failed npdu_len=%d\n", npdu_len);
        return;
    }

    int sent = sendto(udp_sock, frame, frame_len, 0, (struct sockaddr *)&peer, sizeof(peer));
    if (sent < 0)
    {
        printf("TX_TBX_SMOKE send failed errno=%d\n", errno);
    }
    else
    {
        printf("TX_TBX_SMOKE bytes=%d npdu_len=%d seq=%lu\n",
               sent, npdu_len, (unsigned long)(heartbeat_seq - 1));
    }

    while (true)
    {
        uint8_t rx[256];
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int got = recvfrom(udp_sock, rx, sizeof(rx), 0,
                           (struct sockaddr *)&from, &from_len);
        if (got <= 0)
        {
            break;
        }

        tbx_origin_t origin = {0};
        const uint8_t *npdu_rx = NULL;
        size_t npdu_rx_len = 0;
        if (tbx_unwrap_bacnet_npdu(rx, (size_t)got, &origin, &npdu_rx, &npdu_rx_len))
        {
            if (is_local_origin(&origin))
            {
                printf("RX_TBX_SMOKE dropped self-origin frame from %s:%u bytes=%d\n",
                       inet_ntoa(from.sin_addr), ntohs(from.sin_port), got);
                continue;
            }

            printf("RX_TBX_SMOKE from %s:%u bytes=%d origin=",
                   inet_ntoa(from.sin_addr), ntohs(from.sin_port), got);
            print_origin_id(&origin);
            printf(" npdu_len=%u payload=", (unsigned)npdu_rx_len);
            fwrite(npdu_rx, 1, npdu_rx_len, stdout);
            putchar('\n');
        }
        else
        {
            printf("RX_UDP_RAW from %s:%u bytes=%d payload=", inet_ntoa(from.sin_addr),
                   ntohs(from.sin_port), got);
            fwrite(rx, 1, (size_t)got, stdout);
            putchar('\n');
        }
    }
}

void app_main(void)
{
    printf("\n\nESP32 HaLow Router STA Scaffold (Built " __DATE__ " " __TIME__ ")\n\n");
    printf("MVP milestone: HaLow STA + UDP/TBX smoke. peer=%s:%d local_port=%d\n",
           CONFIG_ROUTER_STA_UDP_PEER_IP,
           CONFIG_ROUTER_STA_UDP_PEER_PORT,
           CONFIG_ROUTER_STA_UDP_LOCAL_PORT);

    init_tbx_origin();
    app_wlan_init();
    app_wlan_start();
    udp_smoke_init();

    while (true)
    {
        printf("ROUTER_STA status link=%s rssi=%ld seq=%lu\n",
               app_link_is_up() ? "up" : "down",
               (long)get_link_rssi(),
               (unsigned long)heartbeat_seq);
        udp_smoke_tick();
        mmosal_task_sleep(CONFIG_ROUTER_STA_STATUS_INTERVAL_MS);
    }
}
