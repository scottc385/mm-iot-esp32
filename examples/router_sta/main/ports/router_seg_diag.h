#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bacnet/bacaddr.h"

#ifndef ROUTER_SEG_DIAG_ENABLE
#ifdef CONFIG_ROUTER_STA_SEGMENT_DIAG_LOG
#define ROUTER_SEG_DIAG_ENABLE CONFIG_ROUTER_STA_SEGMENT_DIAG_LOG
#else
#define ROUTER_SEG_DIAG_ENABLE 1
#endif
#endif

#define ROUTER_SEG_DIAG_SAFE_UDP_PAYLOAD 1472u
#define ROUTER_SEG_DIAG_ETHERNET_MTU 1500u
#define ROUTER_SEG_DIAG_IPV4_UDP_OVERHEAD 28u

void router_seg_diag_log_npdu(const char *tag,
                              const uint8_t *npdu,
                              size_t npdu_len);
void router_seg_diag_log_addresses(const char *tag,
                                   const BACNET_ADDRESS *daddr,
                                   const BACNET_ADDRESS *saddr);

