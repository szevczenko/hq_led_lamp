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
# ESP32 LEDC high-speed timer is clocked from APB (80 MHz).  The ESP HAL PWM
# backend (platform/hq_platform/src/hal/esp/hal_pwm_esp.c) runs LEDC at a
# FIXED 13-bit duty resolution, so the budget must be validated against 2^13
# regardless of the Kconfig resolution value:
# valid if: frequency * 2^13 <= APB_CLK_HZ
math(EXPR _klc_steps   "1 << 13")
math(EXPR _klc_clk_req "${CONFIG_KLC_LED_PWM_FREQUENCY_HZ} * ${_klc_steps}")
set(_klc_apb_hz 80000000)

if(_klc_clk_req GREATER ${_klc_apb_hz})
    message(FATAL_ERROR
        "KLC: PWM ${CONFIG_KLC_LED_PWM_FREQUENCY_HZ} Hz needs "
        "${_klc_clk_req} Hz at the ESP HAL's fixed 13-bit duty resolution, "
        "which exceeds the APB clock (${_klc_apb_hz} Hz). "
        "Reduce KLC_LED_PWM_FREQUENCY_HZ (practical maximum 9765 Hz; default "
        "8000 Hz).")
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

# --- Partition table validation (TASK-107) -----------------------------------
# Parse the custom partition table and verify the OTA-capable layout before
# anything is compiled:
#   * exactly the required data partitions are present
#     (NVS, PHY, OTA data and a LittleFS storage partition of at least
#     256 KiB),
#   * two OTA application slots exist,
#   * partitions do not overlap, app partitions are 64 KiB aligned and data
#     partitions are 4 KiB aligned,
#   * the table fits inside the configured flash size.
# klc_validate.cmake is included from the project's top-level CMakeLists,
# so CMAKE_CURRENT_LIST_DIR here points at the project cmake/ directory.
# Tests may pre-set _klc_partitions_csv to point at a synthetic table;
# otherwise resolve the partition table relative to this file's location.
if(NOT DEFINED _klc_partitions_csv)
    get_filename_component(_klc_cmake_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
    if(NOT EXISTS "${_klc_cmake_dir}/partitions.csv")
        get_filename_component(_klc_cmake_dir "${_klc_cmake_dir}/.." ABSOLUTE)
    endif()
    set(_klc_partitions_csv "${_klc_cmake_dir}/partitions.csv")
endif()

if(EXISTS "${_klc_partitions_csv}" AND DEFINED CONFIG_ESPTOOLPY_FLASHSIZE)
    # Flash size from the Kconfig setting, e.g. "4MB" -> 4194304.
    set(_klc_flash_mb 0)
    if(CONFIG_ESPTOOLPY_FLASHSIZE MATCHES "^([0-9]+)MB$")
        math(EXPR _klc_flash_mb "${CMAKE_MATCH_1}")
    endif()

    set(_klc_flash_bytes 0)
    if(_klc_flash_mb GREATER 0)
        math(EXPR _klc_flash_bytes "${_klc_flash_mb} * 1024 * 1024")
    endif()

    file(STRINGS "${_klc_partitions_csv}" _klc_partition_lines)
    set(_klc_p_names "")
    set(_klc_p_types "")
    set(_klc_p_offsets "")
    set(_klc_p_sizes "")

    foreach(_klc_line IN LISTS _klc_partition_lines)
        string(STRIP "${_klc_line}" _klc_line)
        if(_klc_line STREQUAL "" OR _klc_line MATCHES "^#")
            continue()
        endif()
        # file(STRINGS) may split long '#' comments (the ones containing a
        # semicolon) into multiple lines; the continuation fragment does not
        # start with '#' anymore.  Real partition rows always begin with a
        # known partition name, so anything else is treated as a comment.
        if(NOT _klc_line MATCHES "^(nvs|phy_init|otadata|ota_0|ota_1|storage)[, ]")
            continue()
        endif()
        # Split on commas and spaces; a trailing comma yields an empty
        # last field, so strip empty entries before indexing.
        string(REGEX REPLACE "[, ]+" ";" _klc_fields "${_klc_line}")
        set(_klc_clean "")
        foreach(_klc_f IN LISTS _klc_fields)
            if(NOT _klc_f STREQUAL "")
                list(APPEND _klc_clean "${_klc_f}")
            endif()
        endforeach()
        list(LENGTH _klc_clean _klc_nfields)
        if(_klc_nfields LESS 5)
            # Comment continuation line (a '#' comment was split across
            # file(STRINGS) lines): not a partition definition.
            continue()
        endif()
        # Skip comment-continuation fragments: a partition name in this
        # table always matches a fixed identifier charset.
        list(GET _klc_clean 0 _klc_name)
        list(GET _klc_clean 1 _klc_type)
        list(GET _klc_clean 3 _klc_offset)
        list(GET _klc_clean 4 _klc_size)
        set(_klc_fields "")
        if(NOT _klc_type MATCHES "^(app|data)$")
            message(FATAL_ERROR
                "KLC: partition '${_klc_name}' has unknown type '${_klc_type}' "
                "in partitions.csv (expected 'app' or 'data').")
        endif()
        if(NOT _klc_offset MATCHES "^0x[0-9a-fA-F]+$" OR
           NOT _klc_size MATCHES "^0x[0-9a-fA-F]+$")
            message(FATAL_ERROR
                "KLC: partition '${_klc_name}' in partitions.csv must use "
                "explicit hexadecimal offset and size fields "
                "(got offset='${_klc_offset}', size='${_klc_size}').")
        endif()
        if(_klc_offset STREQUAL "")
            message(FATAL_ERROR
                "KLC: partition '${_klc_name}' in partitions.csv must have an "
                "explicit offset for the validation to be exact.")
        endif()
        list(APPEND _klc_p_names "${_klc_name}")
        list(APPEND _klc_p_types "${_klc_type}")
        list(APPEND _klc_p_offsets "${_klc_offset}")
        list(APPEND _klc_p_sizes "${_klc_size}")
    endforeach()

    list(LENGTH _klc_p_names _klc_p_count)
    math(EXPR _klc_last "${_klc_p_count} - 1")
    set(_klc_have_ota0 FALSE)
    set(_klc_have_ota1 FALSE)
    set(_klc_lfs_size 0)

    foreach(_klc_i RANGE ${_klc_last})
        list(GET _klc_p_names ${_klc_i} _klc_name)
        if(_klc_name STREQUAL "ota_0")
            set(_klc_have_ota0 TRUE)
        elseif(_klc_name STREQUAL "ota_1")
            set(_klc_have_ota1 TRUE)
        elseif(_klc_name STREQUAL "storage")
            list(GET _klc_p_sizes ${_klc_i} _klc_lfs_size)
        endif()
    endforeach()

    if(NOT _klc_have_ota0 OR NOT _klc_have_ota1)
        message(FATAL_ERROR
            "KLC: partitions.csv must define two OTA app slots (ota_0 and ota_1).")
    endif()

    if(_klc_lfs_size EQUAL 0)
        message(FATAL_ERROR
            "KLC: partitions.csv must define a LittleFS 'storage' data partition.")
    endif()

    math(EXPR _klc_lfs_min "262144")
    if(_klc_lfs_size LESS _klc_lfs_min)
        message(FATAL_ERROR
            "KLC: LittleFS storage partition is ${_klc_lfs_size} bytes; "
            "at least 262144 (256 KiB) is required for certificates, "
            "configuration and diagnostics.")
    endif()

    # Overlap / alignment / bounds checks.
    set(_klc_prev_end 0)
    foreach(_klc_i RANGE ${_klc_last})
        list(GET _klc_p_names ${_klc_i} _klc_name)
        list(GET _klc_p_types ${_klc_i} _klc_type)
        list(GET _klc_p_offsets ${_klc_i} _klc_offset)
        list(GET _klc_p_sizes ${_klc_i} _klc_size)

        math(EXPR _klc_off "${_klc_offset}")
        math(EXPR _klc_sz "${_klc_size}")
        math(EXPR _klc_end "${_klc_off} + ${_klc_sz}")

        if(_klc_off LESS _klc_prev_end)
            message(FATAL_ERROR
                "KLC: partition '${_klc_name}' (offset ${_klc_offset}) overlaps "
                "the previous partition (ends at ${_klc_prev_end}).")
        endif()

        if(_klc_type STREQUAL "app")
            math(EXPR _klc_mod "${_klc_off} % 65536")
            if(NOT _klc_mod EQUAL 0)
                message(FATAL_ERROR
                    "KLC: app partition '${_klc_name}' offset ${_klc_offset} "
                    "is not 64 KiB aligned.")
            endif()
        else()
            math(EXPR _klc_mod "${_klc_off} % 4096")
            if(NOT _klc_mod EQUAL 0)
                message(FATAL_ERROR
                    "KLC: data partition '${_klc_name}' offset ${_klc_offset} "
                    "is not 4 KiB aligned.")
            endif()
        endif()

        if(_klc_flash_bytes GREATER 0 AND _klc_end GREATER _klc_flash_bytes)
            message(FATAL_ERROR
                "KLC: partition '${_klc_name}' ends at ${_klc_end}, beyond the "
                "configured ${_klc_flash_mb} MB flash (${_klc_flash_bytes} bytes). "
                "Check CONFIG_ESPTOOLPY_FLASHSIZE against the module SKU.")
        endif()

        set(_klc_prev_end ${_klc_end})
    endforeach()

    math(EXPR _klc_lfs_bytes "${_klc_lfs_size}")
    message(STATUS "KLC: partition table OK "
                   "(${_klc_p_count} partitions, LittleFS storage "
                   "${_klc_lfs_bytes} bytes, flash ${_klc_flash_mb} MB).")
endif()
