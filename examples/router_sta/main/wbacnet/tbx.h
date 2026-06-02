#ifndef TBX_H
#define TBX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TBX_MAGIC0 0x54 /* 'T' */
#define TBX_MAGIC1 0x42 /* 'B' */

/* TBX message types */
#define TBX_TYPE_BACNET_NPDU 1

/* BACNET_NPDU flags */
#define TBX_BNPDU_FLAG_RLOC16 0x01
#define TBX_BNPDU_FLAG_IPV6   0x02
#define TBX_BNPDU_FLAG_NET_OWNER 0x04
#define TBX_BNPDU_FLAG_ORIGIN_ID 0x08

#define TBX_ORIGIN_ID_LEN 4

typedef struct {
    bool have_rloc16;
    uint16_t rloc16;
    bool have_ipv6;
    uint8_t ipv6[16];
    bool have_origin_id;
    uint8_t origin_id[TBX_ORIGIN_ID_LEN];
    bool net_owner;
} tbx_origin_t;

size_t tbx_wrap_bacnet_npdu(uint8_t *out,
                            size_t out_len,
                            const uint8_t *npdu,
                            size_t npdu_len,
                            const tbx_origin_t *origin);

bool tbx_unwrap_bacnet_npdu(const uint8_t *buf,
                            size_t len,
                            tbx_origin_t *origin,
                            const uint8_t **npdu,
                            size_t *npdu_len);

#endif /* TBX_H */
