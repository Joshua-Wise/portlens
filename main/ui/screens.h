#pragma once

#include <stdint.h>
#include <stdbool.h>

// Data screens that the user cycles through
typedef enum {
    SCREEN_SUMMARY = 0,
    SCREEN_NETWORK,
    SCREEN_SWITCH,
    SCREEN_CONNECTIVITY,
    SCREEN_COUNT
} screen_id_t;

// Link-layer info passed from main to screen renderer
typedef struct {
    bool link_up;
    bool is_100m;
    bool full_duplex;
} link_info_t;

/** Get the next screen ID (wraps around). */
screen_id_t screen_next(screen_id_t current);
