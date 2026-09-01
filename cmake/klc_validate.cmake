# KLC configure-time validation — catches invalid PWM and GPIO settings before
# any source file is compiled.

if(NOT DEFINED CONFIG_KLC_LED_PWM_FREQUENCY_HZ OR
   NOT DEFINED CONFIG_KLC_LED_PWM_RESOLUTION_BITS)
    return()
endif()

# --- Board profile vs IDF target check --------------------------------------
# Ensure the selected board profile matches the configured ESP-IDF target.
if(DEFINED IDF_TARGET)
    if(CONFIG_KLC_BOARD_ESP32_WROOM_32D AND NOT IDF_TARGET STREQUAL "esp32")
        message(FATAL_ERROR "KLC: Board ESP32-WROOM-32D requires IDF_TARGET=esp32 (got '${IDF_TARGET}').")
    elseif(CONFIG_KLC_BOARD_ESP32_S3 AND NOT IDF_TARGET STREQUAL "esp32s3")
        message(FATAL_ERROR "KLC: Board ESP32-S3 requires IDF_TARGET=esp32s3 (got '${IDF_TARGET}').")
    elseif(CONFIG_KLC_BOARD_ESP32_C6 AND NOT IDF_TARGET STREQUAL "esp32c6")
        message(FATAL_ERROR "KLC: Board ESP32-C6 requires IDF_TARGET=esp32c6 (got '${IDF_TARGET}').")
    endif()
endif()

# --- PWM clock-budget check -------------------------------------------------
# ESP32 LEDC high-speed timer is clocked from APB (80 MHz).
# Valid if: frequency * 2^resolution <= APB_CLK_HZ
math(EXPR _klc_steps   "1 << ${CONFIG_KLC_LED_PWM_RESOLUTION_BITS}")
math(EXPR _klc_clk_req "${CONFIG_KLC_LED_PWM_FREQUENCY_HZ} * ${_klc_steps}")
set(_klc_apb_hz 80000000)

if(_klc_clk_req GREATER ${_klc_apb_hz})
    message(FATAL_ERROR
        "KLC: PWM ${CONFIG_KLC_LED_PWM_FREQUENCY_HZ} Hz x "
        "${CONFIG_KLC_LED_PWM_RESOLUTION_BITS}-bit resolution needs "
        "${_klc_clk_req} Hz, which exceeds the APB clock (${_klc_apb_hz} Hz). "
        "Reduce KLC_LED_PWM_FREQUENCY_HZ or KLC_LED_PWM_RESOLUTION_BITS. "
        "Example: 20000 Hz x 11 bits = 40960000 Hz (valid).")
endif()

# --- Input-only GPIO check (ESP32-WROOM-32D) --------------------------------
# GPIOs 34-39 have no output driver on ESP32.
if(CONFIG_KLC_BOARD_ESP32_WROOM_32D AND
   DEFINED CONFIG_KLC_LED_PWM_GPIO)
    if(CONFIG_KLC_LED_PWM_GPIO GREATER 39)
        message(FATAL_ERROR
            "KLC: GPIO ${CONFIG_KLC_LED_PWM_GPIO} is not a valid ESP32 GPIO (valid: 0-39; GPIOs 34-39 are input-only).")
    elseif(CONFIG_KLC_LED_PWM_GPIO GREATER_EQUAL 34)
        message(FATAL_ERROR
            "KLC: GPIO ${CONFIG_KLC_LED_PWM_GPIO} is input-only on ESP32-WROOM-32D "
            "(GPIOs 34-39 have no output driver). "
            "Set KLC_LED_PWM_GPIO to an output-capable GPIO (0-33).")
    endif()
endif()
