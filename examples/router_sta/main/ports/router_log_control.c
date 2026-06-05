#include "router_log_control.h"

#include "sdkconfig.h"

#ifndef CONFIG_ROUTER_STA_SEGMENT_DIAG_LOG
#define CONFIG_ROUTER_STA_SEGMENT_DIAG_LOG 0
#endif

static volatile bool s_bacnet_log;
static volatile bool s_segment_diag_log = CONFIG_ROUTER_STA_SEGMENT_DIAG_LOG;
static volatile bool s_status_log = true;

bool router_log_get(router_log_flag flag)
{
    switch (flag) {
    case ROUTER_LOG_BACNET:
        return s_bacnet_log;
    case ROUTER_LOG_SEGMENT_DIAG:
        return s_segment_diag_log;
    case ROUTER_LOG_STATUS:
        return s_status_log;
    default:
        return false;
    }
}

void router_log_set(router_log_flag flag, bool enabled)
{
    switch (flag) {
    case ROUTER_LOG_BACNET:
        s_bacnet_log = enabled;
        break;
    case ROUTER_LOG_SEGMENT_DIAG:
        s_segment_diag_log = enabled;
        break;
    case ROUTER_LOG_STATUS:
        s_status_log = enabled;
        break;
    default:
        break;
    }
}

const char *router_log_name(router_log_flag flag)
{
    switch (flag) {
    case ROUTER_LOG_BACNET:
        return "bacnet";
    case ROUTER_LOG_SEGMENT_DIAG:
        return "seg";
    case ROUTER_LOG_STATUS:
        return "status";
    default:
        return "?";
    }
}

