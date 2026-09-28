/**
 * @file ota_manager.c
 * @brief ThingsBoard OTA supervisor (see ota_manager.h).
 */

#include "ota_manager.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "osal_log.h"
#include "osal_task.h"
#include "tb_firmware_update.h"

typedef struct ota_manager_module {
    bool initialized;
    ota_manager_config_t cfg;
    char title[OTA_MANAGER_STR_MAX_LEN + 1u];
    char version[OTA_MANAGER_STR_MAX_LEN + 1u];

    tb_client_t *client;
    bool fw_ready;
    bool init_pending;
    uint32_t init_retry_at_ms;

    bool active;
    bool indicator_on;
    uint32_t next_toggle_ms;
    unsigned last_decile;

    bool reboot_armed;
    uint32_t reboot_at_ms;

    uint32_t last_check_ms;
    tb_firmware_update_state_t last_state;
} ota_manager_module_t;

static ota_manager_module_t s_ota;
static atomic_bool s_check_requested;
static atomic_bool s_confirm_requested;
static uint32_t s_confirm_retry_at_ms;
static atomic_bool s_disconnected;
static atomic_bool s_reboot_requested;

static uint32_t ota_now(void)
{
    return (s_ota.cfg.now_ms != NULL) ? s_ota.cfg.now_ms()
                                      : osal_task_get_time_ms();
}

static bool ota_time_reached(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

static void ota_copy(char *dst, const char *src)
{
    strncpy(dst, src, OTA_MANAGER_STR_MAX_LEN);
    dst[OTA_MANAGER_STR_MAX_LEN] = '\0';
}

static void ota_emit(ota_manager_event_t event)
{
    if (s_ota.cfg.on_event != NULL)
    {
        s_ota.cfg.on_event(event, s_ota.cfg.ctx);
    }
}

static void ota_set_indicator(bool on)
{
    s_ota.indicator_on = on;
    if (s_ota.cfg.on_indicator != NULL)
    {
        s_ota.cfg.on_indicator(on, s_ota.cfg.ctx);
    }
}

/* Transport thread: only flags the supervisor. */
static void ota_on_reboot_required(const char *new_title,
                                   const char *new_version, void *user_data)
{
    (void)new_title;
    (void)new_version;
    (void)user_data;
    atomic_store(&s_reboot_requested, true);
}

static void ota_try_init(uint32_t now)
{
    tb_firmware_update_status_t status;
    const tb_firmware_update_config_t fw_cfg = {
        .current_title = s_ota.title,
        .current_version = s_ota.version,
        .chunk_size = s_ota.cfg.chunk_size,
        .chunk_timeout_ms = s_ota.cfg.chunk_timeout_ms,
        .max_chunk_retries = s_ota.cfg.chunk_retries,
        .on_reboot_required = ota_on_reboot_required,
    };

    if (tb_firmware_update_init(s_ota.client, &fw_cfg) != 0)
    {
        s_ota.init_pending = true;
        s_ota.init_retry_at_ms = now + OTA_MANAGER_INIT_RETRY_MS;
        osal_log_warning("[ota] firmware updater init failed; retry in %u ms",
                         (unsigned)OTA_MANAGER_INIT_RETRY_MS);
        return;
    }

    s_ota.fw_ready = true;
    s_ota.init_pending = false;
    memset(&status, 0, sizeof(status));
    (void)tb_firmware_update_get_status(&status);
    s_ota.last_state = status.state;
    osal_log_info("[ota] firmware updater ready: title=%s version=%s "
                  "last_state=%s%s%s",
                  s_ota.title, s_ota.version,
                  tb_firmware_update_state_name(s_ota.last_state),
                  (status.last_error[0] != '\0') ? " error=" : "",
                  status.last_error);
}

static void ota_teardown_updater(void)
{
    if (s_ota.fw_ready)
    {
        tb_firmware_update_deinit(s_ota.client);
        s_ota.fw_ready = false;
    }
}

static void ota_finish_failed(const char *reason)
{
    s_ota.active = false;
    if (s_ota.indicator_on)
    {
        ota_set_indicator(false);
    }
    osal_log_error("[ota] FAILED: %s; returning to normal operation",
                   (reason != NULL && reason[0] != '\0') ? reason
                                                          : "unknown error");
    ota_emit(OTA_MANAGER_EVENT_FAILED);
}

static void ota_start_active(uint32_t now)
{
    s_ota.active = true;
    s_ota.last_decile = 0u;
    ota_emit(OTA_MANAGER_EVENT_STARTED);
    ota_set_indicator(true);
    s_ota.next_toggle_ms = now + s_ota.cfg.blink_period_ms;
}

static void ota_blink(uint32_t now)
{
    if (ota_time_reached(now, s_ota.next_toggle_ms))
    {
        ota_set_indicator(!s_ota.indicator_on);
        s_ota.next_toggle_ms = now + s_ota.cfg.blink_period_ms;
    }
}

static void ota_track_status(uint32_t now)
{
    tb_firmware_update_status_t status;

    if (tb_firmware_update_get_status(&status) != 0)
    {
        return;
    }

    if (status.state != s_ota.last_state)
    {
        osal_log_info("[ota] state %s -> %s%s%s",
                      tb_firmware_update_state_name(s_ota.last_state),
                      tb_firmware_update_state_name(status.state),
                      (status.last_error[0] != '\0') ? " error=" : "",
                      status.last_error);
    }

    if (!s_ota.active &&
        (status.state == TB_FIRMWARE_UPDATE_STATE_DOWNLOADING))
    {
        osal_log_info("[ota] DOWNLOADING title=%s version=%s size=%u",
                      status.target_title, status.target_version,
                      (unsigned)status.total_size);
        ota_start_active(now);
    }

    if (s_ota.active)
    {
        if (status.total_size > 0u)
        {
            const unsigned decile = (unsigned)(
                ((uint64_t)status.downloaded_size * 10u) / status.total_size);
            if (decile > s_ota.last_decile)
            {
                s_ota.last_decile = decile;
                osal_log_info("[ota] progress %u%% (%u/%u bytes)",
                              decile * 10u,
                              (unsigned)status.downloaded_size,
                              (unsigned)status.total_size);
            }
        }

        if (status.state == TB_FIRMWARE_UPDATE_STATE_FAILED)
        {
            ota_finish_failed(status.last_error);
        }
        else
        {
            ota_blink(now);
        }
    }

    s_ota.last_state = status.state;
}

static void ota_run_checks(uint32_t now)
{
    const char *reason = NULL;

    if (atomic_exchange(&s_check_requested, false))
    {
        reason = "requested";
    }
    else if ((s_ota.cfg.check_period_ms != 0u) &&
             ((uint32_t)(now - s_ota.last_check_ms) >=
              s_ota.cfg.check_period_ms))
    {
        reason = "periodic";
    }
    if (reason == NULL)
    {
        return;
    }

    s_ota.last_check_ms = now;
    if (tb_firmware_update_request_check(s_ota.client) == 0)
    {
        osal_log_info("[ota] firmware check requested (%s)", reason);
    }
    else
    {
        osal_log_warning("[ota] firmware check request failed (%s)", reason);
    }
}

ota_manager_status_t ota_manager_init(const ota_manager_config_t *config)
{
    if ((config == NULL) || (config->title == NULL) ||
        (config->version == NULL) || (config->restart == NULL))
    {
        return OTA_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (s_ota.initialized)
    {
        return OTA_MANAGER_ERR_ALREADY_INITIALIZED;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.cfg = *config;
    ota_copy(s_ota.title, config->title);
    ota_copy(s_ota.version, config->version);
    s_ota.cfg.title = s_ota.title;
    s_ota.cfg.version = s_ota.version;
    if (s_ota.cfg.chunk_size == 0u)
    {
        s_ota.cfg.chunk_size = OTA_MANAGER_CHUNK_SIZE_DEFAULT;
    }
    if (s_ota.cfg.reboot_delay_ms == 0u)
    {
        s_ota.cfg.reboot_delay_ms = OTA_MANAGER_REBOOT_DELAY_DEFAULT_MS;
    }
    if (s_ota.cfg.blink_period_ms == 0u)
    {
        s_ota.cfg.blink_period_ms = OTA_MANAGER_BLINK_PERIOD_DEFAULT_MS;
    }
    atomic_store(&s_check_requested, false);
    atomic_store(&s_confirm_requested, false);
    s_confirm_retry_at_ms = 0u;
    atomic_store(&s_disconnected, false);
    atomic_store(&s_reboot_requested, false);
    s_ota.last_check_ms = ota_now();
    s_ota.initialized = true;
    return OTA_MANAGER_OK;
}

void ota_manager_deinit(void)
{
    if (!s_ota.initialized)
    {
        return;
    }
    ota_teardown_updater();
    memset(&s_ota, 0, sizeof(s_ota));
}

void ota_manager_on_connected(tb_client_t *client)
{
    if (!s_ota.initialized || (client == NULL) || s_ota.reboot_armed)
    {
        return;
    }

    atomic_store(&s_disconnected, false);
    ota_teardown_updater();
    if (s_ota.active)
    {
        ota_finish_failed("transport reconnected during download");
    }
    s_ota.client = client;
    ota_try_init(ota_now());
}

void ota_manager_on_disconnected(void)
{
    atomic_store(&s_disconnected, true);
}

void ota_manager_request_check(void)
{
    atomic_store(&s_check_requested, true);
}

void ota_manager_request_image_confirmation(void)
{
    atomic_store(&s_confirm_requested, true);
}

static void ota_confirm_image(uint32_t now)
{
    if (!atomic_load(&s_confirm_requested) ||
        !ota_time_reached(now, s_confirm_retry_at_ms))
    {
        return;
    }
    if (tb_firmware_update_confirm_health(s_ota.client) == 0)
    {
        atomic_store(&s_confirm_requested, false);
        return;
    }
    s_confirm_retry_at_ms = now + OTA_MANAGER_CONFIRM_RETRY_MS;
    osal_log_warning("[ota] running image confirmation failed; retry in %u ms",
                     (unsigned)OTA_MANAGER_CONFIRM_RETRY_MS);
}

void ota_manager_poll(void)
{
    uint32_t now;

    if (!s_ota.initialized)
    {
        return;
    }
    now = ota_now();

    if (atomic_exchange(&s_disconnected, false))
    {
        ota_teardown_updater();
        s_ota.init_pending = false;
        s_ota.client = NULL;
        if (s_ota.active && !s_ota.reboot_armed)
        {
            ota_finish_failed("transport lost during download");
        }
    }

    if (!s_ota.reboot_armed && atomic_exchange(&s_reboot_requested, false))
    {
        if (!s_ota.active)
        {
            ota_start_active(now);
        }
        s_ota.reboot_armed = true;
        s_ota.reboot_at_ms = now + s_ota.cfg.reboot_delay_ms;
        osal_log_info("[ota] image activated; restarting in %u ms",
                      (unsigned)s_ota.cfg.reboot_delay_ms);
    }

    if (s_ota.reboot_armed)
    {
        if (ota_time_reached(now, s_ota.reboot_at_ms))
        {
            osal_log_info("[ota] restarting into the new firmware");
            ota_set_indicator(false);
            ota_emit(OTA_MANAGER_EVENT_RESTARTING);
            s_ota.cfg.restart();
            return;
        }
        ota_blink(now);
        return;
    }

    if (!s_ota.fw_ready)
    {
        if (s_ota.init_pending && (s_ota.client != NULL) &&
            ota_time_reached(now, s_ota.init_retry_at_ms))
        {
            ota_try_init(now);
        }
        return;
    }

    ota_confirm_image(now);
    tb_firmware_update_poll(s_ota.client, now);
    ota_track_status(now);

    if (!s_ota.active)
    {
        ota_run_checks(now);
    }
}

bool ota_manager_is_active(void)
{
    return s_ota.initialized && (s_ota.active || s_ota.reboot_armed);
}
