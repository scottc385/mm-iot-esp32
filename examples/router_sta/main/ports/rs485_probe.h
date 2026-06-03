#pragma once

#include <stdbool.h>
#include <stdint.h>

bool rs485_probe_start(void);
void rs485_probe_poll(uint32_t now_ms);
