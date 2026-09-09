# lamp_fs — LittleFS filesystem bootstrap (TASK-107)
#
# ESP-IDF component registration.  The component mounts the LittleFS
# "storage" partition at /littlefs through the portable OSAL filesystem
# contract and creates the /cert, /config and /state directory layout
# idempotently.  It never formats storage on the boot path.

idf_component_register(
    SRCS "lamp_fs.c"
    INCLUDE_DIRS "include"
    REQUIRES osal
)
