#pragma once

#include <stdbool.h>

typedef enum {
    ROUTER_LOG_BACNET = 0,
    ROUTER_LOG_SEGMENT_DIAG,
    ROUTER_LOG_STATUS,
} router_log_flag;

bool router_log_get(router_log_flag flag);
void router_log_set(router_log_flag flag, bool enabled);
const char *router_log_name(router_log_flag flag);

