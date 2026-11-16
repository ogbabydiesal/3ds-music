// Shared 3DS networking + config: target host/port stored on SD, UDP socket, OSC send.
#pragma once
#include <3ds.h>
#include <stdbool.h>

typedef struct {
    char host[64];
    int  port;
    char prefix[32];
} NetConfig;

extern NetConfig net_cfg;
extern volatile unsigned long net_sent, net_errors;

void net_config_load(void);
void net_config_save(void);

bool net_init(void);      // socInit + UDP socket; call after config load
void net_exit(void);
void net_update_dest(void);

// Send an OSC message to exactly `address` (no prefix). types as in osc_encode.
void net_send(const char *address, const char *types, ...);

// Open the on-screen keyboard to edit IP + port, then save and apply.
// Must be called outside a citro3d frame.
void net_edit_settings(void);

// This 3DS's IP as a dotted string.
const char *net_local_ip(void);
