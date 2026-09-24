# Bootstrap ThingsBoard enrollment and one-shot credential commit.
idf_component_register(
    SRCS "tb_provisioning.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES osal json thingsboard
)
