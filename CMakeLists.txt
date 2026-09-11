# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Pull in FreeRTOS Kernel (must be before project)
set(FREERTOS_KERNEL_PATH ${USERHOME}/.pico-sdk/FreeRTOS-Kernel)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

project(WiBiDiB C CXX ASM)

# CYW43 task config — must be BEFORE pico_sdk_init() so SDK library sources see it
add_compile_definitions(CYW43_TASK_STACK_SIZE=2048 CYW43_TASK_PRIORITY=3)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Sources — 
   add_executable(WiBiDiB
    main.c
    led.c
    bidib.c
    crc_8bit.c
    tcp_server.c
    smartphone_if.c
    withrottle_if.c
    log.c
    # dhcpserver fourni par pico-examples — copier le dossier dhcpserver/ ici
    dhcpserver/dhcpserver.c
    bidib_client_parser.c
    bidib_client_if.c
    mdns.c
    flash_store.c
    roster.c
    http_server.c
)

# Générer le header C depuis le fichier PIO
pico_generate_pio_header(WiBiDiB
    ${CMAKE_CURRENT_LIST_DIR}/bidib_uart.pio
)

pico_set_program_name(WiBiDiB "WiBiDiB")
pico_set_program_version(WiBiDiB "0.1")

# Sortie UART (bridge debug probe), USB désactivé
pico_enable_stdio_uart(WiBiDiB 1)
pico_enable_stdio_usb(WiBiDiB 0)

# Bibliothèques
target_link_libraries(WiBiDiB
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_gpio
    pico_unique_id
    pico_cyw43_arch_lwip_sys_freertos  # WiFi + lwIP + FreeRTOS (NO_SYS=0)
    pico_lwip_mdns              # mDNS responder (Engine Driver discovery)
    hardware_spi                # SPI1 flash W25Q32VFSIG
    FreeRTOS-Kernel             # FreeRTOS kernel
    FreeRTOS-Kernel-Heap4       # FreeRTOS heap (heap_4.c — dynamic alloc)
)

target_include_directories(WiBiDiB PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
	${CMAKE_CURRENT_LIST_DIR}/dhcpserver
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# FreeRTOS config file location
target_compile_definitions(WiBiDiB PRIVATE
    FREERTOS_CONFIG_FILE_DIRECTORY="${CMAKE_CURRENT_LIST_DIR}/include"
    FREERTOS_KERNEL_INCLUDE_DIR="${FREERTOS_KERNEL_PATH}/include"
)

pico_add_extra_outputs(WiBiDiB)
