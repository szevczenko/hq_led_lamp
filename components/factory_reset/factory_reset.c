#include "factory_reset.h"

#include <string.h>

#include "hal_gpio.h"

#define FACTORY_RESET_DEBOUNCE_MS 50u

typedef struct factory_reset_state {
    bool initialized;
    bool pressed;
    bool triggered;
    bool wait_release;
    bool indicator_active;
    uint32_t pressed_since_ms;
    factory_reset_config_t config;
} factory_reset_state_t;

static factory_reset_state_t s_reset;

static void factory_reset_set_indicator(bool active)
{
    if (s_reset.indicator_active == active) {
        return;
    }
    s_reset.indicator_active = active;
    if (s_reset.config.indicator != NULL) {
        s_reset.config.indicator(active, s_reset.config.context);
    }
}

factory_reset_status_t factory_reset_init(const factory_reset_config_t *config)
{
    hal_gpio_config_t gpio_config;

    if (config == NULL || config->pin == HAL_PIN_NONE || config->hold_ms == 0u ||
        config->erase == NULL || config->restart == NULL) {
        return FACTORY_RESET_ERR_INVALID_ARGUMENT;
    }
    if (s_reset.initialized) {
        return FACTORY_RESET_ERR_ALREADY_INITIALIZED;
    }

    gpio_config.pin = config->pin;
    gpio_config.polarity = config->active_low ? HAL_POLARITY_ACTIVE_LOW
                                              : HAL_POLARITY_ACTIVE_HIGH;
    gpio_config.pull = config->active_low ? HAL_GPIO_PULL_UP
                                          : HAL_GPIO_PULL_DOWN;
    gpio_config.output = false;
    if (hal_gpio_init(&gpio_config) != HAL_OK) {
        return FACTORY_RESET_ERR_GPIO;
    }
    memset(&s_reset, 0, sizeof(s_reset));
    s_reset.initialized = true;
    s_reset.config = *config;
    return FACTORY_RESET_OK;
}

void factory_reset_poll(uint32_t now_ms)
{
    bool active = false;

    if (!s_reset.initialized || s_reset.triggered ||
        hal_gpio_read(s_reset.config.pin, &active) != HAL_OK) {
        return;
    }
    if (!active) {
        s_reset.pressed = false;
        s_reset.wait_release = false;
        s_reset.pressed_since_ms = 0u;
        factory_reset_set_indicator(false);
        return;
    }
    if (s_reset.wait_release) {
        return;
    }
    if (!s_reset.pressed) {
        s_reset.pressed = true;
        s_reset.pressed_since_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - s_reset.pressed_since_ms) <
        (s_reset.config.hold_ms + FACTORY_RESET_DEBOUNCE_MS)) {
        factory_reset_set_indicator(true);
        return;
    }

    s_reset.triggered = true;
    factory_reset_set_indicator(true);
    if (!s_reset.config.erase(s_reset.config.context)) {
        s_reset.triggered = false;
        s_reset.wait_release = true;
        factory_reset_set_indicator(false);
        return;
    }
    s_reset.config.restart(s_reset.config.context);
}

void factory_reset_deinit(void)
{
    if (!s_reset.initialized) {
        return;
    }
    (void)hal_gpio_deinit(s_reset.config.pin);
    memset(&s_reset, 0, sizeof(s_reset));
}

bool factory_reset_is_triggered(void)
{
    return s_reset.triggered;
}
