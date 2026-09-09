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

    # Read the file whole rather than with file(STRINGS): file(STRINGS)
    # splits lines at embedded semicolons, turning long '#' comments into
    # unmarked continuation fragments that could be mistaken for partition
    # rows.  Escaping semicolons first keeps every comment intact as a
    # single '#' line, so the parser can treat every non-comment row as a
    # real partition definition and validate it.
    file(READ "${_klc_partitions_csv}" _klc_csv_content)
    string(REPLACE ";" "\\;" _klc_csv_content "${_klc_csv_content}")
    string(REPLACE "\r" "" _klc_csv_content "${_klc_csv_content}")
    string(REPLACE "\n" ";" _klc_partition_lines "${_klc_csv_content}")
    set(_klc_p_names "")
    set(_klc_p_types "")
    set(_klc_p_subtypes "")
    set(_klc_p_offsets "")
    set(_klc_p_sizes "")

    foreach(_klc_line IN LISTS _klc_partition_lines)
        string(STRIP "${_klc_line}" _klc_line)
        if(_klc_line STREQUAL "" OR _klc_line MATCHES "^#")
            continue()
        endif()
        # Every remaining line must be a real partition definition: any row
        # that does not parse as one (or uses an unknown name) is an error,
        # never silently skipped.
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
            message(FATAL_ERROR
                "KLC: line '${_klc_line}' in partitions.csv is not a comment "
                "and not a complete partition row (need Name, Type, SubType, "
                "Offset, Size).")
        endif()
        list(GET _klc_clean 0 _klc_name)
        list(GET _klc_clean 1 _klc_type)
        list(GET _klc_clean 2 _klc_subtype)
        list(GET _klc_clean 3 _klc_offset)
        list(GET _klc_clean 4 _klc_size)
        set(_klc_fields "")
        if(NOT _klc_name MATCHES "^[A-Za-z0-9_-]+$")
            message(FATAL_ERROR
                "KLC: partition name '${_klc_name}' in partitions.csv uses "
                "characters outside the allowed set (letters, digits, "
                "underscore, hyphen).")
        endif()
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
        list(APPEND _klc_p_names "${_klc_name}")
        list(APPEND _klc_p_types "${_klc_type}")
        list(APPEND _klc_p_subtypes "${_klc_subtype}")
        list(APPEND _klc_p_offsets "${_klc_offset}")
        list(APPEND _klc_p_sizes "${_klc_size}")
    endforeach()

    list(LENGTH _klc_p_names _klc_p_count)
    math(EXPR _klc_last "${_klc_p_count} - 1")

    # Every parsed row must belong to the fixed WROOM layout: an unexpected
    # partition name is an error, never silently ignored.
    set(_klc_expected_names "nvs;phy_init;otadata;coredump;ota_0;ota_1;storage")
    foreach(_klc_i RANGE ${_klc_last})
        list(GET _klc_p_names ${_klc_i} _klc_name)
        list(FIND _klc_expected_names "${_klc_name}" _klc_name_idx)
        if(_klc_name_idx EQUAL -1)
            message(FATAL_ERROR
                "KLC: partitions.csv contains unexpected partition "
                "'${_klc_name}'. This fixed ESP32-WROOM-32D layout accepts "
                "only: ${_klc_expected_names}.")
        endif()
    endforeach()

    # --- Required partitions: exact names, exactly once, with the expected
    # --- type and subtype (TASK-107).  'coredump' is part of the layout as
    # --- well: it pads otadata's end (0x12000) up to the 64 KiB app-slot
    # --- alignment boundary so the table stays gap-free.
    set(_klc_required_names "nvs;phy_init;otadata;coredump;ota_0;ota_1;storage")
    set(_klc_required_types "data;data;data;data;app;app;data")
    set(_klc_required_subtypes "nvs;phy;ota;coredump;ota_0;ota_1;littlefs")
    set(_klc_lfs_size 0)

    foreach(_klc_r RANGE 6)
        list(GET _klc_required_names ${_klc_r} _klc_req_name)
        list(GET _klc_required_types ${_klc_r} _klc_req_type)
        list(GET _klc_required_subtypes ${_klc_r} _klc_req_subtype)
        set(_klc_req_count 0)
        foreach(_klc_i RANGE ${_klc_last})
            list(GET _klc_p_names ${_klc_i} _klc_name)
            if(NOT _klc_name STREQUAL _klc_req_name)
                continue()
            endif()
            math(EXPR _klc_req_count "${_klc_req_count} + 1")
            list(GET _klc_p_types ${_klc_i} _klc_type)
            list(GET _klc_p_subtypes ${_klc_i} _klc_subtype)
            if(NOT _klc_type STREQUAL _klc_req_type OR
               NOT _klc_subtype STREQUAL _klc_req_subtype)
                message(FATAL_ERROR
                    "KLC: partition '${_klc_name}' must be "
                    "${_klc_req_type}/${_klc_req_subtype} "
                    "(got ${_klc_type}/${_klc_subtype} in partitions.csv).")
            endif()
            if(_klc_name STREQUAL "storage")
                list(GET _klc_p_sizes ${_klc_i} _klc_lfs_size)
            endif()
        endforeach()
        if(NOT _klc_req_count EQUAL 1)
            message(FATAL_ERROR
                "KLC: partitions.csv must define '${_klc_req_name}' "
                "(${_klc_req_type}/${_klc_req_subtype}) exactly once "
                "(found ${_klc_req_count} entries).")
        endif()
    endforeach()

    math(EXPR _klc_lfs_min "262144")
    if(_klc_lfs_size LESS _klc_lfs_min)
        message(FATAL_ERROR
            "KLC: LittleFS storage partition is ${_klc_lfs_size} bytes; "
            "at least 262144 (256 KiB) is required for certificates, "
            "configuration and diagnostics.")
    endif()

    # Overlap / gap / alignment / bounds checks.
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

        if(_klc_flash_bytes GREATER 0)
            if(_klc_i EQUAL 0)
                # Everything below the first partition entry is reserved by
                # the 2nd-stage bootloader: bootloader at 0x0-0x8000 and the
                # partition table at 0x8000-0x9000.  The first partition must
                # start exactly at 0x9000: any offset below intrudes into the
                # bootloader/partition-table region, and any offset above
                # leaves an unpartitioned gap between the reserved area and
                # the first partition, violating the gap-free requirement.
                if(NOT _klc_off EQUAL 36864)
                    message(FATAL_ERROR
                        "KLC: first partition '${_klc_name}' (offset "
                        "${_klc_offset}) does not start at 0x9000. Offsets "
                        "below 0x9000 intrude into the bootloader and "
                        "partition-table region (0x0-0x9000); offsets above "
                        "0x9000 leave an unpartitioned gap. The table must "
                        "fill the flash with no gaps or overlaps.")
                endif()
            elseif(NOT _klc_off EQUAL _klc_prev_end)
                message(FATAL_ERROR
                    "KLC: partition '${_klc_name}' (offset ${_klc_offset}) "
                    "leaves a gap: the previous partition ends at "
                    "${_klc_prev_end}. The table must fill the flash with no "
                    "gaps or overlaps.")
            endif()
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

    # The table must fill the flash exactly: the last partition's end has to
    # match the configured flash size (no trailing gap).
    if(_klc_flash_bytes GREATER 0 AND NOT _klc_prev_end EQUAL _klc_flash_bytes)
        math(EXPR _klc_tail_gap "${_klc_flash_bytes} - ${_klc_prev_end}")
        message(FATAL_ERROR
            "KLC: partition table leaves ${_klc_tail_gap} unpartitioned bytes "
            "at the end of the ${_klc_flash_mb} MB flash (last partition ends "
            "at ${_klc_prev_end}, flash is ${_klc_flash_bytes} bytes). "
            "The table must fill the flash with no gaps.")
    endif()

    math(EXPR _klc_lfs_bytes "${_klc_lfs_size}")
    message(STATUS "KLC: partition table OK "
                   "(${_klc_p_count} partitions, LittleFS storage "
                   "${_klc_lfs_bytes} bytes, flash ${_klc_flash_mb} MB).")
endif()
