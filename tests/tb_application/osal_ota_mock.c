/**
 * @file osal_ota_mock.c
 * @brief Test-only OTA surface for tb_application host tests (TASK-112)
 *
 * The REAL platform ThingsBoard sources compiled into this harness include
 * tb_firmware_update.c, which references the OSAL OTA API.  The POSIX OTA
 * implementation (osal_ota_impl.c) depends on Mongoose's mg_sha256_*; the
 * synchronizer tests do not exercise firmware updates, so this double
 * provides the OTA symbols with deterministic not-implemented results and
 * keeps the harness free of the Mongoose amalgamation.  Compiling the real
 * OTA implementation adds nothing to the TASK-112 coverage.
 */

#include <stddef.h>
#include <stdint.h>

#include "osal_ota.h"
#include "osal_ota_state.h"

osal_status_t osal_ota_init(void)
{
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_get_security_info(osal_ota_security_info_t *info)
{
    (void)info;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_begin(const osal_ota_descriptor_t *descriptor)
{
    (void)descriptor;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_write(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_verify(void)
{
    return OSAL_ERR_NOT_IMPLEMENTED;
}

bool osal_ota_needs_confirmation(void)
{
    return false;
}

osal_status_t osal_ota_confirm_running_image(void)
{
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_finish(bool apply_update)
{
    (void)apply_update;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_abort(void)
{
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_get_progress(size_t *written_size, size_t *total_size)
{
    (void)written_size;
    (void)total_size;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_state_save(const osal_ota_state_t *state)
{
    (void)state;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_state_load(osal_ota_state_t *state)
{
    (void)state;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_ota_state_clear(void)
{
    return OSAL_ERR_NOT_IMPLEMENTED;
}