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

# Pro Controller 2 headphone jack audio (Opus encoder on core 1).
# Needs the RP2350 (Pico 2 W): about half a core at 150 MHz.
if (PICO_PLATFORM MATCHES "rp2350")
    option(S2_HEADSET_AUDIO "Pro Controller 2 headphone audio" ON)
else()
    option(S2_HEADSET_AUDIO "Pro Controller 2 headphone audio" OFF)
endif()
# Development: play a 440 Hz tone to the headphones instead of silence.
option(S2_AUDIO_TEST_TONE "Headphone audio test tone" OFF)
# In development: the headset mic as a USB microphone (not tested yet).
option(S2_HEADSET_MIC "Pro Controller 2 headset mic (in development)" OFF)
if (S2_HEADSET_AUDIO)
    include(${CMAKE_CURRENT_LIST_DIR}/pico/opus.cmake)
endif()

set(CORE ${CMAKE_CURRENT_LIST_DIR}/core/src)
set(PICO_SRC ${CMAKE_CURRENT_LIST_DIR}/pico)
add_executable(switch2_pico
        ${CORE}/app_core.c
        ${CORE}/ds5.c
        ${CORE}/hd_rumble.c
        ${CORE}/log.c
        ${CORE}/mapping.c
        ${CORE}/mode_select.c
        ${CORE}/bonds.c
        ${CORE}/profiles.c
        ${CORE}/joycon.c
        ${CORE}/battery.c
        ${CORE}/procon.c
        ${CORE}/s2_link.c
        ${CORE}/s2_proto.c
        ${CORE}/settings.c
        ${CORE}/usb_hid.c
        ${CORE}/usb_mode.c
        ${CORE}/usb_ms_desc.c
        ${CORE}/usb_pro_desc.c
        ${CORE}/x360.c
        ${CORE}/gc_adapter.c
        ${CORE}/sinput.c
        ${CORE}/web_api.c
        ${CORE}/webusb.c
        ${PICO_SRC}/fw_update_pico.c
        ${PICO_SRC}/main.c
        ${PICO_SRC}/platform_pico.c
        ${PICO_SRC}/s2_ble_btstack.c
        ${PICO_SRC}/usb_descriptors.c
        ${PICO_SRC}/status_led.c
        ${PICO_SRC}/bootsel.c
        ${PICO_SRC}/web/wifi_ap.c
        ${PICO_SRC}/web/dhcp_server.c
        ${PICO_SRC}/web/dns_server.c
        ${PICO_SRC}/web/http_server.c
        ${WEB_UI_GEN}
)
if (S2_HEADSET_AUDIO)
    target_sources(switch2_pico PRIVATE ${PICO_SRC}/headset_audio_pico.c ${PICO_SRC}/usb_audio.c)
    target_link_libraries(switch2_pico opus pico_multicore)
    target_compile_definitions(switch2_pico PRIVATE S2_HEADSET_AUDIO=1
            S2_AUDIO_TEST_TONE=$<BOOL:${S2_AUDIO_TEST_TONE}>)
    if (S2_HEADSET_MIC)
        target_sources(switch2_pico PRIVATE ${PICO_SRC}/headset_mic_pico.c)
        target_compile_definitions(switch2_pico PRIVATE S2_HEADSET_MIC=1)
        opus_ram_linker_script(switch2_pico celt/celt_decoder src/opus_decoder)
    else()
        opus_ram_linker_script(switch2_pico)
    endif()
endif()

target_compile_definitions(switch2_pico PRIVATE
        S2P_VERSION="${S2P_VERSION}"
        CYW43_LWIP=1
        # Keep the BTstack HCI dump quiet unless explicitly enabled.
        PICO_STDIO_USB=0
)

target_include_directories(switch2_pico PRIVATE
        ${CORE}
        ${PICO_SRC}
        ${PICO_SRC}/web
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
        pico_multicore
)

# stdio over the debug UART (GP0/GP1) only; USB is owned by the controller emulation.
pico_enable_stdio_uart(switch2_pico 1)
pico_enable_stdio_usb(switch2_pico 0)

pico_set_program_name(switch2_pico "switch2-pico")
pico_set_program_version(switch2_pico "${S2P_VERSION}")
pico_set_program_url(switch2_pico "https://github.com/joshbirnholz/switch2-pico")

pico_add_extra_outputs(switch2_pico)
