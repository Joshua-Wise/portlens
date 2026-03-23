#pragma once

#include "config.h"

#if FEATURE_BUTTON_NAV

#include "esp_err.h"

typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS,
} button_event_t;

/**
 * Initialize the button GPIO (active-low with internal pull-up).
 * Uses BUTTON_GPIO from config.h.
 */
esp_err_t button_init(void);

/**
 * Poll the button state. Call this every ~50ms from the main loop.
 * Returns BUTTON_EVENT_SHORT_PRESS on release after 50-500ms hold,
 * or BUTTON_EVENT_LONG_PRESS after 1000ms while still held.
 */
button_event_t button_poll(void);

#endif // FEATURE_BUTTON_NAV
