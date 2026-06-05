#pragma once

#include <stddef.h>
#include <stdint.h>

void router_bacnet_trace_npdu(const char *dir,
                              const char *transport,
                              const uint8_t *npdu,
                              size_t npdu_len);

