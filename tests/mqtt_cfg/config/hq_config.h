/* Minimal platform configuration for mqtt_cfg host tests (TASK-110).
 *
 * The platform normally generates hq_config.h from its defconfig; host
 * test projects that compile mqtt_config.c / osal sources provide a small
 * hand-written equivalent instead.  Only the macros needed by the
 * compiled sources are defined here. */

#ifndef HQ_CONFIG_H
#define HQ_CONFIG_H

#define CONFIG_HQ_PLATFORM_POSIX 1

/* osal_log.h gate (0 = debug level, everything prints via vprintf). */
#define CONFIG_OSAL_LOG_LEVEL 0

/* mqtt_config.c defaults (overridden by mqtt_cfg_apply in tests). */
#define CONFIG_MQTT_DEFAULT_ADDRESS "mqtt://192.168.1.169:1883"
#define CONFIG_MQTT_DEFAULT_TOPIC_PREFIX "/config/"
#define CONFIG_MQTT_DEFAULT_POST_TOPIC "/post_data/"
#define CONFIG_MQTT_DEFAULT_USERNAME ""
#define CONFIG_MQTT_DEFAULT_PASSWORD ""
#define CONFIG_MQTT_DEFAULT_CLIENT_ID "hq_"

/* mongoose_process.c log gate (not compiled by mqtt_cfg_tests). */
#define CONFIG_MONGOOSE_LOG_LEVEL 2

#endif /* HQ_CONFIG_H */