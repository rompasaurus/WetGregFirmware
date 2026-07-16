# ============================================================================
#  CMakeLists.txt  —  the build recipe for the wetgreg-hub-rtos firmware
# ============================================================================
#
#  WHAT IS CMAKE?
#  -------------
#  C/C++ has no built-in package manager or build runner (unlike `npm`/`vite`).
#  CMake is the de-facto tool: this file describes WHAT to compile, WHICH
#  libraries to link, and HOW, and CMake generates the low-level build files
#  (we use the Ninja generator) that actually invoke the cross-compiler.
#
#  This is the RTOS variant of wetgreg-hub. Compared to the original it:
#    1. Pulls in the FreeRTOS kernel (the RTOS) + its heap_4 allocator.
#    2. Switches the Wi-Fi/Bluetooth "arch" from the POLLED variant
#       (pico_cyw43_arch_lwip_poll) to the FreeRTOS variant
#       (pico_cyw43_arch_lwip_sys_freertos), so networking runs in its own task.
#    3. Links pico_flash for flash_safe_execute() (multi-core-safe flash writes).
#  Everything else (display driver, BLE, OTA bootloader) is carried over.
# ============================================================================

cmake_minimum_required(VERSION 3.13)

# --- Bring in the two "SDK import" shims BEFORE project() ---------------------
# These must run before project() because they influence the toolchain/compiler
# that project() locks in.
#   pico_sdk_import.cmake     → locates the Raspberry Pi Pico SDK.
#   FreeRTOS_Kernel_import.cmake → locates the FreeRTOS kernel source.
include(pico_sdk_import.cmake)

# Tell the FreeRTOS import shim where the kernel lives. Our kernel is a git
# submodule at the repository root (two directories up from here), mirroring how
# the `picowota` bootloader submodule is wired. Only default it when neither a
# -D nor the FREERTOS_KERNEL_PATH env var is already set — the Docker build mounts
# the kernel elsewhere and points at it via the environment.
if(NOT DEFINED FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/../../FreeRTOS-Kernel)
endif()
include(FreeRTOS_Kernel_import.cmake)

project(wetgreg_hub_rtos C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise the Pico SDK (sets up the cross-compiler, board headers, etc.).
pico_sdk_init()

# --- Display variant selection (unchanged from wetgreg-hub) -------------------
# The WetGreg ships with a WeAct 2.13" V4 panel; other variants kept for parity.
if(NOT DEFINED DISPLAY_VARIANT)
    set(DISPLAY_VARIANT "V4")
endif()

if(DISPLAY_VARIANT STREQUAL "V2")
    set(EPD_SOURCE lib/e-Paper/EPD_2in13_V2.c)
    set(EPD_DEFINE DISPLAY_V2)
elseif(DISPLAY_VARIANT STREQUAL "V3a")
    set(EPD_SOURCE lib/e-Paper/EPD_2in13_V3a.c)
    set(EPD_DEFINE DISPLAY_V3A)
elseif(DISPLAY_VARIANT STREQUAL "V4")
    set(EPD_SOURCE lib/e-Paper/EPD_2in13_V4.c)
    set(EPD_DEFINE DISPLAY_V4)
else()
    set(EPD_SOURCE lib/e-Paper/EPD_2in13_V3.c)
    set(EPD_DEFINE "")
endif()
message(STATUS "Display variant: ${DISPLAY_VARIANT}")

# --- The firmware executable -------------------------------------------------
# The application is split into modules by concern (see docs + the src/ tree):
#   src/app      — main() + the UI state machine (the app shell)
#   src/drivers  — one .c/.h pair per hardware entity (joystick, speaker, ...)
#   src/sys      — system services (motion, power, storage, rng, rtc seed)
#   src/net      — WiFi + NTP
#   src/ui       — canvas, font, icons, octopus art, one module per screen family
add_executable(wetgreg_hub_rtos
    src/app/app_main.c
    src/drivers/accel.c
    src/drivers/battery.c
    src/drivers/display.c
    src/drivers/joystick.c
    src/drivers/speaker.c
    src/net/ntp.c
    src/net/wifi.c
    src/sys/motion.c
    src/sys/power.c
    src/sys/rng.c
    src/sys/rtc_clock.c
    src/sys/storage.c
    src/ui/canvas.c
    src/ui/icons.c
    src/ui/octopus.c
    src/ui/screens_anim.c
    src/ui/screens_home.c
    src/ui/screens_menu.c
    src/ui/screens_network.c
    src/ui/screens_social.c
    src/ui/text.c
    rtos_tasks.c          # the Input/Display/Housekeeping task orchestration
    freertos_hooks.c      # kernel-required callbacks (idle/timer memory, error hooks)
    bt.c
    lib/Config/DEV_Config.c
    lib/Config/rtc_compat.c   # shared software-RTC state (one clock for all modules)
    ${EPD_SOURCE}
)

if(EPD_DEFINE)
    target_compile_definitions(wetgreg_hub_rtos PRIVATE ${EPD_DEFINE}=1)
endif()

target_include_directories(wetgreg_hub_rtos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # so #include "FreeRTOSConfig.h" is found
    ${CMAKE_CURRENT_SOURCE_DIR}/src/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sys
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/Config
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/e-Paper
)

# --- Libraries we link against ----------------------------------------------
target_link_libraries(wetgreg_hub_rtos
    pico_stdlib            # core SDK (GPIO, time, stdio, ...)
    hardware_spi           # e-ink panel bus
    hardware_gpio
    hardware_adc           # battery voltage sense
    hardware_pwm           # piezo speaker
    hardware_i2c           # accelerometer
    hardware_flash         # low-level flash erase/program
    hardware_watchdog      # watchdog_reboot — hold-CENTER hard reboot
    hardware_vreg          # vreg_set_voltage — core-rail drop in the sleep state
    pico_flash             # flash_safe_execute() — multicore-safe flash writes
    m                      # libm (sqrtf/atan2f for the accelerometer maths)

    # --- The RTOS itself ---
    FreeRTOS-Kernel        # the scheduler + kernel objects (tasks/queues/mutexes)
    FreeRTOS-Kernel-Heap4  # the heap_4 memory allocator (see FreeRTOSConfig.h)

    # --- Wi-Fi/Bluetooth, FreeRTOS flavour ---
    # This REPLACES pico_cyw43_arch_lwip_poll. The "_sys_freertos" variant runs
    # the cyw43 driver + lwIP + BTstack inside a FreeRTOS task, so we no longer
    # call cyw43_arch_poll() by hand.
    pico_cyw43_arch_lwip_sys_freertos
)

# --- Bluetooth LE (BTstack) — unchanged from wetgreg-hub ----------------------
target_compile_definitions(wetgreg_hub_rtos PRIVATE CYW43_ENABLE_BLUETOOTH=1)
target_link_libraries(wetgreg_hub_rtos pico_btstack_ble pico_btstack_cyw43)
# Generate wetgreg.h (the GATT database "profile_data") from wetgreg.gatt.
pico_btstack_make_gatt_header(wetgreg_hub_rtos PRIVATE ${CMAKE_CURRENT_LIST_DIR}/wetgreg.gatt)

# --- Wi-Fi credentials (pass via -D or edit wifi_config.h) -------------------
if(DEFINED WIFI_SSID)
    target_compile_definitions(wetgreg_hub_rtos PRIVATE WIFI_SSID="${WIFI_SSID}")
endif()
if(DEFINED WIFI_PASS)
    target_compile_definitions(wetgreg_hub_rtos PRIVATE WIFI_PASS="${WIFI_PASS}")
endif()

# hardware_rtc only exists on RP2040; RP2350 uses a software clock fallback.
if(PICO_PLATFORM STREQUAL "rp2040")
    target_link_libraries(wetgreg_hub_rtos hardware_rtc)
endif()

# Console over USB (handy for printf debugging); UART console off.
pico_enable_stdio_usb(wetgreg_hub_rtos 1)
pico_enable_stdio_uart(wetgreg_hub_rtos 0)

# In an OTA build, picowota_build_combined() produces the output artifacts, so
# adding the normal extra outputs here too would define them twice (Ninja errors
# on duplicate outputs). Only add them for the plain USB build.
if(NOT PICOWOTA_OTA)
    pico_add_extra_outputs(wetgreg_hub_rtos)
endif()

# --- picowota Wi-Fi OTA bootloader (optional) --------------------------------
# Enable with: cmake -DPICOWOTA_OTA=ON -DPICO_BOARD=pico2_w -DWIFI_SSID=.. -DWIFI_PASS=.. ..
# Produces picowota_wetgreg_hub_rtos.uf2 (flash once over USB) + wetgreg_hub_rtos.elf
# (the OTA payload pushed over Wi-Fi). The bootloader is a SEPARATE binary and is
# unaffected by FreeRTOS (it runs before our scheduler ever starts).
option(PICOWOTA_OTA "Build the picowota WiFi OTA bootloader + combined image" OFF)
if(PICOWOTA_OTA)
    if(DEFINED WIFI_SSID AND NOT DEFINED PICOWOTA_WIFI_SSID)
        set(PICOWOTA_WIFI_SSID "${WIFI_SSID}")
    endif()
    if(DEFINED WIFI_PASS AND NOT DEFINED PICOWOTA_WIFI_PASS)
        set(PICOWOTA_WIFI_PASS "${WIFI_PASS}")
    endif()
    if(NOT DEFINED PICOWOTA_WIFI_AP)
        set(PICOWOTA_WIFI_AP 0)
    endif()

    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../picowota picowota)
    target_link_libraries(wetgreg_hub_rtos picowota_reboot)
    target_compile_definitions(wetgreg_hub_rtos PRIVATE PICOWOTA_OTA=1)
    picowota_build_combined(wetgreg_hub_rtos)
endif()
