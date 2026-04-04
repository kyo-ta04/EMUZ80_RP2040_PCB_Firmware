# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

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
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(EMUZ80_RP2040_PCB_Firmware C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(EMUZ80_RP2040_PCB_Firmware
    EMUZ80_RP2040_PCB_Firmware.c
    ccp_bdos.c
    bios01.c
    cpm22_1.c
    cpm22_disk1.c
    cpm22_tp301a.c
    cpm22_z80forth.c
    cpm22_htc.c
)

pico_set_program_name(EMUZ80_RP2040_PCB_Firmware "EMUZ80_RP2040_PCB_Firmware")
pico_set_program_version(EMUZ80_RP2040_PCB_Firmware "0.1")

# Generate PIO header
pico_generate_pio_header(EMUZ80_RP2040_PCB_Firmware ${CMAKE_CURRENT_LIST_DIR}/AE-RP2040.pio)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(EMUZ80_RP2040_PCB_Firmware 0)
pico_enable_stdio_usb(EMUZ80_RP2040_PCB_Firmware 1)

# Add the standard library to the build
target_link_libraries(EMUZ80_RP2040_PCB_Firmware
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(EMUZ80_RP2040_PCB_Firmware PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(EMUZ80_RP2040_PCB_Firmware 
        hardware_pio
        hardware_timer
        hardware_pwm
        pico_multicore
        hardware_structs
        hardware_dma
        )

pico_add_extra_outputs(EMUZ80_RP2040_PCB_Firmware)

# 400MHz Overclock Optimizations
pico_set_binary_type(EMUZ80_RP2040_PCB_Firmware copy_to_ram)
target_compile_options(EMUZ80_RP2040_PCB_Firmware PRIVATE -O3)

