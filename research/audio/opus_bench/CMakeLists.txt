# Opus encode/decode benchmark for the RP2350 (Pico 2 W), separate from the
# switch2-pico firmware. Builds libopus from a source checkout (OPUS_DIR,
# default ~/src/opus, tag v1.5.2) in fixed point and prints timings over
# USB serial. See main.c.
cmake_minimum_required(VERSION 3.13)
if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../../pico_sdk_import.cmake)
project(opus_bench C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

if (NOT DEFINED OPUS_DIR)
    set(OPUS_DIR $ENV{HOME}/src/opus)
endif()

# Source lists from libopus's own makefiles.
function(opus_mk_list file var out)
    file(READ ${OPUS_DIR}/${file} txt)
    string(REPLACE "\\\n" " " txt "${txt}")
    string(REGEX MATCH "${var} = [^\n]*" line "${txt}")
    string(REPLACE "${var} = " "" line "${line}")
    separate_arguments(items UNIX_COMMAND "${line}")
    list(TRANSFORM items PREPEND ${OPUS_DIR}/)
    set(${out} ${items} PARENT_SCOPE)
endfunction()
opus_mk_list(celt_sources.mk CELT_SOURCES CELT_SRC)
opus_mk_list(silk_sources.mk SILK_SOURCES SILK_SRC)
opus_mk_list(silk_sources.mk SILK_SOURCES_FIXED SILK_FIXED_SRC)
opus_mk_list(opus_sources.mk OPUS_SOURCES OPUS_SRC)

add_library(opus STATIC ${CELT_SRC} ${SILK_SRC} ${SILK_FIXED_SRC} ${OPUS_SRC})
target_include_directories(opus PUBLIC ${OPUS_DIR}/include
    PRIVATE ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed ${OPUS_DIR}/src ${OPUS_DIR})
target_compile_definitions(opus PRIVATE OPUS_BUILD FIXED_POINT HAVE_LRINT HAVE_LRINTF
    NONTHREADSAFE_PSEUDOSTACK GLOBAL_STACK_SIZE=120000 ${OPUS_EXTRA_DEFS})
target_compile_options(opus PRIVATE -O2 -w)

add_executable(opus_bench main.c)
target_link_libraries(opus_bench pico_stdlib hardware_clocks opus)
pico_enable_stdio_usb(opus_bench 1)
pico_enable_stdio_uart(opus_bench 0)
pico_add_extra_outputs(opus_bench)
