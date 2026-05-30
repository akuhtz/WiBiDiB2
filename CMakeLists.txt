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

project(WiBiDiB C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Sources — 
   add_executable(WiBiDiB
    main.c
    bidib.c
    crc_8bit.c
    tcp_server.c
    smartphone_if.c
    withrottle_if.c
    # dhcpserver fourni par pico-examples — copier le dossier dhcpserver/ ici
    dhcpserver/dhcpserver.c
    bidib_client_parser.c
    bidib_client_if.c
)

# Générer le header C depuis le fichier PIO
pico_generate_pio_header(WiBiDiB
    ${CMAKE_CURRENT_LIST_DIR}/bidib_uart.pio
)

pico_set_program_name(WiBiDiB "WiBiDiB")
pico_set_program_version(WiBiDiB "0.1")

# Sortie USB pour printf, UART désactivé
pico_enable_stdio_uart(WiBiDiB 0)
pico_enable_stdio_usb(WiBiDiB 1)

# Bibliothèques
target_link_libraries(WiBiDiB
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_gpio
    pico_unique_id
    pico_cyw43_arch_lwip_poll   # WiFi + lwIP bare-metal
)

target_include_directories(WiBiDiB PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
	${CMAKE_CURRENT_LIST_DIR}/dhcpserver
    ${CMAKE_CURRENT_LIST_DIR}/include
)

pico_add_extra_outputs(WiBiDiB)
