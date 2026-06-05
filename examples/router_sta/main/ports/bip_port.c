#include "bip_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "mmosal.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "router_service.h"
#include "router_seg_diag.h"

#define BVLC_TYPE_BIP 0x81u
#define BVLC_ORIGINAL_UNICAST_NPDU 0x0au
#define BVLC_ORIGINAL_BROADCAST_NPDU 0x0bu
#define BVLC_HEADER_LEN 4u
#define BIP_MAC_LEN 6u

static const char *TAG = "router_seg_bip";

#ifndef CONFIG_ROUTER_STA_W5500_BIP_LOG_FRAMES
#define CONFIG_ROUTER_STA_W5500_BIP_LOG_FRAMES 0
#endif

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xffu);
}

static uint32_t parse_ipv4_or_broadcast(const char *text)
{
    struct in_addr addr = {0};
    if (text && inet_aton(text, &addr) != 0) {
        return addr.s_addr;
    }
    return htonl(INADDR_BROADCAST);
}

static void bip_mac_from_sockaddr(uint8_t mac[BIP_MAC_LEN], const struct sockaddr_in *addr)
{
    uint32_t ip = ntohl(addr->sin_addr.s_addr);
    mac[0] = (uint8_t)(ip >> 24);
    mac[1] = (uint8_t)(ip >> 16);
    mac[2] = (uint8_t)(ip >> 8);
    mac[3] = (uint8_t)ip;
    mac[4] = (uint8_t)(ntohs(addr->sin_port) >> 8);
    mac[5] = (uint8_t)ntohs(addr->sin_port);
}

static void bip_sockaddr_from_mac(struct sockaddr_in *addr, const uint8_t mac[BIP_MAC_LEN])
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(((uint32_t)mac[0] << 24) |
                                  ((uint32_t)mac[1] << 16) |
                                  ((uint32_t)mac[2] << 8) |
                                  (uint32_t)mac[3]);
    addr->sin_port = htons((uint16_t)(((uint16_t)mac[4] << 8) | mac[5]));
}

static void print_mac(const uint8_t *mac, size_t len)
{
    if (!mac || len == 0) {
        printf("-");
        return;
    }
    for (size_t i = 0; i < len; i++) {
        printf("%s%02x", i == 0 ? "" : ":", mac[i]);
    }
}

bool bip_port_open(bip_port *port, uint16_t net, uint16_t udp_port)
{
    if (!port || net == 0 || udp_port == 0) {
        return false;
    }
    memset(port, 0, sizeof(*port));
    port->sock = -1;
    port->net = net;
    port->udp_port = udp_port;

    port->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (port->sock < 0) {
        printf("BIP_OPEN socket failed errno=%d\n", errno);
        return false;
    }

    int yes = 1;
    (void)setsockopt(port->sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    (void)setsockopt(port->sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(udp_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(port->sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("BIP_OPEN bind port=%u failed errno=%d\n", (unsigned)udp_port, errno);
        close(port->sock);
        port->sock = -1;
        return false;
    }

    int flags = fcntl(port->sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(port->sock, F_SETFL, flags | O_NONBLOCK);
    }

    printf("BIP_OPEN net=%u bind=0.0.0.0:%u\n", (unsigned)net, (unsigned)udp_port);
    return true;
}

void bip_port_close(bip_port *port)
{
    if (!port || port->sock < 0) {
        return;
    }
    close(port->sock);
    port->sock = -1;
    printf("BIP_CLOSE net=%u\n", (unsigned)port->net);
}

static void bip_port_send_npdu_to(bip_port *port,
                                  const uint8_t *npdu,
                                  size_t npdu_len,
                                  const struct sockaddr_in *target,
                                  uint8_t function)
{
    if (!port || port->sock < 0 || !npdu || npdu_len == 0 || !target ||
        npdu_len + BVLC_HEADER_LEN > BIP_PORT_MAX_FRAME) {
        return;
    }

    port->tx_frame[0] = BVLC_TYPE_BIP;
    port->tx_frame[1] = function;
    write_be16(&port->tx_frame[2], (uint16_t)(npdu_len + BVLC_HEADER_LEN));
    memcpy(&port->tx_frame[BVLC_HEADER_LEN], npdu, npdu_len);

    size_t frame_len = npdu_len + BVLC_HEADER_LEN;
    int sent = sendto(port->sock, port->tx_frame, frame_len, 0,
                      (const struct sockaddr *)target, sizeof(*target));
#if ROUTER_SEG_DIAG_ENABLE
    ESP_LOGI(TAG,
             "TX_BIP_DIAG target=%s:%u bvlc_len=%u npdu_len=%u udp_payload=%u sendto_ret=%d errno=%d",
             inet_ntoa(target->sin_addr),
             (unsigned)ntohs(target->sin_port),
             (unsigned)frame_len,
             (unsigned)npdu_len,
             (unsigned)frame_len,
             sent,
             sent == (int)frame_len ? 0 : errno);
    router_seg_diag_log_npdu("TX_BIP_NPDU", npdu, npdu_len);
    if (frame_len >= ROUTER_SEG_DIAG_SAFE_UDP_PAYLOAD) {
        ESP_LOGW(TAG,
                 "TX_BIP_MTU_WARN bvlc_len=%u udp_payload=%u ethernet_payload=%u safe_udp_payload=%u ethernet_mtu=%u",
                 (unsigned)frame_len,
                 (unsigned)frame_len,
                 (unsigned)(frame_len + ROUTER_SEG_DIAG_IPV4_UDP_OVERHEAD),
                 ROUTER_SEG_DIAG_SAFE_UDP_PAYLOAD,
                 ROUTER_SEG_DIAG_ETHERNET_MTU);
    }
#endif
    if (sent != (int)frame_len) {
        printf("TX_BIP_ERR errno=%d npdu_len=%u bvlc_len=%u\n",
               errno, (unsigned)npdu_len, (unsigned)frame_len);
        port->tx_errors++;
        return;
    }

    port->tx_frames++;
#if CONFIG_ROUTER_STA_W5500_BIP_LOG_FRAMES
    printf("TX_BIP net=%u npdu_len=%u bvlc_len=%u target=%s:%u function=0x%02x tx_frames=%lu\n",
           (unsigned)port->net,
           (unsigned)npdu_len,
           (unsigned)frame_len,
           inet_ntoa(target->sin_addr),
           (unsigned)ntohs(target->sin_port),
           (unsigned)function,
           (unsigned long)port->tx_frames);
#endif
}

void bip_port_send_npdu_broadcast(bip_port *port, const uint8_t *npdu, size_t npdu_len)
{
    if (!port || port->sock < 0 || !npdu || npdu_len == 0) {
        return;
    }

    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr =
        parse_ipv4_or_broadcast(CONFIG_ROUTER_STA_W5500_BIP_BROADCAST_ADDR);
    target.sin_port = htons(port->udp_port);
    bip_port_send_npdu_to(port, npdu, npdu_len, &target, BVLC_ORIGINAL_BROADCAST_NPDU);
}

static void bip_router_send_npdu(tb_router_service *svc,
                                 void *user_ctx,
                                 tb_router_port *router_port,
                                 const uint8_t *npdu,
                                 size_t npdu_len,
                                 const BACNET_ADDRESS *daddr)
{
    (void)user_ctx;
    bip_port *port = (bip_port *)tb_router_port_transport_state(router_port);
    if (!port || port->sock < 0) {
        return;
    }

#if ROUTER_SEG_DIAG_ENABLE
    BACNET_ADDRESS dtmp = {0};
    BACNET_ADDRESS saddr = {0};
    (void)tb_router_npdu_decode_addresses(npdu, npdu_len, &dtmp, &saddr);
    ESP_LOGI(TAG,
             "ROUTER_EMIT_BIP in_port=? out_port=%u dnet=%u snet=%u npdu_len=%u bvlc_len=%u safe_udp_payload=%u",
             (unsigned)tb_router_port_id(router_port),
             daddr ? (unsigned)daddr->net : 0u,
             (unsigned)saddr.net,
             (unsigned)npdu_len,
             (unsigned)(npdu_len + BVLC_HEADER_LEN),
             ROUTER_SEG_DIAG_SAFE_UDP_PAYLOAD);
    router_seg_diag_log_npdu("ROUTER_EMIT_BIP_NPDU", npdu, npdu_len);
#endif

    struct sockaddr_in target = {0};
    uint8_t function = BVLC_ORIGINAL_BROADCAST_NPDU;
    if (daddr && daddr->len == BIP_MAC_LEN) {
        bip_sockaddr_from_mac(&target, daddr->adr);
        function = BVLC_ORIGINAL_UNICAST_NPDU;
    } else if (svc && daddr && daddr->net != 0 && daddr->net != BACNET_BROADCAST_NETWORK) {
        BACNET_ADDRESS next_hop = {0};
        if (tb_router_service_lookup_next_hop(svc,
                                              tb_router_port_id(router_port),
                                              daddr->net,
                                              &next_hop) &&
            next_hop.len == BIP_MAC_LEN) {
            bip_sockaddr_from_mac(&target, next_hop.adr);
            function = BVLC_ORIGINAL_UNICAST_NPDU;
#if CONFIG_ROUTER_STA_W5500_BIP_LOG_FRAMES
            printf("TX_BIP_NEXT_HOP dnet=%u next_hop=",
                   (unsigned)daddr->net);
            print_mac(next_hop.adr, next_hop.len);
            putchar('\n');
#endif
        } else {
            target.sin_family = AF_INET;
            target.sin_addr.s_addr =
                parse_ipv4_or_broadcast(CONFIG_ROUTER_STA_W5500_BIP_BROADCAST_ADDR);
            target.sin_port = htons(port->udp_port);
        }
    } else {
        target.sin_family = AF_INET;
        target.sin_addr.s_addr =
            parse_ipv4_or_broadcast(CONFIG_ROUTER_STA_W5500_BIP_BROADCAST_ADDR);
        target.sin_port = htons(port->udp_port);
    }

    bip_port_send_npdu_to(port, npdu, npdu_len, &target, function);
}

const tb_router_transport_ops k_bip_router_ops = {
    .name = "bacnet_ip",
    .send_npdu = bip_router_send_npdu,
};

void bip_port_poll(bip_port *port,
                   tb_router_service *svc,
                   uint8_t router_port_id,
                   uint32_t now_ms)
{
    if (!port || port->sock < 0 || !svc) {
        return;
    }

    while (true) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int got = recvfrom(port->sock, port->rx_frame, sizeof(port->rx_frame), 0,
                           (struct sockaddr *)&from, &from_len);
        if (got <= 0) {
            return;
        }

        if (got < (int)BVLC_HEADER_LEN ||
            port->rx_frame[0] != BVLC_TYPE_BIP ||
            (port->rx_frame[1] != BVLC_ORIGINAL_UNICAST_NPDU &&
             port->rx_frame[1] != BVLC_ORIGINAL_BROADCAST_NPDU) ||
            read_be16(&port->rx_frame[2]) != (uint16_t)got) {
            port->rx_bad++;
            printf("RX_BIP_BAD from=%s:%u bytes=%d bad=%lu\n",
                   inet_ntoa(from.sin_addr),
                   (unsigned)ntohs(from.sin_port),
                   got,
                   (unsigned long)port->rx_bad);
            continue;
        }

        uint8_t sadr[BIP_MAC_LEN];
        bip_mac_from_sockaddr(sadr, &from);
        const uint8_t *npdu = &port->rx_frame[BVLC_HEADER_LEN];
        size_t npdu_len = (size_t)got - BVLC_HEADER_LEN;
        uint16_t bvlc_len = read_be16(&port->rx_frame[2]);

        port->rx_frames++;
#if ROUTER_SEG_DIAG_ENABLE
        ESP_LOGI(TAG,
                 "RX_BIP_DIAG net=%u from=%s:%u raw_bvlc_len=%u recv_len=%d npdu_len=%u function=0x%02x",
                 (unsigned)port->net,
                 inet_ntoa(from.sin_addr),
                 (unsigned)ntohs(from.sin_port),
                 (unsigned)bvlc_len,
                 got,
                 (unsigned)npdu_len,
                 (unsigned)port->rx_frame[1]);
        router_seg_diag_log_npdu("RX_BIP_NPDU", npdu, npdu_len);
#endif
#if CONFIG_ROUTER_STA_W5500_BIP_LOG_FRAMES
        printf("RX_BIP net=%u from=%s:%u npdu_len=%u sadr=",
               (unsigned)port->net,
               inet_ntoa(from.sin_addr),
               (unsigned)ntohs(from.sin_port),
               (unsigned)npdu_len);
        print_mac(sadr, sizeof(sadr));
        putchar('\n');
#endif

        uint32_t handle_start_ms = mmosal_get_time_ms();
        int rc = tb_router_service_handle_frame(svc,
                                                router_port_id,
                                                npdu,
                                                npdu_len,
                                                sadr,
                                                sizeof(sadr),
                                                0,
                                                now_ms);
        uint32_t handle_ms = mmosal_get_time_ms() - handle_start_ms;
        port->handle_count++;
        port->handle_total_ms += handle_ms;
        if (handle_ms > port->handle_max_ms) {
            port->handle_max_ms = handle_ms;
        }
        if (rc >= 0) {
            port->rx_router_accepted++;
        }
#if CONFIG_ROUTER_STA_W5500_BIP_LOG_FRAMES
        printf("RX_BIP_ROUTER rc=%d accepted=%lu\n", rc, (unsigned long)port->rx_router_accepted);
#endif
    }
}

void bip_port_print_status(const bip_port *port)
{
    if (!port) {
        return;
    }
    uint32_t handle_avg_ms = port->handle_count ?
        (port->handle_total_ms / port->handle_count) : 0;
    printf("BIP_STATUS net=%u tx=%lu rx=%lu bad=%lu router_rx=%lu tx_errors=%lu handle_ms_avg=%lu handle_ms_max=%lu\n",
           (unsigned)port->net,
           (unsigned long)port->tx_frames,
           (unsigned long)port->rx_frames,
           (unsigned long)port->rx_bad,
           (unsigned long)port->rx_router_accepted,
           (unsigned long)port->tx_errors,
           (unsigned long)handle_avg_ms,
           (unsigned long)port->handle_max_ms);
}
