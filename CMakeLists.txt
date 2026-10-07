cmake_minimum_required(VERSION 3.13)

# Switch2-Pico: a Raspberry Pi Pico 2 W firmware that connects to a Nintendo
# Switch 2 Pro Controller over Bluetooth LE and presents it over USB as a
# Nintendo Switch (1) Pro Controller.

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif ()

include(pico_sdk_import.cmake)

project(switch2_pico C CXX ASM)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" S2P_VERSION LIMIT_COUNT 1)
string(STRIP "${S2P_VERSION}" S2P_VERSION)

# All of our USB / Bluetooth / Wi-Fi work happens from one cooperative main
# loop, so TinyUSB does not need an OS abstraction layer.
set(TINYUSB_OPT_OS OPT_OS_NONE CACHE STRING "TinyUSB OS abstraction" FORCE)

pico_sdk_init()

if (NOT PICO_CYW43_SUPPORTED)
    message(FATAL_ERROR "switch2-pico needs a wireless board (pico2_w or pico_w); PICO_BOARD=${PICO_BOARD}")
endif ()

# --------------------------------------------------------------------------
# Embed the configuration web page (web/index.html) as a C array.
# --------------------------------------------------------------------------
set(WEB_UI_SRC ${CMAKE_CURRENT_LIST_DIR}/web/index.html)
set(WEB_UI_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated/web_ui_data.c)
add_custom_command(
        OUTPUT ${WEB_UI_GEN}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${WEB_UI_SRC} -DOUTPUT=${WEB_UI_GEN}
                -P ${CMAKE_CURRENT_LIST_DIR}/tools/embed_file.cmake
        DEPENDS ${WEB_UI_SRC} ${CMAKE_CURRENT_LIST_DIR}/tools/embed_file.cmake
        COMMENT "Embedding web/index.html"
        VERBATIM
)

add_executable(switch2_pico
        src/main.c
        src/settings.c
        src/log.c
        src/s2_proto.c
        src/s2_link.c
        src/hd_rumble.c
        src/mapping.c
        src/procon.c
        src/mcu_nfc.c
        src/amiibo.c
        src/usb_descriptors.c
        src/usb_hid.c
        src/status_led.c
        src/bootsel.c
        src/web/wifi_ap.c
        src/web/dhcp_server.c
        src/web/dns_server.c
        src/web/http_server.c
        src/web/web_api.c
        ${WEB_UI_GEN}
)

target_compile_definitions(switch2_pico PRIVATE
        S2P_VERSION="${S2P_VERSION}"
        CYW43_LWIP=1
        # Keep the BTstack HCI dump quiet unless explicitly enabled.
        PICO_STDIO_USB=0
)

target_include_directories(switch2_pico PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/src/web
)

target_compile_options(switch2_pico PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

target_link_libraries(switch2_pico
        pico_stdlib
        pico_unique_id
        pico_flash
        pico_rand
        hardware_flash
        hardware_sync
        tinyusb_device
        tinyusb_board
        pico_cyw43_arch_lwip_poll
        pico_btstack_ble
        pico_btstack_cyw43
)

# stdio over the debug UART (GP0/GP1) only; USB is owned by the controller emulation.
pico_enable_stdio_uart(switch2_pico 1)
pico_enable_stdio_usb(switch2_pico 0)

pico_set_program_name(switch2_pico "switch2-pico")
pico_set_program_version(switch2_pico "${S2P_VERSION}")
pico_set_program_url(switch2_pico "https://github.com/joshbirnholz/switch2-pico")

pico_add_extra_outputs(switch2_pico)
