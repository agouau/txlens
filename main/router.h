#pragma once

#include <stdbool.h>

typedef enum {
    ROUTER_CONNECTING,
    ROUTER_NOT_CONFIGURED,
    ROUTER_WAITING_IP,
    ROUTER_READY,
    ROUTER_SUBNET_CONFLICT,
} router_state_t;

typedef struct {
    router_state_t state;
    unsigned clients;
    char sta_ip[16];
    char ap_ip[16];
} router_status_t;

void router_start(void);
router_status_t router_get_status(void);
