#include "tbx.h"

#include <string.h>

size_t tbx_wrap_bacnet_npdu(uint8_t *out,
                            size_t out_len,
                            const uint8_t *npdu,
                            size_t npdu_len,
                            const tbx_origin_t *origin)
{
    if (!out || !npdu) {
        return 0;
    }
    uint8_t flags = 0;
    uint8_t rloc_len = 0;
    uint8_t ipv6_len = 0;
    uint8_t origin_id_len = 0;
    if (origin) {
        if (origin->have_rloc16) {
            flags |= TBX_BNPDU_FLAG_RLOC16;
            rloc_len = 2;
        }
        if (origin->have_ipv6) {
            flags |= TBX_BNPDU_FLAG_IPV6;
            ipv6_len = 16;
        }
        if (origin->have_origin_id) {
            flags |= TBX_BNPDU_FLAG_ORIGIN_ID;
            origin_id_len = TBX_ORIGIN_ID_LEN;
        }
        if (origin->net_owner) {
            flags |= TBX_BNPDU_FLAG_NET_OWNER;
        }
    }

    uint8_t hlen = (uint8_t)(5 + rloc_len + ipv6_len + origin_id_len);
    size_t total = (size_t)hlen + npdu_len;
    if (total > out_len) {
        return 0;
    }

    out[0] = TBX_MAGIC0;
    out[1] = TBX_MAGIC1;
    out[2] = TBX_TYPE_BACNET_NPDU;
    out[3] = hlen;
    out[4] = flags;
    size_t offset = 5;
    if (rloc_len) {
        out[offset++] = (uint8_t)(origin->rloc16 >> 8);
        out[offset++] = (uint8_t)(origin->rloc16 & 0xff);
    }
    if (ipv6_len) {
        memcpy(&out[offset], origin->ipv6, 16);
        offset += 16;
    }
    if (origin_id_len) {
        memcpy(&out[offset], origin->origin_id, TBX_ORIGIN_ID_LEN);
        offset += TBX_ORIGIN_ID_LEN;
    }
    memcpy(&out[hlen], npdu, npdu_len);
    return total;
}

bool tbx_unwrap_bacnet_npdu(const uint8_t *buf,
                            size_t len,
                            tbx_origin_t *origin,
                            const uint8_t **npdu,
                            size_t *npdu_len)
{
    if (!buf || len < 5 || !npdu || !npdu_len) {
        return false;
    }
    if (buf[0] != TBX_MAGIC0 || buf[1] != TBX_MAGIC1) {
        return false;
    }
    if (buf[2] != TBX_TYPE_BACNET_NPDU) {
        return false;
    }
    uint8_t hlen = buf[3];
    if (hlen < 5 || hlen > len) {
        return false;
    }
    uint8_t flags = buf[4];
    size_t offset = 5;
    if (origin) {
        origin->have_rloc16 = false;
        origin->have_ipv6 = false;
        origin->have_origin_id = false;
        origin->net_owner = false;
        origin->rloc16 = 0;
        memset(origin->ipv6, 0, sizeof(origin->ipv6));
        memset(origin->origin_id, 0, sizeof(origin->origin_id));
    }
    if (flags & TBX_BNPDU_FLAG_RLOC16) {
        if (offset + 2 > hlen) {
            return false;
        }
        if (origin) {
            origin->have_rloc16 = true;
            origin->rloc16 = (uint16_t)((buf[offset] << 8) | buf[offset + 1]);
        }
        offset += 2;
    }
    if (flags & TBX_BNPDU_FLAG_IPV6) {
        if (offset + 16 > hlen) {
            return false;
        }
        if (origin) {
            origin->have_ipv6 = true;
            memcpy(origin->ipv6, &buf[offset], 16);
        }
        offset += 16;
    }
    if (flags & TBX_BNPDU_FLAG_ORIGIN_ID) {
        if (offset + TBX_ORIGIN_ID_LEN > hlen) {
            return false;
        }
        if (origin) {
            origin->have_origin_id = true;
            memcpy(origin->origin_id, &buf[offset], TBX_ORIGIN_ID_LEN);
        }
        offset += TBX_ORIGIN_ID_LEN;
    }
    if (origin && (flags & TBX_BNPDU_FLAG_NET_OWNER)) {
        origin->net_owner = true;
    }

    *npdu = &buf[hlen];
    *npdu_len = len - hlen;
    return true;
}
