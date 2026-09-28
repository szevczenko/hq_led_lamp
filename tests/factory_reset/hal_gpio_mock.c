#include <stdbool.h>
#include <stddef.h>
#include "hal_gpio.h"

static bool s_initialized;
static bool s_active;

void hal_gpio_mock_set_active(bool active) { s_active = active; }

hal_status_t hal_gpio_init(const hal_gpio_config_t *config)
{
    if (config == NULL || config->pin == HAL_PIN_NONE) return HAL_ERR_INVALID_ARGUMENT;
    s_initialized = true;
    return HAL_OK;
}

hal_status_t hal_gpio_read(hal_pin_t pin, bool *active)
{
    if (!s_initialized || pin == HAL_PIN_NONE || active == NULL) return HAL_ERR_NOT_INITIALIZED;
    *active = s_active;
    return HAL_OK;
}

hal_status_t hal_gpio_deinit(hal_pin_t pin)
{
    if (!s_initialized || pin == HAL_PIN_NONE) return HAL_ERR_NOT_INITIALIZED;
    s_initialized = false;
    return HAL_OK;
}
