#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_types.h"

typedef enum factory_reset_status {
    FACTORY_RESET_OK = 0,
    FACTORY_RESET_ERR_INVALID_ARGUMENT = -1,
    FACTORY_RESET_ERR_ALREADY_INITIALIZED = -2,
    FACTORY_RESET_ERR_NOT_INITIALIZED = -3,
    FACTORY_RESET_ERR_GPIO = -4,
    FACTORY_RESET_ERR_ERASE = -5
} factory_reset_status_t;

typedef bool (*factory_reset_erase_fn_t)(void *context);
typedef void (*factory_reset_indicator_fn_t)(bool active, void *context);
typedef void (*factory_reset_restart_fn_t)(void *context);

typedef struct factory_reset_config {
    hal_pin_t pin;
    bool active_low;
    uint32_t hold_ms;
    factory_reset_erase_fn_t erase;
    factory_reset_indicator_fn_t indicator;
    factory_reset_restart_fn_t restart;
    void *context;
} factory_reset_config_t;

factory_reset_status_t factory_reset_init(const factory_reset_config_t *config);
void factory_reset_poll(uint32_t now_ms);
void factory_reset_deinit(void);
bool factory_reset_is_triggered(void);

#endif
