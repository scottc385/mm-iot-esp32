#pragma once

#include <stdbool.h>

bool w5500_probe_start(void);
void w5500_probe_stop_reset(void);
bool w5500_probe_has_ip(void);
