#include "device_identity.h"

device_identity_status_t device_identity_token(const char **out_token)
{
    if (out_token == NULL) return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
    *out_token = "test-token";
    return DEVICE_IDENTITY_OK;
}
