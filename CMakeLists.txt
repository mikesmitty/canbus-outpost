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
include(FreeRTOS_Kernel_import.cmake)

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
    src/identify_led.c
    src/lcc.c
    src/lcc_memconfig.c
    src/cdi_data.c
    src/gridconnect.c
    src/util/dbg.c
)

# x-release-please-start-version
set(PROJECT_VERSION_BASE "0.2.1")
# x-release-please-end

# Get the current git version if available
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# If git describe only returned a hash (no tags), prepend the base version
if (GIT_VERSION MATCHES "^[0-9a-f]+(-dirty)?$")
    set(GIT_VERSION "${PROJECT_VERSION_BASE}-${GIT_VERSION}")
endif()

if (NOT GIT_VERSION)
    set(GIT_VERSION "${PROJECT_VERSION_BASE}-unknown")
endif()

pico_set_program_name(canbus-outpost "canbus-outpost")
pico_set_program_version(canbus-outpost "${GIT_VERSION}")

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
    PICO_FLASH_SAFE_EXECUTE_PICO_SUPPORT_MULTICORE_LOCKOUT=0
    LCC_SW_VERSION="${GIT_VERSION}"
)

# Add the standard library to the build
target_link_libraries(canbus-outpost
        pico_stdlib
        hardware_pio
        hardware_pwm
        hardware_dma
        hardware_flash
        pico_flash
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
