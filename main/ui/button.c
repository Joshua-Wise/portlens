#include "config.h"

#if FEATURE_BUTTON_NAV

#include "ui/button.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "button";

// Timing thresholds (in ticks — with CONFIG_FREERTOS_HZ=1000, 1 tick = 1ms)
#define DEBOUNCE_TICKS  pdMS_TO_TICKS(50)
#define SHORT_MAX_TICKS pdMS_TO_TICKS(500)
#define LONG_TICKS      pdMS_TO_TICKS(1000)

static bool s_was_pressed = false;
static TickType_t s_press_start = 0;
static bool s_long_fired = false;

esp_err_t button_init(void)
{
    ESP_LOGI(TAG, "Initializing button on GPIO %d", BUTTON_GPIO);

    // GPIO 34-39 are input-only on ESP32 and lack internal pull-up/pull-down.
    // The Olimex board has an external 10k pull-up on GPIO 34 (BUT1).
    bool can_pull_up = (BUTTON_GPIO < 34);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = can_pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

button_event_t button_poll(void)
{
    bool pressed = (gpio_get_level(BUTTON_GPIO) == 0); // active-low
    TickType_t now = xTaskGetTickCount();

    if (pressed && !s_was_pressed) {
        // Press started
        s_press_start = now;
        s_was_pressed = true;
        s_long_fired = false;
    } else if (pressed && s_was_pressed && !s_long_fired) {
        // Still held — check for long press
        if ((now - s_press_start) >= LONG_TICKS) {
            s_long_fired = true;
            return BUTTON_EVENT_LONG_PRESS;
        }
    } else if (!pressed && s_was_pressed) {
        // Released
        s_was_pressed = false;
        TickType_t elapsed = now - s_press_start;
        if (!s_long_fired && elapsed >= DEBOUNCE_TICKS && elapsed < SHORT_MAX_TICKS) {
            return BUTTON_EVENT_SHORT_PRESS;
        }
    }

    return BUTTON_EVENT_NONE;
}

#endif // FEATURE_BUTTON_NAV
