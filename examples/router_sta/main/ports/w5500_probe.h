#pragma once

#include <stdbool.h>
#include <stdint.h>

bool w5500_probe_start(void);
void w5500_probe_stop_reset(void);
bool w5500_probe_has_ip(void);
bool w5500_probe_get_ip_info(uint32_t *ip_addr, uint32_t *netmask, uint32_t *gateway);
