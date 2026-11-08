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
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(ATAboy C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(ATAboy
        ATAboy.c
        fault.c
        ide_pio.c
        ide.c
        drive.c
        ata_policy.c
        sat.c
        sense.c
        json.c
        api.c
        menus.c
        usb.c
        msc_rw.c
        sector_hash.c
        sha256_hw.c
        usb_descriptors.c
        config.c)

# Version string reported by the API comes from version.h; the git hash is
# baked in so a host can tell exactly which build is flashed.
find_package(Git QUIET)
set(FW_GIT_HASH "unknown")
if (GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=10
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE FW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if (NOT FW_GIT_HASH)
        set(FW_GIT_HASH "unknown")
    endif()
endif()
target_compile_definitions(ATAboy PRIVATE FW_GIT_HASH="${FW_GIT_HASH}"
        PICO_STACK_SIZE=0x1000          # core 0 (SCRATCH_Y, 4 KiB): TinyUSB callbacks + IDE + JSON
        PICO_CORE1_STACK_SIZE=0x1000)   # core 1 (SCRATCH_X, 4 KiB): menu (vsnprintf, IDENTIFY buffers)
target_compile_options(ATAboy PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation)

pico_set_program_name(ATAboy "ATAboy")
pico_set_program_version(ATAboy "0.6f3-api1")

# Generate PIO header
pico_generate_pio_header(ATAboy ${CMAKE_CURRENT_LIST_DIR}/ataboy.pio)

# No pico_stdio — all console I/O through TinyUSB CDC via queues
pico_enable_stdio_uart(ATAboy 0)
pico_enable_stdio_usb(ATAboy 0)

# Link libraries
target_link_libraries(ATAboy
        pico_stdlib
        pico_multicore
        hardware_flash
        hardware_sync
        hardware_pio
        hardware_dma
        hardware_clocks
        hardware_watchdog
        hardware_sha256
        pico_bootrom
        pico_unique_id
        tinyusb_device
        tinyusb_board
        )

# Add the standard include files to the build
target_include_directories(ATAboy PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(ATAboy)

