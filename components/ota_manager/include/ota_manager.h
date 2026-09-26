/**
 * @file ota_manager.h
 * @brief ThingsBoard OTA supervisor for the Kitchen LED Controller.
 *
 * Owns the lifecycle of the platform firmware updater (tb_firmware_update)
 * on behalf of the application supervisor task:
 *
 *   - (re)initializes the updater on every ThingsBoard connection and tears
 *     it down (aborting a running download) on disconnect,
 *   - requests a firmware metadata check on demand (after synchronization,
 *     on a `fw_*` shared-attribute push) and periodically,
 *   - drives the platform chunk timeout policy,
 *   - reports STARTED / FAILED / RESTARTING to the integrator, drives the
 *     progress blink through the indicator callback and restarts the device
 *     after a bounded delay once the verified image is activated.
 *
 * Threading: ota_manager_on_connected() and ota_manager_poll() run on the
 * supervisor task only; ota_manager_on_disconnected() and
 * ota_manager_request_check() may be called from any context.  Every
 * callback in the configuration runs on the supervisor task.
 *
 * Log markers (stable, parsed by the on-target checks): lines starting with
 * "[ota] " — "firmware updater ready", "firmware check requested",
 * "DOWNLOADING", "progress", "state", "image activated", "restarting",
 * "FAILED".
 */

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_client tb_client_t;

#define OTA_MANAGER_CHUNK_SIZE_DEFAULT       4096u
#define OTA_MANAGER_REBOOT_DELAY_DEFAULT_MS  2000u
#define OTA_MANAGER_BLINK_PERIOD_DEFAULT_MS  1000u
#define OTA_MANAGER_INIT_RETRY_MS            10000u
#define OTA_MANAGER_STR_MAX_LEN              127u

typedef enum ota_manager_status {
    OTA_MANAGER_OK                   = 0,
    OTA_MANAGER_ERR_INVALID_ARGUMENT = -1,
    OTA_MANAGER_ERR_ALREADY_INITIALIZED = -2
} ota_manager_status_t;

typedef enum ota_manager_event {
    OTA_MANAGER_EVENT_STARTED = 0, /**< Download began: output owned by the indicator. */
    OTA_MANAGER_EVENT_FAILED,      /**< Update failed/aborted: restore normal operation. */
    OTA_MANAGER_EVENT_RESTARTING   /**< Image activated: last call before restart(). */
} ota_manager_event_t;

typedef uint32_t (*ota_manager_now_fn_t)(void);
typedef void (*ota_manager_event_fn_t)(ota_manager_event_t event, void *ctx);
typedef void (*ota_manager_indicator_fn_t)(bool on, void *ctx);
typedef void (*ota_manager_restart_fn_t)(void);

typedef struct ota_manager_config {
    const char *title;               /**< Running firmware title (required). */
    const char *version;             /**< Running firmware version (required). */
    uint32_t chunk_size;             /**< Requested chunk size; 0 = default. */
    uint32_t chunk_timeout_ms;       /**< Chunk response timeout; 0 = platform default. */
    uint32_t chunk_retries;          /**< Chunk re-requests; 0 = platform default. */
    uint32_t check_period_ms;        /**< Periodic check; 0 = disabled. */
    uint32_t reboot_delay_ms;        /**< Delay before restart; 0 = default. */
    uint32_t blink_period_ms;        /**< Indicator half period; 0 = default. */
    ota_manager_now_fn_t now_ms;     /**< Clock; NULL = OSAL monotonic clock. */
    ota_manager_event_fn_t on_event; /**< May be NULL. */
    ota_manager_indicator_fn_t on_indicator; /**< May be NULL. */
    ota_manager_restart_fn_t restart; /**< Required. */
    void *ctx;                       /**< Passed to on_event/on_indicator. */
} ota_manager_config_t;

ota_manager_status_t ota_manager_init(const ota_manager_config_t *config);
void ota_manager_deinit(void);

void ota_manager_on_connected(tb_client_t *client);
void ota_manager_on_disconnected(void);
void ota_manager_request_check(void);
void ota_manager_poll(void);

/** @brief True while a download runs or a restart is pending. */
bool ota_manager_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MANAGER_H */
