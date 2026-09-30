#include "device_identity.h"

device_identity_status_t device_identity_token(const char **out_token)
{
    static const char token[] = "mqtt-cfg-test-token";

    if (out_token == NULL)
    {
        return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
    }

    *out_token = token;
    return DEVICE_IDENTITY_OK;
}
