#include "factory_reset.h"

#include <string.h>

#include "hq_factory_reset.h"

#define FACTORY_RESET_DEBOUNCE_MS 50u

static hq_factory_reset_t s_reset;

factory_reset_status_t factory_reset_init(const factory_reset_config_t *config)
{
    hq_factory_reset_config_t platform_config;
    hq_factory_reset_status_t status;

    if (config == NULL || config->pin == HAL_PIN_NONE || config->hold_ms == 0u ||
        config->erase == NULL || config->restart == NULL) {
        return FACTORY_RESET_ERR_INVALID_ARGUMENT;
    }
    if (s_reset.initialized) {
        return FACTORY_RESET_ERR_ALREADY_INITIALIZED;
    }

    memset(&platform_config, 0, sizeof(platform_config));
    platform_config.pin = config->pin;
    platform_config.polarity = config->active_low ? HAL_POLARITY_ACTIVE_LOW
                                                  : HAL_POLARITY_ACTIVE_HIGH;
    platform_config.hold_ms = config->hold_ms;
    platform_config.debounce_ms = FACTORY_RESET_DEBOUNCE_MS;
    platform_config.erase = config->erase;
    platform_config.indicator = config->indicator;
    platform_config.restart = config->restart;
    platform_config.context = config->context;

    status = hq_factory_reset_init(&s_reset, &platform_config);
    if (status == HQ_FACTORY_RESET_ERR_GPIO) {
        return FACTORY_RESET_ERR_GPIO;
    }
    if (status != HQ_FACTORY_RESET_OK) {
        return FACTORY_RESET_ERR_INVALID_ARGUMENT;
    }
    return FACTORY_RESET_OK;
}

void factory_reset_poll(uint32_t now_ms)
{
    hq_factory_reset_poll(&s_reset, now_ms);
}

void factory_reset_deinit(void)
{
    hq_factory_reset_deinit(&s_reset);
}

bool factory_reset_is_triggered(void)
{
    return hq_factory_reset_is_triggered(&s_reset);
}
