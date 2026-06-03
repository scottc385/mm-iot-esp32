#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "router_service.h"
#include "router_transport.h"

#define LOCAL_APP_OBJECT_NAME_MAX 64

typedef struct {
    bool active;
    tb_router_service *router_service;
    uint8_t port_id;
    uint16_t net;
    uint32_t device_id;
    char object_name[LOCAL_APP_OBJECT_NAME_MAX];
} local_app_port;

extern const tb_router_transport_ops k_local_app_router_ops;

void local_app_port_init(local_app_port *app,
                         tb_router_service *router_service,
                         uint8_t port_id,
                         uint16_t net,
                         uint32_t device_id,
                         const char *object_name);
