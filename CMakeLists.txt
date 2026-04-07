# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.14)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Pull in FreeRTOS
set(FREERTOS_KERNEL_PATH ${USERHOME}/git/FreeRTOS-Kernel CACHE PATH "Path to FreeRTOS Kernel")
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

project(canbus-outpost C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable
add_executable(canbus-outpost
    canbus-outpost.c
    src/can2040.c
    src/railcom.c
    src/servo.c
    src/nv_storage.c
    src/lcc.c
    src/lcc_memconfig.c
    src/cdi_data.c
    src/gridconnect.c
    src/util/dbg.c
)

pico_set_program_name(canbus-outpost "canbus-outpost")
pico_set_program_version(canbus-outpost "0.1")

# Generate PIO headers
pico_generate_pio_header(canbus-outpost ${CMAKE_CURRENT_LIST_DIR}/blink.pio)
pico_generate_pio_header(canbus-outpost ${CMAKE_CURRENT_LIST_DIR}/src/railcom_sample.pio)

# USB CDC for serial I/O, disable UART stdio, enable RTT for debug
pico_enable_stdio_uart(canbus-outpost 0)
pico_enable_stdio_usb(canbus-outpost 1)
pico_enable_stdio_rtt(canbus-outpost 1)

# Wait up to 2s during stdio_usb_init() for USB enumeration
target_compile_definitions(canbus-outpost PRIVATE
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=2000
)

# Add the standard library to the build
target_link_libraries(canbus-outpost
        pico_stdlib
        hardware_pio
        hardware_pwm
        hardware_dma
        hardware_flash
        pico_unique_id
        pico_multicore
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
)

# Add the standard include files to the build
target_include_directories(canbus-outpost PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
)

pico_add_extra_outputs(canbus-outpost)
