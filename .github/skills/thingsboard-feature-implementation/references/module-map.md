# Module Map

Use this map to route feature work quickly to the nearest owning code.

- `src/thingsboard/tb_client.c`: core client lifecycle, MQTT session behavior, shared module coordination.
- `src/thingsboard/tb_firmware_update.c`: firmware metadata, chunk handling, OTA state flow, install decision path.
- `src/thingsboard/tb_attributes.c`: shared and client attribute requests, subscriptions, and update dispatch.
- `src/thingsboard/tb_rpc.c`: server RPC handling and client RPC request flow.
- `src/thingsboard/tb_telemetry.c`: telemetry publish behavior and payload shaping.
- `src/thingsboard/tb_claim.c`: claim request behavior.
- `src/thingsboard/tb_provision.c`: provisioning request and credential flow.
- `tests/thingsboard/tb_tests.c`: current unit-test entry point.
- `tests/thingsboard/mqtt_app_mock.c`: MQTT adapter mock behavior for ThingsBoard tests.
- `examples/common/thingboard_firmware_update_demo.c`: current OTA demo behavior.
- `examples/common/thingboard_rgb_lamp.c`: existing ThingsBoard example with runtime interaction.

When the owning logic only wires modules together, step one layer deeper to the code that computes, mutates, or decides the behavior.