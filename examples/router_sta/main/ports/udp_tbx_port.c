#include "udp_tbx_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "tbx.h"

static void origin_id_from_string(uint8_t out[TBX_ORIGIN_ID_LEN], const char *origin_id)
{
    memset(out, 0, TBX_ORIGIN_ID_LEN);
    if (!origin_id) {
        return;
    }
    size_t len = strlen(origin_id);
    if (len > TBX_ORIGIN_ID_LEN) {
        len = TBX_ORIGIN_ID_LEN;
    }
    memcpy(out, origin_id, len);
}

static void print_origin_id(const tbx_origin_t *origin)
{
    if (!origin || !origin->have_origin_id) {
        printf("none");
        return;
    }
    for (size_t i = 0; i < TBX_ORIGIN_ID_LEN; i++) {
        uint8_t c = origin->origin_id[i];
        putchar((c >= 32 && c <= 126) ? (int)c : '.');
    }
}

bool udp_tbx_port_open(udp_tbx_port *port,
                       uint16_t local_port,
                       const char *peer_ip,
                       uint16_t peer_port,
                       const char *origin_id)
{
    if (!port || !peer_ip) {
        return false;
    }
    memset(port, 0, sizeof(*port));
    port->sock = -1;
    origin_id_from_string(port->origin_id, origin_id);

    port->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (port->sock < 0) {
        printf("UDP_TBX_OPEN socket failed errno=%d\n", errno);
        return false;
    }

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(port->sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("UDP_TBX_OPEN bind port=%u failed errno=%d\n", (unsigned)local_port, errno);
        close(port->sock);
        port->sock = -1;
        return false;
    }

    int flags = fcntl(port->sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(port->sock, F_SETFL, flags | O_NONBLOCK);
    }

    port->peer.sin_family = AF_INET;
    port->peer.sin_port = htons(peer_port);
    port->peer.sin_addr.s_addr = inet_addr(peer_ip);

    printf("UDP_TBX_OPEN local_port=%u peer=%s:%u origin_id=%s\n",
           (unsigned)local_port, peer_ip, (unsigned)peer_port, origin_id ? origin_id : "");
    return true;
}

static bool is_local_origin(const udp_tbx_port *port, const tbx_origin_t *origin)
{
    return port && origin && origin->have_origin_id &&
           memcmp(origin->origin_id, port->origin_id, TBX_ORIGIN_ID_LEN) == 0;
}

void udp_tbx_port_send_npdu_direct(udp_tbx_port *port,
                                   const uint8_t *npdu,
                                   size_t npdu_len)
{
    if (!port || port->sock < 0 || !npdu || npdu_len == 0) {
        return;
    }

    uint8_t frame[UDP_TBX_PORT_MAX_FRAME];
    tbx_origin_t origin = {0};
    origin.have_origin_id = true;
    memcpy(origin.origin_id, port->origin_id, TBX_ORIGIN_ID_LEN);

    size_t frame_len = tbx_wrap_bacnet_npdu(frame, sizeof(frame), npdu, npdu_len, &origin);
    if (frame_len == 0) {
        printf("TX_TBX_FAIL npdu_len=%u\n", (unsigned)npdu_len);
        port->tx_errors++;
        return;
    }

    int sent = sendto(port->sock, frame, frame_len, 0,
                      (const struct sockaddr *)&port->peer, sizeof(port->peer));
    if (sent != (int)frame_len) {
        printf("TX_TBX_ERR errno=%d npdu_len=%u tbx_len=%u\n",
               errno, (unsigned)npdu_len, (unsigned)frame_len);
        port->tx_errors++;
        return;
    }

    port->tx_frames++;
    printf("TX_TBX npdu_len=%u tbx_len=%u tx_frames=%lu\n",
           (unsigned)npdu_len, (unsigned)frame_len, (unsigned long)port->tx_frames);
}

static void udp_tbx_router_send_npdu(void *user_ctx,
                                     tb_router_port *router_port,
                                     const uint8_t *npdu,
                                     size_t npdu_len,
                                     const BACNET_ADDRESS *daddr)
{
    (void)user_ctx;
    (void)daddr;
    udp_tbx_port *port = (udp_tbx_port *)tb_router_port_transport_state(router_port);
    udp_tbx_port_send_npdu_direct(port, npdu, npdu_len);
}

const tb_router_transport_ops k_udp_tbx_router_ops = {
    .name = "udp_tbx",
    .send_npdu = udp_tbx_router_send_npdu,
};

void udp_tbx_port_poll(udp_tbx_port *port,
                       tb_router_service *svc,
                       uint8_t router_port_id,
                       uint32_t now_ms)
{
    if (!port || port->sock < 0 || !svc) {
        return;
    }

    while (true) {
        uint8_t frame[UDP_TBX_PORT_MAX_FRAME];
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int got = recvfrom(port->sock, frame, sizeof(frame), 0,
                           (struct sockaddr *)&from, &from_len);
        if (got <= 0) {
            return;
        }

        tbx_origin_t origin = {0};
        const uint8_t *npdu = NULL;
        size_t npdu_len = 0;
        if (!tbx_unwrap_bacnet_npdu(frame, (size_t)got, &origin, &npdu, &npdu_len)) {
            port->rx_bad++;
            printf("RX_TBX_BAD from=%s:%u bytes=%d bad=%lu\n",
                   inet_ntoa(from.sin_addr), ntohs(from.sin_port), got,
                   (unsigned long)port->rx_bad);
            continue;
        }

        if (is_local_origin(port, &origin)) {
            port->rx_loop++;
            printf("RX_TBX_DROP_LOOP from=%s:%u bytes=%d loop=%lu\n",
                   inet_ntoa(from.sin_addr), ntohs(from.sin_port), got,
                   (unsigned long)port->rx_loop);
            continue;
        }

        port->rx_frames++;
        printf("RX_TBX from=%s:%u bytes=%d origin=", inet_ntoa(from.sin_addr), ntohs(from.sin_port), got);
        print_origin_id(&origin);
        printf(" npdu_len=%u\n", (unsigned)npdu_len);

        tb_router_route_hints hints = {0};
        if (tb_router_service_extract_route_hints(svc, npdu, npdu_len, &hints) && hints.count > 0) {
            printf("RX_TBX_ROUTE_HINTS count=%u", (unsigned)hints.count);
            for (size_t i = 0; i < hints.count; i++) {
                printf(" dnet=%u", (unsigned)hints.nets[i]);
            }
            putchar('\n');
        }

        uint8_t src_flags = origin.net_owner ? TB_ROUTER_SRC_FLAG_NET_OWNER : 0;
        int rc = tb_router_service_handle_frame(svc,
                                                router_port_id,
                                                npdu,
                                                npdu_len,
                                                NULL,
                                                0,
                                                src_flags,
                                                now_ms);
        if (rc >= 0) {
            port->rx_router_accepted++;
        }
        printf("RX_TBX_ROUTER rc=%d accepted=%lu\n", rc, (unsigned long)port->rx_router_accepted);
    }
}

void udp_tbx_port_print_status(const udp_tbx_port *port)
{
    if (!port) {
        return;
    }
    printf("UDP_TBX_STATUS tx=%lu rx=%lu bad=%lu loop=%lu router_rx=%lu tx_errors=%lu\n",
           (unsigned long)port->tx_frames,
           (unsigned long)port->rx_frames,
           (unsigned long)port->rx_bad,
           (unsigned long)port->rx_loop,
           (unsigned long)port->rx_router_accepted,
           (unsigned long)port->tx_errors);
}
