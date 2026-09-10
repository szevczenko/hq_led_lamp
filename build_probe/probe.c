#include <stdio.h>
#include "app_config.h"
#include "osal_file_mock.h"

int main(void)
{
    osal_file_mock_reset();
    const char *doc =
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}";
    int rc1 = app_config_validate_device_json(doc, NULL);
    char with_garbage[512];
    snprintf(with_garbage, sizeof(with_garbage), "%s \"trailing\"", doc);
    int rc2 = app_config_validate_device_json(with_garbage, NULL);
    char concat[512];
    snprintf(concat, sizeof(concat), "%s%s", doc, doc);
    int rc3 = app_config_validate_device_json(concat, NULL);
    printf("valid=%d trailing_garbage=%d concat=%d\n", rc1, rc2, rc3);
    return 0;
}
